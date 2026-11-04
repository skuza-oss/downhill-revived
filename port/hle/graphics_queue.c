/* Copied VIF1/GIF packets run in FIFO order on one optional graphics worker.
   Atomic publication owns the ring bytes until the consumer retires them.
   Design reference: PCSX2 MTVU's copied command ring; no source copied. */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL_mutex.h>   /* SDL_Semaphore */
#include <SDL3/SDL_thread.h>
#if defined(__aarch64__)
/* The Android build (finding 091): FPCR for MXCSR; isb for pause: yield is a no-op on the Cortex
   cores, so the 4096-step spins before a sleep lasted ~2 us instead of x86's ~100 us and the
   workers slept and were woken (a futex call each side) far more often (AND2). */
static inline unsigned fp_control(void) { unsigned long v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return (unsigned)v; }
/* skipped when unchanged (the usual case on the workers): the write and its isb serialize the core */
static inline void fp_set_control(unsigned v) { if (v != fp_control()) __asm__ volatile("msr fpcr, %0\n\tisb" :: "r"((unsigned long)v) : "memory"); }
/* IEEE: nearest (RMode 0), no flush to zero (FZ, FZ16), no default NaN, no AH/FIZ/NEP, no traps */
#define FP_IEEE(c) ((c) & ~((3u << 22) | (1u << 24) | (1u << 25) | (1u << 19) | 0x9F07u))
#define cpu_pause() __asm__ volatile("isb" ::: "memory")
#else
#include <xmmintrin.h>
#define fp_control() _mm_getcsr()
#define fp_set_control(v) _mm_setcsr(v)
#define FP_IEEE(c) (((c) | 0x1F80u) & ~(63u | 0xE040u))  /* IEEE: nearest, no FTZ/DAZ */
#define cpu_pause() _mm_pause()
#endif
#include "hle.h"
#include "../gs/gs.h"
#include "graphics_profile.h"

#ifdef GRAPHICS_QUEUE_TEST
enum { RING_BYTES = 4096 };
#else
enum { RING_BYTES = 8 * 1024 * 1024 };
#endif

typedef struct { uint32_t bytes, unit, words, padding; } Packet;
static uint8_t *ring;
static SDL_Thread *worker;
static SDL_Semaphore *available, *retired;
/* Producer/consumer writes must not invalidate each other's publication line.
   Keep announcements separate too: both threads poll them for every packet. */
#define QUEUE_COUNTER(name) static _Atomic uint32_t name __attribute__((aligned(64)))
QUEUE_COUNTER(written);
QUEUE_COUNTER(consumed);
QUEUE_COUNTER(stopping);
/* Each side signals only when the other sleeps: an event per packet (thousands a
   frame) cost a kernel call and a thread wake each. */
QUEUE_COUNTER(sleeping);
QUEUE_COUNTER(waiting);
static int initialized;
static int adaptive_waits;
static int spike_waits;
static double spin_seconds = -1;  /* DHD_SPIN_US, read at the first long wait (idle_wait) */
static _Thread_local double spin_end, spin_returned;  /* this thread's wait's spin, and its last return */

/* Reads only acquire ownership published by the other side. A locked RMW here
   unnecessarily takes exclusive ownership of its cache line on every poll.
   Keep the exchanging publications and sleep announcements below: their full
   barriers also order each publication before the subsequent waiter check. */
static uint32_t position(_Atomic uint32_t *p) { return atomic_load_explicit(p, memory_order_acquire); }

/* An auto-reset event: wakes the sleeper, or makes its next wait return at once. Set only while
   it is clear, so a burst of packets leaves one wakeup, not one per packet. */
static void wake(SDL_Semaphore *event)
{
    if (!SDL_GetSemaphoreValue(event)) SDL_SignalSemaphore(event);
}

#include "graphics_timeline.h"

/* Spin briefly before sleeping until *p moves away from at. *flag announces the sleep;
   the atomic increment and reread pair with the other side's publish-then-read. timeout: ms,
   -1 for none. */
