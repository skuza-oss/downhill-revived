# 015: PC port, starting point and plan

Status: partly superseded by 088 (the 32-bit host of step 1; the renderer choice of step 7)
Checked: 2026-11-04

The starting point and steps 2-6 still describe how the port is built. Step 1
(32-bit x86) was replaced by a wasm32 core run 64-bit through wasm2c (findings 081, 082, 088), and step
7 is decided: the port's own GS renderer in Rust/wgpu (findings 085, 088). Notes inline.

Date: 2026-10-12. Decomp state: game 98.2% and engine 92.6% of code matched (4,526 of 4,593
functions; 28 left in the queue, 7 hand-written, 164 eight-byte remnants that aren't functions).

## Measured starting point

- **Native compile census** (`port/tools/native_census.py`): every `src/` unit through clang 23,
  `--target=i686-pc-linux-gnu -fsyntax-only`, no changes to the sources. **3,245 of 3,796 files
  compile as they are.** The 551 others have 2,082 errors:
  - MIPS inline asm (unknown registers `$vf11`, constraints `"l"`/`"h"`/`"d"` used the MIPS way):
    1,430 errors in about 320 files;
  - gcc 2.95 extensions: string literals with raw newlines (asm bodies, ~110 files) and g++ `>?`/`<?`
    (min/max, ~23 files);
  - about 150 ordinary C errors (bit-field wider than its type, pointer types clang rejects, one
    file per redefinition), fixable one by one.
- **Inline asm census** (`port/tools/asm_census.py`): 2,132 asm blocks, 529 distinct sequences,
  but one small vocabulary: VU0 macro mode (`lqc2/sqc2`, `vadd/vsub/vmul*/vopmula/vopmsub`,
  `qmtc2`), the EE FPU extras (`mula.s/madda.s/madd.s`, `sqrt.s/rsqrt.s`, `min.s/max.s`), 128-bit
  `lq/sq` copies, plus a few `sync`/`ei`. The top 30 sequences are vector copy, add, sub, scale,
  cross product, dot product, length, normalize and matrix x vector, copy-pasted in whitespace
  variants.
- **SDK surface**: decompiled code calls 189 distinct functions in the SDK region (802 functions,
  mostly unidentified; `config/libmatch.txt` names 97). The five most called (0x254D68 366 call
  sites, 0x254544, 0x254204, 0x254D08, 0x2466A0) come first.
- **Hardware access outside the SDK**: 31 files use DMA/VIF/GS register addresses
  (0x1000xxxx/0x1200xxxx), 30 the uncached mirrors, 15 the scratchpad (0x70000000).

## Plan

The port keeps the matching sources as they are. Everything PC-specific lives in `port/` and is
applied at build time, so the matching build and its checks never change.

1. **Host: 32-bit x86 first.** Pointers stay 4 bytes, so struct layouts, data files with embedded
   offsets and the game's pointer arithmetic work unchanged. 64-bit comes later, after types are
   cleaned up. *Now:* the game core is a wasm32 module (still 4-byte pointers), compiled to C by
   wasm2c and run on a 64-bit host on every platform (findings 081, 082); the i686 build is retired
   (finding 088).
2. **Source transform** (`port/tools`): turn each MIPS asm statement into a call to a small C
   runtime that executes that instruction sequence on a VU0/FPU register file (the vocabulary
   above is under 40 opcodes). Also join raw-newline strings and rewrite `a >? b`. Goal: every
   unit compiles for i686 (now: for wasm32, `port/tools/transform.py`, `build_wasm.py`).
3. **Data**: convert the splat data sections (`asm/data/*.s`, generated from the user's own ELF,
   never committed) to x86 assembler at build time (`.word` -> `.long`, symbol references kept).
   (Now `port/tools/data.py`, a data image copied into emulated PS2 RAM.)
4. **Link census**: link everything against empty stubs; the undefined-symbol list is the exact
   remaining work: unfinished asm functions, SDK calls, libgcc/libc.
5. **Leftover MIPS functions** (28 queue functions, 7 hand-written, libgcc): static recompilation
   to C (registers in a context struct). On a 32-bit host a PS2 pointer and a host pointer are the
   same value, so recompiled and native code can call each other with a thin bridge. libc, libm
   and libgcc are replaced by the host's. (Done: `port/tools/recomp.py`, finding 018; inside the
   wasm32 core a guest pointer is still the module's own address.)
6. **HLE runtime for the 189 SDK calls**, in this order:
   - boot, kernel threads/semaphores, memory;
   - CD reads from an extracted disc;
   - pad input;
   - the IOP RPC (sound) protocol;
   - graphics last.
   First target: the game runs headless to its first frame.
7. **Graphics**: the game sends DMA chains to VIF1/VU1 microcode (`.vutext`) which draws through
   the GS. The decision is open between emulating VIF1/VU1/GS (PCSX2 is GPL-3, compatible with this
   repo) and replacing the engine's draw calls with a modern renderer. It depends on a study of
   the engine's render path (engine region 0x201000-0x243000). *Decided:* the port emulates
   VIF1/VU1 and draws GS packets with its own renderer (Rust/wgpu, an exact and an upscaled native
   front end; findings 019, 085, 088); PCSX2's GS is only the referee (finding 025).
8. **Audio and video**: SPU2 through the IOP sound driver, MPEG movies through IPU (`sceMpeg*`).
   Study after graphics. (Done: findings 022, 055.)

This port runs the original code on a PC; it is not a reimplementation.
