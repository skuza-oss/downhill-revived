# 164: Native VU1 rendering, phase 1: the world mesh loop on the GPU

Status: in progress

Branch `native/vu-gs` (local only: never pushed or merged until the owner decides), after 162's
`30bb9ee`, started 2026-11-06. This file is the lane's plan while it runs.

## Goal and acceptance

Finding 162's "Phase 1 proposal", approved by the owner (2026-11-06): translate the prelit
textured world mesh, VU1 loop 0170 (MSCAL 0078: fog, back-face culling, clipping; 37-49 % of all
VU1 time), into GPU work, in the native renderer, behind a switch (off by default). The game keeps
building its packets; the VU thread hands the native renderer a mesh draw (the GS register state
the program's A+D tag sets, the batch's vertex slice) instead of running the loop, and a vertex
shader does the loop's maths; GPU clipping and culling replace the VU clipper. Everything else
stays emulated (other VU1 programs, VU0, the GS front end for every other draw, render targets,
uploads, the exact renderer).

Acceptance (the decision gate after this phase):
- **Correct:** with the switch on, the pictures of finding 156's route (title, shop, race start,
  mid-race; native renderer, 2x and 4x) match the switch-off pictures within a stated tolerance,
  per scene, with the differences shown and explained (expected: clip-edge and rasterisation
  rounding, fog per pixel vs per vertex if chosen); the current renderer and PCSX2 are the
  referees. Game state unchanged (boot and race state traces equal with the switch on).
- **Measured:** the same timing method as 156/162 (timed runs, the PC to themselves), Windows RTX
  at 4x and the Mi 11i at 2x as players have it and on the t612 profile: VU thread, GS thread,
  render thread and GPU ms per field, fps per scene, switch off vs on. Compare with 162's estimate
  (race start about -1.5 ms of VU work, -0.5 to -0.7 ms GS).
- **The gate's answer** in this finding: does the measured gain justify phase 2 (the other
  programs), and what the translator itself costs.

## Progress

- [ ] 2026-11-06: finding reserved

## Decisions

## Surprises

## Result
