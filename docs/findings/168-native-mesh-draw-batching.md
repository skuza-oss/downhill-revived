# 168: Native mesh draws batched, for the render thread

Status: current
Checked: 2026-11-10

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

- [x] 2026-11-06: finding reserved
- [x] counters: `DHD_RSTAT` lines count draw calls, constants copies, group 0 and 1 binds, pipeline
  binds, commands, vertices and indices a picture
- [x] batching: each object's values in a record in the frame's vertices, read by the mesh vertex
  shader through the vertex buffer bound again as storage (group 2); the mesh draws' constants
  without them (ABI 3); consecutive batches join one command whatever their object
- [x] pictures: identical to before, 2x and 4x, every field; boot state traces equal
- [x] timing: Mi 11i t612 2x (off, all-on before, all-on after, two rounds; and a round with
  `DHD_RSTAT`); desktop timing not taken (Result)
- [x] race state traces; `check.py port` (`2afb7f4`: every step OK, at two build slots)

## Where the mesh draws' render-thread cost came from

`DHD_RSTAT` (render.rs) gained counters: per picture the draw calls (`draw` / `draw_indexed`), the
constants copies into the uniform arena, the binds of group 0 (a new dynamic offset) and group 1
(a texture), the pipeline binds, the commands, vertices and indices. Finding 156's route at 2x
(Windows, the native renderer), mean a picture per scene, switch off and `all` before this change:

| scene | draws | constants copies | group 0 | group 1 | pipelines | commands |
|---|---|---|---|---|---|---|
| title, off / all | 65 / 136 | 37 / 131 | 37 / 131 | 60 / 60 | 22 / 82 | 66 / 136 |
| shop | 86 / 132 | 63 / 118 | 63 / 118 | 71 / 71 | 61 / 96 | 94 / 140 |
| race start | 425 / 505 | 254 / 393 | 255 / 394 | 383 / 383 | 180 / 225 | 466 / 538 |
| mid-race | 383 / 468 | 199 / 337 | 200 / 338 | 360 / 360 | 133 / 186 | 428 / 506 |

The textures bound are the same either way: the extra 70-85 draws a picture come from the mesh
draws' constants (each object's matrix, guard band, colour scale and fog made its own constants
block, so each object was its own command, copy, group 0 bind and draw call) and from the pipeline
changes between mesh draws and the GS path's.

## The change

