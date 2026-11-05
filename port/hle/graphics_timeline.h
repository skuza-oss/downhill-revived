/* DHD_TIMELINE=1 (or a CSV path): where each graphics-pipeline thread spends the fields of the
   DHD_BENCH window. Measures waits (idle/blocked time) with the performance counter, so busy
   time per thread is the field's wall time minus its waits; nothing is timed when it is unset.
   - game (EE): waiting in graphics_wait for the VU worker, then for the GS worker, by call site
     (host return address, see the link map), and for room in the VU ring;
   - vu: idle (no packet queued) and blocked on a full GS ring;
   - gs: idle (no packet queued).
   Each thread also splits idle_wait into spin wall time and event-wait wall time (park).
   These are subsets of the waits above: do not subtract them again from busy.
   CPU milliseconds include spins, handoffs and driver work. Busy is outside queue waits,
   not pure game computation: descheduling and GPU/API blocking remain inside it.
   Windows also samples scheduled CPU cycles for game/vu/gs/native (columns in millions of
   cycles, NOT milliseconds). They help distinguish work from descheduling, but are not
   context-switch traces and cannot by themselves distinguish changes in CPU frequency.
   The render thread ("native"): its CPU ms, its own waits (the next field, pacing) and its time in
   the present (port/gs/perf_log.c); the GPU: frames whose timestamps arrived in the field, their
   GPU ms (the passes' sum) and their spans (first pass's start to last pass's end; on Apple GPUs,
   whose passes overlap, the sum overcounts and the span is the bound to read) from dhdr_perf_read:
   one frame in 8, every frame with DHD_GPU_TIMES (finding 156). Per field the GPU columns are
   lumpy (the readback lands a few frames late): sum them over a window.
   bench_vsync calls graphics_timeline at every field of the window; at its end the medians and
   means go to stderr ("bench timeline:" lines) and, with a path, one CSV row per field. */
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
#include <SDL3/SDL_time.h>   /* the wall clock's date */
#include <SDL3/SDL_timer.h>  /* the performance counter */
enum { TL_EE_VU, TL_EE_GS, TL_EE_FULL, TL_VU_IDLE, TL_VU_FULL, TL_GS_IDLE,
       TL_EE_CYCLES, TL_VU_CYCLES, TL_GS_CYCLES, TL_NATIVE_CYCLES,
       TL_EE_CPU, TL_VU_CPU, TL_GS_CPU,
       TL_EE_SPIN, TL_EE_PARK, TL_VU_SPIN, TL_VU_PARK, TL_GS_SPIN, TL_GS_PARK,
       TL_NATIVE_CPU, TL_NATIVE_WAIT, TL_PRESENT, TL_GPU_FRAMES, TL_GPU_MS, TL_GPU_SPAN, TL_COUNT };
static const char *const tl_names[TL_COUNT] = {"ee_wait_vu", "ee_wait_gs", "ee_ring_full", "vu_idle", "vu_gs_full",
                                              "gs_idle", "ee_mcycles", "vu_mcycles", "gs_mcycles", "native_mcycles", "ee_cpu_ms", "vu_cpu_ms", "gs_cpu_ms",
                                              "ee_spin", "ee_park", "vu_spin", "vu_park", "gs_spin", "gs_park",
                                              "native_cpu_ms", "native_wait", "present_wait", "gpu_frames", "gpu_ms", "gpu_span_ms"};
static int tl_on = -1;
/* One role per host thread: EE=0, VU=1, GS=2. */
static _Thread_local unsigned tl_role;
static _Atomic int64_t tl_ticks[TL_COUNT];
enum { TL_SITES = 32 };
static struct { void *site; unsigned calls, pending; int64_t ticks; } tl_sites[TL_SITES];

static int timeline_enabled(void)
{
    if (tl_on < 0) tl_on = getenv("DHD_TIMELINE") != NULL;
    return tl_on;
}

static int64_t tl_now(void) { return (int64_t)SDL_GetPerformanceCounter(); }

/* The waits themselves are counted by default too (one performance-counter read at each end of
   a wait, nothing in the loops): port/gs/perf_log.c's perf: lines take the busy time of the game,
   vu and gs threads from them (graphics_wait_seconds; finding 129). DHD_PERF_LOG=0 without
   DHD_TIMELINE counts nothing. The per-field rows and the call sites stay DHD_TIMELINE's. */
static int tl_count_on = -1;
static int tl_counting(void)
{
    if (tl_count_on < 0) {
        const char *s = getenv("DHD_PERF_LOG");
        tl_count_on = timeline_enabled() || !s || strcmp(s, "0") != 0;
    }
    return tl_count_on;
}

/* The game's (EE: VU, GS, a full VU ring), vu's (idle, a full GS ring) and gs's (idle) waits so
   far, in seconds. */
