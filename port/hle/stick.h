/* The sticks' dead zone (dhd.ini stick_dead_zone, DHD_STICK_DEAD_ZONE; finding 134).

   The game reads a stick axis as a byte, 128 the centre (func_00220AE0, finding 059), and has its
   own dead zone: raw 91-163 give 0, and an action (steering, leaning, pedalling, braking) squares
   the value (v * v / 127), so the first byte that moves anything is 174 (80 below the centre),
   about 36% of the travel. The squared value 1 (raw 174-177 and 77-80) even reads as a digital
   press there, full lock (func_0012DEB0).

   stick_byte turns SDL's axis value (-32768..32767) into that byte:
   - dead < 0 ("game", the default): as the game always had it, (v >> 8) + 128;
   - dead 0..95 (a percentage): the stick's travel up to dead% is the centre, and the travel past
     it is spread over the bytes that steer in proportion: 178..255 one way, 76..0 the other (the
     first ones whose squared value is 2, past the full-lock pair). Steering then starts at the
     player's dead zone and still reaches full lock at the end of the travel.
   Each axis on its own, as the game reads them (steering is the left stick's X alone).
   The launcher's live indicator (port/launcher/ui/stick.js) draws the same mapping. */
#ifndef DHD_STICK_H
#define DHD_STICK_H

#include <stdint.h>

enum { STICK_GAME = -1, STICK_FIRST_POS = 178, STICK_FIRST_NEG = 76 };

/* The setting's value: STICK_GAME for "game" (or nothing), else the percentage 0..95. */
static inline int stick_dead_zone_of(const char *setting)
{
    if (!setting || setting[0] < '0' || setting[0] > '9') return STICK_GAME;
    int n = 0;
    for (const char *p = setting; *p >= '0' && *p <= '9' && n < 1000; p++) n = n * 10 + (*p - '0');
    return n > 95 ? 95 : n;
}

static inline uint8_t stick_byte(int v, int dead)
{
    if (dead < 0) {
        int x = (v >> 8) + 128;
        return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
    }
    int mag = v >= 0 ? v : -(v + 1);  /* 0..32767 either way */
    if (mag > 32767) mag = 32767;
    int zone = dead * 32767 / 100;
    if (mag <= zone) return 128;
    int span = 32767 - zone, past = mag - zone;
    if (v > 0) return (uint8_t)(STICK_FIRST_POS + ((255 - STICK_FIRST_POS) * past + span / 2) / span);
    return (uint8_t)(STICK_FIRST_NEG - (STICK_FIRST_NEG * past + span / 2) / span);
}

#endif
