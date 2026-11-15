# 249: Native VU must not cost speed where the GPU limits

Status: current

Branch `perf/native-vu-speed-249` from `main` cae6f05c (release 0.8.5 plus lane 247), started
2026-11-15. This file is the lane's plan while it runs and its finding when it merges. Someone
who has only this file and the git log must be able to pick the lane up.

## Goal and acceptance

Finding 247 measured, on the throttled Mi 11i (`phone_throttle.py --apply t612`, `DHD_SCALE=4`), a
whole race at 57.0 fields a second with Native VU on (the players' default) against 60.4 with it
off: the default path is about 6 % slower than the emulated path it is meant to replace, where the
GPU limits. Finding 198 saw the same sign on the Redmi 9 at 2x (-5 %) and on an integrated Radeon
at 4x (-3 %).

Target: on that stand-in, Native VU on is at least as fast as off over the whole race and at the
race start, without changing what the game does. Deleting the emulated VU1 path is not this lane;
this lane removes the speed reason to keep it.

Method: the base first (both settings, interleaved), then a profile of where the difference is,
then one change at a time. A change is kept only when the whole race gains about 3 % or more,
clearly outside the runs' spread, and the boot and race state traces on the PC equal the base's.

Acceptance: `python tools/check.py quick` and `port`; boot (600 fields) and race (5000) state
traces equal to `main` cae6f05c's; the Android unit tests.

Out of scope: gameplay, the picture's content, players' settings, the deletion of the emulated
path, what findings 146-149, 174, 183, 195, 197, 198 and 247 tried and dropped.

## Progress

- [x] 2026-11-15: finding reserved (6735c5de)
- [x] 2026-11-15: the base measured, both settings in turn, and profiled
- [x] 2026-11-15: change 1, a mesh's pixels run the GS draws' fragment stages (48962741)
- [x] 2026-11-15: change 2, a mesh's vertices arrive in clip space with their colour (6d9c4423)
- [x] 2026-11-15: the near batches on the GPU's clipper timed (a measurement build, not merged)
- [x] 2026-11-15: `check.py port`, the state traces, the Android unit tests, the browser build
- [ ] the lead: the merge, the Redmi 9's confirmation (below), Status

## How it was measured

The Mi 11i with `phone_throttle.py --apply t612`, `DHD_SCALE=4`, release-signed test builds with
the profile of finding 197, finding 156's route (`bench_race.py`'s keys) with `DHD_TIMELINE`.
Scenes by field: the race 6321-14000, the race start 6400-7300, mid-race 9000-14000, the route
300-14000. Each build ran on, off, on, off with a 60 s pause between runs; a fifth run with
`DHD_RSTAT=1 DHD_GPU_TIMES=1` gave the pass times. Two runs of one build and setting differ by 0.1 %
or less (race), so a difference of 0.5 % is real here. Battery 25 to 32 degrees C in every run.

## Result in one table

Fields a second on the throttled Mi 11i at 4x, the mean of two runs each:

