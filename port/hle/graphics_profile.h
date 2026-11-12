/* Optional wall-time accounting around the graphics worker's FIFO execution. */
#if !defined(GRAPHICS_QUEUE_TEST) || defined(GS_QUEUE_TEST)
extern volatile uint32_t gs_presented_frames;
extern double vif_gs_seconds;
static FILE *worker_profile;
static int worker_profile_init;
static double worker_profile_after, worker_profile_until, worker_profile_start;
static double worker_profile_busy, worker_profile_gs_start;
static unsigned worker_profile_frame;
static uint64_t worker_profile_cpu;
/* The calling thread's CPU time in 100 ns ticks (host_threads.c's host_current_thread_cpu). */
double host_current_thread_cpu(void);
static uint64_t worker_cpu_ticks(void) { return (uint64_t)(host_current_thread_cpu() * 1e7); }
static double worker_begin(void)
{
    if (!worker_profile_init) {
        worker_profile_init = 1;
        const char *path = getenv("DHD_WORKER_PROFILE");
        if (path) {
            worker_profile = fopen(path, "w");
            if (!worker_profile) { perror(path); abort(); }
            const char *after = getenv("DHD_VU1_PROFILE_AFTER"), *until = getenv("DHD_VU1_PROFILE_UNTIL");
            worker_profile_after = after ? atof(after) : 90;
            worker_profile_until = until ? atof(until) : 150;
        }
    }
    if (!worker_profile) return 0;
    double now = host_wall_seconds();
    if (now < worker_profile_after) return 0;
    if (!worker_profile_start) {
        worker_profile_start = now; worker_profile_cpu = worker_cpu_ticks();
        worker_profile_frame = gs_presented_frames; worker_profile_gs_start = vif_gs_seconds;
    }
    if (now >= worker_profile_until) {
        fprintf(worker_profile, "wall_s,frames,busy_s,gs_s,cpu_s\n%.6f,%u,%.6f,%.6f,%.6f\n",
                now-worker_profile_start, gs_presented_frames-worker_profile_frame, worker_profile_busy,
                vif_gs_seconds-worker_profile_gs_start, (worker_cpu_ticks()-worker_profile_cpu)*1e-7);
        fclose(worker_profile); worker_profile = NULL; return 0;
    }
    return now;
}
static void worker_end(double start) { if (start) worker_profile_busy += host_wall_seconds()-start; }
#else
static double worker_begin(void) { return 0; }
static void worker_end(double start) { (void)start; }
#endif
