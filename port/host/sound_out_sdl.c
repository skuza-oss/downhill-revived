/* Host side of the 989snd output (finding 022, phase 2 part C, finding 075), the one sound output
   on every platform: an SDL3 audio device stream (WASAPI under it on Windows, Core Audio on macOS,
   PipeWire/PulseAudio/ALSA on Linux, AAudio on Android). The mixer (port/hle/sound.c) renders game
   time and hands its PCM over with host_sound_write; nothing here affects what the game can
   observe. Interface: port/host/host_api.h, "sound output". Headless runs (DHD_HEADLESS) open no
   device and drop the PCM.

   The game renders 4004/5 samples per 59.94 Hz field, in bursts at its own pace, into a ring; the
   device plays at its own clock. The stream's get-callback runs on SDL's device thread once per
   device period (asked for 10 ms, as WASAPI's event period the output was tuned on), and does
   what the WASAPI thread did (finding 043, round 30): it keeps a cushion of DHD_SND_LATENCY_MS
   (default 70) queued between the ring and the device, fills the stream only to DEVICE_MS, and
   follows the drift between the game's frame clock and the device clock by reading the ring up to
   0.5% faster or slower (dynamic rate control, linearly interpolated). If the cushion runs out it
   plays silence once and builds the cushion again; if far too much is queued (the game catching
   up after a stall) it skips ahead. The queue it measures is the stream's plus one device period
   (what the device holds when SDL asks for more), the WASAPI path's padding.

   When the game itself runs slower than 59.94 fields/s (heavy scenes), it renders less sound than
   the device plays, and no cushion is deep enough. Then the output time-stretches: it plays the
   ring at the game's measured speed (down to half) without changing pitch, by WSOLA
   (waveform-similarity overlap-add, as SoundTouch, which PCSX2 uses, does it): 20-ms Hann windows
   at a 10-ms output hop, each placed where the input best matches the natural continuation of the
   previous one, searched over +-5 ms. At full speed the stretcher copies the ring unchanged; it
   joins and leaves at a window boundary, where both are the same signal. DHD_SND_STRETCH=0 turns
   it off (gaps instead).

   The stream is opened on the default playback device, which SDL follows when the user changes
   it (or unplugs the one in use). No device is opened in headless runs (DHD_HEADLESS) or with
   DHD_SND_OFF. Counters (DHD_BENCH window: one "bench snd:" line; DHD_SND_HOST_TRACE=1: a line
   per second): underruns (gaps), silence written inside gaps, frames dropped or skipped, queue
   level, and the game's tick interval. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "host_api.h"
#include "voice.h"
#include "../hle/hle.h"

#define SND_RATE HOST_SOUND_RATE
#define RING_SAMPLES (SND_RATE / 2)
#define DEVICE_MS 30
#define PERIOD_FRAMES "480" /* the device period SDL is asked for: 10 ms */
#define SKIP_MS 120 /* queued beyond the cushion: skip ahead */
#define RATE_RANGE 0.005
static int16_t ring[RING_SAMPLES * 2];
static volatile long ring_read, ring_write;
static long ring_end;  /* the device thread's view of ring_write, taken once per callback */
static int host_started;
static volatile long host_underruns, host_silence, host_dropped, host_skipped, host_frames;
static volatile long host_queue_hist[512]; /* queued ms at each device top-up */
static volatile long host_tick_hist[128];  /* game tick interval, 0.5-ms bins */
static volatile int host_hist_reset;
static volatile double host_ratio = 1.0;
static volatile double host_tempo = 1.0;   /* stretch tempo (1: copied) */
static volatile long host_stretched;       /* output frames made by the time stretcher */

static int trace_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("DHD_SND_TRACE") != NULL;
    return on;
}

/* The game thread's side: copies what fits, then publishes it (the release pairs with the
   acquire in host_callback, so the device thread never sees the position before the samples). */
