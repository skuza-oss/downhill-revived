# 156: Where a frame's time goes, before replacing the VU1 and GS emulation

Status: in progress

Branch `perf/profile-vu1-gs` from `main` 085117f, started 2026-11-05. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

The owner's direction (2026-11-05): keep the decompiled game code, and consider replacing what is
still emulated for graphics (VU1 microprograms, VIF/DMA packets, the GS command stream and its
renderer front end) with native rendering. Before any design, measure where a frame's time goes
today, per thread and per stage, so the gain of a native path is known.

Measure the current main (no speed changes in this lane) on:
- **Windows** (this PC: the CPU and GPU named in dhd.log),
- **macOS** (the M1 MacBook Air build host),
- **Android, the Mi 11i** (later, after the next mobile release; the lead says when).

Per machine, at least: the attract/title, a menu with a 3D scene (the bike shop), the race start
(the slowest interval on phones, findings 147-148) and mid-race, uncapped
(`DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1`), at the scales players use there (the owner,
2026-11-05: 1x and 2x are neither played on desktops nor stress them):
- **Windows and macOS**: 4x and 8x, in one ordinary window size per machine (the internal
  resolution the scale sets is what costs; the window's size changes only the final present);
- **the phone**: 2x (the phone default) and 4x.

For each: frames per second and the time per frame of each thread and stage: EE game code, VU0,
VU1 (native kernels / translated / interpreted), VIF/DMA, the GS front end, the renderer's CPU side
(wgpu encoding, uploads, conversions, readbacks such as the Z read), and the GPU. Use what exists
first (dhd.log `perf:` lines, finding 129); add only instrumentation that is cheap and worth keeping.

Acceptance: a table per machine in this finding, the method to repeat it, and a ranked list of
what a native VU1+GS path would remove (ms per frame and which thread), versus what stays (game
code, audio, presentation). No game-state change.

## Progress

- [x] 2026-11-05: finding reserved
- [x] 2026-11-05: scales changed by the owner: desktops 4x and 8x in one ordinary window each;
  the phone 2x and 4x
- [x] 2026-11-05: `d9d2baa` DHD_TIMELINE's per-field CSV gains the render thread and the GPU
- [x] Windows tables (RTX 3070 and, as a second GPU, the CPU's integrated Radeon)
- [x] macOS tables (M1, 8 GB)
- [x] phone recipe (below; the Mi 11i is measured after the next mobile release)

## Summary

- **Windows with a fast GPU is held by the VU thread.** RTX 3070 at 4x and at 8x: the same
  254 fields/s over the route (272-275 mid-race, 214-222 at the race start, 187 in the title),
  because the VU thread (VIF unpack, VU1 programs, GIF hand-off) is busy for the whole of every
  field: 3.5-5.3 ms. The GPU needs 0.8-2.0 ms a frame. Removing that thread raises the ceiling to
  the next thread, the game (EE) thread: about 3.0 ms mid-race (about 335 fields/s) and 3.6 ms at the
  race start (about 280).
- **The M1 and the integrated Radeon are held by the GPU** at 4x and 8x (M1: 93-107 fps at 4x,
  58-72 at 8x; Radeon: 90-141 at 4x, 25-35 at 8x). There, CPU work a native path removes gives
  no frames by itself; only the GPU work it removes or cheapens does (the scanout pass, the
  texture path), and the draws themselves stay.
- **What a native VU1+GS path removes on the CPU** (race, ms per field, Windows / Mac): the VU
  thread 3.5-4.4 / 3.5-3.9, most of the GS thread 1.9-2.5 / 1.6-1.8, the EE's spin and waits on
  the graphics queue 0.4-0.6 spin / 0.4-0.6 (and up to 1.9 spin in the title), the lens flare's Z
  read 0.15 / 0.2-0.3; it shrinks the render thread's encoding (12-17 render passes a race picture
  today). **What stays:** the game code 1.9-2.9 ms (part of it builds the packets: not yet
  attributed), VU0 0.5-0.7, the sound mixer about 0.1, the wasm memory checks 0.1-0.2, wgpu and
  the driver, the present, and the GPU's draws.

## Method

All runs: this branch's game (a release build, `build_host64.py --release --debug`: the shipped code
with line tables; the Mac `--release --debug --target macos-arm64` from the same core's wasm2c
C, 8 parts), the race route of `race_compare.py` (`NAV`, `RACE_PAD`, `DHD_RACE_SYNC=1`,
`DHD_RACE_SEED=81257`), `DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1`, `DHD_SKIP_MOVIES=1
DHD_MEMCARD=0 DHD_SND_OFF=1 DHD_SETTINGS=none`, `DHD_BENCH=300-14000 DHD_BENCH_EXIT=1`, and
`DHD_TIMELINE=<path>` (one CSV row per field). With the frame clock every run draws the same
fields, so the scenes are fixed field windows on every machine:

