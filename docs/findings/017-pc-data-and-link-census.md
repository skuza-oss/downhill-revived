# 017 — PC data conversion and link census

Status: history

Plan steps 3-4 of finding 015, 2026-10-12/13. The 32-bit (i686) link and its
census are retired (finding 088, `v0.3.1` is the last 32-bit release; `port/tools/link.py` is
gone). The data conversion survives, rebuilt for the wasm32 core: `port/tools/data.py` now writes
one x86 assembler block the runtime copies into emulated PS2 RAM (see its header; findings 081,
082). The census numbers below are a snapshot of where the port stood when it first linked.

## What the data stage did, and still does (data.py)
- The six splat data files (`asm/data/*.s`, generated from your own ELF, never committed) become
  one data image, `.vutext` through `.sdata` in PS2 address order; `.sbss`/`.bss` are zero.
- Every data label becomes an absolute symbol at its PS2 address (then: 32-bit `.long`,
  `.align`→`.p2align`, Win32 `_` prefix; now: `PS2_RAM_BASE + address`, no prefix).
- Code labels (`.L…` switch tables, ~6,100) and `$LEH_…` exception ranges keep their PS2 value /
  0; `D_8A0400` (PS2 `.bss` end, heap start) was linked to `_end`; `D_` inside `.vutext` points
  into `assets/vutext.bin`.
- 616 data objects are defined by the C sources, so the converted data leaves them out. A C
  object whose neighbour the code reaches by pointer arithmetic (e.g. `STR8` in 001BA3B8) can
  break that pairing; nothing detects it. (The portable-core state trace, finding 081, is how
  such breaks are now caught.)

## Census snapshot (2026-10-12/13), for the record
- 3,797 units + the asm runtime + the data linked with 0 duplicate and 0 other errors; 493
  undefined symbols, all accounted for: 294 SDK asm functions, 23 named SDK library functions,
  7 callback entry points, 114 libgcc functions, 26 C library/runtime, 29 unfinished
  game/engine/crt0 functions. Apart from the 29 unfinished functions, everything left was
  platform work (SDK + C library); the game's own code and data linked completely.

## Symbol rules that carried over to the wasm32 core
- **C++ names:** a C++ function's mangled name depends on the declaration its unit saw, while the
  data and the C units know only `func_X`; every mangled symbol whose demangled identifier is a
  retail name is mapped back to it (then `llvm-objcopy --redefine-syms`, 909 symbols; now
  `build_wasm.py` with `wasm_retail_names.py`).
- **Tentative definitions:** gcc 2.95 merged C tentative definitions; clang needs `-fcommon`
  (one case, `D_0029DE30`; still in `build_wasm.py`).
- **varargs:** two units define the EE's `va_start` with gcc 2.95 MIPS builtins; `port/patches.py`
  turns them into `__builtin_va_start`.

## Dead approach
- **Windows target choice (i686, retired):** the link used clang `i686-pc-windows-itanium` for the
  game and `-msvc` for host code, because windows-itanium lays out structs/bitfields as gcc does
  (MSVC moved a test struct's bitfields 7→16, size 32→48) and gives C++ methods cdecl under the
  Itanium ABI (mingw/MSVC thiscall would break C callers and the function tables). All moot after
  the 64-bit move (findings 082, 088).

Full text: `git show 860ab6b:docs/findings/017-pc-data-and-link-census.md`
