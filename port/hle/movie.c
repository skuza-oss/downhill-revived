/* The game's movies (MOV/<name>.PSS on the player's disc), played natively (finding 055).

   On the PS2 the game plays a movie with Sony's sample stream player (the "mpegstr" sample's
   videodec / vibuf / audiodec, 0x0023B050-0x0023E188 and 0x002422B0-0x00242998): a thread runs
   libmpeg (sceMpegGetPicture) on the IPU, fed by a toIPU DMA chain the game builds itself; a
   VBLANK handler sends finished pictures to the GS over PATH3; the PCM goes to 989snd's external
   stream through SIF DMA. None of that hardware is on the PC. Instead the player's three entry
   points are patched (port/patches.py, unit 00223BB0.c):
   - func_00223BB0(name, audio): host_movie_start; the game's flags say a movie runs;
   - func_00223C60(keep): host_movie_update each game frame; keep == 0 is the player's skip;
   - when the movie is over the flags say so, as func_00224308 leaves them.

   Here a host thread reads the file from the disc image (its own handle), splits the MPEG
   program stream, decodes the MPEG-2 video with libmpeg2 (port/third_party/libmpeg2, GPL-2+)
   into RGBA pictures and the PCM into a ring. Every movie on the disc is 640x368 progressive
   MPEG-2 at 29.97 or 30 pictures/s with 48 kHz 16-bit stereo PCM ("SShd"/"SSbd", 0x200-byte
   interleave), in Sony's PSS layout: private stream 1 (0xBD) packets with a 4-byte sub-header.

   Timing: the movie's clock is the number of sound samples the mixer rendered since it started
   (sound.c calls movie_audio for every rendered chunk, 4004/5 samples a field), so picture and
   sound can't drift apart. The picture shown is the last one whose time has come; a picture the
   decoder hasn't finished yet keeps the previous one on screen, and late sound is dropped, so a
   slow disc read never moves the clock. The presenter takes the picture each field
   (movie_picture, port/gs/present.c), centred in 640x448 as the PS2 showed it (DISPLAY2 at DY
   90 instead of 50: 40 black lines above and below).

   A movie that can't play (file missing, nothing decodes) ends at the update after next (never in
   the frame it started, see host_movie_update): the game goes on as after a finished one. */
#include "hle.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include "../host/host_os.h"  /* host_os_fopen (UTF-8 paths), host_fseek (64-bit) */

#include "../third_party/libmpeg2/mpeg2.h"

enum {
    PICTURE_WIDTH = 640, PICTURE_HEIGHT = 448,  /* what the presenter gets */
    SLOTS = 20,                                 /* decoded pictures ahead (0.67 s: the files' video
                                                   leads their sound by up to 0.46 s) */
    RING = 1 << 17,                             /* sound frames ahead (2.7 s) */
    RATE = 48000,
    READ = 256 * 1024
};

typedef struct {
    uint32_t *rgba;     /* width x height */
    int64_t index;      /* the picture's number in display order, -1: free */
} Slot;

static struct {
    SDL_Mutex *lock;
    SDL_Condition *changed;
    SDL_Thread *thread;
    volatile int stop;
    int active, audio;
    /* the decoder thread's results, under the lock */
    Slot slots[SLOTS];
    int width, height;          /* of the pictures */
    unsigned frame_period;      /* 27 MHz ticks per picture (libmpeg2's sequence) */
    int64_t decoded;            /* pictures decoded so far */
    int ended;                  /* the file is read and decoded to its end */
    int failed;                 /* nothing to show */
    int64_t audio_offset;       /* sound frame where the PCM starts on the movie's clock */
    int16_t *ring;              /* RING stereo frames */
    int64_t ring_write;         /* sound frames demuxed so far */
    int64_t ring_read;          /* sound frames the mixer took or dropped */
    int audio_ok;               /* PCM16 stereo at 48 kHz */
    /* the movie's clock and picture, game thread */
    int64_t clock;              /* sound frames rendered since the movie began */
    int64_t shown;              /* picture number in `picture`, -1: none yet */
    uint32_t *picture;          /* PICTURE_WIDTH x PICTURE_HEIGHT, the presented picture */
    /* the file */
    char iso[1024];
    uint32_t lsn, size;
    char name[64];
} M;

