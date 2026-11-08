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
- [x] 2026-11-07: the widest set off (shadows, sun glow and flare, fog, point sampling, overlay A's
  particles, the frame at 896 lines): 49.9 -> 62.8 fields/s over the route (below)
- [x] 2026-11-07: `e30cd85` checked: pictures byte-identical to the base on the Redmi 9 (5 shots,
  2x) and the PC (5 shots, 4x, Vulkan), boot and race state traces match, `check.py port` OK
- [ ] the Mi 11i (no regression on Adreno): not run, its 0.7.12 is release-signed and the lane
  has no release key, so a debug-signed build cannot go over it
- [x] 2026-11-08: the Mi 11i (throttled, 4x) as the Redmi 9's stand-in: the widest set off gives
  the same gain over the race (+40 % against +40 %), not the same fps or the same shape per scene
  ("Mi 11i stand-in" below)

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

**The widest set** (the owner's question of 2026-11-07: everything a Performance group could
offer, all on against all off). The same diagnostic build plus three more switches (not committed;
`point`, `DHD_DIAG_SKIP_A`, `DHD_DIAG_OUTPUT`):

- the three above (`small`, `fog`, `blend:885`);
- `point`: each draw's bilinear bit cleared (point-sampled textures; the scene shader is
  arithmetic-bound and bilinear is its gather path);
- `DHD_DIAG_SKIP_A`: overlay A's programs not run (their VU1 work and their draws) except 0x26D8.
  Skipped one at a time on the PC over the route: 0x26D8 draws the sun and its flare (kept: skipping
  it removes the sun), 0x0E10 and 0x29C0 the riders' ground shadows, 0x02D0 and 0x2E48 a few
  pixels (dust); the others change neither checked picture (fields 6800 and 9500; likely particles
  of other places, tracks and weather);
- `DHD_DIAG_OUTPUT=896`: the window's frame drawn 896 lines high (2x's picture height, the window's
  aspect), the display scaling it to 2340 x 1080 (not confirmed on the phone: the log does not
  give the swapchain's size).

What it looks like (PC, 2x): the sun is gone with `small` (its glow chain draws it), the haze too;
point sampling makes the sky's large textures visibly blocky.

| | on 1 | on 2 | off 1 | off 2 |
|---|---:|---:|---:|---:|
| fields/s over the route | 50.12 | 49.72 | 62.80 | 62.76 |
| p99 field ms | 37.3 | 38.3 | 22.9 | 22.8 |
| race, 30 s windows (fps) | 47-52, start 38.4 | 47-51, start 37.6 | 60-66 | 60-66 |
| busy ms a field: render / VU / GS | 12.6 / 15.2 / 18.9 | 12.9 / 15.4 / 19.1 | 8.2 / 13.7 / 14.6 | 8.1 / 13.7 / 14.7 |

About +12.9 fields/s (+26 %), and the race start goes from 38 to 60: about 2.4 fields/s more than
the three effects alone (60.4). With all off the limit moves between the GPU and the VU thread.

## Mi 11i stand-in

The Redmi 9 is no longer available. The question: can the rooted Mi 11i (Snapdragon 888, Adreno
660), throttled and at 4x, stand in for the Redmi 9 at 2x? The test was the widest set above, all
on against all off, with the same APK (`fx5`, sha256 `ba662e08…`), interleaved on, off, on, off.

- **The phone**: `phone_throttle.py --apply t612` with the profile's own clocks (cpu6-7 offline, A78
  at 1.44 GHz, A55 at 1.80 GHz, the Adreno held at level 9, 315 MHz). That is finding 127's
  nominal T612, closer to the Helio G80's 2.0 GHz A75 than 127's calibrated 1.21 / 1.50 GHz.
  Samples every 5 s: cpus 0-5 online and the GPU at 315 MHz throughout; battery 26-31 °C.
- **The run**: the Redmi's keys with `DHD_SCALE=4` (picture 2048 x 1792, shown 1440 x 1080 in
  the 2400 x 1080 window), the touch controls drawn (classic pad, as on the Redmi), 90 s cool-down
  with the screen on and other apps closed.
- **`DHD_DIAG_OUTPUT=896` kept**: on the Redmi, 896 lines was the picture's height and 83 % of the
  screen's 1080. At 4x the picture (1792 lines) is taller than the Mi's screen, so a "picture
  height" rule would change nothing. 896 lines cuts the present pass by the same fraction as on
  the Redmi: 1991 x 896 instead of 2400 x 1080.
