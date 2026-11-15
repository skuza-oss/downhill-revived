/* The bounds-checked guest layout's placement (guest_place.h, finding 258) on address spaces shaped
   like an iPhone's. GPL-3.0-or-later. */
#include "guest_place.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define COUNT(a) (unsigned)(sizeof a / sizeof *a)

/* guest_memory.c's numbers: [0, 0x3a000000) and [0x70000000, end) with a 64 KiB window, the core's
   initial memory ending at 0x70E00000, the 2 GiB maximum. */
static const GuestPieces CORE = {0x3a000000, 0x70000000, 0x70E00000 + 0x10000, 0x80000000ull + 0x10000, 0x10000};

/* The base and end lie in free ranges, both parts inside one each. */
static int inside(const GuestRange *free, unsigned n, uint64_t lo, uint64_t hi)
{
    for (unsigned i = 0; i < n; i++)
        if (lo >= free[i].lo && hi <= free[i].hi) return 1;
    return 0;
}
static void check_fit(const GuestRange *free, unsigned n, const GuestPieces *p, uint64_t base, uint64_t end)
{
    CHECK(base && !(base & (p->align - 1)));
    CHECK(end >= p->need && end <= p->want);
    CHECK(inside(free, n, base, base + p->low));
    CHECK(inside(free, n, base + p->high, base + end));
}

int main(void)
{
    uint64_t base = 0, end = 0;

    /* A desktop's (or the simulator's) space: one huge range, the whole 2 GiB and its window. */
    GuestRange desktop[] = {{0x100004000ull, 0x7ff000000000ull}};
    CHECK(guest_place(desktop, 1, &CORE, &base, &end));
    CHECK(base == 0x100010000ull && end == CORE.want);
    check_fit(desktop, 1, &CORE, base, end);

    /* An iPhone with 3 GB at the process's start (iOS 18: the shared cache at [0x180000000,
       0x300000000), malloc's nano zone above it, the top at 0x358000000): early malloc below the
       app, the app (33 MB) and dyld near 0x100000000, the main thread's stack below 0x170000000. */
    GuestRange xr_start[] = {
        {0x101000000ull, 0x102000000ull},   /* below the app */
        {0x104400000ull, 0x104500000ull},   /* between the app and dyld */
        {0x105600000ull, 0x16f600000ull},   /* up to the main thread's stack */
        {0x16fe00000ull, 0x180000000ull},   /* up to the shared cache */
        {0x320000000ull, 0x358000000ull},   /* above the nano zone */
    };
    GuestRange big = guest_largest_free(xr_start, COUNT(xr_start));
    CHECK(big.lo == 0x105600000ull && big.hi - big.lo < 0x80010000ull);   /* the old single 2 GiB block: no room */
    CHECK(guest_place(xr_start, COUNT(xr_start), &CORE, &base, &end));
    check_fit(xr_start, COUNT(xr_start), &CORE, base, end);
    CHECK(base == 0x105600000ull);                       /* the lowest base: the longest growth room */
    CHECK(end == 0x180000000ull - 0x105600000ull);       /* the high part runs up to the shared cache */
    CHECK(end - 0x10000 - 0x70E00000 >= 0x7000000);      /* more than 100 MiB to grow */

    /* The same phone late, after the launcher, WebKit and Metal: the low addresses are spread with
       mappings (what 0.8.5 met): nowhere to put the low part 1792 MiB under a free high part. */
    GuestRange xr_late[] = {
        {0x110000000ull, 0x128000000ull}, {0x12a000000ull, 0x150000000ull},
        {0x158000000ull, 0x16a000000ull}, {0x16fe00000ull, 0x180000000ull},
        {0x320000000ull, 0x358000000ull},
    };
    CHECK(!guest_place(xr_late, COUNT(xr_late), &CORE, &base, &end));
    big = guest_largest_free(xr_late, COUNT(xr_late));
    CHECK(big.lo == 0x320000000ull && big.hi == 0x358000000ull);   /* above the nano zone, 896 MiB: too short */

    /* Something already in the gap between the parts does not matter. */
    GuestRange gap_used[] = {{0x200000000ull, 0x23a000000ull}, {0x250000000ull, 0x290000000ull}};
    CHECK(guest_place(gap_used, COUNT(gap_used), &CORE, &base, &end));
    CHECK(base == 0x200000000ull && end == CORE.want);
    check_fit(gap_used, COUNT(gap_used), &CORE, base, end);

    /* Alignment: an unaligned free start rounds up, and a range too short after rounding fails. */
    GuestRange odd[] = {{0x200001234ull, 0x23a011234ull}, {0x270000000ull, 0x272000000ull}};
    CHECK(guest_place(odd, COUNT(odd), &CORE, &base, &end));
    CHECK(base == 0x200010000ull && end == 0x272000000ull - base);
    check_fit(odd, COUNT(odd), &CORE, base, end);
    GuestRange tight[] = {{0x200001234ull, 0x23a001234ull}, {0x270000000ull, 0x272000000ull}};
    CHECK(!guest_place(tight, COUNT(tight), &CORE, &base, &end));

    /* Of two bases the one with more room to grow wins, though it lies higher. */
    GuestRange two[] = {
        {0x200000000ull, 0x23a000000ull}, {0x270000000ull, 0x270F00000ull},   /* 15 MiB: fits, little room */
        {0x400000000ull, 0x43a000000ull}, {0x470000000ull, 0x480010000ull},   /* the whole 2 GiB */
    };
    CHECK(guest_place(two, COUNT(two), &CORE, &base, &end));
    CHECK(base == 0x400000000ull && end == CORE.want);
    /* Equal room: the lower base. */
    GuestRange equal[] = {{0x400000000ull, 0x500000000ull}, {0x200000000ull, 0x300000000ull}};
    CHECK(guest_place(equal, COUNT(equal), &CORE, &base, &end));
    CHECK(base == 0x200000000ull && end == CORE.want);

    /* The high part's minimum: one byte short of need fails. */
    GuestRange short_high[] = {{0x200000000ull, 0x23a000000ull}, {0x270000000ull, 0x270E0FFFFull}};
    CHECK(!guest_place(short_high, COUNT(short_high), &CORE, &base, &end));

    /* Nothing, empty ranges, a bad request. */
    CHECK(!guest_place(NULL, 0, &CORE, &base, &end));
    GuestRange empty[] = {{0x300000000ull, 0x300000000ull}, {0x400000000ull, 0x3f0000000ull}};
    CHECK(!guest_place(empty, COUNT(empty), &CORE, &base, &end));
    big = guest_largest_free(empty, COUNT(empty));
    CHECK(big.lo == 0 && big.hi == 0);
    GuestPieces bad = CORE;
    bad.align = 0x3000;
    CHECK(!guest_place(desktop, 1, &bad, &base, &end));
    bad = CORE;
    bad.need = bad.want + 1;
    CHECK(!guest_place(desktop, 1, &bad, &base, &end));

    puts("PASS guest place: desktop, an iPhone with 3 GB at the start and late, the gap, alignment, growth room");
    return 0;
}
