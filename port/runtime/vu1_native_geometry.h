static unsigned long long native_vertices, native_clipped_vertices;
/* Direct-write SIMD geometry loop with a directly threaded clipping path. */
typedef struct { __m128i sign, zero, overflow; __m128 last; int mask, sticky_complete; } NativeFlags;
static __m128 native_mask(int d)
{
    return _mm_castsi128_ps(_mm_setr_epi32(d & 8 ? -1 : 0, d & 4 ? -1 : 0, d & 2 ? -1 : 0, d & 1 ? -1 : 0));
}
static __m128 native_merge(__m128 old, __m128 value, int d)
{
    __m128 mask = native_mask(d);
    return _mm_or_ps(_mm_andnot_ps(mask, old), _mm_and_ps(mask, value));
}
static __m128 native_result(NativeFlags *f, __m128 value, int d)
{
    __m128i bits = _mm_castps_si128(value), mask = _mm_castps_si128(native_mask(d));
#ifdef PS2_VU_IEEE_PROBE
    vu_ieee_probe_result(value, d);
#endif
#ifdef PS2_VU_IEEE  /* FTZ and the overflow clamp of plain IEEE results (ieee_clamp4, vu0_interp.c) */
    value = ieee_clamp4(value);
#endif
    if (!f->sticky_complete) {
        f->sign = _mm_or_si128(f->sign, _mm_and_si128(bits, mask));
        f->zero = _mm_or_si128(f->zero, _mm_and_si128(_mm_castps_si128(_mm_cmpeq_ps(value, _mm_setzero_ps())), mask));
    }
    /* f->overflow stays 0: no result has exponent 255 (fmac_flags4). */
    f->last = value; f->mask = d;
    /* All values here are SSE arithmetic results on operand4 values under FTZ/DAZ and
       toward-zero rounding: no subnormal FMAC result survives to set the interpreter's U test,
       and arithmetic4 of the result is the identity (operand4, vu0_interp.c). */
    return value;
}
static __m128 native_add(NativeFlags *f, __m128 a, __m128 b, int d)
{ return native_result(f, VU_ADD4(operand4(a), operand4(b)), d); }
static __m128 native_sub(NativeFlags *f, __m128 a, __m128 b, int d)
{ return native_result(f, VU_SUB4(operand4(a), operand4(b)), d); }
static __m128 native_mul(NativeFlags *f, __m128 a, __m128 b, int d)
{ return native_result(f, VU_MUL4(operand4(a), operand4(b)), d); }
static __m128 native_madd(NativeFlags *f, __m128 acc, __m128 a, __m128 b, int d)
{
    return native_result(f, VU_ADD4(operand4(acc), VU_PRODUCT4(operand4(a), operand4(b))), d);
}
/* acc must have all four lanes written by a preceding FMAC under the VU1
   FTZ/DAZ, toward-zero environment. Such results are finite normals or zeros.
   Keep the optimizer barrier even though the exceptional-value clamp is dead. */
static __m128 native_madd_clean_acc(NativeFlags *f, __m128 acc, __m128 a, __m128 b, int d)
{
#ifndef PS2_VU_IEEE  /* plain IEEE ops: no host rounding mode, and the clamped results keep acc clean */
    PS2_OPAQUE_M128(acc);
#endif
    return native_result(f, VU_ADD4(acc, VU_PRODUCT4(operand4(a), operand4(b))), d);
}
/* Clean operands (finding 197). An FMAC operand is clamped first (operand4: exponent 255 becomes
   the largest finite magnitude). Most operands are results of earlier FMAC or ITOF operations,
   which are finite already, so a kernel can keep beside each register v[r] its clamped value
   c[r] = operand4(v[r]) and read that: c[r] is v[r] itself where an FMAC or ITOF result is
   written (operand4 of such a result is the identity: native_result's note), a moved or merged
   c where v is moved or merged, and one operand4 where a raw value arrives (a register at the
   kernel's entry, a load from data memory, FTOI, MIN/MAX). The operations and their operand
   values are then the ones the clamps at every use gave, with a clamp for each raw value instead
   of one for each use. native_clean keeps operand4's optimizer barrier (the compiler must not
   fold the operation or evaluate it under its own rounding). */