static void idle_wait(_Atomic uint32_t *p, uint32_t at, _Atomic uint32_t *flag, SDL_Semaphore *event, Sint32 timeout,
                      unsigned *budget)
{
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
    int64_t spin_start = tl_begin();
#endif
    unsigned limit = budget ? (adaptive_waits ? *budget : 4096) : 512;
    for (unsigned spin = 0; spin < limit; spin++) {
        if (position(p) != at) {
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
            if (!budget && spin_seconds > 0) spin_returned = host_wall_seconds();
#endif
            if (budget && adaptive_waits) {
                unsigned next = spin * 2 + 128;
                *budget = next < 4096 ? next : 4096;
            }
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
            tl_add(TL_EE_SPIN + 2 * tl_role, spin_start);
#endif
            return;
        }
        cpu_pause();
    }
    /* DHD_SPIN_US (finding 091 AND2): the producer's waits (retired/full, the game thread's) spin
       that long before sleeping, so a phone's governor sees a busy core and keeps its clock up.
       Android's default 3000 (+4% race fields/s on the Mi 11i, p99 down); elsewhere 0.
       That long once per wait (finding 146): the callers wait in a loop, and the other side wakes
       them at every packet it retires, so a call that comes straight after one that returned
       (within 50 us) goes on with that wait's spin instead of starting another 3 ms. */
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
    if (!budget && spin_seconds) {
        if (spin_seconds < 0) {
            const char *us = getenv("DHD_SPIN_US");
#ifdef __ANDROID__
            spin_seconds = (us ? atof(us) : 3000) * 1e-6;
#else
            spin_seconds = us ? atof(us) * 1e-6 : 0;
#endif
        }
        double now = host_wall_seconds();
        if (spin_seconds > 0 && now - spin_returned > 50e-6) spin_end = now + spin_seconds;
        while (spin_seconds > 0 && now < spin_end) {
            for (unsigned spin = 0; spin < 256; spin++) {
                if (position(p) != at) {
                    spin_returned = host_wall_seconds();
                    tl_add(TL_EE_SPIN + 2 * tl_role, spin_start);
                    return;
                }
                cpu_pause();
            }
            now = host_wall_seconds();
        }
    }
#endif
    /* Only empty consumer waits adapt. Retired/full waits spin at most 512 pauses
       before parking. Their timeout and lost-wakeup handshake are unchanged. */
    if (budget && adaptive_waits) *budget = limit > 256 ? limit / 2 : 128;
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
    tl_add(TL_EE_SPIN + 2 * tl_role, spin_start);
#endif
    atomic_fetch_add(flag, 1);
    if (position(p) == at) {
#ifndef GRAPHICS_QUEUE_TEST
        double start = spike_waits ? host_wall_seconds() : 0;
#endif
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
        int64_t park_start = tl_begin();
#endif
        bool woken = SDL_WaitSemaphoreTimeout(event, timeout);
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
        tl_add(TL_EE_PARK + 2 * tl_role, park_start);
#endif
#ifndef GRAPHICS_QUEUE_TEST
        if (spike_waits && !woken)
            fprintf(stderr, "spike_queue: start=%.9f ms=%.6f moved=%u\n", start,
                    (host_wall_seconds()-start)*1e3, position(p) != at);
#endif
    }
    atomic_fetch_sub(flag, 1);
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
    if (!budget && spin_seconds > 0) spin_returned = host_wall_seconds();
#endif
}

/* Retired-side waits share one auto-reset event; the short timeout covers two waiters. */
/* Wait on the same observation that failed the caller's predicate. A fresh load here
   can already equal its target: no packet follows it until the EE starts the next field. */
static void wait_retired(uint32_t at) { idle_wait(&consumed, at, &waiting, retired, 1, NULL); }

#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
/* Markers (finding 090 HO1): a VBLANK or a picture queued in order with the packets, run by the
   thread that owns the GS once everything sent before it has run there. Unit 3 in this ring, then
   GS_PATH_MARK in the GS ring. Words: sequence number, kind, payload. */
