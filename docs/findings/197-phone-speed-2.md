# 197: the phone's vertex loops and palette path, Native VU off

Status: in progress

Branch `perf/phone-fable-2` from `main` 652a734b (release 0.7.15), started 2026-11-10. This file is
the lane's plan while it runs and its finding when it merges. Someone who has only this file and the
git log must be able to pick the lane up.

## Goal and acceptance

The owner's minimum target on Android is 60 fps at 2x on a Redmi 9 class phone (Helio G80,
Mali-G52) with the default settings, Native VU off. Finding 195 left the CPU-bound stand-in (the
Mi 11i throttled to the t612 profile at 2x) at 61.9 fields/s at the race start with Native VU off:
no margin. Its ranked cost table names what is left there: the VU and GS threads' loops over the
scene's vertices (`native_geometry_loop_impl`, `packed_strip`, `native_draw_strip` and
`native_set`: 18.5 of the 28 ms the two threads work at the race start) and the GS thread's
palette and CLUT path (1.6 to 2.1 ms).

The work: cut those, the vertex loops first, then the palette path; each gain proven by timed runs
on the phone (uncapped, the screen on, other apps closed, base / change / base interleaved in one
batch), on finding 195's stand-ins:

- "t612 2x": `port/tools/phone_throttle.py --apply t612 --big-max 1209600 --little-max 1497600`,
  `DHD_SCALE=2`: the CPU limits. The primary cell is Native VU off.
- "4x": `--apply t612` (the profile's own clocks), `DHD_SCALE=4`: the GPU limits.

Acceptance:

- Each change keeps the game's state: boot (600 fields) and race (5000) state traces equal to
  `main`'s in every row.
- The emulated VU stays exact, and the pictures identical to `main`'s (finding 195's picture route:
  `native_vu_pictures.py`, eleven fields, 2x and 4x, Native VU off and on).
- Each kept change has a measured gain on a named stand-in.
- `python tools/check.py port` passes; the release APK builds.

Out of scope: gameplay, Native VU as the default, the render thread and the GPU (finding 195's
"What is left" 2 to 5), anything findings 146-149, 172 and 195 tried and dropped, unless a new idea
differs from what they tried (said where it does).

## Progress

- [x] 2026-11-10: finding reserved (this commit)

## Decisions

## Surprises

## Result
