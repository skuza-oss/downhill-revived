/* Included first in every unit of the PC build (port/tools/transform.py adds it). */
#ifndef PORT_H
#define PORT_H
#include "fps.h"

/* SN EE long long is a 128-bit integer, unlike the host long long. */
typedef int ps2_int128 __attribute__((mode(TI)));
typedef unsigned int ps2_uint128 __attribute__((mode(TI)));

#ifdef __cplusplus
extern "C" {
#endif

/* EE CVT.W.S saturates, unlike an overflowing native C float-to-int cast. */
int ps2_cvt_w_s(float value);
/* EE finite division for native decompiled callers, including zero durations. */
float ps2_fdiv(float numerator, float denominator);
float ps2_fmul(float left, float right);
float ps2_fadd(float left, float right);
float ps2_fsub(float left, float right);

/* One asm operand: where its value lives on the host and how many bytes it has. */
typedef struct {
    void *p;
    unsigned int size;
} ps2_op;

/* Runs a MIPS/VU0 instruction sequence (an inline asm template) on the emulated register file.
   constraints: the operands' constraint strings joined by '|', outputs first. */
int host_skip_movies(void);
void host_movie_start(const char *name, int audio);  /* port/hle/movie.c (finding 055) */
int host_movie_update(int keep);                     /* nonzero: the movie is over */

void ps2_store32(unsigned int address, unsigned int value);
void ps2_store64(unsigned int address, unsigned long long value);

void ps2_asm(const char *tmpl, const char *constraints, int n, ps2_op *ops);

#ifdef __cplusplus
}
#endif

/* An inline asm statement with a literal template (transform.py): fn is the function
   port/tools/ps2asm_aot.py generates for that template, constraints and count, with the operand
   binding folded in; it runs ps2_asm the first time and its own code once the run-time parse is
   found identical (finding 090 SP13). The wasm build (build_wasm.py, PS2_ASM_DIRECT) calls it; the
   others call ps2_asm. The asm label gives the block-scope declaration C linkage in C++ too. */
#ifdef PS2_ASM_DIRECT
#define PS2A(fn, tmpl, constraints, n, ops) do { \
    void fn(const char *, const char *, int, ps2_op *) __asm__(#fn); fn(tmpl, constraints, n, ops); } while (0)
#else
#define PS2A(fn, tmpl, constraints, n, ops) ps2_asm(tmpl, constraints, n, ops)
#endif

/* Type of an asm input operand's value (arrays decay to pointers, as on the PS2). */
#ifdef __cplusplus
#define PS2_INTYPE(e) __typeof__(+(e)) /* unary + also decays void *, which C++ can't add 0 to */
#else
#define PS2_INTYPE(e) __typeof__((e) + 0)
#endif

/* A fixed PS2 RAM address written as a number in the game's code (a buffer at the top of RAM):
   its host address in the emulated RAM (port/runtime/memory.h, port/tools/data.py). */
#define PS2_RAM(addr) (0x08000000u + (unsigned int)(addr))

/* Widescreen: the scale of the game's X field of view constants (port/patches.py), 1 unless
   DHD_WIDESCREEN (port/hle/host.c). */
#ifdef __cplusplus
extern "C" float port_fov_x;
extern "C" float port_ws_menu_focal(float);
extern "C" void port_ws_set_focal(int, float, float);
#else
extern float port_fov_x;
extern float port_ws_menu_focal(float);
extern void port_ws_set_focal(int, float, float);
#endif

#ifdef __cplusplus
/* A g++ 2.95 virtual call (port/tools/transform.py gcc_virtuals), inside the class: slot k + 1
   of the vtable at ps2_vptr holds the function and the adjustment of `this`. */
struct ps2_vslot {
    short delta, index;
    void *function;
};

/* g++ 2.95 member pointers: signed this delta, signed virtual index, then
   either a direct address or a signed offset to the object's vtable pointer. */
template<class R> struct ps2_member {
    short delta, index;
    unsigned int target;

    template<class C> ps2_member &operator=(R (C::*method)()) {
        struct { unsigned int function; int adjustment; } native;
        __builtin_memcpy(&native, &method, sizeof native);
        /* Virtual declarations are lowered to ordinary methods by the port. */
        if (native.function & 1 || native.adjustment != (short)native.adjustment) __builtin_trap();
        delta = (short)native.adjustment; index = -1; target = native.function;
        return *this;
    }

    R invoke(void *object) const {
        char *adjusted = (char *)object + delta;
        void *function = (void *)target;
        if (index >= 0) {
            const ps2_vslot *table = *(const ps2_vslot **)((char *)object + (short)target);
            const ps2_vslot *slot = table + index - 1;
            adjusted += slot->delta; function = slot->function;
        }
        return ((R (*)(void *))function)(adjusted);
    }
};
#define PS2_VSLOT(k) ((const ps2_vslot *)ps2_vptr + (k) + 1)
#define PS2_VCALL(type, k) ((type)PS2_VSLOT(k)->function)
#define PS2_VTHIS(k) ((void *)((char *)this + PS2_VSLOT(k)->delta))
#endif

/* g++ 2.95 minimum/maximum operators (a <? b, a >? b). */
#define PS2_MIN(a, b) ({ PS2_INTYPE(a) ps2_min_a = (a); PS2_INTYPE(b) ps2_min_b = (b); \
                         ps2_min_a < ps2_min_b ? ps2_min_a : ps2_min_b; })
#define PS2_MAX(a, b) ({ PS2_INTYPE(a) ps2_max_a = (a); PS2_INTYPE(b) ps2_max_b = (b); \
                         ps2_max_a > ps2_max_b ? ps2_max_a : ps2_max_b; })

#endif