static int inited;

/* The last picture of the movie that ended, kept for the presenter (movie_picture, movie_drop): the
   PS2's player shows its pictures from a buffer of its own, which stays on screen after the movie
   until the game sets its display again (finding 160). */
static uint32_t *held;
static unsigned held_rows;

/* The presenter is done with the last movie's picture (or a new movie starts). */
void movie_drop(void)
{
    free(held);
    held = NULL;
}

static void movie_init_once(void)
{
    if (inited) return;
    M.lock = SDL_CreateMutex();
    M.changed = SDL_CreateCondition();
    inited = 1;
}

/* ---------------- decoder thread ---------------- */

typedef struct {
    FILE *f;
    uint32_t left;                /* bytes of the file not yet read */
    uint8_t *buf;
    size_t start, end;            /* unparsed bytes */
    mpeg2dec_t *dec;
    int64_t pts_first_video, pts_first_audio;
    uint8_t audio_header[40];     /* "SShd" (8 + 24) and "SSbd" (8) */
    unsigned audio_header_have;
    uint32_t audio_bytes_left;    /* SSbd's size */
    uint8_t block[0x400];         /* one interleave block: 0x200 left, 0x200 right */
    unsigned block_have;
    unsigned interleave;
} Demux;

static int refill(Demux *d, size_t want)
{
    if (d->end - d->start >= want) return 1;
    memmove(d->buf, d->buf + d->start, d->end - d->start);
    d->end -= d->start;
    d->start = 0;
    while (d->left && d->end < READ) {
        size_t n = READ - d->end;
        if (n > d->left) n = d->left;
        size_t got = fread(d->buf + d->end, 1, n, d->f);
        if (!got) { d->left = 0; break; }
        d->end += got;
        d->left -= (uint32_t)got;
    }
    return d->end - d->start >= want;
}

/* Waits under the lock until cond holds or the movie stops; nonzero if it holds. */
#define WAIT_UNTIL(cond) \
    do { while (!(cond) && !M.stop) SDL_WaitCondition(M.changed, M.lock); } while (0)

static int64_t clock_picture(void);

static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

/* ITU-R BT.601, studio range (MPEG-2's default colour description, and the IPU's CSC), each
   chroma sample covering 2x2 pixels as the IPU's colour conversion does. Fixed point, 1 << 16. */
static void to_rgba(uint32_t *out, int width, int height, uint8_t *const planes[3], int y_stride, int c_stride)
{
    for (int y = 0; y < height; y++) {
        const uint8_t *py = planes[0] + y * y_stride;
        const uint8_t *pu = planes[1] + (y >> 1) * c_stride, *pv = planes[2] + (y >> 1) * c_stride;
        uint32_t *o = out + y * width;
        for (int x = 0; x < width; x++) {
            int l = (py[x] - 16) * 76309, u = pu[x >> 1] - 128, v = pv[x >> 1] - 128;
            int r = (l + 104597 * v + 32768) >> 16;
            int g = (l - 25675 * u - 53279 * v + 32768) >> 16;
            int b = (l + 132201 * u + 32768) >> 16;
            o[x] = clamp8(r) | (uint32_t)clamp8(g) << 8 | (uint32_t)clamp8(b) << 16 | 0xFF000000u;
        }
    }
}

static void picture_out(Demux *d, const mpeg2_info_t *info)
{
    const mpeg2_sequence_t *s = info->sequence;
    if (!info->display_fbuf || !s) return;
    int w = (int)s->picture_width, h = (int)s->picture_height;
    if (w > PICTURE_WIDTH) w = PICTURE_WIDTH;
    if (h > PICTURE_HEIGHT) h = PICTURE_HEIGHT;
    SDL_LockMutex(M.lock);
    int64_t k = M.decoded;
    Slot *slot = &M.slots[k % SLOTS];
    /* A slot is free once the clock is past its picture, or the presenter has copied it. */
    WAIT_UNTIL(slot->index < 0 || slot->index <= M.shown || slot->index < clock_picture());
    if (M.stop) { SDL_UnlockMutex(M.lock); return; }
    M.width = w; M.height = h;
    if (s->frame_period) M.frame_period = s->frame_period;
    SDL_UnlockMutex(M.lock);
    to_rgba(slot->rgba, w, h, info->display_fbuf->buf, (int)s->width, (int)s->chroma_width);
    SDL_LockMutex(M.lock);
    slot->index = k;
    M.decoded = k + 1;
    SDL_BroadcastCondition(M.changed);
    SDL_UnlockMutex(M.lock);
}