- **The release-signed app**: 0.7.12 uninstalled first, after its files were backed up (the ISO
  moved aside on the phone, the rest on the PC and in a tar on the phone, md5 checked). At the
  end the release APK went back on (sha256 equal to the release's `SHA256SUMS`) and the files
  were restored: md5 equal, the app's new uid, the app's SELinux context. The same method would
  let the Adreno regression check above run.

| | on 1 | on 2 | off 1 | off 2 |
|---|---:|---:|---:|---:|
| fields/s over the route | 61.05 | 61.08 | 79.29 | 79.16 |
| p99 field ms | 32.6 | 32.1 | 23.8 | 24.2 |
| busy ms a field: render / VU / GS | 11.3 / 12.5 / 15.8 | 11.3 / 12.5 / 15.8 | 4.4 / 11.7 / 10.6 | 4.5 / 11.7 / 10.7 |
| GPU ms a frame (timestamps) | 13.16 | 13.16 | 8.26 | 8.24 |
| race windows (30 s): limits | gpu at the start, then cpu:render x3 | the same | gpu, cpu:vu x2 | cpu:gs, cpu:vu x2 |

Over the route, +18.2 fields/s (+29.7 %); the Redmi gave +12.9 (+25.8 %). The 30 s windows cover
different fields on the two phones, because the Mi runs faster. So the scenes are compared over the
same field ranges, from the `DHD_TIMELINE` CSVs (fields/s, means of the two runs; the Mi's GPU ms a
frame in brackets):

| fields | Mi on | Mi off | gain | Redmi on | Redmi off | gain |
|---|---:|---:|---:|---:|---:|---:|
| menus and loading, 300-6060 | 62.5 (11.8) | 73.6 (8.0) | +18 % | 54.3 | 59.3 | +9 % |
| race start, 6321-7500 | 47.3 (17.4) | 74.3 (7.8) | +57 % | 37.8 | 63.7 | +69 % |
| 7500-9000 | 60.7 (14.2) | 77.6 (8.7) | +28 % | 47.4 | 66.0 | +39 % |
| 9000-10500 | 63.5 (13.5) | 82.3 (8.5) | +30 % | 48.4 | 66.6 | +38 % |
| 10500-12000 | 62.7 (13.8) | 92.6 (8.8) | +48 % | 49.5 | 65.9 | +33 % |
| 12000-14000 | 66.6 (12.9) | 94.8 (8.3) | +42 % | 51.9 | 66.6 | +28 % |
| **the race, 6321-14000** | **60.3** | **84.6** | **+40.3 %** | **47.2** | **65.9** | **+39.6 %** |

The two runs of each state agree within 1.2 % on every range.

What differs:

- **The level**: the Mi at 4x is 22-28 % faster than the Redmi at 2x in both states (race 60 against
  47 with all on). So it cannot answer "does it reach 60".
- **The limit**: with all on, the Mi is GPU-bound only at the race start (GPU 17.4 ms of a 21 ms
  frame). Mid-race the render thread is the limit (busy 13.5-15 ms a field, A78 at 1.44 GHz), with
  the GPU 80-90 % busy (13-14 ms of a 15-16.5 ms frame). The Redmi's counters show its GPU busy
  throughout.
- **The off state**: the Redmi stays flat at about 66, held by the VU thread. The Mi's faster CPU
  lets it climb to 95 late in the race. That is why the shape per range differs (Mi +28 to +48 %,
  Redmi +28 to +39 %), and why the menus gain twice as much on the Mi.
- **The render thread** drops more on the Mi (11.3 -> 4.5 ms a field, Redmi 12.6 -> 8.1). Part of
  the Mi's gain is CPU work, not GPU work.

**Verdict**: the Mi 11i throttled at 4x can stand in for the Redmi 9 at 2x on one measure only:
the gain over the whole race (6321-14000) from a large GPU-side change. On this change it was
within 1 point (+40.3 % against +39.6 %), and over the route within 4 points (+29.7 % against
+25.8 %); count it as matching within ±5 points. It is not a stand-in for:

- absolute fps or the 60 target;
- a single scene (up to 15 points apart);
- a GPU-only change mid-race, where the Mi is render-thread-bound and would understate the gain.

From the GPU times, about 5x (1.56x the pixels: the race start's 17.4 ms -> about 27 ms, the
Redmi's 26 ms) would bring the Mi's GPU load to the Redmi's. This estimate is not measured.

## Mi 11i stand-in at 5x

The 4x test repeated at `DHD_SCALE=5` (picture 2560 x 2240), the estimate from the GPU times above.
Everything else was the same: the `fx5` APK, the keys, the route, `--apply t612`,
`DHD_DIAG_OUTPUT=896` in the off state, on, off, on, off with 90 s cool-downs, the screen on and
other apps closed. Samples every 5 s showed cpus 0-5 online and the GPU at 315 MHz throughout, with
the battery at 27-32 °C. At the end the throttle restore printed RESTORED, the 0.7.12 release APK
went back on (sha256 equal to `SHA256SUMS`) and the app's files were restored (md5 equal).

| | on 1 | on 2 | off 1 | off 2 |
|---|---:|---:|---:|---:|
| fields/s over the route | 45.38 | 45.49 | 69.16 | 69.38 |
| p99 field ms | 42.7 | 42.4 | 23.8 | 23.3 |
| busy ms a field: render / VU / GS | 15.5 / 12.8 / 21.8 | 15.5 / 12.8 / 21.7 | 4.5 / 11.7 / 13.4 | 4.6 / 11.7 / 13.6 |
| GPU ms a frame (timestamps) | 19.49 | 19.48 | 12.15 | 12.15 |
| race windows (30 s): limits | gpu x5, cpu:render x1 | the same | gpu x3, cpu:vu x1 | the same |

With all on, the Mi is now GPU-bound mid-race: 5 of the 6 race windows give `limit=gpu`, at
18.7-26 GPU ms a frame. One window (48 fps) gives cpu:render. At 4x it was render-thread-bound
after the start. With all off, GPU and VU alternate. The Redmi's off state was mostly cpu:vu.

The same field ranges as before (fields/s, means of the two runs; the Mi's GPU ms a frame in brackets):

| fields | Mi 5x on | Mi 5x off | gain | Mi 4x gain | Redmi on | Redmi off | gain |
|---|---:|---:|---:|---:|---:|---:|---:|
| menus and loading, 300-6060 | 47.9 (17.8) | 66.2 (12.1) | +38 % | +18 % | 54.3 | 59.3 | +9 % |
| race start, 6321-7500 | 34.8 (25.7) | 72.2 (11.2) | +107 % | +57 % | 37.8 | 63.7 | +69 % |
| 7500-9000 | 44.4 (20.7) | 70.6 (12.6) | +59 % | +28 % | 47.4 | 66.0 | +39 % |
| 9000-10500 | 46.2 (19.7) | 73.0 (12.2) | +58 % | +30 % | 48.4 | 66.6 | +38 % |
| 10500-12000 | 45.3 (20.2) | 69.2 (12.9) | +53 % | +48 % | 49.5 | 65.9 | +33 % |
| 12000-14000 | 48.0 (19.0) | 74.4 (12.0) | +55 % | +42 % | 51.9 | 66.6 | +28 % |
| **the race, 6321-14000** | **43.9** | **72.0** | **+63.9 %** | **+40.3 %** | **47.2** | **65.9** | **+39.6 %** |

On every range, the two runs of each state agree within 3.3 % (the off state's race start: 71.0 and 73.3).

- **Absolute fps**: the 5x numbers are closer than the 4x ones, but they fall on opposite sides
  of the Redmi. With all on, the Mi is 5-12 % slower: the race is 43.9 against 47.2 (-7 %), the
  start 34.8 against 37.8 (-8 %) and the route 45.4 against 49.9 (-9 %). With all off it is
  5-13 % faster: the race is 72.0 against 65.9 (+9 %) and the route 69.3 against 62.8 (+10 %).
  The Mi's VU and render threads are faster, so its off state is not held at about 66 the way
  the Redmi's is.
- **Gains**: they overshoot by 20-39 points per range and by 24 over the race (+63.9 % against
  +39.6 %). At 4x the race gain was within 1 point. At 5x the Mi's GPU carries more of the frame
  than the Redmi's does (its CPU is faster, so less of the frame waits on the CPU), so a GPU-side
  change shows up larger.

**Verdict**: do not use 5x as the stand-in. It is within about 10 % of the Redmi's absolute fps
(low with all on, high with all off). Its gains run 20-39 points high, against about 1 point over
the race at 4x. Keep the 4x stand-in, for whole-race gains only, with the limits listed above.
5x can serve as a rough check of a GPU-bound "all on" level, if a 5-12 % low reading is
acceptable. Between the two scales the Mi moves from understating GPU-only gains mid-race (4x,
render-bound) to overstating them (5x, GPU-bound). A scale between them (not measured) would
probably land closer on both measures.

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
