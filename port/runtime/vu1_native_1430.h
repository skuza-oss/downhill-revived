/* Program 1430's mesh/light loop, 1510..1620 (dispatch VI13=0x288).
   Registers remain named as in the microprogram to preserve operation order.
   No MAC or sticky-status reads occur in this loop: FSAND reads only the new
   DIV's D bit. FCAND reads the CLIP issued four pairs earlier. The closed
   clipping routine returns through VI5 to 15E0; it retains exact pipeline
   helpers. Unsupported vertex counts or a pending Q decline before writing. */
/* The registers the loop writes (the others stay in the file as loaded). */
enum { WRITES_1430 = 1u << 5 | 1u << 6 | 1u << 8 | 1u << 9 | 1u << 10 | 1u << 11 | 1u << 12 | 1u << 16 | 1u << 20 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 28 | 1u << 29 | 1u << 30 };
static inline __attribute__((always_inline)) unsigned native_geometry_1430_impl(Vu *u, int sticky_complete)
{
    unsigned begin = u->c->vi[8], end = u->c->vi[10], distance = (end - begin) & 65535;
    if (!distance || distance % 3 || distance / 3 > 64 || (u->q_ready && u->q_ready > u->cycle))
        return 0;
    ps2_q128 *memory = u->c->vu0_mem;
#define cpu (*u->c)
    drain_flags(u);
    update_q(u);
    unsigned entry_status = u->status;
    __m128 v[32], c[32], acc = _mm_loadu_ps(cpu.vacc);
    NATIVE_LOAD_FILE();
    NativeFlags flags = {_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(),
                         _mm_setzero_ps(), 0};
    flags.sticky_complete = sticky_complete;
    unsigned clip = u->clip, pairs = 0, index = begin;
    uint16_t *vi = cpu.vi;
    float q = cpu.q;
    __m128 qv = operand4(_mm_set1_ps(q)); /* Q as an operand: the file's clamped, a divide's as it is */
    unsigned q_flags = u->status & 0x30;
#define NATIVE_SAVE()                                                                              \
    do {                                                                                           \
        NATIVE_STORE_FILE(WRITES_1430);                                                            \
        _mm_storeu_ps(cpu.vacc, acc);                                                              \
        cpu.q = q;                                                                                 \
        Upper last = {0};                                                                          \
        fmac_flags4(&last, flags.last, flags.mask);                                                \
        unsigned mac = last.mac;                                                                   \
        unsigned status = (mac & 15 ? 1 : 0) | (mac & 240 ? 2 : 0) | (mac & 3840 ? 4 : 0) |        \
                          (mac & 61440 ? 8 : 0);                                                   \
        unsigned sticky = (_mm_movemask_ps(_mm_castsi128_ps(flags.zero)) ? 1 : 0) |                \
                          (_mm_movemask_ps(_mm_castsi128_ps(flags.sign)) ? 2 : 0) |                \
                          (_mm_movemask_ps(_mm_castsi128_ps(flags.overflow)) ? 8 : 0);             \
        u->status = (entry_status & ~0x3fu) | status | q_flags | (sticky << 6);                    \
        u->mac = mac;                                                                              \
        u->clip = u->clip_next = clip;                                                             \
        u->n_flags = 0;                                                                            \
        u->flag_head = 0;                                                                          \
        u->q_ready = 0;                                                                            \
        u->cycle += 64;                                                                            \
        u->flag_cycle = u->cycle;                                                                  \
    } while (0)
#define NADD(a, b, d) native_add_c(&flags, (a), (b), (d))
#define NMUL(a, b, d) native_mul_c(&flags, (a), (b), (d))
#define NMADD(a, b, c_, d) native_madd_c(&flags, (a), (b), (c_), (d))
#define LOAD(a) _mm_loadu_ps(memory[(a) & 1023].f)
#define STORE(a, x, d) store_fields(memory[(a) & 1023].f, _mm_castps_si128(x), (d))
    do {
        float denominator = _mm_cvtss_f32(arithmetic4(lane4(v[25], 3)));
        q_flags = denominator == 0 ? 0x20 : 0;
        unsigned issued = q_flags ? 35 : 34;
        q = denominator == 0
                ? vu_max(signbit(denominator))
                : _mm_cvtss_f32(arithmetic4(VU_DIV_SS(_mm_set_ss(1), _mm_set_ss(denominator))));
        qv = _mm_set1_ps(q);
        NATIVE_SETLANE(30, 25, 3, 1);
        NATIVE_SET(23, NMUL(c[23], lane4(c[23], 3), 1), 1);
        NATIVE_SETLANE(16, 0, 3, 1);
        NATIVE_SET(9, NADD(c[9], c[19], 12), 12);
        NATIVE_SETLANE(25, 18, 3, 1);
        acc = NMUL(c[0], c[22], 15);
        NATIVE_RAW(6, LOAD(373 + vi[9]));
        NATIVE_RAW(10, LOAD(20));
        NATIVE_SET(23, NMUL(c[23], lane4(c[23], 3), 1), 1);
        NATIVE_FULL(24, NMADD(acc, c[25], qv, 15));
        vi[1] = q_flags;
        NATIVE_SET(20, NMUL(c[9], qv, 14), 14);
        NATIVE_FULL(6, native_itof(v[6], 1.0f / 4096));
        if (q_flags)
            NATIVE_SET(20, NADD(c[9], lane4(c[0], 0), 14), 14);
        acc = NMUL(c[10], lane4(c[23], 3), 15);
        NATIVE_FULL(23, NMADD(acc, c[12], c[31], 15));
        cpu.i = 255.0f;
        NATIVE_SET(5, NADD(c[24], c[13], 12), 12);
        NATIVE_RAW(8, LOAD(7));
        NATIVE_SET(5, NADD(c[13], lane4(c[30], 3), 2), 2);
        STORE(index, v[20], 14);
        NATIVE_SETRAW(24, minmax4(v[24], lane4(v[18], 1), 0), 1);
        NATIVE_RAW(10, LOAD(8));
        NATIVE_RAW(23, minmax4(v[23], _mm_set1_ps(cpu.i), 0));
        NATIVE_MOVE(11, 9);
        acc = NMUL(c[14], lane4(c[6], 0), 15);
        NATIVE_RAW(9, LOAD(index + 5));
        clip = ((clip << 6) | native_clip(v[5], lane4(v[13], 3))) & 0xffffff;
        NATIVE_RAW(20, LOAD(181 + vi[9]));
        NATIVE_SETRAW(24, minmax4(v[24], lane4(v[0], 0), 1), 1);
        NATIVE_RAW(30, native_ftoi(v[23], 1));
        acc = NMADD(acc, c[8], lane4(c[6], 1), 15);
        NATIVE_MOVE(28, 5);
        NATIVE_FULL(23, NMADD(acc, c[10], lane4(c[6], 2), 15));
        vi[1] = (clip & 0x02fbef) != 0;
        NATIVE_SET(24, NADD(c[11], lane4(c[24], 3), 1), 1);
        NATIVE_FULL(9, native_itof(v[9], 1.0f / 4096));
        STORE(index + 1, v[30], 15);
        native_vertices++;
        if (vi[1]) {
            native_clipped_vertices++;
            NATIVE_SAVE();
            unsigned clipped = native_clip_run(u, 0x15e0 / 8);
            if (!clipped)
                return ~0u;
            pairs += clipped;
            NATIVE_LOAD_FILE();
            acc = _mm_loadu_ps(cpu.vacc);
            q = cpu.q;
            qv = operand4(_mm_set1_ps(q));
            clip = u->clip;
            entry_status = u->status;
            q_flags = u->status & 0x30;
            memset(&flags, 0, sizeof flags);
            flags.sticky_complete = sticky_complete;
        }
        NATIVE_RAW(23, minmax4(v[23], lane4(v[0], 0), 1));
        NATIVE_RAW(12, LOAD(index + 4));
        acc = NMUL(c[4], lane4(c[0], 3), 15);
        NATIVE_RAW(8, LOAD(18));
        acc = NMADD(acc, c[1], lane4(c[20], 0), 15);
        NATIVE_MOVE(25, 26);
        NATIVE_RAW(24, native_ftoi(v[24], 16));
        index = (index + 3) & 65535;
        vi[8] = (uint16_t)index;
        NATIVE_SET(23, NMUL(c[23], lane4(c[23], 3), 1), 1);
        NATIVE_MOVE(29, 28);
        acc = NMADD(acc, c[2], lane4(c[20], 1), 15);
        NATIVE_RAW(10, LOAD(19));
        NATIVE_FULL(25, NMADD(acc, c[3], lane4(c[20], 2), 15));
        vi[9] = (uint16_t)memory[(index + 3) & 1023].w[3];
        NATIVE_FULL(12, native_itof(v[12], 1));
        NATIVE_SET(23, NMUL(c[23], lane4(c[23], 3), 1), 1);
        STORE(index - 1, v[24], 15);
        pairs += issued;
    } while (index != end);
    NATIVE_SAVE();
    return pairs;
#undef NATIVE_SAVE
#undef cpu
#undef NADD
#undef NMUL
#undef NMADD
#undef LOAD
#undef STORE
}
static unsigned native_geometry_1430(Vu *u) {
    if ((u->status & 0xc0) == 0xc0) return native_geometry_1430_impl(u, 1);
    return native_geometry_1430_impl(u, 0);
}
