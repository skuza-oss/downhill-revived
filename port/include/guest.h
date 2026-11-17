/* Guest (EE) memory for host-side code (docs/findings/081-portable-core.md, phase 2).

   A guest address is a 32-bit PS2 address as the module sees it: PS2 RAM at PS2_RAM_BASE
   (port/runtime/memory.h), the scratchpad, the VU memories and the hardware registers at their
   own addresses (0x11008000, 0x12001000, ...). Host code reaches guest memory only through
   these two macros; it never casts a guest address to a pointer or a pointer to a guest address
   by hand.

   In this build (32-bit Windows) the host pointer of a guest address is the address itself, so
   both macros are the identity. GUEST_BASE is the one hook point: on a 64-bit host it is the
   runtime variable guest_base (defined in port/host/host_win32.c), the host address of guest
   address 0 (GUEST(a) = base + a, GADDR(p) = p - base), and every host file changes with it
   unchanged. It stays 0 (the identity) until the 64-bit host maps guest memory itself. On i686
   it is the constant 0, so that build's code does not change. */
#ifndef DHD_GUEST_H
#define DHD_GUEST_H

#include <stdint.h>

#if defined(__x86_64__) || defined(_WIN64) || defined(__aarch64__)
/* Host address of guest address 0: a variable on a 64-bit host (host_win32.c). */
extern uintptr_t guest_base;
#define GUEST_BASE guest_base
#else
/* Host address of guest address 0: 0 here (identity). */
#define GUEST_BASE ((uintptr_t)0)
#endif

/* Host pointer of a 32-bit guest address. The layout's one switch, HOST64_TWO_PARTS
   (memory_layout.h), decides it here as in guest memory and the access checks (finding 262); the
   module's own units (built for wasm, where the guest's memory is the module's) have no layout. */
#if defined(__wasm__)
#define GUEST_TWO_PARTS 0
#else
#include "../host64/memory_layout.h"
#define GUEST_TWO_PARTS HOST64_TWO_PARTS
#endif
#if GUEST_TWO_PARTS
/* iOS (finding 258): the guest's memory is two parts with other mappings of the process between
   and above them; an address there ends the process as the guest's own fault does
   (guest_memory_posix.c), never reaching what lies there. */
extern uint64_t host64_guest_end;
/* The high part [0x70000000, end) has a host address of its own (finding 267): guest address a
   there is at host64_high_base + a. */
extern uint8_t *host64_high_base;
void host64_memory_wild(uint64_t address) __attribute__((noreturn, cold));
static inline void *guest_pointer(uint32_t addr)
{
    if (__builtin_expect(addr - 0x3a000000u < 0x36000000u || addr >= host64_guest_end, 0))
        host64_memory_wild(addr);
    return (void *)((addr >= 0x70000000u ? (uintptr_t)host64_high_base : GUEST_BASE) + (uintptr_t)addr);
}
#define GUEST(addr) guest_pointer((uint32_t)(addr))
/* The guest address of a host pointer into either part (its inverse). */
static inline uint32_t guest_address(uintptr_t p)
{
    uintptr_t high = (uintptr_t)host64_high_base;
    return (uint32_t)(p - (p - (high + 0x70000000u) < host64_guest_end - 0x70000000u ? high : GUEST_BASE));
}
#define GADDR(ptr) guest_address((uintptr_t)(ptr))
#else
#define GUEST(addr) ((void *)(GUEST_BASE + (uintptr_t)(uint32_t)(addr)))
#endif

/* Guest address of a host pointer (or of a host address held in an integer). The result is
   32 bits: on a 64-bit host it is only meaningful for pointers that point into guest memory. */
#if !GUEST_TWO_PARTS
#define GADDR(ptr) ((uint32_t)((uintptr_t)(ptr) - GUEST_BASE))
#endif

#endif
