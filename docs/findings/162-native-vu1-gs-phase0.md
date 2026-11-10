# 162: Native VU1 and GS rendering, phase 0: what the game draws and how

Status: current
Checked: 2026-11-10

Branch `native/vu-gs` from `main` 9ecb8e1, started 2026-11-06. At first a local branch, kept off
`main` until the owner decided. Merged into `main` with the rest of `native/vu-gs` at 32df819b on 2026-11-10, behind finding
194's Native VU setting (off by default); ships in 0.7.15.

## Goal and acceptance

The owner's direction (2026-11-05/06): keep the decompiled game code; replace what is still
emulated for graphics (VU1 microprograms, VIF/DMA, the GS command stream and its renderer front
end) with native GPU rendering, built at the packet level first: the game keeps building its
VIF/DMA packets, a translator turns them into GPU draws whose shaders do each VU1 program's maths.
Finding 156 measured the gain: VU + GS are about half of the CPU work on every machine; on
the low-end phone profile removing both takes the race start from about 60 to 80 fps.

Phase 0 is measurement and inventory only, no renderer change. It answers, with numbers:

1. **The game thread's hot functions**, named (the decomp's names or addresses with what they do)
   and grouped by system (physics, AI, camera, scene traversal, **packet building**: the code that
   writes VIF/DMA/GIF packets for VU1 and the GS, sound, other), per scene (title, shop, race
   start, mid-race) on Windows and the Mi 11i (finding 156's routes and runners). How many ms of
   the game thread a deeper hook (before packet building) would also remove.
2. **The VU1 programs**: every microprogram entry the game uploads and runs, what each does
   (skinning, terrain, sky, particles, shadows, HUD/2D, ...), its share of VU time per scene, its
   inputs (the packet layout: unpacks, matrices, lights, flags, which VU0/EE data it reads) and
   outputs (GIF packets: primitive types, registers, texture use). Which programs cover 90 % of
   the time.
3. **The GS features the game uses** per scene: primitive types, texture formats and CLUTs,
   alpha/blend/test modes, depth use, render-to-texture and framebuffer reads (the flare's Z read,
   local-to-local copies, feedback effects), PATH1/2/3 mixes, anything a native path must
   reproduce or replace.
4. **The translation plan**: per program and feature, how it maps to a GPU draw (vertex shader for
   the VU1 maths, the native renderer's fragment path, what stays emulated longest), the
   order to build it (biggest gain first), and how each step is checked (pictures against the
   current renderer and PCSX2, state traces).

Acceptance: the tables and lists above in this finding, the method to repeat them, and a phase 1
proposal (which program first, the prototype's scope and its measurement). Tools that are cheap
and worth keeping may be committed on this branch; nothing changes the game's state or output.

## Progress

- [x] 2026-11-06: finding reserved on the local branch
- [x] 2026-11-06: release build with line tables (`build.py -- --release --debug`), core unchanged
- [x] tools: `sample_profile.py --stacks` (whole call stacks through dbghelp's StackWalk64),
  `route_profile.py` (finding 156's route on the hidden desktop, timeline per scene, optional
  sampling per scene), `gs_census.py` (what a PCSX2-format GS dump uses), `DHD_VU1_LAYOUT`
  (per kind of VU1 run: the VIF1 commands before it and the GIF tags it kicks)
- [x] GS census of the route (27 captures of 6 fields, every 500 fields)
- [x] Windows: timing, two profiles with stacks, VU1 profile and layout per scene
- [x] the Mi 11i: game-thread call stacks per scene (2x, as players have it)
- [x] race state traces: the census code and the census itself change no game state
- [x] tables, translation plan, phase 1 proposal
- [x] commits on this branch: `814809f` (`sample_profile.py --stacks`, `route_profile.py`),
  `e0e26d8` (`gs_census.py`), `c864a4c` (`DHD_VU1_LAYOUT`); `check.py quick` passed

## Summary

- **The game thread spends 31-61 % of its work building and handing off the graphics packets**
  (Windows; 36-56 % on the Mi 11i): the scene walk `func_0021B8A8` (the engine's hand-written
  node interpreter with its VU0 culling and matrix code) and the DMA kick that walks the chain and
  copies it into the graphics queue. That is **0.4-1.5 ms a field on the RTX PC and 0.8-2.5 ms
  on the Mi 11i**, the most at the race start. A packet-level native path keeps all of it; a
  deeper hook (draw records instead of VIF chains) could remove most of it, and on a fast desktop
  it is the next limit once the VU and GS threads are gone (the game thread, 3.6 ms busy at the
  race start, would drop to about 2.1 ms).
- **What stays on the game thread** in races: rider physics and collision (20-25 %, the triangle
  tests `func_00213C08` and `func_00213658`), the engine's animation/scene-graph update `func_002065C0`
  (9-19 % in every scene), rider control, AI and components (8-12 %), particles (3-5 %), the sound
  mixer (2-5 %), and the emulated kernel's `FlushCache` poll (2-9 %).
- **One VU1 loop is a third of everything**: overlay B's prelit textured mesh loop (MSCAL 0078,
  loop 0170) takes 37-49 % of VU1 time and draws 34-48 % of the vertices in every scene. With the
  four other prelit loops that share its packet layout (1638, 07F0, 06F0, 17E0) it is 60-66 % of
  VU1 time in races; with the lit loops (0040/1050/12F0/1430) and the skinning (0010/0020) about
  90 % in every scene. Overlay A (particles) is up to 10 % (mid-race).
- **The mesh packets are simple and regular**: per object a 4x4 matrix, texture register triplets
  and colour/fog constants; a per-object pool of up to ~190 float positions and byte normals; per
  batch a strip of up to 44 vertices of 3 qwords (a byte index into the pool, a 5:5:5:1 colour, a
  16-bit UV) with strip restarts patched in by masked byte unpacks. The output is always the same
  GIF form: PACKED ST, RGBAQ, XYZF2 strips (`PRIM` 03C or 07C) after an A+D tag that sets TEX0,
  TEX1, MIPTBP1 and TEXFLUSH. That is an indexed vertex buffer and a vertex shader.
- **The GS features are few**: triangle strips (93-100 % of primitives), sprites, clip fans; 8- and
  4-bit textures with CT32 palettes (CSM1) on nearly every draw, bilinear filtering, two mip levels
  on a third of race draws, fog on nearly every draw, five to nine blend equations, Z24 with
  GEQUAL; no destination-alpha test, PABE, FBA, dithering, frame masks or region clamps. What a
  native path must keep: **two small render-to-texture targets** (64 pixels wide, the riders'
  shadows), **T8H/T4HL textures stored in the Z24 buffer's spare top byte**, textures uploaded as
  CT32 and read as T8/T4 (15-17 uploads a field in races, over PATH2), CLUT loads (140-500 a
  field), the flare's 4x4 Z readback and a few 64x64 local copies.
- **Phase 1 proposal**: translate the 0078 loop alone (native mode only, behind a switch), keeping
  everything else emulated. Expected: the VU thread about a third lighter (Windows race start
  4.4 -> about 2.9 ms), the GS thread about a quarter, so a fast desktop moves from VU-bound to
  game-thread-bound (race start about 214 -> 270 fps) and the low-end phone profile's race start
  from 60 to about 75-80 fps, where its game and render threads take over.

## Method

All of it is repeatable with the committed tools on any Windows PC with the game; the phone part
with finding 156's runner (simpleperf added, below).

- **Build**: this branch, `build.py -j 6 -- --release --debug` (the shipped code with line tables
  and a PDB). Core id unchanged from `main` (`DHD_VU1_LAYOUT` is host code in `port/hle/vif.c`).
- **Route**: finding 156's (`route_profile.py`: race_compare.py's navigation and race pad, seed
  81257, `DHD_RACE_SYNC=1`, `DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1`, no movies, memory card,
  sound or settings, `DHD_BENCH=300-14000`, `DHD_TIMELINE`), on the hidden desktop, 4x, windowed
  1920x1080, the timed game lock. Scenes are field windows: title 450-1100, shop 2950-3750, race
  start 6400-7300 (GO is 6321), mid-race 9000-14000.
- **Game-thread stacks (Windows)**: `route_profile.py EXE OUT --profile` runs
  `sample_profile.py --pid PID --stacks game` for a few seconds in each scene (started from the log's
  `nav step 2`, `nav step 6` and `race anchor=g` lines) and records the fields each window
  covered. Two runs: 5,000 game-thread stacks (title fields 472-892, shop 3032-3703, race start
  6433-6975 and 6441-6914, mid-race 12111-13312). A sample is a wait when its innermost frames
  are the queue spin (`idle_wait`) or an OS wait; the rest is work. A sample's system is the
  first rule in the list below that matches a frame of its stack; a system's ms = its share of
  the work samples x the game thread's CPU work per field (CPU minus spin) from finding 156's clean
  timing runs (the PC was loaded during this lane's runs, see Caveats).
- **Game-thread stacks (Mi 11i)**: finding 156's runner and APK (0.7.11's code, PGO, line tables,
  release-signed), 2x as players have the phone, with `simpleperf record -t <game tid> -f 500
  --call-graph dwarf,32768` at the same four scenes (the build omits frame pointers, so DWARF);
  6,670 stacks, read through simpleperf's report library and classified by the same rules; ms
  from finding 156's phone timing runs (as players have it).
- **VU1 per program**: `DHD_VU1_PROFILE` with `DHD_VU1_PROFILE_FIELDS=300-14000/50`, rows summed
  per scene window (the profile's own clock inflates its totals by about a third, finding 156:
  shares, not ms).
- **VU1 packet layout**: `DHD_VU1_LAYOUT=file DHD_VU1_LAYOUT_FIELDS=450-1100,2950-3750,6400-7300,9000-13990`
  (new, `port/hle/vif.c`): per scene and kind of VU1 run (overlay, MSCAL program, entry, MSCNT,
  VI13 at the mesh continuations 1040/1CB0/12E0) the VIF1 commands since the previous run (UNPACK
  format, mask, sign, address and count range, STCYCL, STMASK value, OFFSET/BASE, MPG, DIRECT) and
  the GIF tags each XGKICK sends (FLG, NREG, REGS, PRE/PRIM, the registers written by A+D, vertices
  and qwords). The microcode itself: `vudis.py` (finding 019).
- **GS features**: one untimed route run with `DHD_GS_CAPTURE` (6 fields every 500, at 1x; threaded
  graphics off, the stream is the same at any scale), then `gs_census.py` over the 27 dumps grouped
  by field window. It runs the GIF stream through a register model and counts per primitive the
  state it is drawn with; render-to-texture is a texture whose pages were last written by a draw.
- **No game-state change**: race state traces (5,000 fields) of the base build without the census
  code, of this build, and of this build with `DHD_VU1_LAYOUT` on: identical (below).

Systems (first match wins): sound mixer at the vsync (`sound_tick`/`render` innermost);
`FlushCache` (`func_0025A960`, the kernel HLE's `kernel_poll`); the DMA kick (`dma_mmio_write`
on the stack); lens flare and Z probe (`func_00228328`, `func_002286F8`); packet building
(`func_00215B68`, `func_002161B8`, `func_002184D0`, `func_00218720`, `func_0021B8A8`,
`func_002167C0`, `func_00216980`, `func_0021E9A8` and the full-screen passes); particle packets
(overlay A senders); 2D/HUD/loading/display senders; particle update `func_0022DE40`; engine
animation `func_002065C0`; rider physics `func_0012D360`/`func_00135690`; rider control and AI
`func_0012D380`; rider components `func_0012D3C8`; per-rider input `func_0012C8B0`,
`func_0012D248`, `func_00134688`; race and menu update; the frame wait.

## 1. The game thread's hot functions

### How a frame is built

The race loop `func_001B40A0` runs per field: the game update `func_0016C0C8` -> the race update
`func_0011A100` (per rider: input `func_0012C8B0`, `func_0012D248`, physics `func_0012D360`, control
and AI `func_0012D380`, components and animation `func_0012D3C8`, then rider-rider contacts
`func_00134688`); the view chains `func_00215B68` (per view: the VU1 program table
`func_0022F600`, the scene walk `func_002167C0` -> `func_0021B8A8`, the particle chains
`func_0022E9A0`, then the DMA kick `func_0021E9A8`); the engine's animation update `func_002065C0`;
per player the second chain `func_002184D0`/`func_002161B8`; the lens flares `func_00228328`;
the particle update `func_0022DE40`; `sceGsSyncPath` (`func_002433A8`) and the vsync wait. The
menus run `func_001EB4D0` with the same view chain code.

`func_0021B8A8` is the core: a hand-written node interpreter (finding 019) that dispatches on the
low 6 bits of each scene node, tests boxes and builds matrices with VU0 macro code and
microprograms (`vcallms` 0x0-0xCD0), and copies each visible mesh's VIF chain with its matrices
into the frame chain. Its hottest line (two thirds of its own samples) is the node loop's first
doubleword load: the walk is memory-bound on the scene data.

### Windows (RTX 3070 PC, 4x): share of the game thread's work, and ms a field

ms = share x finding 156's game work (title 1.55, shop 0.86, race start 3.57, mid-race 2.92 ms).

| system | main functions | title | shop | race start | mid-race |
|---|---|---|---|---|---|
| **packet building: scene walk** | `func_0021B8A8` (+ `func_0021CC44`, `func_0021B7E8`), VU0 cull/transform (5-6 % of the work), `func_00215B68`, `func_002161B8` | 42.3 % 0.66 | 38.4 % 0.33 | 33.3 % 1.19 | 25.2 % 0.74 |
| **packet hand-off: DMA kick** | `func_0021E9A8` -> `dma_mmio_write`: chain walk and copy into the graphics queue | 18.6 % 0.29 | 10.0 % 0.09 | 6.8 % 0.24 | 5.5 % 0.16 |
| lens flare, Z probe; 2D, HUD | `func_00228328`, `func_002286F8`; `func_002013F8`, `func_002102A8` | 0.5 % | 0.5 % | 1.1 % 0.04 | 0.0 % |
| rider physics and collision | `func_00135690` -> `func_00131998` -> `func_00214BA0` (spatial batch query) -> `func_00210CA8`/`func_00210A18` -> `func_00213C08` (mesh BVH vs ray/box), `func_00213658` (swept sphere vs triangle) | | | 19.8 % 0.71 | 25.0 % 0.73 |
| engine animation, scene graph | `func_002065C0` -> `func_00206440` -> `func_00204C40`/`func_00204CE0` (node evaluation), `func_00206E48` (keyframes), `func_002628A0` | 19.4 % 0.30 | 18.4 % 0.16 | 10.4 % 0.37 | 11.3 % 0.33 |
| rider control and AI | `func_0012D380` (controller vtable, `func_0015BFC8`, `func_00154058`) | | | 7.8 % 0.28 | 3.3 % 0.10 |
| rider components, animation | `func_0012D3C8` (`func_001AF0C0`-`func_001B0AF0`, `func_0012D860`) | | | 4.5 % 0.16 | 6.0 % 0.18 |
| per-rider input, contacts | `func_0012C8B0`, `func_0012D248`, `func_00134688` | | | 2.8 % 0.10 | 3.0 % 0.09 |
| race and menu update, other | `func_0011A100`'s rest, `func_00220DA0`, menus | 0.3 % | 0.5 % | 4.5 % 0.16 | 8.4 % 0.25 |
| particle update | `func_0022DE40` -> `func_0022B708`, `func_0022B028` | | 4.3 % 0.04 | 3.3 % 0.12 | 4.6 % 0.13 |
| sound mixer (SPU2) at the vsync | `func_0025AB70` -> `sound_tick` | 2.7 % 0.04 | 4.9 % 0.04 | 2.5 % 0.09 | 2.8 % 0.08 |
| kernel HLE: `FlushCache` | `func_0025A960` -> `kernel_poll` | 8.5 % 0.13 | 8.6 % 0.07 | 1.5 % 0.05 | 2.2 % 0.06 |
| frame wait, other | `func_0025AB70`, `func_00238C00` | 7.6 % 0.12 | 14.3 % 0.12 | 1.7 % 0.06 | 2.7 % 0.08 |
| **packets in all** | | **61.4 % 0.95** | **48.9 % 0.42** | **41.2 % 1.47** | **30.7 % 0.90** |

Waits were 80-83 % of the game thread's samples in the title and the shop (it waits for the VU
thread) and 1 % in races. Inside the scene walk, VU0 microprograms are 5-6 % of the game thread's
work and the wasm memory-access helpers 7-14 %.

### The Mi 11i (2x, as players have it)

ms = share x finding 156's phone game work (2.35, 1.76, 5.91, 4.97 ms).

| system | title | shop | race start | mid-race |
|---|---|---|---|---|
| **packet building: scene walk** (VU0 13-18 % of the work) | 35.8 % 0.84 | 32.3 % 0.57 | 31.3 % 1.85 | 27.5 % 1.37 |
| **packet hand-off: DMA kick** | 19.6 % 0.46 | 14.1 % 0.25 | 9.2 % 0.54 | 7.9 % 0.39 |
| lens flare, 2D | 0.6 % | 0.7 % | 1.5 % 0.09 | 0.3 % |
| rider physics and collision | | | 16.5 % 0.98 | 21.0 % 1.04 |
| engine animation, scene graph | 13.0 % 0.31 | 16.1 % 0.28 | 10.1 % 0.60 | 8.9 % 0.44 |
| rider control, AI, components, input | | | 13.2 % 0.78 | 11.3 % 0.56 |
| race and menu update, other | 6.6 % | 5.9 % | 8.5 % 0.50 | 10.2 % 0.51 |
| particle update | | 3.4 % 0.06 | 3.3 % 0.20 | 5.0 % 0.25 |
| sound mixer | 2.4 % | 4.5 % | 2.1 % 0.12 | 2.3 % 0.11 |
| kernel HLE: `FlushCache` | 6.9 % 0.16 | 8.4 % 0.15 | 2.8 % 0.17 | 1.7 % 0.08 |
| frame wait | 15.1 % 0.35 | 14.3 % 0.25 | 1.4 % | 4.0 % 0.20 |
| **packets in all** | **56.0 % 1.32** | **47.1 % 0.83** | **42.0 % 2.48** | **35.7 % 1.77** |

The split is the desktop's, with VU0 code relatively slower (13-18 % of the work against 5-6 %)
and the vsync path heavier. On the low-end profile (finding 156: game work 11.2 ms at the race
start) the same share would be about 4.7 ms.

### What a deeper hook would also remove

The packet path in the tables (scene walk + DMA kick) is the upper bound of what a hook *before*
packet building removes from the game thread: **Windows 0.42-1.47 ms a field (31-61 %), the Mi
11i 0.83-2.48 ms (36-56 %)**. A packet-level translator removes none of it (it needs the packets);
a deeper hook would replace `func_0021B8A8`'s packet output with draw records (object, mesh,
matrix, textures) but still needs the walk's culling and matrices, so the real gain is that bound
minus a native walk (likely a small fraction of today's cost: today it runs as recompiled asm with
emulated VU0 macro code). The DMA kick alone (copy into the queue) is 0.09-0.29 ms on Windows and
0.25-0.54 ms on the phone.