static void ring_push(const int16_t *stereo, int samples)
{
    long w = ring_write, room = RING_SAMPLES - (w - ring_read), i;
    long count = samples < room ? samples : room;
    if (count < samples)
        host_dropped += samples - count; /* host is behind: drop; game state already advanced */
    for (i = 0; i < count; i++) {
        ring[((w + i) % RING_SAMPLES) * 2] = stereo[i * 2];
        ring[((w + i) % RING_SAMPLES) * 2 + 1] = stereo[i * 2 + 1];
    }
    SDL_MemoryBarrierRelease();
    ring_write = w + count;
}

static int host_env_ms(const char *name, int fallback, int low, int high)
{
    const char *v = getenv(name);
    int ms = v && *v ? atoi(v) : fallback;
    return ms < low ? low : ms > high ? high : ms;
}

static void host_histograms(unsigned queued)
{
    unsigned ms = queued / (SND_RATE / 1000);
    if (host_hist_reset) {
        memset((void *)host_queue_hist, 0, sizeof host_queue_hist);
        host_hist_reset = 0;
    }
    host_queue_hist[ms < 511 ? ms : 511]++;
}

static unsigned host_percentile(volatile long *hist, unsigned bins, double fraction)
{
    unsigned i;
    long total = 0, seen = 0;
    for (i = 0; i < bins; i++) total += hist[i];
    for (i = 0; i < bins; i++) {
        seen += hist[i];
        if (total && seen >= total * fraction) return i;
    }
    return 0;
}

/* DHD_BENCH window start (0) and end (1), called by present.c. */
void snd_host_bench(int end)
{
    static long underruns, silence, dropped, skipped, frames, stretched;
    const double per_ms = SND_RATE / 1000.0;
    if (!end) {
        underruns = host_underruns; silence = host_silence; dropped = host_dropped;
        skipped = host_skipped; frames = host_frames; stretched = host_stretched;
        host_hist_reset = 1;
        memset((void *)host_tick_hist, 0, sizeof host_tick_hist);
        return;
    }
    fprintf(stderr, "bench snd: underruns=%ld silence_ms=%.1f dropped_ms=%.1f skipped_ms=%.1f played_s=%.2f "
            "queue_ms p1=%u p50=%u p99=%u rate_ppm=%+.0f tick_ms p50=%.1f p99=%.1f max=%.1f stretched_s=%.2f\n",
            host_underruns - underruns, (host_silence - silence) / per_ms, (host_dropped - dropped) / per_ms,
            (host_skipped - skipped) / per_ms, (host_frames - frames) / (double)SND_RATE,
            host_percentile(host_queue_hist, 512, 0.01), host_percentile(host_queue_hist, 512, 0.5),
            host_percentile(host_queue_hist, 512, 0.99), (host_ratio - 1) * 1e6,
            host_percentile(host_tick_hist, 128, 0.5) * 0.5, host_percentile(host_tick_hist, 128, 0.99) * 0.5,
            host_percentile(host_tick_hist, 128, 1.0) * 0.5, (host_stretched - stretched) / (double)SND_RATE);
}

/* ---------------- the time-stretch stage: ring -> fifo, copied or WSOLA-stretched ---------------- */
#define ST_HOP 480           /* 10 ms output hop; the window is twice that */
#define ST_SEEK 240          /* +-5 ms search, at most the shortest input hop */
#define ST_FIFO 8192
static int16_t st_fifo[ST_FIFO * 2];
static long st_read, st_write;
static struct {
    int on;                  /* stretching; else copying */
    long ref;                /* input position of the previous window's natural continuation */
    double where;            /* fractional input position of the next window */
    float rise[ST_HOP], tail[ST_HOP * 2];  /* the previous window's falling half, L/R */
} st;

static float st_mono(long at) { const int16_t *f = &ring[(at % RING_SAMPLES) * 2]; return (float)f[0] + f[1]; }

/* Best start in [lo, hi] for a window like the one at ref (normalized cross-correlation on the
   mono sum, every second sample, then refined by one sample each way). */