| Build | Race, on | Race, off | Start, on | Start, off | Mid-race, on | Mid-race, off |
|---|---|---|---|---|---|---|
| base (cae6f05c) | 61.25 | 65.30 | 49.74 | 51.95 | 64.31 | 68.74 |
| change 1 | 63.14 | 65.41 | 50.35 | 52.08 | 66.57 | |
| change 2 (the lane's head) | 64.31 | 65.36 | 51.14 | 51.89 | 67.74 | 68.83 |

Native VU on went from 6.2 % behind off to 1.6 % behind over the race (61.25 -> 64.31, +5.0 %), and
from 4.3 % to 1.4 % behind at the race start (49.74 -> 51.14, +2.8 %). Over the whole route
(menus and loads included) on is now level with off: 65.97 against 65.89 (base 63.19 against
65.79). The target, on at least as fast as off in the race, is not reached: 1.05 fields a second
are left.

## The base's profile

- The GPU limits in both settings (the GPU's frame 14.67 ms on, 13.67 ms off, of a 16.3 and 15.3
  ms field). The CPU does not: with Native VU on the VU thread works 9.3 ms a field and the GS
  thread 6.2 ms, with it off 11.1 and 8.1 ms. Native VU saves CPU and costs GPU.
- The cost is in the draw passes (`DHD_GPU_TIMES`): 12.74 ms on against 11.79 ms off over the
  race, 15.90 against 15.16 at the start, 12.18 against 11.23 mid-race. Passes are the same (12.7 a
  picture), so the pass order of lane 247 (`lane247-pass-order`) does not matter here.
- What differs per picture mid-race (`DHD_RSTAT`): draws 358 on against 318 off, pipeline changes
  163 against 111, vertices 47.6 k against 43.2 k. A translated batch is a draw of its own kind,
  so a draw state whose batches are partly meshes and partly VU-kicked (see the work list below)
  is two or more draws.
- `DHD_GPU_STATS`' per-draw times do not show a fragment cost on a tile-based GPU, only each
  draw's overhead; they did not locate the cost.
- The mesh's fragment stages were the first suspect: they were entry points of their own with
  perspective-correct varyings (STQ, colour and fog divided again per pixel), while the GS draws'
  stages take screen-linear varyings.

## Change 1: a mesh's pixels run the GS draws' fragment stages (kept, 48962741)

The GS interpolates STQ, colour and fog linearly in screen space. The mesh vertex stage now puts
out the same no-perspective varyings as `native_vertex` (STQ times q, the fog floored), so a mesh
runs the very fragment stages of a VU-kicked draw: the four `native_pixel*_mesh` entry points and
the two dual ones are deleted, and a mesh no longer needs fragment pipelines of its own.

- Phone: race 61.25 -> 63.14 (+3.1 %), start 49.74 -> 50.35 (+1.2 %), mid-race 64.31 -> 66.57.
  Draw passes over the race 12.74 -> 12.29 ms. Off unchanged (65.41).
- Pictures (`native_vu_pictures.py`, 11 fields at 4x) against the base with Native VU on: mean
  difference 0.000, largest 2 of 255. Against off: as the base.
- `native_spirv` passes (the Mali-G52 and G57 rules of finding 190).

## What was left after change 1

- At 1x the draw passes cost 2.45 ms on against 1.90 ms off: +0.55 ms, the same as the 4x
  difference left (12.29 against 11.79). So what was left did not grow with the pixels: it was the
  mesh vertex stage and the extra draws. (At 1x, where the CPU limits, Native VU on is the faster
  one: 83.2 against 75.4 fields a second over the race.)
- malioc (Mali-G52): `native_vertex` 1.08 arithmetic cycles and 11 load/store, `native_mesh_vertex`
  3.75 and 18. The mesh vertex read eight vec4s of its object's record from a storage buffer and
  multiplied a 4x4 matrix.
- Counted on the PC over the route: the loops' guard band changed once in 12 million batches; the
  fog changed inside a joined draw 21 thousand times (about twice a field). Both can be draw
  constants without splitting draws.

## Change 2: a mesh's vertices arrive in clip space with their colour (kept, 6d9c4423)

The VU thread, which already walked every vertex, now multiplies it by the loop's matrix (in the
loop's own order: the fourth column, then x, y, z; in the VU's rounding, toward zero) and writes
the final colour bytes. `GsMeshVertex` is the clip-space position, u v and the Q factor, and the
colour. The vertex stage only divides, fogs, tests the guard band and places the vertex. Gone: the
per-object record (four vertices a batch), its storage buffer binding (group 2), the mesh pipeline
layouts and the "no storage buffer in the vertex stage" refusal. The guard band and fog travel as
draw constants (`DhdrNativeConstants` 112 -> 144 bytes, ABI version 4).

- Phone: race 63.14 -> 64.31 (+1.9 %), start 50.35 -> 51.14 (+1.6 %), mid-race 66.57 -> 67.74.
  Draw passes over the race 12.29 -> 12.02 ms (off 11.79). VU thread work 9.3 -> 9.6 ms a field
  (off 11.1): the matrix product costs the CPU 0.3 ms a field, where the GPU limits.
- 1.9 % is under the 3 % bar of the method. It is kept for three reasons together: it is 20
  times the runs' spread, it deletes more than it adds (555 lines in, 667 out, one fallback
  less), and the pictures moved toward the emulated path, below.
- Pictures against Native VU off, worst field per scene (mean difference / share of pixels off by
  more than 8): the base 0.067 / 0.087 % (mid-race, field 9300); change 2 0.006 / 0.007 %. The
  positions are now the VU's own bits, so edges land on the same pixels.
- A first version multiplied in the PC's default rounding (to nearest): 1.5 % of the vertices
  then differed from the VU's by 1/16 pixel and the pictures against off went to 0.078 / 0.100 %,
  the tolerance's edge. The VU thread already runs in the VU's rounding on arm64
  (`graphics_queue.c`); on x86-64 `mesh_before` now sets it around `mesh_build`.
- `DHD_NATIVE_VU1_CHECK=1` over the picture run's route: of 408 million vertices inside the guard
  band none is off by more than 1/16 pixel in XY and none differs in colour; Z differs by more
  than 1 in 9,418 (18B0 7,971, 1110 1,387), the fog in 13,245 and STQ in 19 (all 2DE8), ADC in
  7,455 triangles (0170).

## Hypothesis 3: the near batches on the GPU's clipper (measured, not kept)

