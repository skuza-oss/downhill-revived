/* The bounds-checked guest layout's base (guest_place.h, finding 258). Written for this task; no
   third-party code. GPL-3.0-or-later. */
#include "guest_place.h"

int guest_place(const GuestRange *free, unsigned count, const GuestPieces *p, uint64_t *base, uint64_t *end)
{
    int found = 0;
    uint64_t best_base = 0, best_end = 0, mask = p->align - 1;
    if (!p->align || (p->align & mask) || p->low > p->high || p->high >= p->need || p->need > p->want)
        return 0;
    for (unsigned i = 0; i < count; i++) {           /* the range the low part lies in */
        const GuestRange *a = &free[i];
        if (a->hi <= a->lo || a->hi - a->lo < p->low) continue;
        for (unsigned j = 0; j < count; j++) {       /* the range the high part starts in (maybe the same) */
            const GuestRange *b = &free[j];
            if (b->hi <= b->lo || b->hi < p->need) continue;
            /* base in [a.lo, a.hi - low] and base + high in [b.lo, b.hi - (need - high)] */
            uint64_t lo = a->lo, hi = a->hi - p->low;
            if (b->lo > p->high && b->lo - p->high > lo) lo = b->lo - p->high;
            if (b->hi - p->need < hi) hi = b->hi - p->need;
            if (lo > UINT64_MAX - mask) continue;
            lo = (lo + mask) & ~mask;
            if (!lo || lo > hi) continue;
            /* the lowest base in the window gives the high part its longest end */
            uint64_t e = b->hi - lo < p->want ? b->hi - lo : p->want;
            if (!found || e > best_end || (e == best_end && lo < best_base)) {
                found = 1;
                best_base = lo;
                best_end = e;
            }
        }
    }
    if (found) {
        *base = best_base;
        *end = best_end;
    }
    return found;
}

GuestRange guest_largest_free(const GuestRange *free, unsigned count)
{
    GuestRange best = {0, 0};
    for (unsigned i = 0; i < count; i++)
        if (free[i].hi > free[i].lo && free[i].hi - free[i].lo > best.hi - best.lo) best = free[i];
    return best;
}
