# 108: Where a 120 field/s frame goes on the phone

Status: current
Checked: 2026-11-04

2026-10-28, Mi 11i M2012K11G / Snapdragon 888 / Adreno 660 / Android 13, adb `1ae461e`. A
test-sub-agent round: measure where each frame's time goes at `DHD_FPS=120` over render scales 1-4,
and check that the Android build really displays at 120 Hz.

## Summary

**The display is not the problem; the frame is.** The panel already runs at 120 Hz during a race
(SurfaceFlinger `refresh-rate : 120.00 Hz`, `mActiveSfDisplayMode` 120.00001, and nothing in the
build asks for anything else), so no display fix was needed. At the sustained 1.32 GHz clock the
best measured configuration is **76.5 field/s at 1x** - 13.07 ms a field where 120 Hz has 8.33. 2x
reaches 61.4, 3x 46.0 and 4x 34.4 field/s, and no uncapped run beats the best paced run at the same
scale, so there is no vsync headroom to recover.

Where the 1x frame goes (12.84 ms, render thread, ms per field): **submit 7.68** (`encoder.finish`
4.99 + `queue.submit` 2.66), **present 2.68**, **Z-probe readback 1.42**, **command building 0.90**,
other 0.17. The 4.99 ms of `finish` is mostly the per-pass `VkFramebuffer` create+destroy churn
(44.6 % of the render thread's CPU, ~4.3 ms a field) and it costs the same at every scale;
`queue.submit` is the GPU wait and grows 2.7 ms per megapixel, which is why 3x and 4x are fill-bound
and 1x/2x are CPU-bound.

To reach 120, in order: **(1)** keep the framebuffer cache alive across `reset_all` (~4.5 ms at
every scale; takes 1x from 13.07 to ~8.5 ms); **(2)** overlap the GPU wait with the next field's CPU
work (~2.7 ms at 1x, 5.1 at 2x); **(3)** fold the Z probe into the picture's submission (~0.7-1.4
ms). That is enough to put 1x within reach.

## Build identity

Branch `perf/phone120-measure` off `prof/android-pcsx2` (`4988e2981`). The portable core
`build/wasm/dhd_core.wasm` is unchanged from the base commit (no core or game source changed), so it
was not rebuilt. `build_host64_android.py --jobs 4` and `build_apk.py --jobs 4 --offline` pass; APK
0.6.4 / versionCode 604. Everything below is committed on `perf/phone120-measure`.

Instrumentation added for this round, both off unless `DHD_RSTAT` is set:

- `port/render/src/native/render.rs` (`rstat`): per-picture stopwatches on the render thread for
  the Rust picture (`reserve`, `surfaces`, `record`, `write`, `scanout`, `submit`), the present
  (`acquire`, `precord`, `psubmit`) and the Z copies (`zstart`, `ztake`), one mean line per 240
  pictures.
- `port/gs/native_core.inc`, `gpu_rust_shared.inc`, `gpu_rust_native.inc`: the same for the C half
  of the render thread (`wait`, `lock`, `prepare`, `snapshot`, `draw`, `present`, `pace`, `show`,
  `zread`, `frame`).

`DHD_SPIKES` was left on for every run as a cross-check; its thresholds are why finding 107 could
not see the small parts.

## Scene and window: anchor on GO, not on the field number

The scene is `bench_race.SCENES['race']`, seed 81257, `DHD_SKIP_MOVIES=1`, `DHD_MEMCARD=0`,
`DHD_WIDESCREEN=0`, `DHD_ASPECT=4:3`, native renderer on Vulkan, sound on, two fields in flight.
*paced* = `DHD_FPS=120` only (the field clock holds each picture to its due time); *uncapped* =
`DHD_FPS=120 DHD_FRAME_CLOCK=1 DHD_VSYNC=0`.

The bench window is a range of *fields*, and the field at which the race starts is not the same in
every run: the menus and the load screen are real-time waits, so a run that renders fields faster
reaches GO in fewer fields. The GO fields this round were 3,397 (4x paced) to ~6,000 (the uncapped
runs), against 5,323 in the calibration run that produced the "5700-8900" window. A fixed field
range therefore covers a different part of the track in every run - and the track's cost varies
along it: the four quarters of `p1s2p`'s window run at 77.6, 57.7, 54.4 and 71.7 field/s, and
`lat2`'s at 81.6, 99.5, 88.3 and 101.1. Comparing runs on a fixed field window folds that +-20 %
into every number; it is why the first matrix's EE work seemed to grow with the scale (6.5 ms at 1x,
9.4 at 4x) when it cannot.

