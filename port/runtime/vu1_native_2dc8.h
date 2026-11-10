/* Program 2DC8's 2F00..2FC0 loop (VI13=0x5BD).
   FSAND at 2F30 sees the MAC from 2F10: no intervening read can stall after
   that pair. Its Z/S bits and the current DIV D bit control the fallback.
   The final DIV remains pending for two more issue slots; the guarded tail
   has no early MAC/status read. Register names preserve microcode ordering. */
/* The registers the loop writes (the others stay in the file as loaded). */
enum { WRITES_2DC8 = 1u << 5 | 1u << 6 | 1u << 9 | 1u << 11 | 1u << 12 | 1u << 17 | 1u << 19 | 1u << 20 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 26 | 1u << 28 | 1u << 29 | 1u << 30 | 1u << 31 };
static inline __attribute__((always_inline)) unsigned native_geometry_2dc8_impl(Vu *u, int sticky_complete)
{
    unsigned begin = u->c->vi[8], end = u->c->vi[10], distance = (end - begin) & 65535;
    if (!distance || distance % 3 || distance / 3 > 64 || (u->q_ready && u->q_ready > u->cycle))
        return 0;
    ps2_q128 *memory = u->c->vu0_mem;
#define cpu (*u->c)
    drain_flags(u);
    update_q(u);
    unsigned entry_status = u->status;
    __m128 v[32], c[32], acc = operand4(_mm_loadu_ps(cpu.vacc));
    NATIVE_LOAD_FILE();
    NativeFlags flags = {_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(),
                         _mm_setzero_ps(), 0};
    flags.sticky_complete = sticky_complete;
    unsigned clip = u->clip, pairs = 0, index = begin;
    uint16_t *vi = cpu.vi;
    float q = cpu.q, next_q = q;
    unsigned q_flags = u->status & 0x30, next_q_flags = q_flags;
#define NATIVE_SAVE()                                                                              \
    do {                                                                                           \
        NATIVE_STORE_FILE(WRITES_2DC8);                                                            \
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
/* Q as an operand: it comes from the file, the clipping routine's divide or this loop's (next_q) */
#define qv operand4(_mm_set1_ps(q))
#define LOAD(a) _mm_loadu_ps(memory[(a) & 1023].f)
#define STORE(a, x, d) store_fields(memory[(a) & 1023].f, _mm_castps_si128(x), (d))
    do {
        NATIVE_FULL(12, NMADD(acc, c[10], lane4(c[20], 2), 15));
        NATIVE_MOVE(25, 26);
        acc = native_merge(acc, NADD(c[9], lane4(c[9], 1), 2), 2);
        NATIVE_SETRAW(17, LOAD(index + 5), 1);
        __m128 branch_value = NMADD(acc, c[19], lane4(c[9], 0), 2);
        NATIVE_SET(9, branch_value, 2);
        NATIVE_RAW(20, LOAD(181 + vi[9]));
        Upper branch_flags = {0};
        fmac_flags4(&branch_flags, branch_value, 2);
        unsigned branch_status = (branch_flags.mac & 15 ? 1 : 0) | (branch_flags.mac & 240 ? 2 : 0);
        NATIVE_SET(24, NADD(c[23], lane4(c[18], 1), 1), 1);
        STORE(index, v[31], 14);
        acc = NMUL(c[4], lane4(c[0], 3), 15);
        vi[6] = (uint16_t)memory[(index + 3) & 1023].w[3];
        clip = ((clip << 6) | native_clip(v[5], lane4(v[13], 3))) & 0xffffff;
        vi[9] = (uint16_t)memory[(index + 9) & 1023].w[3];
        NATIVE_SET(23, NMUL(c[25], qv, 14), 14);
        vi[1] = branch_status | (q_flags & 0x20);
        unsigned issued = vi[1] ? ((q_flags & 0x20) ? 25 : 24) : 21;
        NATIVE_SET(31, NMUL(c[12], qv, 14), 14);
        NATIVE_SET(23, native_itof(v[17], 1.0f / 4096), 1);
        NATIVE_RAW(9, LOAD(373 + vi[6]));
        if (vi[1]) {
            NATIVE_SET(19, NADD(c[0], lane4(c[0], 0), 1), 1);
            vi[1] = q_flags & 0x20;
            if (vi[1])
                NATIVE_SET(31, NADD(c[12], lane4(c[0], 0), 14), 14);
        }
        acc = NMADD(acc, c[1], lane4(c[20], 0), 15);
        NATIVE_SETLANE(11, 24, 3, 1);
        acc = NMADD(acc, c[2], lane4(c[20], 1), 15);
        NATIVE_MOVE(28, 5);
        NATIVE_FULL(26, NMADD(acc, c[3], lane4(c[20], 2), 15));
        vi[1] = (clip & 0x02fbef) != 0;
        NATIVE_FULL(9, native_itof(v[9], 1.0f / 4096));
        /* Both issue in 2F88: SQ must read the old VF19.w before ADDx commits. */
        STORE(index + 1, v[19], 15);
        NATIVE_SET(19, NADD(c[6], lane4(c[0], 0), 1), 1);
        native_vertices++;
        if (vi[1]) {
            native_clipped_vertices++;
            NATIVE_SAVE();
            unsigned clipped = native_clip_run(u, 0x2f90 / 8);
            if (!clipped)
                return ~0u;
            pairs += clipped;
            NATIVE_LOAD_FILE();
            acc = _mm_loadu_ps(cpu.vacc);
            q = cpu.q;
            clip = u->clip;
            entry_status = u->status;
            q_flags = u->status & 0x30;
            memset(&flags, 0, sizeof flags);
            flags.sticky_complete = sticky_complete;
        }
        NATIVE_RAW(30, native_ftoi(v[24], 16));
        NATIVE_RAW(6, LOAD(1013));
        acc = NMUL(c[14], lane4(c[0], 3), 15);
        NATIVE_SETMOVE(24, 23, 14);
        acc = NMADD(acc, c[7], lane4(c[20], 0), 15);
        /* DIV waits for a clipping DIV before replacing it. No intervening Q read. */
        if (u->q_ready) {
            q = u->q_next;
            q_flags = u->q_flags;
            u->q_ready = 0;
        }
        float denominator = _mm_cvtss_f32(arithmetic4(lane4(v[26], 3)));
        next_q_flags = denominator == 0 ? 0x20 : 0;
        next_q =
            denominator == 0
                ? vu_max(signbit(denominator))
                : _mm_cvtss_f32(arithmetic4(VU_DIV_SS(_mm_set_ss(1), _mm_set_ss(denominator))));
        acc = NMADD(acc, c[8], lane4(c[20], 1), 15);
        index = (index + 3) & 65535;
        vi[8] = (uint16_t)index;
        NATIVE_SET(9, NMUL(c[9], c[6], 14), 14);
        NATIVE_MOVE(29, 28);
        NATIVE_SET(5, NADD(c[13], lane4(c[25], 3), 2), 2);
        NATIVE_SET(5, NADD(c[24], c[13], 12), 12);
        STORE(index - 1, v[30], 15);
        pairs += issued;
        if (index != end) {
            q = next_q;
            q_flags = next_q_flags;
        }
    } while (index != end);
    NATIVE_SAVE();
    /* DIV at 2FA0 has issued five slots at exit; it completes in seven. */
    u->q_next = next_q;
    u->q_flags = next_q_flags;
    u->q_ready = u->cycle + 2;
    return pairs;
#undef NATIVE_SAVE
#undef cpu
#undef NADD
#undef NMUL
#undef NMADD
#undef qv
#undef LOAD
#undef STORE
}
static unsigned native_geometry_2dc8(Vu *u) {
    if ((u->status & 0xc0) == 0xc0) return native_geometry_2dc8_impl(u, 1);
    return native_geometry_2dc8_impl(u, 0);
}
