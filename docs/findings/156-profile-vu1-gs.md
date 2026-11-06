# 156: Where a frame's time goes, before replacing the VU1 and GS emulation

Status: current
Checked: 2026-11-06

Branch `perf/profile-vu1-gs` from `main` 085117f, started 2026-11-05; the desktops merged into
`main` at 113f76f, the phone (Mi 11i, the 0.7.11 code) measured on 2026-11-06.

## Goal and acceptance

The owner's direction (2026-11-05): keep the decompiled game code, and consider replacing what is
still emulated for graphics (VU1 microprograms, VIF/DMA packets, the GS command stream and its
renderer front end) with native rendering. Before any design, measure where a frame's time goes
today, per thread and per stage, so the gain of a native path is known.

Measure the current main (no speed changes in this lane) on:
- **Windows** (this PC: the CPU and GPU named in dhd.log),
- **macOS** (the M1 MacBook Air build host),
- **Android, the Mi 11i** (after the 0.7.11 mobile release: measured 2026-11-06).

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
- [x] phone recipe ("Phone method" below)
- [x] 2026-11-06: the Mi 11i on main 113f76f (0.7.11's code): timing runs as players have the
  phone and on finding 127's frozen low-end profile, 2x and 4x, two runs each; diagnostics and
  simpleperf as players have it

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
- **The Mi 11i as players have it** (no throttle): 2x runs at the 120 Hz display in races
  (116-117 fps; 119-126 fields/s over the route), the VU thread busy 7.6 of 8.6 ms at the race start
  and the whole field in the title (8.5-8.8 ms). At 4x the GPU holds the races (8.1-10.7 ms a frame:
  85 fps at the race start, 112 mid-race). Every CPU stage costs 1.3-2.6 times the desktops'.
- **On the frozen low-end profile** (`t612`, as findings 146-149) the VU thread holds every 2x
  scene (busy 96-100 % of the field: 51 fps in the title, 60 at the race start, 74 mid-race) and the GS
  thread is right behind it (work 9.6-17.4 ms a field, on the little cores): removing only the VU
  thread would leave the GS thread as the limit at nearly the same rate. Removing both moves the
  limit to the game thread and the render thread (about 12 ms busy each at the race start: about
  80 fps instead of 60). At 4x the GPU limits the race (19.3 ms a frame at the race start).
- **On the phone a native path also removes more than half of the process's CPU time**: the VU
  thread, most of the GS thread and the game thread's spin on a big core (2.3-3.9 ms a field, 9.7
  in the title on the low-end profile) are 57-62 % of it at 2x and 4x on both profiles (of 20 ms a
  field as players have it, 48 on the low-end profile): heat and battery, even where no frame is
  gained.

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

## The phone (Mi 11i)

### Phone method

The recipe planned here before the release, as run on 2026-11-06:

- **One APK for every run**: main 113f76f (0.7.11's code plus `d9d2baa`'s timeline columns,
  host-only), `build_apk.py --release --line-tables --pgo-use <the profile the 0.7.11 APK was
  built with>`: the shipped build's code, with `libmain.debug.so` beside it for symbols.
  Release-signed, installed with `adb install -r`; the shipped 0.7.11 APK put back at the end.
