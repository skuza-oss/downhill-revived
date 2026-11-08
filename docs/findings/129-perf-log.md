# 129: perf: lines, how fast the game ran and what held it back, in every dhd.log

Status: current
Checked: 2026-11-04

Why: a player on an i7-7500U (2 cores, 4 threads) with a GeForce 920M sent two 0.7.6 logs, at
scale 2 and 3, both "low fps". They had the settings, the GPU and its driver and the exit's
`profile native:` counts, but no frame rate, no frame times and no per-thread or GPU time, so
nobody could tell whether the CPU or the GPU held the game back. The project's rule is that what
the game can detect goes in dhd.log, so a report can be read without asking the player anything.

Branch `feat/perf-log` (from 0.7.6). Every player gets the lines below; there is no setting.
`DHD_PERF_LOG=0` turns all of it off (the counting too). Code: `port/gs/perf_log.c`
(`perf_log.h`), with small hooks in the threads that wait or present, and the GPU half in Rust
(`gputimes` in `port/render/src/native/pipelines.rs`, `dhdr_perf_read` in `device.rs`).

## What is logged

At start (once; the `gs:` lines come from the renderer when it makes its device and surface):

    gs: GPUs: NVIDIA GeForce RTX 3070 (discrete), AMD Radeon(TM) Graphics (integrated), Microsoft Basic Render Driver (cpu (software)); the game runs on NVIDIA GeForce RTX 3070 (discrete)
    gs: present mode Mailbox (V-Sync on; the surface offers [Mailbox, Fifo, Immediate])
    perf: a line every 30 s of race or menus with the window in front, and the whole session at exit: ... (a legend, finding 129)
    perf: render native scale=4 picture=2048x1792 window=1920x1080 shown=1440x1080 | fps=60 pacing=just-in-time vsync=on present_mode=mailbox display=143.86 Hz | gpu=discrete gpu_timer=on

- `gs: GPUs`: every adapter of the backend in use with its kind (integrated, discrete, virtual,
  cpu), and the one the game runs on. When a discrete GPU is there but not used, the line says so
  and, on Windows, where to change it (Settings > System > Display > Graphics). On a laptop with
  two GPUs this is the first thing to read: the game takes DXGI's first adapter (Windows' choice
  for the program), which is often the integrated one.
- `gs: present mode`: what the surface took (Fifo waits for the display; Mailbox replaces a
  waiting picture; Immediate tears), whether V-Sync was asked for, and what the surface offers.
- `perf: render`: logged when any of its values changes and has then held for 2 s (so the boot's
  640-wide title and a window being dragged give no burst of lines): the renderer, its scale, the
  picture's size in pixels (the internal resolution: the displayed picture at that scale), the
  window's drawable size, the part of it the picture fills, the requested rate (`uncapped` with
  `DHD_FRAME_CLOCK=1`), the pacing (`just-in-time`, `wait before VBLANK` in online races and with
  `DHD_INTERP`, `unpaced`), the V-Sync setting, the present mode, the display's refresh, the GPU's
  kind and whether its timer is `on`, `off` or `not offered by this GPU`.

Then, after every 30 s of one kind of play (race or menus) with the window in front, and at exit
for the whole run (one line per kind that had frames):

    perf: race 30 s frames=1800 fps=59.9/60 low1%=59.2 over=0.0%(>20.0ms) max=17.1ms | ms/field cpu/busy game=4.40/3.54 vu=3.70/3.69 gs=3.15/2.65 render=2.19/1.21 present_wait=0.33 | gpu_ms/frame=2.36 | limit=none
    perf: session race 125 s frames=7519 fps=59.8/60 low1%=52.2 over=0.0%(>20.0ms) max=84.5ms | ms/field cpu/busy game=3.99/3.13 vu=3.32/3.34 gs=2.96/2.66 render=2.19/1.21 present_wait=0.34 | gpu_ms/frame=2.27 | limit=none