void graphics_wait_seconds(double out[3])
{
    double f = (double)SDL_GetPerformanceFrequency();
    int64_t t[TL_COUNT];
    for (unsigned i = 0; i < TL_COUNT; i++) t[i] = atomic_load_explicit(&tl_ticks[i], memory_order_relaxed);
    out[0] = (double)(t[TL_EE_VU] + t[TL_EE_GS] + t[TL_EE_FULL]) / f;
    out[1] = (double)(t[TL_VU_IDLE] + t[TL_VU_FULL]) / f;
    out[2] = (double)t[TL_GS_IDLE] / f;
}

/* Each counter has one writing thread; the atomic add keeps the 64-bit value whole for the EE's
   reads on a 32-bit host. */
static void tl_add(unsigned which, int64_t start)
{
    if (start) atomic_fetch_add_explicit(&tl_ticks[which], tl_now() - start, memory_order_relaxed);
}

static int64_t tl_begin(void) { return tl_counting() ? tl_now() : 0; }

static void tl_site(void *site, int pending, int64_t ticks)
{
    unsigned i = 0;
    while (i < TL_SITES - 1 && tl_sites[i].site && tl_sites[i].site != site) i++;
    tl_sites[i].site = site;
    tl_sites[i].calls++;
    tl_sites[i].pending += pending;
    tl_sites[i].ticks += ticks;
}

