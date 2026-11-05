/* dhd.log's perf: lines (finding 129): how fast the game ran and what held it back, for every
   player, so a low-fps report can be read from the log alone.

   - "perf: render ..." when the picture's size, the window, the pacing or the present mode
     changes (once they have held for 2 s): the renderer and scale, the picture's size in pixels
     (the internal resolution), the window's drawable size and the part the picture fills, the
     requested frame rate and the pacing, V-Sync and the present mode the surface took, the
     display's refresh, the GPU's kind and whether its timer works.
   - "perf: race ..." / "perf: menus ..." after every 30 s of that kind of play with the window in
     front, and "perf: session race|menus ..." at exit for the whole run.
   Each summary has the frames shown, the average fps against the requested, the 1% low (the mean
   of the slowest hundredth of frames, as fps), the share of frames longer than 1.2 times the
   frame budget, the longest frame; each counted thread's CPU time and busy time (not waiting) per
   field, the render thread's time inside the present, the GPU's time per frame; and `limit`, the
   one word for what held the frame rate back (perf_limit below; the rule is in the finding).

   What it costs: per picture, one clock read and a histogram count here (under an uncontended
   spinlock); a few clock reads per wait in the threads that note waits (graphics_queue.c's
   counters, host_wait_wall, the native renderer's handoffs); once a second, the threads' CPU
   times (an OS call each) and the GPU counters (one Rust call); a log line at most every 2 s.
   The log itself is written by host.c's pipe thread, so a slow disk never holds a game thread.

   Threads: game (the EE), vu (VIF1/VU1), gs (the GS worker), native (the render thread, "render"
   in the lines). A thread that does not exist in this run (no graphics threads, the exact
   renderer's missing render thread) or a value the platform cannot measure is n/a. */
#include "perf_log.h"
#include "../render/include/dhd_render_ext.h"   /* dhdr_perf_read */
#include <SDL3/SDL_atomic.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double host_wall_seconds(void);                                              /* host_sdl.c */
void host_wait_sink(_Atomic uint64_t *nanoseconds);                          /* host_sdl.c */
unsigned host_thread_cpu(const char **names, double *seconds, unsigned max); /* host.c */
double host_pacing_period(void);                                             /* host.c */
void pacing_handed(double *due, double *began);                              /* host.c */
extern uint64_t kernel_vsync_count;                                          /* host.c: the field */
void graphics_wait_seconds(double out[3]);  /* graphics_queue.c: the game's, vu's and gs's queue waits */

enum { T_GAME, T_VU, T_GS, T_RENDER, THREADS };
static const char *const thread_names[THREADS] = {"game", "vu", "gs", "native"};  /* host_thread_name's */
static const char *const thread_labels[THREADS] = {"game", "vu", "gs", "render"};

enum { BINS = 1000 };            /* frame times in 0.25 ms bins up to 250 ms; longer ones in a tail */
static const double BIN_MS = 0.25;
static const double WINDOW_SECONDS = 30, SEGMENT_SECONDS = 1, STABLE_SECONDS = 2;

static int on = -1;
int perf_log_on(void)
{
    if (on < 0) {
        const char *s = getenv("DHD_PERF_LOG");
        on = !s || strcmp(s, "0") != 0;
    }
    return on;
}

/* ---- the waits threads note themselves ---- */

static _Atomic uint64_t self_wait_ns[THREADS];
static _Atomic uint64_t present_wait_ns;
static _Thread_local int thread_slot = -1;

void perf_thread(const char *name)
{
    for (int i = 0; i < THREADS; i++)
        if (!strcmp(name, thread_names[i])) {
            thread_slot = i;
            if (perf_log_on()) host_wait_sink(&self_wait_ns[i]);  /* its pacing waits */
        }
}

void perf_wait_self(double seconds)
{
    if (thread_slot >= 0 && seconds > 0)
        atomic_fetch_add_explicit(&self_wait_ns[thread_slot], (uint64_t)(seconds * 1e9), memory_order_relaxed);
}

void perf_present_wait(double seconds)
{
    if (seconds > 0) atomic_fetch_add_explicit(&present_wait_ns, (uint64_t)(seconds * 1e9), memory_order_relaxed);
}

/* DHD_TIMELINE's per-field rows (graphics_timeline.h, finding 156): the render thread's own waits
   so far (the next field, its pacing) and its time inside the present, in seconds. */
