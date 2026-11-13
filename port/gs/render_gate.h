/* The render thread's gate while a phone's app is away (finding 227). The one rule: every piece of
   GPU work the native render thread records, submits or presents (a picture, and on Android a call
   the GS worker posted: native_serve_call) passes this gate, which refuses to start one while the
   app is away, and counts the ones under way (busy), which the platform's callback reads to know the
   thread is quiet. Each absence is a generation: the platform's callback starts one (render_gate_leave);
   a return releases the hold only for the generation it waited through (render_gate_release), so a
   return that finds a newer absence leaves the hold on and waits again. Quitting always passes (no
   poster may hang at the end). Plain C, no locking of its own: native_core.inc holds the render
   queue's lock around every call; port/gs/test_render_gate.c checks the rules. GPL-3.0-or-later. */
#ifndef PORT_GS_RENDER_GATE_H
#define PORT_GS_RENDER_GATE_H

typedef struct RenderGate {
    int hold;            /* the app is away: no new GPU work */
    unsigned generation; /* absences started (render_gate_leave) */
    int busy;            /* GPU work under way on the render thread */
    int held;            /* pictures refused while held (for the log) */
} RenderGate;

/* The platform's callback: an absence starts; returns its generation. */
static inline unsigned render_gate_leave(RenderGate *g)
{
    g->hold = 1;
    return ++g->generation;
}

/* May the render thread start GPU work now? Always when quitting. */
static inline int render_gate_may_start(const RenderGate *g, int quitting) { return quitting || !g->hold; }

/* Around each piece of GPU work: begin only after render_gate_may_start said yes (same lock). */
static inline void render_gate_begin(RenderGate *g) { g->busy++; }
static inline void render_gate_end(RenderGate *g) { g->busy--; }

/* Nothing under way: with the hold on, nothing can start either. */
static inline int render_gate_quiet(const RenderGate *g) { return g->busy == 0; }

/* A return: the hold goes only when no newer absence began after the one waited through. 1 released. */
static inline int render_gate_release(RenderGate *g, unsigned generation)
{
    if (g->generation != generation) return 0;
    g->hold = 0;
    return 1;
}

#endif