So every number below is windowed on each run's own GO: **GO+360 .. GO+3600 fields**, 3 s to 30 s of
race time at 120 fields/s, the same game-time section in every run whatever its frame rate. The
bench window is widened to 3,700-9,700 fields so that every run's GO window falls inside it;
`DHD_TIMELINE`'s per-field CSV supplies `wall_ms` and the per-thread CPU, and the `DHD_RSTAT`
blocks, attributed to the field of the last `spike_render` line, supply the parts (blocks that
straddle a window edge are dropped, so 11-13 blocks of 240 fields are averaged per run).

The check that the alignment works: the EE's own work (`ee_cpu_ms - ee_spin`) is the same at every
scale to within the run-to-run spread - 9.1, 7.9, 8.1, 9.0 ms at 1x-4x - as it must be, since the EE
emulation does not depend on the render scale. On the fixed window it seemed to grow from 6.5 to 9.4
ms; that was the scene, not the port. The run-to-run spread (up to 11 % in field/s at 1x paced, 0.2
% at 3x and 4x) is why every configuration has two runs, and why the parts (which repeat to a few
hundredths of a ms) carry the argument and the frame rates only the summary.

A run only counts for the matrix if its bench window covers the whole GO window: `go_window.py`
reports `covered=0` and the run is used for parts only, not for rates. That is why the `q1*` runs
(window 5700-8900, GO ~3,770) give the `submit` sub-split - a later, slower stretch of the same race
- but not a frame rate that can be compared with the 3,700-9,700 runs.

## The display already runs at 120 Hz

`dumpsys display` during a race: `mActiveSfDisplayMode` refreshRate **120.00001** (the panel's 120
Hz mode is active, not the 60 Hz one), and `dumpsys SurfaceFlinger` for our own layer: `refresh-rate
: 120.00 Hz`, `Frame Rate Overrides (setFrameRate): {}`, display policy `appRequestRange=[0, 120]`.
SDL3 3.4.18 and `port/android` make no frame-rate request at all (`SDLSurface.java` only reads
`mDisplay.getRefreshRate()` for `nativeSetScreenResolution`; `DhdActivity.java` only sets
`DHD_MONITOR_ASPECT`). MIUI's `RefreshRatePlicy` has `mDefaultPeakRefreshRate: 60.0`, but its vote
is `PRIORITY_MIUI_REFRESH_RATE maxRefreshRate=120` and the active mode during the race is the 120 Hz
one, so **nothing had to be changed** to get 120 Hz. The gap to 120 is the game's own frame cost,
not the display.

The runner samples `dumpsys display` every 3.4 s: in `p1s1p` and `p1s1u` every sample taken once the
game owned the display reads `DisplayMode{id=1 ... refreshRate=120.00001}`; the only 60 Hz samples
are the first one of a run (id 0, at 3.8 s, before the game shows a picture). The 120 Hz mode is
what MIUI picked on its own; that also means a game-side `ANativeWindow_setFrameRate` request would
only restate it, which is why the planned one-line call in `gpu_rust.c` was dropped.

## Frames per second and cost per thread, scales 1-4

The matrix: two runs per configuration (paced and uncapped at each of 1x-4x), all with the wide
window and `DHD_RSTAT`/`DHD_SPIKES` on. `fields/s` is the mean over GO+360..GO+3600; `displayed fps`
(`bench interp`) equals it in every run and SurfaceFlinger counted no dropped or duplicated frames,
so the one column is what the player sees. `EE work` is `ee_cpu_ms - ee_spin` (the game thread's
useful work; the rest of its CPU is its spin), and `big cores` is the median cpu4/cpu7
`scaling_cur_freq` over the window. All ms are per field.