void perf_render_waits(double out[2])
{
    out[0] = atomic_load_explicit(&self_wait_ns[T_RENDER], memory_order_relaxed) * 1e-9;
    out[1] = atomic_load_explicit(&present_wait_ns, memory_order_relaxed) * 1e-9;
}

/* ---- present.c's view of the window and the game ---- */

static SDL_AtomicInt window_state;  /* bit 0: in front; the rest counts the changes */
static _Atomic double refresh;      /* the display's refresh in Hz, 0 unknown */
static SDL_AtomicInt riding;

void perf_window(int front, double hz)
{
    int old = SDL_GetAtomicInt(&window_state);
    if ((old & 1) != (front != 0)) SDL_SetAtomicInt(&window_state, ((old >> 1) + 1) << 1 | (front != 0));
    atomic_store_explicit(&refresh, hz > 0 ? hz : 0, memory_order_relaxed);
}

void perf_field(int ride) { SDL_SetAtomicInt(&riding, ride != 0); }

/* ---- the counters, once a segment (at most a second) ---- */

typedef struct {
    double wall;
    uint64_t fields;
    unsigned cpu_mask;  /* the threads host_thread_cpu found */
    double cpu[THREADS], wait[THREADS], present;
    uint64_t gpu[6];    /* dhdr_perf_read: timer state, frames sampled, busy ns, span ns, present mode, adapter */
} Snap;

static void snap(Snap *s, double now)
{
    memset(s, 0, sizeof *s);
    s->wall = now;
    s->fields = kernel_vsync_count;
    const char *names[16];
    double seconds[16];
    unsigned n = host_thread_cpu(names, seconds, 16);
    for (unsigned i = 0; i < n; i++)
        for (int t = 0; t < THREADS; t++)
            if (!(s->cpu_mask & 1u << t) && !strcmp(names[i], thread_names[t])) s->cpu_mask |= 1u << t, s->cpu[t] = seconds[i];
    double queue[3];
    graphics_wait_seconds(queue);
    for (int t = 0; t < THREADS; t++)
        s->wait[t] = (t < 3 ? queue[t] : 0) + atomic_load_explicit(&self_wait_ns[t], memory_order_relaxed) * 1e-9;
    s->present = atomic_load_explicit(&present_wait_ns, memory_order_relaxed) * 1e-9;
    dhdr_perf_read(s->gpu, 6);
}

/* One kind of play's sums: race (1) or menus (0). */
typedef struct {
    double seconds;                 /* wall time of its segments in front */
    uint64_t frames, over;          /* frame intervals counted, those longer than 1.2x the budget */
    double frame_ms, max_ms, over_ms;
    double requested;               /* the requested rate at the last frame counted (0 unknown) */
    int paced;
    uint32_t bins[BINS];
    uint64_t tail;                  /* frames of 250 ms and more, and their sum */
    double tail_ms;
    uint64_t fields;
    unsigned cpu_mask;
    double cpu[THREADS], wait[THREADS], present;
    uint64_t gpu_frames;
    double gpu_busy_ms;
    int gpu_timer;                  /* the last segment's timer state */
} Sums;

static Sums window_sums[2], session_sums[2];
static SDL_SpinLock lock;
static struct { int open, state, window; double start; Snap base; } segment;
static double last_present;

static void count_frame(Sums *s, double ms, double over_ms, double requested, int paced)
{
    s->requested = requested, s->paced = paced;
    s->frames++;
    s->frame_ms += ms;
    if (ms > s->max_ms) s->max_ms = ms;
    if (over_ms > 0 && ms > over_ms) s->over++;
    s->over_ms = over_ms;
    unsigned bin = (unsigned)(ms / BIN_MS);
    if (bin < BINS) s->bins[bin]++;
    else s->tail++, s->tail_ms += ms;
}

static void add_segment(Sums *s, const Snap *a, const Snap *b)
{
    s->seconds += b->wall - a->wall;
    s->fields += b->fields - a->fields;
    s->cpu_mask |= a->cpu_mask & b->cpu_mask;
    for (int t = 0; t < THREADS; t++) {
        if (a->cpu_mask & b->cpu_mask & 1u << t) s->cpu[t] += b->cpu[t] - a->cpu[t];
        s->wait[t] += b->wait[t] - a->wait[t];
    }
    s->present += b->present - a->present;
    s->gpu_timer = (int)b->gpu[0];
    if (b->gpu[1] >= a->gpu[1]) {
        s->gpu_frames += b->gpu[1] - a->gpu[1];
        s->gpu_busy_ms += (double)(b->gpu[2] - a->gpu[2]) * 1e-6;
    }
}

