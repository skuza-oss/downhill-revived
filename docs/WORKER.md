# Worker guide: decompile one function to matching C

You get a list of functions (claimed for you in the queue). For each one, write C (or C++)
that compiles with the retail toolchain to **exactly** the retail bytes. Only
`tools/try_func.py` decides whether it matches — never claim a match it did not print.

Work quietly: nobody reads your messages until the end. No plans, progress narration or
explanations between tool calls; spend the effort on reasoning and try_func runs. The only
text that matters is the final report (format at the end), with its notes kept short.

## Loop for one function `FUNC`

1. Read the asm: `asm/nonmatchings/text/FUNC.s` (do not edit anything under `asm/`).
2. First draft: `python tools/ext/m2c/m2c.py --target mipsee-gcc-c asm/nonmatchings/text/FUNC.s`
   (m2c output is a starting point, often not matching and never final style).
3. Write the candidate to `build-try/cand/FUNC.c` (or `.cpp`, see C++ below).
4. Test: `python tools/try_func.py FUNC build-try/cand/FUNC.c`
   It first compiles only your file (~1–2 s) and compares the function with retail outside
   the fields the linker fills in (call targets, `%hi`/`%lo`, `$gp` offsets). If anything else
   differs, that's final: it prints `DIFF ... (quick check ...)` and `whole image: not built`,
   and built words with a relocation show `[reloc SYMBOL]`. Otherwise it builds the whole game
   image with your file in place (~15 s) and prints a side-by-side retail | built listing, `*`
   on differing words, then `MATCH`/`DIFF` and whether the rest of the image still matches.
   `--quiet` shows only the verdict; `--compact` only the first 10 differing words with 1 line of
   context; `--full-build` skips the quick check.
5. Iterate on the source until `MATCH FUNC` and `whole image: MATCH`.
6. Report (format at the end). Do not promote, commit, or edit files outside `build-try/`.

Budget, handoff notes and the short list of rules: docs/WORKER-CORE.md (read it first; this file
is the reference behind it).

Run one `try_func` per shell command (a full build takes ~15–30 s while other workers run;
a loop over many functions exceeds the 2-minute command timeout). If a function's asm has no
`jr`/`j` at all, or is only epilogue pieces (`addiu $sp,...` with nothing before), it is a
dead-strip leftover, not a function: report FAIL "remnant" without trying.

## The toolchain (facts, verified)

- Compiler: SN ProDG 3.01, gcc 2.95.3 (SN BUILD 1.36), `-O2 -G8 -fopt-stack`; assembler SN
  ps2eeas. C++ is compiled with `-fno-exceptions` by default. 1999–2003 era C: declare
  variables at block start in C; no `//`-only features beyond what gcc 2.95 accepts; no C99.
- Names: use the splat names exactly — `func_0010A3B0` (8 hex digits), data `D_0029EA20`,
  jump tables `jtbl_00288C20`. Library functions already have real names (`memcpy`, `strlen`,
  ... see `config/symbol_addrs.txt`). Declare everything you call/use `extern` in your file.
- `-G8` small data (finding 004). The assembler only uses `$gp` for a scalar whose size it
  has already seen, except in branch delay slots (always `$gp`). So for a global scalar:
  - `$gp`-relative in retail **outside** a delay slot → it was defined earlier in the same
    file: define it in your file before the function with its retail initial value
    (`int D_0029D5C4 = 20;`, read the value in `asm/data/sdata.sdata.s`) and pass its range
    (`--data .sdata=0x29D5C4:0x29D5CC`, from its `dlabel` to the next symbol);
  - reached with `lui`/`%lo` (or `$gp` only in delay slots) → `extern int D_x;`.
  A bigger object (array/struct) reached with `lui`/`%lo`: declare it as an array or struct
  (`extern float D_0039E818[];` then `D_0039E818[0]`).
- m2c drafts can drop call arguments and miss that a callee receives a pointer still in
  `$a0`; check every `jal` against the registers set before it.
- Float constants: retail builds them inline (`lui $at,0x4248; mtc1`), the compiler does the
  same for literal constants — just write `50.0f`.
- Struct fields: retail offsets are what matter. Use a struct with padding to place fields
  (`char pad0[0x74]; struct Obj *x74;`), name fields `xNN` by offset unless the meaning is
  obvious. Field *types* change codegen (signed/unsigned loads `lh`/`lhu`, `lb`/`lbu`).
