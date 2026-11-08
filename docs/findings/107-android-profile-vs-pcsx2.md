# 107: Current Android profile versus PCSX2

Status: current
Checked: 2026-11-04

2026-10-28, Mi 11i M2012K11G / Snapdragon 888 / Adreno 660 / Android 13, adb `1ae461e`. The current
native port reaches the 60 Hz pacing limit on the first level: **1x median 59.748 fields/s across
three runs**; default scale 2 gives 59.600 fields/s across two runs. Finding 091's 25-30 fields/s
numbers describe older code. This is a fresh measurement, not an isolated A/B attribution to any one
intervening optimisation.

## Build identity and preservation

Gameplay/source base `4de73d87b`; build tooling commit `137b4125291d21db8b1dab93a9ff6a2d414206a5`,
branch `prof/android-pcsx2`. APK 0.6.4 / versionCode 604, normal debug signature and
release-optimised native libraries. Portable core SHA-256:
`8bcfccccb6ac8bc87667fd2e8ab30c0e60d4f067627d96573f44a466642963a7`. Rebuilt with `build_wasm.py -j
2`; its bytes equal the main checkout's existing current core. `build_host64_android.py --jobs 2`
and `build_apk.py --jobs 2 --offline` both pass.

Three AOT generators compared a resolved destination against an unresolved build path and rejected
that junction; their containment guards now resolve both sides (outside-build rejection checked).
These are build-only fixes; no game/port runtime or matching check changed. No alternate game code,
PGO or speed experiment was used. All output, caches, temporary files and the SDL source download
stayed on F:. The main checkout was read-only.

`adb install -r` succeeded, retaining the ISO and app data; no uninstall was needed. Settings,
shared preferences and the original log were backed up before changes and restored afterwards.
`restoration.json` verifies original `dhd.ini` and `dhd.log` byte for byte and records the unchanged
2,530,869,248-byte ISO size. No saves were present initially; `DHD_MEMCARD=0` prevented writes. The
updated APK remains installed, stopped. `/sdcard/PS2/` was never accessed. No game, benchmark or
PCSX2 process ran on the PC.

## Scene and measurement recipe

The existing `bench_race.SCENES['race']` route selects the normal first level, **Mt. Midoule, Italy
FR (ALPINE)**, confirmed by `raw/calibration.png`. Same navigation, race pad and seed 81257 as
finding 091. Calibration with `DHD_RACE_FRAMELOG` found countdown at field 2954, GO at 3214; every
valid measurement run logged those same anchors.

Normal runs use `DHD_FPS=60`, native renderer/Vulkan, default two fields in flight, default Android
producer spin (3000 us), sound enabled, `DHD_WIDESCREEN=0`, `DHD_ASPECT=4:3`, scale 1 or 2,
`DHD_SKIP_MOVIES=1`, `DHD_MEMCARD=0`, `DHD_BENCH_EXIT=1`, and `DHD_TIMELINE` enabled. No flat clock,
strict sync, exact-probe override, affinity override or `DHD_PROFILE` was used (the latter would
disable deferred vsync and change the workload). Scale 2 means the default resolution multiplier,
with the comparison's 4:3 aspect retained. Native 1x is nominally the same 512x448 source resolution
as the supplied PCSX2 baseline, not identical rendered pixels.

A stale-log startup bug was caught and fixed by removing the prior log before launch; `s1-*` /
`s2-*` attempts are invalid and excluded. Only `run1` through `run5` below count. The calibration
includes menu/intro work and is excluded too. Timeline summaries were captured in every valid log.
Relative-path timeline CSV creation failed in Android's working directory; the scripts now use an
absolute app-files path for future runs. Do not treat the `*-timeline.csv` error captures as data.

## Repeated measurements

Field times are the bench's EE/field-boundary intervals; the separate presentation table below
measures what the native renderer actually presents. Neither is a promise of identical PCSX2 OSD
sampling boundaries.