Where it would pay: on the RTX PC after VU and GS are native the game thread is the limit (busy
3.6 ms at the race start, 3.0 mid-race); removing the packet path takes it to about 2.1 / 2.1 ms,
next to the render thread (1.8 / 1.3 ms busy). On the low-end phone profile the render thread
(12.3 ms) is the next limit after the game thread (12.4 ms), so there the deeper hook gains
frames only with a cheaper render thread.

## 2. The VU1 programs

Two overlays upload at VU1 address 0 (finding 019): **overlay B** (world meshes; 0x269C50) at the
start of every view chain, **overlay A** (particles; 0x265C00) in each particle batch: 14-22 MPG
uploads a field (6.5-10 K words). Overlay B's mesh programs share one shape: the MSCAL entry sets
VI13 (the loop), runs the setup at 1AE0 (object x view-projection, a light matrix, a second matrix
product into qwords 27-30 and 176-179) and ends; each later batch is an MSCNT at 1CB0 (the first,
with the vertex pool) or 1040 (the rest), which jumps to the loop at VI13 x 8. The loop transforms
each vertex, divides, projects, computes fog from w, clips (CLIP + FCAND against the two previous
vertices, the clipper at 1948/1CC0 writes fans), and the GIF tail at 0FF8 kicks one strip.

