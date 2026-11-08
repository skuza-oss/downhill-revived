/* VU0 micro mode: runs the microcode the game uploaded (VIF0 MPG, port/hle/vif.c) from micro
   memory, for the vcallms entries with no native version (vu0_micro.c). Instruction pairs as the
   VU User's Manual documents them; the decoding follows port/tools/vudis.py.

   Arithmetic is host float, as in the macro mode runtime (ps2asm.c): no fused multiply-add, the
   same operation order. Timing is modelled where results depend on it:
   - an FMAC or load result is ready 4 cycles after issue; an instruction reading a register that
     isn't ready stalls until it is (whole register, as the dest fields of in-flight writes);
   - MAC, status and clip flags from an instruction are seen by flag reads 4 cycles after it
     issues, counting stalls (finding 016);
   - Q is written when the division ends (DIV/SQRT 7 cycles, RSQRT 13); a new one waits for the
     last, WAITQ waits for it, q-operands read the Q of the moment.
   The integer unit's results are ready on the next instruction. */
#include "ps2asm.h"
#include "../include/guest.h"
#include "portable.h"
#include "../host/host_api.h"
#include "ps2float.h"
#include "memory.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#if defined(__aarch64__)
/* The Android build (finding 091): SIMDe maps these SSE calls to NEON, the same IEEE single
   operations (under FPCR instead of MXCSR). */
#define SIMDE_ENABLE_NATIVE_ALIASES
#include <simde/x86/sse2.h>
#include <arm_neon.h>
/* Native NEON for the SSE idioms the VU1 kernels lean on (AND3, finding 091). The build's
   -ffp-model=strict makes clang split a vector float compare into four scalar compares (a
   constrained fcmp has no vector lowering on AArch64), so the float compares are the NEON
   instructions themselves: FCMEQ/FCMGT/FCMGE, what SIMDe's vceqq/vcgtq/vcgeq_f32 are, and like
   CMPPS under DAZ they read a denormal as zero under FPCR.FZ. */
static inline __attribute__((always_inline)) uint32x4_t vu_neon_cmpeq(float32x4_t a, float32x4_t b)
{
    uint32x4_t r;
    __asm__("fcmeq %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(a), "w"(b));
    return r;
}
static inline __attribute__((always_inline)) uint32x4_t vu_neon_cmpgt(float32x4_t a, float32x4_t b)
{
    uint32x4_t r;
    __asm__("fcmgt %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(a), "w"(b));
    return r;
}
static inline __attribute__((always_inline)) uint32x4_t vu_neon_cmpge(float32x4_t a, float32x4_t b)
{
    uint32x4_t r;
    __asm__("fcmge %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(a), "w"(b));
    return r;
}
#undef _mm_cmpeq_ps
#undef _mm_cmplt_ps
#undef _mm_cmpgt_ps
#undef _mm_cmple_ps
#undef _mm_cmpge_ps
#define _mm_cmpeq_ps(a, b) vreinterpretq_f32_u32(vu_neon_cmpeq((a), (b)))
#define _mm_cmplt_ps(a, b) vreinterpretq_f32_u32(vu_neon_cmpgt((b), (a)))
#define _mm_cmpgt_ps(a, b) vreinterpretq_f32_u32(vu_neon_cmpgt((a), (b)))
#define _mm_cmple_ps(a, b) vreinterpretq_f32_u32(vu_neon_cmpge((b), (a)))
#define _mm_cmpge_ps(a, b) vreinterpretq_f32_u32(vu_neon_cmpge((a), (b)))
/* The MAC flag bits of a lane mask, x bit 3 .. w bit 0 (SSE: reverse[_mm_movemask_ps(m)]). */
static inline __attribute__((always_inline)) unsigned vu_neon_lanes(uint32x4_t m)
{
    const uint32x4_t weight = {8, 4, 2, 1};
    return vaddvq_u32(vandq_u32(m, weight));
}
/* _mm_cvttps_epi32: FCVTZS, and INT32_MIN where CVTTPS gives its integer indefinite (NaN, or
   at least 2^31; below -2^31 FCVTZS saturates to INT32_MIN itself). Integer tests on the bits,
   so no float compare. */
static inline __attribute__((always_inline)) __m128i vu_neon_cvttps_epi32(__m128 a)
{
    int32x4_t bits = vreinterpretq_s32_f32(a);
    uint32x4_t big = vcgeq_s32(bits, vdupq_n_s32(0x4f000000));  /* +2^31 .. +inf, +NaN */
    uint32x4_t nan = vcgtq_u32(vreinterpretq_u32_f32(a), vdupq_n_u32(0xff800000u));  /* -NaN */
    int32x4_t r = vbslq_s32(vorrq_u32(big, nan), vdupq_n_s32(INT32_MIN), vcvtq_s32_f32(a));
    return vreinterpretq_s64_s32(r);
}
#undef _mm_cvttps_epi32
#define _mm_cvttps_epi32(a) vu_neon_cvttps_epi32(a)
#else
#include <xmmintrin.h>
#include <emmintrin.h>
#endif

enum { FMAC_LATENCY = 4, FLAG_LATENCY = 4, DIV_LATENCY = 7, RSQRT_LATENCY = 13 };
enum { MAX_STEPS = 1 << 20 };  /* a program that never ends (bad upload) gives up */

#ifndef PS2_VU_REFERENCE
static ps2_vu_kick_trace_fn kick_trace;
void ps2_vu1_set_kick_trace(ps2_vu_kick_trace_fn trace) { kick_trace = trace; }
#endif

typedef struct {
    ps2_cpu *c;
    /* A run is bounded to MAX_STEPS and the longest stall is 54 cycles.
       32-bit timestamps cover the entire run, including the final drain. */
    uint32_t cycle;
    uint32_t vf_ready[32];
    uint32_t component_ready[32][4];
    struct { uint32_t at, mac, status, clip, mask, kind; } flags[16];
    int n_flags, flag_head;
    uint32_t flag_cycle;  /* the cycle the flags are read at: the current pair's issue */
    uint32_t clip_next;
    uint32_t mac, status, clip;  /* what flag reads see now */
    uint32_t q_ready;
    float q_next;
    uint32_t q_flags;
    uint32_t p_ready;
    float p_next;
    unsigned unit, code_mask, data_mask, top, itop;
    ps2_vu_kick_fn kick;
    void *opaque;
    unsigned vi_previous_reg;
    uint16_t vi_previous_value;
    uint32_t vi_written_at;
    int vu1_used;
#ifdef PS2_VU_AOT
    /* MAC updates the translated blocks made lazily (commit_upper_lazy): the sticky Z and S lanes
       so far, and the latest result with its dest, valid while no queued MAC update is newer.
       drain_flags folds them in. */
    __m128i lazy_sign, lazy_zero;
    __m128 lazy_last;
    int lazy_dest, lazy_valid, lazy_used;
#endif
    ps2_q128 vu1_file[64]; /* VU0 only: VU1 VF/VI as VU0 addresses them at 0x4000, filled on first use */
} Vu;

/* A pending upper result: written after the lower instruction of the same pair has read. */
typedef struct {
    int reg, dest, to_acc, raw;
    float v[4];
    uint32_t bits[4];
    int flags;          /* 1: MAC and status flags, 2: a clip judgement */
    uint32_t mac, clip_bits;
} Upper;

#define BIT(w, n) ((w) >> (n) & 1)
#define FT(w) ((w) >> 16 & 31)
#define FS(w) ((w) >> 11 & 31)
#define FD(w) ((w) >> 6 & 31)
#define DEST(w) ((w) >> 21 & 15)  /* bit 3 x, 2 y, 1 z, 0 w */
#define HAS(d, comp) ((d) >> (3 - (comp)) & 1)

static int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

static float vf(Vu *u, int r, int comp)
{
    return r ? u->c->vf[r].f[comp] : comp == 3 ? 1.0f : 0.0f;
}

static uint32_t vf_bits(Vu *u, int r, int comp)
{
    float f = vf(u, r, comp);
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

/* Whole registers as vectors: one 16-byte load or store instead of four lanes through a stack
   array, which the host can't forward from the narrow stores into the wide load. */
static inline __attribute__((always_inline)) __m128 vf4(Vu *u, int r)
{
    return r ? _mm_loadu_ps(u->c->vf[r].f) : _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
}

static inline __attribute__((always_inline)) __m128 lane4(__m128 v, int comp)
{
    switch (comp) {
    case 0: return _mm_shuffle_ps(v, v, 0x00);
    case 1: return _mm_shuffle_ps(v, v, 0x55);
    case 2: return _mm_shuffle_ps(v, v, 0xAA);
    default: return _mm_shuffle_ps(v, v, 0xFF);
    }
}

/* Stores the fields in d (bit 3 x .. bit 0 w) of v at p, keeping the others. */
static inline __attribute__((always_inline)) void store_fields(void *p, __m128i v, int d)
{
    if (d == 15) { _mm_storeu_si128((__m128i *)p, v); return; }
    __m128i keep = _mm_setr_epi32(d & 8 ? 0 : -1, d & 4 ? 0 : -1, d & 2 ? 0 : -1, d & 1 ? 0 : -1);
    __m128i old = _mm_loadu_si128((const __m128i *)p);
    _mm_storeu_si128((__m128i *)p, _mm_or_si128(_mm_and_si128(keep, old), _mm_andnot_si128(keep, v)));
}

/* PCSX2's interpreter clamps exponent-255 arithmetic operands and flushes
   denormals. Bitwise moves, MIN/MAX and conversion instructions keep raw bits. */
static float arithmetic(Vu *u, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, 4);
    if (!(bits & 0x7f800000)) bits &= 0x80000000;
    else if ((bits & 0x7f800000) == 0x7f800000) bits = (bits & 0x80000000) | 0x7f7fffff;
    memcpy(&value, &bits, 4);
    return value;
}

static float arithmetic_vf(Vu *u, int reg, int comp) { return arithmetic(u, vf(u, reg, comp)); }

/* SSE2 lanes use the same MXCSR rounding and separate multiply/add as the
   scalar reference. Clamp raw exponent bits before performing arithmetic. */
static __m128 arithmetic4(__m128 value)
{
    __m128i bits = _mm_castps_si128(value), exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7f800000));
    __m128i sign = _mm_and_si128(bits, _mm_set1_epi32((int)0x80000000));
    __m128i small = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
    __m128i large = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7f800000));
    bits = _mm_or_si128(_mm_andnot_si128(small, bits), _mm_and_si128(small, sign));
    bits = _mm_or_si128(_mm_andnot_si128(large, bits),
                        _mm_and_si128(large, _mm_or_si128(sign, _mm_set1_epi32(0x7f7fffff))));
    return _mm_castsi128_ps(bits);
}

/* arithmetic4 of an FMAC operand, in fewer steps, for code running under the VU MXCSR
   (ps2_vu1_run: toward zero, FTZ, DAZ, exceptions masked). DAZ reads a denormal operand as
   the signed zero arithmetic4 would make of it, so only exponent 255 needs the clamp. The
   operation then sees finite operands, so its result is finite and normal or zero: overflow
   toward zero gives the largest finite magnitude and FTZ flushes. arithmetic4 of the result is
   therefore the identity and is skipped (fuzzed against arithmetic4 for ADD/SUB/MUL/MADD/MSUB,
   test_vu1.c). The empty asm hides the operand from the compiler: it must not fold the
   operation (x * 1.0 = x would let a denormal through) or evaluate it under its own rounding. */
#if defined(PS2_VU_IEEE)
#ifdef PS2_VU_IEEE_PROBE
/* Diagnostics: operand lanes flushed (denormal) and clamped (exponent 255); FMAC result lanes
   written (fields in d) that underflowed to a denormal and overflowed to inf. */
static double vu_ieee_probe[4];
__attribute__((export_name("web_vu_ieee_probe"))) double web_vu_ieee_probe(int which) { return vu_ieee_probe[which & 3]; }
static inline void vu_ieee_probe_result(__m128 v, int d)
{
    static const unsigned reverse[16] = {0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15};
    __m128i magnitude = _mm_and_si128(_mm_castps_si128(v), _mm_set1_epi32(0x7fffffff));
    __m128i denormal = _mm_andnot_si128(_mm_cmpeq_epi32(magnitude, _mm_setzero_si128()),
                                        _mm_cmplt_epi32(magnitude, _mm_set1_epi32(0x00800000)));
    vu_ieee_probe[2] += __builtin_popcount(reverse[_mm_movemask_ps(_mm_castsi128_ps(denormal))] & d);
    vu_ieee_probe[3] += __builtin_popcount(reverse[_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7f7fffff))))] & d);
}
#endif
/* The browser's graphics reactor (port/web/build_graphics.py; never the core module, x64 or i686)
   runs VU1 on plain IEEE float ops: round to nearest, no FTZ/DAZ, no VU MXCSR (the player's
   picture only; no VU1 float result feeds game state). What the VU mode did is done here instead,
   with ieee_clamp4 (arithmetic4 in fewer steps: a denormal becomes its signed zero, exponent 255
   the largest finite magnitude). On an operand it is DAZ and the operand clamp. On an FMAC result
   (fmac_flags4, native_result) it is FTZ and what toward-zero rounding makes of an overflow; the
   race measured 53k denormal and 280k inf result lanes in 4000 fields (--vu-ieee-probe), and
   80M denormal and 2.7M exponent-255 operand lanes, so neither clamp is dead. The rounding (toward
   zero) is vu_add4 and the others below. No optimizer barrier: there is no host rounding mode for
   constant folding to disagree with. */
