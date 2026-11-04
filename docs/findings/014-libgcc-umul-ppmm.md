# 014: libgcc's `umul_ppmm` is longlong.h inline asm

Status: current
Checked: 2026-11-04

Decided 2026-10-12; `tools/source_rules.py` allows exactly this form.

```c
#define umul_ppmm(w1, w0, u, v) \
  __asm__ ("multu %2,%3" \
           : "=l" ((UWtype) (w0)), \
             "=h" ((UWtype) (w1)) \
           : "d" ((UWtype) (u)), \
             "d" ((UWtype) (v)))
```

This is the MIPS definition from gcc's `longlong.h`, which `libgcc2.c` includes for the 64-bit
division routines (`__udivdi3`, `__umoddi3`, `__divdi3`, `__moddi3`). It is the original source
text, not a steering trick.

## Evidence

- `func_001FBDD8` is `__udivdi3` (inlined `__udivmoddi4`, `-sce29`). In clean C the closest
  attempt left 3 of 372 words: retail has `multu; mfhi; mflo`, the compiler emits the EE
  three-operand `mult` plus `multu; mfhi`.
- A direct probe with cc1:
  every 32-bit multiply spelling gives the 3-operand `mult`; a shared 64-bit product gives one
  `multu` but `mflo` first and 7 extra words; the 64-bit compare collapses to one `sltu`.
- With the macro above the function matches (0 of 372 words, whole image MATCH).

## Rule

Only this exact form (operands `x`, `(T) x` or `(T) (x)` with `T` = `UWtype`/`USItype`; no
`volatile`, no clobbers, no extra instructions), and only in libgcc routines that include
longlong.h. Every other `multu`/`mult` asm still gets a try_func WARNING and is rejected.
