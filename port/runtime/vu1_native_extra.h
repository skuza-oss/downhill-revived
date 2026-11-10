/* Additional mesh loops sharing the native clipping routine. */
#include "vu1_native_1430.h"
#include "vu1_native_2dc8.h"
/* The registers 1638's loop writes (the others stay in the file as loaded). */
enum { WRITES_1638 = 1u << 5 | 1u << 7 | 1u << 8 | 1u << 9 | 1u << 10 | 1u << 11 | 1u << 12 | 1u << 20 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 26 | 1u << 28 | 1u << 29 | 1u << 30 };
static inline __attribute__((always_inline)) unsigned native_geometry_1638_impl(Vu *u, int sticky_complete)
{
    unsigned begin = u->c->vi[8], end = u->c->vi[10];
    unsigned distance = (end - begin) & 0xffff;
    if (!distance || distance % 3 || distance / 3 > 64 || (u->q_ready && u->q_ready > u->cycle)) { return 0; }
    ps2_q128 *memory = u->c->vu0_mem;
#define cpu (*u->c)
    drain_flags(u); update_q(u);
    unsigned entry_status = u->status;
    __m128 v[32], c[32], acc = _mm_loadu_ps(cpu.vacc);
    NATIVE_LOAD_FILE();
    NativeFlags flags = {_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_ps(), 0};
    flags.sticky_complete = sticky_complete;
    unsigned clip = u->clip, pairs = 0, index = begin;
    uint16_t *vi = cpu.vi;
    float q = cpu.q; unsigned q_flags = 0;
    __m128 qv = operand4(_mm_set1_ps(q));   /* Q as an operand: the file's clamped, a divide's as it is */
#define NATIVE_SAVE() do { \
    NATIVE_STORE_FILE(WRITES_1638); \
    _mm_storeu_ps(cpu.vacc,acc); cpu.q = q; \
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
        NATIVE_FULL(12, NMUL(c[10],c[31],15)); vi[9]=(uint16_t)memory[(index+6)&1023].w[3];
        NATIVE_SET(5, NADD(c[13],lane4(c[25],3),2), 2); STORE(index,v[8],14);
        NATIVE_SET(5, NADD(c[24],c[13],12), 12);
        float denominator = _mm_cvtss_f32(arithmetic4(lane4(v[26],3)));
        q_flags = denominator == 0 ? 0x20 : 0;
        q = denominator == 0 ? vu_max(signbit(denominator)) :
            _mm_cvtss_f32(arithmetic4(VU_DIV_SS(_mm_set_ss(1),_mm_set_ss(denominator)))); qv = _mm_set1_ps(q);
        NATIVE_SETRAW(24, minmax4(v[24],lane4(v[18],1),0), 1); NATIVE_MOVE(25, 26);
        NATIVE_RAW(12, minmax4(v[12],_mm_set1_ps(cpu.i),0)); NATIVE_RAW(10, LOAD(index+4));
        acc = NMUL(c[0],c[22],15); NATIVE_RAW(20, LOAD(181+vi[9]));
        clip=((clip<<6)|native_clip(v[5],lane4(v[13],3)))&0xffffff;
        NATIVE_SETLANE(26, 18, 3, 1);
        NATIVE_SETRAW(24, minmax4(v[24],lane4(v[0],0),1), 1); NATIVE_RAW(9, LOAD(index+8));
        NATIVE_RAW(12, native_ftoi(v[12],1)); NATIVE_MOVE(28, 5);
        NATIVE_FULL(10, native_itof(v[10],1)); vi[1]=q_flags;
        NATIVE_FULL(23, NMADD(acc,c[26],qv,15)); vi[1]=(clip&0x02fbef)!=0;
        NATIVE_SET(24, NADD(c[11],lane4(c[24],3),1), 1);
        if(q_flags) { NATIVE_SET(8, NADD(c[7],lane4(c[0],0),14), 14); }
        else { NATIVE_SET(8, NMUL(c[7],qv,14), 14); }
        STORE(index+1,v[12],15); native_vertices++;
        if (vi[1]) {
            native_clipped_vertices++;
            NATIVE_SAVE();
            unsigned clipped = native_clip_run(u, 0x1778/8);
            if (!clipped) return 0;
            pairs += clipped;
            NATIVE_LOAD_FILE();
            acc = _mm_loadu_ps(cpu.vacc); q=cpu.q;qv=operand4(_mm_set1_ps(q));
            clip=u->clip; entry_status=u->status; q_flags=u->status&0x30;
            memset(&flags,0,sizeof flags);
            flags.sticky_complete = sticky_complete;
        }
        acc = NMUL(c[4],lane4(c[0],3),15); NATIVE_MOVE(11, 7);
        NATIVE_FULL(7, native_itof(v[9],1.0f/4096)); index=(index+3)&65535;vi[8]=(uint16_t)index;
        NATIVE_RAW(30, native_ftoi(v[24],16));
        acc = NMADD(acc,c[1],lane4(c[20],0),15);NATIVE_MOVE(29, 28);
        acc = NMADD(acc,c[2],lane4(c[20],1),15);NATIVE_MOVE(24, 23);
        NATIVE_FULL(26, NMADD(acc,c[3],lane4(c[20],2),15));
        NATIVE_SET(7, NADD(c[7],c[19],12), 12);STORE(index-1,v[30],15);
        pairs+=21;
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
static unsigned native_geometry_1638(Vu *u) {
    if ((u->status & 0xc0) == 0xc0) return native_geometry_1638_impl(u, 1);
    return native_geometry_1638_impl(u, 0);
}

/* The registers 17e0's loop writes (the others stay in the file as loaded). */
enum { WRITES_17E0 = 1u << 5 | 1u << 7 | 1u << 8 | 1u << 9 | 1u << 11 | 1u << 20 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 26 | 1u << 28 | 1u << 29 | 1u << 30 };
static inline __attribute__((always_inline)) unsigned native_geometry_17e0_impl(Vu *u, int sticky_complete)
{
    unsigned begin = u->c->vi[8], end = u->c->vi[10];
    unsigned distance = (end - begin) & 0xffff;
    if (!distance || distance % 3 || distance / 3 > 64) { return 0; }
    ps2_q128 *memory = u->c->vu0_mem;
#define cpu (*u->c)
    drain_flags(u); update_q(u);
    unsigned entry_status = u->status;
    __m128 v[32], c[32], acc = _mm_loadu_ps(cpu.vacc);
    NATIVE_LOAD_FILE();
    NativeFlags flags = {_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_ps(), 0};
    flags.sticky_complete = sticky_complete;
    unsigned clip = u->clip, pairs = 0, index = begin;
    uint16_t *vi = cpu.vi;
    float q = cpu.q; unsigned q_flags = u->status & 0x30;
    __m128 qv = operand4(_mm_set1_ps(q));   /* Q as an operand: the file's clamped, a divide's as it is */
#define NATIVE_SAVE() do { \
    NATIVE_STORE_FILE(WRITES_17E0); \
    _mm_storeu_ps(cpu.vacc,acc); cpu.q = q; \
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
        acc = NMUL(c[4],lane4(c[0],3),15);NATIVE_RAW(9, LOAD(index+8));
        acc = NMADD(acc,c[1],lane4(c[20],0),15);vi[9]=(uint16_t)memory[(index+9)&1023].w[3];
        clip=((clip<<6)|native_clip(v[5],lane4(v[13],3)))&0xffffff;NATIVE_SETMOVE(24, 23, 14);
        acc = NMADD(acc,c[2],lane4(c[20],1),15);STORE(index,v[8],14);
        NATIVE_SET(24, NADD(c[11],lane4(c[14],3),1), 1);NATIVE_MOVE(25, 26);
        NATIVE_FULL(28, NADD(c[5],lane4(c[0],0),15));vi[1]=q_flags;
        NATIVE_SET(23, NMUL(c[26],qv,14), 14);
        NATIVE_FULL(26, NMADD(acc,c[3],lane4(c[20],2),15));vi[1]=(clip&0x02fbef)!=0;
        if(q_flags) { NATIVE_SET(8, NADD(c[7],lane4(c[0],0),14), 14); }
        else { NATIVE_SET(8, NMUL(c[7],qv,14), 14); }
        NATIVE_RAW(20, LOAD(181+vi[9]));native_vertices++;
        if (vi[1]) {
            native_clipped_vertices++;
            NATIVE_SAVE();
            unsigned clipped = native_clip_run(u, 0x1900/8);
            if (!clipped) return 0;
            pairs += clipped;
            NATIVE_LOAD_FILE();
            acc = _mm_loadu_ps(cpu.vacc); q=cpu.q;qv=operand4(_mm_set1_ps(q));
            clip=u->clip; entry_status=u->status; q_flags=u->status&0x30;
            memset(&flags,0,sizeof flags);
            flags.sticky_complete = sticky_complete;
        }
        NATIVE_RAW(30, native_ftoi(v[24],16));NATIVE_MOVE(11, 7);
        NATIVE_SET(5, NADD(c[13],lane4(c[25],3),2), 2);index=(index+3)&65535;vi[8]=(uint16_t)index;
        NATIVE_SET(5, NADD(c[23],c[13],12), 12);
        float denominator = _mm_cvtss_f32(arithmetic4(lane4(v[26],3)));
        q_flags = denominator == 0 ? 0x20 : 0;
        q = denominator == 0 ? vu_max(signbit(denominator)) :
            _mm_cvtss_f32(arithmetic4(VU_DIV_SS(_mm_set_ss(1),_mm_set_ss(denominator)))); qv = _mm_set1_ps(q);
        NATIVE_FULL(7, native_itof(v[9],1.0f/4096));
        NATIVE_FULL(29, NMUL(c[28],lane4(c[0],3),15));STORE(index-1,v[30],15);
        pairs+=15;
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
static unsigned native_geometry_17e0(Vu *u) {
    if ((u->status & 0xc0) == 0xc0) return native_geometry_17e0_impl(u, 1);
    return native_geometry_17e0_impl(u, 0);
}

/* The registers 20c's loop writes (the others stay in the file as loaded). */
enum { WRITES_20C = 1u << 5 | 1u << 7 | 1u << 9 | 1u << 10 | 1u << 11 | 1u << 12 | 1u << 15 | 1u << 20 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 28 | 1u << 29 | 1u << 30 };
static inline __attribute__((always_inline)) unsigned native_geometry_20c_impl(Vu *u, int sticky_complete)
{
    unsigned begin = u->c->vi[8], end = u->c->vi[10];
    unsigned distance = (end - begin) & 0xffff;
    if (!distance || distance % 3 || distance / 3 > 64) { return 0; }
    ps2_q128 *memory = u->c->vu0_mem;
#define cpu (*u->c)
    drain_flags(u); update_q(u);
    unsigned entry_status = u->status;
    __m128 v[32], c[32], acc = _mm_loadu_ps(cpu.vacc);
    NATIVE_LOAD_FILE();
    NativeFlags flags = {_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_ps(), 0};
    flags.sticky_complete = sticky_complete;
    unsigned clip = u->clip, pairs = 0, index = begin;
    uint16_t *vi = cpu.vi;
    float q = cpu.q; unsigned q_flags = u->status & 0x30;
    __m128 qv = operand4(_mm_set1_ps(q));   /* Q as an operand: the file's clamped, a divide's as it is */
#define NATIVE_SAVE() do { \
    NATIVE_STORE_FILE(WRITES_20C); \
    _mm_storeu_ps(cpu.vacc,acc); cpu.q = q; \
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
        acc = NMUL(c[0],c[22],15);NATIVE_RAW(12, LOAD(index+4));
        NATIVE_FULL(24, NMADD(acc,c[25],qv,15));vi[9]=(uint16_t)memory[(index+3)&1023].w[3];
        NATIVE_SET(5, NADD(c[13],lane4(c[30],3),2), 2);vi[1]=q_flags&0x20;
        NATIVE_SET(10, NMUL(c[9],qv,14), 14);
        NATIVE_FULL(7, native_itof(v[12],1));cpu.i=255;
        if(vi[1])NATIVE_SET(10, NADD(c[9],lane4(c[0],0),14), 14);
        NATIVE_SET(5, NADD(c[24],c[13],12), 12);NATIVE_SETMOVE(11, 9, 1);
        NATIVE_SETRAW(24, minmax4(v[24],lane4(v[18],1),0), 1);NATIVE_RAW(15, LOAD(373+vi[9]));
        NATIVE_RAW(23, minmax4(v[23],_mm_set1_ps(cpu.i),0));NATIVE_RAW(20, LOAD(181+vi[9]));
        acc = NMUL(c[4],lane4(c[0],3),15);STORE(index,v[10],14);
        clip=((clip<<6)|native_clip(v[5],lane4(v[13],3)))&0xffffff;NATIVE_RAW(9, LOAD(index+5));
        NATIVE_SETRAW(24, minmax4(v[24],lane4(v[0],0),1), 1);NATIVE_SETMOVE(10, 15, 14);
        NATIVE_RAW(30, native_ftoi(v[23],1));NATIVE_MOVE(28, 5);
        acc = NMADD(acc,c[1],lane4(c[20],0),15);NATIVE_SETMOVE(23, 15, 1);
        acc = NMADD(acc,c[2],lane4(c[20],1),15);vi[1]=(clip&0x02fbef)!=0;
        NATIVE_FULL(25, NMADD(acc,c[3],lane4(c[20],2),15));
        NATIVE_SET(24, NADD(c[11],lane4(c[24],3),1), 1);STORE(index+1,v[30],15);
        native_vertices++;
        if(vi[1]) {
            native_clipped_vertices++;NATIVE_SAVE();
            unsigned clipped=native_clip_run(u,0x1198/8);if(!clipped)return 0;pairs+=clipped;
            NATIVE_LOAD_FILE();
            acc = _mm_loadu_ps(cpu.vacc);q=cpu.q;qv=operand4(_mm_set1_ps(q));clip=u->clip;entry_status=u->status;q_flags=u->status&0x30;
            memset(&flags,0,sizeof flags);
            flags.sticky_complete = sticky_complete;
        }
        NATIVE_SET(23, NMUL(c[23],lane4(c[23],3),1), 1);index=(index+3)&65535;vi[8]=(uint16_t)index;
        NATIVE_FULL(7, NMUL(c[7],c[8],15));NATIVE_MOVE(29, 28);
        NATIVE_FULL(9, native_itof(v[9],1.0f/4096));
        float denominator=_mm_cvtss_f32(arithmetic4(lane4(v[25],3)));
        q_flags=denominator==0?0x20:0;
        q=denominator==0?vu_max(signbit(denominator)):_mm_cvtss_f32(arithmetic4(VU_DIV_SS(_mm_set_ss(1),_mm_set_ss(denominator)))); qv = _mm_set1_ps(q);
        NATIVE_RAW(24, native_ftoi(v[24],16));NATIVE_SETMOVE(30, 25, 1);
        acc = NMUL(c[31],lane4(c[23],3),15);NATIVE_SETMOVE(25, 18, 1);
        acc = native_merge(acc, NMADD(acc,c[26],lane4(c[0],3),14), 14);
        NATIVE_FULL(23, NMADD(acc,c[7],c[10],15));
        NATIVE_SET(9, NADD(c[9],c[19],12), 12);STORE(index-1,v[24],15);
        pairs+=25;
    }while(index!=end);
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
static unsigned native_geometry_20c(Vu *u) {
    if ((u->status & 0xc0) == 0xc0) return native_geometry_20c_impl(u, 1);
    return native_geometry_20c_impl(u, 0);
}

/* The registers 260's loop writes (the others stay in the file as loaded). */
enum { WRITES_260 = 1u << 9 | 1u << 12 | 1u << 15 | 1u << 20 | 1u << 21 | 1u << 23 | 1u << 25 | 1u << 26 | 1u << 28 | 1u << 29 | 1u << 30 };
static inline __attribute__((always_inline)) unsigned native_geometry_260_impl(Vu *u, int sticky_complete)
{
    unsigned begin = u->c->vi[8], end = u->c->vi[10];
    unsigned distance = (end - begin) & 0xffff;
    if (!distance || distance % 3 || distance / 3 > 64) { return 0; }
    ps2_q128 *memory = u->c->vu0_mem;
#define cpu (*u->c)
    drain_flags(u); update_q(u);
    unsigned entry_status = u->status;
    __m128 v[32], c[32], acc = _mm_loadu_ps(cpu.vacc);
    NATIVE_LOAD_FILE();
    NativeFlags flags = {_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_ps(), 0};
    flags.sticky_complete = sticky_complete;
    unsigned clip = u->clip, pairs = 0, index = begin;
    uint16_t *vi = cpu.vi;
    float q = cpu.q; unsigned q_flags = u->status & 0x30;
    __m128 qv = operand4(_mm_set1_ps(q));   /* Q as an operand: the file's clamped, a divide's as it is */
#define NATIVE_SAVE() do { \
    NATIVE_STORE_FILE(WRITES_260); \
    _mm_storeu_ps(cpu.vacc,acc); cpu.q = q; \
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
        NATIVE_FULL(9, native_itof(v[9],1.0f/4096));vi[9]=(uint16_t)memory[(index+6)&1023].w[3];
        acc = NMUL(c[12],c[28],15);vi[6]=(uint16_t)memory[(index+3)&1023].w[3];
        acc = native_merge(acc, NMADD(acc,c[13],lane4(c[0],3),14), 14);cpu.i=255;
        NATIVE_FULL(29, NMADD(acc,c[14],lane4(c[23],3),15));index=(index+3)&65535;vi[8]=(uint16_t)index;
        NATIVE_SET(30, NADD(c[9],lane4(c[24],3),1), 1);NATIVE_RAW(12, LOAD(index+1));
        NATIVE_SET(30, NMUL(c[25],qv,14), 14);NATIVE_RAW(15, LOAD(373+vi[6]));
        NATIVE_SET(21, NMUL(c[9],qv,14), 14);NATIVE_RAW(20, LOAD(181+vi[9]));
        NATIVE_RAW(29, minmax4(v[29],_mm_set1_ps(cpu.i),0));NATIVE_RAW(9, LOAD(index+2));
        acc = NMUL(c[4],lane4(c[0],3),15);
        float denominator=_mm_cvtss_f32(arithmetic4(lane4(v[26],3)));
        q_flags=denominator==0?0x20:0;
        q=denominator==0?vu_max(signbit(denominator)):_mm_cvtss_f32(arithmetic4(VU_DIV_SS(_mm_set_ss(1),_mm_set_ss(denominator)))); qv = _mm_set1_ps(q);
        NATIVE_RAW(30, native_ftoi(v[30],16));NATIVE_SETMOVE(25, 26, 14);
        acc = NMADD(acc,c[1],lane4(c[20],0),15);NATIVE_SETMOVE(28, 15, 14);
        NATIVE_RAW(29, native_ftoi(v[29],1));NATIVE_SETMOVE(23, 15, 1);
        acc = NMADD(acc,c[2],lane4(c[20],1),15);STORE(index-3,v[21],15);
        NATIVE_FULL(12, native_itof(v[12],1));STORE(index-1,v[30],15);
        NATIVE_FULL(26, NMADD(acc,c[3],lane4(c[20],2),15));
        NATIVE_SET(23, NMUL(c[23],lane4(c[23],3),1), 1);STORE(index-2,v[29],15);
        native_vertices++;pairs+=16;
    }while(index!=end);
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
static unsigned native_geometry_260(Vu *u) {
    if ((u->status & 0xc0) == 0xc0) return native_geometry_260_impl(u, 1);
    return native_geometry_260_impl(u, 0);
}

/* The unclipped mesh loop B 08B0..0930 (dispatch 0x100: MSCAL 07F0, then MSCNT 1040 or 1CB0).
   Three input and three output qwords a vertex, no CLIP and no flag reads. Its DIV at 08D8 is read
   eight pairs later (0918/0920), after the divider's latency, so each vertex sees its own Q.
   The two accumulator products 0908 (vf0 * vf22) and 0930 (vf4 * vf0w) read registers the loop
   never writes: each is computed once, its sticky flags are the same every time. The vertex count
   is checked before anything is written; any other count leaves the pair-by-pair path to run. */
/* The registers 800's loop writes (the others stay in the file as loaded). */
enum { WRITES_800 = 1u << 7 | 1u << 8 | 1u << 9 | 1u << 10 | 1u << 11 | 1u << 12 | 1u << 20 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 26 | 1u << 28 };
static inline __attribute__((always_inline)) unsigned native_geometry_800_impl(Vu *u, int sticky_complete)
{
    unsigned begin = u->c->vi[8], end = u->c->vi[10];
    unsigned distance = (end - begin) & 0xffff;
    if (!distance || distance % 3 || distance / 3 > 341 || (u->q_ready && u->q_ready > u->cycle)) return 0;
    ps2_q128 *memory = u->c->vu0_mem;
#define cpu (*u->c)
    drain_flags(u); update_q(u);
    unsigned entry_status = u->status;
    __m128 v[32], c[32], acc = operand4(_mm_loadu_ps(cpu.vacc));
    NATIVE_LOAD_FILE();
    NativeFlags flags = {_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_ps(), 0};
    flags.sticky_complete = sticky_complete;
    unsigned clip = u->clip, pairs = 0, index = begin;
    uint16_t *vi = cpu.vi;
    float q = cpu.q; unsigned q_flags = 0;
    __m128 qv = operand4(_mm_set1_ps(q));   /* Q as an operand: the file's clamped, a divide's as it is */
#define NADD(a,b,d) native_add_c(&flags,(a),(b),(d))
#define NMUL(a,b,d) native_mul_c(&flags,(a),(b),(d))
#define NMADD(a,b,c_,d) native_madd_c(&flags,(a),(b),(c_),(d))
#define LOAD(a) _mm_loadu_ps(memory[(a) & 1023].f)
#define STORE(a,x) _mm_storeu_ps(memory[(a) & 1023].f, (x))
    /* Loop invariants: I (MINIi), vf18, vf19, vf1-vf4, vf22, vf31. */
    const __m128 limit = _mm_set1_ps(cpu.i), zero_x = lane4(v[0], 0);
    const __m128 acc_0908 = NMUL(c[0], c[22], 15);
    const __m128 acc_0930 = NMUL(c[4], lane4(c[0], 3), 15);
    do {
        NATIVE_SETRAW(23, minmax4(v[23], lane4(v[18], 1), 0), 1); NATIVE_MOVE(11, 7);            /* 08B0 */
        NATIVE_FULL(7, native_itof(v[9], 1.0f / 4096.0f));                                  /* 08B8 */
        NATIVE_RAW(10, minmax4(v[10], limit, 0));                                          /* 08C0 */
        NATIVE_FULL(12, native_itof(v[28], 1.0f)); NATIVE_SETMOVE(24, 23, 14);                      /* 08C8 */
        NATIVE_SETRAW(24, minmax4(v[23], zero_x, 1), 1);                                     /* 08D0 */
        vi[9] = (uint16_t)memory[(index + 9) & 1023].w[3];
        NATIVE_SET(7, NADD(c[7], c[19], 12), 12);                                         /* 08D8 */
        float denominator = _mm_cvtss_f32(arithmetic4(lane4(v[26], 3)));
        q_flags = denominator == 0 ? 0x20 : 0;
        q = denominator == 0 ? vu_max(signbit(denominator)) :
            _mm_cvtss_f32(arithmetic4(VU_DIV_SS(_mm_set_ss(1.0f), _mm_set_ss(denominator)))); qv = _mm_set1_ps(q);
        NATIVE_RAW(10, native_ftoi(v[10], 1.0f)); index = (index + 3) & 0xffff;            /* 08E0 */
        acc = NMADD(acc, c[1], lane4(c[20], 0), 15); NATIVE_SETMOVE(25, 18, 1);            /* 08E8 */
        NATIVE_SET(24, NADD(c[11], lane4(c[24], 3), 1), 1); NATIVE_SETMOVE(25, 26, 14);           /* 08F0 */
        acc = NMADD(acc, c[2], lane4(c[20], 1), 15);                               /* 08F8 */
        STORE(index - 3, v[8]); NATIVE_FULL(26, NMADD(acc, c[3], lane4(c[20], 2), 15));     /* 0900 */
        STORE(index - 2, v[10]); acc = acc_0908;                                   /* 0908 */
        NATIVE_RAW(24, native_ftoi(v[24], 16.0f)); NATIVE_RAW(20, LOAD(181 + vi[9]));              /* 0910 */
        NATIVE_FULL(23, NMADD(acc, c[25], qv, 15)); NATIVE_RAW(28, LOAD(index + 4));    /* 0918 */
        NATIVE_SET(8, NMUL(c[7], qv, 14), 14); NATIVE_RAW(9, LOAD(index + 5));        /* 0920 */
        NATIVE_FULL(10, NMUL(c[12], c[31], 15));                                            /* 0928 */
        acc = acc_0930; STORE(index - 1, v[24]);                                   /* 0930 */
        native_vertices++;
        pairs += 17;
    } while (index != end);
    vi[8] = (uint16_t)index;
    /* The last FMAC is 0930's. */
    flags.last = VU_MUL4(native_clean(c[4]), native_clean(lane4(c[0], 3))); flags.mask = 15;
    NATIVE_STORE_FILE(WRITES_800);
    _mm_storeu_ps(cpu.vacc, acc); cpu.q = q;
    Upper last = {0}; fmac_flags4(&last, flags.last, flags.mask);
    unsigned mac = last.mac;
    unsigned status = (mac & 15 ? 1 : 0) | (mac & 240 ? 2 : 0) | (mac & 3840 ? 4 : 0) | (mac & 61440 ? 8 : 0);
    unsigned sticky = (_mm_movemask_ps(_mm_castsi128_ps(flags.zero)) ? 1 : 0) |
                      (_mm_movemask_ps(_mm_castsi128_ps(flags.sign)) ? 2 : 0) |
                      (_mm_movemask_ps(_mm_castsi128_ps(flags.overflow)) ? 8 : 0);
    u->status = (entry_status & ~0x3fu) | status | q_flags | (sticky << 6);
    u->mac = mac; u->clip = u->clip_next = clip; u->n_flags = 0; u->flag_head = 0;
    u->q_ready = 0; u->cycle += 64; u->flag_cycle = u->cycle;
    return pairs;
#undef cpu
#undef NADD
#undef NMUL
#undef NMADD
#undef LOAD
#undef STORE
}
static unsigned native_geometry_800(Vu *u) {
    if ((u->status & 0xc0) == 0xc0) return native_geometry_800_impl(u, 1);
    return native_geometry_800_impl(u, 0);
}

/* The end state of a kernel with no clipping path: the registers it wrote (NATIVE_STORE_FILE: v and
   cpu are the kernel's), then the flags as the exact path leaves them once every queued update has
   landed. The last FMAC gives MAC and the status bits
   0-3; sticky bits 6-9 are those on entry or set by any FMAC in the loop; the Q bits (4, 5) are
   q_flags. */
#define native_finish(u, written, acc, q, flags, entry_status, q_flags, clip) do { \
    NATIVE_STORE_FILE(written); \
    native_finish_flags((u), (acc), (q), (flags), (entry_status), (q_flags), (clip)); \
} while (0)
static void native_finish_flags(Vu *u, __m128 acc, float q, const NativeFlags *flags,
                                unsigned entry_status, unsigned q_flags, unsigned clip)
{
    _mm_storeu_ps(u->c->vacc, acc); u->c->q = q;
    Upper last = {0}; fmac_flags4(&last, flags->last, flags->mask);
    unsigned mac = last.mac;
    unsigned status = (mac & 15 ? 1 : 0) | (mac & 240 ? 2 : 0) | (mac & 3840 ? 4 : 0) | (mac & 61440 ? 8 : 0);
    unsigned sticky = (_mm_movemask_ps(_mm_castsi128_ps(flags->zero)) ? 1 : 0) |
                      (_mm_movemask_ps(_mm_castsi128_ps(flags->sign)) ? 2 : 0) |
                      (_mm_movemask_ps(_mm_castsi128_ps(flags->overflow)) ? 8 : 0);
    u->status = (entry_status & ~0x3fu) | status | q_flags | (sticky << 6);
    u->mac = mac; u->clip = u->clip_next = clip; u->n_flags = 0; u->flag_head = 0;
    u->q_ready = 0; u->cycle += 64; u->flag_cycle = u->cycle;
}

/* The unlit, unclipped mesh loop B 0798..07D8 (dispatch 0x0E0: MSCAL 06F0, then MSCNT 1040, 1CB0
   or 12E0), the sibling of 08B0's loop above: three input and output qwords a vertex, 9 pairs, no
   CLIP and no flag reads. 07A0's DIV is read by the next vertex's MULq at 0798/07A0, 8 pairs
   later, after the divider's 7 cycles, so each vertex sees the Q of the one before; the last
   DIV lands at 07D8, inside the loop, so the run leaves no division pending. 07D0's MULAw
   (vf4 * vf0w) reads registers the loop never writes: computed once, its sticky flags are the
   same every time. The last FMAC is 07D8's ADDw. Declines before writing anything when a
   division is pending or the count is not a whole number of vertices; the pairs then run as
   before. */
/* The registers 0798's loop writes (the others stay in the file as loaded). */
enum { WRITES_0798 = 1u << 7 | 1u << 8 | 1u << 9 | 1u << 20 | 1u << 24 | 1u << 25 | 1u << 26 | 1u << 28 };
static inline __attribute__((always_inline)) unsigned native_geometry_0798_impl(Vu *u, int sticky_complete)
{
    unsigned begin = u->c->vi[8], end = u->c->vi[10];
    unsigned distance = (end - begin) & 0xffff;
    if (!distance || distance % 3 || distance / 3 > 341 || (u->q_ready && u->q_ready > u->cycle)) return 0;
    ps2_q128 *memory = u->c->vu0_mem;
    drain_flags(u); update_q(u);
    const unsigned entry_status = u->status;
#define cpu (*u->c)
    __m128 v[32], c[32], acc = operand4(_mm_loadu_ps(cpu.vacc));
    NATIVE_LOAD_FILE();
    NativeFlags flags = {_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_ps(), 0};
    flags.sticky_complete = sticky_complete;
    unsigned pairs = 0, index = begin;
    uint16_t *vi = u->c->vi;
    float q = u->c->q; unsigned q_flags = 0;
    __m128 qv = operand4(_mm_set1_ps(q));   /* Q as an operand: the file's clamped, a divide's as it is */
#define NADD(a,b,d) native_add_c(&flags,(a),(b),(d))
#define NMUL(a,b,d) native_mul_c(&flags,(a),(b),(d))
#define NMADD(a,b,c_,d) native_madd_c(&flags,(a),(b),(c_),(d))
#define LOAD(a) _mm_loadu_ps(memory[(a) & 1023].f)
#define STORE(a,x) _mm_storeu_ps(memory[(a) & 1023].f, (x))
    const __m128 acc_07d0 = NMUL(c[4], lane4(c[0], 3), 15);
    do {
        
        NATIVE_SET(24, NMUL(c[25], qv, 14), 14); vi[9] = (uint16_t)memory[(index + 9) & 1023].w[3];   /* 0798 */
        NATIVE_SET(8, NMUL(c[7], qv, 14), 14);                                                          /* 07A0 */
        float denominator = _mm_cvtss_f32(arithmetic4(lane4(v[26], 3)));
        q_flags = denominator == 0 ? 0x20 : 0;
        q = denominator == 0 ? vu_max(signbit(denominator)) :
            _mm_cvtss_f32(arithmetic4(VU_DIV_SS(_mm_set_ss(1.0f), _mm_set_ss(denominator)))); qv = _mm_set1_ps(q);
        NATIVE_FULL(7, native_itof(v[9], 1.0f / 4096.0f)); index = (index + 3) & 0xffff;               /* 07A8 */
        acc = NMADD(acc, c[1], lane4(c[20], 0), 15); NATIVE_SETMOVE(25, 26, 14);                       /* 07B0 */
        NATIVE_RAW(28, native_ftoi(v[24], 16.0f)); NATIVE_RAW(9, LOAD(index + 5));                             /* 07B8 */
        acc = NMADD(acc, c[2], lane4(c[20], 1), 15); STORE(index - 3, v[8]);                   /* 07C0 */
        NATIVE_FULL(26, NMADD(acc, c[3], lane4(c[20], 2), 15)); NATIVE_RAW(20, LOAD(181 + vi[9]));              /* 07C8 */
        acc = acc_07d0;                                                                         /* 07D0 IBNE */
        NATIVE_SET(24, NADD(c[7], lane4(c[5], 3), 1), 1); STORE(index - 1, v[28]);                    /* 07D8 */
        native_vertices++;
        pairs += 9;
    } while (index != end);
    vi[8] = (uint16_t)index;
    native_finish(u, WRITES_0798, acc, q, &flags, entry_status, q_flags, u->clip);
    return pairs;
#undef cpu
}
static unsigned native_geometry_0798(Vu *u) {
    if ((u->status & 0xc0) == 0xc0) return native_geometry_0798_impl(u, 1);
    return native_geometry_0798_impl(u, 0);
}

/* The loop B 3140..3178 (dispatch 0x616: MSCAL 30A0, 0040 or 0020, then MSCNT 1040 or 12E0): 0798's
   loop without the perspective divide, 8 pairs a vertex (MULw by vf0w instead of MULq, a constant
   colour qword vf10 stored at +1). No CLIP, no flag, Q or P reads; the status Q bits stay as they
   are. The last FMAC is 3178's ADDw. */
/* The registers 3140's loop writes (the others stay in the file as loaded). */
enum { WRITES_3140 = 1u << 7 | 1u << 9 | 1u << 20 | 1u << 24 | 1u << 25 | 1u << 26 | 1u << 28 };
static inline __attribute__((always_inline)) unsigned native_geometry_3140_impl(Vu *u, int sticky_complete)
{
    unsigned begin = u->c->vi[8], end = u->c->vi[10];
    unsigned distance = (end - begin) & 0xffff;
    if (!distance || distance % 3 || distance / 3 > 341 || (u->q_ready && u->q_ready > u->cycle)) return 0;
    ps2_q128 *memory = u->c->vu0_mem;
    drain_flags(u); update_q(u);
    const unsigned entry_status = u->status;
#define cpu (*u->c)
    __m128 v[32], c[32], acc = operand4(_mm_loadu_ps(cpu.vacc));
    NATIVE_LOAD_FILE();
    NativeFlags flags = {_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_ps(), 0};
    flags.sticky_complete = sticky_complete;
    unsigned pairs = 0, index = begin;
    uint16_t *vi = u->c->vi;
    const __m128 one_w = lane4(v[0], 3);
    const __m128 acc_3170 = NMUL(c[4], one_w, 15);
    do {
        NATIVE_SET(24, NMUL(c[25], one_w, 14), 14); vi[9] = (uint16_t)memory[(index + 9) & 1023].w[3]; /* 3140 */
        NATIVE_FULL(7, native_itof(v[9], 1.0f / 4096.0f)); index = (index + 3) & 0xffff;                /* 3148 */
        acc = NMADD(acc, c[1], lane4(c[20], 0), 15); NATIVE_SETMOVE(25, 26, 14);                        /* 3150 */
        NATIVE_RAW(28, native_ftoi(v[24], 16.0f)); NATIVE_RAW(9, LOAD(index + 5));                              /* 3158 */
        acc = NMADD(acc, c[2], lane4(c[20], 1), 15); STORE(index - 2, v[10]);                    /* 3160 */
        NATIVE_FULL(26, NMADD(acc, c[3], lane4(c[20], 2), 15)); NATIVE_RAW(20, LOAD(181 + vi[9]));               /* 3168 */
        acc = acc_3170;                                                                          /* 3170 IBNE */
        NATIVE_SET(24, NADD(c[7], lane4(c[5], 3), 1), 1); STORE(index - 1, v[28]);                     /* 3178 */
        native_vertices++;
        pairs += 8;
    } while (index != end);
    vi[8] = (uint16_t)index;
    native_finish(u, WRITES_3140, acc, u->c->q, &flags, entry_status, entry_status & 0x30, u->clip);
    return pairs;
#undef NADD
#undef NMUL
#undef NMADD
#undef LOAD
#undef STORE
#undef cpu
}
static unsigned native_geometry_3140(Vu *u) {
    if ((u->status & 0xc0) == 0xc0) return native_geometry_3140_impl(u, 1);
    return native_geometry_3140_impl(u, 0);
}

/* The skinning loop 0468-0570 (MSCAL 0010/0020 through 03C8): a vertex blended from three bone
   matrices by weights, its normal transformed and lit, 34 pairs a vertex. The loop reads no
   flags, Q, P or clip, and the VU stalls a read until its producer lands, so the pairs run in
   program order (each upper before its lower: 0510's MADDAz reads vf5 before the MOVE replaces
   it, and no lower reads its own pair's upper result). The last FMAC is 0570's MADDw. Declines
   before writing anything when a division or EFU result is still pending or the count could
   pass the run's step limit. */
static inline __attribute__((always_inline)) unsigned native_skin_468_impl(Vu *u, int sticky_complete)
{
    int16_t count = (int16_t)u->c->vi[10];
    if ((u->q_ready && u->q_ready > u->cycle) || (u->p_ready && u->p_ready > u->cycle) || count > 8192) return 0;
    ps2_q128 *memory = u->c->vu0_mem;
#define cpu (*u->c)
    drain_flags(u); update_q(u);
    unsigned entry_status = u->status;
    /* ACC is written whole (0488) before the loop ends, and its XYZ before 0478 and 0480 read
       them: the file's is never an operand. Only the registers the loop writes go back. */
    enum { WRITES = 1u << 5 | 1u << 6 | 1u << 7 | 1u << 8 | 1u << 9 | 1u << 10 | 1u << 12 | 1u << 17 | 1u << 20 | 1u << 21 |
                    1u << 22 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 26 | 1u << 29 | 1u << 30 };
    __m128 v[32], c[32], acc = _mm_loadu_ps(cpu.vacc);
    NATIVE_LOAD_FILE();
    NativeFlags flags = {_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_ps(), 0};
    flags.sticky_complete = sticky_complete;
    uint16_t *vi = cpu.vi;
    unsigned pairs = 0;
    const __m128 i_reg = operand4(_mm_set1_ps(cpu.i)), one_w = lane4(v[0], 3), zero_x = lane4(v[0], 0);
#define NADD(a,b,d) native_add_c(&flags,(a),(b),(d))
#define NSUB(a,b,d) native_sub_c(&flags,(a),(b),(d))
#define NMUL(a,b,d) native_mul_c(&flags,(a),(b),(d))
#define NMADD(a,b,c_,d) native_madd_c(&flags,(a),(b),(c_),(d))
#define LOAD(a) _mm_loadu_ps(memory[(a) & 1023].f)
#define STORE(a,x,d) store_fields(memory[(a) & 1023].f, _mm_castps_si128(x), (d))
#define LANE_BITS(r,c) ((uint16_t)_mm_cvtsi128_si32(_mm_castps_si128(lane4(v[r], c))))
    do {
        NATIVE_SET(26, native_itof(v[26], 1.0f), 1); NATIVE_RAW(24, LOAD(vi[1] + 859));  /* 0468 */
        acc = native_merge(acc, NMUL(c[21], lane4(c[17], 0), 14), 14); NATIVE_SETMOVE(9, 5, 1); /* 0470 */
        acc = native_merge(acc, NMADD(acc, c[22], lane4(c[17], 1), 14), 14);           /* 0478 */
        vi[6] = vi[5] & vi[6];
        NATIVE_SET(25, NMADD(acc, c[23], lane4(c[17], 2), 14), 14); vi[3] = vi[5] & vi[3]; /* 0480 */
        acc = NMUL(c[21], lane4(c[5], 0), 15); NATIVE_RAW(17, LOAD(vi[8] + 192));      /* 0488 */
        acc = NMADD(acc, c[22], lane4(c[5], 1), 15); NATIVE_RAW(21, LOAD(vi[6] + 856)); /* 0490 */
        acc = NMADD(acc, c[23], lane4(c[5], 2), 15); NATIVE_RAW(22, LOAD(vi[6] + 857)); /* 0498 */
        NATIVE_FULL(6, NMADD(acc, c[24], one_w, 15)); NATIVE_RAW(23, LOAD(vi[6] + 858)); /* 04A0 */
        acc = NMUL(c[14], lane4(c[25], 0), 15);                                       /* 04A8 */
        acc = NMADD(acc, c[15], lane4(c[25], 1), 15);                                 /* 04B0 */
        NATIVE_FULL(25, NMADD(acc, c[16], lane4(c[25], 2), 15));                      /* 04B8 */
        NATIVE_SET(29, NSUB(c[10], c[26], 1), 1); NATIVE_RAW(24, LOAD(vi[6] + 859));   /* 04C0 */
        acc = NMUL(c[21], lane4(c[5], 0), 15); NATIVE_RAW(12, LOAD(vi[8])); vi[8]++;   /* 04C8 */
        acc = NMADD(acc, c[22], lane4(c[5], 1), 15); NATIVE_RAW(21, LOAD(vi[3] + 856)); /* 04D0 */
        acc = NMADD(acc, c[23], lane4(c[5], 2), 15); NATIVE_RAW(22, LOAD(vi[3] + 857)); /* 04D8 */
        NATIVE_FULL(7, NMADD(acc, c[24], one_w, 15)); NATIVE_RAW(23, LOAD(vi[3] + 858)); /* 04E0 */
        NATIVE_SET(30, NADD(c[9], c[29], 1), 1); NATIVE_RAW(24, LOAD(vi[3] + 859));    /* 04E8 */
        /* MAX of two clean values (0 4B8's result and zero) is one of them */
        NATIVE_FULL(25, minmax4(v[25], zero_x, 1)); STORE(vi[8] - 3, v[20], 14);       /* 04F0 */
        acc = NMUL(c[21], lane4(c[5], 0), 15); vi[1] = LANE_BITS(12, 0);              /* 04F8 */
        acc = NMADD(acc, c[22], lane4(c[5], 1), 15); vi[6] = LANE_BITS(12, 1);        /* 0500 */
        NATIVE_SET(10, NMUL(c[12], i_reg, 1), 1); vi[3] = LANE_BITS(12, 2);            /* 0508 */
        acc = NMADD(acc, c[23], lane4(c[5], 2), 15); NATIVE_MOVE(5, 12);               /* 0510 */
        NATIVE_FULL(17, native_itof(v[17], 1.0f / 4096.0f)); vi[1] = vi[5] & vi[1];    /* 0518 */
        NATIVE_SET(30, NSUB(c[0], c[30], 1), 1); vi[10]--;                             /* 0520 */
        NATIVE_FULL(8, NMADD(acc, c[24], one_w, 15));                                  /* 0528 */
        NATIVE_SET(25, NMUL(c[25], lane4(c[25], 3), 1), 1);                            /* 0530 */
        acc = native_merge(acc, NMUL(c[11], lane4(c[25], 0), 14), 14);                /* 0538 */
        acc = native_merge(acc, NMADD(acc, c[27], lane4(c[25], 1), 14), 14); NATIVE_RAW(21, LOAD(vi[1] + 856)); /* 0540 */
        NATIVE_SET(25, NMADD(acc, c[28], lane4(c[25], 2), 14), 14); NATIVE_RAW(22, LOAD(vi[1] + 857)); /* 0548 */
        NATIVE_SET(25, NMUL(c[25], lane4(c[25], 3), 1), 1); NATIVE_RAW(23, LOAD(vi[1] + 858)); /* 0550 */
        NATIVE_SETRAW(26, native_ftoi(v[10], 1.0f), 1);                                /* 0558 */
        acc = NMUL(c[6], lane4(c[9], 3), 15);                                         /* 0560 */
        acc = NMADD(acc, c[7], lane4(c[29], 3), 15);                                  /* 0568 IBGTZ vi10 */
        NATIVE_FULL(20, NMADD(acc, c[8], lane4(c[30], 3), 15)); STORE(vi[8] + 190, v[25], 15); /* 0570 (delay slot) */
        pairs += 34;
    } while ((int16_t)vi[10] > 0);
    NATIVE_STORE_FILE(WRITES);
    _mm_storeu_ps(cpu.vacc, acc);
    Upper last = {0}; fmac_flags4(&last, flags.last, flags.mask);
    unsigned mac = last.mac;
    unsigned status = (mac & 15 ? 1 : 0) | (mac & 240 ? 2 : 0) | (mac & 3840 ? 4 : 0) | (mac & 61440 ? 8 : 0);
    unsigned sticky = (_mm_movemask_ps(_mm_castsi128_ps(flags.zero)) ? 1 : 0) |
                      (_mm_movemask_ps(_mm_castsi128_ps(flags.sign)) ? 2 : 0) |
                      (_mm_movemask_ps(_mm_castsi128_ps(flags.overflow)) ? 8 : 0);
    u->status = (entry_status & ~0xfu) | status | (sticky << 6);
    u->mac = mac; u->n_flags = 0; u->flag_head = 0;
    u->cycle += 64; u->flag_cycle = u->cycle;
    return pairs;
#undef cpu
#undef NADD
#undef NSUB
#undef NMUL
#undef NMADD
#undef LOAD
#undef STORE
#undef LANE_BITS
}
static unsigned native_skin_468(Vu *u) {
    if ((u->status & 0xc0) == 0xc0) return native_skin_468_impl(u, 1);
    return native_skin_468_impl(u, 0);
}

#include "vu1_native_0e10.h"

#include "vu1_native_2e48.h"
