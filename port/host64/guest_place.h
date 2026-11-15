/* Where the bounds-checked guest layout (iOS) goes in a process's address space (finding 258).
   The guest uses two parts of its 2 GiB: [0, 0x3a000000), the RAM views and their mirrors, the
   EE stack slots, the bridge arena and the hardware windows; and [0x70000000, end), the
   scratchpad and the module's own memory. Between them nothing is ever committed. A phone with
   3 GB or less has no 2 GiB hole at all (iOS caps such an app's address space and puts the system's
   shared cache in the middle of it), so the two parts are reserved apart, at one base where both
   are free; whatever lies between them is not the guest's.
   Plain arithmetic, no system calls: port/host64/test_guest_place.c tests it on any machine.
   GPL-3.0-or-later. */
#ifndef DHD_HOST64_GUEST_PLACE_H
#define DHD_HOST64_GUEST_PLACE_H
#include <stdint.h>

typedef struct { uint64_t lo, hi; } GuestRange;   /* [lo, hi) */

typedef struct {
    uint64_t low;    /* the low part: [base, base + low) */
    uint64_t high;   /* the high part starts at base + high (high > low) */
    uint64_t need;   /* and ends at base + need at least */
    uint64_t want;   /* and at base + want at most (room for the memory to grow) */
    uint64_t align;  /* the base's alignment, a power of two */
} GuestPieces;

/* The base for the two parts in the free ranges (any order, not overlapping): the one that gives
   the high part the longest end (need to want), the lowest base among those. 1 with *base and
   *end, 0 when no base fits. */
int guest_place(const GuestRange *free, unsigned count, const GuestPieces *p, uint64_t *base, uint64_t *end);

/* The longest free range (for the log); {0, 0} when there is none. */
GuestRange guest_largest_free(const GuestRange *free, unsigned count);

#endif