enum { UNIT_MARK = 3, MARK_VBLANK = 0, MARK_PRESENT = 1, MARK_STORE = 2, MARK_WORDS = 64 };
static uint32_t marks_sent, vblank_mark, present_mark;  /* EE */
static uint32_t csr_mark;                               /* EE: the last marker that changes the CSR */
QUEUE_COUNTER(marks_done);                              /* the GS owner: the last marker run */
/* CSR after the last VBLANK or store marker run; read after csr_mark */
static volatile uint64_t marked_csr;
void (*graphics_present_marked)(const void *payload);  /* present.c */

/* Queued store-image reads (host_service_gs_store_image, finding 148). The EE fills a slot and
   queues its marker; the GS owner reads the GS into the slot when the marker runs (in order with
   the drawing, where the EE used to wait for both workers to go idle); the EE copies the bytes to
   guest RAM at the read's first access (host_service_gs_store_settle). */
enum { STORE_SLOTS = 16, STORE_BYTES = 16384 };
static struct { uint32_t destination, bytes, mark; int ok; uint8_t data[STORE_BYTES]; } stores[STORE_SLOTS];
static unsigned stores_sent, stores_settled;  /* EE */

static void run_store(unsigned slot)
{
    /* What sceGsExecStoreImage does after its waits: defer the read (staged in order with the
       draws) and settle it at once into the slot, or read it now; then consume FINISH. */
    uint8_t *data = stores[slot].data;
    uint32_t bytes = stores[slot].bytes;
    size_t got = gs_defer_read_image(&gs, data, bytes);
    if (got == bytes) gs_settle_reads(&gs);
    else {
        if (gs_reads_pending(&gs)) gs_settle_reads(&gs);
        got = gs_read_image(&gs, data, bytes);
    }
    stores[slot].ok = got == bytes;
    gs_priv_write(&gs, GS_CSR, 2);
}

static void run_mark(const uint32_t *words)
{
    if (words[1] == MARK_VBLANK) {
        gs_vblank(&gs);
        marked_csr = gs_priv_read(&gs, GS_CSR);
    } else if (words[1] == MARK_STORE) {
        run_store(words[2]);
        marked_csr = gs_priv_read(&gs, GS_CSR);
    } else if (graphics_present_marked) graphics_present_marked(words + 2);
    atomic_exchange(&marks_done, words[0]);
}
#endif

#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
#include "gs_queue.inc"
#endif

static void execute(unsigned unit, const uint32_t *words, unsigned count)
{
    /* VIF1/GIF execute VU1 and host rendering, not EE game code. Host IEEE
       exceptional values must not inherit the game's diagnostic SSE mask. */
    unsigned control = fp_control();
#if defined(__aarch64__) && (!defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST))
    /* VIF decoding is integer work and its GS consumer owns a separate IEEE
       environment. Keep VU rounding for this whole VIF1 packet, so each MSCAL
       need not serialize the core twice to change/restore FPCR. ps2_vu1_run
       retains its own guard and observes the same mode as before. */
    if (unit == 1 && gs_worker)
        fp_set_control((3u << 22) | (1u << 24)); /* toward zero, FZ, no traps */
    else
#endif
    fp_set_control(FP_IEEE(control));  /* IEEE: nearest, no FTZ/DAZ */
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
    if (gs_worker) gs_batch_begin();
    if (unit == UNIT_MARK) {
        if (gs_worker) graphics_gif_feed(GS_PATH_MARK, words, count);
        else run_mark(words);
    } else
#endif
    if (unit == 2) {
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
        double start = worker_profile_start && worker_profile ? host_wall_seconds() : 0;
        graphics_gif_feed(3, words, count);
        if (start) vif_gs_seconds += host_wall_seconds()-start;
#else
        gs_feed(&gs, 3, words, count);
#endif
    }
    else vif_feed((int)unit, words, count);
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
    if (gs_worker) gs_batch_end();
#endif
    fp_set_control(control);
}

