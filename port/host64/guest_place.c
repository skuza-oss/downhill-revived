/* The bounds-checked guest layout's two host addresses (guest_place.h, findings 258, 267). Written
   for this task; no third-party code. GPL-3.0-or-later. */
#include "guest_place.h"

static uint64_t align_up(uint64_t x, uint64_t mask, int *ok)
{
    if (x > UINT64_MAX - mask) { *ok = 0; return 0; }
    return (x + mask) & ~mask;
}

int guest_place(const GuestRange *free, unsigned count, const GuestPieces *p,
                uint64_t *low_at, uint64_t *high_at, uint64_t *end)
{
    int found = 0;
    uint64_t best_low = 0, best_high = 0, best_end = 0, mask = p->align - 1;
    if (!p->align || (p->align & mask) || p->low > p->high || p->high >= p->need || p->need > p->want)
        return 0;
    const uint64_t need = p->need - p->high, want = p->want - p->high;   /* the high part's length */
    for (unsigned i = 0; i < count; i++) {           /* the range the low part lies in */
        const GuestRange *a = &free[i];
        int ok = 1;
        uint64_t lo = align_up(a->lo, mask, &ok);
        if (!ok || !lo || a->hi <= lo || a->hi - lo < p->low) continue;
        for (unsigned j = 0; j < count; j++) {       /* the range the high part lies in (maybe the same) */
            const GuestRange *b = &free[j];
            uint64_t from = i == j ? lo + p->low : b->lo;   /* in the low part's range: right above it */
            uint64_t hi = align_up(from, mask, &ok);
            if (!ok || b->hi <= hi || b->hi - hi < need) { ok = 1; continue; }
            uint64_t e = p->high + (b->hi - hi < want ? b->hi - hi : want);
            if (!found || e > best_end || (e == best_end && (lo < best_low || (lo == best_low && hi < best_high)))) {
                found = 1;
                best_low = lo;
                best_high = hi;
                best_end = e;
            }
        }
    }
    if (found) {
        *low_at = best_low;
        *high_at = best_high;
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