static inline __attribute__((always_inline)) __m128 native_clean(__m128 value)
{
#ifndef PS2_VU_IEEE  /* plain IEEE ops: no host rounding mode to hide the operand from */
    PS2_OPAQUE_M128(value);
#endif
    return value;
}
#ifdef PS2_VU_CLEAN_CHECK
/* Test builds (vu1_seq_diff.py --clean-check): every value a kernel writes as clean is compared
   with its clamp in the lanes written. */
unsigned long long ps2_vu1_unclean;
static inline __attribute__((always_inline)) __m128 native_checked(__m128 value, int d)
{
    __m128i differs = _mm_and_si128(_mm_xor_si128(_mm_castps_si128(operand4(value)), _mm_castps_si128(value)),
                                    _mm_castps_si128(native_mask(d)));
    if (_mm_movemask_epi8(_mm_cmpeq_epi32(differs, _mm_setzero_si128())) != 0xffff) ps2_vu1_unclean++;
    return value;
}
#else
#define native_checked(value, d) (value)
#endif
static __m128 native_add_c(NativeFlags *f, __m128 a, __m128 b, int d)
{ return native_result(f, VU_ADD4(native_clean(a), native_clean(b)), d); }
static __m128 native_sub_c(NativeFlags *f, __m128 a, __m128 b, int d)
{ return native_result(f, VU_SUB4(native_clean(a), native_clean(b)), d); }
static __m128 native_mul_c(NativeFlags *f, __m128 a, __m128 b, int d)
{ return native_result(f, VU_MUL4(native_clean(a), native_clean(b)), d); }
static __m128 native_madd_c(NativeFlags *f, __m128 acc, __m128 a, __m128 b, int d)
{ return native_result(f, VU_ADD4(native_clean(acc), VU_PRODUCT4(native_clean(a), native_clean(b))), d); }
static __m128 native_msub_c(NativeFlags *f, __m128 acc, __m128 a, __m128 b, int d)
{ return native_result(f, VU_SUB4(native_clean(acc), VU_PRODUCT4(native_clean(a), native_clean(b))), d); }
/* The kernels' register writes with c[] kept (v, c and, for the ACC ones, acc and cacc are the
   kernel's locals): a clean value (an FMAC or ITOF result) into every lane or the lanes of d; a
   raw value, clamped once here; a register or one of its lanes copied. */
#define NATIVE_FULL(r, x) do { __m128 x_ = native_checked((x), 15); v[r] = x_; c[r] = x_; } while (0)
#define NATIVE_SET(r, x, d) do { __m128 x_ = native_checked((x), (d)); \
    v[r] = native_merge(v[r], x_, (d)); c[r] = native_merge(c[r], x_, (d)); } while (0)
#define NATIVE_RAW(r, x) do { __m128 x_ = (x); v[r] = x_; c[r] = operand4(x_); } while (0)
#define NATIVE_SETRAW(r, x, d) do { __m128 x_ = (x); \
    v[r] = native_merge(v[r], x_, (d)); c[r] = native_merge(c[r], operand4(x_), (d)); } while (0)
#define NATIVE_MOVE(r, s) do { v[r] = v[s]; c[r] = c[s]; } while (0)
#define NATIVE_SETMOVE(r, s, d) do { v[r] = native_merge(v[r], v[s], (d)); c[r] = native_merge(c[r], c[s], (d)); } while (0)
#define NATIVE_SETLANE(r, s, lane, d) do { v[r] = native_merge(v[r], lane4(v[s], (lane)), (d)); \
    c[r] = native_merge(c[r], lane4(c[s], (lane)), (d)); } while (0)
/* Every register from the VU's file with its clean copy (the kernel's entry, and after the
   clipping routine ran on the file); the compiler drops the copies a kernel never reads. Written
   out register by register: in a loop the clamp keeps the compiler from unrolling it, and v[] and
   c[] then stay in memory for the whole kernel. */