| Run (order) | Scale | fields/s | field p50 ms | field p99 ms | field max ms |
|---|---|---|---|---|---|
| run1-s1 | 1 | 59.765 | 16.704 | 20.944 | 40.878 |
| run2-s2 | 2 | 59.684 | 16.728 | 22.475 | 39.657 |
| run3-s1 | 1 | 59.748 | 16.690 | 22.025 | 36.831 |
| run4-s2 | 2 | 59.516 | 16.691 | 20.994 | 47.452 |
| run5-s1 | 1 | 59.684 | 16.686 | 21.353 | 45.976 |

CPU milliseconds per field (includes scheduled spins and kernel time; **not wall busy time**):

| Run | game | vu | gs | native | sound | process | busy cores |
|---|---|---|---|---|---|---|---|
| run1-s1 | 10.518 | 8.979 | 4.481 | 8.528 | 0.196 | 33.791 | 2.02 |
| run2-s2 | 10.516 | 8.931 | 4.600 | 8.680 | 0.199 | 34.054 | 2.03 |
| run3-s1 | 10.697 | 9.805 | 4.999 | 8.979 | 0.207 | 35.829 | 2.14 |
| run4-s2 | 13.253 | 10.177 | 5.203 | 9.210 | 0.211 | 39.202 | 2.33 |
| run5-s1 | 12.100 | 10.336 | 5.257 | 9.065 | 0.216 | 38.099 | 2.27 |

| Run | displayed fps | present p50 ms | present p99 ms | present max ms |
|---|---|---|---|---|
| run1-s1 | 59.65 | 16.68 | 17.41 | 57.26 |
| run2-s2 | 59.56 | 16.68 | 18.36 | 64.81 |
| run3-s1 | 59.62 | 16.68 | 17.28 | 63.04 |
| run4-s2 | 59.44 | 16.69 | 18.60 | 64.70 |
| run5-s1 | 59.56 | 16.68 | 17.37 | 73.79 |

Audio output: run1-s1: 1 underrun(s), 70.0 ms silence; run2-s2: 1 underrun(s), 90.0 ms silence;
run3-s1: 1 underrun(s), 107.6 ms silence; run4-s2: 1 underrun(s), 90.0 ms silence; run5-s1: 1
underrun(s), 70.0 ms silence. There is no PCSX2 audio-counter baseline.

Each run also has a 57-74 ms maximum presentation interval despite its low p99. Without the
per-field CSV or a scheduler/GPU trace, these isolated gaps cannot be localised or assigned to
benchmark-start draining, a driver stall or another cause. They remain a smoothness issue to
investigate; the near-60 average does not establish that every picture arrives on time.

## Thread comparison with the supplied PCSX2 baseline

User's Aether Play / PCSX2: Vulkan, 1x 512x448, MTVU on, hand-ridden normal first level near 22 s:
59.4 fps; frame min/average/max 10.2/16.8/22.4 ms; EE 11.40 ms (67.7%), GS 8.66 (51.4%), VU 8.71
(51.7%), GPU 2.07 (12.3%). The fork's build, clocks, temperature and other settings are unknown.

| Work | Ours CPU ms/field (1x median) | PCSX2 CPU ms/frame | Ours minus PCSX2 ms |
|---|---|---|---|
| game / EE | 10.697 | 11.400 | -0.703 |
| vu / MTVU | 9.805 | 8.710 | +1.095 |
| gs front end | 4.999 | not separately supplied | — |
| native renderer | 8.979 | inside its GS thread | — |
| gs + native (combined CPU) | 13.978 | 8.660 | +5.318 |
| sound output | 0.207 | not supplied | — |
| GPU execution (not thread CPU) | not instrumented | 2.070 | — |

