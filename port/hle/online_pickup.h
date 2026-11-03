/* Reliable EVENT v2/type 9: pool index, global collector, type, track, xyz.
   No heap addresses or host layout enter the wire format. */
#ifndef DHD_ONLINE_PICKUP_H
#define DHD_ONLINE_PICKUP_H
#include <stdint.h>
#include <string.h>
enum { PICKUP_EVENT_SIZE = 18 };
static inline void pickup_encode(uint8_t *e, int id, int slot, int type, int track, const void *xyz)
{
    e[0] = 2; e[1] = 9; e[2] = id; e[3] = slot; e[4] = type; e[5] = track;
    memcpy(e + 6, xyz, 12);
}
static inline int pickup_matches(const uint8_t *e, int n, int count, int type, int track, const void *xyz)
{
    return n == PICKUP_EVENT_SIZE && e[0] == 2 && e[1] == 9 &&
        e[2] < 150 && e[2] < count && e[3] < 10 && e[4] < 6 &&
        e[4] == type && e[5] == track && !memcmp(e + 6, xyz, 12);
}
#endif