- **Object records in the vertex array.** `native_mesh_record` (native_core.inc) writes a batch's
  object values (the matrix's columns, the guard band, the colour scale, fog A, B and max: 128
  bytes, four vertices' room) into the frame's vertices, once per object (the next batch with the
  same values reuses it), and each of the batch's vertices carries the record's vertex index in its
  spare word (`GsMeshVertex.unused`, the GS path's fog). The vertex buffer is uploaded once a
  picture as before; the renderer binds it a second time, as read-only storage for the vertex
  stage (group 2, `Arenas::mesh_group`, once a pass), and `native_mesh_vertex` reads the record
  (`mesh_records`, eight vec4s from twice the vertex index). The mesh pipelines' layouts have that
  group (init.rs; not in the browser, which draws no meshes: there the vertex buffer has no
  storage use).
- **Constants without the object.** `NativeConstants` keeps only the draw offset, the largest Z
  and the Z scale for a mesh draw (`mesh_offset`): 224 -> 112 bytes, `DHDR_ABI_VERSION` 3. A mesh
  draw's constants are then the same for every object under one draw state.
- **One command for consecutive batches.** `native_mesh` joins a batch to the open mesh command
  when the GS state, context, PRIM, draw offset and culling agree, whatever the object; the render
  thread's constants dedup and draw extension (render.rs, unchanged) then find nothing between
  them. The vertex and index uploads stay one each a picture (they were already).

## Draws after the change

The same counters and route (2x, a picture per scene's mean), `all` after the change, and with
`:gpuclip` (every batch translated, the near-bound ones too: a measurement, its pictures are not
the game's, finding 164):

| scene | draws: off / before / after / gpuclip | constants copies: off / before / after | pipelines: off / after |
|---|---|---|---|
| title | 65 / 136 / 121 / 96 | 37 / 131 / 95 | 22 / 82 |
| shop | 86 / 132 / 119 / 94 | 63 / 118 / 99 | 61 / 96 |
| race start | 425 / 505 / 457 / 431 | 254 / 393 / 294 | 180 / 225 |
| mid-race | 383 / 468 / 422 / 393 | 199 / 337 / 252 | 133 / 186 |

In races the change takes back a little over half of what the mesh draws had added (race start
505 -> 457 against 425 off), and nearly all of the rest is the batches that still run on the VU
(a vertex below the near bound, 15-42 % of a world loop's batches, finding 167): their GS-path
draws sit between an object's mesh batches and split its run, which `:gpuclip` shows (431, 6 more
than off). In the title and shop the culled and two-sided mesh loops (different pipelines) also
alternate (pipelines 82 against 22 off).

## Timing: the Mi 11i on the t612 profile, 2x

Finding 167's method (release-signed APKs with the shipped builds' PGO profile, finding 156's run
script, the frozen t612 profile, the clocks at its caps in every run, 24-30 C): switch off and
`all` with this change (`2afb7f4`), `all` before it (finding 167's APK, `39533bb`), two rounds.
Fields a second over the route: off 64.2 / 63.9, before 74.9 / 74.9, after 74.0 / 74.3; a third
round with `DHD_RSTAT` (its clock reads cost the same in each): off 64.0, before 74.7, after 74.9.
**No change**: the after runs sit within a frame of the before runs, either side. Per scene (two
rounds; ms a field from the timeline, work = CPU minus spin, busy = the field minus the thread's
waits, as route_profile.py's columns):

| scene | switch | fps | field | game work / busy | VU work / busy | GS work / busy | render CPU / busy |
|---|---|---:|---:|---|---|---|---|
| title | off | 50.2 | 19.92 | 5.62 / 3.24 | 18.56 / 19.92 | 17.48 / 17.54 | 4.60 / 3.39 |
| title | before | 70.5 | 14.19 | 4.32 / 3.95 | 12.51 / 14.19 | 8.23 / 6.77 | 5.42 / 4.19 |
| title | after | 70.6 | 14.17 | 4.41 / 4.13 | 12.52 / 14.17 | 8.40 / 6.98 | 5.42 / 4.16 |
| shop | off | 63.2 | 15.82 | 3.94 / 2.87 | 14.31 / 15.82 | 9.68 / 9.45 | 6.38 / 5.28 |
| shop | before | 75.8 | 13.20 | 3.30 / 3.21 | 11.29 / 12.96 | 5.80 / 5.59 | 7.27 / 6.18 |
| shop | after | 76.8 | 13.01 | 3.29 / 3.18 | 11.10 / 12.77 | 5.88 / 5.71 | 7.34 / 6.21 |
| start | off | 57.9 | 17.27 | 11.28 / 12.45 | 14.76 / 16.58 | 15.05 / 15.86 | 13.46 / 12.56 |
| start | before | 62.4 | 16.03 | 10.38 / 11.69 | 11.64 / 12.84 | 9.13 / 15.53 | 14.22 / 13.34 |
| start | after | 60.7 | 16.49 | 10.33 / 11.58 | 11.79 / 12.93 | 9.39 / 16.01 | 14.57 / 13.59 |
| mid | off | 73.1 | 13.68 | 9.75 / 11.12 | 11.82 / 13.57 | 11.74 / 12.05 | 10.67 / 9.66 |
| mid | before | 82.5 | 12.12 | 9.22 / 10.95 | 9.77 / 11.22 | 7.33 / 11.66 | 11.33 / 10.37 |
| mid | after | 80.5 | 12.42 | 9.14 / 10.82 | 9.87 / 11.23 | 7.41 / 12.03 | 11.60 / 10.64 |
| route | off | 64.0 | 15.61 | 7.75 / 8.01 | 12.96 / 14.49 | 12.66 / 13.51 | 9.48 / 8.47 |
| route | before | 74.9 | 13.35 | 7.05 / 8.08 | 9.88 / 11.29 | 7.53 / 10.94 | 10.43 / 9.48 |
| route | after | 74.1 | 13.49 | 7.01 / 8.02 | 9.90 / 11.26 | 7.64 / 11.22 | 10.62 / 9.63 |

What limits the phone with the switch on is the render thread, and it is not the draws' count:

- **The render thread never waits for work** with the switch on: its wait for the next field is
  0.00 ms at the race start and 0.03 mid-race (2.16 and 2.11 with the switch off), and it is busy
  13.3-13.6 ms of a 16.0-16.5 ms field at the race start plus 2.7 ms in the present.
- **The GS thread waits behind it**: never idle (0.4 ms a field), but working only 9.1-9.4 ms of
  the field; the rest it is off the CPU without waiting for its input, which with the render
  thread never waiting reads as the hand-over of finished fields to it (not traced further; with
  the switch off the GS thread worked 15.0 ms and limited, the render thread waiting 2.2 ms a field
  for it).
- **The render thread's time is wgpu's, per state change** (`DHD_RSTAT`, race start, ms a
  picture): recording the draws 2.0-2.1, the uploads 0.9-1.0, `encoder.finish` (wgpu replays the
  recorded pass into Vulkan) 5.5 off / 6.1 before / 6.3 after, `queue.submit` 2.8-2.9, the
  present 1.2-1.7. About 13 us a draw in `finish` on the throttled cores, and the states it
  replays are mostly the GS path's own: 383 texture binds, 255-295 constants binds and 180-225
  pipeline binds a picture at the race start, against 425-505 draws. Taking out a tenth of the
  draws, the mesh draws' constants binds among them, left `finish` where it was (6.1 -> 6.3 ms).
  The likely reason, not measured apart: the mesh pipelines' layout (three groups) differs from the
  GS path's (two), so each pipeline change between the two (about 110 a picture) is a layout
  change for wgpu's binder; the untimed follow-up below removes it.

## Correctness

- **Pictures:** finding 156's route, `DHD_NATIVE_VU1=all`, the eleven fields at 2x and 4x against
  finding 167's captures of the same build before this change: identical, every pixel of every
  field (so within 167's tolerance of the switch-off pictures as before).
- **Boot state traces** (600 fields, headless): shadow mode equal to the base in every row; skip
  mode equal to finding 167's skip trace in every row.
- **Race state traces** (5000 fields, the first track, headless, `DHD_NATIVE_VU1=all`): shadow mode
  equal to finding 162's base in every row; skip mode equal to finding 167's skip trace in every row.

## Decisions

- **No desktop timing** (the owner and the lead, 2026-11-06): the Windows RTX runs were stopped
  when the owner took the PC (the one all-on-before run taken, 185.9 fields/s, ran beside another
  session's online test and is dropped), and the Mac was not used either (another session's
  PC<->Mac races were running on it). The desktop numbers here are `DHD_RSTAT`'s counts and
  per-picture times from the picture runs, not timed runs.
- **The branch stays at the measured change.** A follow-up was built and checked for pictures
  (identical at 2x) but not timed: every draw pipeline with the three-group layout, group 2 bound
  once a pass for all draws and its minimum size given (no layout change between mesh and GS
  draws, no late size check); kept out of the branch as untested.

## Surprises

- **Fewer draws, no faster phone.** The draw count was the plan's measure of the render thread's
  cost; on the phone wgpu's `finish` and `submit` cost per replayed state change (texture,
  constants, pipeline and layout), most of them the GS path's, so a tenth fewer draws with one
  layout change more each time a mesh draw meets a GS draw did not show.
- **The limit moved with finding 167.** With the switch off the GS thread limits the phone (the
  render thread waits for it); with it on, the render thread (the GS thread waits for it): the
  next gain on the phone is in the render thread's state changes for every draw, not in the mesh
  draws alone.
- **An orphaned timing script spun a core all day.** Phase 1's `win.sh`, left from a session
  restart at 06:59, busy-looped on one core until 18:45: every Windows idle gate today ran with
  it (the floors of 11-24 % include it). Stopped; the timed runs' comparisons are within one
  session each, so they hold.

## Result

- **What changed:** each object's mesh values (matrix, guard band, colour scale, fog) are a 128-byte
  record in the frame's vertex array, read by the mesh vertex shader through the vertex buffer
  bound again as storage; the draw constants lost them (224 -> 112 bytes, ABI 3), so consecutive
  batches under one draw state join one command whatever their object. `DHD_RSTAT` counts draw
  calls, constants copies, binds, commands, vertices and indices.
- **Draws:** race start 505 -> 457 a picture (425 with the switch off), mid-race 468 -> 422 (383),
  title 136 -> 121 (65); constants copies 393 -> 294 (254) at the race start. What is left over the
  switch-off count is the batches still on the VU (near bound), whose GS draws split an object's
  mesh run (`:gpuclip` leaves 6 draws over), and, in the title, the culled and two-sided mesh
  pipelines alternating.
- **Pictures:** identical to before at 2x and 4x, every field; boot and race state traces as
  before.
- **Phone (Mi 11i, t612, 2x):** no change, 74.9 -> 74.1 / 74.9 fields/s over the route (off 64.0).
  The render thread limits there with the switch on, and its cost is wgpu's replay of every
  draw's state changes, most of them the GS path's (above).
- **Desktop:** not timed (Decisions).
- **What is left for the phone's render thread:** fewer state changes for every draw, not only the
  mesh draws: the texture binds (383 a picture at the race start; a texture array or atlas), the
  constants binds (255 with the switch off; constants indexed per draw instead of a dynamic
  offset each), the layouts kept the same across pipelines (the follow-up above); and the near-bound
  batches on the GPU (finding 164) for the mesh runs they split.