PCSX2's GS thread combines work split between our GS front end and native renderer. Its GPU 2.07 ms
is **GPU execution**, not a CPU thread: subtracting it from our 8.979 ms native CPU time would be
invalid. Combining GS+native CPU is useful for total cost, but their overlapping CPU times must not
be summed as a critical-path/frame latency. Our warmed diagnostic runs are excluded from these
medians: callgraphs ran at 58.512 fields/s and counters at 56.530, with instrumentation and
different core/clock distribution.

## Waits and scheduling

Mean wall milliseconds per field from the same bench window:

| Run | ee_wait_vu | ee_wait_gs | ee_spin | ee_park | vu_idle | vu_gs_full | vu_spin | vu_park | gs_idle | gs_spin | gs_park |
|---|---|---|---|---|---|---|---|---|---|---|---|
| run1-s1 | 0.372 | 2.679 | 2.514 | 0.532 | 7.614 | 0.000 | 0.012 | 7.601 | 0.277 | 0.026 | 0.250 |
| run2-s2 | 0.386 | 2.747 | 2.548 | 0.580 | 7.557 | 0.000 | 0.012 | 7.544 | 0.646 | 0.102 | 0.537 |
| run3-s1 | 0.432 | 2.588 | 2.480 | 0.535 | 6.692 | 0.000 | 0.013 | 6.678 | 0.595 | 0.103 | 0.487 |
| run4-s2 | 0.444 | 6.778 | 5.768 | 1.449 | 6.406 | 0.000 | 0.014 | 6.390 | 0.501 | 0.081 | 0.416 |
| run5-s1 | 0.428 | 4.625 | 4.152 | 0.896 | 6.236 | 0.000 | 0.014 | 6.220 | 0.425 | 0.058 | 0.364 |

`ee_ring_full` rounded to 0.000 in every run. Spin and park are subsets of the queue waits, not
extra costs to add. Timeline 'busy' means wall time outside those specific queue waits; it still
contains descheduling, pacing and renderer locks. For example run1 GS is 16.455 ms outside `gs_idle`
but only 4.481 CPU ms: most of that interval is not GS computation. The return-site addresses in
Android logs are ASLR addresses, not usable ELF-relative offsets.

Native/sound lack equivalent timeline counters. `/proc/TID/schedstat` deltas over the interior
3-second-sampled window estimate their blocking instead. Below are medians across 1x runs,
normalised by the bench mean field time; endpoints are approximate, not exact bench boundaries:

| Thread | runnable but not scheduled ms/field | sleep/block ms/field |
|---|---|---|
| game | 0.235 | 6.512 |
| vu | 0.422 | 6.328 |
| gs | 0.660 | 11.105 |
| native | 0.760 | 6.930 |
| sound | 0.993 | 15.537 |

Runnable delay is scheduler contention; sleep/block includes timers, condition variables and driver
waits, which this measurement cannot separate. Sleeping itself is not CPU work. Ignore the
inherited, almost-idle extra thread also named `game`; the parser chooses the active TID.

## Clocks, temperature and placement

Phone plugged into USB, battery 100%; no phone performance or emulator setting changed. ADPF logs
'no performance hint session'. CPU frequency reads are cluster snapshots, including idle moments,
not each thread's instantaneous executing clock:

| Run | Battery C | pa_therm0 C | A78 median GHz | X1 median GHz |
|---|---|---|---|---|
| run1-s1 | 31-32 | 39.1-40.0 | 2.112 | 2.150 |
| run2-s2 | 33-33 | 41.1-42.2 | 2.112 | 2.150 |
| run3-s1 | 34-34 | 42.2-42.6 | 1.882 | 1.901 |
| run4-s2 | 35-35 | 43.7-44.5 | 1.882 | 1.728 |
| run5-s1 | 36-36 | 43.9-44.3 | 1.670 | 1.670 |

