# 168: Native mesh draws batched, for the render thread

Status: in progress

Branch `native/vu-gs` (local only: never pushed or merged until the owner decides), after 167's
`eb14849`, started 2026-11-06. This file is the lane's plan while it runs.

## Goal and acceptance

Finding 167: with every translatable VU1 loop on the GPU, the Mi 11i on the t612 profile at 2x
went 63.9 -> 74.7 fields/s (+17 %), while Windows RTX 8x gained +38 %. On the phone the limit is
now the render thread (the Vulkan driver, dhd_render and wgpu: finding 156's breakdown), and the
mesh draws added 0.5-1.1 ms a field to it: each translated batch (at most 44 vertices) became its
own draw with its own constants, where the emulated path's draws were merged.

Goal: cut the render thread's cost of the mesh draws, on every platform (one code path): merge
consecutive mesh batches that share pipeline, texture and render state into one draw; per-object
constants (matrix, fog, colour scale, guard band) in one buffer indexed per batch or vertex, not a
constants write per draw; one vertex/index upload per frame. Whatever the current draw-merging of
the native front end does for ordinary draws, the mesh path joins it where it can.

Acceptance:
- Pictures with the switch on (all programs, as 167 left them) unchanged against before this
  change (identical or within 167's tolerance; say which), at 2x and 4x over finding 156's route.
- Boot state trace (switch on) unchanged; race trace once at the end.
- Measured: draw calls per frame and render-thread ms before/after; the Mi 11i on t612 at 2x only
  (the owner's rule), switch off / all-on-before / all-on-after; Windows RTX 8x for the same three
  (idle-PC gate as in 164/167).
- `check.py port` passes.

## Progress

- [ ] 2026-11-06: finding reserved

## Decisions

## Surprises

## Result