/* The requested frame rate (0 unknown) and whether the frames are paced at all. */
static double requested_fps(int *paced)
{
    static int unpaced = -1;
    if (unpaced < 0) {
        const char *s = getenv("DHD_FRAME_CLOCK");
        unpaced = s && !strcmp(s, "1");
    }
    *paced = !unpaced;
    double period = host_pacing_period();
    return period > 0 ? 1 / period : 0;
}

static const char *present_mode_name(uint64_t mode)
{
    static const char *const names[] = {"n/a", "immediate", "mailbox", "fifo", "fifo-relaxed"};
    return mode < 5 ? names[mode] : "n/a";
}

/* The mean of the slowest hundredth of frames, as frames per second. */
static double low_1pct(const Sums *s)
{
    uint64_t want = s->frames / 100 ? s->frames / 100 : 1, left = want;
    double sum = 0;
    if (s->tail) {
        uint64_t take = left < s->tail ? left : s->tail;
        sum += s->tail_ms * (double)take / (double)s->tail;
        left -= take;
    }
    for (int b = BINS - 1; b >= 0 && left; b--) {
        uint64_t take = left < s->bins[b] ? left : s->bins[b];
        sum += (double)take * (b + 0.5) * BIN_MS;
        left -= take;
    }
    return sum > 0 ? 1000.0 * (double)(want - left) / sum : 0;
}

/* What held the frame rate back (finding 129). T: the mean frame time; B: the frame budget (T when
   unpaced). gpu_ms < 0 and busy[t] < 0: not measured. In order:
   1. paced and at least 97% of the requested rate: none;
   2. the largest of the GPU's time and each thread's busy time at 85% of B or more: gpu or
      cpu:<thread>;
   3. the render thread held inside the present for a quarter of T or more: gpu when the GPU's time
      is unknown or at least half of T, or the present mode never waits for the display
      (immediate); else vsync (the display or the compositor holds the present);
   4. a present mode that can wait for the display (fifo, fifo-relaxed, mailbox) and the rate
      within 3% of the refresh or a whole fraction of it: vsync;
   5. else the largest of step 2 (nothing reaches the budget alone: the threads' chain, or the OS). */
static const char *perf_limit(double fps, double requested, int paced, double gpu_ms, const double busy[THREADS],
                              double present_ms, uint64_t present_mode, double hz, char *out, size_t size)
{
    if (fps <= 0) return "n/a";
    double frame = 1000 / fps, budget = paced && requested > 0 ? 1000 / requested : frame;
    if (paced && requested > 0 && fps >= 0.97 * requested) return "none";
    const char *name = NULL;
    double most = -1;
    if (gpu_ms >= 0) most = gpu_ms, name = "gpu";
    for (int t = 0; t < THREADS; t++)
        if (busy[t] >= 0 && busy[t] > most) {
            most = busy[t];
            snprintf(out, size, "cpu:%s", thread_labels[t]);
            name = out;
        }
    if (name && most >= 0.85 * budget) return name;
    if (present_ms >= 0.25 * frame)
        return gpu_ms < 0 || gpu_ms >= 0.5 * frame || present_mode == 1 ? "gpu" : "vsync";
    int waits = present_mode >= 2 && present_mode <= 4;
    for (int k = 1; waits && hz > 0 && k <= 4; k++)
        if (fabs(fps - hz / k) <= 0.03 * hz / k) return "vsync";
    return name ? name : "n/a";
}

