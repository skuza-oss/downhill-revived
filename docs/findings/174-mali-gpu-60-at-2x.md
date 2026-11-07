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

- [x] 2026-11-06: finding reserved
- [x] 2026-11-07: the Redmi 9's GPU measured: Mali hardware counters through Arm Streamline's
  headless capture (no root), the passes from `DHD_PASS_TRACE` (extended: sizes, areas, reads),
  the draw mix from `DHD_GPU_STATS` on the PC (below)
- [x] 2026-11-07: tile-bandwidth hypothesis priced and refuted: a load and store round trip of
  the 1024 x 512 frame buffer and Z costs about 0.07 ms on the Mali-G52
- [x] 2026-11-07: a new constants buffer every picture (to drop a write-after-read barrier):
  no gain (48.19 -> 48.40 fields/s, inside the spread), not kept
- [x] 2026-11-07: `70e5e27` DHD_PASS_TRACE gives each pass's size, area, bounds, draws and reads
- [x] 2026-11-07: `e30cd85` wgpu-hal (vendored): barriers per stage pair, textures without the
  vertex stage: 48.26 -> 49.53 fields/s over the route (race start 34.2 -> 36.4)
- [x] 2026-11-07: `44125b2` native_spirv writes the race's commonest specialisations at texel
  scale 1 (malioc sees them)
- [x] 2026-11-07: the owner's question: the optional effects on and off (below): all off,
  49.5 -> 60.4 fields/s over the route; the riders' shadows and the sun glow carry most of it
- [x] 2026-11-07: `e30cd85` checked: pictures byte-identical to the base on the Redmi 9 (5 shots,
  2x) and the PC (5 shots, 4x, Vulkan), boot and race state traces match, `check.py port` OK
- [ ] the Mi 11i (no regression on Adreno): not run, its 0.7.12 is release-signed and the lane
  has no release key, so a debug-signed build cannot go over it

## Method

