/* The bounds-checked guest layout's placement (guest_place.h, findings 258, 267) on address spaces
   shaped like an iPhone's. GPL-3.0-or-later. */
#include "guest_place.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define COUNT(a) (unsigned)(sizeof a / sizeof *a)

/* guest_memory.c's numbers: [0, 0x3a000000) and [0x70000000, end) with a 64 KiB window, the core's
   initial memory ending at 0x70E00000, the 2 GiB maximum. */
static const GuestPieces CORE = {0x3a000000, 0x70000000, 0x70E00000 + 0x10000, 0x80000000ull + 0x10000, 0x10000};

static int inside(const GuestRange *free, unsigned n, uint64_t lo, uint64_t hi)
{
    for (unsigned i = 0; i < n; i++)
        if (lo >= free[i].lo && hi <= free[i].hi) return 1;
    return 0;
}
/* Both parts aligned, each inside a free range, not overlapping each other. */
static void check_fit(const GuestRange *free, unsigned n, const GuestPieces *p, uint64_t low, uint64_t high, uint64_t end)
{
    CHECK(low && !(low & (p->align - 1)) && high && !(high & (p->align - 1)));
    CHECK(end >= p->need && end <= p->want);
    CHECK(inside(free, n, low, low + p->low));
    CHECK(inside(free, n, high, high + (end - p->high)));
    CHECK(low + p->low <= high || high + (end - p->high) <= low);
}

/* 0.8.9's rule (finding 258): one base, the high part 1792 MiB above the low one. */
static int one_base_fits(const GuestRange *free, unsigned n, const GuestPieces *p)
{
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = 0; j < n; j++) {
            uint64_t lo = (free[i].lo + p->align - 1) & ~(p->align - 1), hi = free[i].hi - p->low;
            if (free[i].hi - free[i].lo < p->low) continue;
            if (free[j].lo > p->high && free[j].lo - p->high > lo) lo = free[j].lo - p->high;
            if (free[j].hi < p->need) continue;
            if (free[j].hi - p->need < hi) hi = free[j].hi - p->need;
            lo = (lo + p->align - 1) & ~(p->align - 1);
            if (lo <= hi) return 1;
        }
    return 0;
}