static int SDLCALL consume(void *unused)
{
    (void)unused;
    host_thread_name("vu");
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
    tl_role = 1;
#endif
    unsigned idle_budget = 1024;
    for (;;) {
        uint32_t at = position(&consumed);
        if (at == position(&written)) {
            if (position(&stopping)) return 0;
            int64_t idle = tl_begin();
            idle_wait(&written, at, &sleeping, available, -1, &idle_budget);
            tl_add(TL_VU_IDLE, idle);
            continue;
        }
        const Packet *packet = (const Packet *)(ring + (at & (RING_BYTES - 1)));
        if (packet->unit != UINT32_MAX) {
            double start = worker_begin();
            execute(packet->unit, (const uint32_t *)(packet + 1), packet->words);
            worker_end(start);
        }
        atomic_exchange(&consumed, at + packet->bytes);
        if (position(&waiting)) wake(retired);
    }
}

void graphics_wait(void)
{
    if (!worker) return;
    uint32_t target = position(&written);
    int64_t start = tl_begin();
    int pending = (int32_t)(position(&consumed) - target) < 0;
    uint32_t observed;
    while ((int32_t)((observed = position(&consumed)) - target) < 0)
        wait_retired(observed);
    tl_add(TL_EE_VU, start);
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
    int64_t gs_start = tl_begin();
    pending |= gs_position(&gs_consumed) != gs_position(&gs_written);
    graphics_gs_wait();
    tl_add(TL_EE_GS, gs_start);
#endif
    if (start && timeline_enabled()) tl_site(__builtin_return_address(0), pending, tl_now() - start);
}

void graphics_shutdown(void)
{
    if (!worker) return;
    graphics_wait();
    atomic_exchange(&stopping, 1);
    SDL_SignalSemaphore(available);
    SDL_WaitThread(worker, NULL);
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
    gs_queue_shutdown();
#endif
    SDL_DestroySemaphore(available); SDL_DestroySemaphore(retired);
    free(ring);
    worker = NULL; available = retired = NULL; ring = NULL;
}

/* The default since round 22: race frames and dumps are identical to inline execution
   (finding 030). DHD_MTVU=0 keeps everything on the EE thread. */
int graphics_threaded(void)
{
    const char *option = getenv("DHD_MTVU");
    return !option || strcmp(option, "0");
}

static void initialize(void)
{
    initialized = 1;
    spike_waits = getenv("DHD_SPIKES") != NULL;
    /* DHD_FIFO_ADAPTIVE=0 keeps the fixed 4096-spin budget for the consumers' empty waits (finding 069). */
    const char *adaptive = getenv("DHD_FIFO_ADAPTIVE");
    adaptive_waits = !adaptive || strcmp(adaptive, "0");
    if (!graphics_threaded()) return;
#ifdef GRAPHICS_QUEUE_TEST
    /* Cross the 32-bit publication counter rollover during the stress test. */
    written = consumed = 0xFFFFF800u;
#endif
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
    gs_queue_initialize();
#endif
    ring = malloc(RING_BYTES);
    available = SDL_CreateSemaphore(0);
    retired = SDL_CreateSemaphore(0);
    if (ring && available && retired) worker = SDL_CreateThread(consume, "vu", NULL);
    if (!worker) {
        SDL_DestroySemaphore(available);
        SDL_DestroySemaphore(retired);
        free(ring); ring = NULL; available = retired = NULL;
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
        gs_queue_shutdown();
#endif
        hle_log("graphics: worker unavailable, using inline execution\n");
    } else {
        atexit(graphics_shutdown);
        hle_log("graphics: VIF1/GIF worker enabled\n");
    }
}

static void feed_words(int unit, const uint32_t *words, uint32_t count);

void graphics_feed(int unit, uint32_t guest_words, uint32_t count)
{
    const uint32_t *words = GUEST(guest_words);
    if (unit == 0) { vif_feed(unit, words, count); return; }
    feed_words(unit, words, count);
}