static int tl_compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* phase 0: the window's first field; 1: every later field; 2: the last one (report). */
void graphics_timeline(int phase, unsigned field, double wall_seconds)
{
    static int64_t last[TL_COUNT];
    static double *rows;  /* per field: wall then TL_COUNT waits, in ms */
    static unsigned count, capacity, first;
    static double frequency;
    if (!timeline_enabled()) return;
    if (!frequency) frequency = (double)SDL_GetPerformanceFrequency();
    extern unsigned host_thread_cycles(const char **names, uint64_t *cycles, unsigned max);
    int64_t now[TL_COUNT] = {0};
    for (unsigned i = 0; i < TL_COUNT; i++) now[i] = atomic_load_explicit(&tl_ticks[i], memory_order_relaxed);
    const char *names[16]; uint64_t cycles[16];
#ifdef GRAPHICS_QUEUE_TEST
    unsigned threads = 0;
#else
    unsigned threads = host_thread_cycles(names, cycles, 16);
#endif
    const char *wanted[] = {"game", "vu", "gs", "native"};
    for (unsigned i = 0; i < threads; i++)
        for (unsigned j = 0; j < 4; j++)
            if (!strcmp(names[i], wanted[j])) now[TL_EE_CYCLES + j] = cycles[i];
#ifndef GRAPHICS_QUEUE_TEST
    extern unsigned host_thread_cpu(const char **names, double *seconds, unsigned max);
    double seconds[16];
    threads = host_thread_cpu(names, seconds, 16);
    for (unsigned i = 0; i < threads; i++) {
        for (unsigned j = 0; j < 3; j++)
            if (!strcmp(names[i], wanted[j])) now[TL_EE_CPU + j] = (int64_t)(seconds[i] * 1e7);
        if (!strcmp(names[i], "native")) now[TL_NATIVE_CPU] = (int64_t)(seconds[i] * 1e7);
    }
    extern void perf_render_waits(double out[2]);  /* port/gs/perf_log.c */
    double render[2];
    perf_render_waits(render);
    now[TL_NATIVE_WAIT] = (int64_t)(render[0] * 1e9), now[TL_PRESENT] = (int64_t)(render[1] * 1e9);
    extern uint32_t dhdr_perf_read(uint64_t *out, uint32_t count);  /* the Rust renderer */
    uint64_t gpu[4] = {0};
    if (dhdr_perf_read(gpu, 4) >= 4)
        now[TL_GPU_FRAMES] = (int64_t)gpu[1], now[TL_GPU_MS] = (int64_t)gpu[2], now[TL_GPU_SPAN] = (int64_t)gpu[3];
#endif
    if (phase == 0) {
        fprintf(stderr, "bench timeline: clock field=%u host_s=%.9f\n", field, host_wall_seconds());
        memcpy(last, now, sizeof last);
        memset(tl_sites, 0, sizeof tl_sites);
        SDL_Time utc;
        if (SDL_GetCurrentTime(&utc))
            fprintf(stderr, "bench timeline: clock unix_s=%.9f host_s=%.9f\n", utc * 1e-9, host_wall_seconds());
        first = field, count = 0;
        return;
    }
    if (count == capacity) {
        capacity = capacity ? capacity * 2 : 1024;
        rows = realloc(rows, capacity * (TL_COUNT + 1) * sizeof *rows);
        if (!rows) { tl_on = 0; return; }
    }
    double *row = rows + count++ * (TL_COUNT + 1);
    row[0] = wall_seconds * 1e3;
    for (unsigned i = 0; i < TL_COUNT; i++) {
        double unit = (i >= TL_EE_CYCLES && i <= TL_NATIVE_CYCLES) ? 1e-6               /* M cycles */
                    : (i >= TL_EE_CPU && i <= TL_GS_CPU) || i == TL_NATIVE_CPU ? 1e-4  /* 100 ns ticks */
                    : i == TL_GPU_FRAMES ? 1                                           /* frames */
                    : i >= TL_NATIVE_WAIT ? 1e-6                                       /* ns */
                    : 1e3 / frequency;                                                 /* the counter */
        row[i + 1] = (double)(now[i] - last[i]) * unit;
    }
    memcpy(last, now, sizeof last);
    if (phase != 2) return;
    /* Report: mean and median of each column, then the busy share of each thread. */
    double *column = malloc(count * sizeof *column), mean[TL_COUNT + 1], median[TL_COUNT + 1];
    if (!column) return;
    for (unsigned c = 0; c <= TL_COUNT; c++) {
        double sum = 0;
        for (unsigned r = 0; r < count; r++) sum += column[r] = rows[r * (TL_COUNT + 1) + c];
        qsort(column, count, sizeof *column, tl_compare);
        mean[c] = sum / count, median[c] = column[count / 2];
    }
    free(column);
    fprintf(stderr, "bench timeline: fields=%u mean_ms field=%.3f", count, mean[0]);
    for (unsigned i = 0; i < TL_COUNT; i++) fprintf(stderr, " %s=%.3f", tl_names[i], mean[i + 1]);
    fprintf(stderr, " | median_ms field=%.3f", median[0]);
    for (unsigned i = 0; i < TL_COUNT; i++) fprintf(stderr, " %s=%.3f", tl_names[i], median[i + 1]);
    fputc('\n', stderr);
    double field_ms = mean[0];
    double gpu_frames = mean[1 + TL_GPU_FRAMES];
    fprintf(stderr, "bench timeline: busy_ms_per_field game=%.3f vu=%.3f gs=%.3f render=%.3f present_wait=%.3f"
                    " | gpu_ms_per_timed_frame=%.3f span=%.3f\n",
            field_ms - mean[1 + TL_EE_VU] - mean[1 + TL_EE_GS] - mean[1 + TL_EE_FULL],
            field_ms - mean[1 + TL_VU_IDLE] - mean[1 + TL_VU_FULL], field_ms - mean[1 + TL_GS_IDLE],
            field_ms - mean[1 + TL_NATIVE_WAIT] - mean[1 + TL_PRESENT], mean[1 + TL_PRESENT],
            gpu_frames > 0 ? mean[1 + TL_GPU_MS] / gpu_frames : 0.0,
            gpu_frames > 0 ? mean[1 + TL_GPU_SPAN] / gpu_frames : 0.0);
    /* exe+offset: the site's place in the link map (dhd64.map), whatever the load address */
#ifdef _WIN32
    extern char __ImageBase[];  /* the linker's: the executable's load address */
    uintptr_t image = (uintptr_t)__ImageBase;
#else
    uintptr_t image = 0;
#endif
    for (unsigned i = 0; i < TL_SITES && tl_sites[i].site; i++)
        fprintf(stderr, "bench timeline: ee_wait site=%p exe+0x%llx calls=%.2f pending=%.2f wait_ms=%.3f (per field)\n",
                tl_sites[i].site, (unsigned long long)((uintptr_t)tl_sites[i].site - image),
                (double)tl_sites[i].calls / count, (double)tl_sites[i].pending / count,
                (double)tl_sites[i].ticks * 1e3 / frequency / count);
    const char *path = getenv("DHD_TIMELINE");
    if (path && strcmp(path, "1")) {
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "field,wall_ms");
            for (unsigned i = 0; i < TL_COUNT; i++) fprintf(f, ",%s", tl_names[i]);
            fputc('\n', f);
            for (unsigned r = 0; r < count; r++) {
                fprintf(f, "%u", first + 1 + r);
                for (unsigned c = 0; c <= TL_COUNT; c++) fprintf(f, ",%.4f", rows[r * (TL_COUNT + 1) + c]);
                fputc('\n', f);
            }
            fclose(f);
        }
    }
    fflush(stderr);
}
#else
static int timeline_enabled(void) { return 0; }
static int64_t tl_now(void) { return 0; }
static int64_t tl_begin(void) { return 0; }
static void tl_add(unsigned which, int64_t start) { (void)which, (void)start; }
static void tl_site(void *site, int pending, int64_t ticks) { (void)site, (void)pending, (void)ticks; }
enum { TL_EE_VU, TL_EE_GS, TL_EE_FULL, TL_VU_IDLE, TL_VU_FULL, TL_GS_IDLE };
#endif
