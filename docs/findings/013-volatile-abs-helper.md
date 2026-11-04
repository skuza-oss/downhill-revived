# 013: the game's `Abs` is a volatile `abs.s` asm helper

Status: current
Checked: 2026-11-04

Accepted by the user 2026-10-11; `tools/source_rules.py` allows exactly this form.

```c
static inline float Abs(float x)
{
    float r;
    __asm__ volatile ("abs.s %0,%1" : "=&f"(r) : "f"(x));
    return r;
}
```

## Evidence

- Every helper of the game's inlined vector library (WORKER.md "The game's vector helpers",
  436 matched VU0 functions) is one `volatile` asm statement. An `Abs` in the same style fits.
- With `fabsf` (gcc emits a plain `abs.s`) the scheduler and delay-slot filler move loads and
  constants across the `abs.s`; retail never does in these functions. A volatile asm stops
  that, because reorg's `redundant_insn()` / `resource_conflicts_p` and sched treat volatile
  insns as barriers. A non-volatile asm `abs.s` behaves like `fabsf`.
- The same helper gives whole-image matches in three unrelated functions:
  - `func_0010FDA0` and `func_0010F0D8` (promoted before this decision and marked rule-breaking
    then): `fabsf` leaves 233 of 398 words different in 0010F0D8.
  - `func_00148F60` (2026-10-11): the only difference left with `fabsf` was a `bc1f`
    target at 0x00149034. dbr finds the earlier identical `lw a0,0x2B4(s1)` and skips the
    join reload, unless a volatile insn sits between them. With `Abs` all 301 words match.
- `func_001656A0` was first rejected for a plain asm `abs.s`, and later matched
  in C (a872640f): many functions don't need the barrier.

## Rule

Use `fabsf` by default. Use `Abs` only when retail keeps code from moving across the `abs.s`.
Any other `abs.s` asm (non-volatile, extra operands, clobbers) still gets a try_func WARNING
and is rejected.