| scene | fields | what is on screen |
|---|---|---|
| title | 450-1100 | menu 32: the attract flyover behind the title |
| shop | 2950-3750 | menu 34: the bike shop, the rider on the bike |
| race start | 6400-7300 | GO is field 6321 (countdown 6060), as on the phone (finding 148) |
| mid-race | 9000-14000 | finding 147's window |

Three kinds of run per machine and scale, never mixed:

1. **Timing** (the tables below): only the timeline. Per scene, from the CSV rows: fps = fields /
   their wall time; each thread's CPU ms per field (the OS's thread times; game, vu, gs and the
   render thread `native`); spin = the measured spin-wait wall time (CPU spent waiting: subtracted
   to give work); busy = wall time minus the thread's measured waits (game: waits for VU1 and the
   GS and a full ring; vu: idle and a full GS ring; render: waiting for a field and the present);
   GPU = `gpu_span_ms / gpu_frames` (one frame in 8 timed). A thread whose busy time equals the
   field time is the limit.
2. **Diagnostics** (stage times, never used for fps): `DHD_RSTAT=1` (the render thread's parts per
   picture), `DHD_GPU_TIMES=240` (GPU ms per kind of pass), `DHD_VU1_PROFILE` with
   `DHD_VU1_PROFILE_FIELDS=300-14000/100` (VU1 ms by microprogram, GIF apart). The rstat and
   gpu_times lines carry no field: they are placed by the nav-step and race-anchor lines around
   them (one picture per field in these runs); VU1 rows by wall time through the timeline.
3. **Profiles** (stage shares): Windows `port/tools/sample_profile.py --pid` (1 ms, the function
   holding each sample), macOS `/usr/bin/sample PID S 1` (call trees, every thread at every tick).
   Each sample is put in a stage by its function (the lists in "Stages" below); waits (OS waits,
   spins) are set apart, and a stage's ms = the timing run's CPU work (CPU minus spin) of that
   thread times the stage's share of the thread's work samples. The sampler slows the game, so
   only shares are taken from it.