static inline __attribute__((always_inline)) __m128 ieee_clamp4(__m128 value)
{
    __m128i bits = _mm_castps_si128(value), magnitude = _mm_and_si128(bits, _mm_set1_epi32(0x7fffffff));
    __m128i small = _mm_cmplt_epi32(magnitude, _mm_set1_epi32(0x00800000));
    __m128i large = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7f7fffff));
    bits = _mm_andnot_si128(_mm_and_si128(small, _mm_set1_epi32(0x7fffffff)), bits);
    return _mm_castsi128_ps(_mm_xor_si128(bits, _mm_and_si128(large, _mm_xor_si128(magnitude, _mm_set1_epi32(0x7f7fffff)))));
}
static inline __attribute__((always_inline)) __m128 operand4(__m128 value)
{
#ifdef PS2_VU_IEEE_PROBE
    {
        __m128i magnitude = _mm_and_si128(_mm_castps_si128(value), _mm_set1_epi32(0x7fffffff));
        __m128i denormal = _mm_andnot_si128(_mm_cmpeq_epi32(magnitude, _mm_setzero_si128()),
                                            _mm_cmplt_epi32(magnitude, _mm_set1_epi32(0x00800000)));
        vu_ieee_probe[0] += __builtin_popcount(_mm_movemask_ps(_mm_castsi128_ps(denormal)));
        vu_ieee_probe[1] += __builtin_popcount(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7f7fffff)))));
    }
#endif
    return ieee_clamp4(value);
}
/* Toward-zero rounding without a rounding mode (WB1, finding 090). wasm has only round to
   nearest, so the VU mode's rounding was the one difference left above, and it is not just the last
   bit of a picture: a program that turns a float into a count or an index (FTOI) can be one off,
   and the stream it kicks then comes apart (the browser's exploding triangles after an Adrenaline
   Boost pickup). Each operation here is done to nearest, its exact error is found without a rounding
   mode, and a result that went away from zero steps back one unit (its bits minus one: the next
   value toward zero, inf to FLT_MAX). Exact for every input:
   - add/sub: TwoSum's error term (exact under round to nearest); the result went away from zero when
     the error has the other sign;
   - mul, div, sqrt, int to float: the operands' exact product (24 x 24 bits) in double, compared
     with the result (div: the result times the divisor against the dividend);
   so the VU1 results are bit for bit the x64 build's (VU mode: toward zero, FTZ/DAZ), at a few
   SIMD ops each instead of the --exact-float helpers' calls. Operands are already clamped finite
   (operand4/arithmetic), and results still go through ieee_clamp4 (FTZ, overflow). */
static inline __attribute__((always_inline)) __m128 vu_toward_zero4(__m128 r, __m128 away)
{
    return _mm_castsi128_ps(_mm_add_epi32(_mm_castps_si128(r), _mm_castps_si128(away)));  /* -1: one unit down */
}
static inline __attribute__((always_inline)) __m128 vu_add4(__m128 a, __m128 b)
{
    __m128 s = _mm_add_ps(a, b), bb = _mm_sub_ps(s, a);
    __m128 err = _mm_add_ps(_mm_sub_ps(a, _mm_sub_ps(s, bb)), _mm_sub_ps(b, bb));
    __m128i bits = _mm_castps_si128(s);
    __m128i other = _mm_srai_epi32(_mm_xor_si128(bits, _mm_castps_si128(err)), 31);
    __m128i away = _mm_and_si128(other, _mm_castps_si128(_mm_cmpneq_ps(err, _mm_setzero_ps())));
    __m128i inf = _mm_cmpeq_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7fffffff)), _mm_set1_epi32(0x7f800000));
    return vu_toward_zero4(s, _mm_castsi128_ps(_mm_or_si128(away, inf)));
}
static inline __attribute__((always_inline)) __m128 vu_sub4(__m128 a, __m128 b)
{
    return vu_add4(a, _mm_xor_ps(b, _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000))));
}
/* lanes where |r| > |exact|, exact given as two double pairs (lanes 0-1, 2-3) */
static inline __attribute__((always_inline)) __m128 vu_away4(__m128 r, __m128d lo, __m128d hi)
{
    const __m128d magnitude = _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffLL));
    __m128d rlo = _mm_and_pd(_mm_cvtps_pd(r), magnitude), rhi = _mm_and_pd(_mm_cvtps_pd(_mm_movehl_ps(r, r)), magnitude);
    __m128d alo = _mm_cmpgt_pd(rlo, _mm_and_pd(lo, magnitude)), ahi = _mm_cmpgt_pd(rhi, _mm_and_pd(hi, magnitude));
    return _mm_shuffle_ps(_mm_castpd_ps(alo), _mm_castpd_ps(ahi), _MM_SHUFFLE(2, 0, 2, 0));
}
static inline __attribute__((always_inline)) __m128 vu_mul4(__m128 a, __m128 b)
{
    __m128 r = _mm_mul_ps(a, b);
    __m128d lo = _mm_mul_pd(_mm_cvtps_pd(a), _mm_cvtps_pd(b));
    __m128d hi = _mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(a, a)), _mm_cvtps_pd(_mm_movehl_ps(b, b)));
    return vu_toward_zero4(r, vu_away4(r, lo, hi));
}
static inline __attribute__((always_inline)) __m128 vu_itof4(__m128i i)
{
    __m128 r = _mm_cvtepi32_ps(i);
    return vu_toward_zero4(r, vu_away4(r, _mm_cvtepi32_pd(i), _mm_cvtepi32_pd(_mm_shuffle_epi32(i, _MM_SHUFFLE(1, 0, 3, 2)))));
}
static inline float vu_toward_zero(float r)
{
    uint32_t bits; memcpy(&bits, &r, 4); bits--; memcpy(&r, &bits, 4); return r;
}
static inline float vu_addf(float a, float b)
{
    float s = a + b, bb = s - a, err = (a - (s - bb)) + (b - bb);
    return isinf(s) || (err != 0 && signbit(err) != signbit(s)) ? vu_toward_zero(s) : s;
}
static inline float vu_mulf(float a, float b)
{
    float r = a * b;
    return fabs((double)r) > fabs((double)a * (double)b) ? vu_toward_zero(r) : r;
}
static inline float vu_divf(float a, float b)  /* b != 0 */
{
    float r = a / b;
    return fabs((double)r * (double)b) > fabs((double)a) ? vu_toward_zero(r) : r;
}
static inline float vu_sqrtf(float x)  /* x >= 0 */
{
    float r = sqrtf(x);
    return (double)r * (double)r > (double)x ? vu_toward_zero(r) : r;
}
static inline float vu_itof(int32_t i)
{
    float r = (float)i;
    return fabs((double)r) > fabs((double)i) ? vu_toward_zero(r) : r;
}
static inline float vu_daz(float x)
{
    uint32_t bits; memcpy(&bits, &x, 4);
    if (!(bits & 0x7f800000)) bits &= 0x80000000;
    memcpy(&x, &bits, 4); return x;
}
#define VU_ADD4(a, b) vu_add4((a), (b))
#define VU_SUB4(a, b) vu_sub4((a), (b))
#define VU_MUL4(a, b) vu_mul4((a), (b))
#define VU_ITOF4(i) vu_itof4(i)
#define VU_DIV_SS(a, b) _mm_set_ss(vu_divf(_mm_cvtss_f32(a), _mm_cvtss_f32(b)))
#define VU_ADDF(a, b) vu_addf((a), (b))
#define VU_MULF(a, b) vu_mulf((a), (b))
#define VU_DIVF(a, b) vu_divf((a), (b))
#define VU_SQRTF(x) vu_sqrtf(x)
#define VU_ITOF(i) vu_itof(i)
#define VU_DAZ(x) vu_daz(x)
/* An intermediate result inside one VU operation (the product of MADD/MSUB, OPMSUB's, the EFU's
   partial results) is flushed and clamped as the VU mode does before the next step: a plain
   product that overflowed to inf would otherwise swallow the accumulator (-FLT_MAX + inf clamps to
   +FLT_MAX where the VU gives -FLT_MAX + FLT_MAX = 0). */
#define VU_PRODUCT4(a, b) ieee_clamp4(vu_mul4((a), (b)))
#define VU_STEP(u, x) arithmetic((u), (x))
#else
static inline __attribute__((always_inline)) __m128 operand4(__m128 value)
{
#if defined(__aarch64__)
    /* NEON can clamp the unsigned magnitude with UMIN and retain the sign. */
    uint32x4_t bits = vreinterpretq_u32_f32(value);
    const uint32x4_t sign = vdupq_n_u32(0x80000000u);
    uint32x4_t magnitude = vbicq_u32(bits, sign);
    value = vreinterpretq_f32_u32(vbslq_u32(sign, bits, vminq_u32(magnitude, vdupq_n_u32(0x7f7fffff))));
    PS2_OPAQUE_M128(value);
    return value;
#else
    __m128i bits = _mm_castps_si128(value), magnitude = _mm_and_si128(bits, _mm_set1_epi32(0x7fffffff));
    __m128i large = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7f7fffff));
    __m128 result = _mm_castsi128_ps(_mm_xor_si128(bits, _mm_and_si128(large, _mm_xor_si128(magnitude, _mm_set1_epi32(0x7f7fffff)))));
    PS2_OPAQUE_M128(result);
    return result;
#endif
}
/* The VU mode (toward zero, FTZ/DAZ) already rounds and flushes every result, intermediate too. */
#define VU_ADD4(a, b) _mm_add_ps((a), (b))
#define VU_SUB4(a, b) _mm_sub_ps((a), (b))
#define VU_MUL4(a, b) _mm_mul_ps((a), (b))
#define VU_ITOF4(i) _mm_cvtepi32_ps(i)
#define VU_DIV_SS(a, b) _mm_div_ss((a), (b))
#define VU_ADDF(a, b) ((a) + (b))
#define VU_MULF(a, b) ((a) * (b))
#define VU_DIVF(a, b) ((a) / (b))
#define VU_SQRTF(x) sqrtf(x)
#define VU_ITOF(i) ((float)(i))
#define VU_DAZ(x) (x)
#define VU_PRODUCT4(a, b) _mm_mul_ps((a), (b))
#define VU_STEP(u, x) (x)
#endif

static __m128 minmax4(__m128 a, __m128 b, int maximum)
{
    __m128i x = _mm_castps_si128(a), y = _mm_castps_si128(b);
    __m128i both_negative = _mm_srai_epi32(_mm_and_si128(x, y), 31);
    /* Reverse signed-bit ordering when both signs are negative. Equal bit
       patterns may select either input, so a second comparison is unnecessary.
       Retain integer ordering for PS2 NaNs, denormals and signed zero. */
    __m128i take = _mm_xor_si128(_mm_cmpgt_epi32(x, y), both_negative);
    if (!maximum) take = _mm_xor_si128(take, _mm_set1_epi32(-1));
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(take, x), _mm_andnot_si128(take, y)));
}

/* Scalar VU0 lanes share the EE FPU and VU0 macro ordering (ps2float.h). */
static float minmax_bits(float a, float b, int maximum) { return ps2_float_minmax(a, b, maximum); }

static void stall_for(Vu *u, int r)
{
    if (!u->unit && r && u->vf_ready[r] > u->cycle)
        u->cycle = u->vf_ready[r];
}

static void retire_flags(Vu *u)
{
    /* Issued flag results are ordered by cycle. Retire the ready prefix;
       do not copy all outstanding pipeline entries on every instruction. */
    while (u->n_flags && u->flags[u->flag_head].at <= u->flag_cycle) {
        unsigned i = (unsigned)u->flag_head;
        if (u->flags[i].kind == 1) u->mac = u->flags[i].mac;
        u->status = (u->status & ~u->flags[i].mask) | u->flags[i].status;
        if (u->flags[i].kind != 1) u->clip = u->flags[i].clip;
        u->flag_head = (u->flag_head + 1) & 15;
        u->n_flags--;
    }
}

/* Runs before every pair: flags read in this pair are those landed by its issue cycle. They
   are retired only when something reads or replaces them (flags_now): the Q writes to the
   status flag (bits 4, 5, 10 and 11) commute with the queued ones (bits 0-3 and 6-9). */
static inline __attribute__((always_inline)) void apply_flags(Vu *u)
{
    u->flag_cycle = u->cycle;
}

static inline __attribute__((always_inline)) void flags_now(Vu *u)
{
    if (u->n_flags && u->flags[u->flag_head].at <= u->flag_cycle) retire_flags(u);
}

/* Queues the flags an instruction issued now produces: a new MAC flag (and the status bits it
   implies), or a clip judgement shifted in. Each entry holds the whole flag state after it. */
static inline __attribute__((always_inline)) void queue_flags(Vu *u, const Upper *up)
{
    uint32_t mac, clip, st;
#ifdef PS2_VU_AOT
    if (up->flags == 1) u->lazy_valid = 0;  /* this update is newer than any lazy one */
#endif
    if (u->n_flags == 16)
        flags_now(u);
    if (u->n_flags == 16)
        return;
    mac = u->n_flags ? u->flags[(u->flag_head + u->n_flags - 1) & 15].mac : u->mac;
    clip = u->clip_next;
    st = 0;
    if (up->flags == 1) {
        mac = up->mac;
        st = (mac & 0xF ? 1 : 0) | (mac & 0xF0 ? 2 : 0) | (mac & 0xF00 ? 4 : 0) | (mac & 0xF000 ? 8 : 0);
        st |= st << 6;  /* sticky */
    } else {
        clip = up->flags == 3 ? up->clip_bits : ((clip << 6) | up->clip_bits) & 0xFFFFFF;
        u->clip_next = clip;
    }
    unsigned tail = (unsigned)(u->flag_head + u->n_flags) & 15;
    u->flags[tail].at = u->cycle + FLAG_LATENCY;
    u->flags[tail].mac = mac;
    u->flags[tail].status = st;
    u->flags[tail].mask = up->flags == 1 ? 0xF : 0;  /* Z S U O replaced, sticky bits only set */
    u->flags[tail].clip = clip;
    u->flags[tail].kind = up->flags;
    u->n_flags++;
}