- Register allocation and instruction order respond to how you write things: order of
  statements, temporaries, `if`/`else` shape, `a ? b : c`, `&&`/`||`, loop form (`for` vs
  `while`), and variable types. If only registers differ, try reordering/expressing the same
  computation differently before anything else.
- Store order: with several independent stores, gcc 2.95's scheduler emits the **last-written**
  store first, then the rest in order (in tiny leaf functions the first one may land in the
  `jr` delay slot). So retail `x8, x0, x4` comes from source `x0; x4; x8;`.
- Types: `int` and `long` sizes differ from what you may expect: `long` is 64-bit on this
  EE gcc and `long long` is 128-bit (TImode: `por`/`sq`). Use `long` for 64-bit `ld`/`sd` fields.
- `$v0` set to a constant in a leaf function does not mean it returns it: the allocator often
  puts a stored constant in `$v0` in a `void` function. Check the callers before choosing.
- A global written through `lui $vN` + `%lo` (not an assembler `$at` expansion) was declared
  as an array/struct; a `$gp` store kept out of the delay slot (with a `nop` after `jr`)
  means the variable was `volatile`.
- `switch`: cases returning the same value as `default` should share the `default` label;
  separate `case X: return 0;` blocks create duplicate epilogues.
- 128-bit by-value arguments (e.g. names packed into one register: `pcpyld`/`dsll`/`ori`)
  match with `typedef unsigned int u128 __attribute__((mode(TI)));` constants.
- C++ virtual calls: gcc 2.95 vtable entries are 8 bytes (delta at +0, function at +4) and the
  first virtual is at +8, so a call through `0x20(vtbl)` is the 4th virtual.
- A callee getting a pointer left in `$a0` with a `nop` in the `jal` delay slot, where the
  callee ignores `$a0`, is a C++ method call (`obj->Method()`).

## Data your function owns

If the function has a **jump table** (`jr $vN` on a value loaded from `jtbl_...`) or its own
**string literals / constants in .rodata**, the compiled object will contain them, and you
must tell the build which retail range they replace:

    python tools/try_func.py FUNC build-try/cand/FUNC.c --data .rodata=0x288C20:0x288C50

