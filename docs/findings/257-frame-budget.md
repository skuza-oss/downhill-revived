# 257: where a frame goes now on the throttled phone (0.8.6)

Status: current

Branch `perf/profile-257` from `main` ca519dde (release 0.8.6), started 2026-11-15. A measuring
lane: no code change. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

After the speed lanes (findings 195, 247, 249, 253, 256): on the throttled Mi 11i
(`phone_throttle.py --apply t612`), Native VU on, finding 156's route, the pipeline list of an
earlier run in use, at 4x (where the GPU limits) and at 1x (where the CPU does): the time a field
of every thread and of the stages inside it, which one limits, the VU thread's split (finding
253's setup programs, the translated batches, the near-bound clipper, waiting), every stage over
about 3 % of a field with an idea and a size, and a verdict: is any one change worth a lane (about
5 % or more), or are we at small steps.

Acceptance: the numbers below, each marked measured or guess; `check.py docs`.

Out of scope: changing code.

## Progress

- [x] 2026-11-15: six runs of the 0.8.6 release APK as players have it (no test build): at 4x and
  at 1x one run with `DHD_VU1_PROFILE` and `DHD_VU1_EMULATED`, one clean run, one with simpleperf
- [x] the budget, the VU thread's split, the leftovers
- [ ] the lead: Status, and what the Redmi 9 must answer (below)

## How it was measured

The 0.8.6 release APK (versionCode 806, the profile-guided build) on the Mi 11i with the `t612`
profile, `DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1`, `DHD_BENCH=300-14000`, `DHD_TIMELINE`, 60 s
cool-down before each run, battery 29 to 32 degrees C. The race is fields 6321-14000 (start
6400-7300, mid-race 9000-14000). Thread times are the timeline's (CPU ms and waits a field);
stages are simpleperf (`cpu-clock`, 1 kHz, frame-pointer call stacks, the whole process) symbolised
with the release's own unstripped `libmain.debug.so`, counted over the same fields. The stages of
the VU thread's emulated and translated runs are `DHD_VU1_PROFILE` (time per program and entry,
host clock around each run; it adds about 0.7 ms a field to the VU thread, so its shares are used,
not its absolute times). The simpleperf run is 0.1 to 1.5 % slower than the clean one.

Whole route, fields a second: 4x 69.33 (clean), 69.14 (simpleperf), 69.36 (VU profile); 1x 91.61,
90.09, 89.16.

## The budget: race, ms a field

| | 4x | 1x |
|---|---|---|
| field (fields a second) | **15.31 (65.3)** | **11.71 (85.4)** |
| game thread: CPU / busy / waiting for the GS | 9.18 / 10.65 / 4.66 | 9.78 / **11.40** / 0.31 |
| . the game's own code (recompiled EE) | 5.67 | 6.02 |
| . VU0 (macro calls, micro programs) | 1.75 | 1.88 |
| . core helpers (address, render, DMA and the like) and libc | 1.42 | 1.51 |
| . HLE graphics feed | 0.14 | 0.15 |
| . spinning while it waits (`DHD_SPIN_US`) | 2.66 | 0.18 |
| VU thread: CPU / busy / idle | 8.82 / 9.82 / 5.49 | 9.13 / 10.53 / 1.17 |
| GS thread (the GS front end): CPU / busy | 5.55 / 15.20 (blocked on the render thread) | 6.45 / 5.56 |
| . the native list building (`native_*`) | 2.49 | 2.80 |
| . GS state, page marks, writes, CLUT (`gs_*`) | 2.07 | 2.24 |
| . copies | 0.90 | 0.99 |
| render thread: CPU / waiting for a field / present wait | 8.91 / 0.03 / 2.15 | 8.07 / 2.87 / 1.78 |
| . encoding passes (wgpu and driver) | 1.83 | 1.70 |
| . our own prepare (`render.rs`) | 1.47 | 1.40 |
| . wgpu maintain, command pool reset | 1.30 | 1.34 |
| . submit (wgpu and driver) | 1.15 | 1.00 |
| . present (acquire, scanout pass, queue present) | 1.04 | 0.71 |
| . uploads (`write_buffer`, `write_texture`) | 0.84 | 0.79 |
| . other driver and libc | 1.06 | 1.01 |
| encode thread (`dhd-encode`) | 1.75 | 1.64 |
| GPU: span of a picture (timestamps) | **13.74** | 3.93 |