### VU1 time per program and scene (Windows 4x)

Share of VU1 time (the profile's clock: title 5.7, shop 4.5, race start 5.2, mid-race 3.8 ms a
field, inflated; VU1 code itself is 2.6-3.3 ms of the VU thread's 3.5-4.4 ms in races, finding
156). A program includes its MSCNT batches.

| program | loop (VI13) | what it does | title | shop | start | mid | runs/field (mid) |
|---|---|---|---:|---:|---:|---:|---:|
| B 0078 | 0170 (0x11) | prelit textured mesh, fog, backface cull, clipping: the world (terrain, track, scenery; opaque layer 03C and blended layer 07C) | 42.5 | 49.4 | 37.2 | 38.3 | 519 |
| B 0020, 0010 | setup 03C8, skinning loop 0468, tail 0590; batches 0x20C/0x260/0x616 | 3-bone skinned, lit meshes: riders and bikes; 0x616 batches draw their shadow silhouettes | 32.1 | 23.5 | 10.2 | 6.9 | 84 + 27 |
| B 0040 | setup 11E8, lighting loop 1280; batches at 12E0/1040 with 0x20C, 0x260, 0x616 | lit (normals, light directions) meshes: rider and bike parts, dynamic objects; 0x616 shadows | 2.0 | 7.3 | 14.6 | 9.4 | 190 |
| B 1638 | 1708 (0x2C9) | prelit textured, clipping, no backface cull (two-sided: foliage, fences) | 3.4 | 3.6 | 10.9 | 12.0 | 122 |
| B 07F0 | 08B0 (0x100) | prelit textured, no clipping (objects inside the view) | 0.4 | | 3.0 | 8.5 | 319 |
| B 06F0 | 0798 (0xE0) | prelit textured, no clipping, no backface (also untextured blended 06C) | 10.1 | 1.0 | 4.7 | 3.9 | 193 |
| B 17E0 | 17F0 (0x2FE) | prelit textured, clipping, no backface | 4.2 | 10.9 | 3.9 | 3.3 | 52 |
| B 2DC8 | own setup 2FD8, loop 2DE8 (0x5BD) | second matrix on position and normal: likely an environment-mapped (reflective) layer | | | 2.6 | 4.4 | 28 |
| B 1430, 1050, 12F0, 0C20, 0A90 | 1440 / 1110 / 13A0 / 0C30 / 0AA0 | lit meshes (normals at 373+, lights at 6-8 and 1015); 1430 draws untextured (02C) | 1.4 | 0.4 | 4.5 | 2.3 | 52 |
| B 0000 | 05A0, then batches 0x20C/0x260 | rigid per-vertex bone transform and lighting of the pool (indices into bones at 856+) | 3.0 | 0.6 | 0.5 | 0.1 | 0.4 |
| B 30A0 | setup 3190, loop 3140 (0x616) | orthographic projection with the matrix at 1008-1011, no divide: shadow geometry | | | 0.4 | 0.5 | 20 |
| B 0030, 0050 | 0050 | kick the prebuilt GIF packet at 0x6C (register setup, full-screen passes) | 0.4 | 0.2 | 0.2 | 0.2 | 16 |
| B 0280, 0948, 2738, 28D0, 2B10, 2C20 | 0338, 0958, 2748, 28E0, 2B20, 2C30 | other mesh variants (2738/28D0: 12-qword records; 2B10/2C20: untextured) | 0.5 | 0.6 | 0.6 | 0.3 | <10 |
| A 2E48 | | particle quads (2 vertices a loop, ST): snow and dust | | | | 3.6 | 9.3 |
| A 29C0 | | particle generator: a recurrence with random numbers, MAC-flag culling, 318 sprites a kick | 0.1 | 2.0 | 2.6 | 3.4 | 4.0 |
| A 0E10, 02D0, 1240, 26D8, 1010 | | textured fog sprites (UV), billboards; 26D8 register packets and sprites | | 0.4 | 3.6 | 2.8 | 14 |