Windows: Ryzen 5 7600 (6 cores, 12 threads), RTX 3070 (DX12; also the CPU's Radeon integrated
GPU through `DHD_WGPU_ADAPTER=integrated`), Windows 11, the hidden desktop (`hidden_game.py`,
`timed=True`, normal priority), windowed 1920x1080 (the PC's display size). macOS: MacBook Air M1,
8 GB (4 performance and 4 efficiency cores), macOS 27.2, Metal, borderless on the built-in display
(1440x900 points, a 2880x1800 drawable), started over ssh into the logged-in session. Picture
sizes: 4x 2048x1792 (2560x1792 while a widescreen title card is up), 8x 4096x3584.

## Tables

Columns: fps; field = ms per field; CPU work ms per field (CPU minus spin) / busy ms for game and
vu; gs CPU work; render CPU / busy; present = the render thread's ms inside the present;
GPU = ms per frame (span of the timed frames).

### Windows, RTX 3070

| scale | scene | fps | field | game work / busy | vu work / busy | gs | render CPU / busy | present | GPU | limit |
|---|---|---:|---:|---|---|---:|---|---:|---:|---|
| 4x | title | 187 | 5.35 | 1.55 / 1.01 | 5.31 / 5.35 | 2.45 | 0.72 / 0.54 | 0.32 | 0.79 | vu |
| 4x | shop | 248 | 4.03 | 0.86 / 0.77 | 3.95 / 4.03 | 1.15 | 1.04 / 0.70 | 0.30 | 1.13 | vu |
| 4x | race start | 214 | 4.67 | 3.57 / 3.56 | 4.44 / 4.67 | 2.46 | 1.98 / 1.78 | 0.36 | 1.10 | vu |
| 4x | mid-race | 272 | 3.68 | 2.92 / 2.98 | 3.51 / 3.67 | 1.86 | 1.64 / 1.33 | 0.32 | 0.92 | vu |
| 8x | title | 188 | 5.31 | 1.68 / 1.00 | 5.26 / 5.31 | 2.16 | 0.87 / 0.51 | 0.30 | 1.44 | vu |
| 8x | shop | 247 | 4.06 | 1.08 / 0.76 | 4.02 / 4.06 | 1.30 | 1.00 / 0.68 | 0.29 | 1.73 | vu |
| 8x | race start | 222 | 4.51 | 3.49 / 3.37 | 4.37 / 4.51 | 2.52 | 1.94 / 1.67 | 0.32 | 2.00 | vu |
| 8x | mid-race | 275 | 3.64 | 2.95 / 2.97 | 3.45 / 3.58 | 1.97 | 1.56 / 1.30 | 0.31 | 1.60 | vu |

Whole route (fields 300-14000): 253.9 fields/s at 4x, 254.6 at 8x.

### Windows, the CPU's integrated Radeon (same PC)

| scale | scene | fps | field | game work / busy | vu work / busy | gs | render CPU / busy | present | GPU | limit |
|---|---|---:|---:|---|---|---:|---|---:|---:|---|
| 4x | title | 141 | 7.08 | 1.72 / 1.73 | 5.25 / 5.51 | 1.86 | 4.69 / 0.73 | 6.28 | 6.75 | gpu |
| 4x | shop | 121 | 8.27 | 1.25 / 1.20 | 4.07 / 4.25 | 0.80 | 6.04 / 1.03 | 7.24 | 8.12 | gpu |
| 4x | race start | 90 | 11.06 | 4.54 / 3.63 | 4.16 / 4.09 | 1.81 | 7.86 / 1.79 | 9.27 | 10.48 | gpu |
| 4x | mid-race | 119 | 8.44 | 3.67 / 3.66 | 3.67 / 3.63 | 1.55 | 5.37 / 1.53 | 6.91 | 8.09 | gpu |
| 8x | title | 35 | 28.25 | 1.80 / 1.52 | 5.31 / 5.14 | 1.71 | 24.47 / 0.72 | 27.52 | 27.74 | gpu |
| 8x | shop | 30 | 33.42 | 1.25 / 1.22 | 4.27 / 4.32 | 0.96 | 28.11 / 1.07 | 32.35 | 33.09 | gpu |
| 8x | race start | 25 | 40.56 | 4.62 / 3.90 | 4.06 / 4.19 | 1.72 | 35.57 / 2.03 | 38.53 | 40.04 | gpu |
| 8x | mid-race | 31 | 31.88 | 4.14 / 4.10 | 3.75 / 3.80 | 1.52 | 27.30 / 1.75 | 30.12 | 31.44 | gpu |

Route: 119.0 fields/s at 4x, 31.2 at 8x. The render thread's CPU equals its present time here: the
driver spins inside the present (not seen on the RTX or the M1).

### macOS, M1

| scale | scene | fps | field | game work / busy | vu work / busy | gs | render CPU / busy | present | GPU | limit |
|---|---|---:|---:|---|---|---:|---|---:|---:|---|
| 4x | title | 107 | 9.36 | 1.62 / 1.73 | 4.47 / 4.67 | 1.49 | 1.66 / 1.40 | 7.96 | 9.50 | gpu |
| 4x | shop | 105 | 9.54 | 1.22 / 1.28 | 4.02 / 4.18 | 0.82 | 1.56 / 1.33 | 8.20 | 10.12 | gpu |
| 4x | race start | 93 | 10.75 | 3.74 / 3.62 | 3.48 / 3.59 | 1.84 | 2.91 / 2.71 | 8.04 | 10.45 | gpu |
| 4x | mid-race | 100 | 10.02 | 4.15 / 4.87 | 3.85 / 4.46 | 1.59 | 2.83 / 2.67 | 7.31 | 10.49 | gpu |
| 8x | title | 65 | 15.27 | 1.59 / 1.56 | 5.72 / 5.81 | 1.84 | 1.39 / 1.18 | 14.08 | 17.08 | gpu |
| 8x | shop | 65 | 15.47 | 1.44 / 1.77 | 5.10 / 5.72 | 1.07 | 1.93 / 1.77 | 13.52 | 17.11 | gpu |
| 8x | race start | 58 | 17.23 | 5.91 / 6.00 | 6.21 / 6.83 | 3.02 | 3.62 / 3.47 | 13.43 | 16.97 | gpu |
| 8x | mid-race | 72 | 13.97 | 8.23 / 10.59 | 9.05 / 11.58 | 3.60 | 4.23 / 3.99 | 9.49 | 14.94 | gpu |

Route: 101.2 fields/s at 4x, 68.7 at 8x. The GPU span on Metal is the frame's first pass start to
last pass end; the sum of the passes overcounts there (Apple's GPU overlaps passes: 25-77 ms of
"pass time" in a 10 ms frame at 4x), so only the span is given. The CPU columns at 8x mid-race are
twice those at 4x for the same work: the last 70 s of a GPU-bound run on a fanless laptop (see
Caveats); read the 4x CPU columns as the M1's work.

## Stages

ms per field of CPU work, from the profiles' shares (Windows: the RTX 4x timing run; Mac: 4x).
Race start / mid-race; the title and shop are in the same proportions unless noted.

| thread | stage | functions | Windows | Mac |
|---|---|---|---|---|
| game | game code | `w2c_core_rc_func_*` and the rest of the core | 2.35 / 1.89 | 2.47 / 2.93 |
| game | VU0 | `ps2_vu0_run`, `run_fast_program`, `native0_*`, `vu0c_*` | 0.58 / 0.48 | 0.71 / 0.63 |
| game | wasm memory translation | `w2c_core_address`, `host64_memory_address_slow` | 0.12 / 0.10 | 0.18 / 0.19 |
| game | sound mixer (SPU2) | `w2c_core_render` | 0.07 / 0.07 | <0.05 / 0.11 |
| game | graphics HLE (DMA kicks, queue) | `dma_*`, `graphics_*` | <0.05 | <0.05 |
| game | spin on the graphics queue | `idle_wait` | 0.58 / 0.37 (title 1.86) | 0.69 / 0.03 |
| vu | VU1 native kernels | `native_geometry_*`, `native_clip_run`, `native_skin_*`, `native_particles_*` | 2.07 / 1.81 | 1.74 / 2.00 |
| vu | VU1 translated blocks | `aot_block_*` | 0.89 / 0.48 | 0.62 / 0.55 |
| vu | VU1 dispatch / interpreter | `ps2_vu1_run`, `run_micro` | 0.34 / 0.30 | 0.21 / 0.25 |
| vu | VIF unpack and dispatch | `unpack_stream`, `vif_feed`, `execute` | 0.76 / 0.59 | 0.74 / 0.82 |
| vu | GIF hand-off (XGKICK) | `graphics_xgkick`, `graphics_gif_feed` | ~0.1 (inlined) | 0.11 / 0.12 |
| gs | draw recording | `native_draw*`, `native_triangle*`, `native_claim`, `gs_color` | 1.03 / 0.79 | 0.90 / 0.68 |
| gs | GS packet decode and registers | `gs_feed`, `gs_consume`, `gs_write` | 0.88 / 0.71 | 0.60 / 0.60 |
| gs | textures, CLUT, palettes | `load_clut`, `*palette*`, `native_decode` | 0.35 / 0.26 | 0.21 / 0.21 |
| gs | GS VRAM transfers | `gs_write_pixels`, `gs_mark_pages` | 0.10 / 0.04 | 0.09 / 0.05 |
| render | encoding and submit (wgpu, driver) | `dhdr_native_picture_indexed` | rstat `submit` 1.15 / 0.84 (`finish` 0.93 / 0.65) | 1.84 / 1.75 |
| render | uploads (vertex/index arenas) | `write_buffer` | rstat `write` 0.17 / 0.14 | 0.37 / 0.36 |
| render | recording passes | rstat `record` | 0.20 / 0.16 | 0.20 / 0.23 (rstat) |
| render | the flare's Z read | `dhdr_native_zread_start` | rstat `zstart` 0.15 / 0.15 | 0.21 / 0.28 |
| render | the present call (scanout pass, acquire, present) | `native_present` | 0.15 CPU | 0.30 / 0.27 |

Render passes per race picture: 17.4 at the race start, 11.8 mid-race (2.2 in the title, 4.3 in the
shop): one per render-target or Z-buffer change the GS stream makes.

VU1 programs by time (`DHD_VU1_PROFILE`, Windows 4x, mid-race; overlay 2): entry 0078 1.09 ms a
field, 1638 0.29, 0040 0.25, 07F0 0.21, 0020 0.15. Title: 0078 1.63, 0020 1.01, 06F0 0.35. (The
profile's own clock reads inflate its totals by about a third; the ranking holds.)

GPU per kind of pass (`DHD_GPU_TIMES`, ms a frame, race start / mid-race):

| | draw | scanout | present | convert | surface |
|---|---:|---:|---:|---:|---:|
| RTX 3070 4x | 0.81 / 0.63 | 0.07 / 0.08 | 0.07 / 0.07 | 0.07 / 0.08 | 0 |
| RTX 3070 8x | 1.62 / 1.22 | 0.15 / 0.17 | 0.07 / 0.08 | 0.05 / 0.06 | 0 |
| Radeon iGPU 4x | 9.02 / 6.70 | 0.75 / 0.74 | 0.59 / 0.58 | 0.05 / 0.06 | 0 |
| M1 4x (pass sums, overlapping) | 72.8 / 41.6 | 1.62 / 1.74 | 4.07 / 4.14 | 1.52 / 0.62 | 0 |

In the title and the shop the scanout and present passes weigh more (Radeon 4x: 0.79 and 0.78 of
6.9 ms; M1 4x: 5.0 and 7.9 of the overlapping sum).

## What a native VU1+GS path would remove, ranked

Ranked by what it would give where it limits; ms per field (race start / mid-race; Windows RTX,
then Mac M1).

1. **The VU thread, all of it: 4.4 / 3.5 ms (Windows), 3.5 / 3.9 ms (Mac).** VU1 native kernels
   2.1 / 1.8, VIF unpack 0.8 / 0.6, translated blocks 0.9 / 0.5, dispatch 0.3, GIF hand-off
   about 0.1. On a desktop with a fast GPU this thread is the frame-rate limit in every scene
   (busy = the whole field); removing it moves the limit to the game thread (3.6 / 3.0 ms busy):
   about +30 % at the race start and +23 % mid-race; in the title (5.3 ms of VU work against at most
   2.1 ms busy on any other thread, the GS thread, itself mostly removed) more than double. The transform, lighting, skinning and clipping it does move
   to vertex shaders: on these GPUs well under a millisecond (about 50,000 strip triangles a field,
   finding 146).
2. **Most of the GS thread: 2.5 / 1.9 ms (Windows), 1.8 / 1.6 ms (Mac).** Packet decode 0.9 / 0.7,
   textures and CLUTs 0.35 / 0.26, VRAM transfers 0.1; draw recording 1.0 / 0.8 becomes recording
   per mesh rather than per PS2 vertex, so most of it goes too. Not a limit today on any desktop
   measured (it waits half its time), so this is CPU (and phone power) saved, not frames, until the
   VU thread is gone.
3. **The EE's waits and spins on the graphics queue: 0.4-0.6 ms of spin in races, 1.9 in the
   title (Windows), and up to 1.1 ms of waiting for the GS a field.** They exist because the game
   synchronises with emulated hardware (DMA completion, the GS FINISH / the flare's Z read).
4. **GPU: the scanout pass and the conversions: 0.15 / 0.2 ms on the RTX at 4x / 8x, 0.8 ms on the
   Radeon at 4x (about 9 % of its frame), up to 5 ms of overlapping pass time in the M1's menus.**
   The PS2's display circuit read of the frame buffer becomes the one final composite a native
   renderer needs anyway (with the present, which stays). Where the GPU limits (M1, integrated
   GPUs, phones at 4x) this is the GPU time a native path surely removes; the draws are the rest.
5. **The render thread's GS-shaped work: about 0.3-0.6 ms.** The flare's Z read 0.15-0.3 (a GPU
   occlusion query instead), and part of the encoding: 12-17 render passes a race picture follow
   the GS stream's target switches (`finish` alone 0.65-0.93 ms on Windows). Uploads (0.14-0.37)
   stay in another form.

**What stays** (ms per field, race): the game code 1.9-2.9 (some of it builds the VIF/DMA packets:
the top functions, `rc_func_00213C08`, `0021B8A8` (VU0 calls), `002628A0`, `00213658`, are not yet
named; how much of the game code is the game's renderer is the next measurement, and that part is
replaced only if the game's renderer functions are rewritten), VU0 0.5-0.7 (game maths), the wasm
memory checks 0.1-0.2, the sound mixer about 0.1, wgpu and the driver's encoding (about 1 ms on
Windows, 1.8 on the M1, less with fewer passes), the present (0.15-0.3 CPU), and on the GPU the
draws: 73-87 % of every measured GPU frame. A native renderer could make those draws cheaper
(hardware texture formats and samplers instead of the GS texel path that finding 149 priced on
phones), but that is a design choice, not removed emulation.

## Phone recipe (the Mi 11i, after the next mobile release)

The same three kinds of run at 2x and 4x, with finding 147-149's runner (writes the run's `DHD_*`
keys into the app's dhd.ini by root and restores it, closes third-party apps, cools 45-90 s with
the screen on, samples clocks, records simpleperf, pulls the files):

1. APK: the release commit, `build_apk.py --release --line-tables` (code unchanged; the
   `libmain.debug.so` beside it symbolises), and the shipped PGO build for the headline numbers.
   Decide once whether the phone runs as players have it (no throttle) or on finding 127's frozen
   profile (`phone_throttle.py --apply t612 ...`, comparable with 146-149); record which.
2. Keys for every run: `DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1 DHD_SCALE=2|4 DHD_SKIP_MOVIES=1
   DHD_MEMCARD=0 DHD_SND_OFF=1 DHD_BENCH=300-14000 DHD_BENCH_EXIT=1`, the race route's `DHD_NAV`,
   `DHD_RACE_SYNC=1`, `DHD_RACE_PAD` and `DHD_RACE_SEED=81257` (race_compare.py), and
   `DHD_TIMELINE=<a file in the app's files folder>`.
3. Timing run: only the keys above. Diagnostics run: add `DHD_RSTAT=1 DHD_GPU_TIMES=240
   DHD_VU1_PROFILE=<file> DHD_VU1_PROFILE_FIELDS=300-14000/100`. Profile run: `simpleperf record
   -p PID -f 1000 --duration S` four times, started from the log: 1 s after `nav step 2` (title),
   1 s after `nav step 6` (shop), at `race anchor=g` (race start, 4 s) and about 45 s after it at
   phone rates (mid-race fields 9000-14000; check the field in the next heartbeat).
4. Read them as above: the scene windows of the CSV, the stages by the function lists in "Stages"
   (simpleperf's report per thread: `--sort comm,symbol`), the rstat and gpu_times lines placed by
   the nav and anchor lines. Adreno's timestamps are per pass and do not overlap as Apple's do.
5. Expected from 146-149: CPU-bound at 2x (the VU thread busy 17-18 ms of a race field), GPU-bound
   at the 4x race start.

## Decisions

- **Release builds** (`--release`): the code players run (finding 151: the chunked development C
  inlines less). `--debug` adds line tables and a PDB only.
- **The integrated Radeon as a second Windows GPU.** Its numbers are what a laptop without a
  discrete GPU looks like on this CPU, and it gave clean GPU numbers while the RTX was busy (below).
- **No sampling in timing runs.** The samplers stop threads at every tick; only their shares are
  used, multiplied by a clean run's CPU times.
- **CPU work = CPU minus measured spin.** Spinning on the graphics queue costs CPU but is a wait;
  it is listed apart because a native path removes it.
- **One instrumentation commit kept** (`d9d2baa`): DHD_TIMELINE's CSV and summary gain
  `native_cpu_ms`, `native_wait`, `present_wait`, `gpu_frames`, `gpu_ms` and `gpu_span_ms`. It is
  read only with DHD_TIMELINE set, is host-only (core id 3c01ff3cfcfea8d5 unchanged), and is what
  lets one run give every scene. The driver and analysis scripts stayed local; their rules are in
  "Method" and "Stages".

## Surprises

- **The VU thread, not the GPU or the game, limits a fast desktop, at 8x as at 4x.** 8x doubles
  the RTX's GPU time (0.9 -> 1.6 ms) and changes nothing else.
- **The title is the heaviest VU1 scene** (5.3 ms: programs 0078 and 0020) and spins the EE most
  (1.9 ms a field waiting for the queue).
- **Apple's GPU timestamps overlap.** The summed pass times on the M1 are 3-8 times the frame
  (`gpu_ms` 25-77 ms in a 10 ms frame); `gpu_span_ms` was added for that reason.
- **The Radeon's driver spins inside the present**: the render thread's CPU equals its time in the
  present (24-36 ms a field at 8x). One core burnt while GPU-bound on that GPU.
- **docs/TESTING.md has no Tracy, simpleperf or Instruments route** (the plan assumed one):
  `sample_profile.py` (Windows), `/usr/bin/sample` (macOS) and findings 146-149's simpleperf runner
  (Android) are what exists.
- `DHD_HEADLESS=1` (the Mac smoke runs') has no window and no renderer: no use for a GPU or render
  measurement.

## Caveats

- **Hidden desktop**: the Windows game ran on `hidden_game.py`'s hidden desktop (no focus, its
  window never shown); a focused window can differ (finding 129's perf lines count it as in front
  through `DHD_TEST_INPUT`). Presentation to a hidden window may cost less than to a visible one.
- **The RTX was shared during the first runs.** While the PC's owner played a game (the RTX at
  99-100 %, about 3.3 CPU threads), the same 4x run measured 6-41 fps with 24-164 ms of GPU a
  frame: time-slicing, not this game's work. Those runs are discarded (one also overlapped a
  release build). The RTX tables are from runs after the other game closed (GPU at 5 % before the
  run); the integrated Radeon's 4x timing run started while that game still ran (its 3.3 CPU threads
  busy; the Radeon itself not shared), its 8x and diagnostics runs after.
- **The M1**: macOS system daemons used about 2.5 cores during the runs (notification and
  address-book sync, XProtect); the 8x CPU columns rise late in the run (a fanless laptop under
  sustained GPU load; not verified with clock readings). The display stayed on; the game ran in
  the logged-in session.
- **Sampling**: Windows samples carry the function holding the pc and its inlined frames but no
  callers (a `memmove` is counted by its own name); the render thread's split on Windows is
  therefore from DHD_RSTAT's timers. macOS `sample` gives full call trees.
- **The EE game code is not split into "the game's renderer" and the rest**: that needs names for
  the hot functions (the next step for a design).

## Result

Measured on Windows (RTX 3070 and the integrated Radeon) and macOS (M1) at 4x and 8x; tables,
stages and the ranked list above; the phone recipe for the Mi 11i. One commit of instrumentation
(`d9d2baa`); no speed changes and no game-state change (host-only, core id unchanged).
