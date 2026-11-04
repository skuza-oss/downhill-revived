# 003 — Build system: retail image from asm + C, byte-identical

Status: current
Checked: 2026-11-04

## Result
`python tools/configure.py && python -m ninja` rebuilds SCUS_971.77's loaded image and
`tools/check_image.py` reports MATCH: entry point and every allocated section (.text .vutext
.data .rodata .gcc_except_table .sdata .sbss .bss) identical in address, size and bytes.
Functions decompiled to C/C++ are compiled with the retail toolchain and linked in place.

Not reproduced (by design): ELF/program headers, section order in the file, .reginfo, the
DVP overlay sections, symbols. They don't affect the loaded program; the original was
linked by SN's ps2ld.

## Pipeline
| Input | Tool | Notes |
|---|---|---|
| retail ELF | splat 0.50 (`tools/split.py`, `config/splat.yaml`) | 5,690 functions, one .s each; data sections; `.vutext` as a binary |
| function asm (not decompiled) | ps2dev binutils 2.45 `as -march=r5900` | `tools/asmprep.py`: VU0 `$ACC/$I/$Q/$R`; `.L` labels → `L_` (jump tables in .rodata reference them across objects) |
| C/C++ unit | SN `ee-gcc2953` -S → `ps2eeas` (`tools/sncc.py`) | INCLUDE_ASM'd functions converted by `tools/asm_sn.py` |
| data asm | ps2dev `as`, split around C-owned ranges | `.align` → `.org` to the retail address (see below) |
| link | ps2dev `ld` (`tools/link.py`) | every piece at its absolute address; ASSERTs on overlap/overrun; C++ name aliases |

## Problems solved (each would silently break matching)
- **Old binutils can't read splat asm**: SN/Sony gas 2.9 has no `%gp_rel`, no ABI register
  names. Modern binutils (ps2dev) assemble it exactly; R5900 incl. MMI and VU0 macro mode.
- **ps2eeas quirks** (for INCLUDE_ASM inside C units): only numeric registers; segfaults on
  `.macro` with `\label`; no `%gp_rel` (and crashes on `sym+off` in macro form); crashes on
  `c1 imm` (EE `sqrt.s`, which spimdisasm can't name). `asm_sn.py` expands label macros,
  writes `$gp` operands as numeric offsets from `_gp` (layout is fixed), and emits unnamed
  COP ops as `.word`. All 5,690 functions assemble with it.
- **Jump tables across objects**: `.L` labels never reach the symbol table; renamed `L_`.
- **splat's .sbss pad / bss_size**: `bss_size` counts from the end of .sdata (0x29DCF0);
  splat's trailing "unreferenced pad" in .sbss is the .bss alignment gap, dropped.
- **Cutting data at unaligned addresses**: `.align` is relative to the section start and
  raises the section alignment, so ld moved a piece cut at 0x29C104 to 0x29C108. Each `.align`
  becomes `.org` to the retail address of the next data line.
- **C++ exception frame info**: g++ 2.95 emits DWARF frame info into .data for every
  function; retail .data has only 4 CIEs, so C++ defaults to `-fno-exceptions`.
- **C++ names**: mangled `func_X__1A` vs splat's `func_X`: link-time PROVIDE aliases.
- **Extern small-data scalars**: cc1 (GAS mode) writes `.extern sym, size` at the end of the
  file; ps2eeas only uses sizes it has already seen, so the access became lui/%lo instead of
  `$gp`. sncc hoists `.extern` to the top (cc1's `-mmips-as` mode does the same but also moves
  top-level asm/INCLUDE_ASM ahead of the functions, so it can't be used).
- **Windows**: ninja's CreateProcess needs native absolute exe paths; no shell (`&&`).
  Each build dir has its own `builddir` so parallel trials don't share `.ninja_log`.

## Library identification (`tools/libmatch.py`, `config/libmatch.txt`)
Masked byte matching of archive members against .text (unique hits): 96 functions —
libgcc (soft-float, EH, frame) 0x1FC3A8–0x200A08; SCE SDK libs (libgraph, libmc, libmpeg,
libipu, libkernl, libmrpc, libsn, …), libm and SN's libc in 0x243BE0–0x260790. Matched with
SDK 2.4 archives (the game uses 2.7.0), so the regions are wider than the hits.
Region map used by the queue: crt0 0x10A000–0x10A208, game –0x1FC000, libgcc –0x201000,
engine 0x201000–0x243000 (s-reg saves `sd` like game code, and 3/3 probe functions matched
with the game toolchain and flags), sdk –end.

## Verification
- Negative controls: one changed instruction in asm, one changed constant in C, a wrong
  candidate in try_func — all reported as DIFF with the function and offset.
- 21 functions currently built from source (C and C++: jump table in .rodata, `$gp` variable
  defined in C, INCLUDE_ASM between C functions, a method with `$s0` save).
