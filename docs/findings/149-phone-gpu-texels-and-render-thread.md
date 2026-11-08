# 149: The throttled phone at 4x: one gather per bilinear texel footprint; the render thread's framebuffer a frame

Status: current
Checked: 2026-11-04

2026-11-04, from local main `f0f5c2e` (finding 148's head). Target: 60 fps throughout races on the
rooted Mi 11i throttled to finding 127's low-end profile (`phone_throttle.py --apply t612 --big-max
1209600 --little-max 1497600`), at render scale 2x and 4x. Every speed run unlocked (`DHD_FPS=240`,
`DHD_VSYNC=0`, `DHD_FRAME_CLOCK=1`), release-signed non-PGO APKs with line tables unless noted.

## Summary

Six speed changes and a test fix, all keeping the picture and the game the same on the phone. Base
(`f0f5c2e`), change 6's code with PGO and the head (`4435f9a`) in back-to-back sessions, unlocked,
throttled, non-PGO unless noted; the second race perf line is the race start (perf lines are 30 s
each, so a faster build's lines cover more fields):

| scale | build | successive race perf fps | window 9000-14000 fields/s |
|---|---|---|---:|
| 2x | base | 66.9, **55.2**, 59.9, 66.6 | 66.07 |
| 2x | head | 62.5, **59.5**, 62.6, 71.8 | 70.12 |
| 2x | head + PGO | 63.4, **61.6**, 68.1, 79.9 | 76.52 |
| 4x | base | 54.2, **43.2**, 51.5, 54.6, 56.4 | 55.43 |
| 4x | head | 55.1, **45.9**, 57.6, 63.2, 63.1 | 62.12 |
| 4x | head + PGO | 55.8, **46.3**, 58.0, 63.8, 63.5 | 62.65 |

(The PGO rows are change 6's code, `b7c2bed`, built with a fresh profile of one instrumented 2x
race to field 7000; change 7 only moves the GS thread's CPU. Change 6's code without PGO measured
61.9-62.5, 58.2-59.5, 62.5-62.7, 72.4-72.6 at 2x and 54.6-54.8, 46.0-46.4, 57.4-57.6, 63.0-63.1,
63.3-63.5 at 4x in two sessions.) Finding 148's own measurement of the base: 2x 67.0, 55.8, 60.1,
66.4 (65.81); 4x 54.9, 42.9, 51.6, 54.5, 55.5 (55.16).

60 is held throughout at 2x only with PGO (every race interval 61.6 or more); without PGO the race
start is 58.2-59.5. At 4x the race start is still GPU-bound (about 25 ms of GPU a frame in its
slowest stretch) and the first race interval GS-bound (54.6-55.8); from the third interval on 4x
is at 57.6-63.8, limited by the VU1 thread as 2x is. Where the time went: the 4x GPU's bilinear
texel fetches (changes 2-3, GPU -2.1 ms a frame), the render thread's framebuffer a frame (change 6,
-3.1 ms CPU a field), two threads on the profile's two big cores (change 4) and the GS (5, 7).
One interval got slower: 2x's first race line (the countdown and GO; 66.9 -> 62.5), which the GS
thread now limits on the little cores (change 4); it stays above 60.

## Method

- The runner of findings 147 and 148 (scratch, not committed): writes the run's `DHD_*` keys into
  the app's dhd.ini by root and puts the original back (md5 checked), closes third-party apps, cools
  45-60 s, samples clocks, optionally records simpleperf, pulls dhd.log and named files.
- Per-change A/B: the race-start window (fields 6400-7300; GO is at 6321), alternated builds, two
  runs each (spread of one build about +-1.5 %). GPU changes at 4x with `DHD_GPU_TIMES=240` (GPU ms a
  frame for each 240-frame block); CPU changes at 2x with each thread's CPU per field (`bench
  threads`).
- Before/after: window 9000-14000 and every 30 s race perf line, base and head in one session.
- Diagnostics: simpleperf (1 kHz flat; 300 Hz DWARF call graphs), `DHD_TIMELINE`,
  `DHD_VU1_PROFILE`, `DHD_GPU_STATS`, temporary counters and shader variants (below; none committed).

## Where the 4x race start's GPU time went

`DHD_GPU_TIMES`, base, 4x, race start: draw passes 16.5-24.8 ms a frame in its 240-frame blocks
(27.0 ms of GPU in the slowest), scanout 1.09-1.36, present 0.42-0.88. `DHD_GPU_STATS` in the
slowest stretch: 26.3 Mpixels shaded a frame (overdraw 7), 10.4 of them one draw a frame of
alpha-tested, fogged, bilinear sprites (the start's spray), 7.3 the blended terrain layer.

A temporary build edited native.wgsl's text at run time (`DHD_DIAG_FS`, not committed; pictures
wrong on purpose) to price the parts, the slowest stretch's GPU ms a frame (base 27.0):

| variant | slowest block | race-start window, fields/s (base 40.4) |
|---|---:|---:|
| no texture fetch, alpha test removed (`flat`) | 19.2 | 45.0 |
| point sampling for decoded textures | 23.0 | 46.1 |
| point sampling for float textures (`fetch`) | 21.4 | 43.7 |
| the gather wherever its outer test holds (`gatherall`, wrong at edges) | 25.8 | 41.8 |
| never the gather (four loads) | 30.2 | 36.4 |
| blending off | 26.0 | 41.4 |
| every depth test Always | 36.9 | 32.5 |

So the bilinear texel fetch is a large part, the float-texture path (the race start's converted
textures) most of all; blending (colour reads) is not; early depth rejection is essential. Timestamps
do not change the speed (`DHD_PERF_LOG=0` 42.61 against 42.67 with them).

## Changes

1. **`69acc85` Renderer tests: device_smoke passes the pipeline key as u64.** `rustgs.py test` no
   longer compiled since finding 131's 64-bit keys.
2. **`c28a6da` Native renderer: every repeat/clamp bilinear in one gather, conversions as texels.**
   The gather was taken only where the four texels were neighbours; at a texture edge (repeat
   wrapping, clamp holding the last texel) four bounds-tested loads ran, and a wave with one such
   pixel ran both paths. The texels sampler now repeats, so a gather at corner c reads texels c - 1
   and c mod the size; clamp takes c = b + 1 inside and the edge texel twice outside (component
   selects). Converted textures (native_convert) become R32Uint texels like decoded ones and draw by
   the `_u` entry points. `sized` also requires power-of-two sizes. 4x race start 40.33-40.44 ->
   42.24-42.41 fields/s; GPU -1.3 ms a frame in every block.
3. **`a768ad2` Native renderer: the alpha test before the colour, and a texel's alpha as its
   byte.** `combine()` computes the alpha, tests and discards, then the colour (same expressions);
   a spec bit (20) says the draw's alpha_scale is 255 (every textured draw), so the texel's alpha
   needs no run-time scaling. 42.41 -> 43.12 fields/s; GPU -0.8 ms a frame.
4. **`56e3d27` Android: with one or two big cores, the GS and render threads stay on the others.**
   The four busy threads shared the profile's two A78s; game and VU ran a fifth of their time on an
   A55. Without `DHD_AFFINITY`, a phone with one or two cores faster than its slowest cluster
   (`cpu_capacity`) keeps `gs` and `native` on the others (re-read every 60 fields; nothing placed
   with more big cores; `DHD_AFFINITY=0` places nothing). 2x race start mean 51.2 -> 53.5 (+4.5 %;
   alternated in one session 51.29/52.21 -> 52.86/53.22); 4x unchanged (GPU-bound there).
5. **`b9767d9` GS: a strip's conversion cache set once, after its last triangle.** 144 bytes stored
   per triangle that only the last triangle's survive; set once after the loop (native_set again).
   GS thread 15.95/15.90 -> 14.99/14.99 ms a field; fields/s within the spread (the render thread
   limited next).
6. **`b7c2bed` wgpu-hal (vendored): a swapchain image's view kept across frames.** A DWARF call
   graph put 16 % of the render thread under `kgsl_ioctl_gpuobj_alloc`, 480 of its 517 samples from
   `vkCreateFramebuffer`: every acquire made a new swapchain view, so the present pass's framebuffer
   missed the cache every frame (a temporary counter: about one miss a picture, 2400 x 1080), and
   the Adreno driver allocates GPU memory for each framebuffer, the kernel walking the process's
   allocations. `create_texture_view` now returns the kept view of a swapchain image (and its
   identity); the swapchain's release destroys them (PATCH.md). 2x race start 53.23/53.84 ->
   56.93/57.31 fields/s (+7 %); render thread 16.96/16.51 -> 13.60/13.57 ms a field.
7. **`4435f9a` GS: a PACKED strip's vertex and registers in locals for the loop.** packed_strip
   stored each RGBAQ/ST/UV qword into g->reg, g->vertex and the path's Q and then copied g->vertex
   into its list; the vertex, Q and those registers now live in locals and are stored once after
   the loop. GS thread 14.94/15.05 -> 14.63/14.57 ms a field (-2.7 %); fields/s within the spread
   (57.39/56.44 -> 56.75/57.32). Kept because the GS limits the first race interval now.

## Checks

- **Pictures on the phone's GPU.** A scratch harness (not committed) builds `port/gs/replay.c` as an
  Android executable (POSIX shims for its Win32 timer and the test stubs, and the few SDL thread
  and lock calls on pthreads: SDL's own need a JVM) over the APK's `libdhd_render.a`, and replays
  GS captures headless on the Adreno through Vulkan. Three captures from the phone (`DHD_GS_CAPTURE`,
  12 fields each at the race start, middle and later) replayed by the base's library and by the
  library with changes 2 and 3 (the shader changes): every field's picture and the VRAM identical,
  at 2x and 4x, with specialised pipelines (`DHD_NATIVE_SPEC=sync`) and generic ones (`=0`);
  specialised equal to generic. (Changes 5 and 7 change what the GS records, checked by the frame
  hash below; change 6 only the swapchain's views.)
- **On the PC** (`replay_gs.py`'s replay.exe, the same three captures, 4x, the RTX): Vulkan and DX12/FXC
  identical to the base; DX12/DXC two pixels by one step in one of the 36 fields, DXC's
  reassociation by code shape (finding 131's known behaviour; Vulkan, which the phones use, exact).
- **Recorded native frames** (the frame hash of finding 148, temporary): a deterministic Windows race
  with the exact probe to field 9000, all 8,999 frames identical to the base's, after change 5 and
  again after change 7.
- **Windows state trace** (`state_trace_run.py` against the base's traces): boot 600 and race 5000
  rows match after change 6 and again after change 7.
- **Phone state trace** (finding 146's recipe: headless, exact probe, 2x, 4:3, 5000 race fields),
  the head's APK against finding 148's head (this lane's base) on the same phone: `trace_columns.py`
  PASS, all 13 columns identical.
- `rustgs.py test`: all suites pass (after change 1).

## Tried and not kept

| what | result |
|---|---|
| scanout by a render pass (full-screen triangle, DontCare load) instead of the compute pass | scanout 1.09 -> 1.45 ms at 4x: worse; the copy is bandwidth |
| presenting straight from the display surface (no scanout) | not exact: the race's display doubles rows (a 2048 x 1024 surface shown as 2048 x 1792), which a filtered present cannot reproduce; not done |
| the texture's colour unpacked and filtered only after the alpha test (alpha filtered first) | GPU -0.1 to -0.26 ms (1 %), and FXC moved two pixels by one step: not kept |
| only the render thread on the little cores (GS free) | 2x window 68.69 vs 70.31, race start 58.6 vs 59.5, first race line 66.1 vs 62.5; 4x window 62.14 vs 62.29, first line 58.6 vs 54.8 (one full run each) |
| the mesh loop's sticky-flag accumulation stopped once set | a counter: every call already enters with both sticky flags set (the loop skips them already) |
| GMEM vs system-memory rendering | not observable (no perfetto GPU producer on this firmware); timestamps make no difference |

## What limits it now, and what is left

- **2x:** the race start (58.2-59.5 without PGO) is limited by the VU1 thread (busy 16.5 of a 17 ms
  field), with the GS (on the little cores, 15.7 busy) close behind; the first race interval by the
  GS thread. PGO lifts every interval above 60 (61.6 at the race start).
- **4x race start:** the GPU, ~25 ms a frame in its slowest stretch, most of it the per-pixel work
  of one draw of alpha-tested bilinear sprites (10 Mpixels a frame) and the blended terrain. The
  diagnostics price the bilinear fetch and the shading ALU at several ms; what keeps the picture
  exact is used. Scanout (~1.1 ms) and present (~0.9 ms) are bandwidth.
- **The VU1 thread** limits most of the race at both scales (`limit=cpu:vu`): programs 13.6 ms a
  field at the 2x race start (`DHD_VU1_PROFILE`), the mesh loop at 1040 about 56 % of them (516
  calls a field, ~40 vertices each, ~145 cycles a vertex on the A78 at 1.21 GHz), VIF unpack and
  feed ~14 % of the thread, GIF packet copies 3 %.
- **The GS thread** (on the little cores now): `gs_feed` 33 %, `native_draw_strip` 26 %.
- **The render thread** (little cores): `Queue::write_buffer`'s copies 9 % (vertices and indices,
  ~2 MB a picture), wgpu-core recording and tracking, the Adreno driver.
- **PGO** (a fresh profile of one instrumented 2x race to field 7000): +8 % on the 2x window (70.3-70.8
  -> 76.5), +3.5-6 % at the race start, little at 4x (GPU and GS). As in findings 146 and 148, a
  release profile should cover more tracks and the menus.

## Phone state at the end

`phone_throttle.py --restore` printed RESTORED (every saved value read back); dhd.ini is
byte-identical to the saved original (md5 9af4f977637895d329ec56b472b53514); the replay files and
captures in /data/local/tmp were removed, no run files are left in the app's folder; `/sdcard/PS2`
untouched, no uninstall. Installed: the head's release-signed build (non-PGO, line tables; code of
`4435f9a`), SHA-256 a4d382c1a0acdf53701e6c78689c414416aee48cfff2d49accb21b97db9a3869. The phone
lock was removed.

## Sources

- Findings 111, 127, 129, 131, 146, 147 and 148; the native renderer, vendored wgpu-hal 29.0.3
  (`command.rs` framebuffer cache, `device.rs` views, `swapchain/native.rs`), GS and VIF sources.
- Qualcomm's Adreno game developer guide (FlexRender: the driver picks binning or direct rendering
  per pass) and Mesa Turnip's `tu_pass.c` for how tilers choose GMEM or system memory; the
  [Vulkan tile-based rendering best practices](https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html).
  No external code copied.

Raw logs, profiles, captures, APKs, the replay harness and the temporary patches (shader variants,
counters, frame hash, rejected changes) are in the ignored scratch folder. No game data, keys or
machine paths are tracked.
