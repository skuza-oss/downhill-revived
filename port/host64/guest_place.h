/* Where the bounds-checked guest layout (iOS) goes in a process's address space (findings 258, 267).
   The guest uses two parts of its 2 GiB: [0, 0x3a000000), the RAM views and their mirrors, the
   EE stack slots, the bridge arena and the hardware windows; and [0x70000000, end), the
   scratchpad and the module's own memory. Between them nothing is ever committed. A phone with
   3 GB or less has no 2 GiB hole at all (iOS caps such an app's address space and puts the system's
   shared cache in the middle of it), and on the iPhone XR not even the two parts 1792 MiB apart
   fit on most starts (finding 267), so each part goes where it fits, at its own host address:
   the low part's is the guest's base, the high part's its second base (memory_access.h).
   Plain arithmetic, no system calls: port/host64/test_guest_place.c tests it on any machine.
   GPL-3.0-or-later. */
#ifndef DHD_HOST64_GUEST_PLACE_H
#define DHD_HOST64_GUEST_PLACE_H
#include <stdint.h>

typedef struct { uint64_t lo, hi; } GuestRange;   /* [lo, hi) */

typedef struct {
    uint64_t low;    /* the low part: guest [0, low) */
    uint64_t high;   /* the high part: guest [high, end), high > low */
    uint64_t need;   /* end at least */
    uint64_t want;   /* end at most (room for the memory to grow) */
    uint64_t align;  /* both parts' host alignment, a power of two */
} GuestPieces;

/* Host addresses for the two parts in the free ranges (any order, not overlapping): *low_at for
   guest [0, low), *high_at for guest [high, *end). Of the choices, the one that gives the high
   part the longest end (need to want), then the lowest low part, then the lowest high part. 1 when
   they fit, 0 when not. */
int guest_place(const GuestRange *free, unsigned count, const GuestPieces *p,
                uint64_t *low_at, uint64_t *high_at, uint64_t *end);

/* The longest free range (for the log); {0, 0} when there is none. */
GuestRange guest_largest_free(const GuestRange *free, unsigned count);

#endif
