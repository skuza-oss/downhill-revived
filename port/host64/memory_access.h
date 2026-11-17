/* Host address translation only: guest pointer VALUES are never rewritten.
   prepare_wasm2c.py applies this after wasm optimization, before native /O2. */
#ifndef DHD_HOST64_MEMORY_ACCESS_H
#define DHD_HOST64_MEMORY_ACCESS_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "wasm-rt.h"
#include "memory_layout.h"
#if WASM_RT_USE_SEGUE || WABT_BIG_ENDIAN
#error Host64 translation requires little-endian linear memory and Segue disabled
#endif
/* This is an allocator fact, not permission to ignore out-of-bounds accesses.
   Exposing it lets LLVM eliminate checks only for provably in-range addresses. */
static inline uint64_t host64_memory_size(uint64_t size) {
    __builtin_assume(size >= HOST64_MIN_MEMORY_SIZE);
    return size;
}
static inline int host64_alias_crossing(uint64_t address,uint64_t size) {
    uint64_t offset=address&0x1ffffffull, window=address&~0x1ffffffull;
    if (size>0x2000000ull-offset) return 1;
    /* Low RAM aliases start 4 MiB into these windows. */
    return (window==0 || window==0x20000000 || window==0x30000000) &&
        offset<0x400000 && size>0x400000-offset;
}
/* 32-MiB windows (address >> 25) that hold a RAM alias or a low view, where the code below may
   fold or treat a span as crossing: 0x00000000, 0x20000000, 0x28000000, 0x30000000, 0x38000000. */
#define HOST64_ALIAS_WINDOWS 0x11110001u
/* Every view ends below this address: a span starting at or above it is never folded. */
#define HOST64_VIEWS_END 0x3a000000ull
/* Windows (address >> 25, mod 64) where a span of at most 32 MiB may touch a view other than
   canonical RAM: the five above, and the window below each of the upper four (a span starting
   there may cross into it; the low view has none below). */
#define HOST64_ALIAS_SLOW ((1ull << 0) | (3ull << 15) | (3ull << 19) | (3ull << 23) | (3ull << 27))
#if HOST64_TWO_PARTS
/* iOS (finding 258): the guest's memory is two parts, [0, HOST64_VIEWS_END) and
   [HOST64_HIGH_BASE, host64_guest_end), and other mappings of the process may lie in the gap
   between them or above the high part. Wasm2c's own checks are off there (finding 224), so the
   adapters below keep a wild access out of them: every window of the gap (29..55) and of the
   high part above its first 32 MiB (57..63) is slow too, and the slow path ends the process with
   the fault reporter's line (host64_memory_wild) for a span outside both parts, as an access of
   the reservation that was there before did. Window 56 stays plain: the high part always reaches
   HOST64_HIGH_END_MIN, past window 56 and the 64 KiB a plain base's offset may add. */
#define HOST64_SLOW_WINDOWS (HOST64_ALIAS_SLOW | ((1ull << 56) - (1ull << 29)) | (~0ull << 57))
#define HOST64_PLAIN_WINDOW(window) ((window) < 64 && !((HOST64_SLOW_WINDOWS >> (window)) & 1))
extern uint64_t host64_guest_end;   /* guest_memory.c: the high part's end, its 64 KiB window included */
/* The high part has a host address of its own (finding 267): guest address a >= HOST64_HIGH_BASE is
   at host64_high_base + a, every other at base + a (base: the memory's data, guest_base). */
/* A pointer, not an integer: LLVM shares the constants of large offsets only from a pointer base. */
extern uint8_t *host64_high_base;
#define HOST64_HOST(base, address) (((address) >= HOST64_HIGH_BASE ? host64_high_base : (base)) + (address))
/* An access site's base (prepare_wasm2c.py split_accesses), for its hot arm, where host64_base_ok
   holds: lo the function's memory base, hi its high one (HOST64_HIGH_LOCAL, read once per function
   as the memory base is), b the site's 32-bit base value, off its constant offset. The part follows
   from what host64_base_ok allows: an offset in the module's data (b below 16 MiB) is the high part;
   with an offset below 64 KiB, b's own window decides, and a larger offset b + off's, as in
   host64_base_ok. Of a plain window's addresses, those below the gap (below 0x36000000) have bit 30
   clear and window 56's (the high part) set: one bit test and select per base value (an immediate
   compare with 0x70000000 would need the constant in a register), none for the module's data. */
#define HOST64_SITE_BASE(lo, hi, b, off) \
    ((off) >= HOST64_HIGH_BASE ? (hi) : \
     ((((off) + 16 <= 0x10000 ? (uint64_t)(b) : (uint64_t)(b) + (off)) >> 30) & 1) ? (hi) : (lo))
#define HOST64_HIGH_LOCAL(lo) (host64_high_base)
void host64_memory_wild(uint64_t address) __attribute__((noreturn, cold));
/* Whether a span touches neither part's inside: the gap, or at or above the high part's end
   (any address of 2 GiB or more). A zero-size span touches nothing. */