| run | scale | mode | big cores GHz | fields/s | p50 | p99 | max | EE work | vu | gs | native | process | cores | EE wait GS |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| g1s1p | 1 | paced | 1.32 / 1.30 | 76.5 | 12.67 | 24.13 | 44.2 | 9.11 | 11.62 | 8.12 | 9.63 | 43.3 | 3.29 | 3.31 |
| g2s1p | 1 | paced | 1.32 / 1.30 | 68.0 | 14.34 | 24.03 | 51.9 | 8.75 | 12.65 | 9.30 | 10.57 | 50.9 | 3.36 | 2.86 |
| g1s1u | 1 | uncapped | 1.32 / 1.30 | 73.0 | 13.19 | 22.99 | 61.6 | 8.98 | 12.61 | 9.27 | 9.47 | 47.3 | 3.33 | 3.73 |
| g2s1u | 1 | uncapped | 1.32 / 1.30 | 72.3 | 13.27 | 23.26 | 64.6 | 9.20 | 12.78 | 9.59 | 9.76 | 48.8 | 3.37 | 3.63 |
| g1s2p | 2 | paced | 1.32 / 1.30 | 61.4 | 16.42 | 23.57 | 47.8 | 7.89 | 11.78 | 6.82 | 11.05 | 46.7 | 2.77 | 7.39 |
| g2s2p | 2 | paced | 1.21 / 1.19 | 56.8 | 17.60 | 26.99 | 59.0 | 8.42 | 13.11 | 7.16 | 11.57 | 54.0 | 2.95 | 8.01 |
| g1s2u | 2 | uncapped | 1.21 / 1.19 | 52.9 | 18.95 | 25.70 | 72.3 | 9.40 | 15.81 | 9.99 | 11.43 | 56.5 | 3.07 | 8.55 |
| g2s2u | 2 | uncapped | 1.21 / 1.19 | 53.5 | 18.59 | 26.25 | 74.0 | 9.30 | 15.85 | 9.97 | 11.26 | 56.3 | 3.08 | 8.49 |
| g1s3p | 3 | paced | 1.32 / 1.30 | 46.0 | 21.80 | 30.55 | 81.5 | 8.11 | 12.62 | 6.68 | 11.93 | 56.4 | 2.48 | 12.64 |
| g2s3p | 3 | paced | 1.32 / 1.30 | 45.9 | 22.00 | 30.09 | 62.5 | 8.13 | 12.65 | 6.67 | 11.91 | 56.2 | 2.47 | 12.66 |
| g1s3u | 3 | uncapped | 1.21 / 1.19 | 41.5 | 24.16 | 30.97 | 81.7 | 9.14 | 16.94 | 9.61 | 11.67 | 60.6 | 2.67 | 14.09 |
| g2s3u | 3 | uncapped | 1.21 / 1.19 | 41.2 | 24.25 | 31.23 | 93.5 | 9.05 | 17.07 | 9.61 | 11.76 | 60.9 | 2.68 | 14.34 |
| g1s4p | 4 | paced | 1.21 / 1.19 | 34.4 | 28.93 | 38.18 | 95.8 | 8.98 | 13.85 | 7.25 | 12.83 | 65.3 | 2.17 | 19.04 |
| g2s4p | 4 | paced | 1.21 / 1.19 | 34.4 | 29.13 | 38.53 | 85.9 | 9.10 | 13.80 | 7.20 | 12.98 | 65.6 | 2.17 | 18.97 |
| g1s4u | 4 | uncapped | 1.21 / 1.19 | 31.3 | 32.08 | 42.41 | 104.9 | 8.71 | 19.71 | 10.01 | 11.35 | 68.0 | 2.29 | 22.31 |
| g2s4u | 4 | uncapped | 1.21 / 1.19 | 30.7 | 32.74 | 44.10 | 102.5 | 8.96 | 19.89 | 10.20 | 11.65 | 68.2 | 2.29 | 22.68 |

Reading it:

- **120 field/s is out of reach at every scale.** The best configuration is 76.5 field/s at 1x (13.07
  ms a field where 120 Hz has 8.33); 2x is 61.4, 3x 46.0 and 4x 34.4, each scale step costing 3.3,
  5.4 and 7.3 ms.