**90 % of VU1 time**: title {0078, 0020, 06F0, 0010, 17E0, 1638} 92 %; shop {0078, 0020, 17E0,
0040, 1638} 91 %; race start {0078, 0040, 1638, 0020, 06F0, 17E0, 1430, 07F0, 29C0, 2DC8} 90 %;
mid-race {0078, 1638, 0040, 07F0, 0020, 2DC8, 06F0, 2E48, 29C0, 17E0} 92 %. The phone's order is
the same (finding 156's 2x profile summed by scene: 0078 36-50 %, 0020 9-27 %, 0040 8-15 %).

### Vertices a field by loop (the unit a vertex shader replaces)

From the layout census (kicked vertices): title 85,500, shop 44,700, race start 63,500, mid-race
51,600 a field.

| loop | title | shop | start | mid |
|---|---:|---:|---:|---:|
| 0170 (0078) | 37.7 % | 48.4 % | 38.1 % | 33.8 % |
| 0798 (06F0) | 27.8 % | 2.0 % | 6.0 % | 4.2 % |
| 08B0 (07F0) | 0.3 % | | 4.6 % | 14.6 % |
| 1708 (1638), 17F0 (17E0) | 10.4 % | 22.1 % | 9.7 % | 9.9 % |
| lit 1110 (0x20C), 13A0 (0x260), 1440, 0C30 | 18.4 % | 22.7 % | 30.0 % | 15.8 % |
| skinning tail 0590 | 4.3 % | 2.0 % | 1.5 % | |
| shadow 3140 (0x616) | | | 3.5 % | 4.0 % |
| 2DE8 (2DC8) | | | 0.8 % | 2.0 % |
| overlay A (2E48, 0E10, 29C0, 02D0) | | 2.5 % | 6.2 % | 14.2 % |

### The packet layout (VU1 data memory, qword addresses)

Once per view, with the overlay upload: constants at 9-25 (view and projection, light matrix; one
17-qword UNPACK), 26, 0x6C (the prebuilt GIF packet), 0xA4, 996, 1015-1016.

Per object (the MSCAL run), STCYCL 4/4, V4-32 UNPACKs: 0-3 the object matrix (when it changes),
4-5 colour and fog parameters, 20, 150-158 up to three texture register triplets (TEX0, TEX1,
MIPTBP1, selected per batch by the header: `LQ 151(vi3)`); lit programs add 6-8 (light directions)
and 1015 (light colour); skinned programs 856+ (15-27 bone matrices, 60-108 qwords). Then DIRECT
(PATH2) for GS state and texture uploads.

Per object vertex pool (the first batch, at 1CB0): S-32 at 180 (count), V3-32 unsigned at 181+
(float positions, 55-190), V3-8 at 373+ (signed byte normals); skinned objects V4-32 at 181+
(position and weights).

Per batch (each MSCNT), into one of three rotating buffers at 0x238, 0x2C8, 0x358: STCYCL 3/1
(3 qwords a vertex), V4-32 header (GIF tag and count), then n <= 44 (usually 41-44) of S-8
unsigned (the pool index, qword 0), V4-5 (colour 5:5:5:1, qword 1), V2-16 masked (UV, qword 2);
then STMASK BFBF and 2-14 S-8 masked UNPACKs of 2-3 vectors that set the ADC (no-kick) bit at the
strip joins; STMASK 5050. The 12E0 batches of 0040 use V4-8 colours.

Output, written in place and kicked at 0FF8: an A+D tag (TEX0_1, TEX1_1, MIPTBP1_1, TEXFLUSH) when
the texture changes, then PACKED NREG 3 {ST, RGBAQ, XYZF2} x n with PRE: 03C (textured, Gouraud,
fog), 07C (+ blending), 00C (untextured, no fog: shadow silhouettes), 02C/06C (untextured with
fog, blended); the clipper adds triangle fans 03D/07D. Some runs kick an empty tag. No program
reads a flag left by another, and nothing outside VU1 reads its output (finding 090, OP3).

Overlay A: positions V4-32 at 8+, colours V4-8 unsigned at 0x108+, sizes and UVs V4-16 at
0x168/0x208, per-emitter matrices V4-32 at 0x148/0x20C; output sprites (PACKED RGBAQ, UV, XYZF2,
UV, XYZF2 with PRIM 156/176, or ST/RGBAQ/XYZF2 pairs for 2E48).

Volume: 11,000-19,000 UNPACKs and 190,000-340,000 vectors a field, 270,000-410,000 VIF1 words
(1.1-1.6 MB) a field; 1,700-2,600 VU1 runs a field.

## 3. The GS features per scene

From 27 dumps of 6 fields along the route (`gs_census.py`); a field's numbers.

| | title | menus | shop | flyover | race start | race | mid-race |
|---|---:|---:|---:|---:|---:|---:|---:|
| primitives | 41,200 | 33,700 | 16,000 | 8,500 | 35,500 | 30,900 | 24,700 |
| vertices | 70,600 | 64,300 | 37,500 | 15,600 | 56,100 | 54,300 | 44,400 |
| qwords PATH1 / PATH2 / PATH3 | 99.8 / 0.2 / 0.0 % | 99.8 / 0.2 / 0.0 % | 99 / 1 / 0 % | 99 / 0.6 / 0.1 % | 94 / 5.6 / 0.0 % | 95 / 5 / 0 % | 97 / 2.7 / 0.0 % |
| render targets (switches a field) | 2 (0.8) | 4 (4.2) | 6 (3.8) | 10 (3.8) | 10 (13.3) | 8 (10.0) | 8 (10.0) |
| blended primitives | 84 % | 77 % | 73 % | 58 % | 39 % | 47 % | 48 % |
| mipmapped (of textured) | 0 | 0 | 0 | 52 % | 28 % | 39 % | 36 % |
| alpha-tested | 0 | 0 | 5 % | 9 % | 13 % | 19 % | 14 % |
| drawn from a render target | 0 | 3,000 | 9 | 6 | 1,200 | 1,800 | 1,100 |
| T8H/T4HL in the Z buffer's top byte | 0 | 11 | 268 | 223 | 561 | 554 | 524 |
| CLUT loads | 285 | 194 | 153 | 136 | 467 | 497 | 380 |
| host-to-local uploads (pixels) | 0 | 0 | 1.7 (3,400) | 0 | 17 (35,000) | 15 (30,000) | 5.6 (11,400) |
| local-to-local copies, readbacks | | | | 0.3 | 1.7 copies, 0.8 Z reads | | |
| distinct textures in the window | 26 | 24 | 38 | 105 | 131 | 114 | 123 |

What the game uses, all scenes:

- **Primitives**: triangle strips 93-100 % (Gouraud, textured, fog), sprites up to 7 % (particles,
  HUD, full-screen passes, clears), triangle fans (the VU1 clipper), a few triangles. No points or
  lines. Perspective ST everywhere in 3D (FST only on sprites).
- **Paths**: PATH1 (VU1 XGKICK) 94-100 % of the qwords; PATH2 (VIF1 DIRECT) the per-object GS
  register setup and, in races, texture uploads as IMAGE; PATH3 (GIF DMA) only the setup, the display
  double buffer and loading screens (3-5 tags a field).
- **Textures**: T8 (52-86 %) and T4 (12-43 %) with CT32 palettes in CSM1, loaded with CLD 1
  (140-500 loads a field); CT32 for the shadow targets; T8H and T4HL read from the Z24 buffer's
  top byte; CT24/CT16 rare. MODULATE with alpha (TCC) on ~100 %; DECAL rare. Sizes 32-512, mostly
  64-256. Bilinear everywhere; two mip levels (MXL 2, LINEAR_MIP_NEAREST, LOD from Q) on up to half
  of the race and flyover draws. REPEAT 85-100 %, CLAMP the rest; no region modes. TEXA only with
  zero.
- **Blending**: (Cs-Cd)As+Cd (80-93 % of blended draws), (0-Cd)As+Cd (darkening: shadows, 10-17 %),
  (Cs-0)As+Cd (additive), (Cd-Cs)As+Cs, (Cd-0)As+Cd, and four FIX variants in the flyover and race
  start. No PABE, FBA, dithering, COLCLAMP off or frame masks.
- **Tests**: alpha test GREATER 0x40 (cut-outs), GEQUAL 1, GREATER 0/1/8, and NEVER with the
  RGB_ONLY fail (colour-only writes: no alpha, no Z); no destination-alpha test. Depth Z24 GEQUAL (with and
  without writes), GREATER without writes, and untested draws.
- **Fog** on 88-100 % of 3D draws, one FOGCOL per track (menus and title another).
- **Render targets**: the displayed frame (CT24, 640x448 in menus, 512x448 in races, double
  buffered at pages 0x46/0x8C or 0x38/0x70) with Z24 at page 0, and **two 64-pixel-wide CT32
  targets at pages 0x1FC/0x1FE** (with their own Z at page 4): the riders' shadows are drawn
  untextured into them (the 0x616 batches, PRIM 00C) and then drawn onto the ground as CT32
  textures with (0-Cd)As+Cd. CT16 sprites clear them. 13 target switches a field at the race
  start, 10 mid-race (finding 156 counted 12-17 render passes).
- **GS memory reads**: the lens flare's 4x4 Z32 readback (local to host, when the sun is up);
  64x64 CT24 local-to-local copies (1.7 a field at the race start); textures in the 24-bit
  buffers' spare byte; textures uploaded as CT32 blocks and read as T8/T4 (the native renderer
  already models GS memory).