static long st_seek(long lo, long hi, long ref)
{
    static float want[ST_HOP / 2];
    long best = lo, at;
    double best_score = -1e300;
    for (int i = 0; i < ST_HOP / 2; i++) want[i] = st_mono(ref + 2 * i);
    for (int pass = 0; pass < 2; pass++) {
        long from = pass ? best - 1 : lo, to = pass ? best + 1 : hi, step = pass ? 1 : 2;
        if (from < lo) from = lo;
        if (to > hi) to = hi;
        for (at = from; at <= to; at += step) {
            double dot = 0, energy = 1;
            for (int i = 0; i < ST_HOP / 2; i++) {
                float x = st_mono(at + 2 * i);
                dot += x * want[i]; energy += x * x;
            }
            double score = dot / sqrt(energy);
            if (score > best_score) { best_score = score; best = at; }
        }
    }
    return best;
}

static void st_put(int16_t l, int16_t r)
{
    st_fifo[(st_write % ST_FIFO) * 2] = l;
    st_fifo[(st_write % ST_FIFO) * 2 + 1] = r;
    st_write++;
}

static int16_t st_clamp(float v) { return (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : lrintf(v)); }

/* Leaves stretching at the natural continuation, where copying gives the same signal. */
static void st_leave(void)
{
    if (st.on) { ring_read = st.ref; st.on = 0; }
}

/* Puts frames into the fifo until it holds `want`, at `tempo` (input frames per output frame). */
static void st_fill(unsigned want, double tempo)
{
    if (!st.rise[ST_HOP - 1])
        for (int i = 0; i < ST_HOP; i++) {
            double w = sin(3.14159265358979 / 2 * (i + 0.5) / ST_HOP);
            st.rise[i] = (float)(w * w);
        }
    if (tempo >= 1.0) st_leave();
    else if (!st.on) {  /* join: the copied signal is the previous window */
        st.on = 1; st.ref = ring_read; st.where = ring_read - ST_HOP;
        for (int i = 0; i < ST_HOP; i++) {
            const int16_t *f = &ring[((ring_read + i) % RING_SAMPLES) * 2];
            float fall = 1.0f - st.rise[i];
            st.tail[i * 2] = f[0] * fall; st.tail[i * 2 + 1] = f[1] * fall;
        }
    }
    while ((unsigned)(st_write - st_read) < want && st_write - st_read <= ST_FIFO - ST_HOP) {
        if (!st.on) {
            long r = ring_read;
            if (r >= ring_end) return;
            st_put(ring[(r % RING_SAMPLES) * 2], ring[(r % RING_SAMPLES) * 2 + 1]);
            ring_read = r + 1;
            continue;
        }
        long q = (long)floor(st.where + ST_HOP * tempo), lo = q - ST_SEEK, hi = q + ST_SEEK;
        if (lo < ring_read) lo = ring_read;
        if (hi + 2 * ST_HOP > ring_end) hi = ring_end - 2 * ST_HOP;  /* short of input: search less */
        if (hi < lo) return;  /* not enough input yet */
        long at = st_seek(lo, hi, st.ref);
        for (int i = 0; i < ST_HOP; i++) {
            const int16_t *f = &ring[((at + i) % RING_SAMPLES) * 2], *g = &ring[((at + ST_HOP + i) % RING_SAMPLES) * 2];
            float fall = 1.0f - st.rise[i];
            st_put(st_clamp(st.tail[i * 2] + f[0] * st.rise[i]), st_clamp(st.tail[i * 2 + 1] + f[1] * st.rise[i]));
            st.tail[i * 2] = g[0] * fall; st.tail[i * 2 + 1] = g[1] * fall;
        }
        host_stretched += ST_HOP;
        st.where += ST_HOP * tempo;  /* the nominal grid; the choice only moves within +-ST_SEEK */
        st.ref = at + ST_HOP;
        /* still needed: the template at ref and the next search, which starts at least at
           where (ST_SEEK <= the shortest input hop) */
        ring_read = (long)floor(st.where) < at ? (long)floor(st.where) : at;
    }
}

/* Output frames from the stretcher's fifo at `ratio` input frames per output frame;
   returns how many it could make. */
