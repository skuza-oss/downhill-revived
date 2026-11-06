/* Optional second FIFO: VIF/VU owns the producer, GS/D3D owns the consumer.
   The EE may produce only after graphics_wait has retired both queues. */
#include "gs_queue.h"
GraphicsProvenance graphics_provenance;
#ifdef GS_QUEUE_TEST
enum { GS_RING_BYTES = 4096 };
#else
enum { GS_RING_BYTES = 8 * 1024 * 1024 };
#endif
typedef struct {
    uint32_t bytes, path, words, pad;
    GraphicsProvenance provenance;
} GsPacket;
static uint8_t *gs_ring;
static SDL_Thread *gs_worker;
static SDL_Semaphore *gs_available, *gs_retired;
QUEUE_COUNTER(gs_written);
QUEUE_COUNTER(gs_consumed);
QUEUE_COUNTER(gs_stopping);
QUEUE_COUNTER(gs_sleeping);
QUEUE_COUNTER(gs_waiting);
static uint32_t gs_position(_Atomic uint32_t *p)
{
    return position(p);
}
/* One VIF packet can kick many GIF packets. Keep their individual headers,
   bytes and provenance, but publish the completed prefix together. Only the
   VU producer owns these two fields; consumers still use gs_written. */
static uint32_t gs_pending;
static uint32_t gs_cached_consumed;
static int gs_batching;
static void gs_publish(void)
{
    if (gs_pending == gs_position(&gs_written)) return;
    atomic_exchange(&gs_written, gs_pending);
    if (gs_position(&gs_sleeping)) wake(gs_available);
}
static void gs_batch_begin(void)
{
    gs_pending = gs_position(&gs_written);
    gs_batching = 1;
}
static void gs_batch_end(void)
{
    gs_publish();
    gs_batching = 0;
}
static void gs_execute(unsigned path, const uint32_t *words, unsigned count, const GraphicsProvenance *p)
{
    gs.vu_entry = p->entry;
    gs.vu_top = p->top;
    gs.vu_itop = p->itop;
    gs.vu_pc = p->pc;
    gs.vu_clip = p->clip;
    gs.vu_qword = p->qword;
    gs.vu_overlay = p->overlay;
    gs.vu_program = p->program;
    gs_vu_shadow = p->shadow;
    gs.trace_seconds = p->seconds;
    if (path == GS_PATH_MESH) gs_mesh(&gs, words, count);
    else gs_feed(&gs, path, words, count);
}
static FILE *gs_profile_file;
static int gs_profile_initialized;
static double gs_profile_start, gs_profile_after, gs_profile_until, gs_profile_busy;
static unsigned gs_profile_frame;
static uint64_t gs_profile_cpu;
static double gs_profile_begin(void)
{
    if (!gs_profile_initialized) {
        gs_profile_initialized = 1;
        const char *path = getenv("DHD_GS_WORKER_PROFILE");
        if (path) {
            gs_profile_file = fopen(path, "w");
            if (!gs_profile_file) {
                perror(path);
                abort();
            }
            const char *a = getenv("DHD_VU1_PROFILE_AFTER"), *b = getenv("DHD_VU1_PROFILE_UNTIL");
            gs_profile_after = a ? atof(a) : 90;
            gs_profile_until = b ? atof(b) : 150;
        }
    }
    if (!gs_profile_file)
        return 0;
    double now = host_wall_seconds();
    if (now < gs_profile_after)
        return 0;
    if (!gs_profile_start) {
        gs_profile_start = now;
        gs_profile_frame = gs_presented_frames;
        gs_profile_cpu = worker_cpu_ticks();
    }
    if (now >= gs_profile_until) {
        fprintf(gs_profile_file, "wall_s,frames,busy_s,cpu_s\n%.6f,%u,%.6f,%.6f\n", now - gs_profile_start,
                gs_presented_frames - gs_profile_frame, gs_profile_busy,
                (worker_cpu_ticks() - gs_profile_cpu) * 1e-7);
        fclose(gs_profile_file);
        gs_profile_file = NULL;
        return 0;
    }
    return now;
}
static int SDLCALL gs_consume(void *unused)
{
    (void)unused;
    host_thread_name("gs");
    tl_role = 2;
    unsigned idle_budget = 1024;
    fp_set_control(FP_IEEE(fp_control()));  /* IEEE (graphics_queue.c's macros: MXCSR, or FPCR on arm64) */
    for (;;) {
        uint32_t at = gs_position(&gs_consumed);
        if (at == gs_position(&gs_written)) {
            if (gs_position(&gs_stopping))
                return 0;
            int64_t idle = tl_begin();
            idle_wait(&gs_written, at, &gs_sleeping, gs_available, -1, &idle_budget);
            tl_add(TL_GS_IDLE, idle);
            continue;
        }
        const GsPacket *p = (const GsPacket *)(gs_ring + (at & (GS_RING_BYTES - 1)));
        if (p->path == GS_PATH_MARK)
            run_mark((const uint32_t *)(p + 1));
        else if (p->path != UINT32_MAX) {
            double start = gs_profile_begin();
            gs_execute(p->path, (const uint32_t *)(p + 1), p->words, &p->provenance);
            if (start)
                gs_profile_busy += host_wall_seconds() - start;
        }
        atomic_exchange(&gs_consumed, at + p->bytes);
        if (gs_position(&gs_waiting))
            wake(gs_retired);
    }
}
void graphics_gs_wait(void)
{
    if (!gs_worker)
        return;
    uint32_t target = gs_position(&gs_written);
    uint32_t observed;
    while ((int32_t)((observed = gs_position(&gs_consumed)) - target) < 0)
        idle_wait(&gs_consumed, observed, &gs_waiting, gs_retired, 1, NULL);
}
static void gs_queue_shutdown(void)
{
    if (!gs_worker)
        return;
    graphics_gs_wait();
    atomic_exchange(&gs_stopping, 1);
    SDL_SignalSemaphore(gs_available);
    SDL_WaitThread(gs_worker, NULL);
    SDL_DestroySemaphore(gs_available);
    SDL_DestroySemaphore(gs_retired);
    free(gs_ring);
    gs_ring = NULL;
    gs_worker = NULL;
    gs_available = gs_retired = NULL;
}
static void gs_queue_initialize(void)
{
    const char *option = getenv("DHD_GS_WORKER");
    if (option && !atoi(option))
        return;
    /* Suspect capture reads GS counters synchronously inside the VU callback. */
    if (getenv("DHD_VU_SUSPECT"))
        return;
#ifdef GS_QUEUE_TEST
    gs_written = gs_consumed = 0xfffff800u;
#endif
    gs_pending = gs_position(&gs_written);
    gs_cached_consumed = gs_position(&gs_consumed);
    gs_ring = malloc(GS_RING_BYTES);
    gs_available = SDL_CreateSemaphore(0);
    gs_retired = SDL_CreateSemaphore(0);
    if (gs_ring && gs_available && gs_retired)
        gs_worker = SDL_CreateThread(gs_consume, "gs", NULL);
    if (!gs_worker) {
        fprintf(stderr, "GS worker initialization failed\n");
        abort();
    }
    hle_log("graphics: separate GS/D3D worker enabled\n");
}
void graphics_gif_feed(unsigned path, const uint32_t *words, unsigned count)
{
    if (!gs_worker) {
        gs_execute(path, words, count, &graphics_provenance);
        return;
    }
    while (count) {
        unsigned take = count > GS_RING_BYTES / 16 ? GS_RING_BYTES / 16 : count;
        unsigned bytes = (sizeof(GsPacket) + take * 4 + 15) & ~15u;
        uint32_t at = gs_batching ? gs_pending : gs_position(&gs_written);
        unsigned tail = GS_RING_BYTES - (at & (GS_RING_BYTES - 1)), wrap = bytes > tail ? tail : 0;
        /* A previously observed retired position remains a safe lower bound.
           Refresh it only when that bound no longer leaves enough room. */
        if (GS_RING_BYTES - (at - gs_cached_consumed) < bytes + wrap)
            gs_cached_consumed = gs_position(&gs_consumed);
        if (GS_RING_BYTES - (at - gs_cached_consumed) < bytes + wrap) {
            /* The consumer needs the preceding batch to free this space. */
            if (gs_batching) gs_publish();
            int64_t full = tl_begin();
            uint32_t observed;
            while (GS_RING_BYTES - (at - (observed = gs_position(&gs_consumed))) < bytes + wrap)
                idle_wait(&gs_consumed, observed, &gs_waiting, gs_retired, 1, NULL);
            gs_cached_consumed = observed;
            tl_add(TL_VU_FULL, full);
        }
        if (wrap) {
            GsPacket *p = (GsPacket *)(gs_ring + (at & (GS_RING_BYTES - 1)));
            p->bytes = wrap;
            p->path = UINT32_MAX;
            at += wrap;
        }
        GsPacket *p = (GsPacket *)(gs_ring + (at & (GS_RING_BYTES - 1)));
        p->bytes = bytes;
        p->path = path;
        p->words = take;
        p->pad = 0;
        p->provenance = graphics_provenance;
        memcpy(p + 1, words, take * 4);
        gs_pending = at + bytes;
        if (!gs_batching || gs_pending - gs_position(&gs_written) >= 65536)
            gs_publish();
        words += take;
        count -= take;
    }
}
int graphics_xgkick(uint32_t guest_memory, unsigned qword)
{
    const uint32_t *memory = GUEST(guest_memory);
    if (!gs_worker) {
        const GraphicsProvenance *p = &graphics_provenance;
        gs.vu_entry = p->entry;
        gs.vu_top = p->top;
        gs.vu_itop = p->itop;
        gs.vu_pc = p->pc;
        gs.vu_clip = p->clip;
        gs.vu_qword = p->qword;
        gs.vu_overlay = p->overlay;
        gs.vu_program = p->program;
        gs_vu_shadow = p->shadow;
        gs.trace_seconds = p->seconds;
        return gs_xgkick(&gs, memory, qword);
    }
    /* Every completed XGKICK ends at EOP. Determine the span without touching
       the concurrently owned GS parser, then copy before VU memory can change. */
    unsigned total = 0;
    for (;;) {
        const uint32_t *tag = memory + ((qword + total) & 1023) * 4;
        unsigned loops = tag[0] & 32767, mode = (tag[1] >> 26) & 3, regs = tag[1] >> 28;
        if (!regs)
            regs = 16;
        unsigned payload = mode == 0 ? loops * regs : mode == 1 ? (loops * regs + 1) / 2 : loops;
        if (payload >= 0x4000 - total)
            return -1;
        total += 1 + payload;
        if (tag[0] & 0x8000)
            break;
        if (total == 0x4000)
            return -1;
    }
    for (unsigned done = 0; done < total;) {
        unsigned start = (qword + done) & 1023, take = 1024 - start;
        if (take > total - done)
            take = total - done;
        graphics_gif_feed(1, memory + start * 4, take * 4);
        done += take;
    }
    return 0;
}
