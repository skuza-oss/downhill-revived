/* dhd.log's perf: lines (port/gs/perf_log.c, finding 129): how fast the game ran and what held it
   back, for every player, from counters that cost a few clock reads a frame. The hooks below are
   called by the threads that wait or present; nothing is written to the log more often than
   every 30 s of play (a render line when the picture's size or the pacing changes, at most every
   2 s). DHD_PERF_LOG=0 turns all of it off.

   The GS test programs (DHD_GS_TEST_BUILD) and the browser (wasm) have no perf_log.c: the hooks
   are empty there. */
#ifndef PERF_LOG_H
#define PERF_LOG_H

#if defined(DHD_GS_TEST_BUILD) || defined(__wasm__)
static inline void perf_presented(void) {}
static inline void perf_wait_self(double seconds) { (void)seconds; }
static inline void perf_present_wait(double seconds) { (void)seconds; }
static inline void perf_render_size(const char *renderer, unsigned scale, unsigned pw, unsigned ph, unsigned ww,
                                    unsigned wh, unsigned vw, unsigned vh)
{
    (void)renderer, (void)scale, (void)pw, (void)ph, (void)ww, (void)wh, (void)vw, (void)vh;
}
static inline int perf_log_on(void) { return 0; }
#else
/* The presenting thread (the render thread, or the game thread with the exact renderer), after
   each picture shown: counts it, and every second takes the threads' counters. */
void perf_presented(void);
/* A wait the calling thread notes itself (not work): the GS worker's handoff to the render thread,
   the render thread's wait for the next field. host_wait_wall's pacing waits are noted by host_sdl.c
   (host_wait_sink). */
void perf_wait_self(double seconds);
/* The render thread's time inside the present (the surface's acquire and present: the GPU, the
   display or the compositor holding it back). */
void perf_present_wait(double seconds);
/* Before perf_presented: the renderer ("native" or "exact"), its scale, the picture's size (the
   internal resolution), the window's drawable size and the part of it the picture fills. */
void perf_render_size(const char *renderer, unsigned scale, unsigned pw, unsigned ph, unsigned ww, unsigned wh,
                      unsigned vw, unsigned vh);
int perf_log_on(void);  /* 0 with DHD_PERF_LOG=0 */
/* host.c's host_thread_name: which of the counted threads this is (game, vu, gs, native). */
void perf_thread(const char *name);
/* present.c: whether the window is in front (focus, not minimised), with the display's refresh
   (Hz, 0 unknown); and every field, whether a race is being ridden. */
void perf_window(int front, double refresh_hz);
void perf_field(int riding);
/* DHD_TIMELINE: the render thread's waits and its present time so far, in seconds. */
void perf_render_waits(double out[2]);
#endif

#endif
