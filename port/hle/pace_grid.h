/* The grid paced frames keep (kernel.c pace(), host.c host_pacing_wait's slow stretch): each frame
   is due one period after the one before, so frames that come in quick repay a slow one, and the
   grid starts again from now once a frame is more than two periods behind it.

   The period is not a constant of the session (fps.c port_frame_period: 1/60 from a race's load to
   GO, a replay's recorded step every update): a change re-anchors the grid at the last frame's
   place, so the frames before keep the period they had. Multiplying all of them by the new one
   moved the due time by frames x the change, a wait of seconds (144 to 60 after 1000 frames:
   9.7 s). */
#ifndef DHD_PACE_GRID_H
#define DHD_PACE_GRID_H
#include <stdint.h>

typedef struct {
    double base;      /* the grid's start (0: none yet) */
    double period;    /* the period of the frames since base */
    uint64_t frames;  /* frames since base */
} PaceGrid;

/* The next frame's due time, or 0 when the grid starts again from `now` (no wait). */
static inline double pace_grid_next(PaceGrid *grid, double now, double period)
{
    if (period != grid->period) {
        if (grid->base) grid->base += (double)grid->frames * grid->period;
        grid->frames = 0;
        grid->period = period;
    }
    double due = grid->base + (double)++grid->frames * period;
    if (!grid->base || now > due + 2 * period) {
        grid->base = now;
        grid->frames = 0;
        return 0;
    }
    return due;
}
#endif