- **Display**: read circuit 2 only, interlaced frame mode, CT24, no merge.

## 4. The translation plan

### Where it hooks

On the VU thread, at `run_micro` (`port/hle/vif.c`): the run's kind (overlay, MSCAL program, entry,
VI13) decides whether it is translated. A translated run does not execute the microprogram; it
reads VU1 data memory as the VIF left it (the pool, the batch records, the matrices and texture
triplets of the layout above) and hands the GS front end a **native mesh draw**: the GS register
writes the run's A+D tag would make (TEX0, TEX1, MIPTBP1, TEXFLUSH), PRIM, and a vertex slice
(pool position or index, colour, UV, ADC) with the per-object uniforms. The GS front end keeps
its register model and its order (other paths' draws, render-target switches, CLUTs, uploads),
and the native renderer draws the slice with a vertex shader that does the loop's maths. Later
the VIF unpack for translated batches can be skipped by reading the VIF stream directly (its
forms are fixed per program).

### Per program and feature

| program, loop | GPU form | notes |
|---|---|---|
| prelit loops 0170 (0078), 1708 (1638), 08B0 (07F0), 0798 (06F0), 17F0 (17E0) | indexed strips; vertex shader: pool position x (object x view-projection), perspective, fog from w, colour 5:5:5:1 x scale, UV x Q | backface cull (0078) as a cull mode; the CLIP/FCAND judgement and the clipper's fans become the GPU's clipping (guard band, near plane); ADC flags become strip restarts |
| lit loops 1110, 13A0, 1440, 0C30, 0AA0 (setups 0040, 1050, 12F0, 1430, 0C20, 0A90) | the same plus normal x light matrix, light colour, ambient | 0040's per-vertex lighting loop 1280 moves into the shader |
| skinning 0010/0020 (loop 0468, tail 0590), 0000 (05A0) | bone palette (856+, up to 27 matrices) in a uniform or storage buffer, 3-bone blend in the vertex shader | the riders: 25-32 % of title and shop VU1 time |
| shadow 3140 (0x616), 30A0 | orthographic projection into the 0x1FC/0x1FE targets | the render-to-texture path stays the native renderer's |
| 2DE8 (2DC8) | second texture coordinate set from the normal | after its purpose is confirmed |
| overlay A particles (2E48, 29C0, 0E10, 02D0, ...) | stay on the CPU (native kernels exist for 2E48, 0E10); later instancing | 29C0 is a sequential recurrence with random numbers and flag culling: a poor GPU fit |
| GS front end | unchanged for PATH2/PATH3, CLUTs, uploads, render targets, the flare probe | translated draws carry the GS state they would have set |

Stays emulated longest: VU0 and the scene walk (game code), overlay A, the GS memory model with
its uploads, CLUTs, 24-bit spare-byte textures and local copies, the exact renderer (the reference
for state traces and `DHD_EXACT_PROBE`), and every program not yet translated (a translated and an
untranslated run can follow each other in one view).

### Precision and the Z probe

The shaders work in IEEE floats; positions differ from the VU's toward-zero, clamped results by an
ulp or so (finding 090, OP3). The native renderer is not game state; the one GS result the game
reads is the flare's Z probe, which in native mode reads the native renderer's own depth (finding
097), so translated draws must write that depth. Exact mode (`DHD_RENDER=exact`) and the state
traces (`DHD_EXACT_PROBE=1`) keep the CPU VU1.

### Order (biggest gain first)

1. Loop 0170 (0078): 37-49 % of VU1 time and 34-48 % of the vertices in every scene.
2. The other prelit loops on the same record layout (1638, 07F0, 06F0, 17E0): with 1, 60-66 % of
   race VU1 time.
3. The lit loops and setups (0040, 1050, 12F0, 1430, 0C20, 0A90): race start +15-20 %.
4. Skinning (0010/0020, 0000): the title and the shop +25-32 %.
5. Shadows (0x616, 30A0) and 2DC8.
6. Overlay A last, or never.

### Checks for each step

- **Maths**: a replay of captured VU1 runs (`DHD_VU_SEQ`, as `vu1_seq_diff.py`): the translator's C
  reference of the shader maths against the exact VU1 output, per vertex (positions within a
  sixteenth of a pixel, colours and UVs exact, the same strips after restarts).
- **Pictures**: deterministic route runs (`DHD_FRAME_CLOCK=1`) with the translator on and off at the
  same fields (`DHD_CAPTURE_PRESENTED`), and against PCSX2 (`compare_pcsx2.py`, `race_compare.py`):
  differences only where clipping and float precision may differ.
- **Game state**: the race and boot state traces with `DHD_EXACT_PROBE=1` must stay identical in
  every non-GS column (the GS stream columns change by design in native mode), and identical in
  all columns with the translator off.
- **Speed**: `route_profile.py` timing runs (the VU and GS threads' work and busy time, fields/s)
  on the RTX PC, the integrated Radeon and the Mi 11i on finding 127's low-end profile.

## Phase 1 proposal

**Translate loop 0170 (MSCAL 0078: the 1AE0 setup and its 1CB0/1040 batches) in native mode only,
behind a switch, off by default; everything else as today.**

Scope:
- On the VU thread: for 0078 runs, read the object matrix, the view constants, the colour and fog
  parameters, the texture triplets, the pool (once per object) and each batch's records; emit one
  native mesh draw per batch (GS register writes + vertex slice) into the GS queue in the run's
  place; skip the microprogram and the XGKICK.
- In the GS front end and the native renderer: a draw kind for these slices that bypasses
  `gs_write`/vertex kicks for their vertices but takes the current GS context (ALPHA, TEST, FRAME,
  ZBUF, FOG, CLAMP and the batch's TEX registers); one WGSL vertex shader (transform, perspective,
  fog, colour, UV) into the existing fragment path; GPU clipping and back-face culling.
- Not in scope: other programs, VIF unpack skipping, the exact renderer, phones' GPU cost.

Measurement: the maths replay on three `DHD_VU_SEQ` captures (title, race start, mid-race);
picture comparison at fixed fields against the switch off and PCSX2; state traces as above; timing
with `route_profile.py` on the RTX PC (4x), the integrated Radeon and the Mi 11i at 2x on the
low-end profile, against this branch's base.

Expected gain (from the shares above; the translator's own cost not yet known):
- **VU thread**: 0078 is 37-49 % of VU1 code time (1.0-1.3 ms of 2.6-3.3 ms in races on Windows)
  and its batches carry most of the V2-16/S-8/V4-5 unpacks (about 0.3 ms of 0.6-0.8 ms): about
  -1.5 ms at the race start (4.4 -> 2.9 ms) and -1.2 ms mid-race (3.5 -> 2.3).
- **GS thread**: 34-48 % of the PATH1 vertices no longer decoded and recorded vertex by vertex:
  about -0.5 to -0.7 ms of its 1.9-2.5 ms.
- **Frames**: on the RTX PC the VU thread stops being the limit: the race start goes to the game
  thread's 3.6 ms (214 -> about 270 fps, +25 %), mid-race to 3.0 ms (272 -> about 330, +20 %),
  the title (VU 5.3 ms) to about 3.5 ms (187 -> about 270). On the Mi 11i's low-end profile at 2x
  the VU (14.3 ms) and GS (14.9 ms) threads drop to about 9.5 and 10.5 ms at the race start, under
  the game and render threads (12.4 and 12.3 ms): about 75-80 fps instead of 60. GPU-bound cases
  (the M1, integrated GPUs, phones at 4x) gain nothing from phase 1 by itself.

## Decisions

- **Whole stacks, not callers.** `sample_profile.py` had a heuristic caller scan for named
  functions; inclusive times per system need real unwinding. `--stacks THREAD` walks the stack
  with dbghelp's StackWalk64 while the thread is suspended (about 2 ms a sample, so the game thread
  is sampled about 370 times a second and runs slower: shares only).
- **ms from finding 156's clean timing runs.** This lane's timing run ran on a PC loaded by other
  lanes (74-96 % CPU before the runs: builds and a PCSX2 run), 203 fields/s against 254; its
  thread times are 10-20 % higher. Shares come from this lane's samples, ms from 156.
- **The packet path as a system of its own**, split into the scene walk and the DMA kick, because
  the packet-level plan keeps both and a deeper hook removes them.
- **A layout census in the game, not offline.** `DHD_VU_SEQ` captures hold VU memory, not the VIF
  commands; logging the commands and kicks per kind of run in `vif.c` (opt-in, read only) gives the
  layout directly. It is host code: the core id does not change.
- **GS census from PCSX2-format dumps**, so the same tool reads PCSX2's own dumps of the retail
  game.
- **The phone with DWARF call graphs** (the build omits frame pointers) and finding 156's APK,
  so no new phone build was needed; the shipped 0.7.11 APK was put back (byte-identical).

## Surprises

- **The scene walk is a third to two thirds of the game thread**, more than physics and AI
  together in every scene; in the menus the game thread's work is mostly packets.
- **The DMA kick costs as much as half the walk in the menus** (18.6 % against 42.3 % in the
  title): the HLE copies each chain into the graphics queue.
- **Textures in the Z buffer's spare byte**: the game keeps T8H/T4HL textures (220-560 draws a field)
  in the top byte of its Z24 buffer, which Z writes leave alone.
- **The shadows are render-to-texture** into two 64-pixel-wide CT32 targets, drawn with the
  untextured 0x616 batches of the skinned and lit programs.
- **Overlay B's mesh vertices are indices**: each strip vertex names a position in a per-object pool
  (a byte), so the pool is a vertex buffer and the strips are index buffers.
- **`FlushCache` costs 2-9 % of the game thread**: the kernel HLE polls timers and DMA at each call.
- **On the phone the vsync path itself is 14-15 % of the game thread's work in the menus** (against
  4-7 % on Windows).

## Caveats

- **The PC was busy** with other lanes' builds and a PCSX2 run during every Windows run here
  (CPU 74-96 % before each); thread times and fps from these runs are inflated, so ms use finding
  156's clean numbers. Shares can shift under contention too (a busy core slows memory-bound code
  such as the walk more).
