# src/: matching decompilation

The root `AGENTS.md` rules apply. This folder (and `include/`, `config/`, `asm/`) is the matching
source: every unit must compile with the retail toolchain to the retail bytes.

- The procedure for one function is `docs/WORKER.md` (short rules: `docs/WORKER-CORE.md`). The
  compiler and its quirks: findings 002, 004, 010, 012; allowed asm forms: 013, 014.
- A match is what `python tools/try_func.py FUNC FILE` prints as `MATCH FUNC` and
  `whole image: MATCH`, nothing else. `tools/promote.py` moves a candidate into `src/` only if the
  whole image still matches; `tools/check_image.py build/dhd.elf` checks a build.
- Never edit `src/` for the port. PC changes are patches in `port/patches*.py`, applied at build
  time by `port/tools/transform.py`. A port bug in matching code is fixed in a patch, and `src/`
  keeps its retail form.
- Do not edit `asm/` (generated from your own executable by `tools/split.py`, never committed).
- Functions that never matched keep behaviour-equivalent C under `#ifdef NONMATCHING` next to
  their `INCLUDE_ASM` (finding 074); they still count as asm.
- Names: real names once the meaning is clear, otherwise `xNN`/`unk_NN` by offset; structs at the
  top of the file or in `include/` once shared (finding 023).

Check before you hand back: `python tools/check.py matching` (configure, build, image check).