static void video_feed(Demux *d, uint8_t *p, uint8_t *end)
{
    mpeg2_buffer(d->dec, p, end);
    for (;;) {
        const mpeg2_info_t *info = mpeg2_info(d->dec);
        mpeg2_state_t state = mpeg2_parse(d->dec);
        if (state == STATE_BUFFER || M.stop) return;
        if (state == STATE_SLICE || state == STATE_END || state == STATE_INVALID_END) picture_out(d, info);
    }
}

/* The sound frames of one interleave block (0x200 bytes of left, then of right) into the ring.
   It never waits: the pictures hold the decoder back (SLOTS), and while they last the sound is at
   most about a second ahead. Sound beyond the last picture (cut anyway, movie_over) overwrites
   the oldest frames instead of holding the end of the file back. */
static void audio_block(Demux *d)
{
    enum { HALF = 0x200, FRAMES = HALF / 2 };
    SDL_LockMutex(M.lock);
    if (M.ring_write + FRAMES - M.ring_read > RING) M.ring_read = M.ring_write + FRAMES - RING;
    if (!M.stop) {
        const int16_t *l = (const int16_t *)d->block, *r = (const int16_t *)(d->block + HALF);
        for (int i = 0; i < FRAMES; i++) {
            int16_t *o = M.ring + ((M.ring_write + i) & (RING - 1)) * 2;
            o[0] = l[i];
            o[1] = r[i];
        }
        M.ring_write += FRAMES;
        SDL_BroadcastCondition(M.changed);
    }
    SDL_UnlockMutex(M.lock);
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void audio_feed(Demux *d, const uint8_t *p, size_t n)
{
    while (n) {
        if (d->audio_header_have < sizeof d->audio_header) {
            size_t take = sizeof d->audio_header - d->audio_header_have;
            if (take > n) take = n;
            memcpy(d->audio_header + d->audio_header_have, p, take);
            d->audio_header_have += (unsigned)take;
            p += take, n -= take;
            if (d->audio_header_have == sizeof d->audio_header) {
                const uint8_t *h = d->audio_header;
                int ok = !memcmp(h, "SShd", 4) && le32(h + 4) >= 24 && !memcmp(h + 32, "SSbd", 4)
                         && le32(h + 8) == 1 && le32(h + 12) == RATE && le32(h + 16) == 2 && le32(h + 20) == 0x200;
                d->audio_bytes_left = le32(h + 36);
                SDL_LockMutex(M.lock);
                M.audio_ok = ok;
                SDL_UnlockMutex(M.lock);
                if (!ok)
                    hle_log("movie: %s: sound not 48 kHz 16-bit stereo PCM (type %u, %u Hz, %u channels), silent\n",
                            M.name, le32(h + 8), le32(h + 12), le32(h + 16));
            }
            continue;
        }
        if (!M.audio_ok) return;
        size_t take = sizeof d->block - d->block_have;
        if (take > n) take = n;
        if (take > d->audio_bytes_left) take = d->audio_bytes_left;
        if (!take) return;
        memcpy(d->block + d->block_have, p, take);
        d->block_have += (unsigned)take;
        d->audio_bytes_left -= (uint32_t)take;
        p += take, n -= take;
        if (d->block_have == sizeof d->block) {
            audio_block(d);
            d->block_have = 0;
        }
    }
}

static int64_t pes_pts(const uint8_t *h)
{
    return (int64_t)((h[0] >> 1) & 7) << 30 | h[1] << 22 | (h[2] >> 1) << 15 | h[3] << 7 | h[4] >> 1;
}

/* One pass over the MPEG-2 program stream: packs, PES packets (video 0xE0, Sony's private
   stream 1 sound), padding and system headers skipped by their length. */
static void demux(Demux *d)
{
    while (!M.stop && refill(d, 6)) {
        uint8_t *p = d->buf + d->start;
        if (p[0] || p[1] || p[2] != 1) { d->start++; continue; }  /* resynchronise */
        int id = p[3];
        if (id == 0xB9) break;                                    /* program end */
        if (id == 0xBA) {                                         /* pack header */
            if (!refill(d, 14)) break;
            p = d->buf + d->start;
            d->start += (p[4] >> 6) == 1 ? 14 + (p[13] & 7) : 12;
            continue;
        }
        if (id < 0xBB) { d->start += 4; continue; }               /* not a packet start */
        size_t length = (size_t)p[4] << 8 | p[5];
        if (!refill(d, 6 + length)) break;
        p = d->buf + d->start;
        uint8_t *body = p + 6, *end = body + length;
        d->start += 6 + length;
        if (id != 0xE0 && id != 0xBD) continue;
        if (length < 3 || (body[0] & 0xC0) != 0x80) continue;     /* MPEG-2 PES header only */
        uint8_t *payload = body + 3 + body[2];
        if (payload > end) continue;
        int64_t pts = (body[1] & 0x80) && body[2] >= 5 ? pes_pts(body + 3) : -1;
        if (id == 0xE0) {
            if (pts >= 0 && d->pts_first_video < 0) d->pts_first_video = pts;
            video_feed(d, payload, end);
        } else if (M.audio && end - payload > 4 && payload[1] == 0xA0) {  /* sub-header ff a0 00 00 */
            if (pts >= 0 && d->pts_first_audio < 0) {
                d->pts_first_audio = pts;
                if (d->pts_first_video >= 0) {
                    int64_t offset = (pts - d->pts_first_video) * RATE / 90000;
                    SDL_LockMutex(M.lock);
                    M.audio_offset = offset > 0 ? offset : 0;
                    SDL_UnlockMutex(M.lock);
                }
            }
            audio_feed(d, payload + 4, (size_t)(end - payload - 4));
        }
    }
    if (!M.stop) {  /* the last picture: libmpeg2 gives it out at the sequence's end */
        static uint8_t sequence_end[] = {0, 0, 1, 0xB7};
        video_feed(d, sequence_end, sequence_end + sizeof sequence_end);
    }
}

static int SDLCALL decoder_thread(void *unused)
{
    (void)unused;
    host_thread_name("movie");
    Demux d = {0};
    d.pts_first_video = d.pts_first_audio = -1;
    d.buf = malloc(READ);
    d.f = host_os_fopen(M.iso, "rb");  /* its own FILE: this thread reads it alone */
    d.dec = mpeg2_init();
    if (d.buf && d.f && d.dec && !host_fseek(d.f, (int64_t)M.lsn * 2048, SEEK_SET)) {
        d.left = M.size;
        demux(&d);
    } else hle_log("movie: %s: can't read the disc image\n", M.name);
    if (d.dec) mpeg2_close(d.dec);
    if (d.f) fclose(d.f);
    free(d.buf);
    SDL_LockMutex(M.lock);
    M.ended = 1;
    if (!M.decoded) M.failed = 1;
    SDL_BroadcastCondition(M.changed);
    SDL_UnlockMutex(M.lock);
    return 0;
}

/* ---------------- the game's side ---------------- */

static void release(void)
{
    if (M.thread) {
        SDL_LockMutex(M.lock);
        M.stop = 1;
        SDL_BroadcastCondition(M.changed);
        SDL_UnlockMutex(M.lock);
        SDL_WaitThread(M.thread, NULL);
        M.thread = NULL;
    }
    for (int i = 0; i < SLOTS; i++) { free(M.slots[i].rgba); M.slots[i].rgba = NULL; }
    free(M.ring); M.ring = NULL;
    free(M.picture); M.picture = NULL;
    M.active = 0;
}

/* Starts the movie at lsn/size of the disc image iso (decoding begins at once; the clock waits
   for the first picture, at most two seconds). Sound only if audio is nonzero. 0: can't. */
int movie_open(const char *iso, uint32_t lsn, uint32_t size, int audio, const char *name)
{
    movie_init_once();
    release();
    movie_drop();
    memset(M.slots, 0, sizeof M.slots);
    M.stop = 0; M.audio = audio; M.width = M.height = 0; M.frame_period = 900900;  /* 29.97 Hz */
    M.decoded = 0; M.ended = M.failed = 0; M.audio_offset = 0;
    M.ring_write = M.ring_read = 0; M.audio_ok = 0;
    M.clock = 0; M.shown = -1;
    snprintf(M.iso, sizeof M.iso, "%s", iso);
    snprintf(M.name, sizeof M.name, "%s", name);
    M.lsn = lsn; M.size = size;
    int ok = 1;
    for (int i = 0; i < SLOTS; i++) {
        M.slots[i].index = -1;
        ok &= (M.slots[i].rgba = malloc(PICTURE_WIDTH * PICTURE_HEIGHT * 4)) != NULL;
    }
    ok &= (M.ring = malloc(RING * 4)) != NULL;
    ok &= (M.picture = calloc(PICTURE_WIDTH * PICTURE_HEIGHT, 4)) != NULL;
    if (ok) {
        for (int i = 0; i < PICTURE_WIDTH * PICTURE_HEIGHT; i++) M.picture[i] = 0xFF000000u;
        M.thread = SDL_CreateThread(decoder_thread, "movie", NULL);
    }
    if (!ok || !M.thread) { release(); return 0; }
    M.active = 1;
    SDL_LockMutex(M.lock);
    while (!M.decoded && !M.ended)
        if (!SDL_WaitConditionTimeout(M.changed, M.lock, 2000)) break;
    int failed = !M.decoded;
    SDL_UnlockMutex(M.lock);
    if (failed) { hle_log("movie: %s: no picture decoded, skipped\n", name); release(); return 0; }
    return 1;
}

void movie_close(void)
{
    if (inited) release();
}

int movie_playing(void) { return M.active; }

/* Under the lock. */
static int64_t clock_picture(void)
{
    /* picture k shows from k * frame_period / 27 MHz on */
    return M.clock * 27000000 / ((int64_t)RATE * M.frame_period);
}

/* Over: the file decoded to its end and its last picture's time passed. Sound that goes on
   after the last picture is cut there, as on the PS2: the game's decoding thread
   (func_0023C420) sets state 3 once the last picture was shown, and func_00223C60 then stops the
   sound stream (DHINCOG1.PSS has 3 s of sound after its 5 s of pictures). */
int movie_over(void)
{
    if (!M.active) return 1;
    SDL_LockMutex(M.lock);
    int over = M.ended && clock_picture() >= M.decoded;
    SDL_UnlockMutex(M.lock);
    return over;
}

/* The mixer's rendered chunk (sound.c render, 48 kHz stereo): the movie's sound added to mix,
   and the clock moved on by frames. */
void movie_audio(int32_t *mix, int frames)
{
    if (!M.active) return;
    SDL_LockMutex(M.lock);
    int64_t from = M.clock - M.audio_offset;  /* sound frame due at mix[0] */
    if (M.audio && M.audio_ok && M.ring)
        for (int i = 0; i < frames; i++, from++) {
            if (from < M.ring_read) continue;           /* before the sound starts */
            if (from >= M.ring_write) break;            /* not demuxed yet: silence */
            const int16_t *s = M.ring + (from & (RING - 1)) * 2;
            mix[i * 2] += s[0];
            mix[i * 2 + 1] += s[1];
        }
    M.clock += frames;
    /* Played, or too late to play: the ring's room for the decoder. */
    int64_t done = M.clock - M.audio_offset;
    if (done > M.ring_read) M.ring_read = done < M.ring_write ? done : M.ring_write;
    SDL_BroadcastCondition(M.changed);  /* the decoder may wait for either */
    SDL_UnlockMutex(M.lock);
}

/* The picture to present now (PICTURE_WIDTH x PICTURE_HEIGHT RGBA, the movie centred on black),
   or NULL without a movie; rows: the picture's rows the movie fills, in its middle (all of them
   until its first picture is decoded). After a movie, its last picture until movie_drop (the
   presenter's call). Game thread, once a field. */
const uint32_t *movie_picture(unsigned *width, unsigned *height, unsigned *rows)
{
    if (!M.active && held) {
        *width = PICTURE_WIDTH, *height = PICTURE_HEIGHT, *rows = held_rows;
        return held;
    }
    if (!M.active || !M.picture) return NULL;
    SDL_LockMutex(M.lock);
    int64_t want = clock_picture();
    if (want >= M.decoded) want = M.decoded - 1;        /* not decoded yet: the newest one */
    if (want > M.shown) {
        Slot *slot = &M.slots[want % SLOTS];
        if (slot->index == want) {
            int w = M.width, h = M.height, x0 = (PICTURE_WIDTH - w) / 2, y0 = (PICTURE_HEIGHT - h) / 2;
            for (int y = 0; y < h; y++)
                memcpy(M.picture + (y0 + y) * PICTURE_WIDTH + x0, slot->rgba + y * w, (size_t)w * 4);
            M.shown = want;
            SDL_BroadcastCondition(M.changed);
        }
    }
    SDL_UnlockMutex(M.lock);
    *width = PICTURE_WIDTH;
    *height = PICTURE_HEIGHT;
    *rows = M.height > 0 ? (unsigned)M.height : PICTURE_HEIGHT;
    return M.picture;
}

#ifdef MOVIE_TEST
/* test_movie.c: the picture due now is decoded and a field of sound is there (or the file ended). */
int movie_test_ready(void)
{
    SDL_LockMutex(M.lock);
    int sound = !M.audio || !M.audio_ok || M.clock - M.audio_offset + 801 <= M.ring_write;
    int ready = M.ended || (clock_picture() < M.decoded && sound);
    SDL_UnlockMutex(M.lock);
    return ready;
}
#else
/* ---- the patched player (port/patches.py, 00223BB0.c) ---- */

/* func_00223BB0: name is the game's path ("mov/attract.pss"), audio its second argument (the
   startup logos pass 0: played silent, as on the PS2). Always "started" for the game: a movie
   that can't play ends at the host_movie_update after next. */
static int started;  /* host_movie_update calls since the start */

void host_service_movie_prepare(void) { movie_close(); started=0; }

void host_service_movie_start(uint32_t address,uint32_t lsn,uint32_t size,int audio,int found)
{
    const char *name=GUEST(address), *iso=getenv("DHD_ISO");
    if (!iso || !found) { hle_log("movie: %s isn't on the disc, skipped\n",name); return; }
    if (movie_open(iso,lsn,size,audio,name))
        hle_log("movie: %s (%u bytes, sound %s)\n",name,size,audio ? "on" : "off");
}

/* func_00223C60, once a game frame: keep is 0 when the player skips. Nonzero: the movie is over
   (closed here). Never over in the frame it started: its callers (func_001EBFD8's D_0029D920,
   func_001B53C0's D_0029D408) note that a movie runs at the top of the next frame, from
   D_0029DAFC, and only then wait for D_0029DB00; ended at once, they would wait forever. */
int host_service_movie_update(int keep)
{
    if (!started++) return 0;
    if (keep && movie_playing() && !movie_over()) return 0;
    if (movie_playing()) hle_log("movie: %s %s\n", M.name, keep ? "ended" : "skipped");
    if (movie_playing() && M.shown >= 0) {  /* its last picture stays on screen (movie_picture) */
        movie_drop();
        held = M.picture, held_rows = (unsigned)M.height;
        M.picture = NULL;
    }
    movie_close();
    return 1;
}
#endif

/* Guest-address-only module boundary; native device code stays above. */
#include "../host/host_api.h"
#include "../include/guest.h"
int host_service_movie_playing(void) { return movie_playing(); }
void host_service_movie_audio(uint32_t a0, int a1) { movie_audio((int32_t *)GUEST(a0), a1); }
