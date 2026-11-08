# 018 — Static recompilation of the remaining MIPS functions

Status: partly superseded by 088 (the i686 link it ran in)
Checked: 2026-11-04

`port/tools/recomp.py` still does this, now for the wasm32 core
(`build_wasm.py` runs it; findings 081, 082); the i686 link (`link.py`, the 1 MB emulated stack
beside host code) is retired (finding 088). The VU0 entries it starts now run compiled
(`vu0_aot.py`, finding 090 SP1). The 2026-10-13 counts below are a snapshot.

The PC build links only C, so every function that is still asm (unfinished, hand-written,
remnant, libgcc) is turned into C mechanically by `port/tools/recomp.py`. This is a bridge to a
running game: a function's matching C replaces its recompiled version as soon as it exists.

## How it works

- Each function becomes `rc_<name>(void)`, its instructions as C statements on the emulated
  register file of the asm runtime (`ps2`, `port/runtime/ps2asm.h`). VU0 macro, MMI and
  `qmtc2`/`qmfc2`/`cfc2` instructions go through `ps2_exec("...")`, the same interpreter the inline
  asm uses (146 distinct instruction strings, all understood).
- A wrapper `<name>(...)` has the function's C signature, taken from the callers' declarations
  through clang's AST dump. It loads `$a0-$t3` and `$f12-$f19`, puts further arguments on a 1 MB
  emulated stack from `0($sp)` in 8-byte slots, and returns `$v0`/`$f0`.
- Calls out of recompiled code go to `rc_<callee>` when the callee is recompiled, otherwise to the
  native function with arguments taken from the registers by its signature. `double` is soft
  float on the EE: one GPR holding the IEEE bits, bit-cast at the native boundary.
- Delay slots run before their branch, branch-likely slots only when taken. `jr` on a register
  other than `$ra` is a switch over the function's labels; the converted jump tables hold PS2
  label addresses (`port/tools/data.py`), so the case values line up.
- EE semantics are kept where they differ from x86: float division by zero gives ±FLT_MAX,
  `sqrt.s` takes |x|, `cvt.w.s` truncates and saturates, integer division by zero has the EE's
  results, `mult`/`madd` write LO/HI and rd, the second pipeline (`mult1`, `div1`, `mflo1`, ...)
  has its own LO1/HI1.
- The build runs it to a fixed point (then `link.py`, now `build_wasm.py`): functions the link
  needs, then whatever those call, and functions referenced by pointer from the data (vtables,
  callback tables).

## Result (2026-10-13)

175 functions recompiled: the 29 unfinished game/engine/crt0 functions, 8 more they reach, 138
libgcc. Undefined symbols left: 319, all platform code (295 SDK asm, 23 SDK library functions,
`main`). 0 duplicates.

Other fixes in the same step:
- The 16 KB scratchpad is mapped at its PS2 address 0x70000000 (`ps2_scratchpad_init`), because
  matched C uses it by number as well; `D_7000xxxx` symbols are absolute.
- Undefined code addresses (labels in asm functions, callback entry points) become their PS2
  address.
- `fabsf`/`floorf` (inline-only in the MSVC x86 CRT) and gcc 2.95's `exception::what` are in
  `port/runtime/libc_shims.c`.
- Nine MMI instructions were added to the runtime: `paddw psubh pcgth pextlh ppach psraw pmulth
  pmfhi pmflo`, with the upper halves of LO/HI.

## Known limits

- `func_0021B7E8`/`func_0021B8A8` (hand-written VU0 code) start VU0 microprograms at 0x0, 0x180,
  0x238, 0x2E0, 0x5E8, 0x7B8, 0x808, 0x8D0, 0xB40, 0xBD0, 0xC50 and 0xCD0, which have no native
  version (finding 016 has the four that do). The VU0 micro interpreter ran them, checked
  against PCSX2's VU0 interpreter on captured race inputs (finding 021, round 5); since finding
  090 SP1 they are compiled to C (`vu0_aot.py`), checked against the uploaded words.
- `jal` doesn't set `$ra`, so libgcc's MIPS exception unwinder links but can't work.
- `jalr` passes 8 integer arguments only; a float argument through a function pointer would be lost.
- `bc0f` (wait for VU0) is treated as done.
