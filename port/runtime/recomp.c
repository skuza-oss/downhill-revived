/* Runtime of recompiled MIPS functions (recomp.h). */
#include "recomp.h"
#include "ps2float.h"
#include "../hle/timer.h"  /* the virtual EE clock (ps2_mfc0 under DHD_FRAME_CLOCK) */

#include "../host/host_api.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static ps2_q128 stack[1 << 16]; /* 1 MB */

void ps2_recomp_enter(void)
{
    if (!GPR(29))
        GPR(29) = (uint32_t)(uintptr_t)&stack[sizeof stack / sizeof *stack];
}

void ps2_recomp_bad_jump(const char *func, uint32_t target)
{
    fprintf(stderr, "%s: indirect jump to unknown target 0x%08x\n", func, (unsigned)target);
    abort();
}

float ps2_fdiv(float a, float b)
{
    return ps2_ee_div(a, b);
}

float ps2_fmul(float a, float b)
{
    return ps2_float_mul(a, b);
}

float ps2_fadd(float a, float b)
{
    return ps2_ee_add(a, b);
}

float ps2_fsub(float a, float b)
{
    return ps2_ee_sub(a, b);
}

float ps2_fsqrt(float a)
{
    return ps2_ee_sqrt(a);
}

int32_t ps2_cvt_w_s(float a)
{
    return ps2_float_to_int(a);
}

void ps2_div(int32_t a, int32_t b)
{
    if (b == 0) {
        ps2.lo = S32(a < 0 ? 1 : -1);
        ps2.hi = S32(a);
    } else if (a == INT32_MIN && b == -1) {
        ps2.lo = S32(INT32_MIN);
        ps2.hi = 0;
    } else {
        ps2.lo = S32(a / b);
        ps2.hi = S32(a % b);
    }
}

void ps2_divu(uint32_t a, uint32_t b)
{
    if (b == 0) {
        ps2.lo = S32(-1);
        ps2.hi = S32(a);
    } else {
        ps2.lo = S32(a / b);
        ps2.hi = S32(a % b);
    }
}

/* Little-endian: ldl fills the high bytes of rt from the aligned doubleword up to addr, ldr the
   low bytes from addr on; the pair ldl 7(x) / ldr 0(x) is an unaligned 64-bit load. */
uint64_t ps2_ldl(uint64_t rt, uintptr_t addr)
{
    unsigned s = (unsigned)(addr & 7);
    uint64_t d = *(uint64_t *)(ps2_ram_canonical(addr) & ~(uintptr_t)7);
    unsigned keep = (7 - s) * 8;
    return keep ? (d << keep) | (rt & ((1ull << keep) - 1)) : d;
}

uint64_t ps2_ldr(uint64_t rt, uintptr_t addr)
{
    unsigned s = (unsigned)(addr & 7);
    uint64_t d = *(uint64_t *)(ps2_ram_canonical(addr) & ~(uintptr_t)7);
    return s ? (d >> (s * 8)) | (rt & ~(~0ull >> (s * 8))) : d;
}

uint64_t ps2_lwl(uint64_t rt, uintptr_t addr)
{
    unsigned s = (unsigned)(addr & 3);
    uint32_t w = *(uint32_t *)(ps2_ram_canonical(addr) & ~(uintptr_t)3);
    unsigned keep = (3 - s) * 8;
    uint32_t r = keep ? (w << keep) | ((uint32_t)rt & ((1u << keep) - 1)) : w;
    return S32(r);
}

uint64_t ps2_lwr(uint64_t rt, uintptr_t addr)
{
    unsigned s = (unsigned)(addr & 3);
    uint32_t w = *(uint32_t *)(ps2_ram_canonical(addr) & ~(uintptr_t)3);
    uint32_t r = s ? (w >> (s * 8)) | ((uint32_t)rt & ~(~0u >> (s * 8))) : w;
    /* lwr with s != 0 leaves the upper 32 bits alone on the EE; s == 0 sign-extends */
    return s ? (rt & ~0xFFFFFFFFull) | r : S32(r);
}

/* sdl stores rt's high bytes into the aligned doubleword up to addr, sdr its low bytes from
   addr on (the pair sdl 7(x) / sdr 0(x) is an unaligned 64-bit store). */
void ps2_sdl(uint64_t rt, uintptr_t addr)
{
    unsigned s = (unsigned)(addr & 7);
    uint64_t *d = (uint64_t *)(ps2_ram_canonical(addr) & ~(uintptr_t)7);
    uint64_t mask = ~0ull >> ((7 - s) * 8);
    *d = (*d & ~mask) | (rt >> ((7 - s) * 8));
}

void ps2_sdr(uint64_t rt, uintptr_t addr)
{
    unsigned s = (unsigned)(addr & 7);
    uint64_t *d = (uint64_t *)(ps2_ram_canonical(addr) & ~(uintptr_t)7);
    uint64_t keep = s ? ~0ull >> ((8 - s) * 8) : 0;
    *d = (*d & keep) | (rt << (s * 8));
}

void ps2_swl(uint64_t rt, uintptr_t addr)
{
    unsigned s = (unsigned)(addr & 3);
    uint32_t *w = (uint32_t *)(ps2_ram_canonical(addr) & ~(uintptr_t)3);
    uint32_t mask = ~0u >> ((3 - s) * 8);
    *w = (*w & ~mask) | ((uint32_t)rt >> ((3 - s) * 8));
}

void ps2_swr(uint64_t rt, uintptr_t addr)
{
    unsigned s = (unsigned)(addr & 3);
    uint32_t *w = (uint32_t *)(ps2_ram_canonical(addr) & ~(uintptr_t)3);
    uint32_t keep = s ? ~0u >> ((4 - s) * 8) : 0;
    *w = (*w & keep) | ((uint32_t)rt << (s * 8));
}

uint32_t ps2_mfc0(int reg)
{
    if (reg == 9) {
        /* cop0 Count (EE clock / 2). Under DHD_FRAME_CLOCK it counts the virtual clock (bus cycles
           / 2), so the frame clock stays the only time the game can see (finding 081). The
           shipped SCUS_971.77 executes no mfc0 $9 (checked: no such word in its code), so this
           path only guards the frame clock. */
        if (ee_frame_clock_enabled()) return (uint32_t)(ee_frame_clock_cycles() / 2);
        /* the host's clock, in EE cycles from its first reading (the shipped game never reads it) */
        return (uint32_t)(host_wall_seconds() * 294912000.0);
    }
    return ps2.cop0[reg];
}
