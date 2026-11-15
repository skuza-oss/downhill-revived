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
   port/ios/tests/test_guest_space.py builds and runs it. GPL-3.0-or-later. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "wasm-rt.h"
#include "core_host.h"
#include "guest_memory_os.h"
#include "guest_place.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uintptr_t carve_at;   /* the block the holes are opened in */

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
    if (argc > 1 && (!strcmp(argv[1], "late") || !strcmp(argv[1], "none"))) {
        carve_at = fill();
        CHECK(carve_at);
    }
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
    } else if (strcmp(mode, "none")) {
        fprintf(stderr, "usage: test_guest_space start|late|none\n");
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
    *(volatile uint32_t *)(guest_base + 0x70100000) = 7;
    *(volatile uint32_t *)(guest_base + 0x70DFFFFC) = 8;
    CHECK(host64_memory_commit(0x70000000, 0x8000));
    *(volatile uint32_t *)(guest_base + 0x70000000) = 9;
    CHECK(host64_memory_commit(0x06004000, 0x3c000));
    *(volatile uint32_t *)(guest_base + 0x06004000) = 10;
    CHECK(host64_memory_commit(0x11000000, 0x10000));
    CHECK(!host64_memory_commit(0x3a000000, 0x1000));
    CHECK(!host64_memory_commit(0x50000000, 0x1000));
    CHECK(host64_memory_reserved(0x70000000, 0x4000) && host64_memory_reserved(0x10000000, 0x10000));
    CHECK(!host64_memory_reserved(0x39fff000, 0x2000) && !host64_memory_reserved(0x60000000, 0x10));
    CHECK(host64_memory_owns(guest_base + 0x39ffffff) && !host64_memory_owns(guest_base + 0x3a000000) &&
          !host64_memory_owns(guest_base + 0x6fffffff) && host64_memory_owns(guest_base + 0x70000000));
    /* growth up to the reservation's limit, never past it */
    uint64_t pages = m.pages, grown = 0;
    while (wasm_rt_grow_memory(&m, 16) != UINT32_MAX) grown += 16;
    CHECK(m.pages == pages + grown && grown >= 16);
    *(volatile uint32_t *)(m.data + m.size - 4) = 11;
    CHECK(host64_memory_owns(guest_base + m.size + 0xffff));   /* the window above */
    printf("PASS guest space %s: guest memory at 0x%llx, grew to %llu MiB\n", mode,
           (unsigned long long)guest_base, (unsigned long long)(m.size >> 20));
    wasm_rt_free_memory(&m);
    CHECK(!guest_base);
    return 0;
}
