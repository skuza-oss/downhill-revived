# 174: 60 fps at 2x on a Mali-G52 phone (Redmi 9): the GPU side

Status: in progress

Branch `perf/mali-gpu` from `main` cfe842a, started 2026-11-06. This file is the lane's plan while
it runs and its finding when it merges.

## Goal and acceptance

The owner's goal (2026-11-06): 60 fps at 2x on the Redmi 9 (Helio G80, Mali-G52 MC2, Android 11),
which becomes the recommended minimum for Android. Measured on it (a local measurement branch's runs,
the native renderer, uncapped, finding 156's route): about 47-49 fields/s at 2x and 56-57 at 1x, with the
native VU1 path on or off making no difference: the CPU threads are all near 11-16 ms a field, and
the GPU is the likely limit at 2x. Not measured yet: the GPU's time per pass there.

1. **Measure first.** The GPU time per frame and per kind of pass on the Redmi 9 at 2x
   (`DHD_GPU_TIMES`, finding 156's method; Mali's timestamp support through wgpu/Vulkan; or
   Arm's tools if they run without root, e.g. Streamline/libGPUCounters through a debuggable
   APK), the render passes per picture and what each loads and stores, and confirm or refute that
   the GPU limits.
2. **Then the tile-GPU costs**, biggest first, one code path for every GPU: Mali (like every
   phone GPU) renders in tiles, and each render pass's load and store of a full-size attachment
   costs memory bandwidth. Candidates: fewer render passes per picture (finding 156 counted 12-18
   in races) by keeping draws in one pass where the GS stream allows; `StoreOp::Discard` for depth
   and transient attachments that are never read after the pass; `LoadOp::Clear`/`DontCare`
   instead of `Load` where the previous contents are not needed; fewer full-screen copies and
   conversions (scanout, the present pass); smaller formats where exact; memoryless/transient
   attachments where wgpu supports them. Keep every picture identical (or explain any difference
   and keep it within a stated tolerance) and game state unchanged.
3. **Keep the scale:** the owner wants 2x, not 1x (1x looks soft on a phone).

Acceptance: the Redmi 9 at 2x before/after per change (two interleaved runs each, cool-downs with
the screen on, no more runs than needed), the Mi 11i once at the end (no regression on Adreno),
pictures identical, boot trace while iterating and race trace once at the end, `check.py port`.

## Progress

- [ ] 2026-11-06: finding reserved

## Decisions

## Surprises

## Result
