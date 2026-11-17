/* iOS's access adapters (memory_access.h with wasm2c's bounds-checked layout, finding 258), on any
   host: the guest's memory is two parts, [0, 0x3a000000) and [0x70000000, end), and what lies
   between and above them is not the guest's. Around every 4 MiB edge up to 8 GiB, for high parts
   of the shortest, a middle and the longest end, every path the generated code has (the call-site
   split's hot and cold arms, the plain access, the bulk span) either gives the address inside a
   part or ends in host64_memory_wild; a hot arm is only ever taken inside a part, and every path
   addresses the high part through its own base (finding 267), the low one through the memory's.
   No memory is touched: the bases are numbers. test_runtime.py builds it with the iOS defines.
   GPL-3.0-or-later. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "memory_access.h"

#if !HOST64_TWO_PARTS
#error build with -DWASM_RT_MEMCHECK_BOUNDS_CHECK=1 (the iOS layout)
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (address 0x%llx size %llu end 0x%llx)\n", \
    __LINE__, #x, (unsigned long long)a, (unsigned long long)size, (unsigned long long)host64_guest_end); exit(1); } } while (0)

uint64_t host64_guest_end;
uint8_t *host64_high_base = (uint8_t *)(uintptr_t)(0x200000000000ull - 0x70000000ull);   /* never dereferenced */
static jmp_buf caught;
static uint64_t wild_seen;
void host64_memory_wild(uint64_t address) { wild_seen = address; longjmp(caught, 1); }

static uint8_t *const base = (uint8_t *)(uintptr_t)0x100000000000ull;   /* never dereferenced */

static int inside(uint64_t a, uint64_t size)
{
    return !size || a + size <= HOST64_VIEWS_END || (a >= HOST64_HIGH_BASE && a + size <= host64_guest_end);
}

/* 1 if host64_memory_wild ended the call (with the span's address), else 0 and *p the result. */
#define TRY(expr, p) (setjmp(caught) ? (wild_seen == a ? 1 : (fprintf(stderr, "FAIL: wild at 0x%llx, not 0x%llx\n", \
    (unsigned long long)wild_seen, (unsigned long long)a), exit(1), 1)) : ((p) = (expr), 0))

int main(void)
{
    static const uint64_t ends[] = {HOST64_HIGH_END_MIN, 0x7c010000ull, 0x80010000ull};
    static const uint64_t offsets[] = {0, 4, 0xfff0, 0x10000, 0x1ffff0, 0x1fffff0, 0x2000000, 0x10000000,
        0x39fffff0, 0x3a000000, 0x40000000, 0x6ffffff0, 0x70000000, 0x70100000, 0x70fffff0, 0x71000000,
        0x7fffffff, 0xffffffffull};
    static const uint64_t bulk[] = {0, 1, 64, 0x10000, 0x2000000, 0x40000000};
    unsigned long long hot = 0, cold = 0, wild = 0;
    for (unsigned e = 0; e < sizeof ends / sizeof ends[0]; e++) {
        host64_guest_end = ends[e];
        volatile uint64_t a = 0, size = 0;
        uint8_t *volatile p = NULL, *volatile q = NULL;
        /* the plain access (MEM_ADDR_MEMOP) and the bulk span (MEM_ADDR) */
        for (uint64_t edge = 0; edge <= 0x200000000ull; edge += 0x400000)
            for (a = edge < 32 ? 0 : edge - 32; a <= edge + 32; a++) {
                for (size = 1; size <= 16; size *= 2) {
                    if (TRY(host64_access_address(base, a, size), p)) { CHECK(!inside(a, size)); wild++; continue; }
                    CHECK(inside(a, size) || a >= 0x80010000ull);   /* 2 GiB and up: as before (finding 224) */
                    if (inside(a, size)) {
                        CHECK(!TRY(host64_memory_address_full(base, a, size), q));
                        CHECK(p == q);
                        CHECK(a >= HOST64_HIGH_BASE ? p == host64_high_base + a : p >= base && p < base + HOST64_VIEWS_END);
                    }
                }
                for (unsigned k = 0; k < sizeof bulk / sizeof bulk[0]; k++) {
                    size = bulk[k];
                    if (TRY(host64_memory_address(base, a, size), p)) { CHECK(!inside(a, size)); wild++; }
                    else CHECK(inside(a, size));
                }
            }
        /* the call-site split: host64_base_ok's hot arm only inside a part; the cold arm either
           inside or wild */
        for (uint64_t edge = 0; edge <= 0x100000000ull; edge += 0x400000)
            for (uint64_t v = edge < 32 ? 0 : edge - 32; v <= edge + 32 && v <= 0xffffffffull; v++)
                for (unsigned k = 0; k < sizeof offsets / sizeof offsets[0]; k++)
                    for (size = 1; size <= 16; size *= 2) {
                        a = v + offsets[k];
                        if (host64_base_ok((uint32_t)v, offsets[k])) {
                            CHECK(inside(a, size) || a >= 0x80010000ull);   /* 2 GiB and up: as before */
                            if (inside(a, size)) {   /* the hot arm's address is the adapter's */
                                CHECK(!TRY(host64_memory_address_full(base, a, size), q));
                                CHECK(HOST64_SITE_BASE(base, host64_high_base, (uint32_t)v, offsets[k]) + a == q);
                            }
                            hot++;
                            continue;
                        }
                        cold++;
                        if (TRY(host64_cold_address(base, a, size), p)) { CHECK(!inside(a, size)); wild++; continue; }
                        CHECK(inside(a, size) || a >= 0x80010000ull);
                    }
    }
    printf("guest access: ok (%llu hot, %llu cold, %llu ended as wild)\n", hot, cold, wild);
    return 0;
}
