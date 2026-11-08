# 002 — Compiler identified: SN ProDG 3.01 (ee-gcc 2.95.3 SN BUILD 1.36 + ps2eeas)

Status: current
Supersedes: 001
Checked: 2026-11-04

Supersedes the verdict table in `001-compiler-identification.md`.

## Result
Game code was built with **SN Systems ProDG for PS2 3.01**:
- compiler `cc1`/`cc1plus` "2.95.3 SN BUILD 1.36" (byte-identical to decomp.me `ee-gcc2.95.3-136`),
- assembler **SN `ps2eeas` 1.9.25.758** (`Ps2EeAs.exe`), *not* GNU as,
- flags **`-O2 -G8 -fopt-stack`**.

12 of 14 hand-written test functions are byte-exact under this one configuration
(`tools/match/confirm.py`). No other compiler/flag set tested gets more than 1–2 of them.

| Function | Kind | Result |
|---|---|---|
| func_10A208 | leaf, `$gp` (.sdata) global defined in TU | MATCH |
| func_10A2B8 | if/else, two calls, branch to shared epilogue | MATCH |
| func_10A388 | one call, arg shuffling | MATCH |
| func_10A3B0 | stack buffer, two calls (the old 2.96 problem case) | MATCH |
| func_10A468 | one call, `%hi/%lo` global | MATCH |
| func_10A5D0 | search loop over 64-entry table (R5900 short-loop nop) | MATCH |
| A::func_10A6E8 | **C++ method**, `$s0` save, call via member | MATCH |
| func_10AFB0 | float constants (`li.s` → `lui/ori/mtc1`), two returns | MATCH |
| func_10B290 | int↔float conversion, `.rodata` float | MATCH |
| func_10B8E8 | early-out + call | MATCH |
| func_10D778 | short-circuit `&&`/`\|\|`, branch-likely | MATCH |
| A::func_11D148 | **C++ method, `switch` jump table** | MATCH (code bytes; table data unchecked) |
| func_10A3F0 | `$s0/$s1` saves, float mul, two calls | DIFF: prologue now exact; one delay-slot choice (`sw $0,D_43F348` vs `li $a1,7`) — source detail not found yet |
| func_10A528 | `$s0–$s2`, 128-bit copies | DIFF: target copies the matrix with `lq $t0–$t3` then `sq` — looks like inline asm (memory operands); compiler output differs only there |

Negative controls (constant changed, wrong call target, wrong global address) all grade DIFF.

## Why earlier candidates were wrongly ruled out
- SN 2.95.3 was dropped in 001 for `sq` callee saves. That is just the default: SN's own
  `-fopt-stack` ("Optimise stack frame", undocumented in `--help`, found in `cc1.exe` strings)
  gives `sd`, 8-byte slots and the retail frame size.
- The decomp.me package runs GNU as. Retail used SN's `ps2eeas`, which (a) inserts the R5900
  short-loop `nop`, (b) always expands `li.s` inline (`lui/ori/mtc1`) even with `-G8`, while
  GNU as uses the `.lit4` pool. That's why one TU can have both `$gp` data and inline floats.
- SCE 2.96 cannot be it: its `cc1` rounds float literals wrong (0.3f → `0x3E999999`,
  retail `0x3E99999A`; same for 0.4f).
- ee-gcc 3.2 (beta2/030926/040921) loads float constants from `.rodata` and stores `(int)`
  conversions via `mfc1+sw`; retail synthesises them inline and uses `swc1`.

## Source-level rules learned (for agents)
- `-G8`: scalars ≤ 8 bytes are `$gp`-relative. A global reached via `lui/%lo` in retail is a
  bigger object (struct/array): declare it as such (e.g. `extern const float D_39E818[];`).
- `ps2eeas` does not apply a trailing `.extern sym,size` retroactively, so an extern scalar
  becomes `lui $at`+offset, not `$gp`. Retail `$gp` scalars in `.sdata` are *defined* in the TU
  (gcc 2.95 puts `= 0` initialisers in `.sdata`).
- A callee getting a pointer left in `$a0` (with `nop` in the `jal` slot) hints at a C++
  method call on that object whose body ignores `this`.
- `switch` cases that return the same value as `default` should share the `default` label;
  separate `case X: return 0;` blocks make duplicate epilogues.

## Running the toolchain
`ps2cc.exe` (the ProDG driver) refuses to run without a ProDG licence. Grading drives the
pieces directly: `ee-gcc.exe` (decomp.me 1.36 package) → `-S` → `Ps2EeAs.exe -G8`.
Cc spec in tools: `ee-gcc2.95.3-136@sn-prodg-3.01` (compiler@assembler package).
`ps2cc` also passes `-m5900 -mips3 -mabi=eabi` to cc1; these made no difference on the test set.

## Artifacts (gitignored under `tools/compilers/`)
- decomp.me `ee-gcc2.95.3-136` from `ps2_compilers.tar.xz`
  (sha256 f337a65a4fbab5cf0e335efe266dcbc7cf69b19c665c6b3a8924736df151a02a).
- `tools/compilers/sn-prodg-3.01/usr/...` fetched from
  https://github.com/AngheloAlf/SN-Systems-ProDG_for_PS2_3.01 (commit d74f6fe0):
  - `Ps2EeAs.exe` cb5adda955e64626564212ef7e0c1434708c4e1ef423344a92ec8033306ed3aa
  - `cc1.exe` 0393bcd31f91a6b9f0255db97f1cc99eba78ee8fc003e9a04dfabed1ae1d522e (= decomp.me 136)
  - `cc1plus.exe` 78a0df900a396098986cbc97a8d3eab6dbbc587a343d4dbd22929a4112c003fb (= decomp.me 136)

## Grader (`tools/match/grade.py`) — limits to fix in Phase 1
- Compares one function's words with the ELF; relocated fields are checked against the
  address encoded in `func_XXXXXX` / `D_XXXXXX` symbol names (`R_MIPS_26`, `HI16/LO16`
  pairs, `GPREL16` with `_gp = 0x2A40F0`). Relocs against local/section symbols (jump tables,
  string literals) are masked, not checked; jump-table *data* is not compared.
- Function size comes from a `jr $ra` heuristic (`disfn.py`); replace with splat boundaries.
- SCE libraries (SDK 2.7.0) and the `10A208` area near crt0 may use other compilers/flags —
  classify per TU later.