Change 2 left 0.23 ms of draw passes between on and off, with 42 more draws and 51 more pipeline
changes a picture: the batches that still run on the VU (the list below) split the draws. To see
what joining them is worth, a build that is not merged (one line: `mesh.gpu_clip = 1`, the
`:gpuclip` measurement option of finding 164 always on, since Android's `dhd.ini` cannot carry it)
drew every batch as a mesh and left the near ones to the GPU's clipping.

- Phone, two runs: race 64.90 and 64.32 (the second with `DHD_RSTAT` and `DHD_GPU_TIMES`, which
  cost about 0.6 %; change 2's own such run gave 63.88), start 51.41, mid-race 68.46; off in the
  same session 65.25, 51.74, 68.71. So +0.9 % over change 2, and still 0.5 % behind off.
- Per picture mid-race: draws 327 (change 2: 360, off: 318), pipeline changes 116 (163, 111), draw
  passes over the race 11.89 ms (12.02, off 11.79). VU thread work 7.6 ms a field (9.6, off 11.1).
- Not kept and not a candidate as it is: the GPU's near clipping does not draw what the VU's
  clipper draws over the near ground (finding 164), so the picture's content changes. It is the
  upper bound of what translating the near batches can give: under 1 %.
- With every batch a mesh the draws are level with off and 0.10 ms of draw passes remain. Meshes
  send 9 % more indices than the VU kicks (99.3 k against 90.6 k a picture mid-race: the back
  faces and the off-screen triangles the VU drops by ADC reach the GPU and are culled there) and
  their vertex stage still does more than `native_vertex` (a divide, the fog, the guard band test).

## Batches that still run on the VU (the next lane's work list)

Whole route, Native VU on, per loop: batches translated / batches run on the VU, and why.

| Loop | Translated | On the VU | Reason |
|---|---|---|---|
| 0170 | 5,396,825 | 966,318 | a vertex below the near bound |
| 1708 | 628,336 | 484,923 | a vertex below the near bound (2: join value) |
| 18B0 | 1,369,108 | 187,929 | a vertex below the near bound |
| 2DE8 | 233,421 | 88,246 | a vertex below the near bound |
| 0C30 | 139,456 | 40,874 | a vertex below the near bound |
| 1510 | 223,689 | 3,591 | a vertex below the near bound |
| 1110 | 1,413,037 | 3,321 | 1,667 a vertex below the near bound, 1,654 colour |
| 13A0 | 1,120,711 | 2,062 | colour |
| 08B0 | 1,505,218 | 0 | |
| 0798 | 1,944,023 | 0 | |
| 3140 | 603,437 | 0 | |
| 0AA0 | 756 | 0 | |

1.78 million of 16.4 million batches (11 %) run on the VU, nearly all for the near bound: the
ground and the track close to the camera. Such a batch splits a draw that would otherwise be
one (the extra 40 draws and 50 pipeline changes a picture of the profile).

## Decisions

- Change 2 kept under the 3 % bar, for the reasons given there. The lead may drop it alone: it is
  one commit (plus a one-line comment commit after it).
- The clip-space product is made on the VU thread, not in the shader: the VU thread has time to
  spare where the GPU limits, and where the CPU limits (1x) Native VU on stays far ahead of off.
- The fog and the guard band are draw constants, not per vertex: they almost never change inside
  a draw, and a vertex stays 32 bytes.

## Surprises

- The per-draw GPU timestamps of `DHD_GPU_STATS` are of no use for a fragment cost on a tiler.
- The emulated VU rounds toward zero on every platform but the browser; code that wants the VU's
  bits must run in that mode (`host_fp_csr`), plain SSE in the worker's mode is not enough on
  x86-64.
- On Android `dhd.ini` reduces `DHD_NATIVE_VU1` to on or off: test values such as
  `all:gpuclip` need a build of their own there.

Tried and dropped, or rejected before building:

- Moving the whole vertex maths (divide, fog, placing) to the CPU: not built. It would undo the
  design of findings 164 and 194 and cost the VU thread where the CPU limits.
- Building on `lane247-pass-order`: the passes are the same on and off (12.7 a picture), so the
  pass order is not where the difference is.
- The near batches on the GPU's clipper (hypothesis 3): +0.9 %, changes the picture near the
  camera, not kept.

## Result

Native VU on, throttled Mi 11i at 4x: the race 61.25 -> 64.31 fields a second (off: 65.36), the
race start 49.74 -> 51.14 (off: 51.89), the route 63.19 -> 65.97 (off: 65.89). The gap to off went
from 6.2 % to 1.6 % (race) and from 4.3 % to 1.4 % (start); it is not closed.

Checks: `check.py port` OK; boot (600 rows) and race (5000 rows) state traces equal the base's
(`state_diff.py`: PASS); the Android unit tests pass; the browser build compiles.

What the Redmi 9 (Mali-G52) still has to confirm: that change 1's shared fragment stages and
change 2's vertex layout (a `Uint8x4` colour attribute read as `vec4<u32>`) draw the same picture
there, and the gain at 2x, its recommended scale (finding 198 had -5 % there).

What is left, in order of expected gain:

1. The near batches (the work list above) as meshes with the VU clipper's own result: the VU thread
   clips those triangles as 1948 does and sends the fans as mesh vertices. Worth at most 0.9 % of
   the race where the GPU limits (hypothesis 3), and 2 ms of VU thread work a field where the CPU
   limits. It is also what the deletion of the emulated path needs.
2. Instead of 1, one pipeline for meshes and VU-kicked draws (a kicked vertex as a mesh vertex with
   w = 1), so that a draw state's batches join whichever way they came: the same 40 draws and 50
   pipeline changes a picture, the same bound.
3. The last 0.5 %: fewer triangles sent (the back faces of 0170, which the VU thread could drop
   from the index list once it has the clip-space positions, at the price of a divide a vertex on
   the CPU) and a lighter vertex stage. Parity where the GPU limits needs these too; none of them
   is a 3 % step.
