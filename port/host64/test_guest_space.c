/* The bounds-checked guest layout (iOS, finding 258) in a crowded address space, on a Mac or Linux:
   the process's free address space is filled with inaccessible mappings until no range of 2 GiB
   is left (an iPhone with 3 GB has none: the shared cache splits its space), so the reservation
   0.8.5 made (one range of 2 GiB + 64 KiB) fails, and guest_memory.c must still start.
   Link this file first: its constructor runs before guest_memory.c's early reservation, so it can
   fill the space before it (Darwin and glibc run an image's initializers in link order and pass
   them main's arguments).
     start  filled after the start (as on a phone: the launcher, WebKit and Metal come later): the
            start's reservation is kept, the old single reservation fails
     late   filled before the start, two holes opened in main: the start's reservation fails, the
            one at the game's start finds the holes
     none   filled before the start, no holes: the game's start fails, with the free ranges and
            the message in the log (exits by abort)
     phone  filled before the start, then the free ranges of the player's iPhone XR log on 0.8.9
            (finding 267) opened at the same distances: 0.8.9's one base (the high part 1792 MiB
            above the low one) fits nowhere, the two parts at their own addresses do
   In start and late, wild accesses follow: a readable, writable mapping in the gap and one above
   the high part, and a child process that accesses them through every path the game has (the
   core's split, plain and bulk accesses, GUEST()); each child must end by SIGSEGV with the fault
   reporter's line and leave the mapping untouched.
   port/ios/tests/test_guest_space.py builds and runs it. GPL-3.0-or-later. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "wasm-rt.h"
#include "core_host.h"
#include "guest_memory_os.h"
#include "guest_place.h"
#include "memory_access.h"   /* the core's access adapters, as the generated C uses them */
#include "guest.h"           /* GUEST(), the host side's */

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uintptr_t carve_at;   /* the block the holes are opened in */
static struct { uintptr_t at; size_t size; } filler[8192];   /* fill()'s own mappings */
static unsigned fillers;

/* The game's (host_sdl.c, port/hle/host.c): Darwin's linker wants a weak reference resolved. */
void host_error_box(const char *title, const char *message) { fprintf(stderr, "error box: %s: %s\n", title, message); }
void host_log_drain(void) { fflush(stderr); }

/* Fills every free range of 16 MiB or more (smaller ones stay for malloc). Returns the lowest
   filled block of 4 GiB or more. */
static uintptr_t fill(void)
{
    uintptr_t best = 0;
    for (int shift = 47; shift >= 24; shift--) {
        size_t size = (size_t)1 << shift;
        for (;;) {
            void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (p == MAP_FAILED) break;
            if (fillers < sizeof filler / sizeof filler[0]) { filler[fillers].at = (uintptr_t)p; filler[fillers++].size = size; }
            if (shift >= 32 && (!best || (uintptr_t)p < best)) best = (uintptr_t)p;
        }
    }
    return best;
}

/* Two holes in a filled block: 960 MiB, then a mapping in the guest's gap, then 224 MiB from
   1760 MiB above: the largest free range is 960 MiB, yet the guest's two parts fit. */
static void carve(uintptr_t x)
{
    x = (x + 0xfffff) & ~(uintptr_t)0xfffff;
    CHECK(munmap((void *)x, 0x3c000000) == 0);
    CHECK(munmap((void *)(x + 0x6e000000), 0x0e000000) == 0);
}

/* The player's iPhone XR on 0.8.9 (finding 267): every free range of 16 MiB or more its log listed,
   opened in a filled block of fill()'s at the same distances from each other. */