static void print_sums(const char *label, Sums *s)
{
    int paced = s->paced;
    double requested = s->requested;
    double frame_seconds = s->frame_ms / 1000, fps = frame_seconds > 0 ? (double)s->frames / frame_seconds : 0;
    char want[24], over[48], line[1024], limit[24], gpu[24], present[24], thread[THREADS][32];
    if (!paced) snprintf(want, sizeof want, "uncapped");
    /* (as an integer: the exit's lines come from the game thread, whose FPU rounds toward zero,
       so %.0f would print 59.94 as 59; the other values lose at most their last digit there) */
    else if (requested > 0) snprintf(want, sizeof want, "%u", (unsigned)(requested + 0.5));
    else snprintf(want, sizeof want, "n/a");
    if (s->over_ms > 0)
        snprintf(over, sizeof over, "%.1f%%(>%.1fms)", s->frames ? 100.0 * (double)s->over / (double)s->frames : 0,
                 s->over_ms);
    else snprintf(over, sizeof over, "n/a");  /* unpaced: no budget */
    double busy[THREADS];
    for (int t = 0; t < THREADS; t++) {
        busy[t] = -1;
        if (!(s->cpu_mask & 1u << t) || !s->fields) { snprintf(thread[t], sizeof thread[t], "n/a"); continue; }
        double cpu = s->cpu[t] * 1000 / (double)s->fields;
        double idle = s->wait[t] + (t == T_RENDER ? s->present : 0);
        busy[t] = (s->seconds - idle) * 1000 / (double)s->fields;
        if (busy[t] < 0) busy[t] = 0;
        snprintf(thread[t], sizeof thread[t], "%.2f/%.2f", cpu, busy[t]);
    }
    double present_ms = -1;
    if ((s->cpu_mask & 1u << T_RENDER) && s->fields) present_ms = s->present * 1000 / (double)s->fields;
    if (present_ms >= 0) snprintf(present, sizeof present, "%.2f", present_ms);
    else snprintf(present, sizeof present, "n/a");
    double gpu_ms = s->gpu_frames ? s->gpu_busy_ms / (double)s->gpu_frames : -1;
    if (gpu_ms >= 0) snprintf(gpu, sizeof gpu, "%.2f", gpu_ms);
    else snprintf(gpu, sizeof gpu, "n/a");
    Snap now;
    dhdr_perf_read(now.gpu, 6);
    const char *verdict = perf_limit(fps, requested, paced, gpu_ms, busy, present_ms, now.gpu[4],
                                     atomic_load_explicit(&refresh, memory_order_relaxed), limit, sizeof limit);
    snprintf(line, sizeof line,
             "perf: %s %.0f s frames=%llu fps=%.1f/%s low1%%=%.1f over=%s max=%.1fms | ms/field cpu/busy "
             "game=%s vu=%s gs=%s render=%s present_wait=%s | gpu_ms/frame=%s | limit=%s\n",
             label, s->seconds, (unsigned long long)s->frames, fps, want, low_1pct(s), over, s->max_ms, thread[T_GAME],
             thread[T_VU], thread[T_GS], thread[T_RENDER], present, gpu, verdict);
    fputs(line, stderr);
    fflush(stderr);
}

/* ---- the render line: what the frame rate depends on ---- */

static struct {
    const char *renderer;
    unsigned scale, pw, ph, ww, wh, vw, vh;
} size_now;

void perf_render_size(const char *renderer, unsigned scale, unsigned pw, unsigned ph, unsigned ww, unsigned wh,
                      unsigned vw, unsigned vh)
{
    size_now.renderer = renderer;
    size_now.scale = scale, size_now.pw = pw, size_now.ph = ph;
    size_now.ww = ww, size_now.wh = wh, size_now.vw = vw, size_now.vh = vh;
}

typedef struct {
    const char *renderer;
    unsigned scale, pw, ph, ww, wh, vw, vh, fps, pacing, mode, hz100;
} Shown;

static void render_line(double now)
{
    static Shown logged, seen;
    static double seen_at;
    static int any;
    int paced;
    double requested = requested_fps(&paced), due, began;
    pacing_handed(&due, &began);
    uint64_t gpu[6];
    dhdr_perf_read(gpu, 6);
    Shown s;
    memset(&s, 0, sizeof s);
    s.renderer = size_now.renderer;
    s.scale = size_now.scale, s.pw = size_now.pw, s.ph = size_now.ph;
    s.ww = size_now.ww, s.wh = size_now.wh, s.vw = size_now.vw, s.vh = size_now.vh;
    s.fps = (unsigned)(requested + 0.5);
    s.pacing = !paced ? 0 : due > 0 ? 1 : 2;
    s.mode = (unsigned)gpu[4];
    s.hz100 = (unsigned)(atomic_load_explicit(&refresh, memory_order_relaxed) * 100 + 0.5);
    if (memcmp(&s, &seen, sizeof s)) { seen = s, seen_at = now; return; }
    if (now - seen_at < STABLE_SECONDS || (any && !memcmp(&s, &logged, sizeof s))) return;
    logged = s, any = 1;
    static const char *const pacing[] = {"unpaced", "just-in-time", "wait before VBLANK"};
    static const char *const kinds[] = {"other", "integrated", "discrete", "virtual", "cpu (software)"};
    static const char *const timer[] = {"off", "on", "not offered by this GPU"};
    const char *vsync = getenv("DHD_VSYNC"), *tearing = getenv("DHD_TEARING");
    int no_tearing = (vsync && atoi(vsync)) || (tearing && !atoi(tearing));
    char view[32], hz[24], rate[16], line[640];
    if (s.vw && s.vh) snprintf(view, sizeof view, "%ux%u", s.vw, s.vh);
    else snprintf(view, sizeof view, "n/a");
    if (s.hz100) snprintf(hz, sizeof hz, "%.2f Hz", s.hz100 / 100.0);
    else snprintf(hz, sizeof hz, "n/a");
    if (!paced) snprintf(rate, sizeof rate, "uncapped");
    else if (s.fps) snprintf(rate, sizeof rate, "%u", s.fps);
    else snprintf(rate, sizeof rate, "n/a");
    snprintf(line, sizeof line,
             "perf: render %s scale=%u picture=%ux%u window=%ux%u shown=%s | fps=%s pacing=%s vsync=%s "
             "present_mode=%s display=%s | gpu=%s gpu_timer=%s\n",
             s.renderer ? s.renderer : "n/a", s.scale, s.pw, s.ph, s.ww, s.wh, view, rate, pacing[s.pacing],
             no_tearing ? "on" : "off", present_mode_name(s.mode), hz, gpu[5] < 5 ? kinds[gpu[5]] : "n/a",
             gpu[0] < 3 ? timer[gpu[0]] : "n/a");
    fputs(line, stderr);
    fflush(stderr);
}