static void feed_words(int unit, const uint32_t *words, uint32_t count)
{
    if (!initialized) initialize();
    if (!worker) { execute((unsigned)unit, words, count); return; }
    while (count) {
        unsigned take = count > RING_BYTES / 16 ? RING_BYTES / 16 : count;
        unsigned bytes = (sizeof(Packet) + take * 4 + 15) & ~15u;
        uint32_t at = position(&written);
        unsigned tail = RING_BYTES - (at & (RING_BYTES - 1));
        unsigned wrap = bytes > tail ? tail : 0;
        if (RING_BYTES - (at - position(&consumed)) < bytes + wrap) {
            int64_t full = tl_begin();
            uint32_t observed;
            while (RING_BYTES - (at - (observed = position(&consumed))) < bytes + wrap)
                wait_retired(observed);
            tl_add(TL_EE_FULL, full);
        }
        if (wrap) {
            Packet *padding = (Packet *)(ring + (at & (RING_BYTES - 1)));
            *padding = (Packet){wrap, UINT32_MAX, 0, 0};
            at += wrap;
        }
        Packet *packet = (Packet *)(ring + (at & (RING_BYTES - 1)));
        *packet = (Packet){bytes, (unsigned)unit, take, 0};
        memcpy(packet + 1, words, take * 4);
        atomic_exchange(&written, at + bytes);
        if (position(&sleeping)) wake(available);
        words += take; count -= take;
    }
}

#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
/* Deferred vsync (finding 090 HO1, after finding 048's f80b4d61): with the workers, the EE does not
   wait for the drawing at vsync. The VBLANK and the picture go into the queues as markers and the
   EE goes on with the next field while VU1 and the GS finish this one; at most two fields are in
   flight (graphics_vblank). The game's CSR page stays exact where it can be read (kernel.c,
   host_service_graphics_wait). Off with DHD_SYNC_STRICT=1 and wherever the EE reads the GS at
   vsync: screenshots, GS captures, DHD_PROFILE. */
int graphics_deferred(void)
{
    static int deferred = -1;
    if (deferred < 0) {
        const char *strict = getenv("DHD_SYNC_STRICT");
        deferred = graphics_threaded() && !(strict && strcmp(strict, "0")) && !getenv("DHD_SCREENSHOT") &&
                   !getenv("DHD_GS_CAPTURE") && !getenv("DHD_RACE_GSDUMP") && !getenv("DHD_PROFILE") &&
                   !getenv("DHD_VU_SUSPECT");
    }
    return deferred;
}

/* Queues a marker behind everything sent so far (payload: whole words, copied). */
static uint32_t graphics_mark(unsigned kind, const void *payload, unsigned bytes)
{
    uint32_t words[2 + MARK_WORDS];
    if (bytes > MARK_WORDS * 4) abort();
    words[0] = ++marks_sent;
    words[1] = kind;
    if (bytes) memcpy(words + 2, payload, bytes);
    feed_words(UNIT_MARK, words, 2 + (bytes + 3) / 4);
    return marks_sent;
}

static void wait_mark(uint32_t sequence)
{
    uint32_t done;
    while ((int32_t)((done = position(&marks_done)) - sequence) < 0) {
        if (gs_worker) idle_wait(&marks_done, done, &gs_waiting, gs_retired, 1, NULL);
        else idle_wait(&marks_done, done, &waiting, retired, 1, NULL);
    }
}

/* The EE's side of a deferred VBLANK (kernel.c): wait until the picture of the field before last
   has been handed on (two fields in flight: the GS works on one while VU1 starts the next), or with
   DHD_FIELDS_IN_FLIGHT=1 the last one, then queue this VBLANK. */
void graphics_vblank(void)
{
    static int in_flight;
    static uint32_t before_last;
    if (!in_flight) {
        const char *option = getenv("DHD_FIELDS_IN_FLIGHT");
        in_flight = option && atoi(option) == 1 ? 1 : 2;
    }
    /* the last picture, or with none since the last VBLANK (no window: DHD_HEADLESS) that VBLANK */
    uint32_t last = (int32_t)(present_mark - vblank_mark) < 0 ? vblank_mark : present_mark;
    uint32_t target = in_flight == 1 ? last : before_last;
    before_last = last;
    int64_t start = tl_begin();
    int pending = (int32_t)(position(&marks_done) - target) < 0;
    wait_mark(target);
    tl_add(TL_EE_GS, start);
    if (start && timeline_enabled()) tl_site(__builtin_return_address(0), pending, tl_now() - start);
    csr_mark = vblank_mark = graphics_mark(MARK_VBLANK, NULL, 0);
}

/* present.c's gs_vsync: the picture, run by graphics_present_marked in order with the drawing. */
void graphics_present(const void *payload, unsigned bytes)
{
    present_mark = graphics_mark(MARK_PRESENT, payload, bytes);
}
#endif