#define NATIVE_LOAD_1(i) v[i] = _mm_loadu_ps(cpu.vf[i].f); c[i] = operand4(v[i]);
#define NATIVE_LOAD_FILE() do { \
    v[0] = c[0] = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f); \
    NATIVE_LOAD_1(1) NATIVE_LOAD_1(2) NATIVE_LOAD_1(3) NATIVE_LOAD_1(4) NATIVE_LOAD_1(5) NATIVE_LOAD_1(6) NATIVE_LOAD_1(7) \
    NATIVE_LOAD_1(8) NATIVE_LOAD_1(9) NATIVE_LOAD_1(10) NATIVE_LOAD_1(11) NATIVE_LOAD_1(12) NATIVE_LOAD_1(13) NATIVE_LOAD_1(14) \
    NATIVE_LOAD_1(15) NATIVE_LOAD_1(16) NATIVE_LOAD_1(17) NATIVE_LOAD_1(18) NATIVE_LOAD_1(19) NATIVE_LOAD_1(20) NATIVE_LOAD_1(21) \
    NATIVE_LOAD_1(22) NATIVE_LOAD_1(23) NATIVE_LOAD_1(24) NATIVE_LOAD_1(25) NATIVE_LOAD_1(26) NATIVE_LOAD_1(27) NATIVE_LOAD_1(28) \
    NATIVE_LOAD_1(29) NATIVE_LOAD_1(30) NATIVE_LOAD_1(31) \
} while (0)
/* The registers of `written` (a bit each, a constant) back to the file. A kernel names the ones
   its loop writes: the others are in the file as it loaded them, and a raw copy kept only to be
   stored back unchanged would cost the loop a register or a stack slot. */