/* ---- segments and windows ---- */

/* Closes the open segment at now: its counters go to its kind of play when the window stayed in
   front all through it (a change of window_state closes it, and then its time is not counted:
   it may hold the background time), and a kind of play with 30 s in its window logs a line. */
static void segment_close(double now, int counted)
{
    Snap s;
    snap(&s, now);
    int state = segment.state;
    if (counted && (segment.window & 1)) {
        add_segment(&window_sums[state], &segment.base, &s);
        add_segment(&session_sums[state], &segment.base, &s);
        if (window_sums[state].seconds >= WINDOW_SECONDS) {
            print_sums(state ? "race" : "menus", &window_sums[state]);
            memset(&window_sums[state], 0, sizeof window_sums[state]);
        }
    }
    segment.base = s;
    segment.start = now;
}

static void perf_exit(void)
{
    SDL_LockSpinlock(&lock);
    if (segment.open) segment_close(host_wall_seconds(), 1);
    int any = 0;
    for (int state = 1; state >= 0; state--)
        if (session_sums[state].frames) {
            print_sums(state ? "session race" : "session menus", &session_sums[state]);
            any = 1;
        }
    if (!any) {
        fputs("perf: session: no picture was shown with the window in front\n", stderr);
        fflush(stderr);
    }
    segment.open = 0;
    on = 0;  /* a picture after this (another thread) counts nothing */
    SDL_UnlockSpinlock(&lock);
}

void perf_presented(void)
{
    if (!perf_log_on()) return;
    double now = host_wall_seconds();
    int state = SDL_GetAtomicInt(&riding), window = SDL_GetAtomicInt(&window_state);
    SDL_LockSpinlock(&lock);
    if (!on) { SDL_UnlockSpinlock(&lock); return; }
    if (!segment.open) {
        static int started;
        if (!started) {
            started = 1;
            atexit(perf_exit);
            fputs("perf: a line every 30 s of race or menus with the window in front, and the whole session at "
                  "exit: frames shown, fps shown/requested, low1% = the slowest 1% of frames as fps, over = frames "
                  "longer than 1.2x the budget; per field, each thread's CPU ms / busy ms (not waiting), the "
                  "render thread's wait inside the present; the GPU's ms per frame; limit = gpu, cpu:<thread>, "
                  "vsync or none (finding 129)\n", stderr);
        }
        segment.open = 1, segment.state = state, segment.window = window;
        snap(&segment.base, now);
        segment.start = now;
        last_present = now;
        SDL_UnlockSpinlock(&lock);
        return;
    }
    if ((window & 1) && window == segment.window && state == segment.state && last_present > 0) {
        int paced;
        double requested = requested_fps(&paced), ms = (now - last_present) * 1000;
        double over_ms = paced && requested > 0 ? 1.2 * 1000 / requested : 0;
        count_frame(&window_sums[state], ms, over_ms, requested, paced);
        count_frame(&session_sums[state], ms, over_ms, requested, paced);
    }
    last_present = now;
    if (window != segment.window || state != segment.state || now - segment.start >= SEGMENT_SECONDS) {
        segment_close(now, window == segment.window);
        segment.state = state, segment.window = window;
        render_line(now);
    }
    SDL_UnlockSpinlock(&lock);
}
