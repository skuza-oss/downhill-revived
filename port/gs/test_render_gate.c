/* The render gate's rules (port/gs/render_gate.h, finding 227), as native_core.inc and present.c use
   them: GPU work refused while the app is away and counted while under way; a call posted while held
   deferred, then served at the return or at the quit; two absences overlapping a return, where only
   the newest absence's return lets the renderer go. Pure logic: the locking and the threads around it
   are the renderer's. GPL-3.0-or-later. */
#include "render_gate.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    RenderGate g = {0};

    /* the foreground: work starts, is counted, ends */
    assert(render_gate_may_start(&g, 0) && render_gate_quiet(&g));
    render_gate_begin(&g);
    assert(!render_gate_quiet(&g));

    /* the app leaves while a picture is under way: none new starts; quiet only once it ends */
    unsigned first = render_gate_leave(&g);
    assert(!render_gate_may_start(&g, 0) && !render_gate_quiet(&g));
    render_gate_end(&g);
    assert(render_gate_quiet(&g));

    /* a call the GS worker posted while held (Android): deferred, nothing under way */
    int served = 0;
    if (render_gate_may_start(&g, 0)) served = 1;
    assert(!served && render_gate_quiet(&g));

    /* the return the game thread waited through: released for its own absence */
    assert(render_gate_release(&g, first) && render_gate_may_start(&g, 0));

    /* two absences overlapping a return: the game's wait went through the first (generation read at
       its start); the app left again while the surface was being made; the first return must not
       let go of the second absence's hold */
    unsigned a = render_gate_leave(&g);
    unsigned waited = g.generation;          /* gs_render_generation at the wait's start */
    assert(waited == a);
    unsigned b = render_gate_leave(&g);      /* the second absence, before the first return released */
    assert(!render_gate_release(&g, waited));  /* the first return: refused */
    assert(g.hold && !render_gate_may_start(&g, 0));
    assert(render_gate_release(&g, b));      /* the second absence's own return */
    assert(!g.hold);

    /* a quick flip with no game wait (gs_app_back releases the newest) */
    render_gate_leave(&g);
    assert(render_gate_release(&g, g.generation) && render_gate_may_start(&g, 0));

    /* quit while held: the deferred call is served (no poster hangs), counted while it runs */
    render_gate_leave(&g);
    assert(!render_gate_may_start(&g, 0) && render_gate_may_start(&g, 1));
    render_gate_begin(&g);
    assert(!render_gate_quiet(&g));
    render_gate_end(&g);
    assert(render_gate_quiet(&g));
    puts("render gate: held work refused and counted; posted calls deferred, served at the quit; "
         "only the newest absence's return releases");
    return 0;
}