A78 snapshots span 1.555-2.112 GHz across diagnostic/normal windows; X1 snapshots include idle 0.845
GHz and peak 2.150 GHz. Nominal hardware maxima are higher. **thermal_zone0 is `pa_therm0`, not a
CPU sensor.** Separate discovery (`thermal-zones.txt`) found CPU sensors; callgraph capture:
`cpuss-0-usr` 56.5-59.5 C, `cpu-1-3-usr` 56.5-61.2 C, board `cpu_therm` 48.759-49.356 C, GPU sensor
`gpuss-0-usr` 51.1-52.4 C. GPU frequency sysfs reads were denied. Clocks changed as the session
warmed; do not attribute the run-to-run CPU increases solely to scale.

Counter-weighted big-core CPU shares appear below. Sampling independently puts the game on cores 4-7
for 97.93% of its CPU-clock weight (X1 core 7: 53.23%), VU 98.48%, GS 89.99%, native 96.67%. Short
migrations to A55 cores occurred; no claim of permanent pinning is made.

## Callgraphs and top functions

`simpleperf record --app app.downhillrevived -e cpu-clock -f 500 --call-graph dwarf,8192 --symfs
/data/local/tmp/andprof-symbols --duration 180 -o /data/local/tmp/andprof-perf.data`, started on the
first adb poll observing the bench start, stopped with the app. It recorded 26.205 s / 30,516
samples / **zero lost**. Start is up to about 3 s after the bench boundary; the capture covers the
same early-race interior including 22 s, not menus. Unstripped matching `libmain.so` was available
while unwinding. NDK binary-cache recovery also resolves SDL, and `llvm-cxxfilt` demangles Rust v0
names. Raw recording, symbol cache, analysis JSON and full callgraphs stay on F:. No kernel
restriction was changed.

Percentages below are **within each thread**, weighted by CPU-clock period. Inclusive includes
callees and overlaps other rows; do not add inclusive percentages. Game/VU/GS/native have
8,583/8,070/5,255/7,355 samples; sound has only **88**, so its percentages are coarse. Callchains
reach 41/27/25/46/16 frames respectively; fiber boundaries and inlining still limit attribution.

### game

| Function / role | self % | inclusive % | Source / identification |
|---|---|---|---|
| `idle_wait` (producer spin/park) | 17.59 | 17.91 | `port/hle/graphics_queue.c` |
| `func_0021B8A8`: EE 3D renderer / VU0 packet construction | 5.77 | 19.92 | handwritten; `config/handwritten.txt`, finding 019; no decompiled C |
| `render`: software sound mixer | 4.22 | 5.95 | `port/hle/sound.c` |
| `func_00213C08`: collision mesh BVH walk / sphere sweep | 3.59 | 8.77 | `src/unsorted/00213C08.c` (NONMATCHING counterpart; runtime uses recomp) |
| `run_fast_program`: EE inline-asm fast templates | 2.73 | 3.68 | `port/runtime/ps2asm_fast.h` |
| `ps2_vu0_run` | 2.53 | 7.32 | `port/runtime/vu0_interp.c` |
| `address`: emulated inline-asm effective address | 2.40 | 2.41 | `port/runtime/ps2asm.c` |
| `native0_transform` | 1.83 | 1.83 | `port/runtime/vu0_native.h` |
| `func_00213658`: swept-sphere/triangle contact tests | 1.61 | 2.60 | `src/unsorted/00213658.c` (NONMATCHING counterpart; runtime uses recomp) |

The `w2c_core_`/`rc_` prefixes are compiler wrappers, not additional game functions. Names above
come from `src/`, `config/handwritten.txt`, `config/pcport_xref.tsv` and finding 019. No known
retail C++ name exists for the three opaque `func_` entries; their descriptive roles are not
invented symbol names. Library/SDK `func_` entries remain retail code in the grouping below.

| Exclusive game-thread category | CPU sample % |
|---|---|
| recompiled game code | 34.72 |
| waits/spinning | 23.88 |
| system/driver/other | 2.90 |
| EE inline asm runtime | 10.33 |
| memory access helpers | 2.73 |
| module HLE/runtime/dispatch | 16.08 |
| VU0 | 7.04 |
| native HLE/runtime | 2.32 |