static void carve_phone(void)
{
    static const GuestRange xr[] = {
        {0x11860c000ull, 0x11a000000ull}, {0x123654000ull, 0x16b720000ull},
        {0x170754000ull, 0x180000000ull}, {0x340000000ull, 0x358000000ull}};
    const uint64_t span = 0x358000000ull - 0x100000000ull;
    uintptr_t x = 0;
    for (unsigned i = 0; i < fillers; i++) {   /* the lowest filled block that holds the span, 4 GiB aligned */
        uintptr_t at = (filler[i].at + 0xffffffffull) & ~(uintptr_t)0xffffffffull;
        if (at + span <= filler[i].at + filler[i].size && (!x || at < x)) x = at;
    }
    CHECK(x);
    for (unsigned i = 0; i < sizeof xr / sizeof xr[0]; i++)
        CHECK(munmap((void *)(x + (xr[i].lo - 0x100000000ull)), xr[i].hi - xr[i].lo) == 0);
}

/* 0.8.9's placement (finding 258): one base, the high part 1792 MiB above the low part. */
static int one_base_fits(void)
{
    GuestRange *r = NULL;
    unsigned n = os_free_ranges(&r);
    int fits = 0;
    for (unsigned i = 0; i < n && !fits; i++)
        for (unsigned j = 0; j < n && !fits; j++) {
            if (r[i].hi - r[i].lo < 0x3a000000 || r[j].hi < HOST64_HIGH_END_MIN) continue;
            uint64_t lo = r[i].lo, hi = r[i].hi - 0x3a000000;
            if (r[j].lo > HOST64_HIGH_BASE && r[j].lo - HOST64_HIGH_BASE > lo) lo = r[j].lo - HOST64_HIGH_BASE;
            if (r[j].hi - HOST64_HIGH_END_MIN < hi) hi = r[j].hi - HOST64_HIGH_END_MIN;
            lo = (lo + 0xffff) & ~0xffffull;
            fits = lo <= hi;
        }
    free(r);
    return fits;
}

static int old_reservation_fails(void)
{
    void *p = mmap(NULL, 0x80010000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return 1;
    munmap(p, 0x80010000ull);
    return 0;
}

static uint64_t largest_free(void)
{
    GuestRange *r = NULL;
    unsigned n = os_free_ranges(&r);
    GuestRange big = guest_largest_free(r, n);
    free(r);
    return big.hi - big.lo;
}

__attribute__((constructor)) static void before_start(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "late") || !strcmp(argv[1], "none") || !strcmp(argv[1], "phone"))) {
        carve_at = fill();
        CHECK(carve_at);
    }
}

/* Wild accesses (finding 258): the core's paths. volatile: not constants to the compiler, as a
   wild address in the game is not. A split access has a constant offset, as wasm2c writes it. */
static volatile uint64_t wild_at;
static void by_access(void) { *(volatile uint32_t *)host64_access_address((uint8_t *)guest_base, wild_at, 4) = 0xdeadbeef; }
#define SPLIT(name, OFFSET) static void name(void) { \
    uint32_t b = (uint32_t)(wild_at - (OFFSET)); \
    uint8_t *p = host64_base_ok(b, OFFSET) ? HOST64_SITE_BASE((uint8_t *)guest_base, host64_high_base, b, OFFSET) + (uint64_t)b + (OFFSET) \
                                           : host64_cold_address((uint8_t *)guest_base, (uint64_t)b + (OFFSET), 4); \
    *(volatile uint32_t *)p = 0xdeadbeef; }
SPLIT(by_split_near, 8u)              /* a field of a struct */
SPLIT(by_split_far, 0x40000000u)      /* an offset in the gap itself */
SPLIT(by_split_data, 0x70100000u)     /* the module's data, indexed */
static void by_bulk(void) { memset(host64_memory_address((uint8_t *)guest_base, wild_at, 64), 0xab, 64); }
static void by_host(void) { *(volatile uint32_t *)GUEST(wild_at) = 0xdeadbeef; }

/* Maps 32 KiB readable and writable at guest address map_at (over fill()'s own mapping, or where
   nothing is), runs access at guest address at in a child, and checks the child ended by SIGSEGV
   and nothing reached the mapping. Where the address on its part's base is the other part's own
   memory (the parts may lie side by side, finding 267), nothing is mapped: the child must still end
   by SIGSEGV. */
