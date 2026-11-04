# 007 — Quick check before the full trial build

Status: current
Checked: 2026-11-04

Date: 2026-10-03.

## The quick check (tools/quick_check.py, try_func's default)

One try_func on an idle machine: configure 9.1 s (it re-reads all 5,690 asm
functions), link 8.2 s (4,100 objects), diff + image check 0.3 s, compile ≈1 s. In one batch,
387 of ~690 trials were DIFF and only 91 MATCH. Each DIFF paid for the full build.

try_func now compiles the candidate alone first and compares the function's bytes with retail,
masking only the bits a relocation covers (R_MIPS_26 target, HI16/LO16/GPREL16 immediates; any
other relocation masks the whole word). Every unmasked bit is final in the object, so a
difference there is a difference in the linked image. The quick check can prove DIFF (and
"compiled larger than retail" or a compile error), never MATCH. When nothing differs outside
relocated fields, or FUNC isn't a single function symbol in the object, try_func runs the full
build exactly as before. MATCH still comes only from the full build and `check_image.py`;
promote.py doesn't use the quick check at all.

Validation:
- All 3,421 C functions in the main build's objects pass the quick check (no false DIFF; 6 s).
- On 81 finished trial builds with an up-to-date object and ELF, it agreed with
  the full build every time: 27 DIFF / 54 MATCH, no disagreement.
- On a fully loaded PC: a near-miss DIFF takes 2 s instead of 21 s; a matching candidate takes the
  full-build time plus ≈1 s.