/* Guest-address-only module boundary; native device code stays above. */
#include "../host/host_api.h"
#include "../include/guest.h"
void host_service_graphics_feed(int a0, uint32_t a1, uint32_t a2) { graphics_feed(a0, a1, a2); }
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
/* The game's CSR page (0x12001000) after a deferred VBLANK or a queued store-image read: exact
   but for SIGNAL and FINISH until the game's next wait, which puts the CSR the last of them left
   there (kernel.c). */
static int csr_provisional;

void host_service_graphics_wait(void)
{
    graphics_wait();
    if (!csr_provisional) return;
    csr_provisional = 0;
    wait_mark(csr_mark);
    *(volatile uint64_t *)GUEST(0x12001000) = marked_csr;
}

/* sceGsExecStoreImage (graphics.c, finding 148) queued: its store packet (28 words at guest
   `packet`), the read of `bytes` to guest `destination` and the VIF1 word that unmasks PATH3, in
   order behind the drawing. 0 when it cannot be (no workers or deferred VBLANKs, too big, every
   slot waiting): nothing is queued, and the caller waits and reads as before. */
uint32_t host_service_gs_store_image(uint32_t packet, uint32_t destination, uint32_t bytes)
{
    if (!worker || !graphics_deferred() || !bytes || bytes > STORE_BYTES || stores_sent - stores_settled == STORE_SLOTS)
        return 0;
    unsigned slot = stores_sent % STORE_SLOTS;
    stores[slot].destination = destination;
    stores[slot].bytes = bytes;
    stores[slot].ok = 0;
    uint32_t payload[1] = {slot};
    feed_words(1, GUEST(packet), 28);
    csr_mark = stores[slot].mark = graphics_mark(MARK_STORE, payload, sizeof payload);
    stores_sent++;
    static const uint32_t unmask = 0x06000000;  /* MSKPATH3 0 */
    feed_words(1, &unmask, 1);
    /* The marker consumes the packet's FINISH (a CSR FINISH write); the rest as the page has it. */
    *(volatile uint64_t *)GUEST(0x12001000) &= ~(uint64_t)2;
    csr_provisional = 1;
    return 1;
}

/* The queued reads' bytes to guest RAM, once the last one has run (its pages are writable again). */
void host_service_gs_store_settle(void)
{
    if (stores_sent == stores_settled) return;
    int64_t start = tl_begin();
    uint32_t last = stores[(stores_sent - 1) % STORE_SLOTS].mark;
    int pending = (int32_t)(position(&marks_done) - last) < 0;
    wait_mark(last);
    tl_add(TL_EE_GS, start);
    if (start && timeline_enabled()) tl_site(__builtin_return_address(0), pending, tl_now() - start);
    for (; stores_settled != stores_sent; stores_settled++) {
        unsigned slot = stores_settled % STORE_SLOTS;
        if (stores[slot].ok) memcpy(GUEST(stores[slot].destination), stores[slot].data, stores[slot].bytes);
        else hle_log("graphics: a queued store-image read of %u bytes failed\n", stores[slot].bytes);
    }
}

/* 2: threaded with deferred vsyncs (graphics_deferred) */
int host_service_graphics_threaded(void) { return graphics_threaded() ? 1 + graphics_deferred() : 0; }

void host_service_graphics_vblank(void)
{
    graphics_vblank();
    volatile uint64_t *csr = (volatile uint64_t *)GUEST(0x12001000);
    *csr = (*csr | 8) ^ 0x2000;  /* gs_vblank's change; SIGNAL and FINISH settle later */
    csr_provisional = 1;
}
#else
void host_service_graphics_wait(void) { graphics_wait(); }
int host_service_graphics_threaded(void) { return graphics_threaded(); }
uint32_t host_service_gs_store_image(uint32_t packet, uint32_t destination, uint32_t bytes)
{ (void)packet; (void)destination; (void)bytes; return 0; }
void host_service_gs_store_settle(void) {}
#endif
void host_service_graphics_shutdown(void) { graphics_shutdown(); }
