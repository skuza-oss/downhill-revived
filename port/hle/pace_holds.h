/* The presenter's holds that blocked a picture (host.c pacing_present's work samples, finding 280).

   The presenter's thread holds each picture to its due time (pacing_present). A hold is no work of
   a later picture, but it delays one only while a picture waits for that thread: drawn by the GS
   worker and not yet taken by the render thread (pace_holds_waited, from the render thread when it
   takes a picture). While nothing waits, the GS worker is still working on the next picture and the
   hold costs it nothing. Holds end at the time they were meant to end, so a thread that wakes late
   keeps its oversleep as work. Everything here runs on the presenter's thread only.

   A picture's work sample leaves out pace_holds_blocked over its time from its handoff at the
   VBLANK to its arrival at the presenter: pictures queued behind a held picture then read as the
   work they took, not as the queue. */
#ifndef DHD_PACE_HOLDS_H
#define DHD_PACE_HOLDS_H

enum { PACE_HOLDS = 8 };

typedef struct {
    double hold_from[PACE_HOLDS], hold_to[PACE_HOLDS];        /* the last holds */
    double blocked_from[PACE_HOLDS], blocked_to[PACE_HOLDS];  /* their parts a waiting picture sat through */
    double blocked_until;                                     /* the waits so far end here: no part twice */
    unsigned holds, blocked;
} PaceHolds;

/* A hold from `from`, meant to end at `target`, woken at `woke`. */
static inline void pace_holds_hold(PaceHolds *h, double from, double target, double woke)
{
    unsigned i = h->holds++ % PACE_HOLDS;
    h->hold_from[i] = from;
    h->hold_to[i] = woke < target ? woke : target;
}

/* A picture waited for the presenter's thread from `from` to `to` (it was taken then). */
static inline void pace_holds_waited(PaceHolds *h, double from, double to)
{
    if (from < h->blocked_until) from = h->blocked_until;
    if (to <= from) return;
    for (unsigned i = 0; i < PACE_HOLDS; i++) {
        double a = h->hold_from[i] > from ? h->hold_from[i] : from, b = h->hold_to[i] < to ? h->hold_to[i] : to;
        if (b <= a) continue;
        unsigned j = h->blocked++ % PACE_HOLDS;
        h->blocked_from[j] = a, h->blocked_to[j] = b;
    }
    h->blocked_until = to;
}

/* The held time between `from` and `to` that a waiting picture sat through. */
static inline double pace_holds_blocked(const PaceHolds *h, double from, double to)
{
    double held = 0;
    for (unsigned i = 0; i < PACE_HOLDS; i++) {
        double a = h->blocked_from[i] > from ? h->blocked_from[i] : from, b = h->blocked_to[i] < to ? h->blocked_to[i] : to;
        if (b > a) held += b - a;
    }
    return held;
}
#endif