| field | what |
|---|---|
| `race` / `menus` | race = a race being ridden (the game's riding flag, `D_0029E290`); menus = everything else, loads and results too |
| `30 s` | wall time counted (window in front) |
| `frames` | pictures presented and counted |
| `fps=A/R` | A: frames over the summed time between them; R: the requested rate (`uncapped` when unpaced) |
| `low1%` | the slowest hundredth of frames as fps (1000 / their mean ms; the bench tools' `worst1pct`) |
| `over` | frames longer than 1.2 times the frame budget (20.0 ms at 60, 8.3 at 144), and that threshold; `n/a` when unpaced |
| `max` | the longest frame |
| `cpu/busy` per thread | ms per field: the thread's CPU time from the OS / its busy time, the wall time it did not spend in a measured wait |
| `render` | the render thread: CPU / busy, busy without its time inside the present |
| `present_wait` | ms per field the render thread spent inside the present (the surface's acquire and present call) |
| `gpu_ms/frame` | GPU time per frame: the sum of the native renderer's passes, from timestamp queries |
| `limit` | `gpu`, `cpu:<thread>`, `vsync`, `none` (holding the target) or `n/a`; the rule is below |

A 1x run on this PC (RTX 3070; hidden desktop, 960x720 window, V-Sync on, 60):

    perf: render native scale=1 picture=512x448 window=960x720 shown=960x720 | fps=60 pacing=just-in-time vsync=on present_mode=mailbox display=143.86 Hz | gpu=discrete gpu_timer=on
    perf: race 30 s frames=1778 fps=59.2/60 low1%=25.1 over=0.5%(>20.0ms) max=149.2ms | ms/field cpu/busy game=3.21/2.01 vu=3.05/3.08 gs=2.96/2.69 render=2.08/1.14 present_wait=0.34 | gpu_ms/frame=0.98 | limit=none
    perf: race 30 s frames=1799 fps=59.9/60 low1%=53.1 over=0.2%(>20.0ms) max=33.4ms | ms/field cpu/busy game=4.59/3.72 vu=3.88/3.80 gs=3.09/2.75 render=2.47/1.29 present_wait=0.36 | gpu_ms/frame=1.22 | limit=none
    perf: race 30 s frames=1800 fps=59.9/60 low1%=58.3 over=0.0%(>20.0ms) max=19.8ms | ms/field cpu/busy game=4.21/3.34 vu=3.04/3.02 gs=2.32/2.13 render=2.34/1.28 present_wait=0.36 | gpu_ms/frame=0.44 | limit=none
    perf: race 30 s frames=1800 fps=59.9/60 low1%=59.3 over=0.0%(>20.0ms) max=17.0ms | ms/field cpu/busy game=4.64/3.94 vu=3.82/3.82 gs=3.62/3.33 render=2.41/1.40 present_wait=0.36 | gpu_ms/frame=0.89 | limit=none
    perf: session race 125 s frames=7506 fps=59.7/60 low1%=44.1 over=0.1%(>20.0ms) max=149.1ms | ms/field cpu/busy game=4.16/3.26 vu=3.46/3.45 gs=3.00/2.73 render=2.31/1.28 present_wait=0.35 | gpu_ms/frame=0.88 | limit=none
    perf: session menus 23 s frames=1422 fps=59.4/60 low1%=33.7 over=0.6%(>20.0ms) max=50.0ms | ms/field cpu/busy game=1.82/1.19 vu=4.35/4.29 gs=2.92/2.61 render=1.54/0.51 present_wait=0.40 | gpu_ms/frame=0.66 | limit=none

(The first race window holds the race's start: its 149 ms frame and low 1% of 25 are the load into
the race. The test route's menus last 23 s, so there is no 30 s menus line, only the session's.)

4x, 1920x1080 window, otherwise the same: the GPU's time grows from about 0.9 to about 2.3 ms a
frame, nothing else moves, the target holds:

    perf: render native scale=4 picture=2048x1792 window=1920x1080 shown=1440x1080 | fps=60 pacing=just-in-time vsync=on present_mode=mailbox display=143.86 Hz | gpu=discrete gpu_timer=on
    perf: race 30 s frames=1791 fps=59.6/60 low1%=37.4 over=0.2%(>20.0ms) max=84.5ms | ms/field cpu/busy game=3.04/1.84 vu=2.92/2.92 gs=2.83/2.57 render=1.93/1.12 present_wait=0.34 | gpu_ms/frame=2.60 | limit=none
    perf: race 30 s frames=1800 fps=59.9/60 low1%=59.2 over=0.0%(>20.0ms) max=17.1ms | ms/field cpu/busy game=4.40/3.54 vu=3.70/3.69 gs=3.15/2.65 render=2.19/1.21 present_wait=0.33 | gpu_ms/frame=2.36 | limit=none
    perf: race 30 s frames=1800 fps=59.9/60 low1%=59.1 over=0.0%(>20.0ms) max=17.6ms | ms/field cpu/busy game=4.00/3.29 vu=2.93/2.94 gs=2.27/2.08 render=2.35/1.21 present_wait=0.35 | gpu_ms/frame=1.98 | limit=none
    perf: race 30 s frames=1800 fps=59.9/60 low1%=59.3 over=0.0%(>20.0ms) max=17.0ms | ms/field cpu/busy game=4.52/3.82 vu=3.66/3.73 gs=3.59/3.29 render=2.31/1.32 present_wait=0.35 | gpu_ms/frame=2.12 | limit=none
    perf: session race 125 s frames=7519 fps=59.8/60 low1%=52.2 over=0.0%(>20.0ms) max=84.5ms | ms/field cpu/busy game=3.99/3.13 vu=3.32/3.34 gs=2.96/2.66 render=2.19/1.21 present_wait=0.34 | gpu_ms/frame=2.27 | limit=none
    perf: session menus 23 s frames=1422 fps=59.4/60 low1%=32.2 over=0.6%(>20.0ms) max=50.0ms | ms/field cpu/busy game=1.71/1.09 vu=4.08/4.08 gs=2.79/2.51 render=1.49/0.50 present_wait=0.32 | gpu_ms/frame=2.00 | limit=none

Uncapped runs give the other verdicts (`DHD_FRAME_CLOCK=1 DHD_VSYNC=0`). 4x: the game (EE) thread
is the limit at about 250 fields/s, which is finding 108's picture of the EE chain on a fast GPU:

    perf: race 30 s frames=6533 fps=217.3/uncapped low1%=90.1 over=n/a max=31.9ms | ms/field cpu/busy game=3.70/3.30 vu=2.84/2.94 gs=2.28/2.15 render=1.72/1.40 present_wait=0.32 | gpu_ms/frame=1.06 | limit=cpu:game
    perf: race 30 s frames=7867 fps=261.7/uncapped low1%=118.7 over=n/a max=20.0ms | ms/field cpu/busy game=2.97/2.44 vu=2.26/2.28 gs=1.62/1.45 render=1.73/1.39 present_wait=0.33 | gpu_ms/frame=0.87 | limit=cpu:game

8x (a 4096x3584 picture), where the GPU's 2.7 ms are the largest part of a 3.5-3.7 ms frame:

    perf: render native scale=8 picture=4096x3584 window=1920x1080 shown=1440x1080 | fps=uncapped pacing=unpaced vsync=off present_mode=immediate display=143.86 Hz | gpu=discrete gpu_timer=on
    perf: race 30 s frames=8332 fps=277.0/uncapped low1%=100.8 over=n/a max=80.9ms | ms/field cpu/busy game=3.25/2.68 vu=3.08/3.11 gs=2.79/2.73 render=1.60/1.28 present_wait=0.94 | gpu_ms/frame=2.68 | limit=cpu:vu
    perf: race 30 s frames=8075 fps=268.6/uncapped low1%=148.8 over=n/a max=8.5ms | ms/field cpu/busy game=3.23/2.57 vu=2.63/2.63 gs=2.05/1.84 render=1.45/1.20 present_wait=0.43 | gpu_ms/frame=2.68 | limit=gpu
    perf: session race 113 s frames=31903 fps=280.3/uncapped low1%=127.8 over=n/a max=80.9ms | ms/field cpu/busy game=3.06/2.38 vu=2.55/2.56 gs=2.01/1.84 render=1.47/1.19 present_wait=0.55 | gpu_ms/frame=2.65 | limit=gpu

An earlier 8x run of the same build (while the PC's GPU was busier) showed the case the present
wait is for: 146.5 fps, GPU 5.71 ms of a 6.8 ms frame, the render thread 4.84 ms inside the
present; the first version of the rule called it `vsync` because 146.5 is within 3% of the
display's 143.86 Hz, although the present mode was Immediate. Step 3 below now calls it `gpu`.

The exact renderer (`DHD_RENDER=exact`, the reference, not the players' default) has no render
thread and no GPU timer, so those fields are `n/a`; it presents on the game thread, whose time in
the present counts as busy there:

    perf: render exact scale=1 picture=512x448 window=960x720 shown=n/a | fps=60 pacing=just-in-time vsync=on present_mode=mailbox display=143.85 Hz | gpu=discrete gpu_timer=off
    perf: race 30 s frames=1529 fps=50.5/60 low1%=28.9 over=42.2%(>20.0ms) max=36.5ms | ms/field cpu/busy game=6.84/13.92 vu=3.77/3.96 gs=5.15/5.19 render=n/a present_wait=n/a | gpu_ms/frame=n/a | limit=cpu:game

## How each value is measured

Nothing new times the game where something already did: the waits are DHD_TIMELINE's counters,
the GPU time is DHD_GPU_TIMES' timestamps, the render-side split is DHD_RSTAT's wait/present
parts, the CPU times are the bench's `host_thread_cpu`.

- **Frames.** `perf_presented` (in `counter_present`, `gpu_rust.c`: every picture shown, by the
  native renderer's render thread or the exact renderer's game thread) reads the clock once and
  counts the time since the last picture in a histogram (0.25 ms bins to 250 ms, then a tail with
  its sum). fps, `low1%`, `over` and `max` come from it. Frames count only when this picture and
  the last one were both shown in front and in the same kind of play.
- **Segments.** Once a second (or when the kind of play or the window changes) the open segment is
  closed: the threads' CPU times (`host_thread_cpu`: GetThreadTimes on Windows, Mach thread times
  on macOS, the thread CPU clocks on Linux and Android), the wait counters and the GPU totals are
  read, and their change goes to the segment's kind of play. A segment in which the window changed
  (focus, minimised) is dropped: it may hold the time in the background. A kind of play with 30 s
  counted writes its line and starts again; the session sums never reset.
- **In front**: SDL's window has the input focus and is not minimised, hidden or occluded
  (`present.c`, on every window event). With `DHD_TEST_INPUT` (test runs on a hidden desktop,
  whose window never gets the focus) shown is enough.
- **Fields**: `kernel_vsync_count` (the host's copy of the game's field), so per field is per
  simulation step: 60 a second at 60, 144 at 144.
- **Busy** = the segment's wall time minus the thread's measured waits:
  - game: waiting for VU1 and the GS worker, for room in the VU ring (`graphics_queue.c`'s
    timeline counters, now counted by default: one performance-counter read at each end of a wait,
    none in the spin loops), and its pacing waits (`host_wait_wall`, which adds its time to a
    per-thread sink the game and render threads register);
  - vu: idle (empty ring) and a full GS ring;
  - gs: idle, and the handoff to the render thread (`gs_native_vsync_handed` waiting for a free
    frame; timed only when it waits);
  - render: waiting for the next field (`native_render_next`), its pacing hold (`pacing_present`
    through `host_wait_wall`), and separately `present_wait`: `native_present_draw` (the surface's
    acquire) and the present call, which is where the GPU, the display or the compositor hold the
    thread back.
  Waits the code does not measure (a lock, a disc read, a page fault, the OS running another
  thread) count as busy; so busy can exceed the CPU time (descheduled: a 2-core PC shows this),
  and CPU can exceed busy (spinning in a measured wait).
- **GPU time**: the native renderer's passes already carry timestamp queries for DHD_GPU_TIMES
  (draws, surface passes, conversions, scanout, the present). Without that variable one frame in
  8 is now timed (the other seven frames' passes write no queries), resolved and read back
  without a wait, with no log line; `dhdr_perf_read` gives the totals. The device asks for
  `TIMESTAMP_QUERY` where the adapter offers it (DX12, Vulkan, Metal). `gpu_ms/frame` is the sum
  of a timed frame's passes (busy time, not the span from its first pass to its last, which
  includes the gaps while the CPU records; the span is kept in `dhdr_perf_read` too). A picture
  that was never presented (a minimised window draws but does not present) has its queries
  dropped at the next picture, and that frame goes untimed. Not counted: the exact renderer's
  Z-probe work for the lens flare (a small compute pass) and anything outside the native passes.
- **The render line** reads the scale and sizes `native_present` passes in (`perf_render_size`),
  the pacing from `pacing_handed` (a due time means just-in-time), the present mode the surface
  took and the adapter's kind from Rust, the display's refresh from SDL (`SDL_GetCurrentDisplayMode`
  of the window's display, on window events).

## The limit rule

T is the mean frame time (1000 / fps), B the frame budget (1000 / the requested rate; T when
unpaced). In order:

1. paced and at least 97% of the requested rate: `none`;
2. the largest of the GPU's ms per frame and each thread's busy ms per field is 85% of B or more:
   `gpu` or `cpu:<thread>` (game, vu, gs, render);
3. the render thread spent a quarter of T or more inside the present: `gpu` when the GPU's time is
   unknown or at least half of T, or the present mode never waits for the display (immediate);
   else `vsync` (the display or the compositor holds the present);
4. the present mode can wait for the display (fifo, fifo-relaxed, mailbox) and the rate is within
   3% of the refresh or of a whole fraction of it (1/2, 1/3, 1/4): `vsync`;
5. otherwise the largest of step 2: nothing reaches the budget alone (the threads hand the field
   on to each other, finding 108's chain, or the OS took the time), and the closest is named.

Reading the 920M player's case with it, once they send a 0.7.7 log: a `gpu` line with
`gpu_ms/frame` near 16.7 at scale 2-3 means lower the scale; `cpu:game` or `cpu:vu` with the GPU
well under budget means the 2-core CPU, and the scale does not matter; `gs: GPUs` says whether the
920M or the Intel GPU drew.

## What it costs

Per picture: one clock read, a histogram count and an uncontended spinlock (`perf_presented`), two
clock reads around the present and two around the render thread's wait. Per wait: one
performance-counter read at each end (the graphics queue's waits; the GS handoff only when it
waits). Once a second: one OS call per counted thread for its CPU time and one Rust call. A log
line at most every 2 s (a render line on change), else every 30 s; the line goes through dhd.log's
pipe and copying thread (finding 097), so a slow disk holds no game thread. One frame in 8 has its
passes' timestamps written and read back (asynchronously, three sets in flight).

Check: 1x uncapped (`DHD_FRAME_CLOCK=1 DHD_VSYNC=0`), the bench window 6000-7000 (race), hidden
desktop, one run each: the unchanged build 243.3 fields/s (p50 3.71 ms), this build 241.1 (p50
3.65 ms), -0.9%, inside the run-to-run spread of these runs (finding 108: up to 11% at 1x); the
bench's CPU per field: game 3.84 / 3.81, vu 3.66 / 3.50, gs 3.12 / 3.20, render 1.59 / 1.52 ms.

## The game is unchanged

`state_trace_run.py boot` (600 fields, headless) of the unchanged build and of this one: all 600
rows match (`state_diff.py`: PASS, mask unchanged). The changes are in the host and the renderer
only; the core (`dhd_core.wasm`) is not rebuilt by them.

## n/a, and where there are no lines

- **The browser**: no perf lines. `perf_log.h` makes the hooks empty in the wasm build (the shared
  `native_core.inc` compiles there, checked with wasi-sdk's clang in both worker modes); the
  browser has no per-thread CPU times and its WebGPU timestamps need a flag. Not cheap enough for
  this round.
- **The exact renderer**: `render`, `present_wait` and `gpu_ms/frame` are n/a (no render thread,
  no timed passes), `gpu_timer=off`, `shown=n/a`.
- **No graphics threads** (any `DHD_SCREENSHOT`: one thread does the drawing, docs/TESTING.md):
  `vu` and `gs` n/a.
- **An adapter without timestamp queries**: `gpu_timer=not offered by this GPU`, `gpu_ms/frame=n/a`;
  the rule then reads a long `present_wait` as `gpu`.
- **No display refresh known**: `display=n/a`, and step 4 is skipped.
- **The session lines** need a normal exit (`exit()`: the window's close, EXIT GAME, Android's
  activity end). A crash, a killed process or a diagnostic exit (`DHD_BENCH_EXIT`,
  `DHD_FPS_STOP`, state traces, the launcher's update exit: `host_exit`) leave only the 30 s lines.
- **DHD_INTERP** (experimental): the render thread's wait for a field is not noted on that path,
  so its busy time is too high there.
- The exit's lines are written on the game thread, whose FPU rounds toward zero (the EE's rule):
  their decimals can be one last digit low; the requested rate is printed as a rounded integer for
  that reason.

## Platforms

The code is shared by every native target (one host, one renderer). Built and run here only on
Windows x64 (DX12). macOS, Linux and Android were not built or run in this round; their parts are
the ones the bench already uses there (`host_thread_cpu`'s Mach and POSIX clocks, SDL's window
flags and display mode, wgpu's timestamps on Metal and Vulkan).

## Choices made

- `over` uses 1.2 times the budget, not the budget itself: paced frames at 60 land within a
  fraction of a millisecond of 16.68 ms on both sides, so "over 16.7" would count half of a
  perfect run; 1.2x counts the frames a player sees as late.
- `low1%` is the mean of the slowest 1% (as the bench tools' `worst1pct`), not the 99th
  percentile.
- Race and menus are summed apart (a menu's 60 says nothing about a race's), each with its own
  30 s window, and the session line is one per kind.
- One frame in 8 is GPU-timed: the GPU time of a scene changes slowly, and it keeps the readbacks
  to 7-30 a second.
- The verdict is one word, so in step 2 the largest part wins even when two are over budget.
- `test_dhdr_abi.py` reports one problem that is older than this branch (`dhdr_surface_counter`,
  finding 120, has no `src/abi.rs` entry); `dhdr_perf_read` is in `dhd_render_ext.h`, which that
  test does not cover.