(Rows with a dot are simpleperf's split of the thread above it; the thread rows are the
timeline's.)

**At 4x the GPU limits**: its span is 13.74 ms of a 15.31 ms field (90 %), the render thread never
waits for a field (0.03 ms) and waits 2.15 ms on the swapchain, the GS thread is blocked behind it
(busy 15.2 ms on 5.6 ms of CPU) and the game thread waits 4.66 ms for the GS. Race start and
mid-race alike (GPU 17.50 ms of 19.06; 13.01 of 14.55).

**At 1x the game thread limits**: busy 11.40 of 11.71 ms (97 %), it waits 0.31 ms. The VU thread is
next, 1.17 ms behind (busy 10.53 ms). The GPU is at a third (3.93 ms), the render thread waits
2.87 ms a field for work. At the race start both are at the limit (game 12.27, VU 11.98 of 13.41).
The game thread's busy time is 1.4 ms more than its CPU time: time it is ready but not running
(the profile keeps two big cores, capped, and four little ones for five busy threads, so at least
three of them share or run on little cores) or blocked outside the queue.

Top functions (self time, 1x race, ms a field):

| game thread | | VU thread | | GS thread | | render thread | |
|---|---|---|---|---|---|---|---|
| `func_0021B8A8` (the scene walk, finding 162) | 0.96 | `run_micro` (translated batches, inlined) | 3.65 | `native_mesh` | 0.85 | `__memcpy` | 0.97 |
| `ps2_vu0_run` (VU0 interpreter) | 0.52 | `vif_feed` (VIF unpack) | 1.76 | `__memcpy` | 0.81 | Adreno driver (one symbol) | 0.33 |
| `func_00213C08` | 0.42 | `ps2_vu1_run` (VU1 interpreter) | 0.95 | `gs_feed` | 0.69 | `Native::render` | 0.33 |
| `native0_transform` (VU0 native) | 0.41 | kernel wake/park (`consume`'s futex) | 0.94 | `native_state` | 0.60 | kernel spin unlock | 0.23 |
| `run_fast_program` (VU0) | 0.40 | `__memcpy` | 0.37 | `native_draw` | 0.56 | `@plt` | 0.18 |
| `__memcpy` | 0.28 | `aot_block_555` (VU1 AOT) | 0.35 | `native_close` | 0.46 | Adreno driver (another) | 0.17 |
| `func_002628A0` | 0.27 | `aot_block_50` | 0.13 | `gs_consume` | 0.42 | `je_free` | 0.16 |
| `vu0c_run` (VU0 compiled) | 0.27 | `aot_block_564` | 0.12 | `gs_mark_pages` | 0.42 | wgpu drop glue | 0.15 |
| `address` (core helper) | 0.25 | atomics | 0.11 | `load_clut` | 0.28 | wgpu `Queue::submit` | 0.14 |
| `render` (core helper) | 0.22 | `memset` | 0.11 | `gs_write_pixels` | 0.22 | Adreno driver (another) | 0.13 |

At 4x the same names in the same order; the game thread adds `idle_wait` (2.65 ms, the spin) and
the VU thread has no kernel wake time (0.08 ms: the GS thread never parks there).

## The VU thread's 9.1 ms (1x race)

| Part | ms a field | How |
|---|---|---|
| translated batches (`run_micro`, the mesh path inlined: decode, transform, clipper, the item) | 3.65 | measured (simpleperf) |
| . of it `mesh_build` (the per-vertex loop: transform, colour, the item's words) | 2.98 | measured (line tables) |
| . of it the near-bound clipper (`mesh_clip_triangle`, `mesh_clip_plane`; with the per-vertex `mesh_clip_flags` 0.11) | 0.06 | measured (line tables) |
| VIF unpack (`vif_feed`) and copies | 2.34 | measured |
| emulated VU1 runs (AOT blocks 1.08 + interpreter 0.95) | 2.03 | measured |
| . setup programs that kick nothing (finding 253's list) | about 0.95 | measured share (below) |
| . drawing programs not yet translated (overlay A, overlay B 0030, 0050, 06E0, 3258, the four VI13 loops) | about 1.08 | measured share |
| parking and waking (the futex in `consume`) | 1.04 | measured |
| idle (no packet) | 1.17 of wall time | measured (timeline) |

`DHD_VU1_PROFILE` over the race (1x): of the VU programs' time, translated batches and their
continuations 65 %, emulated drawing programs 19 %, emulated setup programs 17 %. 410 setup runs a
field (the largest: 0040 47 runs at 9.8 us, 0020 10 at 16.7 us, then 0078, 06F0, 07F0 at about
1.5 us each); 1,084 translated batches a field at 1.6 to 5.0 us each (0170 450 a field, 1.51 ms;
13A0 0.58; 08B0 0.42; 1708 0.28; 1110 0.22).

**If the setup programs became C**: the most they could save is their own run time, about 0.95 ms
a field (their measured share of the emulated time; the VIF unpack of their data stays). At 1x
that does not raise the frame rate: the game thread limits and the VU thread already idles 1.17
ms. It frees a core's time. It would matter only where the VU thread is the limit (the race start
at 1x, where both threads are level; or a phone whose two fast cores carry the game and the VU
thread together).

## The leftovers, ranked

Shares of the 1x field (11.71 ms) unless named; "measured" is the stage's time, the gain is a guess
unless it says otherwise.

| Stage | ms a field (share) | Idea | Gain (guess) |
|---|---|---|---|
| 4x: the GPU's draw passes | 13.74 span of 15.31 (90 % at 4x) | fewer mesh vertices: faces the VU drops by ADC or culls reach the GPU as meshes (finding 249, hypothesis 3) | 1-3 % at 4x |
| 4x: the frame in fewer passes (`lane247-pass-order`, parked) | | already built | +1.1 % race at 4x (measured, finding 247) |
| game code (recompiled EE) | 6.02 (51 %) | the scene walk `func_0021B8A8` (0.96) and its callees are the top; a native scene walk is finding 162's deep hook | large but a big lane; capped at about 10 % by the VU thread behind it |
| VU0 on the game thread | 1.88 (16 %) | `ps2_vu0_run` (0.52) runs programs on the interpreter: compile or write natives for the ones it still meets | 0.3-0.5 ms, 3-4 % at 1x |
| translated batches on the VU thread | 3.65 (31 %) | `mesh_build` is 2.98 of it (NEON for its per-vertex loop); not the limit at 1x (1.2 ms behind), so it pays only once the game thread is cut | 0 at 1x now |
| emulated VU1 runs | 2.03 (17 %) | setup programs in C (0.95), the untranslated drawing programs (1.08) | 0 at 1x now (VU not the limit); frees up to 2 ms of a core |
| VIF unpack and copies on the VU thread | 2.34 (20 %) | unpack straight into the mesh path's input for translated batches | 0 at 1x now |
| GS front end | 6.45 (55 %, its own thread) | not the limit at either scale | 0 |
| render thread | 8.07 (69 %, its own thread) | waits 2.9 ms at 1x; at 4x it waits for the GPU | 0 |
| game thread ready but not running | 1.4 (12 %) | five busy threads on two big cores: which thread runs on a little core decides; pinning the game thread to a big core | unknown; the Redmi 9 decides |

## Verdict

Small steps on this stand-in. At 4x the GPU limits (90 % busy) and the only measured GPU change is
the parked pass order (+1.1 %). At 1x the game thread limits and the VU thread is 1.2 ms (10 %)
behind it: no single change reaches 5 % unless it cuts the game thread by 0.6 ms or more; the
nearest is the VU0 work still on the interpreter (0.5 ms, guess 3-4 %), and the setup programs in C
(0.95 ms of VU thread) raise nothing here. The whole process uses about 37 ms of CPU a 11.7 ms
field at 1x (five busy threads).

What the Redmi 9 (Mali-G52, two fast cores) must answer, which this stand-in cannot: whether the
CPU total, not one thread, limits there (then every thread's time counts, the setup programs and
the VIF unpack included); and whether its GPU limits at 2x as the Adreno does at 4x.

## Decisions

- 2026-11-15: the release APK as players have it, no test build: its timing lines, `DHD_VU1_PROFILE`
  and `DHD_VU1_EMULATED` and simpleperf with the release's own debug library answer the questions.

## Surprises

- The release's `libmain.debug.so` has no C line tables, so simpleperf names functions but not the
  inlined parts of `run_micro`. A rebuild of ca519dde with `build_host64.py --release --debug` and
  the same profile has the same function addresses; its line tables split the release's samples
  (`llvm-addr2line -i`). Nothing of that build went on the phone.
- The near-bound clipper is 0.06 ms a field: finding 253 moved 9 % of the race's batches to it for
  nearly nothing. The VU thread's cost is `mesh_build`, 2.98 ms.
- The VIF unpack's 1.76 ms is `stream_wl1` 0.42, `stream_form` 0.32, the command loop 0.48; the
  emulated runs' interpreter part (0.95 ms) holds the older native kernels too
  (`native_particles_2e48` 0.14, `native_skin_468`, `native_particles_0e10`).
- At 1x the VU thread spends 0.94 ms a field in the kernel waking from its park (`consume`'s
  futex): the GS thread parks there too. At 4x neither parks.
- `DHD_VU1_PROFILE` at 4x wrote an empty file (the 1x run's is complete); the split is from 1x.

## Result

To be filled at the merge.
