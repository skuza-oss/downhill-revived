/* The four streamed sound slots (the SKAT dialogue queues, D_00398C40 + 0x5350; finding 222).

   Each slot's entry (func_001BC868) holds, among others, x0 = positional, x8 = the emitter: a
   pointer to the position the sound comes from. A positional slot that plays is placed every
   update (func_001BBF30): in a race by its distance to the riders (func_001DB6B0), in the front
   end by its distance and angle to the listener (func_00208E40, func_00208C60). Both read the
   emitter.

   The front end's start (func_001B9E88 -> func_001C4E68) asks the rider screen's chatter (SKAT
   event 26) for the rider screen's place 0, which has no rider: its emitter is 0. The event fires
   only when its random roll passes and a line binds, so mostly nothing plays; when it does, the
   update reads the position at address 0. The PS2 read whatever the kernel keeps there; the port
   faults. port_sound_no_emitter makes that update see a sound out of range (amount 0), which the
   game then stops, as it stops any positional sound out of earshot. */
#include <stdio.h>
#include <stdlib.h>

/* The EE program starts at 0x00100000; below it is the PS2 kernel's memory, never a game object:
   an emitter there is none. */
#define GAME_MEMORY 0x00100000u

int port_sound_no_emitter(int slot, unsigned emitter, int key)
{
    static int logged_slot = -1, logged_key = -1;
    if (emitter >= GAME_MEMORY) return 0;
    if (slot != logged_slot || key != logged_key) {
        logged_slot = slot, logged_key = key;
        fprintf(stderr, "sound: slot %d plays a positional sound with no emitter (0x%X): stopped as out of range\n",
                slot, emitter);
    }
    return 1;
}

int port_sound_emitter_null(unsigned emitter) { return emitter < GAME_MEMORY; }

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
