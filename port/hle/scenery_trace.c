/* DHD_SCENERY_TRACE=path: read-only race observer for port/tools/test_scenery_rate.py (finding
   193). Once per race tick (the race clock's whole fields, from GO), one row per flying scenery
   object that has a body: where it is, its state and speed, and the nearest rider and how far
   away it is. Off unless the variable is set; the file holds game-derived state like a state
   trace (keep it under build/ or a scratch folder, never commit it).

   Rows (EE addresses; layouts from the matching units named):
     race      every tick: the player's rider (state: the race phase), so the track is known when
               it has no such object;
     heli      the helicopters' manager D_003AF940 (00172758): up to 7 elements of 0x130, the count
               at +0x888; an element's type at +4, state at +8, body at +0xC (position at body
               +0x10), speed at +0xB8 (units a field).
     bird      the flock manager D_0036B870 (00143D20): a flock at +0x10 (00143538: state at +0,
               birds at +0x70, the count at +4), each bird 0x170 (00142508: state at +0x50, body
               at +0xF0, velocity at +0 in units a field).
     attacker  the swooping attacker's manager D_003F5460 (001918C0): elements of 0xA0 at +0x10,
               the count at +0x214; an element's state at +4, body at +8.
   Riders: as port_fps_observe reads them (0x002AF2C0 + slot * 0x7BD0, position at +0x4270). The
   track is the race setup's (D_004B25A0 +0x2A0, as presence.c reads it). */
#include "hle.h"
#include "../runtime/portable.h"
#include "../include/fps.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned char *body_of(uint32_t pointer)
{
    return pointer >= 0x08000000u && pointer < 0x0A000000u ? (unsigned char *)GUEST(pointer) : NULL;
}

static void row(FILE *out, int tick, const char *kind, int index, int state, int type,
                const unsigned char *body, float speed)
{
    int track = *(const int *)GUEST(0x084B2840);  /* the race setup D_004B25A0 +0x2A0 (presence.c) */
    const float *p = (const float *)(body + 0x10);
    int nearest = -1;
    float best = 1e30f;
    for (int i = 0; i < 10; i++) {
        const unsigned char *r = (const unsigned char *)GUEST(0x082AF2C0u + i * 0x7BD0u);
        const float *q = (const float *)(r + 0x4270);
        float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2], d = dx * dx + dy * dy + dz * dz;
        if (d < best) best = d, nearest = i;
    }
    fprintf(out, "%d,%d,%d,%s,%d,%d,%d,%.8g,%.8g,%.8g,%.8g,%d,%.8g\n", tick, port_fps(), track, kind, index,
            state, type, p[0], p[1], p[2], speed, nearest, sqrtf(best));
}

void port_scenery_observe(void)
{
    static int init, last = -1;
    static FILE *out;
    if (!init) {
        init = 1;
        const char *path = host_getenv("DHD_SCENERY_TRACE");
        if (path && *path) {
            out = fopen(path, "w");
            if (!out) abort();
            fprintf(out, "tick,hz,track,kind,index,state,type,x,y,z,speed,nearest_rider,nearest_distance\n");
        }
    }
    if (!out || !port_race_running(2)) return;
    int tick = *(int *)GUEST(0x0843EEC8);
    if (tick == last) return;
    last = tick;

    /* the race itself: the player's rider, so a run on a track with no such object says which
       track it was */
    const unsigned char *player = (const unsigned char *)GUEST(0x082AF2C0);
    const float *v = (const float *)(player + 0x4180);
    row(out, tick, "race", 0, *(const int *)GUEST(0x0843F348), 0, player + 0x4260,
        sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));

    const unsigned char *heli = (const unsigned char *)GUEST(0x083AF940);
    int count = *(const int *)(heli + 0x888);
    for (int i = 0; i < count && i < 7; i++) {
        const unsigned char *e = heli + i * 0x130, *body = body_of(*(const uint32_t *)(e + 0xC));
        if (body) row(out, tick, "heli", i, *(const int *)(e + 8), *(const int *)(e + 4), body, *(const float *)(e + 0xB8));
    }
    const unsigned char *flocks = (const unsigned char *)GUEST(0x0836B870);
    if (*(const int *)flocks > 0) {
        const unsigned char *flock = flocks + 0x10;
        int birds = *(const int *)(flock + 4);
        for (int i = 0; i < birds && i < 10; i++) {
            const unsigned char *b = flock + 0x70 + i * 0x170, *body = body_of(*(const uint32_t *)(b + 0xF0));
            const float *v = (const float *)b;
            int state = *(const int *)(b + 0x50);
            if (body && state) row(out, tick, "bird", i, state, *(const int *)flock, body,
                                   sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
        }
    }
    const unsigned char *attackers = (const unsigned char *)GUEST(0x083F5460);
    count = *(const int *)(attackers + 0x214);
    for (int i = 0; i < count && i < 3; i++) {
        const unsigned char *e = attackers + 0x10 + i * 0xA0, *body = body_of(*(const uint32_t *)(e + 8));
        if (body) row(out, tick, "attacker", i, *(const int *)(e + 4), 0, body, *(const float *)(e + 0x60));
    }
    fflush(out);
}