This is an exclusive **symbol-origin** grouping, not an instruction census. Inlined bounds/fold
checks stay charged to callers; a mixed recompiled renderer/VU0 routine stays in retail code. 'VU0'
is the separately identifiable VU0 runtime/native kernels. HLE includes the software sound mixer
(`render`), reverb/envelopes, DMA and host bridge; sound mixing runs on **game**, not the small host
sound-output thread. System samples with an identifiable wait ancestor are classified as
waits/spinning. Unidentified system work remains separate.

### vu

| Function | self % | inclusive % | Source |
|---|---|---|---|
| `native_geometry_loop` | 18.91 | 27.94 | `port/runtime/vu1_native_geometry.h` |
| `native_clip_run` | 13.59 | 15.87 | generated by `port/tools/vu1_native_clip.py`, used by `vu0_interp.c` |
| `unpack_stream` | 5.43 | 5.44 | `port/hle/vif.c` |
| `vif_feed` | 4.26 | 98.12 | `port/hle/vif.c` |
| `ps2_vu1_run` | 4.26 | 86.90 | `port/runtime/vu0_interp.c` |
| `graphics_gif_feed` | 3.94 | 11.51 | `port/hle/graphics_queue.c` |
| `native_geometry_1638` | 3.52 | 7.70 | `port/runtime/vu1_native_extra.h` |

### gs

| Function | self % | inclusive % | Source |
|---|---|---|---|
| `gs_write` | 27.95 | 57.24 | `port/gs/gs.c` |
| `native_draw` | 22.97 | 29.00 | `port/gs/native_core.inc` |
| `gs_feed` | 22.36 | 84.13 | `port/gs/gs.c` |
| `idle_wait` | 10.35 | 13.05 | `port/hle/graphics_queue.c` |
| `load_clut` | 3.71 | 3.86 | `port/gs/gs.c` |
| `gs_palette` | 3.04 | 3.04 | `port/gs/gs.c` |

### native

| Function / group | self % | inclusive % | Source |
|---|---|---|---|
| kernel (names restricted) | 49.80 | 49.80 | `[kernel.kallsyms]`; do not assign all of this to ioctl |
| `__memcpy` | 4.46 | 4.51 | bionic libc |
| `dhd_render::native::render::Native::render` | 1.21 | 4.30 | `port/render/src/native/render.rs` |
| `wgpu_core::command::render::encode_render_pass` | 1.16 | 45.10 | wgpu-core 29.0.3 `src/command/render.rs` |
| `dhdr_native_zread_take` (includes device poll) | 0.65 | 12.88 | `port/render/src/ffi.rs`, `native/zread.rs` |
| `wgpu_core::device::queue::Queue::submit` | 0.53 | 12.78 | wgpu-core 29.0.3 `src/device/queue.rs` |
| `qglinternal::vkQueueSubmit` | 0.54 | 8.86 | Adreno Vulkan driver |
| `sched_yield` | 0.48 | 8.54 | bionic libc; caller attribution available in callgraphs |
| `je_free` | 1.22 | 2.66 | bionic allocator |

Adreno's largest userspace symbols are obfuscated: one is 1.81% self / 4.96% inclusive and another
1.66% / 21.40%. They are retained with their real opaque names in the JSON/callgraph; no internal
driver-function name is inferred. Kernel symbols are restricted, so '49.80% kernel' does not prove a
particular ioctl or GPU execution duration.

### sound

| Function / group | self % | inclusive % | Source |
|---|---|---|---|
| kernel (names restricted) | 40.91 | 40.91 | `[kernel.kallsyms]` |
| `host_thread` (SDL output, not game mixer) | 23.86 | 100.00 | `port/host/sound_out_sdl.c` |
| `SDL_SYS_DelayNS` | 10.23 | 53.41 | SDL 3.4.18 `src/timer/unix/SDL_systimer.c` |
| `host_wall_seconds` | 4.55 | 6.82 | `port/host/host_posix.c` |
| `SDL_GetAudioQueueQueued` | 3.41 | 3.41 | SDL 3.4.18 `src/audio/SDL_audioqueue.c` |
| `SDL_GetAudioStreamQueued` | 2.27 | 12.50 | SDL 3.4.18 `src/audio/SDL_audiocvt.c` |