static void expect_wild(const char *what, void (*access)(void), uint64_t at, uint64_t map_at)
{
    /* where the access would land unchecked: the address on its part's base */
    uintptr_t page = ((map_at >= HOST64_HIGH_BASE ? (uintptr_t)host64_high_base : guest_base) + map_at) & ~(uintptr_t)0x3fff;
    int owned = host64_memory_owns(page);
    CHECK(owned == host64_memory_owns(page + 0x7fff));
    int filled = 1;   /* every 16 KiB page of it in one of fill()'s mappings */
    for (uintptr_t at = page; at < page + 0x8000; at += 0x4000) {
        int in = 0;
        for (unsigned i = 0; i < fillers && !in; i++) in = at >= filler[i].at && at - filler[i].at < filler[i].size;
        filled &= in;
    }
    uint8_t *shared = NULL;
    if (!owned) {
        int flags = MAP_SHARED | MAP_ANONYMOUS | (filled ? MAP_FIXED : 0);
        shared = mmap((void *)page, 0x8000, PROT_READ | PROT_WRITE, flags, -1, 0);
        CHECK(shared == (uint8_t *)page);   /* else something of the process's is there: never touched */
        memset(shared, 0, 0x8000);
    }
    fflush(stdout); fflush(stderr);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) { wild_at = at; access(); _exit(0); }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    if (shared) {
        for (unsigned i = 0; i < 0x8000; i++) CHECK(!shared[i]);
        CHECK(munmap(shared, 0x8000) == 0);
        printf("wild %s at 0x%llx: ended by SIGSEGV, the mapping there untouched\n", what, (unsigned long long)at);
    } else {
        printf("wild %s at 0x%llx: ended by SIGSEGV (the other part lies there)\n", what, (unsigned long long)at);
    }
}

