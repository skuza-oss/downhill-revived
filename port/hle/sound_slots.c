/* Sounds with no position (finding 222).

   The four streamed sound slots (the SKAT dialogue queues, D_00398C40 + 0x5350) keep in each
   entry (func_001BC868) x0 = positional and x8 = the emitter, a pointer to the position the sound
   comes from. A positional slot that plays is placed every update (func_001BBF30): in a race by its
   distance to the riders (func_001DB6B0), in the front end by its distance (func_00208E40) and
   angle (func_00208C60) to the listener. The distance (func_00222330) reads a null point as the
   origin; the angle read it from address 0, the PS2 kernel's memory, and faults in the port.

   The front end's start (func_001B9E88 -> func_001C4E68) asks the rider screen's chatter (SKAT
   event 26) for the rider screen's place 0, which has no rider: its emitter is 0. Mostly no line
   binds and nothing plays; when one does, the update reads that angle. port_sound_point gives the
   angle the distance's rule: a null point is the origin. The front end's listener stands about 300
   units from it, beyond the chatter's 25, so the game stops the line at once, as on the PS2. */
#include <stdio.h>
#include <stdlib.h>

static float origin[4] __attribute__((aligned(16)));

float *port_sound_point(float *point)
{
    static int logged;
    if (point) return point;
    if (!logged) {
        logged = 1;
        fprintf(stderr, "sound: a sound's angle with no position: from the origin, as its distance (finding 222)\n");
    }
    return origin;
}

/* Test switches for the repeat (docs/TESTING.md):
   DHD_TEST_SOUND_EVENT=N: the chance roll of SKAT event N always passes (it still rolls, so the
   random sequence is the game's own);
   DHD_TEST_SHELL_VOICE=R: the front end's start asks its chatter for rider R instead of place 0's
   (none), so a line binds. */
static int env_int(const char *name, int unset)
{
    const char *s = getenv(name);
    return s && *s ? atoi(s) : unset;
}

int port_test_sound_event(int index)
{
    static int forced = -2;
    if (forced == -2) forced = env_int("DHD_TEST_SOUND_EVENT", -1);
    return forced >= 0 && index == forced;
}

int port_test_shell_voice(int rider)
{
    static int voice = -2;
    if (voice == -2) voice = env_int("DHD_TEST_SHELL_VOICE", -1);
    return voice >= 0 ? voice : rider;
}
