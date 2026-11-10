# 164: Native VU1 rendering, phase 1: the world mesh loop on the GPU

Status: current
Checked: 2026-11-10

Branch `native/vu-gs`, after 162's `30bb9ee`, started 2026-11-06. Merged into `main` with the rest of `native/vu-gs` at 32df819b on 2026-11-10, behind finding
194's Native VU setting (off by default); ships in 0.7.15.

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
  at 8x only, and the Mi 11i on the frozen t612 profile only, at 2x only (the owner,
  2026-11-06: no Windows 4x, no full-speed phone runs, no phone 4x): VU thread, GS thread, render
  thread and GPU ms per field, fps per scene, switch off vs on. Compare with 162's estimate (race start about
  -1.5 ms of VU work, -0.5 to -0.7 ms GS). The Windows runs must not be held by a frame-rate cap
  or vsync (156's frame-clock route ran 254-275 fields/s uncapped at 8x): any cap the faster runs
  meet is raised or removed for the measurement only, and the finding says how.
- **The gate's answer** in this finding: does the measured gain justify phase 2 (the other
  programs), and what the translator itself costs.

## Progress

- [x] 2026-11-06: finding reserved
- [x] the loop's semantics from the microcode (`vudis.py` on overlay B: setup 1AE0, batch
  prologue 0088-0168, loop 0170-0268, texture A+D 0FA8, end 1000-1030; below)
- [x] the translation behind `DHD_NATIVE_VU1=0170`: VU thread (`port/hle/vu1_mesh.inc`), GS queue
  item and register writes (`port/gs/gs_mesh.h`, `gs_mesh`), native draw kind (`native_mesh`),
  vertex shader (`native_mesh_vertex`) and pipeline bit (`MESH`); draw constants 96 -> 224 bytes
  (ABI 2)
- [x] builds (core id unchanged, `1fe325c006976190`), first pictures; the parity and the clipped
  triangles fixed (Surprises), the shadow check (`DHD_NATIVE_VU1_CHECK`) clean
- [x] pictures switch off vs on (2x, 4x, four scenes, eleven fields): within MAE 0.07
- [x] PCSX2 referee (2.8.2, with every phase 2 loop on: finding 167's run, below)
- [x] boot state traces (switch off, shadow, skip); race state traces with every phase 2 loop on
  as well (finding 167: shadow equal to the base in every row; skip: game state equal)
- [x] timing: Windows RTX 8x; Mi 11i t612 2x (with phase 2's APK)
- [x] the gate's answer (Result)

## How loop 0170 works (the translation's specification)

From the microcode (overlay B, `vudis.py 269C50`). MSCAL 0078 branches to the shared setup 1AE0
with VI13 = 0x11: vf1..vf4 = the view-projection (data 9-12) times the object matrix's rows (data
0-3), their z scaled by data 4's z; vf13 = data 996 (guard band), vf18 = data 21 (fog), vf19 =
data 4 (x, y: the U and V offsets, 0 in every object seen; w: the fog base), vf31 = data 5 (colour
scale); VI2 = VI14 = 0x238; it ends with TPC 1CB0. In a race the guard band is the GS's whole
coordinate range (data 996 = -2048, -2048, -2048, 2047: |X - 2048| <= 2047, w >= 1) and Z = 2^24 / w
- 1024 or so (Z GEQUAL; the near bound w = 1 is where Z reaches 2^24). Every batch is an MSCNT at 1CB0
(the first) or 1040 (after a batch), both `JR VI13` to 0x88:

- **Header** at VI2: the strip's GIF tag (NLOOP n with EOP, PRE, PRIM 03C/07C, PACKED ST RGBAQ
  XYZF2); its unused top word holds the texture triplet offset t (bits 0-14) and in bit 15 the
  strip's parity (the prologue sets vf6.x to 1 at 0098 and negates it at 0158 when the bit is
  clear: nothing carries over from the batch before). With t, 0FA8 writes an A+D tag (data 26) and
  TEX0, TEX1, MIPTBP1 (data 149-151 + t) and TEXFLUSH (164) in the five qwords before the header,
  and the kick starts there.
- **Per vertex k** (three qwords): pool index, colour, texture coordinates. clip = vf1 x + vf2 y +
  vf3 z + vf4 with (x, y, z) = data 181 + index; q = 1 / clip.w; XYZ = FTOI4(clip.xyz q) (the GS's
  12.4 X and Y; Z the integer); fog = clamp(vf19.w + vf18.x + vf18.w q, 0, vf18.y), F = its
  integer; colour = min(c vf31, 255) truncated; STQ = (u, v, uz) q, with vf19.xy added to (u, v)
  from vertex 1 on (vertex 0 skips the add: the pipelined loop loads it before).