static inline __attribute__((always_inline)) int host64_outside(uint64_t address, uint64_t size) {
    uint64_t end = address + size;
    return size && ((end > HOST64_VIEWS_END && address < HOST64_HIGH_BASE) ||
                    (end > HOST64_HIGH_END_MIN && end > host64_guest_end));
}
#else
#define HOST64_SLOW_WINDOWS HOST64_ALIAS_SLOW
#define HOST64_PLAIN_WINDOW(window) ((window) >= 32 || !((HOST64_ALIAS_WINDOWS >> (window)) & 1))
/* One reservation: one base (finding 267's second base is iOS's). */
#define HOST64_HOST(base, address) ((base) + (address))
#define HOST64_SITE_BASE(lo, hi, b, off) (lo)
#define HOST64_HIGH_LOCAL(lo) (lo)
#endif
/* The full adapter: any address, any size. Callers below inline it only for constant spans
   (it folds away at compile time) and otherwise call it out of line, on the rare paths. */
static inline __attribute__((always_inline)) uint8_t *
host64_memory_address_full(uint8_t *base, uint64_t address, uint64_t size) {
#if HOST64_TWO_PARTS
    if (__builtin_expect(host64_outside(address, size), 0))   /* folds away for a constant span */
        host64_memory_wild(address);
#endif
    /* Fast path, the same result as the full test below: a span inside one 32-MiB window that
       is none of those five is neither folded nor crossing, so its host address is base +
       address. This covers canonical RAM (0x08000000) and the module's own data. A zero size
       (address - 1 in another window) and every other span take the full test. iOS: a plain
       window is one of HOST64_SLOW_WINDOWS' clear bits below 64 (the parts' inside). */
    uint64_t window = address >> 25;
    if (__builtin_expect(window == (address + size - 1) >> 25 && HOST64_PLAIN_WINDOW(window), 1))
        return HOST64_HOST(base, address);
    /* A span crossing a 32 MiB boundary may leave/enter an alias window.
       Preserve its exact virtual-byte sequence using the OS views. Such rare
       spans retain the original full barrier; never fold only their first byte.
       Bounds checks remain in wasm2c, against the ORIGINAL guest address. */
    if (host64_alias_crossing(address,size)) {
        uint8_t *p = HOST64_HOST(base, address);
#if defined(__clang__) || defined(__GNUC__)
        __asm__ volatile("" : "+r"(p) : : "memory");
        return p;
#else
#error Host64 boundary spans require the compiler memory/alias barrier
#endif
    }
    /* Exactly [0x28000000,0x2a000000) and [0x38000000,0x3a000000).
       Keep the high 32 bits in the test: wasm effective addresses use u64. */
    if ((address & ~0x11ffffffull) == 0x28000000ull)
        address &= ~0x30000000ull;
    if ((address&0x1ffffffull)>=0x400000 &&
        (address<0x02000000 || (address>=0x20400000 && address<0x22000000) ||
         (address>=0x30400000 && address<0x32000000)))
        address=0x08000000+(address&0x1ffffffull);
    return HOST64_HOST(base, address);
}
/* Out of line: the inlined fold was half of the executable and about 13% of the EE thread
   (finding 089). Its memory barrier (crossing spans) still acts at the call: the call is opaque
   to the caller's alias analysis, and so is the pointer it returns. */
static __attribute__((noinline, cold)) uint8_t *
host64_memory_address_slow(uint8_t *base, uint64_t address, uint64_t size) {
    /* The two views the game reaches RAM through (finding 090 SP14: 84% low view, 16% the
       0x38000000 mirror of the race's slow calls) first, by the full adapter's own rules: a span
       inside the low view [0x00400000, 0x02000000) or inside [0x38000000, 0x3a000000) crosses
       nothing and folds to canonical RAM at 0x08000000 + (address & 0x1ffffff). */
    uint64_t offset = address & 0x1ffffffull, window = address >> 25;
    if (size <= 0x2000000ull - offset &&
        ((window == 0 && offset >= 0x400000) || window == 0x38000000ull >> 25))
        return base + 0x08000000ull + offset;
    return host64_memory_address_full(base, address, size);
}
/* A span of any size (wasm2c's MEM_ADDR: memory.fill, data segments, memory.copy). */
static inline uint8_t *host64_memory_address(uint8_t *base, uint64_t address,
                                             uint64_t size) {
    if (__builtin_constant_p(address) && __builtin_constant_p(size))
        return host64_memory_address_full(base, address, size);
    uint64_t window = address >> 25;
    if (__builtin_expect(window == (address + size - 1) >> 25 && HOST64_PLAIN_WINDOW(window), 1))
        return HOST64_HOST(base, address);
    return host64_memory_address_slow(base, address, size);
}
/* One scalar or SIMD access (wasm2c's MEM_ADDR_MEMOP: size 1..16). Same result as
   host64_memory_address, with a cheaper inline test:
   - a span starting in a window outside HOST64_SLOW_WINDOWS lies in that window and the next,
     neither of which holds a view other than canonical RAM, so neither folds nor crosses into a
     fold (a crossing there joins two uniquely mapped ranges and needs no barrier);
   - windows are taken mod 64 (one bit test). Every in-bounds address is below 2 GiB (the
     module's maximum memory) and so below window 64; an address beyond, whose window wraps onto
     a slow bit, only takes the full test, which leaves it unchanged (it faults as before; iOS:
     the full test ends the process for it, as it is above the high part);
   - an address the compiler proves at or above HOST64_VIEWS_END (the module's data: a memarg
     offset >= 0x3a000000) or proves constant needs no test at run time (iOS: only a constant;
     there the module's data is above the gap but may run past the high part's end). */
