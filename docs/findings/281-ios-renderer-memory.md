# 281: an iPhone with 3 GB closes "out of memory" from the native renderer (0.8.12)

Status: in progress

Branch `fix/ios-gpu-memory` from `main` 6937e805 (0.8.12), started 2026-11-24. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

A player's iPhone XR (iPhone11,8, A12, 2.8 GB, iOS 18.7.10), 0.8.12 sideloaded, Metal, native
renderer, Native VU1 on, resolution 2x (picture 1280x896, surface 1792x828). The guest memory
reserves in two parts (findings 258, 267). Two runs, both closed by iOS with no crash report:

- the first after the disc import: menus, an online room and its rider/bike shop, then sixteen
  `dhdr_texture_create: native texture: Out of Memory`, `gs: the GPU device was lost (Unknown):
  Out of memory` and `wgpu error in dhdr_native_picture_indexed: Out of Memory` (about 30 s);
- the next one: 71 pipelines of the first run's list built ahead in 0.6 s, then at field 148 (the
  title's first 3D picture), 2 s after the start, `gs native: out of memory (vertices)`: a CPU
  `realloc` of the native renderer's vertex array (`port/gs/native_core.inc` native_reserve)
  returned NULL.

Done means: the cause found with evidence; the fix in the one path every platform runs; at an
out-of-memory in the renderer one log line says what was asked, the renderer's own totals and
on Apple the process's footprint and free address space; proof by a test that fails before and
passes after (or the nearest honest thing), the iOS Simulator to a race, `check.py port`,
`check.py docs`, boot and race state traces on the PC equal to the base. Out of scope: the
extended-virtual-addressing and increased-memory-limit entitlements (a paid account's).

## Progress

- [ ] 2026-11-24: the renderer's totals (textures, surfaces, arenas) and the out-of-memory line
- [ ] the cause, measured
- [ ] the fix
- [ ] checks

## Decisions

## Surprises

## Result