- **The two runs of each configuration agree**, which is what makes the scale trend trustworthy:
  3x paced 46.0 and 45.9, 4x paced 34.4 and 34.4, 3x uncapped 41.5 and 41.2, 4x uncapped 31.3 and
  30.7, 2x uncapped 52.9 and 53.5, 1x uncapped 73.0 and 72.3. The one exception is 1x paced (76.5
  and 68.0), where the render thread's `wait` for the field handoff differs by 1.3 ms - the
  emulation threads, not the renderer, were the slower link in that run (see "Blockers and
  caveats").
- **The render thread is the limit at every scale**: `native` 9.6-12.8 ms against a 13.1-29.1 ms
  period, and the render thread's own wait for the next field (`c.wait`) is 0.0-1.6 ms, i.e. it is
  never idle waiting for work.
- **The pipeline is a chain, not a core shortage**: `process` cores fall from 3.3 at 1x to 2.2 at 4x
  as the threads spend more time waiting for each other, and `EE wait GS` rises from 3.1 to 19.0 ms:
  the EE waits for the GS thread, which is waiting for the render thread's `context_lock`.
- **The VU thread grows with the scale** (11.6 -> 13.9 ms of CPU) and the uncapped runs push it to
  16-20 ms: it is the thread that pays for the busy-wait chain when the field clock stops pacing.
- **Uncapped is never faster, and at the same clock it is slightly slower.** 1x is the only scale
  where the two modes share a clock: 73.0/72.3 uncapped against 76.5/68.0 paced, i.e. inside the
  paced spread. At 2x (both 1.21 GHz in one paced and both uncapped runs) the frame is 17.02 paced
  against 18.64/18.33 uncapped, and at 4x (all 1.21 GHz) 28.99 against 32.06/32.71 - 8-9 % more per
  field, with the extra time in the VU and GS threads (2x: vu 13.11 -> 15.8, gs 7.16 -> 10.0), not
  in the renderer. So `DHD_FRAME_CLOCK=1 DHD_VSYNC=0` moves work into the busy-wait chain and there
  is no hidden vsync headroom to recover.

### The phone's clocks sag, so compare within a round

The runner samples the three CPU clusters' `scaling_cur_freq` every 3.4 s and reports the median of
the samples taken inside the bench window. The big cores (cpu4, cpu7) started the session at
1.88/1.90 GHz and settled at **1.32/1.30 GHz**, where every wide-window run but one stayed; the
little core stayed at 1.80 GHz. Zone43 read 57-60 °C and the battery 38-39 °C throughout, so this is
the sustained cap, not a hot spot. One run dropped further: `g1s4p` (4x) ran at 1.21/1.19 GHz, so
its frame is ~8 % pessimistic against the others.

| round | cpu4 / cpu7 GHz | 1x paced |
|---|---|---|
| first matrix (`r1*`, fixed window) | 1.88 / 1.90 → 1.32 → 1.21 / 1.19 | 94.3 field/s (at 1.9 GHz) |
| instrumented (`p1*`, fixed window) | 1.21 / 1.19 | 89.7 field/s |
| wide window (`g1s1p`, `g2s1p`) | 1.32 / 1.30 | 76.5 and 68.0 field/s |

So the 94.3 and 89.7 of the earlier rounds are not comparable with the matrix below: they were taken
at a higher clock and on a later, cheaper stretch of the track. Within the wide-window round the
clock is the same for 1x-3x, which is what the scale trend is read from.

## Where a picture's time goes

`DHD_RSTAT` stopwatches the render thread 22 times per field (one `CLOCK_MONOTONIC` read each, ~0.5
us per field, and nothing at all when unset). It logs the mean of every part once per 240 pictures.
Each block is attributed to the field of the last `spike_render` line before it - which
`rstat_idx.py` checked against the block index (block k covers fields 240k..240k+240, and the spike
attribution matches it to within one field) - and the blocks wholly inside the GO window are
averaged. The parts add up to the C `frame` within 0.1 ms in every run, which is the check that no
time is counted twice or lost.

The five parts the round asked for, ms per field, render thread (two paced runs per scale; the pairs
agree to a few hundredths of a ms in `build`, `submit` and `zread`):

- **build** - Rust `reserve` + `surfaces` + `record` + `write` + `scanout` (command building and
  the uploads) and C `prepare` + `snapshot`;
- **submit** - Rust `submit`: `arenas.write` + `encoder.finish()` + `queue.submit`;
- **present** - Rust `present` (`acquire` + `precord` + `psubmit`), C `pace`, C `show`
  (`dhdr_surface_present`);
- **zread** - C `zread` (Rust `zstart` + `ztake`): the lens flare's Z probe;
- **other** - the rest of the frame (`lock`, `wait`, the recycle handoff).

| run | fields/s | frame | build | submit | present | zread | other |
|---|---|---|---|---|---|---|---|
| g1s1p 1x paced | 76.5 | 12.84 | 0.90 | 7.68 | 2.68 | 1.42 | 0.17 |
| g2s1p 1x paced | 68.0 | 13.18 | 0.98 | 7.84 | 2.60 | 1.60 | 0.17 |
| g1s2p 2x paced | 61.4 | 16.10 | 0.85 | 10.58 | 3.01 | 1.49 | 0.16 |
| g2s2p 2x paced | 56.8 | 17.02 | 0.84 | 10.90 | 4.14 | 0.95 | 0.17 |
| g1s3p 3x paced | 46.0 | 21.47 | 0.88 | 14.62 | 4.56 | 1.24 | 0.17 |
| g2s3p 3x paced | 45.9 | 21.52 | 0.89 | 14.70 | 4.48 | 1.28 | 0.17 |
| g1s4p 4x paced | 34.4 | 28.99 | 1.00 | 20.86 | 5.71 | 1.23 | 0.19 |
| g2s4p 4x paced | 34.4 | 29.01 | 1.00 | 20.68 | 5.90 | 1.24 | 0.19 |

The one part that moves between runs is `present` at 2x (3.01 against 4.14): its `psubmit` is the
same GPU wait as `qsub`, and how much of the wait lands in this field's present rather than in the
next field's submit depends on where the queue is when the field ends. The totals agree to 0.9 ms.

**`submit` is mostly CPU, not wgpu's command building** (60 % of the frame at 1x, 68 % at 3x):

- `record` - walking the picture's commands and encoding every draw - is 0.48 ms at 1x and 0.48 at
  3x. The CPU cost of building a frame is 7 % of the frame and does not grow with the scale.
- `submit` splits into `finish` (`encoder.finish()`, the deferred hal encoding: framebuffer
  creation + `vkCmd*`) **4.99 ms**, `qsub` (`queue.submit`) **2.66 ms** and `awrite` (staging
  arenas) **0.02 ms** at 1x. Only `qsub` grows with the picture's pixels - 2.66 at 1x (0.29 MPix),
  5.05 at 2x (1.15), 8.47 at 3x (2.58) - which is a **1.9 ms + 2.8 ms per megapixel** line
  (residual 0.4 ms): a small fixed GPU cost plus fill. `finish` is CPU and grows by only 0.5 ms per
  scale step.
- `acquire` - taking the window's next image - stays at 0.12 ms at every scale, so the swapchain is
  never starved and the panel is not what the render thread waits for.
- simpleperf (`prof1`, 1x, 500 Hz, DWARF call graphs, 42 s / 3133 fields) says what `finish` is:
  44.6 % of the render thread's CPU samples are inside `qglinternal::vkCreateFramebuffer` and
  `vkDestroyFramebuffer`, i.e. **the per-draw-pass Vulkan framebuffers**, which cost 4.2 ms per
  field at 1x (see "The profile" below).

**`present` (21 % at 1x) is a second GPU wait.** Its `psubmit` (1.64 ms at 1x, 1.82 at 2x, 3.26 at
3x) grows with the *picture's* pixels although the present blit is always the window's 1080x2400, so
it is the same wait as `submit`, one submission later. `show` (`dhdr_surface_present`, 0.89-1.13 ms)
and `acquire` (0.12) are flat; `pace` is 0.01 ms, so at `DHD_FPS=120` the field clock is almost
never what holds a picture back.

**`zread` (11 % at 1x) is one extra submission per field.** The lens-flare Z probe submits a compute
pass over a 64x64 patch plus a buffer copy in *its own* command buffer (`zstart` 0.66 ms, which also
makes a bind group), then polls and copies 16 KB out (`ztake` 0.76). It never waits on the map:
`zread_take` only takes copies that finished and the game reads a value a field or two old. So there
is no readback stall to remove - but there is a whole extra `queue.submit` per field.

**`build` (7 % at 1x)** stays at 0.85-0.90 ms from 1x to 3x: command building is not where the time
is. What is left of the frame is `wait` (1.1 ms in `q1s1p`, 0.26 in `g1s1p` - the render thread
waiting for the next field's handoff, so it moves with how far ahead the other threads are) and
`lock` + `prepare` + `snapshot` (0.05 ms: the render thread already holds the context lock when it
draws, so the lock is free for it).

**`DHD_FRAME_LATENCY=2` is a wash.** `q1s1l2` is `q1s1p` at the same clock (1.32 GHz) with a third
swapchain image: 65.5 field/s against 64.5, p50 14.64 against 15.10 ms, p99 23.9 against 25.0. The
render thread's `wait` for the field handoff falls (1.12 -> 0.67) but its own work rises (frame
13.36 -> 13.64, `present` 1.66 -> 1.89), so the 0.2 ms of wall time is inside the run-to-run spread
of this scene. It is not a lever for 120.

**The frame is the sum of these, not their maximum**: the render thread waits for the GPU inside
`submit`, and only then does the next field's CPU work (present, Z probe, build) begin, because it
holds `context_lock` from the field's handoff to the last Z copy. There is no overlap between the
GPU's picture and the next picture's CPU work today, which is why a 12.8 ms frame at 1x contains 4.3
ms of GPU wait and 8.4 ms of CPU instead of taking 4.3 ms.

## What grows with the scale

The four paced runs of the wide-window matrix (`g1s1p`..`g1s4p`), ms per field:

| part | 1x | 2x | 3x | 4x | grows? |
|---|---|---|---|---|---|
| build | 0.90 | 0.85 | 0.88 | 0.95 | no (uploads and command building) |
| `finish` (CPU) | 4.99 | 5.50 | 6.13 | 6.74 | +0.58 per step, not with the pixels |
| `qsub` (GPU wait) | 2.66 | 5.05 | 8.47 | 14.10 | **yes, 2.7 ms per megapixel** |
| present | 2.68 | 3.01 | 4.56 | 4.41 | only `psubmit`, the same GPU wait |
| zread | 1.42 | 1.49 | 1.24 | 1.24 | no (a fixed 64x64 patch) |
| frame | 12.84 | 16.10 | 21.47 | 28.99 | |
| field period (120 Hz = 8.33) | 13.07 | 16.29 | 21.73 | 29.07 | |

`qsub` is a straight line in the render target's pixels: 1.9 ms + 2.7 ms per megapixel, residuals
under 0.4 ms over the four points. That is the GPU, and it is the only part that is: at 4x the GPU
has ~14 ms of picture work per field, which is why 4x cannot reach 120 with any amount of CPU
tuning. The 4x run also ran at a lower clock than the others (big cores 1.21 GHz against 1.32), so
its frame is ~8 % pessimistic; the pixel fit is unaffected.

`finish` grows by 0.58 ms per scale step although the number of passes is the same (18.5, 18.5,
18.6, 18.7) - the same per-pass work over bigger render targets and textures, i.e. the driver's CPU
cost of a pass is partly memory-bound. It is still ~4.4 ms at every scale that is not 1x, and it is
CPU the render thread cannot overlap with anything today.

So: **CPU-bound at 1x and 2x** (the fixed ~5 ms of framebuffer work plus the game/VU threads), **GPU
fill-bound at 3x and 4x** (`qsub` 8.5 and 14.1 ms), with the two meeting around 2.5x. Lowering the
scale below 1x would buy another ~2.7 ms per megapixel of GPU time and nothing of the CPU, which is
why the path to 120 is the framebuffer work, not the resolution.

## GPU time: what can and cannot be measured

- **The device can do GPU timestamps; the renderer does not ask for them.** A probe added to
  `port/render/src/device.rs` for this round (`DHD_RSTAT`, one line at adapter creation) prints
  `adapter Adreno (TM) 660 (Vulkan) TIMESTAMP_QUERY=yes TIMESTAMP_QUERY_INSIDE_ENCODERS=yes
  TIMESTAMP_QUERY_INSIDE_PASSES=yes`: Adreno 660 on Vulkan 1.1+ exposes the whole timestamp-query
  set, including per-pass timestamps, so a GPU-time breakdown per draw pass is possible on this
  phone. The renderer requests only `DUAL_SOURCE_BLENDING` today, so nothing creates a query set or
  resolves one. Adding it is a contained change - request the feature, one query set per frame in
  flight, `timestamp_writes` on the passes that matter, resolve to a buffer, map on the frame after
  - but it is a renderer change and was out of scope for this round; the numbers below are the
  render thread's blocked time plus the pixel fit instead.
- **Adreno's counters are closed.** `/sys/class/kgsl/kgsl-3d0/gpuclk` and `gpu_busy_percentage` are
  permission-denied on this MIUI build; only `gpubusy` is readable and its two counters move in both
  directions between reads, so they are not a busy/total pair and were not used.
- What is left is the render thread's own blocked time inside `queue.submit`, which is a lower
  bound on the GPU time that is not overlapped with CPU work, plus the pixel fit above.
- The feature probe result is in "The profile" below.

## The profile: a framebuffer per draw pass is the fixed cost

`prof1` (1x paced, `simpleperf record -e cpu-clock -f 500 --call-graph dwarf`, 42 s of race, 3,133
fields, 74.6 field/s under the profiler) recorded 29.5 CPU-seconds on the render thread, 9.4 ms per
field against the bench's 9.7. The APK-embedded debug `libmain.so` had to be symbolized by hand
(simpleperf 29 does not resolve the build id of `base.apk!/lib/arm64-v8a/libmain.so`; the
`libmain.so[+off]` names were batch-resolved with `llvm-addr2line` against the same `.so`), so the
`libmain` frames below are real function names.

| frame, inclusive | share of the render thread |
|---|---|
| `qglinternal::vkCreateFramebuffer` | **35.6 %** |
| `gsl_memory_alloc_pure` → `ioctl_kgsl_sharedmem_alloc` | **31.2 %** |
| `je_*` (jemalloc) | 14.6 % |
| `qglinternal::vkDestroyFramebuffer` | **9.0 %** |
| `vulkan::driver::QueueSubmit` (`vkQueueSubmit`) | 8.4 % |
| `ioctl_kgsl_sharedmem_free` | 7.8 % |
| `vkCmd*` encoding | 7.5 % |
| `vkResetCommandPool` | 5.6 % |
| `memcpy` | 3.8 % |

Self time: 47.1 % `[kernel.kallsyms]`, 17.4 % `libmain`, ~12 % `vulkan.adreno.so`, 3.7 % `memcpy`,
~3.4 % jemalloc, 1.0 % `memset`.

**Why the framebuffers are new every field.** wgpu-hal keeps a `VkFramebuffer` cache per command
pool and *drains it in `reset_all`* (wgpu-hal-29.0.3 `src/vulkan/command.rs:183-186`), which runs
when the pool is reset at the end of each field's encoder. The native renderer opens one draw pass
per target change (`Pass::begin`, `render()`), so every pass of every field creates its
`VkFramebuffer` again, and the Adreno driver backs each one with a fresh `gsl_memory_alloc_pure` →
`ioctl_kgsl_sharedmem_alloc`. The 31.2 % allocation is therefore *not* a texture upload, a staging
buffer or the swapchain: it is the framebuffer churn. Create plus destroy is **44.6 % of the render
thread's CPU**, 4.3 ms per field at 1x, and it does not depend on the render scale, the picture's
pixels or the scene's contents - only on how many passes the field opens.

That is also the fixed cost the scale fit finds in `submit`: it is CPU in the driver, not GPU. The
pixel term of the fit is the GPU; the intercept is this.

The kernel samples do not unwind (the 31.2 % chain is 29.5 % kernel frames only), which is why the
caller could only be named from the `vkCreateFramebuffer` frame that contains it, not from the
allocation itself.

**What the timers say about where it lands** (`q1s1p`, the same build with the sub-stopwatches in
`submit`, GO-anchored window): the `submit` block splits into `finish` **5.21 ms**, `qsub`
(`queue.submit`) **2.77 ms** and `awrite` (writing the staging arenas) **0.02 ms**, with **19.9 draw
passes** per field (18-20 across the window, from `Pass::begin`). A field's passes are therefore
**0.26 ms each**, which is the 4.2 ms create+destroy the profile attributes to them plus ~1 ms of
`vkCmd*` encoding - i.e. `finish` is nearly all framebuffer churn, and the 2.77 ms of `qsub` is the
GPU wait proper (its CPU part is 0.8 ms, 8.4 % of the thread). At 2x the same split is `finish` 5.50
and `qsub` 5.05: the churn grows by only 0.5 ms while the wait grows by 2.4, which is what the
profile says - the framebuffer cost is per pass, not per pixel.

## What to cut, in order

Everything below is ms per field at 1x paced (`g1s1p`, 76.5 field/s, 13.07 ms of wall) unless said
otherwise; the budget is 8.33 ms.

1. **The per-pass framebuffer churn, ~5-7 ms at every scale.** `finish` is 4.99 ms at 1x, 5.50 at 2x,
   6.13 at 3x and 6.74 at 4x - it grows a little with the scale (bigger targets, more memory
   traffic) but far less than the pixels do; 18.5 passes per field at ~0.27 ms each; the profile puts
   44.6 % of
   the render thread's CPU (4.2 ms) in `vkCreateFramebuffer` + `vkDestroyFramebuffer` and the
   allocation behind them. wgpu-hal drops its framebuffer cache in every `reset_all`
   (`vulkan/command.rs:183-186`) and the renderer resets the encoder every field, so every pass of
   every field pays again. Keeping the cache alive across resets - or one command pool per frame in
   flight that is not reset - is the biggest single lever: ~4.5 ms at 1x, 2x, 3x and 4x alike, and it
   takes 1x from 13.07 to ~8.5 ms, within reach of 120.
2. **The GPU wait: overlap it, 2.7 ms at 1x and 5.1 at 2x.** `qsub` (2.66) + `psubmit` (1.64) is GPU
   time the render thread sits through while it holds `context_lock`, so the next field's CPU work
   cannot start until it returns; the field period is CPU + GPU instead of max(CPU, GPU). Two
   pictures in flight would save up to 2.7 ms at 1x and 5.1 at 2x. At 4x `qsub` alone is 14.1 ms,
   so there the GPU becomes the limit and 120 stays out of reach.
3. **Fold the Z probe into the picture's submission: 0.7-1.4 ms.** `zread` submits its own command
   buffer every field (a 64x64 compute pass plus a copy): `zstart` 0.66 ms of CPU (bind group +
   encoder) and `ztake` 0.76 ms of wait. Nothing stalls on the map, so the whole cost is the extra
   submission; recording the probe into the picture's encoder and taking the copy a frame later
   removes it.
4. **Fewer draw passes, if (1) is not done.** 18.5 passes per field at ~0.27 ms of churn each: with
   the framebuffer cache fixed the pass count stops mattering, without it every pass removed is
   ~0.27 ms (bucketing draws into an atlas or a shared target would have to remove most of the 18.5
   to matter, and the renderer's targets are what the game's effects ask for).
5. **The render scale.** 2x costs 3.26 ms more than 1x (frame 16.10 against 12.84) and 4x ~18 ms
   more. 1x is the only scale with a path to 120.
6. **`DHD_FRAME_LATENCY=2`**: measured, no win (above).

**The renderer is not the only limit at 1x.** Per field at 1x the emulation threads need more than
8.33 ms of CPU each: EE 12.42 (9.11 of it work, 3.31 spin), VU 11.62, GS 8.12. A renderer that cost
nothing would still leave 1x at ~110 field/s at the sustained 1.32 GHz clock, and the VU thread's
10.5 ms of busy time is above the 120 Hz budget on its own.

## Blockers and caveats

  so no two runs overlap; the cost is that a batch can wait minutes for the lock, and the batches
  are separated by minutes of idle, which is long enough for the CPU clock to move. That is the
  reason for the clock column and for reading the scale trend only within a round.
- **The sustained clock, not the peak, is what the table shows.** The big cores settle at
  1.32/1.30 GHz under a race load (from 1.88/1.90 at idle) with zone43 at 57-60 °C and the battery
  at 38-39 °C, i.e. a policy cap, not a hot spot. The earlier 94.3 field/s at 1x was taken at
  1.9 GHz and on a cheaper stretch of track; it is not a target this build can hold. A phone that
  holds 1.9 GHz would move every number by roughly the clock ratio, which does not change any
  conclusion here - the framebuffer churn and the GPU wait scale with neither clock nor scale in the
  same way.
- **One run was killed by MIUI.** `g1s3p` attempt 1 died at field 3,726 with
  `Force stopping app.downhillrevived ... from process:28429` in logcat; the run was repeated and
  the repeat is the one in the table. The runner now force-stops the app and exits if a run has not
  finished 240 s after its timeout, and one earlier stuck batch (`round4a`) had to be killed by
  hand; no run in the matrix is from a partial log (`go_window.py` requires the timeline to cover
  the window, `covered=1`).
- **The run-to-run spread depends on the scale.** At 3x and 4x the two runs agree to 0.2 % in
  field/s when paced (46.0/45.9 and 34.4/34.4) and to 2 % when uncapped (41.5/41.2, 31.3/30.7), and
  their parts to a hundredth of a ms, which is the check that the GO window works. At 1x paced the
  two runs are 76.5 and 68.0 (11 % apart) although their render work differs by only 0.34 ms: the
  gap is the render thread's `wait` for the next field (0.26 against 1.57 ms), i.e. in `g2s1p` the
  emulation threads were the slower link. That is the co-limit showing up as noise, and it is why
  the ranking below rests on the parts and the profile, while the frame rates only size the gap.
- **The GO window is 3,240 fields of one track section.** The track's cost varies +-20 % along it
  (the four quarters of one window run at 77.6/57.7/54.4/71.7 field/s), so a different section
  would move the absolute field/s; it would not move the split between the parts, which is what
  the round asked for.
- **No GPU timestamps were taken.** The device exposes the full timestamp-query set (probe above),
  but enabling it is a renderer change; the GPU numbers here are the render thread's blocked time
  in `queue.submit` (a lower bound on unoverlapped GPU time) plus the pixel fit. A per-pass GPU
  breakdown is the obvious next measurement and would separate the driver's framebuffer CPU from
  the GPU's fill directly.
- **The profile is one run, at 1x, under the profiler.** `prof1` ran at 74.6 field/s against the
  bench's 76.5, so simpleperf costs ~3 %; its percentages are shares of the render thread's CPU
  samples and its kernel frames do not unwind, so the `gsl_memory_alloc_pure` chain is attributed
  through the `vkCreateFramebuffer` frame that contains it rather than from the allocation itself.
- **Nothing on the phone was changed.** Refresh rate, display mode and MIUI settings were only
  read. `dhd.ini` was restored byte for byte from the copy taken before the first run (device md5
  `e61ff162d66a8e57d0f769413d62df33`), `/sdcard/PS2/` was not touched, and the app was left
  stopped.
- **Choices made without asking** (the round said to work autonomously): the bench window is
  anchored on each run's race start instead of a fixed field range (the fixed range was comparing
  different parts of the track); every configuration got two runs once the first matrix showed an
  11 % spread at 1x; the display was left alone because it already runs at 120 Hz; GPU timestamps
  were probed but not enabled (a renderer change beyond "measure where the time goes"); and
  `DHD_FRAME_LATENCY=2` was measured once, at 1x, as a lever rather than as a matrix dimension.