- **Sampling**: the stack sampler slows the game thread; title and shop have few work samples (366
  and 370 over two runs: +-3 points on the large systems). The mid-race windows are late in the
  route (fields 12111-13312 on Windows).
- **Phone stacks are truncated** (32 KB DWARF copies: 2-4 % reach the thread's root); systems are
  recognised inside the copied frames, and 1.5-6 % stay "other". The phone's mid-race window was
  aimed at field 9300 from the heartbeats.
- **Purposes are inferred** from the microcode, the packet layout and the GS state; 2DC8's
  (environment map) is the least certain.
- **The GS census** counts primitives at the GS register level (before the native renderer merges
  draws), from 6-field dumps every 500 fields at 1x; scenes between dumps are not covered.
- **The phase 1 estimates** assume the translator costs little next to what it removes and that
  0078's share of the VIF and GS work follows its share of the vertices.

## Result

Phase 0 done on Windows (RTX 3070, 4x) and the Mi 11i (2x): the game thread's systems with named
functions per scene, the deeper hook's bound (0.4-1.5 ms a field on the PC, 0.8-2.5 ms on the
phone), every VU1 program with its purpose, share, packet layout and output, the GS feature list
per scene, the translation plan and a phase 1 proposal (loop 0170 first). Tools kept on this
branch: `sample_profile.py --stacks`, `route_profile.py`, `gs_census.py`, `DHD_VU1_LAYOUT`. No game
state change (state traces identical with the census code and with the census on).