static void wild_accesses(void)
{
    /* the same paths inside the parts work */
    struct { void (*access)(void); uint64_t at; } fine[] = {
        {by_access, 0x08000200}, {by_access, 0x70100010}, {by_split_near, 0x70100020}, {by_split_near, 0x08000300},
        {by_split_far, 0x70000100}, {by_split_data, 0x70100030}, {by_bulk, 0x70100040}, {by_host, 0x70100100},
        {by_host, 0x39fffffc}};
    for (unsigned i = 0; i < sizeof fine / sizeof fine[0]; i++) {
        wild_at = fine[i].at;
        fine[i].access();
        CHECK(*(volatile uint8_t *)GUEST(fine[i].at) == (fine[i].access == by_bulk ? 0xab : 0xef));
    }
    const uint64_t gap = 0x50000000, end = host64_guest_end;
    CHECK(end >= HOST64_HIGH_END_MIN && end <= 0x80010000ull);
    expect_wild("plain access in the gap", by_access, gap, gap);
    expect_wild("split access (a field) in the gap", by_split_near, gap, gap);
    expect_wild("split access (offset in the gap)", by_split_far, gap, gap);
    expect_wild("bulk access in the gap", by_bulk, gap, gap);
    expect_wild("host access in the gap", by_host, gap, gap);
    expect_wild("plain access at the gap's start", by_access, 0x3a000000, 0x3a000000);
    expect_wild("bulk access into the gap", by_bulk, 0x39ffffe0, 0x3a000000);
    expect_wild("plain access past the end", by_access, end, end);
    expect_wild("split access (a field) past the end", by_split_near, end, end);
    expect_wild("split access (the module's data) past the end", by_split_data, end, end);
    expect_wild("bulk access past the end", by_bulk, end, end);
    expect_wild("host access past the end", by_host, end, end);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "start")) {
        uintptr_t x = fill();
        CHECK(x);
        carve(x);
    } else if (!strcmp(mode, "late")) {
        carve(carve_at);
    } else if (!strcmp(mode, "phone")) {
        carve_phone();
        CHECK(!one_base_fits());
        puts("the one base with the high part 1792 MiB above (0.8.9): fails");
    } else if (strcmp(mode, "none")) {
        fprintf(stderr, "usage: test_guest_space start|late|none|phone\n");
        return 2;
    }
    uint64_t big = largest_free();
    printf("largest free range: %llu MiB\n", (unsigned long long)(big >> 20));
    CHECK(big < 0x80010000ull);
    CHECK(old_reservation_fails());
    puts("the single 2 GiB reservation (0.8.5): fails");
    fflush(stdout);

    wasm_rt_memory_t m;
    memset(&m, 0, sizeof m);
    wasm_rt_allocate_memory(&m, 0x70E00000 / 65536, 32768, false, 65536);   /* the core's sizes */
    CHECK(guest_base && m.data == (uint8_t *)guest_base);
    /* the RAM and its mirrors are one memory */
    *(volatile uint32_t *)(guest_base + 0x08000100) = 0x12345678;
    CHECK(*(volatile uint32_t *)(guest_base + 0x28000100) == 0x12345678);
    CHECK(*(volatile uint32_t *)(guest_base + 0x38000100) == 0x12345678);
    *(volatile uint32_t *)(guest_base + 0x00400100) = 0x9abcdef0;   /* the low view: RAM at its own addresses */
    CHECK(*(volatile uint32_t *)(guest_base + 0x08400100) == 0x9abcdef0);
    /* the module's own memory, the scratchpad and the stack slots commit; the gap never does */
    *(volatile uint32_t *)GUEST(0x70100000) = 7;
    *(volatile uint32_t *)GUEST(0x70DFFFFC) = 8;
    CHECK(host64_memory_commit(0x70000000, 0x8000));
    *(volatile uint32_t *)GUEST(0x70000000) = 9;
    CHECK((uint8_t *)GUEST(0x70000000) == host64_high_base + 0x70000000 && GADDR(GUEST(0x70100000)) == 0x70100000 &&
          GADDR(GUEST(0x08000100)) == 0x08000100);
    CHECK(host64_memory_commit(0x06004000, 0x3c000));
    *(volatile uint32_t *)(guest_base + 0x06004000) = 10;
    CHECK(host64_memory_commit(0x11000000, 0x10000));
    CHECK(!host64_memory_commit(0x3a000000, 0x1000));
    CHECK(!host64_memory_commit(0x50000000, 0x1000));
    CHECK(host64_memory_reserved(0x70000000, 0x4000) && host64_memory_reserved(0x10000000, 0x10000));
    CHECK(!host64_memory_reserved(0x39fff000, 0x2000) && !host64_memory_reserved(0x60000000, 0x10));
    /* each part's host addresses map back to its guest addresses; right past the low part is free or
       the high part's start (they may lie side by side), right before the high part free or the low
       part's end */
    uint64_t g = 0;
    uintptr_t high_start = (uintptr_t)host64_high_base + 0x70000000;
    CHECK(host64_memory_guest(guest_base + 0x39ffffff, &g) && g == 0x39ffffff);
    CHECK(host64_memory_guest(high_start + 0x10, &g) && g == 0x70000010);
    CHECK(!host64_memory_owns(guest_base + 0x3a000000) || guest_base + 0x3a000000 == high_start);
    CHECK(!host64_memory_owns(high_start - 1) || high_start - 1 == guest_base + 0x39ffffff);
    CHECK(!host64_memory_owns(guest_base - 1));
    /* growth up to the reservation's limit, never past it */
    uint64_t pages = m.pages, grown = 0;
    while (wasm_rt_grow_memory(&m, 16) != UINT32_MAX) grown += 16;
    CHECK(m.pages == pages + grown && grown >= 16);
    *(volatile uint32_t *)GUEST(m.size - 4) = 11;
    CHECK(host64_memory_owns((uintptr_t)host64_high_base + m.size + 0xffff));   /* the window above */
    wild_accesses();
    printf("PASS guest space %s: guest memory at 0x%llx, grew to %llu MiB\n", mode,
           (unsigned long long)guest_base, (unsigned long long)(m.size >> 20));
    wasm_rt_free_memory(&m);
    CHECK(!guest_base);
    return 0;
}
