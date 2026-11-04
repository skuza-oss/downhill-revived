# 001 — Compiler identification (superseded)

Status: superseded by 002

The first search for the game's compiler (2026-09). Resolved in finding 002:
SN ProDG 3.01 (cc1 "2.95.3 SN BUILD 1.36" + SN `ps2eeas`), `-O2 -G8 -fopt-stack`.

## Still true
- Retail ELF: stripped (no `.symtab`, no DWARF), `.text` 0x159bc0 bytes at 0x10a000, entry 0x10a008;
  `.gcc_except_table` (GNU toolchain, C++ with exceptions); `.vutext`/`.DVP.overlay.*` (VU
  microcode); library tags `PsIIlib* 2700` (SCE SDK 2.7.0).
- Prologues (`tools/prologue_fingerprint.py`): 3,763 `addiu $sp,$sp,-N`; `$ra` saved with `sd` in
  every one; callee-saved `$s` with `sd` 9,770, `sw` 147, `sq` 0, in 8-byte slots.
- `MOD/SNPROFIL.IRX` on the disc does not by itself mean the SN compiler.

## Wrong turns (why 002 was needed)
- SN 2.95.x builds were "ruled out" for their `sq` callee saves. That was a mistake: `sq` is only
  SN's default; its undocumented `-fopt-stack` gives `sd`, 8-byte slots and the retail frame.
- SCE ee-gcc 2.9 (990721/991111): `sd` but a 16-byte slot stride. Wrong for the game as a whole,
  but finding 010 found one engine file built with it.
- SCE ee-gcc 2.96 matched the prologues best, but no flag set (4 opt levels x 21 sub-flags x
  C/C++) reproduced the delay-slot choices of `func_10A3B0`, and its cc1 rounds float literals
  wrong (finding 002).
- ee-gcc 3.2 beta2 needed Cygwin DLLs and was never run; finding 002 rules out 3.2 by its float
  constant loads.

## Environment notes kept
- The SCE Linux compilers are 32-bit: WSL needs `libc6-i386`, and they fail with EOVERFLOW on
  `/mnt/*`, so they run from a copy on the Linux side (`tools/sncc.py` still does this for the
  `-sce29` files of finding 010).

Full text: `git show 860ab6b:docs/findings/001-compiler-identification.md`
