# 010 — Some engine code was built with SCE ee-gcc 2.9; -fno-opt-stack files

Status: current
Checked: 2026-11-04

Date: 2026-10-09.

## SCE ee-gcc 2.9 file at 0x201030

func_00201030, func_002010A0 and func_00201110 had been failed by every worker with the same
4-word diff: retail saves `sd $s0,0x0($sp)` / `sd $ra,0x10($sp)` in a 0x20 frame and restores
`$ra` first. SN 2.95.3 gives either `sd` with 8-byte slots (`-fopt-stack`, frame 0x10) or `sq`
(`-fno-opt-stack`). Finding 001 had seen exactly this stride from the SCE ee-gcc 2.9 builds
("`sd`, but 16-byte slot stride") and ruled them out for the game as a whole.

The workers' candidates, compiled with `tools/compilers/ee-gcc2.9-991111` (`-O2 -G8`) and assembled
with SN `ps2eeas` as usual, are byte-exact: all three now match (whole image MATCH). GNU as is not
an option for these files: it writes `move` as `or`, retail has `daddu` (ps2eeas).

Build support: a UNIT marker flag `-sce29` makes `tools/sncc.py` compile the unit with that
compiler in WSL (Ubuntu). The 32-bit compiler can't stat files on `/mnt` (EOVERFLOW), so the source
and `include/` are copied to a temp dir on the Linux side first. The compiler itself is copied once
to `/opt/eegcc`.

Leaf functions in the same file (0x201010, 0x201080, 0x2010F0, 0x201160) already matched with
SN: a leaf has no prologue, so both compilers agree.

## -fno-opt-stack

23 matched units already use `-fno-opt-stack` alone, but WORKER.md only allowed flag changes for
-O0 files, so workers didn't try it. func_00201180 drops from 24 to 2 differing words with it.
WORKER.md now lists the three allowed build variants and their prologue signatures.

## Prologue census (unfinished game/engine functions, 2026-10-09)

Save stride across the first saves: `sd`/8 (SN default) 190, `sq` (any stride) 26, `sd`/16 (SCE) 3.