### Hardware counters

Separate same-window run: `simpleperf stat --app app.downhillrevived -e
cpu-cycles,instructions,cache-references,cache-misses,task-clock --per-thread --per-core --duration
25`. It starts at the first bench-start poll and samples 25.023 s of its interior. Counts are summed
over cores/TIDs of each named thread before forming ratios; the idle inherited `game` thread
contributes negligible work. Events are ungrouped; ratios are estimates over the same window, not
instruction-level paired samples. No unsupported-counter failure occurred.

| Thread | IPC | generic cache miss % | misses / 1000 instructions | CPU time on cores 4-7 % |
|---|---|---|---|---|
| game | 1.777 | 0.674 | 1.887 | 98.639 |
| vu | 2.568 | 0.712 | 2.072 | 99.015 |
| gs | 3.004 | 0.628 | 2.410 | 92.822 |
| native | 0.825 | 4.759 | 17.756 | 96.568 |
| sound | 0.756 | 3.796 | 13.002 | 87.529 |

These are the device's **generic cache events**, not identified LLC misses. Percent is misses /
references; MPKI is misses / instructions * 1000. PMU run/enable fractions and per-core counts are
retained in `stat-s1-stat.txt` / `stat-summary.json`. Native's lower IPC and higher misses include
kernel/driver and allocator activity; they do not diagnose one cache level by themselves.

## What explains the cost / gap (profile-supported hypotheses)

### game / EE: -0.703 ms median, so no measured positive CPU gap

- Producer spinning is still real cost: `idle_wait` 17.59% self, all identifiable waits/spinning
  23.88%; queue spin has a 1x median of 2.514 ms and reaches 4.152 ms in warmed run5. Reducing it might save CPU, but AND2 found
  a governor/wakeup regression without spin, so a zero-spin speedup cannot be assumed.
- The recompiled 3D renderer and mesh collision walk are 19.92% and 8.77% inclusive. Their mixed
  scalar/VU/MMI work and scratchpad/register handling differ from PCSX2's ARM JIT.
- EE asm templates (10.33% exclusive), VU0 kernels (7.04%) and explicit address helpers (2.73%)
  remain overhead despite AOT/NEON; inlined memory folding is hidden in retail callers.
- Software audio mixing is also here: `render` 4.22% self / 5.95% inclusive, plus reverb/envelopes.
  Different audio placement and the hand-ridden scene prevent interpreting the negative delta
  as a universal advantage over PCSX2 EE.

### vu / MTVU: +1.095 ms median

- Geometry loop 18.91% self / 27.94% inclusive is the largest kernel; remaining arithmetic,
  flags and temporary/register handling merit inspection even after AND3 NEON.
- Clip loop 13.59% / 15.87% and geometry subkernels consume a second substantial block; preserve
  VU arithmetic/flag exactness when changing their SIMD layout.
- VIF unpack (5.43% self) and GIF forwarding (3.94% / 11.51%) are separate packet/memory costs.
  includes VIF/forwarding work; PCSX2 MTVU has different responsibility boundaries.

### gs front end: 4.999 ms; only the combined graphics comparison has a positive gap

- `gs_feed` 22.36% self and `gs_write` 27.95% repeat front-end packet/register dispatch.
- `native_draw` 22.97% self / 29.00% inclusive converts front-end drawing into native commands.
- CLUT load/palette account for 3.71% + 3.04% self; texture/state processing remains CPU work at 1x.
- Queue spin is 10.35% self in the instrumented run, and GS also blocks outside the queue-idle
  counter. Its low normal CPU time is not proof that the whole graphics pipeline is inexpensive.