- **Two profiles, decided before the runs**: timing runs as players have the phone (no throttle)
  and on finding 127's frozen low-end profile (`phone_throttle.py --apply t612 --big-max 1209600
  --little-max 1497600`, comparable with 146-149; `--restore` after each pair). Two runs of each
  scale and profile, interleaved. Diagnostics and simpleperf runs as players have it.
- **The runner** (scratch, not committed) is findings 147-149's: it writes the run's `DHD_*` keys
  into the app's dhd.ini by root (dhd.ini passes every `DHD_*` key it holds into the environment,
  so `DHD_TIMELINE`, `DHD_RSTAT`, `DHD_GPU_TIMES` and `DHD_VU1_PROFILE` need no patch), closes the
  third-party apps, cools 60 s with the screen on, starts the app, samples clocks every 3 s, pulls
  dhd.log, the timeline CSV and the VU1 profile, and puts the original dhd.ini back (md5 checked).
- **Keys**: the recipe's (`DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1 DHD_SCALE=2|4
  DHD_SKIP_MOVIES=1 DHD_MEMCARD=0 DHD_SND_OFF=1 DHD_BENCH=300-14000 DHD_BENCH_EXIT=1`, the race
  route, `DHD_TIMELINE`), plus finding 147-149's `DHD_RENDER=native DHD_WIDESCREEN=0
  DHD_ASPECT=4:3`: 2x draws 1024x896 (1280x896 under a widescreen card), 4x 2048x1792, shown in
  a 1440x1080 area of the 2400x1080 screen, Vulkan mailbox, the display at 120 Hz.
- **Profiles**: `simpleperf record -p PID -f 1000` (cpu-cycles at 1 kHz a thread; samples, not
  cycles, are counted, so a sample is about a millisecond on any core at any clock), 5-6 s at four
  points read from the log: 1 s after `nav step 2` (title), after `nav step 6` (shop), after `race
  anchor=g` (race start), and from the heartbeats after GO aimed at about field 9300 (2x) and
  10300 (4x) for mid-race. Symbols through simpleperf's binary cache with the build's
  `libmain.debug.so`; each sample address then goes through `llvm-symbolizer --inlining`, and the
  innermost inlined function that matches a stage of its thread decides the stage (PGO inlines the
  VU1 kernels into `ps2_vu1_run`, the strip assembly into `native_draw_strip`). A stage's ms = the
  timing runs' CPU work of that thread (mean of the two runs) times its share of the thread's
  samples outside its spin.

### Phone tables

Columns as the desktops' (fps = 1000 / the mean field; the two runs' fps in brackets where they
differ), plus the game thread's spin and its wait for the GS thread, ms per field. The GS thread's
busy time is not given: on the phone it includes its waits for a free picture slot of the render
thread.

As players have the phone (no throttle):

| scale | scene | fps | field | game work / busy | vu work / busy | gs | render CPU / busy | present | GPU | EE spin | EE waits GS | limit |
|---|---|---:|---:|---|---|---:|---|---:|---:|---:|---:|---|
| 2x | title | 116 (101/134) | 8.65 | 2.35 / 2.10 | 8.52 / 8.65 | 3.90 | 1.85 / 1.53 | 2.01 | 2.10 | 3.85 | 6.54 | vu |
| 2x | shop | 127 | 7.90 | 1.76 / 1.63 | 6.91 / 6.93 | 1.89 | 2.21 / 2.08 | 4.70 | 2.76 | 3.25 | 6.27 | vu, display near |
| 2x | race start | 116 | 8.61 | 5.91 / 5.77 | 7.60 / 7.64 | 3.43 | 4.69 / 6.12 | 2.41 | 4.82 | 2.35 | 2.84 | vu, display near |
| 2x | mid-race | 117 | 8.57 | 4.97 / 5.04 | 6.31 / 6.32 | 2.40 | 3.97 / 3.67 | 4.90 | 3.50 | 2.86 | 3.53 | display (120 Hz) |
| 4x | title | 112 (98/130) | 8.94 | 2.34 / 2.12 | 8.75 / 8.92 | 3.64 | 1.83 / 1.52 | 2.21 | 6.00 | 3.85 | 6.82 | vu |
| 4x | shop | 115 | 8.73 | 1.72 / 1.81 | 7.03 / 7.10 | 1.78 | 2.44 / 6.13 | 1.92 | 7.90 | 3.22 | 6.92 | gpu |
| 4x | race start | 85 | 11.75 | 6.72 / 5.89 | 8.34 / 8.38 | 3.54 | 4.83 / 8.70 | 3.04 | 10.67 | 3.53 | 5.86 | gpu |
| 4x | mid-race | 112 | 8.91 | 5.56 / 5.64 | 7.46 / 7.53 | 3.26 | 4.33 / 6.62 | 2.27 | 8.07 | 2.31 | 3.27 | gpu, vu near |

Route (fields 300-14000): 119.4 and 125.5 fields/s at 2x, 110.1 and 111.8 at 4x.

On the frozen low-end profile (`t612`):

| scale | scene | fps | field | game work / busy | vu work / busy | gs | render CPU / busy | present | GPU | EE spin | EE waits GS | limit |
|---|---|---:|---:|---|---|---:|---|---:|---:|---:|---:|---|
| 2x | title | 51 | 19.59 | 5.37 / 3.22 | 18.24 / 19.59 | 17.36 | 4.57 / 3.38 | 1.70 | 3.55 | 9.66 | 16.37 | vu |
| 2x | shop | 64 | 15.70 | 3.92 / 2.91 | 14.21 / 15.70 | 9.64 | 6.48 / 5.33 | 1.73 | 4.64 | 6.81 | 12.79 | vu |
| 2x | race start | 60 | 16.75 | 11.16 / 12.43 | 14.34 / 16.10 | 14.87 | 13.30 / 12.31 | 2.29 | 7.51 | 2.95 | 4.32 | vu, gs near |
| 2x | mid-race | 74 | 13.55 | 9.73 / 11.16 | 11.64 / 13.43 | 11.69 | 10.64 / 9.64 | 1.84 | 5.78 | 1.84 | 2.40 | vu |
| 4x | title | 51 | 19.66 | 5.32 / 3.25 | 18.27 / 19.66 | 17.39 | 4.60 / 3.41 | 1.71 | 10.61 | 9.80 | 16.41 | vu |
| 4x | shop | 59 | 16.91 | 3.60 / 3.18 | 14.36 / 15.77 | 9.23 | 6.82 / 9.46 | 1.89 | 13.88 | 5.93 | 13.73 | vu |
| 4x | race start | 44 | 22.51 | 12.06 / 11.82 | 15.78 / 16.88 | 16.70 | 14.07 / 16.40 | 6.10 | 19.28 | 6.20 | 10.69 | gpu |
| 4x | mid-race | 63 | 15.76 | 9.81 / 11.20 | 12.24 / 13.66 | 11.67 | 11.20 / 13.69 | 1.88 | 14.08 | 2.63 | 4.55 | gpu, vu near |

Route: 65.0 and 64.8 fields/s at 2x, 57.7 and 57.4 at 4x (the two runs within 0.5 % in every
scene). For comparison, finding 149's PGO build on the same profile (sound on, window runs): race
start 61.6, fields 9000-14000 76.4; here 60 and 74.

### Phone stages

ms per field of CPU work as players have the phone (the profiles' shares times the timing runs'
work); race start / mid-race, and the title at 2x.

| thread | stage | 2x | 4x | title 2x |
|---|---|---|---|---:|
| game | game code (`w2c_core_*`) | 3.59 / 3.18 | 4.11 / 3.66 | 1.13 |
| game | VU0 (`run_fast_program`, `ps2_vcallms`, `vu0c_*`) | 1.23 / 0.89 | 1.29 / 0.93 | 0.40 |
| game | wasm memory translation | 0.21 / 0.20 | 0.19 / 0.23 | 0.05 |
| game | sound mixer (SPU2) | 0.08 / 0.06 | 0.10 / 0.12 | 0.06 |
| game | graphics HLE (DMA kicks, queue) | 0.03 / 0.03 | 0.05 / 0.05 | 0.03 |
| game | libc copies, atomics, kernel | 0.59 / 0.42 | 0.66 / 0.40 | 0.48 |
| game | spin on the graphics queue (`idle_wait`; measured, not sampled) | 2.35 / 2.86 | 3.53 / 2.31 | 3.85 |
| vu | VU1 native kernels (`native_geometry_*`, `native_clip_run`, ...) | 3.81 / 3.43 | 4.25 / 4.13 | 4.31 |
| vu | VU1 translated blocks (`aot_block_*`) | 1.29 / 0.89 | 1.52 / 1.05 | 1.29 |
| vu | VIF unpack and dispatch | 1.03 / 0.84 | 1.09 / 0.96 | 1.14 |
| vu | VU1 dispatch / interpreter (`ps2_vu1_run`, `run_micro`) | 0.67 / 0.54 | 0.76 / 0.67 | 0.70 |
| vu | GIF hand-off (XGKICK) | 0.05 / 0.05 | 0.05 / 0.09 | 0.06 |
| vu | libc copies and clears, kernel wakeups | 0.62 / 0.48 | 0.56 / 0.44 | 0.93 |
| gs | draw recording (`native_draw_strip`, `packed_strip`, ...) | 1.39 / 0.93 | 1.26 / 1.30 | 1.62 |
| gs | GS packet decode and registers | 1.13 / 0.91 | 1.20 / 1.23 | 1.59 |
| gs | textures, CLUT, palettes | 0.43 / 0.31 | 0.42 / 0.44 | 0.12 |
| gs | GS VRAM transfers | 0.11 / 0.07 | 0.14 / 0.05 | 0.02 |
| gs | kernel, libc, atomics | 0.25 / 0.11 | 0.41 / 0.15 | 0.40 |
| render | Vulkan driver (Adreno) | 1.50 / 1.23 | 1.67 / 1.40 | 0.50 |
| render | dhd_render and wgpu (encoding, submit) | 1.37 / 1.11 | 1.40 / 1.29 | 0.38 |
| render | kernel (wakeups, the GPU driver's ioctls) | 0.67 / 0.70 | 0.49 / 0.47 | 0.40 |
| render | memcpy (uploads into mapped buffers) | 0.38 / 0.30 | 0.43 / 0.33 | 0.28 |
| render | allocator, libgui, the rest | 0.77 / 0.63 | 0.84 / 0.84 | 0.29 |

The render thread by `DHD_RSTAT` (diagnostics runs, ms a picture, race start / mid-race): 2x
picture 5.54 / 2.62 = record 0.65 / 0.37, write 0.36 / 0.28, submit 4.49 / 1.93 (the queue submit
`qsub` 2.70 / 0.53, `finish` 1.76 / 1.37), present 0.46 / 0.31, the flare's Z read `zstart` +
`ztake` 0.58 / 0.87; 4x picture 10.05 / 5.70 with `qsub` 7.06 / 2.97 (Adreno's driver blocks the
submit while the GPU is behind: a wait, not work) and the Z read 0.24 / 1.05. Render passes per
picture: 18.0 at the race start, 11.75 mid-race, 6 in the shop, 1 in the title (as on the
desktops).

GPU per kind of pass (`DHD_GPU_TIMES`, ms a frame; Adreno's passes do not overlap, so the sum and
the span agree within 0.8 ms):

| | draw | scanout | present | convert | sum |
|---|---:|---:|---:|---:|---:|
| 2x race start / mid-race | 3.38 / 2.16 | 0.16 / 0.15 | 0.45 / 0.39 | 0.12 / 0.11 | 4.11 / 2.81 |
| 4x race start / mid-race | 10.05 / 6.20 | 0.59 / 0.60 | 0.49 / 0.50 | 0.11 / 0.12 | 11.24 / 7.41 |
| 4x title / shop | 4.76 / 6.27 | 0.78 / 0.74 | 0.31 / 0.29 | 0 / 0.09 | 5.85 / 7.39 |

VU1 programs (`DHD_VU1_PROFILE`, 2x, ms a field on the profile's own clock, ranking only): mid-race
0078 1.67, 1638 0.60, 0040 0.45, 07F0 0.37, 0020 0.26; title 0078 3.32, 0020 2.12, 06F0 0.69: the
desktops' order, at about twice the Windows times.

### What a native VU1+GS path would remove on the phone, ranked

1. **The VU thread: 6.3-8.3 ms a race field as players have it (8.5-8.8 in the title), 11.6-15.8
   on the low-end profile (18.2-18.3 in the title).** Kernels about half of it, translated blocks and
   VIF unpack the next quarter. It is the limit of every 2x scene on the low-end profile, of the
   title at both scales as players have it, and close behind the 120 Hz display in 2x races.
2. **The GS thread: 1.8-3.9 ms as players have it, 9.2-17.4 on the low-end profile.** Draw
   recording and packet decode about 40 % each, textures and CLUTs 5-13 %. On the low-end profile
   it runs on the little cores (finding 146) and at the 2x race start does as much work as the VU
   thread (14.9 against 14.3 ms): on a low-end phone the two must go together, or the GS
   thread takes over the limit at about the same frame rate. With both gone the next limits are
   the game thread (busy 12.4 ms at the race start, 11.2 mid-race) and the render thread (12.3 /
   9.6): about 80 fps at the race start instead of 60 (+35 %), about 90 mid-race instead of 74
   (+21 %), the title more than double.
3. **The game thread's spin and waits on the graphics queue: spin 2.3-3.9 ms a field as players
   have it, 1.8-9.8 on the low-end profile (the most in the title); waits for the GS 2.8-6.9 (up to
   16.4).**
   The spin burns a big core (heat, battery); with the VU and GS threads it is 57-62 % of the
   process's CPU time at 2x and 4x on both profiles.
4. **GPU: the scanout pass and the conversions, 0.27 ms at 2x and 0.7 ms at 4x a frame (6-10 %
   of a 4x race frame, 13 % of the 4x title's).** At 4x the races are GPU-bound (8.1-10.7 ms a frame
   as players have it, 14.1-19.3 on the low-end profile), and the draws are 84-89 % of the frame:
   the GPU frames a native path gains there come from cheaper draws (finding 149's texel path, a
   design choice), not from removed emulation.
5. **The render thread's GS-shaped work: the Z read 0.2-1.1 ms a picture, and part of 2.3-3.1 ms
   of driver plus wgpu encoding** for 12-18 passes a race picture. On the low-end profile the render
   thread (10.6-14.1 ms of CPU a race field) becomes a co-limit once the VU and GS threads are
   gone, so fewer passes and less driver work count on low-end phones, unlike the desktops.

**What stays on the phone** (as players have it, race): the game code 3.2-4.1 ms, VU0 0.9-1.3, the
wasm memory checks 0.2, the sound mixer 0.1, the Vulkan driver and wgpu 2.3-3.1 ms a field
(less with fewer passes), the present 0.3-0.5, and on the GPU the draws.

### Phone and desktops

Mid-race at 4x, ms per field (CPU work; GPU ms a frame):

| | Windows RTX 3070 | macOS M1 | Mi 11i | Mi 11i, low-end profile |
|---|---:|---:|---:|---:|
| fps | 272 | 100 | 112 | 63 |
| game work | 2.92 | 4.15 | 5.56 | 9.81 |
| vu work | 3.51 | 3.85 | 7.46 | 12.24 |
| gs work | 1.86 | 1.59 | 3.26 | 11.67 |
| render CPU | 1.64 | 2.83 | 4.33 | 11.20 |
| GPU | 0.92 | 10.49 | 8.07 | 14.08 |
| limit | vu | gpu | gpu (vu near) | gpu (vu near) |

The phone's CPU stages cost 1.3-2.6 times the desktops' as players have it and 2.4-7.3 times on
the low-end profile (the GS thread most, on little cores); the split inside each thread is the same
(kernels about half of the VU thread, recording and decode most of the GS thread). The Adreno
660's GPU time at 4x is about the M1's (8-11 ms a race frame). The phone gains most from a native path where the desktops gain
nothing: on the low-end profile at 2x, the scale phones run by default.

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
- **The phone both ways** (decided before the runs): timing as players have it and on the frozen
  low-end profile; diagnostics and profiles only as players have it, so the low-end profile's
  stage split is not measured (its thread totals are; the split inside a thread was the same on
  every machine measured).
- **The shipped PGO build for every phone run, with line tables**: the headline numbers and the
  symbols from one APK (line tables change no code, finding 146).
- **No new instrumentation for the phone**: the timeline, rstat, gpu_times and the VU1 profile
  were enough.

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
- **The phone at 2x reaches its 120 Hz display**: unpaced and uncapped, Vulkan mailbox, the 2x
  races still run at 116-117 fps with the render thread waiting 2-5 ms a field in the present.
  Mailbox on Android does not free the game from the display's rate as it does on the desktops.
- **On the low-end profile the GS thread costs as much as the VU thread** (title 17.4 against
  18.2 ms, race start 14.9 against 14.3), against about half of it as players have it: it lands on
  the little cores. A native path that removed only VU1 would gain little on low-end phones.
- **Adreno's driver blocks inside `vkQueueSubmit`** while the GPU is behind (`qsub` 3-7 ms a
  picture at 4x): on the phone the render thread's GPU wait is in the submit, not in the present.
- **The PGO build inlines the VU1 kernels into `ps2_vu1_run`** and the strip assembly into
  `native_draw_strip`: a flat simpleperf report by symbol shows `ps2_vu1_run` at 16 % of all
  samples. The stages need the inline frames (llvm-symbolizer --inlining on each sample address).
- **The local patch for `DHD_*` keys on Android was not needed**: dhd.ini passes every `DHD_*`
  key it holds into the environment (the settings crate's unknown-key rule), so diagnostic keys
  work from dhd.ini as the runner writes it.

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
- **The phone as players have it varies between runs**: the GPU sat at 608 MHz in every sample
  (the thermal level the phone keeps at rest, not the 840 MHz top); the second pair of runs started
  warmer (battery 34 °C against 24-30 °C) and its A78 cores ran mostly at 1.3-1.9 GHz against 2.4.
  The VU thread's work differs by up to 45 % between the two runs (mid-race 2x 5.1 / 7.5 ms) and
  the title the other way (9.7 / 7.4 ms; 101 / 134 fps): the scheduler's choice of core as well as
  the clocks. The 2x races stayed at the display's rate in both. The tables give the mean of the
  two. The low-end profile's two runs agree within 0.5 %.
- **Phone sound off** (`DHD_SND_OFF=1`, as the desktops): the SPU2 mixer still runs on the game
  thread (0.1 ms); the audio output thread does not. Findings 146-149 ran with sound.
- **The phone's mid-race profiles** cover fields of about 9300-10000 (2x) and 10300-10900 (4x), not
  all of 9000-14000; the timing columns they are scaled by cover the whole window.
- **Throttle restore once reported a difference**: the first `--apply` saved the minimum clocks
  while an install's boost held them at the maximum; `--restore` put every other value back and the
  minimums returned to their defaults by themselves (691200 / 710400 / 844800 kHz, the values of
  earlier restores). The second pair waited for the default minimum before applying; its restore
  read back clean. MIUI brought cpu6-7 online a few times during the low-end runs; the watchdog
  took them off again within half a second.

## Result

Measured on Windows (RTX 3070 and the integrated Radeon) and macOS (M1) at 4x and 8x, and on the
Mi 11i at 2x and 4x as players have it and on the frozen low-end profile; tables, stages and the
ranked lists above. On fast desktops the VU thread is the limit; on the M1, integrated GPUs and
4x phone races the GPU is, and the draws stay; on a low-end phone at 2x the VU and GS threads
together are the limit and a native path would gain about a third at the race start and remove
more than half of the CPU time. One commit of instrumentation (`d9d2baa`); no speed changes and no
game-state change (host-only, core id unchanged).