static unsigned host_resample(int16_t *out, unsigned count, double ratio, double *phase)
{
    unsigned i;
    for (i = 0; i < count; i++) {
        long r = st_read;
        double f = *phase;
        const int16_t *a, *b;
        if (st_write - r < 2) break;
        a = &st_fifo[(r % ST_FIFO) * 2];
        b = &st_fifo[((r + 1) % ST_FIFO) * 2];
        out[i * 2] = (int16_t)(a[0] + (b[0] - a[0]) * f);
        out[i * 2 + 1] = (int16_t)(a[1] + (b[1] - a[1]) * f);
        f += ratio;
        while (f >= 1.0) { f -= 1.0; r++; }
        *phase = f;
        st_read = r;
    }
    return i;
}

/* ---------------- the device side: SDL's device thread, once per period ---------------- */
static struct {
    unsigned target;          /* the cushion, frames */
    int running, started, trace, stretch;
    double phase, last_trace, speed, speed_at;
    long speed_write;
    unsigned trace_min, trace_max;
} out;

/* `total` bytes are about to be pulled from the stream (one device period); top it up. */
static void SDLCALL host_callback(void *unused, SDL_AudioStream *stream, int additional, int total)
{
    const unsigned per_ms = SND_RATE / 1000;
    static int16_t buf[ST_FIFO * 2];
    unsigned request = (unsigned)total / 4, padding, level, queued, need, made = 0, target;
    unsigned device = DEVICE_MS * per_ms;
    (void)unused; (void)additional;
    ring_end = ring_write;
    SDL_MemoryBarrierAcquire();
    /* what the device holds when SDL asks (up to a period) plus the stream: WASAPI's padding */
    padding = (unsigned)SDL_GetAudioStreamQueued(stream) / 4 + request;
    if (device < 2 * request) device = 2 * request;  /* every top-up covers the pull */
    target = out.target < device + 10 * per_ms ? device + 10 * per_ms : out.target;
    {   /* the game's sound rate against real time, smoothed over ~0.5 s */
        double now = host_wall_seconds(), dt = now - out.speed_at;
        if (dt >= 0.05) {
            long w = ring_end;
            double inst = (w - out.speed_write) / (dt * SND_RATE), a = dt / 0.5;
            if (out.running) out.speed += (a > 1 ? 1 : a) * ((inst > 1.5 ? 1.5 : inst) - out.speed);
            out.speed_write = w; out.speed_at = now;
        }
    }
    level = (unsigned)(ring_end - ring_read) + (unsigned)(st_write - st_read);
    queued = padding + level;
    if (level > target + SKIP_MS * per_ms) {
        st_leave();
        host_tempo = 1.0;
        level = (unsigned)(ring_end - ring_read) + (unsigned)(st_write - st_read);
        /* far behind the game (it caught up after a stall): skip to the cushion */
        long skip = (long)(queued - target);
        if (skip > ring_end - ring_read) skip = ring_end - ring_read;
        ring_read += skip;
        host_skipped += skip;
        level -= (unsigned)skip;
        queued = target;
    }
    need = device > padding ? device - padding : 0;
    if (!need) return;
    if (need > ST_FIFO - ST_HOP) need = ST_FIFO - ST_HOP;
    host_histograms(queued);
    if (out.trace) {
        double now = host_wall_seconds();
        if (queued < out.trace_min) out.trace_min = queued;
        if (queued > out.trace_max) out.trace_max = queued;
        if (now - out.last_trace >= 1.0) {
            hle_log("snd host: queue %u-%u ms, rate %+.0f ppm, game speed %.3f, tempo %.3f, stretched %.1f s, underruns %ld, silence %.1f ms, dropped %ld, skipped %ld\n",
                    out.trace_min / per_ms, out.trace_max / per_ms, (host_ratio - 1) * 1e6, out.speed, host_tempo,
                    host_stretched / (double)SND_RATE, host_underruns, host_silence / (double)per_ms, host_dropped, host_skipped);
            out.trace_min = ~0u; out.trace_max = 0; out.last_trace = now;
        }
    }
    if (!out.running && queued >= target) out.running = out.started = 1;
    if (out.running) {
        /* Slower than real time (heavy scene): stretch to the game's speed, corrected by the
           cushion; join below 97% or 3/4 of the cushion, leave at full speed and a full cushion. */
        double fill = ((double)queued - target) / target, tempo = 1.0;
        int slow = out.stretch && (out.speed < 0.97 || queued < target * 3 / 4);
        if (out.stretch && (host_tempo < 1.0 ? !(out.speed > 0.99 && queued >= target) : slow)) {
            tempo = out.speed * (1 + 0.5 * fill);
            tempo = tempo < 0.5 ? 0.5 : tempo > 0.999 ? 0.999 : tempo;
        }
        host_tempo = tempo;
        /* proportional rate control: +0.5% when 50% over the cushion (copying only) */
        double error = tempo < 1.0 ? 0 : fill * 0.01;
        host_ratio = 1.0 + (error > RATE_RANGE ? RATE_RANGE : error < -RATE_RANGE ? -RATE_RANGE : error);
        st_fill(need + 2, tempo);
        made = host_resample(buf, need, host_ratio, &out.phase);
        if (made < need && padding - request + made < request) {
            out.running = 0; /* out of cushion: this pull runs short; one gap, then build it again */
            host_underruns++;
            st_leave();
            host_tempo = 1.0;
        }
    }
    if (made < need && !out.running) {
        memset(buf + made * 2, 0, (need - made) * 4);
        if (out.started) host_silence += need - made;
        made = need;
    }
    host_frames += made;
    voice_mix(buf, (int)made);   /* the other players' voices over the game's output (voice.c) */
    SDL_PutAudioStreamData(stream, buf, (int)(made * 4));
}

