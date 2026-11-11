# 199: Native VU in the browser

Status: in progress

Branch `feat/native-vu-browser` from `main` 6e2f7d23 (0.7.16 plus findings 200 and 201), started
2026-11-11. This file is the lane's plan while it runs and its finding when it merges. Someone who
has only this file and the git log must be able to pick the lane up.

## Goal and acceptance

Native VU (findings 164, 167, 168, 194, 198: the VU1 programs' batch loops drawn by the native
renderer's shaders, `port/hle/vu1_mesh.inc`) works in the browser build (WebGPU) through the same
code as every other platform: the browser's VU worker runs `vu1_mesh.inc`'s translation, the
browser's renderer makes the mesh layouts and pipelines, and the same setting (`native_vu`, on by
default, live, with the automatic fallback to the emulated VU when the GPU refuses the mesh
pipelines) is offered in the browser's settings. This is the step before the emulated VU draw path
can be deleted everywhere: the browser must no longer need a path of its own.

Acceptance:

- The browser build compiles and runs with Native VU on: boot, menus, the Bike Shop, a race start
  and mid-race, pictures within finding 164's tolerance (per field MAE <= 0.1, at most 0.1 % of
  pixels off by more than 8) against the browser's own pictures with it off, through the repo's
  headless browser tools (`port/web/check_browser.mjs`, `run_headless.mjs`).
- Off still works; a refused mesh pipeline falls back as on the native platforms
  (`DHD_TEST_NO_MESHES` or the browser's equivalent).
- Native platforms unchanged in behaviour (the shared code changes are behaviour-neutral there; the
  lead runs the Windows traces and pictures).
- WebGPU limits checked against what the mesh path needs; mismatches solved in the shared path.
- `python tools/check.py port` and `check.py docs`, `rustgs.py test`, `rustgs.py check --target
  wasm32-unknown-unknown`, the browser build and its headless checks with Native VU on and off.

Out of scope: deleting the emulated path; timing (the owner is using the PC: no benchmarks).

## Progress

- [x] 2026-11-11: finding reserved

## Decisions

## Surprises

## Result
