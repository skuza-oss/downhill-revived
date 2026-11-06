# 167: Native VU1 rendering, phase 2: the remaining VU1 programs

Status: in progress

Branch `native/vu-gs` (local only: never pushed or merged until the owner decides), after
finding 164's phase 1 (`43d6ce2`), started 2026-11-06. This file is the lane's plan while it runs.

## Goal and acceptance

The owner approved phase 2 (2026-11-06, overnight): translate the rest of what finding 162's table
lists, in this order (cheapest and biggest first), behind the same switch (`DHD_NATIVE_VU1`, a list,
off by default), with anything unusual left on the VU and counted by reason:

1. the prelit world variants 1638, 07F0, 06F0, 17E0 (finding 164's family: two-sided, some without
   clipping);
2. the lit programs: 0040 and its variants 1430, 1050, 12F0, 0C20, 0A90, 0000, 30A0 (per-vertex
   lighting, rigid skinning, shadow projection);
3. the skinned riders and bikes, 0020 and 0010 (3-bone skinning, lighting, shadow silhouettes into
   the shadow render targets);
4. overlay A: particles and sprites (2E48, 29C0, 0E10, 02D0, ...);
5. 2DC8 (the second matrix, likely an environment map).

Per program or family: correctness with finding 164's tolerance (pictures switch off vs on across
finding 156's scenes at 2x and 4x, per field MAE <= 0.1 and at most 0.1 % of pixels off by more than
8; PCSX2 where in doubt), the boot state trace with it on, one clear commit per program. A program
that cannot be matched within tolerance stays on the VU, with the reason recorded. Timing at
milestones only (after 1, after 2, at the end): Windows RTX at 8x, and the Mi 11i on the t612
profile at 2x only (the owner, 2026-11-06), on an idle PC and phone. The race state trace once at
the end with every translated program on, and `check.py port` before handing back. The final
report: per program what was translated (share, % of its batches on the GPU), correctness, the
timing table (off against each milestone), commits, and what is left.

## Progress

- [x] 2026-11-06: finding reserved

## Decisions

## Surprises

## Result