Range = from the table's `dlabel` to the **next symbol's** address in `asm/data/rodata.rodata.s`
(the gap is alignment padding the linker reproduces). Same idea for `.data`, `.sdata` (a
`$gp` variable defined in this function's file: `int D_x = 0;`), `.sbss`, `.bss`.
A wrong range shows up as a link `ASSERT` or `whole image: NOT MATCHING`.

## Several functions in one file (a translation unit)

If neighbouring functions share data that only one file can define (e.g. they all `$gp`-access
a variable outside delay slots), write them together in one candidate whose first line declares
the whole range: `/* UNIT 0x002370E0 0x002372D0 .sdata=0x29DC38:0x29DC58 */` (start of the
first function, start of the function after the last). try_func and promote use the marker's
range, and a merge replaces the single-function files it covers. All functions in the range
must be in the file (C, or INCLUDE_ASM for ones you don't decompile).

If your function already sits inside such a file in `src/` as `INCLUDE_ASM(..., FUNC)`
(`grep -l "INCLUDE_ASM.*FUNC)" src/unsorted/*`; a plain `grep -l FUNC` also finds files that
only declare it `extern`, so check the function's address is inside the UNIT range), copy that
whole file to your candidate path and replace that line with your C. Keep its UNIT marker, and reuse its data definitions instead of redefining them.
A candidate covering only your function would cut the existing unit in half, and it can't be
promoted.

Matched siblings are the best hint: grep `src/` for the callees or a distinctive constant of
your function. A promoted function with the same shape often shows the exact C form needed.

## Unoptimized (-O0) files

A few files were built without optimization (known: 0x21E9A8–0x21EAA0, 0x2212A0–0x221608).
Their functions start `addiu $sp,..; sq $fp,..($sp); daddu $fp,$sp,$zero` even when they're
leaf functions that don't need a frame, and they store arguments to the stack and reload them.
For those, put the flags in a UNIT marker on the candidate's first line:

    /* UNIT 0x002214B0 0x002214E0 -O0 -fno-opt-stack */

(function start, next function start). Then try_func and promote use those flags. Say in NOTES
that the function is -O0.

## Files built with other flags or another compiler

Read the prologue before writing C; it tells you how the file was built. Allowed flag changes
(UNIT marker, as above) are only these three:

- `-fno-opt-stack` (some optimized files, e.g. around 0x2013F8–0x2018C8): registers are saved with
  `sq` (16-byte slots), e.g. `sq $s0,0x10($sp)`. The default `-fopt-stack` gives `sd` with 8-byte
  slots. If retail has `sq` saves and your build has `sd` (or the frame size differs), add it.
- `-O0 -fno-opt-stack`: the unoptimized files above.
- `-sce29`: Sony's own ee-gcc 2.9 instead of SN's compiler (finding 010; known: 0x201030–0x201160).
  Its signature: `sd` saves **16 bytes apart** with `$ra` above them (`sd $s0,0x0($sp)`,
  `sd $ra,0x10($sp)`, frame 0x20), restored `$ra` first. Neither SN mode produces that.

Nothing else (no other -O levels, no -f options) unless a finding says so.

## C++

Use a `.cpp` file. A method `int A::func_0011D148()` gets the mangled name
`func_0011D148__1A`; the build links mangled names to splat's plain ones automatically, so
just name the method after the function. `this` arrives in `$a0`.

## Register and order near misses: look, don't guess

When every instruction is right but values sit in other registers, run

    python tools/regalloc.py build-try/<your cand dir>/FUNC.c

It prints the compiler's global allocation order: each value's references, live length, priority
(about `refs * log2(refs) / live`) and the register it got. Higher priority is allocated first and
takes the first free register; an argument keeps `$a0..` only if no higher-priority value that is live
at the same time took it first. So to move a value to another register, change its live range or
its number of uses (compute it later, store it in its own variable, merge two variables), not the
order of unrelated statements. For instruction-*order* diffs, `--passes --keep DIR` also keeps the
first and second scheduling dumps (`.sched`, `.sched2`) and the delay-slot dump (`.dbr`): find the
pass where your order first differs from retail. A delay-slot difference belongs to `.dbr`, not to
the C order.

More tools for these near misses (each has a usage docstring at the top of its file):
- `docs/findings/012-sn-gcc-matching-levers.md`: C forms proven to steer this compiler
  (register ties, store order, cse/combine, madd fusion, loop hoisting, min.s/max.s). Read the
  section for your kind of diff before inventing forms.
- `tools/passdump.py FUNC SRC [--show PASS]`: the function's RTL after every cc1 pass; find the
  first pass where the build departs from retail.
- `tools/adiff.py FUNC SRC`: aligned diff when the sizes differ (try_func's listing shifts).
- `tools/sweep.py FUNC --base B.c --spec S.py`: build and score many variants of one candidate in
  parallel. Keep the spec file in your candidate dir; sweep writes its variants under
  `build-try/sweep/`, which is allowed.

Stop rules: if three different C variants give byte-identical output, the change you are making
doesn't reach the compiler; try another kind of change. If the compiler's `.s`
(`build-try/<tag>/FUNC/...c.s`) already has retail's instructions and order but the object
differs, the difference is in the assembler or data (relocation, `$gp` access, literal), not the
C: say so in NOTES.

## When try_func shows no listing

- `BUILD FAILED` with `cannot move location counter backwards` or an ASSERT `... is too big`:
  your function compiled **larger** than retail (or a `--data` range is too small), so it
  overlaps the next piece. Shrink it / fix the range.
- To debug, the compiler's assembly is kept at `build-try/<FUNC>/build-try/cand/<FUNC>.c.s`
  (with `--tag T`: `build-try/T/<FUNC>/build-try/<your cand dir>/<FUNC>.c.s`),
  and `tools/compilers/ps2dev-binutils/mips64r5900el-ps2-elf-objdump.exe -dr` works on the
  `.o` next to it.

## VU0 / MMI instructions

gcc 2.95 cannot emit VU0 macro-mode (`lqc2`/`sqc2`/`vadd`/`vmul`/`qmtc2`/...), most MMI
(`pxor`, `pextlw`, ...) or the EE FPU extras (`mula.s`/`madda.s`/`madd.s`, `sqrt.s` written
as `c1 0x...`) instructions; the original source used inline asm for them (as Sony's
`libvu0` headers do). For those instructions only, inline asm is allowed:

    __asm__ volatile ("sqc2 $vf0, 0x0(%0)" : : "r"(p) : "memory");

Also allowed: the game's 16-byte vector copy, `lq $2` + `sq $2` through `$v0` (a scheduling
barrier gcc won't reproduce from C). Use the macro already in promoted files
(`src/unsorted/0010EC80.c`):

    #define COPY_VECTOR(dst, src) \
        __asm__ volatile ("lq $2, 0x0(%1)\n\tsq $2, %0" : "=m"(dst) : "r"(src) : "$2")

The same goes for privileged EE instructions the SDK wraps in inline asm (`ei`, `di`, `sync`,
`sync.p`, `mfc0`/`mtc0`, `cache`), e.g. `__asm__ volatile ("sync\n\tei");`.

Keep each asm statement minimal (just the VU0/MMI instructions), let the compiler do
everything else in C, and say in NOTES that the function uses inline asm. Exception: you may
copy the body of a Sony `libvu0` routine as an asm block, including the GPR/FPU moves it contains
(e.g. `sceVu0ScaleVector`'s `mfc1 $8,%2` + `qmtc2 $8,$vf6`). That's how the original source
was written. If matching needs
anything beyond that, report FAIL with the closest attempt.

### The game's vector helpers (proven forms)

436 VU0 functions are matched. They use a small vector library that the original code inlined:
`Sub`, `Add`, `Scale`, `Dot`, `Normalize`, `Length`, `Cross`, `ApplyMatrix`, `Copy`. Each helper is
**one `volatile` asm statement**: the whole sequence in one string, scratch registers `$vf11`–`$vf14`,
`$2` for GPR↔VU moves, `$8`–`$11` for matrix copies, the result stored with `sqc2 ..., %0` into an
`"=m"` operand. Write a retail sequence as the helper that produces it, as a `static inline`
function (or macro) at the top of your file, and call it from C. Proven forms (file = an example):

| Retail sequence | Form | Example |
|---|---|---|
| `lqc2;lqc2;vsub.xyz;sqc2` (also `vadd`) | `"lqc2 $vf11, 0(%1)\n\tlqc2 $vf12, 0(%2)\n\tvsub.xyz $vf11, $vf11, $vf12\n\tsqc2 $vf11, %0" : "=m"(*d) : "r"(a), "r"(b)` | `0010BF50.c` |
| `lqc2;mfc1;qmtc2.ni;vmulx.xyz;sqc2` (scale) | `... mfc1 $2, %2\n\tqmtc2.ni $2, $vf12\n\tvmulx.xyz $vf11, $vf11, $vf12x ...` : `"=m"(*out) : "r"(v), "f"(s) : "$2"` | `0010ED50.c` |
| `mula.s;madda.s;madd.s` (dot / squared length) | `"mula.s %1,%1\n\tmadda.s %2,%2\n\tmadd.s %0,%3,%3" : "=&f"(sum) : "f"(x), "f"(y), "f"(z)` (+ `\n\tsqrt.s %0,%0` for length) | `0010DEF8.c`, `0010C108.c` |
| `...madd.s;sqrt.s;lui;mtc1;nop;div.s` (1/length) | one statement incl. `lui $2, 0x3f80\n\tmtc1 $2, %0\n\tnop\n\tdiv.s %0, %0, %1`, outputs `"=&f"(inv), "=f"(len)`, clobber `"$2"` | `0010F710.c` |
| `lqc2×4; vmulax; vmadday; vmaddz; sqc2` (matrix × vector) | `vmulax.xyz ACC, $vf12, $vf11x\n\tvmadday.xyz ACC, $vf13, $vf11y\n\tvmaddz.xyz $vf11, $vf14, $vf11z` | `0010B998.c` |
| `lqc2;lqc2;vopmula;vopmsub;sqc2` (cross) | `vopmula.xyz ACC,$vf11,$vf12\n\tvopmsub.xyz $vf11,$vf12,$vf11` | `00123DB0.c` |
| `lq $2;sq $2` (16-byte copy) | `COPY_VECTOR` above | `0010EC80.c` |
| `lq $8..$11; sq ×4` (matrix copy) | one statement, outputs `"=m"(row[0..3])`, clobbers `"$8","$9","$10","$11"` | `0010A528.c` |
| `rsqrt.s` / `sqrt.s` / `min.s` / `max.s` alone | `"rsqrt.s %0,%1,%2" : "=&f"(inv) : "f"(1.0f), "f"(sum)` | `0010DEF8.c` |
| `abs.s` that loads/stores don't cross (`Abs`) | `static inline float Abs(float x) { float r; __asm__ volatile ("abs.s %0,%1" : "=&f"(r) : "f"(x)); return r; }`. Only this exact form; use `fabsf` when retail lets code move across the abs.s (finding 013) | `0010FDA0.c` |
| libgcc 64-bit division (`__udivdi3` etc.): `multu` + `mfhi`/`mflo` | longlong.h `umul_ppmm`: `__asm__ ("multu %2,%3" : "=l" ((UWtype) (w0)), "=h" ((UWtype) (w1)) : "d" ((UWtype) (u)), "d" ((UWtype) (v)))`. Only this exact form, only in libgcc routines (finding 014) | `001FBDD8.c` |

Grep `src/unsorted/` for the retail instruction sequence (e.g. `vopmula`) to find more. If your
prompt names a MATCHED RELATIVE, its helpers are the first thing to copy.

When the order of instructions *inside* a helper differs, change the asm string. When code *around*
the helpers differs, change the C (argument order, temporaries, which helper runs first). If after ~10
tries the retail VU0 code interleaves with ordinary instructions in a way no asm-statement boundary
explains (no helper shape fits, and no matched function shows such a form), stop and report FAIL with
NOTES starting `vu0-no-helper-form:`, so a reviewer can check it for hand-written code.

## Forbidden

- Inline asm for anything gcc can generate itself, and `register ... asm("$N")` tricks.
  (The build's `INCLUDE_ASM` is only for undecompiled functions inside multi-function files;
  you don't need it.)
- Editing `tools/`, `config/`, `asm/`, `include/`, `src/`, or compiler flags. If you think a
  flag or tool is wrong, say so in the report.
- Claiming a match from your own reading of the asm. Only `try_func.py`'s output counts.
- Putting game data (asm, bytes or text copied from the disc or the executable beyond what the
  decompiled function needs), a key or other secret, a server address other than
  `wss://dhd.skuza.app`, or a path of your own machine (drive letters, home folders, LAN
  addresses) into any file you write. The repository's rules are in `AGENTS.md`.

`try_func.py` checks these rules before compiling (`tools/source_rules.py`): `REJECTED` (your
function still `INCLUDE_ASM`, register pinning) is never a match, and a `WARNING` about inline
asm means final verification rejects the file unless it is one of the cases above. Drop that
approach rather than tuning it.

## Style (the code must stay readable)

Real names when the meaning is clear from the code (`count`, `flags`, `pos`); otherwise
`xNN`/`unk_NN` by offset. Struct definitions at the top of the file. Comments only for
non-obvious facts you established (e.g. "bit 14 of flags = visible").

Offsets you find in outside notes (e.g. rider `+0x40C0`) hold in our build only where the struct
layout is the same; trust only what your matching code confirms, and check against the asm.

## NONMATCHING C (functions that never matched)

The last functions that no worker matched have behaviour-equivalent C next to their original asm
(finding 074). The default build keeps the asm, so it still matches byte for byte; the C is built
only with `-DNONMATCHING` (`python tools/configure.py --nonmatching && python -m ninja -f
build-nonmatching/build.ninja`, a separate build dir that links but is not checked):

    #ifdef NONMATCHING
    /* Non-matching: <what differs>; best diff N/M words; notes: build-try/<dir>/FUNC.notes.md */
    <declarations the function needs, extern only (never define data here), then the function>
    #else
    INCLUDE_ASM("asm/nonmatchings/text", FUNC);
    #endif

The function still counts as asm everywhere (queue, report, promote: they look for its
`INCLUDE_ASM`). Starting a new attempt, the C in the block is a good first candidate. When one
matches, replace the whole block with the matching C.

## Report (one block per function)

    FUNC: func_0010B920
    RESULT: MATCH | FAIL
    FILE: build-try/cand/func_0010B920.c
    DATA: .rodata=0x...:0x...        (only if used)
    ATTEMPTS: 4
    NOTES: what you learned (types, struct fields, why it was hard, closest diff if FAIL)