static void host_start(void)
{
    SDL_AudioSpec spec;
    SDL_AudioStream *stream;
    const char *stretch = getenv("DHD_SND_STRETCH");
    if (getenv("DHD_HEADLESS"))
        return;
    out.target = (unsigned)host_env_ms("DHD_SND_LATENCY_MS", 70, 20, 400) * (SND_RATE / 1000);
    out.stretch = !(stretch && *stretch == '0');
    out.trace = getenv("DHD_SND_HOST_TRACE") != NULL;
    out.speed = 1.0;
    out.last_trace = out.speed_at = host_wall_seconds();
    out.speed_write = ring_write;
    out.trace_min = ~0u;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        if (hle_verbose || trace_on())
            hle_log("989snd: SDL audio did not start (%s)\n", SDL_GetError());
        return;
    }
    /* A normal-priority hint: the SDL_AUDIO_DEVICE_SAMPLE_FRAMES environment variable wins. */
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, PERIOD_FRAMES);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = SND_RATE;
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, host_callback, NULL);
    if (!stream) {
        if (hle_verbose || trace_on())
            hle_log("989snd: no SDL audio device (%s)\n", SDL_GetError());
        return;
    }
    voice_open();   /* the voice chat's thread: the microphone, the encoder (voice.c) */
    SDL_ResumeAudioStreamDevice(stream);
    {
        SDL_AudioSpec device;
        int frames = 0;
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &device, &frames);
        hle_log("989snd: SDL audio output started (%d Hz stereo PCM, %s, device %d Hz %d ch, %d-frame period)\n",
                SND_RATE, SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
                device.freq, device.channels, frames);
    }
    /* The stream lives as long as the process; SDL_Quit (or the process end) closes it. */
}

/* ---- the interface (host_api.h, "sound output") ---- */

void host_sound_open(void)
{
    if (host_started)
        return;
    host_started = 1;
    if (!getenv("DHD_SND_OFF"))
        host_start();
}

void host_sound_write(const int16_t *stereo, int frames)
{
    ring_push(stereo, frames);
}

void host_sound_reset(void)
{
    ring_read = ring_write = 0;
}

/* Wall-clock diagnostics: how evenly the game thread renders (DHD_BENCH only). */
void host_sound_tick(void)
{
    static double previous;
    double now = host_wall_seconds();
    if (previous > 0) {
        unsigned bin = (unsigned)((now - previous) * 2000);
        host_tick_hist[bin < 127 ? bin : 127]++;
    }
    previous = now;
}

void host_sound_log_stack(void)
{
    hle_log("989snd: unresolved-play stack:\n");
    host_backtrace();
}