### native: combined gs+native is +5.318 ms median versus PCSX2 GS

- Render-pass encoding is 45.10% inclusive, traversing wgpu and Adreno. This extra CPU layer
  plus our separate front end is a plausible reason graphics total exceeds PCSX2 GS.
- Kernel work is 49.80% self; named `vkQueueSubmit` is 8.86% inclusive. Submission/driver work
  is prominent, although restricted symbols prevent assigning every kernel sample to it.
- Z-read take is 12.88% inclusive and calls `device.poll(Poll)`; `sched_yield` is 8.54% inclusive.
  The default probe is already asynchronous, so further savings concern polling/readback overhead,
  not replacing an exact synchronous probe as in old AND2/AND3 discussions.
- Copies and allocator churn are visible (`memcpy` 4.46%, `je_free` 1.22% self). Native cache MPKI
  17.756 versus VU 2.072 / GS 2.410 supports inspecting working set and allocation/binding reuse.

### sound: 0.207 ms median; no PCSX2 sound-thread baseline

- Stream queue queries and mutex/clock calls are visible; SDL queue access is 12.50% inclusive.
- Ring transfer/resampling is in `sound_out_sdl.c`; the game-thread mixer is charged elsewhere.
  At 88 samples these are cost locations, not a measured explanation of a nonexistent audio gap.

## Optimisation targets, ranked by expected phone CPU ms saved

These are engineering estimates from the sampled budgets, **not measured patch gains**; do not add
them as end-to-end frame savings. The normal runs are already paced near 60, so benefits would
primarily be less CPU/power, more thermal headroom and fewer late frames.

1. **Native submission/encoding and Z-read polling:** about **1-2.5 CPU ms/field**. Inspect render
   pass/binding/buffer reuse and device-poll cadence; `encode_render_pass` spans about 4.4 CPU ms
   of the warmed profile, Z-read take another about 1.25 ms inclusive (budgets may overlap).
2. **VU geometry/clip kernels and packet handling:** about **0.7-1.5 CPU ms/field**. The two main
   kernels are 32.5% self / roughly 4.8 ms inclusive combined in the warmed run; seek fewer
   instructions/temporary transfers while retaining all flags and rounding.
3. **EE renderer/collision asm fast paths and effective addresses:** about **0.5-1.2 CPU ms/field**.
   Focus on the named renderer/collision routines plus `run_fast_program`/`address`; their
   exclusive sampled budget is roughly 1.8 ms before separately identifiable VU0 kernels.

GS packet/native-draw specialisation is another plausible 0.5-1.5 CPU ms candidate; the data do not
establish that it saves more than the ranked items. A spin-policy experiment might save CPU but is
not ranked as a throughput gain without a controlled governor-aware phone A/B.

## Limits, artifacts and sources

- Same level, phone, renderer family and nominal 1x; different rider route/camera and timing
  whole-window percentiles/max are not interchangeable; a longer window naturally exposes more spikes.
- The normal port uses deferred two-field pipelining and finding 097's native depth probe (a
  previous-frame answer). It is not an exact renderer run. No new exactness/state-trace claim is
  made here; no accuracy or matching checks were weakened for these measurements.
- Only PCSX2 Vulkan/1x/MTVU are known. Instant VU1, EE cycle/skip, clamping/rounding, hardware
  download/texture/blending presets and fork-specific defaults/speed hacks were **not verified**.
  Do not credit an assumed default speed hack with a measured millisecond delta.
- GPU timestamps are unavailable in this renderer path (`gpu_rust_shared.inc` writes -1); GPU
  clocks are permission denied. Restricted kernel names and obfuscated driver internals limit
  attribution. No fresh PCSX2 callgraphs or counters exist; its numbers are user supplied.
- Normal timing and diagnostic data are kept separate. Temperature/clock drift and tracing
  overhead preclude interpreting the 58.5/56.5 diagnostic rates as the normal build's rate.

