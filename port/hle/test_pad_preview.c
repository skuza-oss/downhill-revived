/* host_pad_vibrate's preview gate (port/hle/pad_preview.h, finding 141). The game asks for the
   motor values every field (zeros included while nothing vibrates), so a preview must win for its
   window and a switch to off must stop at once. Pure logic, no SDL, no game.
       test_pad_preview            (port/tools/test_pad_preview.py builds and runs it) */
#include "pad_preview.h"

#include <assert.h>
#include <stdio.h>

/* One call to the gate, as host_pad_vibrate makes it: the pads start with last = -1. */
static int step(PadPreview *p, int light, int heavy, int on, int preview, double now, int *light_out)
{
    int out_light = 0, out_heavy = 0;
    int publish = pad_preview_step(p, light, heavy, on, preview, now, &out_light, &out_heavy);
    if (publish) {
        *light_out = out_light;
        assert(out_light == (light && on ? light : 0));  /* never anything but what was asked, or silence */
    }
    return publish;
}

int main(void)
{
    /* The preview's port bit is outside every player index (players are 0-3). */
    assert(PAD_PREVIEW_BIT > 3 && !(PAD_PREVIEW_BIT & 3));
    assert(PAD_PREVIEW_SECONDS > 0);

    PadPreview pad = {-1, 0};
    int light;

    /* Idle: the game's repeated zeros publish once, then nothing. */
    assert(step(&pad, 0, 0, 1, 0, 0.00, &light) == 1 && light == 0);
    assert(step(&pad, 0, 0, 1, 0, 0.01, &light) == 0);
    assert(step(&pad, 0, 0, 1, 0, 0.02, &light) == 0);

    /* The page's buzz: it publishes, and the game's own zeros during its window are dropped. */
    assert(step(&pad, 1, 160, 1, 1, 1.00, &light) == 1 && light == 1);
    assert(step(&pad, 0, 0, 1, 0, 1.01, &light) == 0);
    assert(step(&pad, 0, 0, 1, 0, 1.20, &light) == 0);
    /* ... and after the window the game's values apply again: the buzz stops, nothing is overridden. */
    assert(step(&pad, 0, 0, 1, 0, 1.31, &light) == 1 && light == 0);
    assert(step(&pad, 1, 200, 1, 0, 2.00, &light) == 1 && light == 1);

    /* Switching Vibration off stops at once, even inside the preview's window, and clears the
       window (a later "on" is not still suppressed by it). */
    assert(step(&pad, 1, 160, 1, 1, 3.00, &light) == 1 && light == 1);
    assert(step(&pad, 0, 0, 0, 1, 3.05, &light) == 1 && light == 0);   /* the row is switched off */
    assert(step(&pad, 1, 240, 0, 0, 3.06, &light) == 0);               /* off, and already silent */
    assert(step(&pad, 1, 240, 1, 0, 3.07, &light) == 1 && light == 1); /* on: applies at once, no stale window */

    /* A plain call (the game's own, no preview bit) while the setting is off silences a buzz too. */
    PadPreview quiet = {-1, 0};
    assert(step(&quiet, 1, 160, 1, 1, 5.00, &light) == 1 && light == 1);
    assert(step(&quiet, 1, 160, 0, 0, 5.05, &light) == 1 && light == 0);

    /* The page's release at the end of its buzz: it stops at once and drops the game's zeros. */
    assert(step(&pad, 1, 160, 1, 1, 4.00, &light) == 1 && light == 1);
    assert(step(&pad, 0, 0, 1, 1, 4.25, &light) == 1 && light == 0);
    assert(step(&pad, 0, 0, 1, 0, 4.30, &light) == 0);
    assert(step(&pad, 0, 0, 1, 0, 4.60, &light) == 0);
    assert(step(&pad, 1, 90, 1, 0, 4.61, &light) == 1 && light == 1);  /* the game's own vibration again */

    puts("pad preview: window, game values dropped, off at once, no permanent override PASS");
    return 0;
}