- **ADC**: the texture qword's w is 2048.0 at a strip join (a masked S-8 UNPACK through STMASK
  BFBF writes it); added to the fog before FTOI4 it sets bit 15 of the XYZF2 word. A back face
  sets it too: vf6.x flips each vertex, and with P = 1 when bit 15 is set, else -1, triangle
  (k-2, k-1, k) is culled when P (-1)^k times its screen area (y down) is positive (the cross
  product of the last two screen edges, the earlier one times the parity).
- **Clipping**: CLIP of (X + vf13.x, Y + vf13.y, clip.w + vf13.z) against |vf13.w|; when any of a
  triangle's three vertices is off in x or y or below in w (FCAND 0x02FBEF), the clipper at 1948
  draws it as fans: it divides the three STQ back to (u, v, 1), transforms the positions again
  through its own clip-space matrix (data 22-25 times the object's, stored at 27-30 by the setup),
  rejects a triangle wholly outside one plane, and clips the rest at 1CC0.
- **End** (1000-1030): VI14 += 0x90 (0x238, 0x2C8, 0x358 rotate), VI2 = VI14, TPC 1040.

## The translation

- **VU thread** (`vu1_mesh.inc`, from `run_micro` in `port/hle/vif.c`): the 0078 setup runs on
  the VU as before (it is short, and it leaves every register as the program does). Each batch
  after it (an MSCNT with VI13 0x11; since `02e48c5` keyed on the loop, whatever setup ran, with
  the constants read from the registers at the batch: finding 167) is then read from
  VU1 data memory as the VIF left it and becomes one GS queue item (`GS_PATH_MESH`): the n
  vertices (pool position, u and v with the offset, Q factor, colour bytes: 32 bytes, the shape of
  the native renderer's vertex), the triangles the strip draws (joins left out, each triangle in
  the winding that leaves a back face with a positive area), the A+D qwords, PRIM, and the object's
  matrix, guard band, colour scale and fog terms. The batch's microprogram and its XGKICK do not
  run; the state its end leaves is set (vf6.x and data 1014, VI14, VI2, VI3, VI4, TPC). A batch
  with a vertex below the near bound (w < 1) runs on the VU (the CLIP test in homogeneous form, on
  the CPU: a 4x4 transform a vertex, SSE); one with a vertex only off the guard band sideways is
  drawn and the GPU clips it (since `02e48c5`; first those ran on the VU too). So does a batch of
  any other form (no EOP, another tag or PRIM, a join value other than 0 or 2048.0, ...), counted
  by reason.
- **GS thread** (`gs_mesh` in `port/gs/gs.c`): the A+D writes and PRIM through `gs_write`, as the
  packet would make them, then the backend's hook (`gs_mesh_hook`). `native_mesh` records a draw with the
  current GS state (`native_state`, as any draw), flag 4096 and the object's constants; batches of
  one object under one state join one command. It claims the whole scissor (where its pixels land
  is not known on the CPU).
- **GPU** (`native.wgsl`'s `native_mesh_vertex`, pipeline key bit 30): the maths above per vertex.
  A vertex inside the guard band (the same CLIP test) is placed as the GS places the kicked one:
  X and Y truncated to 1/16 pixel, Z to its integer, through `native_vertex`'s mapping, so it lands
  on the same position as the emulated path's vertex (coplanar passes of other programs still
  meet it in the Z test). Positions go out times w (homogeneous). The GS interpolates STQ, colour
  and fog linearly in screen space; the mesh's varyings are perspective-correct instead, each the
  value times w with w beside it, and the mesh fragment entry points (`native_pixel*_mesh`) divide
  one by the other, which is the screen-linear value, also on triangles a GPU clips (a
  no-perspective varying is not, at a clip vertex whose edge ends behind the eye). Colour and fog
  are rounded to 1/4096 and 1/65536 so a value all three vertices share comes out exact. Back
  faces: `CullMode::Back`. A vertex off the guard band goes out as the plain projection and the
  GPU clips its triangles; with `0170:gpuclip` the batches with a vertex below the near bound are
  drawn here too.
- **Modes**: skip (the program does not run: the gain), or **shadow** while `DHD_EXACT_PROBE`
  answers the flare's Z probe from the GS stream (the state-trace recipe): the batch also runs on
  the VU and its kicks go to the exact path only (`gs_vu_shadow`), so game state is the switch-off
  run's while the picture comes from the mesh items. `0170:skip` / `0170:shadow` force one.
  Native renderer with indexed fields only (`gs_mesh_mode`); the browser builds leave it out.

## Correctness

### Pictures

Finding 156's route, deterministic (`DHD_FRAME_CLOCK=1`), with `DHD_EXACT_PROBE=1` so the flare's
state comes from the GS stream and is the same in both runs (the switch is then in shadow mode:
the pictures come from the mesh items, the game state is the switch-off run's), GS captures
(`DHD_SCREENSHOT` with `DHD_CAPTURE_FIELDS`) at eleven fields: title 700, 950; shop 3200, 3500;
race start 6400, 6600, 6800, 7100; mid-race 9000, 9300, 9600; the native renderer with
`DHD_NATIVE_SPEC=sync`, windowed 1920x1080 on the hidden desktop. Per field: the mean absolute
difference over all channels (0-255), the share of pixels off by more than 8 and by more than 32 in
any channel, and the largest difference. The tolerance set for this gate: per field MAE <= 0.1 and
at most 0.1 % of the pixels off by more than 8.

Worst field of each scene, switch off against on:

| scene | 2x: MAE / > 8 / > 32 / max | 4x: MAE / > 8 / > 32 / max |
|---|---|---|
| title (700, 950) | 0.005 / 0.002 % / 0.001 % / 95 | 0.005 / 0.002 % / 0.001 % / 95 |
| shop (3200, 3500) | 0.000 / 0 / 0 / 1 | 0.000 / 0 / 0 / 1 |
| race start (6400-7100) | 0.014 / 0.007 % / 0.003 % / 117 | 0.014 / 0.007 % / 0.004 % / 129 |
| mid-race (9000-9600) | 0.065 / 0.080 % / 0.008 % / 145 | 0.064 / 0.085 % / 0.008 % / 143 |

Every field is within the tolerance at both scales. What differs: texel noise on a few textured
triangles (the GPU rounds S, T and Q to nearest, the VU toward zero, and a bilinear lookup lands on
the neighbour texel), and single pixels along edges where a vertex's 1/16-pixel position or its Z
differs by one step. The shadow check over the same route (`DHD_NATIVE_VU1_CHECK=1`, 2x, to field
9600: 146.6 M translated vertices, 139 M triangles) counts them: X or Y off by more than 1/16 pixel
on 4,878 vertices, Z by more than 1 on 141,808 (0.1 %), STQ beyond 1e-4 relative on 243, fog and
colour never; the ADC (cull) flag differs on 24,109 triangles (0.017 %: areas near zero, where the
VU's toward-zero rounding decides the sign).

With `0170:gpuclip` (every batch translated, the GPU clipping) title, shop and race start stay
within MAE 0.2, but mid-race fails: field 9000 MAE 12.5 (31 % of pixels off by more than 8), 9600
MAE 2.1 (Surprises).

The captures and their side-by-side and difference images stay local (game pictures, not committed).

### PCSX2 as the referee

Each scene's field (700, 3300, 6600, 9300) captured as a GS dump (`DHD_GS_CAPTURE`, 1x, shadow mode,
so the stream holds the translated loops' packets as the VU kicks them) with the switch off and with
`DHD_NATIVE_VU1=all`, replayed in stock PCSX2 2.8.2's software renderer (`compare_pcsx2.replay_pcsx2`),
against the port's native picture of the same field. 2.8.2 saves this game's field buffers (0x8C0,
0x1180) with the picture 224 rows down, where `compare_pcsx2.scanout` crops at DBY mod 8 (black);
the field's window was found by search instead (row 224 in every field), line-doubled as the
harness does. PCSX2 against the port, MAE / % of pixels off by more than 8:

| field | switch off | `all` |
|---|---|---|
| title 700 | 2.903 / 9.34 % | 2.903 / 9.34 % |
| shop 3300 | 11.503 / 45.37 % | 11.503 / 45.37 % |
| race start 6600 | 6.052 / 19.67 % | 6.053 / 19.67 % |
| mid-race 9300 | 6.326 / 27.58 % | 6.328 / 27.60 % |

The switch moves the port neither toward PCSX2 nor away from it (at most 0.002 MAE): what PCSX2
sees differently is the native renderer's own (finding 162: mip levels, filtering), the same in both
modes. The port's two pictures differ by MAE 0.001-0.073 at 1x.

### Game state

Boot state traces (`state_trace_run.py boot`, 600 fields, headless: no renderer, so the switch's
forced modes run and the items are dropped): the base (162's build) against this build with the
switch off, and against `0170:shadow`: all 600 rows identical in every column. Against `0170:skip`:
EE RAM, scratchpad, CPU, the hardware registers, sound and code identical in every row; the VU
memory and registers (from field 151) and the GS stream and registers (from 150) differ, by design
(the batches do not run, and their packets never reach the GS).

## Timing

### Method

Finding 156's route (`route_profile.py`: `DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1`, the race
route, `DHD_BENCH=300-14000`, the timeline), on the hidden desktop, windowed 1920x1080, release
builds with line tables (`build.py -- --release --debug`): the base is 162's build (`c864a4c`), this
build with the switch off and with `DHD_NATIVE_VU1=0170` (skip mode: no `DHD_EXACT_PROBE`), and
`0170:gpuclip` (every batch translated, the GPU clipping) for the ceiling. Two runs of each,
interleaved. Each run took the PC's timed game lock first (no new game of another lane starts),
then waited for an idle CPU before it started: six 5-second windows under 10 % load, or, since the
owner's background apps (Discord, NVIDIA Broadcast) keep this PC at about 11-14 %, after 4 minutes
six windows under 30 % within 6 points of each other; every run started on 11-16 % (recorded per
run). The first two attempts, with other lanes' builds running (79-100 % load before the runs),
are discarded. No frame cap held any run: the frame clock does not pace, vsync is off, and the
fastest runs reached 434 fields/s with a thread busy for the whole field.

### Windows, RTX 3070, 8x

Mean of two runs (each run's fps in brackets); ms per field; columns as finding 156's.

| scene | build | fps | field | game work / busy | vu work / busy | gs | render CPU / busy | GPU |
|---|---|---:|---:|---|---|---:|---|---:|
| title | base | 193 (190/195) | 5.19 | 1.55 / 0.92 | 5.21 / 5.19 | 2.64 | 0.61 / 0.45 | 1.46 |
| title | off | 193 (191/194) | 5.19 | 1.61 / 0.93 | 5.18 / 5.19 | 2.65 | 0.68 / 0.45 | 1.50 |
| title | on | 225 (223/227) | 4.45 | 1.46 / 0.94 | 4.44 / 4.45 | 2.03 | 0.85 / 0.52 | 1.38 |
| title | gpuclip | 268 (269/267) | 3.73 | 1.48 / 0.97 | 3.72 / 3.73 | 2.02 | 0.70 / 0.49 | 1.33 |
| shop | base | 263 (259/268) | 3.80 | 0.97 / 0.74 | 3.78 / 3.80 | 1.41 | 0.71 / 0.60 | 1.71 |
| shop | off | 262 (266/258) | 3.81 | 1.18 / 0.75 | 3.75 / 3.81 | 1.38 | 0.84 / 0.62 | 1.76 |
| shop | on | 290 (288/292) | 3.45 | 0.99 / 0.75 | 3.44 / 3.45 | 0.99 | 0.99 / 0.66 | 1.68 |
| shop | gpuclip | 434 (436/433) | 2.30 | 0.95 / 0.73 | 2.31 / 2.30 | 1.07 | 0.93 / 0.61 | 1.80 |
| race start | base | 230 (226/235) | 4.34 | 3.35 / 3.08 | 4.33 / 4.34 | 2.43 | 1.83 / 1.55 | 1.92 |
| race start | off | 230 (233/227) | 4.35 | 3.34 / 3.16 | 4.29 / 4.34 | 2.58 | 1.93 / 1.59 | 1.94 |
| race start | on | 280 (281/278) | 3.57 | 3.09 / 3.03 | 3.57 / 3.57 | 2.00 | 1.79 / 1.54 | 1.92 |
| race start | gpuclip | 303 (307/299) | 3.30 | 3.14 / 3.10 | 3.27 / 3.28 | 1.86 | 1.69 / 1.45 | 1.96 |
| mid-race | base | 292 (289/295) | 3.42 | 2.72 / 2.60 | 3.39 / 3.42 | 1.85 | 1.43 / 1.17 | 1.52 |
| mid-race | off | 295 (296/294) | 3.39 | 2.70 / 2.59 | 3.37 / 3.39 | 1.91 | 1.42 / 1.17 | 1.52 |
| mid-race | on | 342 (341/343) | 2.92 | 2.60 / 2.55 | 2.92 / 2.92 | 1.62 | 1.45 / 1.18 | 1.55 |
| mid-race | gpuclip | 355 (364/346) | 2.82 | 2.72 / 2.80 | 2.65 / 2.66 | 1.45 | 1.45 / 1.17 | 1.60 |

Whole route (fields 300-14000): base 263.4 / 269.7, off 270.0 / 264.6, on 308.5 / 310.8, gpuclip
356.3 / 349.8 fields/s.

- **The switch off is the base** within the runs' spread (0-1 %): the default build is unchanged.
- **On:** +17 % in the title, +11 % in the shop, +22 % at the race start, +16 % mid-race. The VU
  thread does 0.3-0.7 ms less a field and the GS thread 0.3-0.6 ms less; the VU thread is still the
  limit (busy for the whole field), now 0.3-0.5 ms above the game thread in races.
- **Against 162's estimate** (race start -1.5 ms VU, -0.5 to -0.7 ms GS): the GS part is met
  (-0.58), the VU part by half (-0.72): a quarter of 0078's batches still run on the VU (the
  clipper's), and the translation costs the VU thread some of what it saves. With every batch
  translated (`gpuclip`) the race start drops 1.03 ms of VU work and the shop 1.47 ms: the shop's
  0078 batches are mostly clipped ones.
- **The GPU** does not notice: 1.4-1.9 ms a frame either way at 8x.

### Mi 11i on the t612 profile, 2x

Measured with phase 2's APK (finding 167: release-signed, the shipped builds' PGO profile, `39533bb`)
and `DHD_NATIVE_VU1=0170`, which is this phase's loop with `02e48c5`'s sideways clipping (85 % of its
batches on the GPU rather than 77 %); finding 156's run script, the frozen t612 profile, two runs of
each (the switch-off pair shared with finding 167). Mean of the two runs; ms a field from the run's
timeline: the game, VU and GS threads' CPU time, the render thread's, the GPU (sampled).

| scene | switch | fps | field | game | VU | GS | render | GPU |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| title | off | 49.6 (49.3/49.8) | 20.18 | 15.34 | 18.71 | 17.53 | 4.65 | 0.40 |
| title | 0170 | 59.4 (59.5/59.3) | 16.84 | 12.82 | 15.51 | 13.98 | 4.99 | 0.43 |
| shop | off | 63.0 (63.0/63.1) | 15.87 | 10.71 | 14.36 | 9.73 | 6.38 | 0.51 |
| shop | 0170 | 70.7 (70.6/70.8) | 14.14 | 9.59 | 12.61 | 8.08 | 6.75 | 0.53 |
| start | off | 58.8 (58.5/59.0) | 17.01 | 14.36 | 14.72 | 15.04 | 13.33 | 0.83 |
| start | 0170 | 63.5 (63.1/63.8) | 15.75 | 13.22 | 12.73 | 12.55 | 13.95 | 0.85 |
| mid | off | 72.7 (72.9/72.6) | 13.75 | 11.78 | 11.89 | 11.83 | 10.69 | 0.64 |
| mid | 0170 | 80.7 (80.8/80.6) | 12.39 | 10.48 | 10.43 | 10.10 | 11.14 | 0.66 |
| route | off | 63.9 (63.7/64.0) | 15.66 | 12.06 | 13.06 | 12.71 | 9.49 | 0.59 |
| route | 0170 | 71.9 (71.8/72.0) | 13.91 | 10.61 | 11.13 | 10.67 | 9.96 | 0.61 |

- **The route: 63.9 -> 71.9 fields/s (+12.5 %)**; the title +20 %, the shop +12 %, the race start
  +8 %, mid-race +11 %. The VU thread does 1.9 ms and the GS thread 2.0 ms less a field over the
  route; the render thread 0.5 ms more (the mesh draws).

### The near ground in fast race frames: not the texture level

The difference `gpuclip` leaves at fields 9000 and 9600 (the owner's reading of the side-by-sides:
same geometry, the VU's near ground softer and streaky, the GPU's sharper with high-contrast
patches) looked like a texture level (mip or LOD) choice. It is not one:

- **The native renderer samples no mip level, in either path.** A decoded GS texture has one
  level (`textures.rs`, `levels: 1`); TEX1's MXL, LCM, L and K and MIPTBP1 are kept in the GS
  state and never read by the draw (`native_texture`), and `native.wgsl` computes no LOD. Only
  texture-pack replacements are mipmapped, by the hardware from the derivatives of u = S/Q, which
  the mesh path gives as the emulated one does. So the switch cannot change a level choice, and the
  pictures compared here are both level 0. (The PS2 uses its two mip levels on up to 39 % of race
  draws, finding 162: the native renderer differs from it there in both modes, a separate matter.)
- **It is the near bound, and only the near bound.** Two measurement runs (shadow mode, 2x, the
  race fields): batches with a vertex off the guard band sideways drawn and clipped by the GPU,
  those with a vertex below the near bound on the VU: every field within tolerance (MAE <= 0.065,
  <= 0.08 % of pixels off by more than 8). Every batch on the GPU but the triangles touching a
  vertex below the near bound left out: field 9000 MAE 5.5 (dropping them is wrong too: the PS2
  draws something there).
- **The clipper's fans there are not the GPU's clipped triangles.** In the GS stream of fields
  9000-9001 (switch off) the clipper's fans are 204 vertices, all at the edges of its own frustum
  (data 176-179: X 1024 and 3071, Y up to 2645, where the screen is 512 x 224 GS units about 2048)
  and none on the near plane (Q at most 0.58: w >= 1.7), although the near ground's triangles cross
  it. What exactly the clipper's code (1CC0-2700) makes of a near-plane polygon was not traced
  further.

So the sideways crossings went to the GPU (`02e48c5`: 0170 from 77 % to 85 % of its batches on the
GPU, pictures unchanged within the tolerance), and the near-plane ones stay on the VU; `gpuclip`
remains a measurement. A full 100 % needs the clipper's near-plane result reproduced (its code, or
a GPU pass that does what it does), not a texture level.

## Decisions

- **The phone only on the t612 profile, at 2x only** (the owner, 2026-11-06): the full-speed Mi 11i
  and 4x are skipped; the throttled profile at 2x is where the VU and GS threads limit (finding
  156).
- **Windows RTX at 8x only, uncapped** (the owner, 2026-11-06: 4x tells nothing more): the PC
  runs well above 240 fields/s at 8x, so no cap or vsync may hold the timing runs.

## Surprises

- **The strip parity does not carry over.** The prologue sets vf6.x to 1 at every batch (0098)
  before the header's bit 15 decides its sign; the first translation carried it from batch to
  batch, and the shadow check found the back-face flags wrong on 31 % of triangles (the title's
  rock half see-through). Fixed: ADC now differs on 0.016 % (near-zero areas, the VU's rounding).
- **Mixed w breaks a GPU's clipping.** Vertices inside the guard band were first sent with w = 1
  and the others homogeneous: one triangle's clip vertices then took garbage texture coordinates.
  Every vertex now goes out times its real w.
- **A GPU's clipping does not draw what the VU's clipper draws.** With every batch translated
  (`0170:gpuclip`), title, shop and race start match (MAE 0.002-0.2), but in fast race frames the
  near ground under the rider is textured differently (field 9000: MAE 12.5, 31 % of pixels off by
  more than 8): the clipped fans there show the game's stretched, streaky look and the GPU's clipped
  triangles a straight perspective mapping. The clipper re-projects its fans through its own
  matrices (data 176-179 and 27-30), and only the near-plane crossings differ ("The near ground in
  fast race frames" above: not a texture level). Batches with a vertex below the near bound run on
  the VU (the CLIP test on the CPU); first every batch the clipper would see did, 77 % of 0078's
  batches translated on the route, then 85 % with the sideways ones on the GPU (`02e48c5`).

## Result

The gate's answer: **yes, phase 1 justified phase 2**, and the owner approved it on these numbers
(2026-11-06; its results are finding 167).

- **Correct:** the pictures with 0170 on the GPU are within the tolerance in every scene at 2x and
  4x (MAE <= 0.066, <= 0.085 % of pixels off by more than 8), the shadow check finds no colour or
  fog difference, and the game state is the switch-off run's (boot, and race with every phase 2
  loop on).
- **Faster:** Windows RTX 8x +16 % over the route (+11 to +22 % per scene), the Mi 11i on the t612
  profile at 2x +12.5 % (+8 to +20 %). The GPU does not notice on either.
- **The translator's own cost** on the VU thread: 0.41 us a translated batch, 10 ns a vertex
  (`DHD_VU1_PROFILE`, Windows 8x), about 0.16 ms a field on the route against the 0.3-0.7 ms of VU
  work and 0.3-0.6 ms of GS work it saves.
- **Left on the VU:** the batches with a vertex below the near bound (15 % of 0170's on the route),
  because the clipper's fans there are not what a GPU's clipping draws.