static inline __attribute__((always_inline)) uint8_t *
host64_access_address(uint8_t *base, uint64_t address, uint64_t size) {
    if (!__builtin_constant_p(size) || size - 1 > 15)
        return host64_memory_address(base, address, size);
    if (__builtin_constant_p(address))
        return host64_memory_address_full(base, address, size);
#if !HOST64_TWO_PARTS
    if (__builtin_constant_p(address >= HOST64_VIEWS_END) && address >= HOST64_VIEWS_END)
        return base + address;
#endif
    if (__builtin_expect(!((HOST64_SLOW_WINDOWS >> ((address >> 25) & 63)) & 1), 1))
        return HOST64_HOST(base, address);
    return host64_memory_address_slow(base, address, size);
}
/* Accesses split at the call site (prepare_wasm2c.py split_accesses). wasm2c writes every access
   address as (u64)base + offset, base a 32-bit value and offset the memarg constant; the site
   becomes host64_base_ok(base, offset) ? NAME_hot(...) : NAME_cold(...). The hot access is
   base + address with no test of its own, so the compiler sees the plain address (addressing
   modes, alias analysis) and computes one test for every access through the same base value.
   host64_base_ok(base, offset) true implies, for every size 1..16, that host64_access_address
   gives base + address:
   - offset >= HOST64_VIEWS_END: the span starts above every view;
   - offset + 16 <= 32 MiB: the span lies in the base's window and the next, so the test of
     host64_access_address holds for the base's window as it does for the address's;
   - a larger offset (none in this module) tests the address itself, as host64_access_address.
   iOS (two parts): an offset in the module's data, [HOST64_HIGH_BASE, + 16 MiB), with a base
   below 16 MiB ends below 0x72000000, inside the high part (one compare, the same for every such
   access through one base); any other offset of 64 KiB or more tests the address itself, so a
   plain base in window 56 reaches at most 64 KiB past it, still inside HOST64_HIGH_END_MIN.
   The cold access gives the same result as host64_access_address, out of line. The hot one
   addresses HOST64_SITE_BASE(...) + address: base on one reservation, the part's base on iOS. */
static inline __attribute__((always_inline)) int host64_base_ok(uint32_t base, uint64_t offset) {
#if HOST64_TWO_PARTS
    if (offset >= HOST64_HIGH_BASE && offset + 16 <= HOST64_HIGH_BASE + 0x1000000)
        return __builtin_expect(base < 0x1000000u, 1);
    uint64_t at = offset + 16 <= 0x10000 ? base : base + offset;
#else
    if (offset >= HOST64_VIEWS_END)
        return 1;
    uint64_t at = offset + 16 <= 0x2000000 ? base : base + offset;
#endif
    return __builtin_expect(!((HOST64_SLOW_WINDOWS >> ((at >> 25) & 63)) & 1), 1);
}
static inline __attribute__((always_inline)) uint8_t *
host64_cold_address(uint8_t *base, uint64_t address, uint64_t size) {
    if (__builtin_constant_p(address))
        return host64_memory_address_full(base, address, size);  /* folds at compile time */
    return host64_memory_address_slow(base, address, size);
}
/* wasm memory.copy promises source-snapshot semantics. A crossing range may
   physically overlap a folded range even when their OS pointers look disjoint.
   Keep the usual memmove fast path; snapshot the rare crossing case. */
static inline void host64_memory_copy(uint8_t *dest, uint64_t d,
                                      const uint8_t *src, uint64_t s,
                                      uint64_t n) {
    if (host64_alias_crossing(d,n) || host64_alias_crossing(s,n)) {
        uint8_t *temporary = (uint8_t *)malloc((size_t)n);
        if (!temporary) wasm_rt_trap(WASM_RT_TRAP_EXHAUSTION);
        memcpy(temporary,host64_memory_address((uint8_t *)src,s,n),(size_t)n);
        memcpy(host64_memory_address(dest,d,n),temporary,(size_t)n);
        free(temporary);
    } else {
        memmove(host64_memory_address(dest,d,n),
                host64_memory_address((uint8_t *)src,s,n),(size_t)n);
    }
}
#endif