#ifdef PS2_VU_AOT
/* The lazy MAC updates, once every queued one has landed: their sticky bits, and the MAC and
   Z S U O status bits of the latest, if no queued update came after it. */
static void lazy_fold(Vu *u)
{
    static const unsigned reverse[16] = {0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15};
    if (u->lazy_valid) {
        __m128 v = u->lazy_last;
        unsigned d = (unsigned)u->lazy_dest;
        unsigned mac = (reverse[_mm_movemask_ps(_mm_cmpeq_ps(v, _mm_setzero_ps()))] & d) |
                       (reverse[_mm_movemask_ps(v)] & d) << 4;
        unsigned st = (mac & 0xF ? 1 : 0) | (mac & 0xF0 ? 2 : 0);
        u->mac = mac;
        u->status = (u->status & ~0xFu) | st;
    }
    u->status |= (_mm_movemask_ps(_mm_castsi128_ps(u->lazy_zero)) ? 0x40u : 0) |
                 (_mm_movemask_ps(_mm_castsi128_ps(u->lazy_sign)) ? 0x80u : 0);
    u->lazy_zero = u->lazy_sign = _mm_setzero_si128();
    u->lazy_valid = u->lazy_used = 0;
}
#endif

/* Latest flag values once every queued one lands (the program's end, a native kernel's entry). */
static void drain_flags(Vu *u)
{
    u->cycle += 64;
    apply_flags(u);
    flags_now(u);
#ifdef PS2_VU_AOT
    if (u->lazy_used) lazy_fold(u);
#endif
}

/* The MAC flags of an FMAC result written to the fields in d. Every caller passes the result of
   an operation on clamped operands under the VU MXCSR, which is finite and normal or zero
   (operand4), so the underflow (nonzero below FLT_MIN) and overflow (exponent 255) bits are 0. */
static inline __attribute__((always_inline)) void fmac_flags4(Upper *out, __m128 v, int d)
{
#ifdef PS2_VU_IEEE_PROBE
    vu_ieee_probe_result(v, d);
#endif
#ifdef PS2_VU_IEEE  /* FTZ and the overflow clamp of plain IEEE results (ieee_clamp4) */
    v = ieee_clamp4(v);
#endif
#if defined(__aarch64__)
    out->mac = (vu_neon_lanes(vu_neon_cmpeq(v, vdupq_n_f32(0.0f))) & d) |
               (vu_neon_lanes(vcltzq_s32(vreinterpretq_s32_f32(v))) & d) << 4;
    out->flags = 1;
    _mm_storeu_ps(out->v, v);  /* an operation on operand4 values needs no result clamp */
#else
    {
        static const unsigned reverse[16] = {0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15};
        __m128 zero = _mm_cmpeq_ps(v, _mm_setzero_ps());
        out->mac = (reverse[_mm_movemask_ps(zero)] & d) |
                   (reverse[_mm_movemask_ps(v)] & d) << 4;
        out->flags = 1;
        _mm_storeu_ps(out->v, v);  /* an operation on operand4 values needs no result clamp */
    }
#endif
}

static inline __attribute__((always_inline)) void fmac_flags(Vu *u, Upper *out, const float r[4], int d)
{
    if (u->unit) {  /* OPMULA/OPMSUB: scalar operands, so keep the result clamp */
        fmac_flags4(out, _mm_loadu_ps(r), d);
        _mm_storeu_ps(out->v, arithmetic4(_mm_loadu_ps(r)));
        return;
    }
    uint32_t mac = 0;
    int comp;
    for (comp = 0; comp < 4; comp++) {
        int bit = 3 - comp;
        if (!HAS(d, comp))
            continue;
        if (r[comp] == 0.0f)
            mac |= 1u << bit;
        uint32_t result_bits; memcpy(&result_bits, &r[comp], 4);
        if (result_bits >> 31)
            mac |= 1u << (bit + 4);
        if (r[comp] != 0.0f && fabsf(r[comp]) < FLT_MIN)
            mac |= 1u << (bit + 8);
        if (u->unit) {
            uint32_t bits; memcpy(&bits, &r[comp], 4);
            if ((bits & 0x7f800000) == 0x7f800000) mac |= 1u << (bit + 12);
            out->v[comp] = arithmetic(u, r[comp]);
        }
        if (isinf(r[comp]))
            mac |= 1u << (bit + 12);
    }
    out->flags = 1;
    out->mac = mac;
}

static inline __attribute__((always_inline)) void write_vf(Vu *u, int r, int d, const float v[4], uint32_t ready)
{
    if (!r)
        return;
    store_fields(u->c->vf[r].f, _mm_loadu_si128((const __m128i *)v), d);
    store_fields(u->component_ready[r], _mm_set1_epi32((int)ready), d);
    /* VU1 stalls read component_ready. vf_ready is the separate VU0
       whole-register scoreboard, never read by a VU1 instruction. */
    if (!u->unit && ready > u->vf_ready[r])
        u->vf_ready[r] = ready;
}

static inline __attribute__((always_inline)) void write_vf_bits(Vu *u, int r, int d, const uint32_t v[4], uint32_t ready)
{
    if (!r)
        return;
    store_fields(u->c->vf[r].w, _mm_loadu_si128((const __m128i *)v), d);
    store_fields(u->component_ready[r], _mm_set1_epi32((int)ready), d);
    if (!u->unit && ready > u->vf_ready[r])
        u->vf_ready[r] = ready;
}

static void set_vi(Vu *u, int r, uint32_t v)
{
    if (r & 15) u->c->vi[r & 15] = (uint16_t)v;
}

static void set_ialu(Vu *u, int r, uint32_t v)
{
    if (r & 15) {
        if (u->vi_previous_reg != (unsigned)(r & 15) || u->vi_written_at + 1 != u->cycle)
            u->vi_previous_value = u->c->vi[r & 15];
        u->vi_previous_reg = r & 15;
        u->vi_written_at = u->cycle;
        u->c->vi[r & 15] = (uint16_t)v;
    }
}

/* The VU1 register file as VU0 micro code addresses it (PCSX2 GET_VU_MEM): VF0..31, then
   VI0..31 one per quadword. VU1 must be idle; the graphics fence waits for it. The reference
   build (test_vu1_pcsx2.py --aot) links next to the native one and shares these. */
#ifndef PS2_VU_REFERENCE
ps2_cpu *ps2_vu1_regs;
uint64_t ps2_vu1_code_version;
uint64_t ps2_vu0_code_version;
int ps2_vu1_native_mode = -1;
int ps2_vu1_aot_mode = -1;  /* tests: 0 runs VU1 on the interpreter only, 1 through the blocks, else DHD_VU_INTERPRETER */
int ps2_vu0_aot_mode = -1;
int ps2_vu0_native_mode = -1;
unsigned long long ps2_vu0_native_runs;
static unsigned long long vu0_native_entries[5];
#ifdef PS2_VU_COVERAGE  /* tests: VU1 pairs run by the interpreter, by pc, and pairs run in blocks */
unsigned long long ps2_vu1_interpreted[2048], ps2_vu1_translated;
#endif  /* tests: 0/1 force the VU0 blocks off/on, else DHD_VU0_AOT */
unsigned long long ps2_vu1_native_runs[PS2_VU1_NATIVE_KINDS];
void (*ps2_vu1_regs_sync)(void);

void ps2_vu0_vu1_file(ps2_q128 file[64])
{
#if defined(__wasm__)
    if (ps2_vu1_regs_sync) ps2_vu1_regs_sync();
    host_vu1_file(GADDR(file));
#else
    memset(file, 0, 64 * sizeof *file);
    const ps2_cpu *v = ps2_vu1_regs;
    if (!v) return;
    if (ps2_vu1_regs_sync) ps2_vu1_regs_sync();
    memcpy(file, v->vf, sizeof v->vf);
    file[0].f[0] = file[0].f[1] = file[0].f[2] = 0; file[0].f[3] = 1;
    for (unsigned i = 0; i < 16; i++) file[32 + i].w[0] = v->vi[i];
    for (unsigned i = 0; i < 16; i++) file[48 + i].w[0] = v->vu0_ctrl[i];
    file[52].w[0] = v->r; memcpy(&file[53].w[0], &v->i, 4);
    memcpy(&file[54].w[0], &v->q, 4); memcpy(&file[55].w[0], &v->p, 4);
#endif
}
#endif

/* AOT blocks know the unit. Inline this boundary so VU1 loads/stores become a
   masked data-memory access, without the VU0 register-file slow path or a call
   that forces the pending upper result and live pipeline state to spill. */
static inline __attribute__((always_inline)) ps2_q128 *mem(Vu *u, uint32_t qword)
{
    if (!u->unit && (qword & 0x400)) {
        if (!u->vu1_used) { ps2_vu0_vu1_file(u->vu1_file); u->vu1_used = 1; }
        return &u->vu1_file[qword & 63];
    }
    return &u->c->vu0_mem[qword & u->data_mask];
}

/* Bits 0/1/2 prove that the selected FS/FT/ACC arithmetic operands are finite.
   Only generated VU1 code supplies proofs; the interpreter supplies zero. Keep
   the rounding barrier even when the exponent-255 clamp is redundant. */
static inline __attribute__((always_inline)) __m128 proven_operand4(__m128 v, int proven)
{
#ifndef PS2_VU_IEEE
    if (proven) { PS2_OPAQUE_M128(v); return v; }
#endif
    return operand4(v);
}

/* VU1 FTOI saturates by the scaled value's sign, including NaNs. The exponent
   test replaces the exceptional lanes after conversion. In-range FCVTZS lanes
   need none of the SSE integer-indefinite emulation used by the general alias. */
static inline __attribute__((always_inline)) __m128 vu1_ftoi4(__m128 value, float scale)
{
    __m128 scaled = _mm_mul_ps(value, _mm_set1_ps(scale));
    __m128i bits = _mm_castps_si128(scaled);
    __m128i exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7f800000));
    __m128i huge = _mm_cmpgt_epi32(exponent, _mm_set1_epi32(0x4effffff));
    __m128i saturated = _mm_xor_si128(_mm_srai_epi32(bits, 31), _mm_set1_epi32(0x7fffffff));
#if defined(__aarch64__)
    __m128i converted = vreinterpretq_s64_s32(vcvtq_s32_f32(scaled));
#else
    __m128i converted = _mm_cvttps_epi32(scaled);
#endif
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(huge, saturated), _mm_andnot_si128(huge, converted)));
}