- **The phone**: Redmi 9 (Helio G80, Mali-G52 MC2 at 950 MHz, driver r26p0, Android 11), not
  rooted. One debuggable APK per build (`build_host64.py --release --debug --pgo-use` with the
  shipped builds' profile, then `build_apk.py`: a release's native code, debug-signed so `run-as`
  works). The runner (scratch, from a local measurement branch's) writes the run's `DHD_*` keys into the app's
  dhd.ini through `run-as` and puts the original back (md5 checked), closes third-party apps, cools
  90 s with the screen on and the app closed, samples the CPU clocks and the battery temperature
  (the GPU clock files need root), pulls dhd.log and the `DHD_TIMELINE` CSV. Finding 156's route
  (`race_compare`'s NAV and RACE_PAD, `DHD_RACE_SEED=81257`), `DHD_FPS=240 DHD_VSYNC=0
  DHD_FRAME_CLOCK=1 DHD_SCALE=2 DHD_RENDER=native DHD_WIDESCREEN=0 DHD_ASPECT=4:3
  DHD_SKIP_MOVIES=1 DHD_MEMCARD=0 DHD_SND_OFF=1 DHD_BENCH=300-14000 DHD_BENCH_EXIT=1`. The touch
  controls are drawn (no pad), as players without a pad see them.
- **GPU time**: the Mali-G52's Vulkan driver has no timestamp queries (`TIMESTAMP_QUERY=no`), so
  `DHD_GPU_TIMES` gives nothing there. The GPU is measured by its hardware counters instead: Arm
  Performance Studio's `streamline_me.py --headless` runs gatord as the debuggable app, with a
  `configuration.xml` of 75 Mali-G52 counters; `Streamline-cli -report -timeline` exports them per
  0.1 s and per 1 ms, placed on the game's fields by the capture's end (the app's exit after field
  14000). Per frame = the window's sums / its fields (one picture a field).
- **Passes**: `DHD_PASS_TRACE=1` (one picture in 240), extended in this lane with each pass's
  attachment size, the union of its draws' scissors and of their vertex bounds, its draws, its
  draws with Z, and the surfaces they read. Untimed PC runs on the hidden desktop (the GS stream
  is the same on every platform).

## The GPU at 2x, before (base cfe842a)

Counters per frame (`Streamline` run of the base, 47.5 fields/s over the route; 48.2 without it):

| scene | fps | GPU active Mcycles | fragment queue | non-fragment | tiles | ext. read MB | ext. write MB | tile write MB | ALU instr. M | core active M |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| title | 50.0 | 10.97 | 8.30 | 2.67 | 12,536 | 26.8 | 19.0 | 7.6 | 15.2 | 21.0 |
| shop | 55.2 | 14.68 | 12.11 | 2.62 | 14,490 | 18.1 | 16.6 | 8.8 | 21.7 | 27.9 |
| race start | 33.8 | 24.92 | 20.38 | 4.71 | 20,513 | 36.7 | 19.0 | 8.6 | 34.6 | 47.1 |
| mid-race | 47.5 | 17.77 | 14.24 | 3.61 | 16,240 | 36.7 | 17.8 | 9.0 | 24.1 | 33.6 |

(Core active and instructions are summed over the two shader cores.) Mid-race: the GPU is active
844 M cycles a second (950 MHz, busy throughout); external traffic 1.74 GB/s read and 0.84 written,
read stalls 2.4 % and write stalls 4.8 % of the active cycles: **memory bandwidth is not the limit**.
The fragment queue (14.24 M) and the non-fragment queue (3.61 M) add up to the active cycles
(17.77 M): binning, vertex and compute work never overlaps fragment work.

A frame mid-race in 1 ms bins (21 ms):

| phase | ms | what |
|---|---:|---|
| the scene's binning | 1-1.5 | vertex shading and tiling of the first pass (25,000 triangles); the fragment side idle |
| the scene's fragment | 9-10 | about 1,800 tiles at 200 a ms; ALU utilisation (instructions / core-active cycles) about 90 % |
| glow chain, the other two frame buffer passes, conversions, the scanout | 4 | the scanout compute about 1.5 ms (60,000 warps, its stores) |
| the present | 2.5-3 | 10,000 tiles of the 2340 x 1080 frame (the picture 1440 x 1080, the bars cleared), the touch controls; ALU 75-80 % |

**Passes a race picture** (PC trace, 2x): mid-race 12, the race start 18. The frame buffer (surface 5)
and its Z (6) are 1024 x 512 (the draws cover 1024 x 448), opened three times mid-race (the scene,
424 draws; 4 draws; the HUD, 15 draws) and five times at the race start; the other passes draw a glow
chain into 64 x 64 and 64 x 128 surfaces (8-11), which never read the frame buffer.

**The draw mix** (`DHD_GPU_STATS=60` on the PC, Vulkan, 2x, mid-race; Mpixels shaded a picture after
early Z): 4.08 in all; textured bilinear fogged blended on As with Z test and no Z write 2.16 (53 %);
the frame's clear sprite 0.46; opaque textured bilinear fogged with Z write 0.44; the rest below
0.32 each. On the Mali-G52 the blended kind is `native_pixel_blend_u` (the driver has no dual-source
blending; that kind writes no alpha, so it is one draw). malioc for Mali-G52 (Arm Performance Studio
2025.4): 2.58 arithmetic cycles a pixel on its gather path, texture 0.50, varying 1.12, 32 work
registers: arithmetic-bound, as the counters say. `native_vertex` is load/store-bound (11 cycles a
vertex: its three outputs written to memory), `native_scanout_fast` load/store-bound (4 cycles a
thread), `present_pixel` 1 load/store cycle a pixel.

## The optional effects on and off

A diagnostic build only (`DHD_DIAG_FX`, not committed) skips chosen draws: `small` the passes into
the small surfaces (the riders' shadows and the sun glow chain), `fog` the fog term, `blend:885`
the lens flare's ghosts. On the Redmi 9 at 2x with `e30cd85`, two interleaved runs each, all three
off (`small,fog,blend:885`):

| | on 1 | on 2 | off 1 | off 2 |
|---|---:|---:|---:|---:|
| fields/s over the route | 49.59 | 49.35 | 60.95 | 59.82 |
| p99 field ms | 39.0 | 37.4 | 24.6 | 25.5 |
| render thread busy ms a field | 12.52 | 12.69 | 8.55 | 8.77 |
| race, 30 s windows (fps) | 47-52, start 37.7 | 47-50, start 38.5 | 58-66 | 58-65 |

About +11 fields/s (+22 %); the race start, the worst place, goes from 38 to 59-60. (The off runs
cover the same fields in less time, so they have five race windows, not six.)

Split per effect on the PC (`DHD_GPU_STATS=60`, Vulkan, 2x, six race windows; Mpixels shaded a
picture after early Z, and passes a picture):

| | Mpix a picture | passes |
|---|---|---:|
| all on | 4.61 5.61 3.83 3.63 4.10 4.08 | 12-18 |
| shadows and glow off (`small`) | 3.51 3.30 2.93 3.00 3.25 3.17 (-17 to -41 %) | 1 |
| lens flare off (`blend:885`) | 4.11 5.19 3.76 3.63 4.10 4.08 (only while the sun is in view) | 12-18 |
| fog off | unchanged | 12-18 |

Fog changes only the arithmetic a pixel: malioc for Mali-G52 gives the commonest scene shader
4.25 cycles on its longest path with fog and 4.03 without (2.58 and 2.47 on its shortest), about
4-5 % of the scene's fragment work. So the riders' shadows and the sun glow carry most of the gain,
the lens flare a little at the race start, fog little. The split is from the PC and malioc; the
phone measured only all on and all off.

## Decisions

- **Counters, not timestamps, on the Mali**: its driver offers no timestamp queries; Streamline's
  headless capture needs no root with a debuggable APK and gives every Mali-G52 counter.
- **One question per round**: each candidate is its own APK, compared against the base in
  interleaved runs; a change that does not move the Redmi 9 is not kept.

## Surprises

- **Load and store round trips are cheap on the Mali-G52.** A temporary build that ended the scene
  pass every 100 draws (the same pictures; 4 more frame buffer passes a race picture, 12 -> 16)
  cost 0.24 M GPU cycles a frame mid-race (17.77 -> 18.01; fps 47.5 -> 46.6): about 0.07 ms for
  each load and store of the 1024 x 512 colour and Z. Mali also processes only the tiles a pass
  draws into (1,230 more tiles for each extra pass, not 2,048). Merging the frame buffer's passes
  would save under 1 %. A scheduler that reorders independent commands so that they merge (all three
  mid-race passes into one) was written and unit-tested but is not measured or kept for that reason.
- **The scene pass is ALU-bound**: 90 % arithmetic utilisation while it shades, texture filtering
  about 12 %, varyings about 14 %.
- **A new uniform buffer each picture changed nothing** (base 48.05 / 48.33, candidate 48.50 /
  48.29 fields/s, interleaved): the arena's write-after-read barrier was not what held the scene's
  binning (on Mali a buffer copy runs as compute, which cannot overlap binning anyway).
- **No dual-source blending on the Mali-G52 r26**: draws blending on As take the RGB pass, plus an
  alpha-only second draw when they write alpha.

## Result