#define NATIVE_STORE_1(written, i) if ((written) >> (i) & 1) _mm_storeu_ps(cpu.vf[i].f, v[i]);
#define NATIVE_STORE_FILE(w) do { \
    NATIVE_STORE_1(w, 1) NATIVE_STORE_1(w, 2) NATIVE_STORE_1(w, 3) NATIVE_STORE_1(w, 4) NATIVE_STORE_1(w, 5) NATIVE_STORE_1(w, 6) \
    NATIVE_STORE_1(w, 7) NATIVE_STORE_1(w, 8) NATIVE_STORE_1(w, 9) NATIVE_STORE_1(w, 10) NATIVE_STORE_1(w, 11) NATIVE_STORE_1(w, 12) \
    NATIVE_STORE_1(w, 13) NATIVE_STORE_1(w, 14) NATIVE_STORE_1(w, 15) NATIVE_STORE_1(w, 16) NATIVE_STORE_1(w, 17) NATIVE_STORE_1(w, 18) \
    NATIVE_STORE_1(w, 19) NATIVE_STORE_1(w, 20) NATIVE_STORE_1(w, 21) NATIVE_STORE_1(w, 22) NATIVE_STORE_1(w, 23) NATIVE_STORE_1(w, 24) \
    NATIVE_STORE_1(w, 25) NATIVE_STORE_1(w, 26) NATIVE_STORE_1(w, 27) NATIVE_STORE_1(w, 28) NATIVE_STORE_1(w, 29) NATIVE_STORE_1(w, 30) \
    NATIVE_STORE_1(w, 31) \
} while (0)
/* A float's arithmetic4 (the divide's result and its denominator, one lane). */
static inline __attribute__((always_inline)) float native_arithmetic(float value)
{
    uint32_t bits; memcpy(&bits, &value, 4);
    if (!(bits & 0x7f800000)) bits &= 0x80000000;
    else if ((bits & 0x7f800000) == 0x7f800000) bits = (bits & 0x80000000) | 0x7f7fffff;
    memcpy(&value, &bits, 4);
    return value;
}
/* The XYZ lanes of xyz and an integer word in W, for one 16-byte store. */
static inline __attribute__((always_inline)) __m128i native_xyz_word(__m128 xyz, uint32_t word)
{
#if defined(__aarch64__)
    return vreinterpretq_s64_u32(vsetq_lane_u32(word, vreinterpretq_u32_f32(xyz), 3));
#else
    __m128i w = _mm_shuffle_epi32(_mm_cvtsi32_si128((int)word), _MM_SHUFFLE(0, 1, 1, 1));
    return _mm_or_si128(_mm_and_si128(_mm_castps_si128(xyz), _mm_setr_epi32(-1, -1, -1, 0)), w);
#endif
}
static __m128 native_itof(__m128 value, float scale)
{ return _mm_mul_ps(VU_ITOF4(_mm_castps_si128(value)), _mm_set1_ps(scale)); }
static __m128 native_ftoi(__m128 value, float scale)
{
    return vu1_ftoi4(value, scale);
}
#if defined(__aarch64__)
/* NEON: the three lanes' signed compares against the bound at once; bits as below. */
static unsigned native_clip(__m128 value, __m128 limit)
{
    uint32_t l = vgetq_lane_u32(vreinterpretq_u32_f32(limit), 0);
    l = l & 0x7f800000 ? l & 0x7fffffff : 0x007fffff;
    int32x4_t v = vreinterpretq_s32_f32(value), bound = vdupq_n_s32((int32_t)l);
    uint32x4_t above = vcgtq_s32(v, bound);
    uint32x4_t below = vcgtq_s32(veorq_s32(v, vdupq_n_s32(INT32_MIN)), bound);
    const uint32x4_t up = {1, 4, 16, 0}, down = {2, 8, 32, 0};
    return vaddvq_u32(vorrq_u32(vandq_u32(above, up), vandq_u32(below, down)));
}
#else
static unsigned native_clip(__m128 value, __m128 limit)
{
    uint32_t v[4], l; _mm_storeu_si128((__m128i *)v, _mm_castps_si128(value));
    memcpy(&l, &limit, 4);
    l = l & 0x7f800000 ? l & 0x7fffffff : 0x007fffff;
    unsigned bits = 0;
    for (int c = 0; c < 3; c++) {
        if ((int32_t)v[c] > (int32_t)l) bits |= 1u << (c * 2);
        if ((int32_t)(v[c] ^ 0x80000000) > (int32_t)l) bits |= 2u << (c * 2);
    }
    return bits;
}
#endif
/* The mesh loop 0170-0258: a vertex transformed, projected, tested for clipping and culling and
   packed, three pairs of the pipelined program a vertex in flight. Its operands are clean copies
   kept at each register write (c[], finding 197: 29 clamps a vertex were a third of its NEON
   work on the phone): raw values arrive from the file at entry and after the clipping routine,
   from data memory (vf20's matrix row, vf06 back from its slot) and from MIN/MAX and FTOI. The
   divide's denominator is vf26's W as the last MADD wrote it (an FMAC result: arithmetic4 of it
   is the identity), so only the entry's is clamped; I and the integer registers the loop writes
   live in locals and go to the file where the clipping routine or the caller can see them. ACC
   is written whole (0210's MULA) before the file is written anywhere, so it is one clean value
   and the file's ACC at entry is never needed; only the registers the loop writes go back. */
enum { NATIVE_LOOP_WRITES = 1u << 5 | 1u << 6 | 1u << 7 | 1u << 8 | 1u << 9 | 1u << 10 | 1u << 11 | 1u << 12 | 1u << 14 |
                            1u << 15 | 1u << 16 | 1u << 17 | 1u << 20 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 26 |
                            1u << 28 | 1u << 29 | 1u << 30 };