static inline __attribute__((always_inline)) int upper_proven(Vu *u, uint32_t w, Upper *out, unsigned proven)
{
    int op = w & 0x3F, d = DEST(w), ft = FT(w), fs = FS(w), fd = FD(w), comp;
    float s[4], t[4], r[4], q = u->c->q, i = u->c->i;
    int kind, sets_flags = 1, from = 0;  /* t: 0 field bc of ft, 1 Q, 2 I, 3 ft */
    enum { ADD, SUB, MADD, MSUB, MAX, MINI, MUL };
    out->flags = out->to_acc = out->raw = 0;
    out->reg = -1;
    if (op >= 0x3C) {
        int sub = (fd << 2) | (op & 3);
        int bc = op & 3;
        stall_for(u, fs);
        if (sub == 0x2F)  /* NOP */
            return 0;
        for (comp = 0; comp < 4; comp++)
            s[comp] = vf(u, fs, comp);
        switch (sub) {
        case 0x10: case 0x11: case 0x12: case 0x13: {  /* ITOF0/4/12/15 */
            static const float scale[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
            if (u->unit) {
                __m128 value = VU_ITOF4(_mm_castps_si128(vf4(u, fs)));
                _mm_storeu_ps(out->v, _mm_mul_ps(value, _mm_set1_ps(1.0f / scale[bc])));
                out->reg = ft; out->dest = d;
                return 0;
            }
            for (comp = 0; comp < 4; comp++)
                out->v[comp] = VU_ITOF((int32_t)vf_bits(u, fs, comp)) / scale[bc];
            out->reg = ft, out->dest = d;
            return 0;
        }
        case 0x14: case 0x15: case 0x16: case 0x17: {  /* FTOI0/4/12/15 */
            static const int shift[4] = {0, 4, 12, 15};
            if (u->unit) {
                _mm_storeu_ps((float *)out->bits, vu1_ftoi4(vf4(u, fs), (float)(1 << shift[bc])));
                out->reg = ft; out->dest = d; out->raw = 1;
                return 0;
            }
            for (comp = 0; comp < 4; comp++) {
                if (u->unit) {
                    float x = s[comp] * (float)(1 << shift[bc]);
                    uint32_t bits; memcpy(&bits, &x, 4);
                    out->bits[comp] = (bits & 0x7f800000) >= 0x4f000000
                        ? (bits >> 31 ? 0x80000000 : 0x7fffffff) : (uint32_t)(int32_t)x;
                } else {
                    double x = (double)s[comp] * (double)(1 << shift[bc]);
                    int32_t v = x >= 2147483647.0 ? 2147483647 : x <= -2147483648.0 ? INT32_MIN : (int32_t)x;
                    out->bits[comp] = (uint32_t)v;
                }
            }
            out->reg = ft, out->dest = d, out->raw = 1;
            return 0;
        }
        case 0x1D:  /* ABS */
            for (comp = 0; comp < 4; comp++)
                out->v[comp] = fabsf(s[comp]);
            out->reg = ft, out->dest = d;
            return 0;
        case 0x1F: {  /* CLIP fs.xyz, ft.w */
            float lim;
            uint32_t bits = 0;
            stall_for(u, ft);
            lim = fabsf(vf(u, ft, 3));
            for (comp = 0; comp < 3; comp++) {
                if (u->unit) {
                    uint32_t bound = vf_bits(u, ft, 3), value = vf_bits(u, fs, comp);
                    bound = bound & 0x7f800000 ? bound & 0x7fffffff : 0x007fffff;
                    if ((int32_t)value > (int32_t)bound) bits |= 1u << (comp * 2);
                    if ((int32_t)(value ^ 0x80000000) > (int32_t)bound) bits |= 2u << (comp * 2);
                } else {
                    if (s[comp] > lim) bits |= 1u << (comp * 2);
                    if (s[comp] < -lim) bits |= 2u << (comp * 2);
                }
            }
            out->flags = 2;
            out->clip_bits = bits;
            return 0;
        }
        }
        /* the accumulator forms */
        if (sub < 0x10 || (sub >= 0x18 && sub < 0x1C)) {
            kind = sub < 0x10 ? (sub >> 2) : MUL;
            from = 0;
            stall_for(u, ft);
            for (comp = 0; comp < 4; comp++)
                t[comp] = vf(u, ft, bc);
        } else if (sub == 0x1C || sub == 0x20 || sub == 0x21 || sub == 0x24 || sub == 0x25) {
            kind = sub == 0x1C ? MUL : sub == 0x20 ? ADD : sub == 0x21 ? MADD : sub == 0x24 ? SUB : MSUB;
            from = 1;
            for (comp = 0; comp < 4; comp++)
                t[comp] = q;
        } else if (sub == 0x1E || sub == 0x22 || sub == 0x23 || sub == 0x26 || sub == 0x27) {
            kind = sub == 0x1E ? MUL : sub == 0x22 ? ADD : sub == 0x23 ? MADD : sub == 0x26 ? SUB : MSUB;
            from = 2;
            for (comp = 0; comp < 4; comp++)
                t[comp] = i;
        } else if (sub == 0x28 || sub == 0x29 || sub == 0x2A || sub == 0x2C || sub == 0x2D) {
            kind = sub == 0x28 ? ADD : sub == 0x29 ? MADD : sub == 0x2A ? MUL : sub == 0x2C ? SUB : MSUB;
            from = 3;
            stall_for(u, ft);
            for (comp = 0; comp < 4; comp++)
                t[comp] = vf(u, ft, comp);
        } else if (sub == 0x2E) {  /* OPMULA ACC.xyz = fs.yzx * ft.zxy */
            stall_for(u, ft);
            r[0] = VU_MULF(arithmetic(u, s[1]), arithmetic_vf(u, ft, 2));
            r[1] = VU_MULF(arithmetic(u, s[2]), arithmetic_vf(u, ft, 0));
            r[2] = VU_MULF(arithmetic(u, s[0]), arithmetic_vf(u, ft, 1));
            r[3] = 0;
            out->to_acc = 1, out->dest = d & 0xE;
            memcpy(out->v, r, sizeof r);
            fmac_flags(u, out, r, d & 0xE);
            return 0;
        } else
            return -1;
        out->to_acc = 1;
    } else {
        int bc = op & 3;
        if (op < 0x1C) {
            kind = op >> 2;
            from = 0;
            stall_for(u, ft);
            for (comp = 0; comp < 4; comp++)
                t[comp] = vf(u, ft, bc);
        } else if (op == 0x1C || op == 0x20 || op == 0x21 || op == 0x24 || op == 0x25) {
            kind = op == 0x1C ? MUL : op == 0x20 ? ADD : op == 0x21 ? MADD : op == 0x24 ? SUB : MSUB;
            from = 1;
            for (comp = 0; comp < 4; comp++)
                t[comp] = q;
        } else if (op == 0x1D || op == 0x1E || op == 0x1F || (op >= 0x22 && op <= 0x27 && op != 0x24 && op != 0x25)) {
            kind = op == 0x1D ? MAX : op == 0x1E ? MUL : op == 0x1F ? MINI : op == 0x22 ? ADD : op == 0x23 ? MADD
                 : op == 0x26 ? SUB : MSUB;
            from = 2;
            for (comp = 0; comp < 4; comp++)
                t[comp] = i;
        } else if (op >= 0x28 && op <= 0x2F && op != 0x2E) {
            static const int k[8] = {ADD, MADD, MUL, MAX, SUB, MSUB, 0, MINI};
            kind = k[op - 0x28];
            from = 3;
            stall_for(u, ft);
            for (comp = 0; comp < 4; comp++)
                t[comp] = vf(u, ft, comp);
        } else if (op == 0x2E) {  /* OPMSUB fd.xyz = ACC.xyz - fs.yzx * ft.zxy */
            stall_for(u, fs);
            stall_for(u, ft);
            r[0] = VU_ADDF(arithmetic(u, u->c->vacc[0]), -VU_STEP(u, VU_MULF(arithmetic_vf(u, fs, 1), arithmetic_vf(u, ft, 2))));
            r[1] = VU_ADDF(arithmetic(u, u->c->vacc[1]), -VU_STEP(u, VU_MULF(arithmetic_vf(u, fs, 2), arithmetic_vf(u, ft, 0))));
            r[2] = VU_ADDF(arithmetic(u, u->c->vacc[2]), -VU_STEP(u, VU_MULF(arithmetic_vf(u, fs, 0), arithmetic_vf(u, ft, 1))));
            r[3] = 0;
            out->reg = fd, out->dest = d & 0xE;
            memcpy(out->v, r, sizeof r);
            fmac_flags(u, out, r, d & 0xE);
            return 0;
        } else
            return -1;
        out->reg = fd;
    }
    stall_for(u, fs);
    if (u->unit) {
        __m128 a = vf4(u, fs), b, result;
        int bc = w & 3;
        b = from == 0 ? lane4(vf4(u, ft), bc) : from == 1 ? _mm_set1_ps(q) : from == 2 ? _mm_set1_ps(i) : vf4(u, ft);
        if (kind != MAX && kind != MINI) { a = proven_operand4(a, proven & 1); b = proven_operand4(b, proven & 2); }
        switch (kind) {
        case ADD: result = VU_ADD4(a,b); break;
        case SUB: result = VU_SUB4(a,b); break;
        case MUL: result = VU_MUL4(a,b); break;
        case MAX: result = minmax4(a,b,1); sets_flags = 0; break;
        case MINI: result = minmax4(a,b,0); sets_flags = 0; break;
        case MADD: result = VU_ADD4(proven_operand4(_mm_loadu_ps(u->c->vacc), proven & 4), VU_PRODUCT4(a,b)); break;
        default: result = VU_SUB4(proven_operand4(_mm_loadu_ps(u->c->vacc), proven & 4), VU_PRODUCT4(a,b)); break;
        }
        out->dest = d;
        if (sets_flags)
            fmac_flags4(out, result, d);
        else
            _mm_storeu_ps(out->v, result);
        return 0;
    }
    for (comp = 0; comp < 4; comp++)
        s[comp] = vf(u, fs, comp);
    for (comp = 0; comp < 4; comp++) {
        float prod;
        if (kind != MAX && kind != MINI) {
            s[comp] = arithmetic(u, s[comp]); t[comp] = arithmetic(u, t[comp]);
        }
        switch (kind) {
        case ADD: r[comp] = s[comp] + t[comp]; break;
        case SUB: r[comp] = s[comp] - t[comp]; break;
        case MUL: r[comp] = s[comp] * t[comp]; break;
        case MAX: r[comp] = minmax_bits(s[comp], t[comp], 1); sets_flags = 0; break;
        case MINI: r[comp] = minmax_bits(s[comp], t[comp], 0); sets_flags = 0; break;
        case MADD: prod = s[comp] * t[comp]; r[comp] = arithmetic(u, u->c->vacc[comp]) + prod; break;
        default: prod = s[comp] * t[comp]; r[comp] = arithmetic(u, u->c->vacc[comp]) - prod; break;
        }
    }
    out->dest = d;
    memcpy(out->v, r, sizeof r);
    if (sets_flags)
        fmac_flags(u, out, r, d);
    return 0;
}

static inline __attribute__((always_inline)) int upper(Vu *u, uint32_t w, Upper *out)
{
    return upper_proven(u, w, out, 0);
}

static inline __attribute__((always_inline)) void commit_result(Vu *u, const Upper *up, int lower_reg);

static inline __attribute__((always_inline)) void commit_upper(Vu *u, const Upper *up, int lower_reg)
{
    if (up->flags)
        queue_flags(u, up);
    commit_result(u, up, lower_reg);
}

#ifdef PS2_VU_AOT
/* commit_upper for an FMAC no flag read can observe (vu1_aot.py lazy_flags): no queued update,
   only its sticky lanes and the result kept as the latest MAC for drain_flags. Its result is
   the value fmac_flags4 took the MAC flags from (never OPMULA/OPMSUB, whose stored result is
   clamped after). */
static inline __attribute__((always_inline)) void commit_upper_lazy_proven(Vu *u, const Upper *up, int lower_reg, int sticky_complete)
{
    if (up->flags == 1) {
        const int d = up->dest;
        const __m128i lanes = _mm_setr_epi32(d & 8 ? -1 : 0, d & 4 ? -1 : 0, d & 2 ? -1 : 0, d & 1 ? -1 : 0);
        const __m128 v = _mm_loadu_ps(up->v);
        /* Sticky bits only accumulate. Once both are already set, these ORs
           cannot change the eventual status. Still retain the latest MAC.
           Lazy blocks exclude FSSET/sticky readers (lazy_flags). */
        if (!sticky_complete) {
            u->lazy_sign = _mm_or_si128(u->lazy_sign, _mm_and_si128(_mm_castps_si128(v), lanes));
            u->lazy_zero = _mm_or_si128(u->lazy_zero, _mm_and_si128(_mm_castps_si128(_mm_cmpeq_ps(v, _mm_setzero_ps())), lanes));
        }
        u->lazy_last = v; u->lazy_dest = d; u->lazy_valid = u->lazy_used = 1;
    } else if (up->flags)
        queue_flags(u, up);
    commit_result(u, up, lower_reg);
}
static inline __attribute__((always_inline)) void commit_upper_lazy(Vu *u, const Upper *up, int lower_reg)
{
    commit_upper_lazy_proven(u, up, lower_reg, (u->status & 0xc0) == 0xc0);
}
#endif

static inline __attribute__((always_inline)) void commit_result(Vu *u, const Upper *up, int lower_reg)
{
    if (up->to_acc) {
        store_fields(u->c->vacc, _mm_loadu_si128((const __m128i *)up->v), up->dest);
        return;
    }
    (void)lower_reg;  /* the upper write wins over a lower one to the same register */
    if (up->reg < 0)
        return;
    if (up->raw)
        write_vf_bits(u, up->reg, up->dest, up->bits, u->cycle + FMAC_LATENCY);
    else
        write_vf(u, up->reg, up->dest, up->v, u->cycle + FMAC_LATENCY);
}

static void start_q(Vu *u, float value, int latency, uint32_t flags)
{
    if (u->q_ready > u->cycle) {  /* the divider is busy: wait for it */
        u->cycle = u->q_ready;
        u->c->q = u->q_next;
    }
    u->q_next = arithmetic(u, value);
    u->q_ready = u->cycle + latency;
    if (u->unit) u->q_flags = flags;
    else u->status = (u->status & ~0x30u) | flags | (flags << 6);
}

static inline __attribute__((always_inline)) void update_q(Vu *u)
{
    if (u->q_ready && u->q_ready <= u->cycle) {
        u->c->q = u->q_next;
        if (u->unit) u->status = (u->status & ~0x30u) | u->q_flags;
        u->q_ready = 0;
    }
}

static float vu_max(int negative) { return negative ? -FLT_MAX : FLT_MAX; }

/* Executes the lower instruction; returns the branch target (instruction index) or -1, and
   *bad if the instruction isn't one VU0 has. */
static inline __attribute__((always_inline)) ps2_q128 *lower_data(Vu *u, uint32_t qword, ps2_q128 *data)
{
    return data ? &data[qword & u->data_mask] : mem(u, qword);
}

/* The closed native clipping caller supplies its fixed physical data memory.
   Other callers pass NULL and retain the general VU0/VU1 accessor. The cached
   buffer cannot alias the CPU register file or the private pipeline. */
static inline __attribute__((always_inline)) int lower_cached(Vu *u, uint32_t w, unsigned pc, int *bad,
                                                              ps2_q128 *__restrict data)
{
    ps2_cpu *c = u->c;
    int op = w >> 25, it = FT(w), is = FS(w), d = DEST(w), comp;
    int32_t imm11 = sext(w & 0x7FF, 11);
    uint16_t vit = c->vi[it & 15], vis = c->vi[is & 15];
    int target = (int)pc + 1 + imm11;
    if (op == 0x40) {
        int sub = w & 0x3F;
        if (sub < 0x3C) {
            int id = FD(w);
            switch (sub) {
            case 0x30: set_ialu(u, id, vis + vit); return -1;
            case 0x31: set_ialu(u, id, vis - vit); return -1;
            case 0x32: set_ialu(u, it, vis + sext((uint32_t)id, 5)); return -1;
            case 0x34: set_ialu(u, id, vis & vit); return -1;
            case 0x35: set_ialu(u, id, vis | vit); return -1;
            }
            *bad = 1;
            return -1;
        }
        switch ((FD(w) << 2) | (sub & 3)) {
        case 0x30: case 0x31: {  /* MOVE, MR32 ft, fs */
            uint32_t v[4];
            stall_for(u, is);
            __m128 x = vf4(u, is);
            _mm_storeu_ps((float *)v, (sub & 3) ? _mm_shuffle_ps(x, x, 0x39) : x);
            write_vf_bits(u, it, d, v, u->cycle + FMAC_LATENCY);
            return -1;
        }
        case 0x34: case 0x36: {  /* LQI ft, (is++) / LQD ft, (--is) */
            ps2_q128 *m;
            if ((sub & 3) == 2)
                set_ialu(u, is, --vis);
            m = lower_data(u, vis, data);
            write_vf_bits(u, it, d, m->w, u->cycle + FMAC_LATENCY);
            if ((sub & 3) == 0)
                set_ialu(u, is, vis + 1);
            return -1;
        }
        case 0x35: case 0x37: {  /* SQI fs, (it++) / SQD fs, (--it) */
            ps2_q128 *m;
            stall_for(u, is);
            if ((sub & 3) == 3)
                set_ialu(u, it, --vit);
            m = lower_data(u, vit, data);
            store_fields(m->w, _mm_castps_si128(vf4(u, is)), d);
            if ((sub & 3) == 1)
                set_ialu(u, it, vit + 1);
            return -1;
        }
        case 0x38: case 0x3A: {  /* DIV / RSQRT Q, fs.fsf, ft.ftf */
            float s, t;
            uint32_t fl = 0;
            stall_for(u, is);
            stall_for(u, it);
            s = arithmetic_vf(u, is, w >> 21 & 3);
            t = arithmetic_vf(u, it, w >> 23 & 3);
            if (u->unit && (sub & 3) == 2 && t == 0.0f) {
                uint32_t bits = (vf_bits(u, is, w >> 21 & 3) ^ vf_bits(u, it, w >> 23 & 3)) & 0x80000000;
                if (s != 0.0f) bits |= 0x7f7fffff;
                float result; memcpy(&result, &bits, 4);
                start_q(u, result, RSQRT_LATENCY, s == 0.0f ? 0x30 : 0x20);
                return -1;
            }
            if ((sub & 3) == 2) {
                if (t < 0.0f)
                    fl |= 0x10;
                t = VU_SQRTF(fabsf(t));
            }
            if (t == 0.0f) {
                fl |= s == 0.0f && (sub & 3) == 0 ? 0x10 : 0x20;
                start_q(u, vu_max(signbit(s) != signbit(t)), (sub & 3) ? RSQRT_LATENCY : DIV_LATENCY, fl);
            } else
                start_q(u, VU_DIVF(s, t), (sub & 3) ? RSQRT_LATENCY : DIV_LATENCY, fl);
            return -1;
        }
        case 0x39: {  /* SQRT Q, ft.ftf */
            float t;
            stall_for(u, it);
            t = arithmetic_vf(u, it, w >> 23 & 3);
            start_q(u, VU_SQRTF(fabsf(t)), DIV_LATENCY, t < 0.0f ? 0x10 : 0);
            return -1;
        }
        case 0x3B:  /* WAITQ */
            if (u->q_ready > u->cycle)
                u->cycle = u->q_ready;
            update_q(u);
            return -1;
        case 0x3C:  /* MTIR it, fs.fsf */
            stall_for(u, is);
            set_ialu(u, it, vf_bits(u, is, w >> 21 & 3));
            return -1;
        case 0x3D: {  /* MFIR ft, is */
            uint32_t v[4];
            for (comp = 0; comp < 4; comp++)
                v[comp] = (uint32_t)(int32_t)(int16_t)vis;
            write_vf_bits(u, it, d, v, u->cycle + FMAC_LATENCY);
            return -1;
        }
        case 0x3E:  /* ILWR it, (is) */
            for (comp = 0; comp < 4; comp++)
                if (HAS(d, comp))
                    set_vi(u, it, lower_data(u, vis, data)->w[comp]);
            return -1;
        case 0x3F:  /* ISWR it, (is) */
            for (comp = 0; comp < 4; comp++)
                if (HAS(d, comp))
                    lower_data(u, vis, data)->w[comp] = vit;
            return -1;
        case 0x40: case 0x41: {  /* RNEXT / RGET ft */
            uint32_t v[4];
            if (!(sub & 3)) {
                uint32_t x = c->r >> 4 & 1, y = c->r >> 22 & 1;
                c->r = (((c->r << 1) ^ x ^ y) & 0x7FFFFFu) | 0x3F800000u;
            }
            for (comp = 0; comp < 4; comp++)
                v[comp] = c->r;
            write_vf_bits(u, it, d, v, u->cycle + FMAC_LATENCY);
            return -1;
        }
        case 0x64: {  /* MFP ft, P (the old P remains readable while EFU runs) */
            if (!u->unit) break;
            uint32_t v[4], p;
            if (u->p_ready && u->p_ready <= u->cycle) {
                c->p = u->p_next; u->p_ready = 0;
            }
            memcpy(&p, &c->p, 4);
            for (comp = 0; comp < 4; comp++) v[comp] = p;
            write_vf_bits(u, it, d, v, u->cycle + FMAC_LATENCY);
            return -1;
        }
        case 0x68: case 0x69:  /* XTOP / XITOP */
            if (!u->unit) break;
            set_vi(u, it, (sub & 3) ? u->itop : u->top);
            return -1;
        case 0x6C:  /* XGKICK */
            if (!u->unit) break;
#ifndef PS2_VU_REFERENCE
            if (kick_trace) { flags_now(u); kick_trace(pc * 8, u->clip, vis & 1023); }
#endif
            if (u->kick && u->kick(u->opaque, vis & 1023)) *bad = 1;
            return -1;
        case 0x7B:  /* WAITP */
            if (!u->unit) break;
            if (u->p_ready > u->cycle) u->cycle = u->p_ready;
            if (u->p_ready) { c->p = u->p_next; u->p_ready = 0; }
            return -1;
        case 0x70: case 0x71: case 0x72: case 0x73:  /* ESADD/ERSADD/ELENG/ERLENG */
        case 0x76: case 0x78: case 0x79: case 0x7A: { /* ESUM/ESQRT/ERSQRT/ERCPR */
            if (!u->unit) break;
            unsigned efu = (FD(w) << 2) | (sub & 3), latency = 12;
            float p, x, y, z;
            stall_for(u, is);
            if (u->p_ready) {
                if (u->p_ready > u->cycle) u->cycle = u->p_ready;
                c->p = u->p_next;
            }
            x = vf(u, is, 0); y = vf(u, is, 1); z = vf(u, is, 2);
            if (efu <= 0x73) {
                float xx = VU_STEP(u, VU_MULF(x, x)), yy = VU_STEP(u, VU_MULF(y, y)), zz = VU_STEP(u, VU_MULF(z, z));
                p = VU_STEP(u, VU_ADDF(VU_STEP(u, VU_ADDF(xx, yy)), zz));
                latency = efu == 0x70 ? 11 : efu == 0x73 ? 24 : 18;
                if (efu == 0x72 || efu == 0x73) p = VU_SQRTF(p);
                if ((efu == 0x71 || efu == 0x73) && p != 0) p = VU_STEP(u, VU_DIVF(1.0f, p));
            } else if (efu == 0x76)
                p = VU_STEP(u, VU_ADDF(VU_STEP(u, VU_ADDF(VU_STEP(u, VU_ADDF(VU_DAZ(x), VU_DAZ(y))), VU_DAZ(z))), VU_DAZ(vf(u, is, 3))));
            else {
                p = vf(u, is, w >> 21 & 3);
                if ((efu == 0x78 || efu == 0x79) && VU_DAZ(p) >= 0) p = VU_SQRTF(VU_DAZ(p));
                if ((efu == 0x79 || efu == 0x7A) && VU_DAZ(p) != 0) p = VU_STEP(u, VU_DIVF(1.0f, p));
                if (efu == 0x79) latency = 18;
            }
            u->p_next = p; u->p_ready = u->cycle + latency;
            return -1;
        }
        case 0x42:  /* RINIT R, fs.fsf */
            stall_for(u, is);
            c->r = 0x3F800000u | (vf_bits(u, is, w >> 21 & 3) & 0x7FFFFFu);
            return -1;
        case 0x43:  /* RXOR R, fs.fsf */
            stall_for(u, is);
            c->r = 0x3F800000u | ((c->r ^ vf_bits(u, is, w >> 21 & 3)) & 0x7FFFFFu);
            return -1;
        }
        *bad = 1;
        return -1;
    }
    /* Integer branches have a one-cycle bypass delay, unlike other VI readers. */
    if (u->unit && op >= 0x28 && op <= 0x2F && u->vi_previous_reg && u->vi_written_at + 1 == u->cycle) {
        if ((unsigned)(is & 15) == u->vi_previous_reg) vis = u->vi_previous_value;
        if ((unsigned)(it & 15) == u->vi_previous_reg) vit = u->vi_previous_value;
    }
    switch (op) {
    case 0x00: {  /* LQ ft, imm(is) */
        write_vf_bits(u, it, d, lower_data(u, (uint32_t)(vis + imm11), data)->w, u->cycle + FMAC_LATENCY);
        return -1;
    }
    case 0x01: {  /* SQ fs, imm(it) */
        ps2_q128 *m;
        stall_for(u, is);
        m = lower_data(u, (uint32_t)(vit + imm11), data);
        store_fields(m->w, _mm_castps_si128(vf4(u, is)), d);
        return -1;
    }
    case 0x04:  /* ILW it, imm(is) */
        for (comp = 0; comp < 4; comp++)
            if (HAS(d, comp))
                set_vi(u, it, lower_data(u, (uint32_t)(vis + imm11), data)->w[comp]);
        return -1;
    case 0x05:  /* ISW it, imm(is) */
        for (comp = 0; comp < 4; comp++)
            if (HAS(d, comp))
                lower_data(u, (uint32_t)(vis + imm11), data)->w[comp] = vit;
        return -1;
    case 0x08: case 0x09: {  /* IADDIU / ISUBIU it, is, imm15 */
        uint32_t imm15 = (w >> 10 & 0x7800) | (w & 0x7FF);
        set_ialu(u, it, op == 0x08 ? vis + imm15 : vis - imm15);
        return -1;
    }
    case 0x10: flags_now(u); set_vi(u, 1, (u->clip & 0xFFFFFF) == (w & 0xFFFFFF)); return -1;          /* FCEQ */
    case 0x11: { /* FCSET uses the same four-cycle flag pipeline as CLIP. */
        Upper flag = {0}; flag.flags = 3; flag.clip_bits = w & 0xFFFFFF;
        queue_flags(u, &flag); return -1;
    }
    case 0x12: flags_now(u); set_vi(u, 1, (u->clip & w & 0xFFFFFF) != 0); return -1;                   /* FCAND */
    case 0x13: flags_now(u); set_vi(u, 1, ((u->clip | w) & 0xFFFFFF) == 0xFFFFFF); return -1;         /* FCOR */
    case 0x1C: flags_now(u); set_vi(u, it, u->clip & 0xFFF); return -1;                                 /* FCGET */
    case 0x14: case 0x15: case 0x16: case 0x17: {  /* FSEQ / FSSET / FSAND / FSOR */
        uint32_t imm12 = (w >> 10 & 0x800) | (w & 0x7FF);
        flags_now(u);
        if (op == 0x14)
            set_vi(u, it, (u->status & 0xFFF) == imm12);
        else if (op == 0x15)
            u->status = (u->status & 0x3F) | (imm12 & 0xFC0);
        else if (op == 0x16)
            set_vi(u, it, u->status & imm12);
        else
            set_vi(u, it, (u->status | imm12) & 0xFFF);
        return -1;
    }
    case 0x18: flags_now(u); set_vi(u, it, (u->mac & 0xFFFF) == vis); return -1;  /* FMEQ */
    case 0x1A: flags_now(u); set_vi(u, it, u->mac & vis); return -1;              /* FMAND */
    case 0x1B: flags_now(u); set_vi(u, it, u->mac | vis); return -1;              /* FMOR */
    case 0x20: return target;                                      /* B */
    case 0x21: set_vi(u, it, pc + 2); return target;              /* BAL */
    case 0x24: return vis;                                         /* JR */
    case 0x25: set_vi(u, it, pc + 2); return vis;                 /* JALR */
    case 0x28: return vit == vis ? target : -1;                    /* IBEQ */
    case 0x29: return vit != vis ? target : -1;                    /* IBNE */
    case 0x2C: return (int16_t)vis < 0 ? target : -1;              /* IBLTZ */
    case 0x2D: return (int16_t)vis > 0 ? target : -1;              /* IBGTZ */
    case 0x2E: return (int16_t)vis <= 0 ? target : -1;             /* IBLEZ */
    case 0x2F: return (int16_t)vis >= 0 ? target : -1;             /* IBGEZ */
    }
    *bad = 1;
    return -1;
}

/* Both halves issue together, after all source and divider stalls. Disjoint
   fields of a VF register do not stall each other. */
static inline __attribute__((always_inline)) int lower(Vu *u, uint32_t w, unsigned pc, int *bad)
{
    return lower_cached(u, w, pc, bad, NULL);
}

static inline __attribute__((always_inline)) void read_fields(Vu *u, unsigned reg, unsigned fields)
{
    if (!reg) return;
    for (unsigned c = 0; c < 4; c++)
        if (HAS(fields, c) && u->component_ready[reg][c] > u->cycle)
            u->cycle = u->component_ready[reg][c];
}

static inline __attribute__((always_inline)) void pair_stalls(Vu *u, uint32_t hi, uint32_t lo)
{
    unsigned op = hi & 63, sub = FD(hi) * 4 + (op & 3), d = DEST(hi);
    if (!(op >= 60 && sub == 0x2f)) {
        if (op >= 60 && sub == 0x1f) {
            read_fields(u, FS(hi), 14); read_fields(u, FT(hi), 1);
        } else {
            read_fields(u, FS(hi), d);
            if (op < 28) read_fields(u, FT(hi), 8 >> (op & 3));
            else if (op >= 40 && op < 48) read_fields(u, FT(hi), d);
            else if (op >= 60 && (sub < 16 || (sub >= 24 && sub < 28)))
                read_fields(u, FT(hi), 8 >> (op & 3));
            else if (op >= 60 && sub >= 40 && sub <= 46) read_fields(u, FT(hi), d);
        }
    }
    if (hi >> 31) return;
    op = lo >> 25; d = DEST(lo);
    if (op == 1) read_fields(u, FS(lo), d);
    if (op != 0x40 || (lo & 63) < 60) return;
    sub = FD(lo) * 4 + (lo & 3);
    switch (sub) {
    case 0x30: case 0x35: case 0x37: read_fields(u, FS(lo), d); break;
    case 0x31: read_fields(u, FS(lo), (d >> 1) | ((d & 1) << 3)); break;
    case 0x38: case 0x3a:
        read_fields(u, FS(lo), 8 >> (lo >> 21 & 3));
        /* fall through */
    case 0x39:
        read_fields(u, FT(lo), 8 >> (lo >> 23 & 3));
        /* fall through */
    case 0x3b:
        if (u->q_ready > u->cycle) u->cycle = u->q_ready;
        break;
    case 0x3c: case 0x42: case 0x43:
        read_fields(u, FS(lo), 8 >> (lo >> 21 & 3)); break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x76:
        read_fields(u, FS(lo), sub == 0x76 ? 15 : 14);
        goto efu;
    case 0x78: case 0x79: case 0x7a:
        read_fields(u, FS(lo), 8 >> (lo >> 21 & 3));
        /* fall through */
    case 0x7b:
    efu:
        if (u->p_ready) {
            if (u->p_ready - 1 > u->cycle) u->cycle = u->p_ready - 1;
            u->c->p = u->p_next; u->p_ready = 0;
        }
        break;
    }
}

static inline void trace_pair(Vu *u, unsigned pc)
{
#ifdef PS2_VU_TRACE
    if (getenv("DHD_VU_TRACE")) {
        fprintf(stderr, "N %04X %llu", pc * 8, (unsigned long long)u->cycle);
        for (unsigned v = 1; v < 32; v++) for (unsigned c = 0; c < 4; c++) fprintf(stderr, " %08X", u->c->vf[v].w[c]);
        fprintf(stderr, "\n");
    }
#else
    (void)u; (void)pc;
#endif
}

/* Both callers select a constant unit. Specialize the shared interpreter so
   VU1 does not test the VU0 arithmetic/pipeline mode in every component op. */
#ifdef PS2_VU_AOT
/* The blocks run VU1 only. Stores through u->c can alias u, so restate the unit before each step:
   the helpers then drop their VU0 paths (scalar lanes, stall checks) instead of testing it. */
#define VU1(u) do { if ((u)->unit != 1) __builtin_unreachable(); } while (0)
/* The VU0 blocks (aot0_*) likewise state unit 0 for the helpers. */
#define VU0(u) do { if ((u)->unit != 0) __builtin_unreachable(); } while (0)
#include "vu1_aot.inc"
#if !defined(PS2_VU_GEOMETRY_BENCH) && !defined(PS2_VU_REFERENCE)
#include "vu1_native_clip.inc"
#include "vu1_native_geometry.h"
#include "vu1_native_extra.h"
#endif
#endif

#if defined(PS2_VU_AOT) && !defined(PS2_VU_GEOMETRY_BENCH) && !defined(PS2_VU_REFERENCE)
/* The native kernels run only where micro memory holds the exact words they were written from.
   In the game's VU1 micro memory (0x11008000, changed only by VIF1 MPG, which bumps
   ps2_vu1_code_version) the answer is kept until the next change, as aot_find does; any other
   code buffer (the replay tests) is compared each time. */
static int native_code_compare(const uint8_t *code, unsigned kind, unsigned start, unsigned end)
{
    const uint32_t *words = kind == 8192 ? native_loop3140_words : kind == 4096 ? native_loop0798_words : kind == 2048 ? native_loop2e48_words : kind == 1024 ? native_loop0e10_words : kind == 512 ? native_loop2dc8_words : kind == 256 ? native_loop1430_words : kind == 128 ? native_loop468_words : kind == 64 ? native_loop800_words : kind == 16 ? native_loop20c_words :
                            kind == 32 ? native_loop260_words : kind == 4 ? native_loop1638_words :
                            kind == 8 ? native_loop17e0_words : native_loop_words;
    /* 1430's signature includes its short exit through 1630. Bulk flags have
       already retired: neither that tail nor the common GIF tail may gain an
       earlier MAC/status read without making this kernel decline. */
    size_t bytes = kind == 256 ? sizeof native_loop1430_words : end - start;
    if (memcmp(code + start, words, bytes)) return 0;
    if ((kind == 256 || kind == 512) && memcmp(code + 0xff8, native_giftail_words, sizeof native_giftail_words)) return 0;
    /* 2DC8 also carries a pending Q through its own branch and delay slot. */
    if (kind == 512 && memcmp(code + 0x2fc8, native_tail2dc8_words, sizeof native_tail2dc8_words)) return 0;
    if (kind == 1024 && memcmp(code + 0xfd0, native_tail0e10_words, sizeof native_tail0e10_words)) return 0;
    if (kind == 2048 && memcmp(code + 0x30c0, native_tail2e48_words, sizeof native_tail2e48_words)) return 0;
    if (kind == 64 || kind == 128 || kind >= 1024) return 1;  /* no clipping path */
    if (kind <= 8 && (kind == 4 ? memcmp(code + 0x17c0, native_zero1638_words, sizeof native_zero1638_words) :
                      kind == 8 ? memcmp(code + 0x1938, native_zero17e0_words, sizeof native_zero17e0_words) :
                      memcmp(code + 0x270, native_zero_words, sizeof native_zero_words)))
        return 0;
    return !memcmp(code + 0x1948, native_clip1_words, sizeof native_clip1_words) &&
           !memcmp(code + 0x1cc0, native_clip2_words, sizeof native_clip2_words);
}

static int native_code_matches(const uint8_t *code, unsigned kind, unsigned start, unsigned end)
{
    static uint64_t checked[PS2_VU1_NATIVE_KINDS];  /* ps2_vu1_code_version + 1 */
    static uint8_t matched[PS2_VU1_NATIVE_KINDS];
    if (GADDR(code) != 0x11008000) return native_code_compare(code, kind, start, end);
    const unsigned at = (unsigned)__builtin_ctz(kind);
    if (checked[at] != ps2_vu1_code_version + 1) {
        matched[at] = (uint8_t)native_code_compare(code, kind, start, end);
        checked[at] = ps2_vu1_code_version + 1;
    }
    return matched[at];
}
#endif

#if defined(PS2_VU0_NATIVE) && !defined(PS2_VU_AOT)
/* The wasm module runs VU0 but not VU1: only the native VU0 routines' words
   (port/tools/vu0_native_words.py, from the same vu1_aot.inc), not the VU1 blocks. */
#include "vu0_native_words.inc"
#endif

#if defined(PS2_VU0_COMPILED) && !defined(PS2_VU_REFERENCE)
/* The compiled VU0 microprograms (port/tools/vu0_aot.py, finding 090 SP1): one C function per
   vcallms entry, with the interpreter's timing (stalls, the flag queue) resolved when it was
   generated. These are its operations, the VU0 path of upper() in vector form: operands through
   arithmetic4 (the per-lane arithmetic() clamp), one operation each, MADD/MSUB as ACC +- (a * b).
   opaque: both operands are known to the C compiler (VF0, LOI constants), which must not fold the
   operation in its own rounding mode. */
#define VU0C_VF0 _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f)
#define VU0C_INLINE static inline __attribute__((always_inline))
VU0C_INLINE __m128 vu0c_lanes(int d)
{
    return _mm_castsi128_ps(_mm_setr_epi32(d & 8 ? -1 : 0, d & 4 ? -1 : 0, d & 2 ? -1 : 0, d & 1 ? -1 : 0));
}
VU0C_INLINE __m128 vu0c_blend(__m128 old, __m128 v, int d)
{
    const __m128 m = vu0c_lanes(d);
    return _mm_or_ps(_mm_and_ps(m, v), _mm_andnot_ps(m, old));
}
VU0C_INLINE void vu0c_store(float *p, __m128 v, int d) { store_fields(p, _mm_castps_si128(v), d); }
VU0C_INLINE __m128 vu0c_add(__m128 a, __m128 b, int opaque)
{
    if (opaque) PS2_OPAQUE_M128(a);
    return _mm_add_ps(arithmetic4(a), arithmetic4(b));
}
VU0C_INLINE __m128 vu0c_sub(__m128 a, __m128 b, int opaque)
{
    if (opaque) PS2_OPAQUE_M128(a);
    return _mm_sub_ps(arithmetic4(a), arithmetic4(b));
}
VU0C_INLINE __m128 vu0c_mul(__m128 a, __m128 b, int opaque)
{
    if (opaque) PS2_OPAQUE_M128(a);
    return _mm_mul_ps(arithmetic4(a), arithmetic4(b));
}
VU0C_INLINE __m128 vu0c_madd(__m128 acc, __m128 a, __m128 b, int opaque)
{
    if (opaque) PS2_OPAQUE_M128(a);
    return _mm_add_ps(arithmetic4(acc), _mm_mul_ps(arithmetic4(a), arithmetic4(b)));
}
VU0C_INLINE __m128 vu0c_msub(__m128 acc, __m128 a, __m128 b, int opaque)
{
    if (opaque) PS2_OPAQUE_M128(a);
    return _mm_sub_ps(arithmetic4(acc), _mm_mul_ps(arithmetic4(a), arithmetic4(b)));
}
/* ITOF and FTOI: upper()'s VU0 expressions, lane by lane. */
VU0C_INLINE __m128 vu0c_itof(__m128 a, int bc, int opaque)
{
    static const float scale[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
    uint32_t bits[4];
    float r[4];
    if (opaque) PS2_OPAQUE_M128(a);
    _mm_storeu_ps((float *)bits, a);
    for (int comp = 0; comp < 4; comp++)
        r[comp] = (float)(int32_t)bits[comp] / scale[bc];
    return _mm_loadu_ps(r);
}
VU0C_INLINE __m128 vu0c_ftoi(__m128 a, int bc)
{
    static const int shift[4] = {0, 4, 12, 15};
    float s[4];
    uint32_t r[4];
    _mm_storeu_ps(s, a);
    for (int comp = 0; comp < 4; comp++) {
        double x = (double)s[comp] * (double)(1 << shift[bc]);
        int32_t v = x >= 2147483647.0 ? 2147483647 : x <= -2147483648.0 ? INT32_MIN : (int32_t)x;
        r[comp] = (uint32_t)v;
    }
    return _mm_loadu_ps((const float *)r);
}
VU0C_INLINE uint32_t vu0c_lane_bits(__m128 v, int comp)
{
    uint32_t bits[4];
    _mm_storeu_ps((float *)bits, v);
    return bits[comp];
}
/* The MAC flags of an FMAC result (fmac_flags4: Z and S of the fields in d) and the status bits
   they imply. */
VU0C_INLINE unsigned vu0c_mac(__m128 v, int d)
{
    Upper up;
    fmac_flags4(&up, v, d);
    return up.mac;
}
VU0C_INLINE unsigned vu0c_status(unsigned mac)
{
    return (mac & 0xF ? 1u : 0) | (mac & 0xF0 ? 2u : 0) | (mac & 0xF00 ? 4u : 0) | (mac & 0xF000 ? 8u : 0);
}
/* Every FMAC's sticky Z and S bits, which the end of the run folds into the status flag. */
#define VU0C_STICKY(v, d) \
    (sticky_zero = _mm_or_ps(sticky_zero, _mm_and_ps(_mm_cmpeq_ps((v), _mm_setzero_ps()), vu0c_lanes(d))), \
     sticky_sign = _mm_or_ps(sticky_sign, _mm_and_ps((v), vu0c_lanes(d))))
#define VU0C_STICKY_BITS() \
    ((_mm_movemask_ps(sticky_zero) ? 0x40u : 0u) | (_mm_movemask_ps(sticky_sign) ? 0x80u : 0u))
#include "vu0_aot.inc"
#ifdef PS2_VU_AOT_EMBED
static int vu0c_loaded = 1;
#else
static int vu0c_loaded;  /* the words came from the game's .vutext (ps2_vu_aot_load) */
#endif
int ps2_vu0_compiled_mode = -1;
unsigned long long ps2_vu0_compiled_runs;

static int vu0_compiled_compare(const ps2_cpu *cpu, unsigned k)
{
    for (unsigned i = 0; i < vu0c_programs[k].n_runs; i++) {
        const unsigned *run = vu0c_programs[k].runs[i];
        if (memcmp(cpu->vu0_code + run[0] * 8, vu0c_programs[k].words + run[2], run[1] * 8)) return 0;
    }
    return 1;
}

/* The compiled program for a vcallms at addr, or -1. It runs only where micro memory holds the
   exact words of every pair it can reach: for the game's VU0 micro memory the answer is kept per
   ps2_vu0_code_version (as vu0_native_code does), other code buffers (the tests) compare each time. */
static int vu0_compiled_find(const ps2_cpu *cpu, unsigned addr)
{
    static signed char index[512];
    static int indexed, verify = -1;
    static uint64_t checked[VU0C_PROGRAMS];  /* ps2_vu0_code_version + 1 */
    static uint8_t matched[VU0C_PROGRAMS];
    if (!indexed) {
        memset(index, -1, sizeof index);
        for (unsigned k = 0; k < VU0C_PROGRAMS; k++) index[vu0c_programs[k].entry / 8] = (signed char)k;
        indexed = 1;
    }
    const int k = index[(addr / 8) & 511];
    if (k < 0 || !vu0c_loaded || cpu->vi[0]) return -1;
    if (GADDR(cpu->vu0_code) != 0x11000000u) return vu0_compiled_compare(cpu, (unsigned)k) ? k : -1;
    if (checked[k] != ps2_vu0_code_version + 1) {
        matched[k] = (uint8_t)vu0_compiled_compare(cpu, (unsigned)k);
        checked[k] = ps2_vu0_code_version + 1;
    }
    if (verify < 0) { const char *value = getenv("DHD_VU0_CODE_CHECK"); verify = value && atoi(value); }
    if (verify) {
        const uint8_t now = (uint8_t)vu0_compiled_compare(cpu, (unsigned)k);
        if (now != matched[k]) {
            fprintf(stderr, "vu0: micro memory changed without VIF0 MPG (compiled %04X: %d -> %d)\n",
                    vu0c_programs[k].entry, matched[k], now);
            matched[k] = now;
        }
    }
    return matched[k] ? k : -1;
}
#endif

void ps2_vu_aot_load(const void *vutext)
{
#if defined(PS2_VU0_COMPILED) && !defined(PS2_VU_REFERENCE) && !defined(PS2_VU_AOT_EMBED)
    vu0c_words_load(vutext);
    vu0c_loaded = 1;
#endif
#if defined(PS2_VU_AOT) && !defined(PS2_VU_AOT_EMBED)
    aot_words_load(vutext);
#if !defined(PS2_VU_GEOMETRY_BENCH) && !defined(PS2_VU_REFERENCE)
    native_words_load(vutext);
#endif
#elif defined(PS2_VU0_NATIVE)
    vu0_native_words_load(vutext);
#else
    (void)vutext;
#endif
#ifndef PS2_VU_REFERENCE
    ps2_vu0_code_version++;  /* the native VU0 routines' words changed: match them again */
#endif
}

static inline __attribute__((always_inline)) int run(ps2_cpu *cpu, unsigned addr, unsigned unit, unsigned top, unsigned itop,
               ps2_vu_kick_fn kick, void *opaque)
{
    Vu u PS2_NO_AUTO_INIT;  /* the prefix is cleared below; vu1_file is filled before use (vu1_used) */
    unsigned mask = unit ? 0x7FF : 0x1FF;
    unsigned pc = (addr / 8) & mask;
    int branch = -1, ending = 0;
    long steps;
    unsigned history[16] = {0}, dispatched = 0;  /* the last 16 pairs or blocks run */
#ifdef PS2_VU_AOT
    static int interpreter_only = -1, vu0_aot = -1;
    if (unit && interpreter_only < 0) interpreter_only = getenv("DHD_VU_INTERPRETER") != NULL;
    /* DHD_VU0_AOT=1: VU0 microprograms (vcallms) through the translated blocks. Off by default
       until port/runtime/vu0_aot_diff.c and a race gate have checked them against the interpreter. */
    if (!unit && vu0_aot < 0) {
        const char *value = getenv("DHD_VU0_AOT");
        vu0_aot = value && atoi(value) && !getenv("DHD_VU_INTERPRETER");
    }
#ifndef PS2_VU_REFERENCE
    if (!unit && ps2_vu0_aot_mode >= 0) vu0_aot = ps2_vu0_aot_mode;
#endif
#ifndef PS2_VU_REFERENCE
    const int use_aot = unit ? (ps2_vu1_aot_mode >= 0 ? ps2_vu1_aot_mode : !interpreter_only) : vu0_aot;
#else
    const int use_aot = unit ? !interpreter_only : vu0_aot;
#endif
#if !defined(PS2_VU_GEOMETRY_BENCH) && !defined(PS2_VU_REFERENCE)
    static int native_enabled = -1;
    static unsigned native_mask;
    if (native_enabled < 0) {
        const char *value = getenv("DHD_VU1_NATIVE");
        native_enabled = (!value || atoi(value)) && !getenv("DHD_VU_TRACE");
        const char *mask = getenv("DHD_VU1_NATIVE_MASK");
        native_mask = mask ? (unsigned)strtoul(mask, NULL, 0) : (1u << PS2_VU1_NATIVE_KINDS) - 1;
    }
    unsigned native_kind = 0;
    /* The mesh programs' dispatch `JR vi13` at 1040, 1CB0 and 12E0 (B0040's MSCNT continuation,
       finding 090 VU1R): each kernel starts at its loop's first pair from the live state, so the
       entry that led there does not matter. */
    if (use_aot && (ps2_vu1_native_mode >= 0 ? ps2_vu1_native_mode : native_enabled) && (addr == 0x1040 || addr == 0x1cb0 || addr == 0x12e0)) {
        if (cpu->vi[13] == 0x11) native_kind = addr == 0x1040 ? 1 : addr == 0x1cb0 ? 2 : 0;
        else if (cpu->vi[13] == 0x2c9) native_kind = 4;
        else if (cpu->vi[13] == 0x2fe) native_kind = 8;
        else if (cpu->vi[13] == 0x20c) native_kind = 16;
        else if (cpu->vi[13] == 0x260) native_kind = 32;
        else if (cpu->vi[13] == 0x100) native_kind = 64;
        else if (cpu->vi[13] == 0x288) native_kind = 256;
        else if (cpu->vi[13] == 0x5bd) native_kind = 512;
        else if (cpu->vi[13] == 0x0e0) native_kind = 4096;
        else if (cpu->vi[13] == 0x616) native_kind = 8192;
    }
    /* MSCAL 0010/0020 branch to the skinning setup at 03C8 (overlay 2), then its loop. */
    if (use_aot && (ps2_vu1_native_mode >= 0 ? ps2_vu1_native_mode : native_enabled) && (addr == 0x10 || addr == 0x20))
        native_kind = 128;
    if (unit && use_aot && (ps2_vu1_native_mode >= 0 ? ps2_vu1_native_mode : native_enabled) && addr == 0xe10) native_kind = 1024;
    if (unit && use_aot && (ps2_vu1_native_mode >= 0 ? ps2_vu1_native_mode : native_enabled) && addr == 0x2e48) native_kind = 2048;
    const int native_geometry = (native_kind & native_mask) != 0;
    unsigned native_start = native_kind == 8192 ? 0x3140 : native_kind == 4096 ? 0x798 : native_kind == 2048 ? 0x2f90 : native_kind == 1024 ? 0xee0 : native_kind == 512 ? 0x2f00 : native_kind == 256 ? 0x1510 : native_kind == 128 ? 0x468 : native_kind == 64 ? 0x8b0 : native_kind == 16 ? 0x1110 : native_kind == 32 ? 0x13a0 : native_kind == 4 ? 0x1708 : native_kind == 8 ? 0x18b0 : 0x170;
    unsigned native_end = native_kind == 8192 ? 0x3180 : native_kind == 4096 ? 0x7e0 : native_kind == 2048 ? 0x30c0 : native_kind == 1024 ? 0xfd0 : native_kind == 512 ? 0x2fc8 : native_kind == 256 ? 0x1628 : native_kind == 128 ? 0x578 : native_kind == 64 ? 0x938 : native_kind == 16 ? 0x11d8 : native_kind == 32 ? 0x1420 : native_kind == 4 ? 0x17b0 : native_kind == 8 ? 0x1928 : 0x260;
    static unsigned long long native_target_calls;
    static unsigned long long native_key_targets[PS2_VU1_NATIVE_KINDS], native_key_calls[PS2_VU1_NATIVE_KINDS],
                              native_key_vertices[PS2_VU1_NATIVE_KINDS], native_key_clipped[PS2_VU1_NATIVE_KINDS];
    const unsigned native_index = native_kind ? (unsigned)__builtin_ctz(native_kind) : 0;
    if (native_geometry) native_target_calls++;
    if (native_geometry) native_key_targets[native_index]++;
#endif
#endif
    memset(&u, 0, offsetof(Vu, vu1_file));  /* about 60k VU0 and 30k VU1 runs a second in a race */
    u.c = cpu;
    u.unit = unit; u.code_mask = mask; u.data_mask = unit ? 0x3FF : 0xFF;
    u.top = top; u.itop = itop; u.kick = kick; u.opaque = opaque;
    u.status = cpu->vu0_ctrl[0] & 0xFFF;
    u.mac = cpu->vu0_ctrl[1] & 0xFFFF;
    u.clip = u.clip_next = cpu->vu0_ctrl[2] & 0xFFFFFF;
    for (steps = 0; steps < MAX_STEPS; steps++) {
#ifdef PS2_VU_GEOMETRY_BENCH
        /* Test-only capture of a pipeline boundary, absent from game builds. */
        extern int vu_geometry_capture(void *, unsigned);
        if (vu_geometry_capture(&u, pc)) return 0;
#endif
#if defined(PS2_VU_AOT) && !defined(PS2_VU_GEOMETRY_BENCH) && !defined(PS2_VU_REFERENCE)
        unsigned long long before_vertices = native_vertices, before_clipped = native_clipped_vertices;
        unsigned native_count = 0;
        if (native_geometry && pc == native_start / 8 && branch < 0 && !ending &&
            steps < MAX_STEPS - (native_kind >= 256 ? 262144 : 16384) &&
            native_code_matches(cpu->vu0_code, native_kind, native_start, native_end) &&
            ((native_count = native_kind == 8192 ? native_geometry_3140(&u) : native_kind == 4096 ? native_geometry_0798(&u) :
                             native_kind == 2048 ? native_particles_2e48(&u) : native_kind == 1024 ? native_particles_0e10(&u) : native_kind == 512 ? native_geometry_2dc8(&u) : native_kind == 256 ? native_geometry_1430(&u) : native_kind == 128 ? native_skin_468(&u) : native_kind == 64 ? native_geometry_800(&u) : native_kind == 16 ? native_geometry_20c(&u) :
                             native_kind == 32 ? native_geometry_260(&u) : native_kind == 4 ? native_geometry_1638(&u) :
                             native_kind == 8 ? native_geometry_17e0(&u) : native_geometry_loop(&u)) ||
             native_kind < 64)) {  /* 08B0 and 0468 decline before writing anything: then they run pair by pair */
            unsigned count = native_count;
            if (!count || count > (unsigned)(MAX_STEPS - steps)) {
                fprintf(stderr, "native geometry failed after direct writes\n");
                return -1; /* No rollback after an unsupported direct-write path. */
            }
            static unsigned long long native_calls;
            native_calls++;
            ps2_vu1_native_runs[native_index]++;
            native_key_calls[native_index]++;
            native_key_vertices[native_index] += native_vertices-before_vertices;
            native_key_clipped[native_index] += native_clipped_vertices-before_clipped;
            if ((native_key_calls[native_index] < 3 || !(native_key_calls[native_index] % 4096)) && getenv("DHD_VU1_NATIVE_STATS"))
                fprintf(stderr, "native key=%u calls=%llu target_calls=%llu vertices=%llu clipped=%llu\n", native_kind,
                        native_key_calls[native_index], native_key_targets[native_index], native_key_vertices[native_index], native_key_clipped[native_index]);
            if ((native_calls < 3 || !(native_calls % 4096)) && getenv("DHD_VU1_NATIVE_STATS"))
                fprintf(stderr, "native geometry calls=%llu target_calls=%llu vertices=%llu "
                        "clipped=%llu rollback=0 fallback=0\n", native_calls, native_target_calls,
                        native_vertices, native_clipped_vertices);
            steps += count - 1;
            pc = native_end / 8;
            continue;
        }
#endif
        history[dispatched++ & 15] = pc * 8;
        const uint32_t *pair = (const uint32_t *)(cpu->vu0_code + pc * 8);
        uint32_t lo, hi;
        int next = -1, bad = 0;
        Upper up;
#ifdef PS2_VU_AOT
        /* Blocks start without a pending transition. Simple branch/E slots
           may be fused; a pending transition still selects a one-pair block. */
        const AotBlock *translated = !use_aot ? NULL : unit ? aot_find(pc, pair, MAX_STEPS - steps, branch >= 0 || ending)
                                                            : aot0_find(pc, pair, MAX_STEPS - steps, branch >= 0 || ending);
        if (translated) {
            next = translated->run(&u, pc, &bad);
            if (bad) return -1;
#ifdef PS2_VU_COVERAGE
            if (unit) ps2_vu1_translated += translated->count;
#endif
            steps += translated->count - 1;
            pc += translated->count - 1;
            lo = translated->words[translated->count * 2 - 2];
            hi = translated->words[translated->count * 2 - 1];
        } else {
#endif
        lo = pair[0], hi = pair[1];
#ifdef PS2_VU_COVERAGE
        if (unit) ps2_vu1_interpreted[pc]++;
#endif
        if (unit) pair_stalls(&u, hi, lo);
        apply_flags(&u);
        update_q(&u);
        if (upper(&u, hi, &up) < 0) {
            fprintf(stderr, "vu%u: unknown upper instruction %08X at 0x%04X\n", unit, hi, pc * 8);
            return -1;
        }
        if (BIT(hi, 31))  /* I bit: the lower word is a float for I */
            memcpy(&cpu->i, &lo, 4);
        else if (!(up.flags == 2 && (lo >> 25) == 0x11))
            next = lower(&u, lo, pc, &bad);
        if (bad) {
            fprintf(stderr, "vu%u: failed lower instruction %08X at 0x%04X\n", unit, lo, pc * 8);
            return -1;
        }
        commit_upper(&u, &up, FT(lo));
#ifdef PS2_VU_AOT
        }
#endif
        trace_pair(&u, pc);
        u.cycle++;
#ifdef PS2_VU_AOT
        if (translated && translated->fused) {
            pc = next >= 0 ? (unsigned)next & mask : (pc + 1) & mask;
            if (translated->fused == 2) break;
            continue;
        }
#endif
        if (ending) {
            pc = branch >= 0 ? (unsigned)branch & mask : (pc + 1) & mask;
            break;
        }
        if (BIT(hi, 30))  /* E bit: the next pair runs, then the program ends */
            ending = 1;
        if (branch >= 0) {
            pc = (unsigned)branch & mask;
            branch = -1;
        } else
            pc = (pc + 1) & mask;
        if (next >= 0)
            branch = next;
    }
    if (steps == MAX_STEPS) {
        fprintf(stderr, "vu%u: microprogram at 0x%04X didn't end\n", unit, addr);
        fprintf(stderr, "last PCs:");
        for (unsigned n = 0; n < 16; n++) fprintf(stderr, " %04X", history[(dispatched + n) & 15]);
        fprintf(stderr, "\nVI:");
        for (unsigned n = 0; n < 16; n++) fprintf(stderr, " %04X", cpu->vi[n]);
        fprintf(stderr, "\n");
        return -1;
    }
    if (u.q_ready) {  /* a division still running finishes */
        cpu->q = u.q_next;
        if (unit) u.status = (u.status & ~0x30u) | u.q_flags;
    }
    if (u.p_ready) cpu->p = u.p_next;
    cpu->vu0_ctrl[10] = pc * 8;
    drain_flags(&u);
    cpu->vu0_ctrl[0] = u.status;
    cpu->vu0_ctrl[1] = u.mac;
    cpu->vu0_ctrl[2] = u.clip;
    return 0;
}

/* DHD_VU0_CAPTURE=dir: save the input of interpreted VU0 microprograms (registers, both 4 KB
   memories and the VU1 VF/VI file VU0 sees at 0x4000) for port/tools/test_vu0_pcsx2.py.
   DHD_VU0_CAPTURE_AT delays it (seconds since start); DHD_VU0_CAPTURE_COUNT per entry, 8 by
   default. The files hold game microcode, so the directory must stay outside version control. */
static void capture(const ps2_cpu *cpu, unsigned addr)
{
    static int initialized; static const char *dir; static double after; static unsigned limit;
    static unsigned seen[512];
    if (!initialized) {
        const char *at = getenv("DHD_VU0_CAPTURE_AT"), *count = getenv("DHD_VU0_CAPTURE_COUNT");
        initialized = 1; dir = getenv("DHD_VU0_CAPTURE"); after = at ? atof(at) : 0;
        limit = count ? (unsigned)atoi(count) : 8;
    }
    if (!dir || (double)clock() / CLOCKS_PER_SEC < after || seen[(addr / 8) & 511] >= limit) return;
    unsigned n = seen[(addr / 8) & 511]++;
    char path[1024];
    snprintf(path, sizeof path, "%s/vu0-%04X-%u.bin", dir, addr, n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    ps2_q128 vu1_file[64];
    ps2_vu0_vu1_file(vu1_file);
    unsigned header[4] = {addr, sizeof *cpu, sizeof vu1_file, 0};
    fwrite(header, sizeof header, 1, f); fwrite(cpu, sizeof *cpu, 1, f);
    fwrite(cpu->vu0_code, 4096, 1, f); fwrite(cpu->vu0_mem, 4096, 1, f);
    fwrite(vu1_file, sizeof vu1_file, 1, f);
    fclose(f);
}

/* DHD_VU0_STATS=N: every N interpreted VU0 microprograms, the calls and time stamp counter
   ticks so far by entry address, on stderr (a diagnostic for choosing native routines). */
static void vu0_stats(unsigned addr, unsigned long long ticks)
{
    static long long every = -1;
    static unsigned long long calls[512], spent[512], total;
    if (every < 0) { const char *s = getenv("DHD_VU0_STATS"); every = s ? atoll(s) : 0; }
    if (!every) return;
    calls[addr / 8 & 511]++; spent[addr / 8 & 511] += ticks;
    if (++total % every) return;
#ifndef PS2_VU_REFERENCE
    fprintf(stderr, "vu0 native after %llu: 0000=%llu 02E0=%llu 05E8=%llu 07B8=%llu 0808=%llu\n", total,
            vu0_native_entries[0], vu0_native_entries[1], vu0_native_entries[2], vu0_native_entries[3], vu0_native_entries[4]);
#endif
    unsigned long long all = 0;
    for (unsigned i = 0; i < 512; i++) all += spent[i];
    for (unsigned i = 0; i < 512; i++)
        if (calls[i])
            fprintf(stderr, "vu0 stats after %llu: entry %04X %llu calls, %.1f%% of ticks, %.0f ticks a call\n", total, i * 8,
                    calls[i], 100.0 * spent[i] / all, (double)spent[i] / calls[i]);
}

#if (defined(PS2_VU_AOT) || defined(PS2_VU0_NATIVE)) && !defined(PS2_VU_REFERENCE)
#include "vu0_native.h"

/* The native VU0 routines run only where micro memory holds the exact words they were written from
   (a zero first word: the words never loaded). The game's VU0 micro memory (0x11000000) changes
   only through VIF0 MPG (vif.c), which bumps ps2_vu0_code_version, as ps2_vu_aot_load does when
   it fills the words; there each routine's answer is kept until the next change (finding 090
   OP2), instead of a byte-wise memcmp of up to 528 bytes on every vcallms. Any other code buffer
   (the tests) is compared each time. DHD_VU0_CODE_CHECK=1 compares every call as well and
   reports a change that came another way (an EE store into micro memory). */
static int vu0_native_code(const ps2_cpu *cpu, unsigned which, unsigned at, const uint32_t *words, size_t bytes)
{
    static uint64_t checked[5];  /* ps2_vu0_code_version + 1 */
    static uint8_t matched[5];
    static int verify = -1;
    if (GADDR(cpu->vu0_code) != 0x11000000u) return words[0] && !memcmp(cpu->vu0_code + at, words, bytes);
    if (checked[which] != ps2_vu0_code_version + 1) {
        matched[which] = (uint8_t)(words[0] && !memcmp(cpu->vu0_code + at, words, bytes));
        checked[which] = ps2_vu0_code_version + 1;
    }
    if (verify < 0) { const char *value = getenv("DHD_VU0_CODE_CHECK"); verify = value && atoi(value); }
    if (verify) {
        uint8_t now = (uint8_t)(words[0] && !memcmp(cpu->vu0_code + at, words, bytes));
        if (now != matched[which]) {
            fprintf(stderr, "vu0: micro memory at %04X changed without VIF0 MPG (native routine %u: %d -> %d)\n",
                    at, which, matched[which], now);
            matched[which] = now;
        }
    }
    return matched[which];
}
#endif

int ps2_vu0_run(ps2_cpu *cpu, unsigned addr)
{
    capture(cpu, addr);
    /* Timing is a launch-time diagnostic. Avoid two timestamp reads around
       short native routines when no statistics have been requested. */
    static int timing = -1;
    if (timing < 0) {
        const char *value = getenv("DHD_VU0_STATS");
        timing = value && atoll(value) != 0;
    }
    unsigned long long start = timing ? host_cycles() : 0;
    /* VU0 micro mode rounds like VU1 (PCSX2's default VU0FPCR is toward zero, FTZ/DAZ). The EE's
       own mode (fp_mode.c HOST_FP_EE) is the same word but for the sticky exception flags, which
       nothing reads: under it, and under a vcallms (ps2asm.c), the word stays as it is (finding
       090 SP15: two host calls per call). */
    unsigned control = host_fp_csr();
    const int set_mode = (control & ~0x3Fu) != 0xFFC0u;
    if (set_mode)
        host_fp_set_csr((control & ~0x603F) | 0xFFC0);
    int result;
#if defined(PS2_VU0_COMPILED) && !defined(PS2_VU_REFERENCE)
    /* The compiled microprograms (vu0_aot.inc), after the native routines. DHD_VU0_COMPILED=0,
       DHD_VU_INTERPRETER or DHD_VU_TRACE keep them off. */
    static int compiled = -1;
    if (compiled < 0) {
        const char *value = getenv("DHD_VU0_COMPILED");
        compiled = (!value || atoi(value)) && !getenv("DHD_VU_INTERPRETER") && !getenv("DHD_VU_TRACE");
    }
    const int use_compiled = ps2_vu0_compiled_mode >= 0 ? ps2_vu0_compiled_mode : compiled;
    int program;
#endif
#if (defined(PS2_VU_AOT) || defined(PS2_VU0_NATIVE)) && !defined(PS2_VU_REFERENCE)
    /* Native VU0 routines (vu0_native.h, finding 066). DHD_VU0_NATIVE=0, DHD_VU_INTERPRETER
       or DHD_VU_TRACE keep every program on the interpreter. */
    static int native = -1;
    if (native < 0) {
        const char *value = getenv("DHD_VU0_NATIVE");
        native = (!value || atoi(value)) && !getenv("DHD_VU_INTERPRETER") && !getenv("DHD_VU_TRACE");
    }
    const int use_native = ps2_vu0_native_mode >= 0 ? ps2_vu0_native_mode : native;
    if (use_native && addr == 0 && !cpu->vi[0] && vu0_native_code(cpu, 0, 0, vu0_cull_words, sizeof vu0_cull_words)) {
        native0_cull(cpu);
        ps2_vu0_native_runs++;
        vu0_native_entries[0]++;
        result = 0;
    } else if (use_native && addr == 0x5e8 && !cpu->vi[0] &&
               vu0_native_code(cpu, 2, 0x5e8, vu0_basis_words, sizeof vu0_basis_words)) {
        native0_transform_basis(cpu);
        ps2_vu0_native_runs++;
        vu0_native_entries[2]++;
        result = 0;
    } else if (use_native && addr == 0x2e0 && !cpu->vi[0] &&
               vu0_native_code(cpu, 1, 0x2e0, vu0_bounds_words, sizeof vu0_bounds_words)) {
        native0_bounds(cpu);
        ps2_vu0_native_runs++;
        vu0_native_entries[1]++;
        result = 0;
    } else if (use_native && addr == 0x7b8 && !cpu->vi[0] &&
               vu0_native_code(cpu, 3, 0x7b8, vu0_length_words, sizeof vu0_length_words)) {
        native0_length_squared(cpu);
        ps2_vu0_native_runs++;
        vu0_native_entries[3]++;
        result = 0;
    } else if (use_native && addr == 0x808 && !cpu->vi[0] &&
               vu0_native_code(cpu, 4, 0x808, vu0_direction_words, sizeof vu0_direction_words)) {
        native0_transform_direction(cpu);
        ps2_vu0_native_runs++;
        vu0_native_entries[4]++;
        result = 0;
    } else
#endif
#if defined(PS2_VU0_COMPILED) && !defined(PS2_VU_REFERENCE)
    if (use_compiled && (program = vu0_compiled_find(cpu, addr)) >= 0) {
        vu0c_run((unsigned)program, cpu);
        ps2_vu0_compiled_runs++;
        result = 0;
    } else
#endif
        result = run(cpu, addr, 0, 0, 0, NULL, NULL);
    if (set_mode)
        host_fp_set_csr(control);
    if (timing) vu0_stats(addr, host_cycles() - start);
    return result;
}

int ps2_vu1_run(ps2_cpu *cpu, unsigned addr, unsigned top, unsigned itop,
                ps2_vu_kick_fn kick, void *opaque)
{
    /* VU arithmetic truncates rather than rounding to nearest. Keep this local
       to the VU so the EE/host and the native VU0 routines (vu0_micro.c) retain theirs. */
#ifndef PS2_VU_IEEE
    unsigned control = host_fp_csr();
#endif
    /* Its FMACs intentionally overflow before the existing bitwise clamp.
       Keep host exceptions masked here even during EE first-fault diagnostics. */
#ifndef PS2_VU_IEEE  /* plain IEEE ops (operand4): no VU mode, and the shared FP word untouched */
    host_fp_set_csr((control & ~0x603F) | 0xFFC0); /* toward zero, FTZ/DAZ, host masks */
#endif
#if defined(PS2_VU_AOT) && !defined(PS2_VU_AOT_EMBED) && !defined(__wasm__) &&     (defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__))
    /* 64-bit host: ps2_image_load (memory.c) runs in the wasm module, whose copy of this file has
       no AOT, so it never fills this copy's block and kernel words. Take them from the same loaded
       image in guest RAM before the first VU1 program, as i686 does at its image load. Without
       this no block or native kernel ever matches and VU1 runs only on the interpreter. */
    static int aot_loaded;
    if (!aot_loaded) {
        aot_loaded = 1;
        ps2_vu_aot_load(GUEST(PS2_RAM_BASE + PS2_IMAGE_START));  /* the guest's PS2_PTR(PS2_IMAGE_START) */
    }
#endif
    int result = run(cpu, addr, 1, top, itop, kick, opaque);
#ifndef PS2_VU_IEEE
    host_fp_set_csr(control);
#endif
    return result;
}