`raw/run{1..5}-*.log` are the authoritative bench/timeline/audio records; matching `*.ini` and
`*-samples.json` retain settings, clocks, temperature and schedstat. `raw/profile-s1-perf.data`
holds callchains; `profile-s1-perf.json`, `profile-analysis.txt` and `callgraphs.txt` contain
reports. `raw/stat-s1-stat.txt` / `stat-summary.json` retain counters. `raw/summary.json`,
`environment-summary.json`, `build-manifest.json`, `source-manifest.json` and `restoration.json` tie
the results together. `binary_cache/` has matching symbols/system libraries; `scripts/` has build,
measurement, analysis and restoration scripts. Invalid startup attempts and timeline CSV error text
are retained as such, not counted. No APK, raw recording, game data, game-memory trace, generated
assembly or symbol cache is committed.

Method references (no external implementation code copied):

- [AOSP simpleperf documentation](https://android.googlesource.com/platform/system/extras/+/main/simpleperf/doc/):
  DWARF collection, symbol recovery, `--symfs`, counters and kernel restrictions.
- [AOSP report-library reference](https://android.googlesource.com/platform/system/extras/+/main/simpleperf/doc/scripts_reference.md):
  CPU-clock periods and callchain parsing with the NDK's report library.
- [PCSX2 performance metrics source](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/PerformanceMetrics.cpp):
  upstream EE/GS/VU CPU-time deltas divided by frames; GPU time tracked separately. Treat the
  user's unverified Android fork as following that definition, not as independently audited.
- [PCSX2 OSD/performance documentation](https://pcsx2.net/docs/troubleshooting/performance/):
  speed-hack and renderer settings which would be needed for a tighter comparison.
- Local findings 089, 090, 091 (AND1/AND2/AND3) and 097; `bench_race.py`, `race_compare.py`,
  `graphics_timeline.h`, `present.c`, source/symbol/config files cited above.

## Uncapped runs (headroom)

Order u1-u4, alternating scales; the phone was at 44.9 C (thermal_zone0) after the last run.

| Run | Scale | fields/s | p50 / p99 / max ms | busy ms/field game / vu / gs | CPU ms/field game / vu / gs / native | ee_wait_gs |
|---|---|---|---|---|---|---|
| u1-s1 | 1 | 102.7 | 9.27 / 15.6 / 35.2 | 5.94 / 8.53 / 9.23 | 9.25 / 8.40 / 5.35 / 7.06 | 3.40 |
| u2-s2 | 2 | 77.6 | 12.6 / 19.7 / 42.8 | 5.96 / 9.39 / 12.49 | 11.57 / 9.23 / 5.03 / 7.63 | |
| u3-s1 | 1 | 98.0 | 9.75 / 16.6 / 37.6 | 6.69 / 9.24 / 9.63 | 9.78 / 9.01 / 5.96 / 7.51 | |
| u4-s2 | 2 | 71.3 | 14.0 / 19.9 / 47.9 | 6.75 / 10.56 / 13.61 | 12.70 / 10.37 / 5.94 / 8.41 | |

- Headroom: about 100 fields/s at 1x and 71-78 at the default scale 2, against 60 needed.
- The limit is the GS -> renderer end, not the game or VU1: the game thread's own work is about
  6 ms/field and it waits 3.4 ms/field on the GS queue (u1). The GS thread is busy 9-14 ms/field
  of wall time but uses only 5-6 ms of CPU, so most of that time it waits on the native render
  thread or the GPU. Its busy time grows with the scale (9.2-9.6 at 1x, 12.5-13.6 at 2x), which
  points at GPU or present cost at 2x. The CPU ms of the game thread include its spin.
- Each run still has one 35-48 ms field and a 55-63 ms present interval (see the paced runs).
- Unpaced runs skip audio to keep up (`skipped_ms`), so they say nothing about the underrun.