int main(void)
{
    uint64_t low = 0, high = 0, end = 0;

    /* A desktop's (or the simulator's) space: one huge range; the high part right above the low. */
    GuestRange desktop[] = {{0x100004000ull, 0x7ff000000000ull}};
    CHECK(guest_place(desktop, 1, &CORE, &low, &high, &end));
    CHECK(low == 0x100010000ull && high == low + 0x3a000000 && end == CORE.want);
    check_fit(desktop, 1, &CORE, low, high, end);

    /* The player's iPhone XR on 0.8.9 (finding 267), every free range of 16 MiB or more its log listed
       at the game's start: 0.8.9's one base fits nowhere; the two parts do, the low one in the
       1152 MiB range, the high one with its whole growth room above the shared cache. */
    GuestRange xr[] = {
        {0x11860c000ull, 0x11a000000ull},   /* 25 MiB */
        {0x123654000ull, 0x16b720000ull},   /* 1152 MiB */
        {0x170754000ull, 0x180000000ull},   /* 248 MiB, up to the shared cache */
        {0x340000000ull, 0x358000000ull},   /* 384 MiB, the top */
    };
    CHECK(!one_base_fits(xr, COUNT(xr), &CORE));
    CHECK(guest_place(xr, COUNT(xr), &CORE, &low, &high, &end));
    check_fit(xr, COUNT(xr), &CORE, low, high, end);
    CHECK(low == 0x123660000ull && high == 0x340000000ull && end == CORE.want);
    /* Without the top range: the high part in the 248 MiB below the shared cache, 2040 MiB to grow. */
    CHECK(guest_place(xr, 3, &CORE, &low, &high, &end));
    check_fit(xr, 3, &CORE, low, high, end);
    CHECK(low == 0x123660000ull && high == 0x170760000ull && end == 0x70000000ull + (0x180000000ull - 0x170760000ull));
    /* Only the 1152 MiB range: both parts in it, the high part above the low one, 224 MiB long. */
    CHECK(guest_place(xr + 1, 1, &CORE, &low, &high, &end));
    check_fit(xr + 1, 1, &CORE, low, high, end);
    CHECK(high == low + 0x3a000000 && end == 0x70000000ull + (0x16b720000ull - high));

    /* The same phone at the process's start, as finding 258 drew it: both fit, one base too. */
    GuestRange xr_start[] = {
        {0x101000000ull, 0x102000000ull}, {0x104400000ull, 0x104500000ull}, {0x105600000ull, 0x16f600000ull},
        {0x16fe00000ull, 0x180000000ull}, {0x320000000ull, 0x358000000ull},
    };
    CHECK(one_base_fits(xr_start, COUNT(xr_start), &CORE));
    CHECK(guest_place(xr_start, COUNT(xr_start), &CORE, &low, &high, &end));
    check_fit(xr_start, COUNT(xr_start), &CORE, low, high, end);
    CHECK(low == 0x105600000ull && end == CORE.want);

    /* No 928 MiB anywhere: no room, whatever the high part has. */
    GuestRange xr_late[] = {
        {0x110000000ull, 0x128000000ull}, {0x12a000000ull, 0x150000000ull},
        {0x158000000ull, 0x16a000000ull}, {0x16fe00000ull, 0x180000000ull},
        {0x320000000ull, 0x358000000ull},
    };
    CHECK(!guest_place(xr_late, COUNT(xr_late), &CORE, &low, &high, &end));
    GuestRange big = guest_largest_free(xr_late, COUNT(xr_late));
    CHECK(big.lo == 0x320000000ull && big.hi == 0x358000000ull);   /* above the nano zone, 896 MiB: too short */

    /* Alignment: an unaligned free start rounds up, and a range too short after rounding fails. */
    GuestRange odd[] = {{0x200001234ull, 0x23a011234ull}, {0x270001234ull, 0x272001234ull}};
    CHECK(guest_place(odd, COUNT(odd), &CORE, &low, &high, &end));
    CHECK(low == 0x200010000ull && high == 0x270010000ull && end == 0x70000000ull + (0x272001234ull - 0x270010000ull));
    check_fit(odd, COUNT(odd), &CORE, low, high, end);
    GuestRange tight[] = {{0x200001234ull, 0x23a001234ull}, {0x270000000ull, 0x272000000ull}};
    CHECK(!guest_place(tight, COUNT(tight), &CORE, &low, &high, &end));

    /* Of two places for the high part the one with more room to grow wins, though it lies higher;
       with equal room, the lower low part, then the lower high part. */
    GuestRange two[] = {{0x200000000ull, 0x23a000000ull}, {0x270000000ull, 0x270F00000ull}, {0x470000000ull, 0x480010000ull}};
    CHECK(guest_place(two, COUNT(two), &CORE, &low, &high, &end));
    CHECK(low == 0x200000000ull && high == 0x470000000ull && end == CORE.want);
    GuestRange equal[] = {{0x400000000ull, 0x500000000ull}, {0x200000000ull, 0x300000000ull}};
    CHECK(guest_place(equal, COUNT(equal), &CORE, &low, &high, &end));
    CHECK(low == 0x200000000ull && high == 0x23a000000ull && end == CORE.want);

    /* The high part's minimum: one byte short of need fails. */
    GuestRange short_high[] = {{0x200000000ull, 0x23a000000ull}, {0x270000000ull, 0x270E0FFFFull}};
    CHECK(!guest_place(short_high, COUNT(short_high), &CORE, &low, &high, &end));
    short_high[1].hi++;
    CHECK(guest_place(short_high, COUNT(short_high), &CORE, &low, &high, &end) && end == CORE.need);

    /* Nothing, empty ranges, a bad request. */
    CHECK(!guest_place(NULL, 0, &CORE, &low, &high, &end));
    GuestRange empty[] = {{0x300000000ull, 0x300000000ull}, {0x400000000ull, 0x3f0000000ull}};
    CHECK(!guest_place(empty, COUNT(empty), &CORE, &low, &high, &end));
    big = guest_largest_free(empty, COUNT(empty));
    CHECK(big.lo == 0 && big.hi == 0);
    GuestPieces bad = CORE;
    bad.align = 0x3000;
    CHECK(!guest_place(desktop, 1, &bad, &low, &high, &end));
    bad = CORE;
    bad.need = bad.want + 1;
    CHECK(!guest_place(desktop, 1, &bad, &low, &high, &end));

    puts("PASS guest place: desktop, the player's iPhone XR (0.8.9's one base: no room), the start, late, alignment, growth room");
    return 0;
}