static inline __attribute__((always_inline)) unsigned native_geometry_loop_impl(Vu *u, int sticky_complete)
{
    unsigned begin = u->c->vi[8], end = u->c->vi[10];
    unsigned distance = (end - begin) & 0xffff;
    if (!distance || distance % 3 || distance / 3 > 64 || (u->q_ready && u->q_ready > u->cycle)) { return 0; }
    ps2_q128 *memory = u->c->vu0_mem;
#define cpu (*u->c)
    drain_flags(u); update_q(u);
    unsigned entry_status = u->status;
    __m128 v[32], c[32], acc = _mm_setzero_ps();
    NATIVE_LOAD_FILE();
    NativeFlags flags = {_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_ps(), 0};
    flags.sticky_complete = sticky_complete;
    unsigned clip = u->clip, pairs = 0, index = begin, vertices = 0;
    uint16_t *vi = cpu.vi;
    unsigned vi1 = vi[1], vi3 = vi[3], vi7 = vi[7], vi9 = vi[9];
    float q = cpu.q, i_reg = cpu.i; unsigned q_flags = 0;
    float denominator = _mm_cvtss_f32(arithmetic4(lane4(v[26], 3)));
#define NATIVE_SAVE() do { \
    NATIVE_STORE_FILE(NATIVE_LOOP_WRITES); \
    _mm_storeu_ps(cpu.vacc,acc); cpu.q = q; cpu.i = i_reg; \
    vi[1] = (uint16_t)vi1; vi[3] = (uint16_t)vi3; vi[7] = (uint16_t)vi7; vi[8] = (uint16_t)index; vi[9] = (uint16_t)vi9; \
    native_vertices += vertices; vertices = 0; \
    Upper last = {0}; fmac_flags4(&last,flags.last,flags.mask); \
    unsigned mac = last.mac; \
    unsigned status = (mac & 15 ? 1 : 0) | (mac & 240 ? 2 : 0) | (mac & 3840 ? 4 : 0) | (mac & 61440 ? 8 : 0); \
    unsigned sticky = (_mm_movemask_ps(_mm_castsi128_ps(flags.zero)) ? 1 : 0) | \
                      (_mm_movemask_ps(_mm_castsi128_ps(flags.sign)) ? 2 : 0) | \
                      (_mm_movemask_ps(_mm_castsi128_ps(flags.overflow)) ? 8 : 0); \
    u->status = (entry_status & ~0x3fu) | status | q_flags | (sticky << 6); \
    u->mac = mac; u->clip = u->clip_next = clip; u->n_flags = 0; u->flag_head = 0; \
    u->q_ready = 0; u->cycle += 64; u->flag_cycle = u->cycle; \
} while(0)
#define NADD(a,b,d) native_add_c(&flags,(a),(b),(d))
#define NSUB(a,b,d) native_sub_c(&flags,(a),(b),(d))
#define NMUL(a,b,d) native_mul_c(&flags,(a),(b),(d))
#define NMADD(a,b,c_,d) native_madd_c(&flags,(a),(b),(c_),(d))
#define LOAD(a) _mm_loadu_ps(memory[(a) & 1023].f)
#define STORE(a,x,d) store_fields(memory[(a) & 1023].f, _mm_castps_si128(x), (d))
    do {
        NATIVE_FULL(15, NMUL(c[15], c[31], 15)); i_reg = 255.0f;
        NATIVE_SET(16, NSUB(c[24], c[10], 14), 14); vi9 = (uint16_t)memory[(index + 6) & 1023].w[3];
        NATIVE_SET(5, NADD(c[13], lane4(c[25], 3), 2), 2); NATIVE_MOVE(10, 24);
        NATIVE_SET(5, NADD(c[24], c[13], 12), 12);
        q_flags = denominator == 0 ? 0x20 : 0;
        q = denominator == 0 ? vu_max(signbit(denominator)) : native_arithmetic(VU_DIVF(1.0f, denominator));
        NATIVE_RAW(15, minmax4(v[15], _mm_set1_ps(i_reg), 0)); NATIVE_MOVE(25, 26);
        NATIVE_SETRAW(17, native_ftoi(v[23], 16.0f), 1); NATIVE_SETLANE(26, 18, 3, 1);
        NATIVE_SET(7, NADD(c[7], c[19], 12), 12); NATIVE_RAW(20, LOAD(181 + vi9));
        NATIVE_SET(6, NSUB(c[0], lane4(c[6], 0), 8), 8); NATIVE_RAW(9, LOAD(8 + index));
        __m128 a = _mm_shuffle_ps(c[16], c[16], _MM_SHUFFLE(3,0,2,1));
        __m128 b = _mm_shuffle_ps(c[14], c[14], _MM_SHUFFLE(3,1,0,2));
        acc = native_merge(acc, NMUL(a,b,14), 14); vi3 = 0x20;
        a = _mm_shuffle_ps(c[14], c[14], _MM_SHUFFLE(3,0,2,1));
        b = _mm_shuffle_ps(c[16], c[16], _MM_SHUFFLE(3,1,0,2));
        NATIVE_SET(12, native_msub_c(&flags, acc, a, b, 14), 14);
        vi7 = (uint16_t)_mm_cvtsi128_si32(_mm_castps_si128(lane4(v[17], 3)));
        unsigned backface = (_mm_movemask_ps(flags.last) & 4) ? 0x20 : 0;
        clip = ((clip << 6) | native_clip(v[5], lane4(v[13], 3))) & 0xffffff;
        vi1 = q_flags;
        acc = NMUL(c[0], c[22], 15); STORE(index, v[8], 14);
        NATIVE_FULL(23, NMADD(acc, c[26], _mm_set1_ps(q), 15));
        NATIVE_FULL(14, NMUL(c[16], lane4(c[6], 0), 15)); vi3 = backface;
        if(q_flags) { NATIVE_SET(8, NADD(c[7],lane4(c[0],0),14),14); }
        else { NATIVE_SET(8, NMUL(c[7], _mm_set1_ps(q), 14), 14); } vi1 = (clip & 0x02fbef) != 0;
        NATIVE_RAW(15, native_ftoi(v[15], 1.0f));
        NATIVE_SETRAW(23, minmax4(v[23], lane4(v[18],1),0), 1); NATIVE_MOVE(28, 5);
        if (backface) vi7 |= vi[4];
        STORE(1014, v[6], 15);
        vertices++;
        STORE(index + 1, v[15], 15);
        if (vi1) {
            native_clipped_vertices++;
            NATIVE_SAVE();
            unsigned clip_pairs = native_clip_run(u, 0x218/8);
            if(!clip_pairs) return 0;
            pairs += clip_pairs;
            NATIVE_LOAD_FILE();
            q=cpu.q;i_reg=cpu.i;
            vi1=vi[1];vi3=vi[3];vi7=vi[7];vi9=vi[9];
            clip=u->clip;entry_status=u->status;q_flags=u->status&0x30;
            memset(&flags,0,sizeof flags);
            flags.sticky_complete = sticky_complete;
        }
        acc = NMUL(c[4], lane4(c[0],3),15); NATIVE_MOVE(11, 7);
        NATIVE_FULL(7, native_itof(v[9], 1.0f/4096.0f)); NATIVE_RAW(15, LOAD(4 + index));
        NATIVE_SETRAW(23, minmax4(v[23], lane4(v[0],0),1),1); index = (index + 3) & 0xffff;
        NATIVE_SETRAW(30,native_ftoi(v[24],16.0f),14); NATIVE_SETMOVE(24, 23, 14);
        acc = NMADD(acc,c[1],lane4(c[20],0),15); NATIVE_MOVE(29, 28);
        NATIVE_FULL(15, native_itof(v[15],1.0f)); NATIVE_RAW(6, LOAD(1014));
        acc = NMADD(acc,c[2],lane4(c[20],1),15);
        NATIVE_FULL(26, NMADD(acc,c[3],lane4(c[20],2),15));
        denominator = _mm_cvtss_f32(lane4(v[26], 3));   /* an FMAC result: its arithmetic4 is itself */
        NATIVE_SET(23,NADD(c[23],lane4(c[11],3),1),1);
        /* vf30's XYZ and vi07 in W at once: a word store before the three-lane merge's 16-byte
           load of the same qword would not be forwarded to it */
        _mm_storeu_si128((__m128i *)memory[(index - 1) & 1023].f, native_xyz_word(v[30], vi7));
        pairs += 29 + !!backface;
    } while (index != end);
    NATIVE_SAVE();
    return pairs;
#undef NATIVE_SAVE
#undef cpu
#undef NADD
#undef NSUB
#undef NMUL
#undef NMADD
#undef LOAD
#undef STORE
}
static unsigned native_geometry_loop(Vu *u) {
    if ((u->status & 0xc0) == 0xc0) return native_geometry_loop_impl(u, 1);
    return native_geometry_loop_impl(u, 0);
}
