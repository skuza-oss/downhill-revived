# 126: Android bench, 2026-10-31 — the low-end phone (Realme RMX3930, Unisoc T612)

A test round run by a sub-agent: benchmark the game on the phone on the end of the USB cable, with
finding 108's measurement method. The phone turned out **not** to be the Mi 11i of findings
107/108/110/112 but a Realme RMX3930 (Unisoc T612, Mali-G57, Android 14) — the low-end target the
next round throttles the user's Mi 11i down to. That is why the numbers below are far below the
Mi 11i's, and why they are useful: they are a profile of what "60 fps on a T612" costs.

The round ended early: **three of the four runs completed, the fourth was skipped and the
calibration capture did not complete** (see "What did not complete").

## Summary

| configuration | fields/s | 1% low | pictures over 20 ms |
|---|---:|---:|---:|
| 1x, paced to 60 (`DHD_FPS=60`, the default) | **39.6** | 54.6 ms | 64.0 % |
| 1x, uncapped (120 field clock, vsync off) | **36.3** | 44.9 ms | 99.1 % |
| 2x, uncapped | **14.4** | 92.4 ms | 100 % |
| 2x, paced to 60 | not run (see below) | | |

Fields/s and percentiles are the GO-anchored window (GO+360 .. GO+2400, 2,401 fields), as finding
108 explains; the whole `DHD_BENCH` window's own numbers are in the table further down.

The phone cannot hold 60 anywhere: at 1x paced it runs at 40 fields/s (25 ms a field against
16.7), and 2x collapses to 14 fields/s. **At 1x the limit is the EE thread** (25.2 ms of wall,
14.9 ms of CPU, so about 10 ms a field descheduled or in an uncounted wait); **at 2x it is the GS
thread waiting for the render thread and the Mali-G57** (GS busy 69.1 of 69.6 ms, EE spinning
41.5 ms waiting for it). The GPU timestamps the next round wants are supported by this driver
(checked), but the capture that would have used them died early.

## Device

`getprop` and sysfs, 2026-10-31. The earlier phone rounds (findings 091, 107, 108, 110, 112: Mi
11i, Snapdragon 888, Adreno 660, 120 Hz) describe a different handset; nothing here is comparable
to them except through the recipe.

| | |
|---|---|
| Model / device | `RMX3930` (Realme; marketing name not recorded), `ro.product.device` `RE6054` |
| SoC | Unisoc `T612` (`ro.board.platform` `ums9230`; `ro.hardware` `ums9230_latte`) |
| Android | 14, API 34, build `RMX3930export_14_A.35` |
| CPU | 8 cores online (`0-7`), two clusters: cpu0-5 `related_cpus 0 1 2 3 4 5`, cpu6-7 `6 7` (T612: 2x Cortex-A75 in cpu6-7, 6x Cortex-A55 in cpu0-5) |
| CPU frequencies | every core: `cpuinfo_min_freq` 614,400 (cpu6-7: 768,000), `cpuinfo_max_freq` **1,820,000**; the same 10 steps 0.614-1.82 GHz on cpu0-5, 7 steps 0.768-1.82 on cpu6-7; `scaling_max_freq` 1.82 GHz on every core at capture |
| RAM | 7,966,232 kB (7.6 GiB) |
| GPU | `ARM Mali-G57`, OpenGL ES 3.2 `v1.r41p0-01eac0.018fc629603b7a70cc68d32f78ac8686`; in-game `gs: GPU Mali-G57 on Vulkan, driver Mali-G57 v1.r41p0-...` |
| Display | 720x1600, modes 60.0 and 90.0 Hz (`activeMode 60.00 Hz`, `refresh-rate : 60.00 Hz` while idle) |
| Thermal sensors | `/sys/class/thermal/thermal_zone*` are **permission denied** to the shell on this build; readings come from `dumpsys thermalservice`, which names them (`soc-thmzone`, `gputh-`/`gpu-thmzone`, `cputop0-tzone0`, `cputop1-thmzone`, `ap-thmzone`, `battery`) |
| Power | `dumpsys thermalservice`: `Thermal Status: 0`; ADPF **is** available here (`android: performance hint session: 4 threads, target 13.0 ms` in every run's log) — finding 091 found no session on the Mi 11i |

## Build

Branch `perf/android-bench-1031`, cut from `269ede0` ("Merge fix/peru-vu1: PERU weather no longer
stalls VU1"), one commit per step.

- Portable core: `build/wasm/dhd_core.wasm` copied from an existing local build tree (built from
  the same game source, PERU fix included) rather than rebuilding it:
  **sha256 `c9d1d1580a57061b42fbdfc03c55b6f5dc2f8fffd40ba5ad114366e30efc4498`**, 8,226,975 bytes.
- `python port/tools/build_host64.py build/wasm/dhd_core.wasm --target android-arm64 --jobs 4`:
  SDL3 3.4.18 and the Rust renderer were cold (first aarch64-android build on this machine, about
  12 minutes together); `libmain.so` 28.2 MB, 37 host objects, 138 generated imports.
- `python port/android/build_apk.py --offline --jobs 4`: `downhill-revived-0.7.5-debug.apk`
  (30.1 MB), `app.downhillrevived` **0.7.5 / versionCode 705**, arm64-v8a, debug-signed,
  installed 2026-10-31 19:32.

The disc image is the test disc (`DHD_TEST_ISO`, sha256 `0fe2b77713d3440cc3030741ea0b18f49e3feaa0
9671a30a421e0d4888a7eb86`, 2,530,869,248 bytes). It was pushed in three 900 MB chunks (`adb push`
to `/data/local/tmp`, appended with `run-as ... cat >>`, chunk deleted) and verified on the device:
**md5 `3654a6f4438c899be4a1f5236cfdcd74`**, size 2,530,869,248 — identical to the host's file.
Streaming it straight into `adb shell ... cat > file` is *not* byte safe (every attempt gave the
same wrong md5), which is why the chunked route was used; `/data` had 4.66 GB free and cannot hold
two copies of the image.

## Method

Finding 107/108/112's race, unchanged where it matters:

- `bench_race.SCENES['race']` (screen-keyed navigation, race pad, seed **81257**),
  `DHD_SKIP_MOVIES=1`, `DHD_MEMCARD=0`, `DHD_WIDESCREEN=0`, `DHD_ASPECT=4:3`, native renderer on
  Vulkan, sound on, two fields in flight, `DHD_SCALE` 1 or 2.
- *paced* = `DHD_FPS=60` only (what a player gets by default; no frame clock);
  *uncapped* = `DHD_FPS=120 DHD_FRAME_CLOCK=1 DHD_VSYNC=0` (finding 112's recipe).
- `DHD_BENCH` a wide window with `DHD_BENCH_EXIT=1`, `DHD_RACE_FRAMELOG=1` and `DHD_TIMELINE` to a
  CSV in the app's files; each run is then windowed **on its own GO** (GO+360 .. GO+2400) from
  the timeline, with the 1% low, the count over 20 ms and the per-thread CPU taken from those
  rows.
- **2,401 fields, not 108's 3,241**: the uncapped runs reach GO at field 6,022, so a bench window
  ending at 9,000 leaves only 2,978 fields after GO; 2,401 fields is 40 s of game time (a field is
  1/60 s of game time in the frame-clock runs) and fits in every run. The 2x run's window was
  widened to 3,400-11,000 for the same reason.
- GO is **3,214** (countdown 2,954) in the paced run and **6,022** (countdown 5,500) in both
  uncapped runs — the faster a run renders, the more fields the real-time menus and load screen
  take, exactly finding 108's point. A fixed field window would compare different track sections.
- Sampling every ~3 s for the whole run, each sample flagged with whether the field was inside the
  `DHD_BENCH` window: all eight cores' `scaling_cur_freq`, `dumpsys thermalservice`,
  `dumpsys battery` and the port threads' `stat`/`schedstat`.
- Cooling between runs: app force-stopped, screen off (`input keyevent 26`), **180 s**, screen on.
  The first run followed the install and the ISO push (the phone had been idle a few minutes).

## Results

GO-anchored window (GO+360 .. GO+2400, 2,401 fields). CPU ms are per field from the timeline;
`native` (the Rust render thread) is from the run's `bench threads` line, which covers the whole
`DHD_BENCH` window — for the two uncapped runs that window starts 2,622 fields *before* GO, so
their `native` also contains menus and loading; the game/vu/gs ms do not.

| run | config | GO | fields/s | 1% low ms | p50 ms | p99 ms | max ms | over 20 ms |
|---|---|---|---:|---:|---:|---:|---:|---:|
| r1-s1p | 1x paced 60 | 3214 | 39.61 | 54.62 | 24.75 | 46.30 | 93.83 | 1,536 / 2,401 (64.0 %) |
| r2-s1u | 1x uncapped | 6022 | 36.30 | 44.85 | 27.40 | 35.83 | 120.79 | 2,379 / 2,401 (99.1 %) |
| r3-s2u | 2x uncapped | 6022 | 14.36 | 92.38 | 69.03 | 87.93 | 120.73 | 2,400 / 2,401 (100 %) |

| run | game ms | vu ms | gs ms | native ms | game busy | vu busy | gs busy | field ms | process ms/field | busy cores |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| r1-s1p | 14.94 | 12.15 | 13.01 | 9.44 | 25.20 | 12.31 | 13.29 | 25.25 | 69.45 | 2.46 |
| r2-s1u | 25.86 | 18.66 | 17.30 | 8.88 | 16.03 | 18.93 | 26.90 | 27.55 | 77.62 | 2.87 |
| r3-s2u | 57.14 | 18.31 | 17.38 | 7.94 | 15.95 | 18.58 | 69.14 | 69.62 | 108.89 | 1.59 |

("busy" is the thread's field wall minus its counted queue waits, from the timeline; it still
contains descheduling and GPU/driver blocking.)

The whole `DHD_BENCH` windows, for comparison with the anchored numbers: r1 3,400-9,000
**35.48 fields/s** (p99 48.84, 1% low 56.89), r2 3,400-9,000 **36.96** (p99 42.67, 1% low 51.31),
r3 3,400-11,000 **14.65** (p99 148.31, 1% low 156.38).

Present intervals and audio over the whole window (`bench interp` / `bench snd`):

| run | displayed fps | present p50 / p99 / max ms | audio underruns | silence ms | queue p50 ms | tick p50 / p99 ms |
|---|---:|---|---:|---:|---:|---:|
| r1-s1p | 35.48 | 27.89 / 47.50 / 157.90 | 34 | 3,050 | 70 | 28.5 / 48.5 |
| r2-s1u | 36.96 | 27.40 / 48.15 / 163.34 | 388 | 55,390 | 63 | 54.5 / 63.5 |
| r3-s2u | 14.65 | 67.24 / 157.73 / 181.64 | 1,206 | 380,920 | 55 | 63.5 / 63.5 |

`displayed fps` equals `fields/s` in each run, so the presenter is not dropping or duplicating
pictures; a player sees what the field clock produces.

## Where a field goes (the profile the throttled Mi 11i is to match)

**1x paced 60 (r1): the EE thread is the limit.** 25.25 ms a field, of which the game thread is
busy 25.2 ms with 14.9 ms of CPU; VU 12.3 busy / 12.2 CPU; GS 13.3 busy / 13.0 CPU; the Rust
render thread 9.4 ms CPU. Process CPU is 69.4 ms per field, i.e. 2.46 cores busy. The queue waits
are tiny (`ee_wait_vu` 0.01, `ee_wait_gs` 0.04, `ee_spin` 0.04), so this is not a handoff stall —
but busy minus CPU leaves **about 10 ms a field that is neither on a CPU nor in a counted wait**:
the EE thread is descheduled, or blocked in something the counters do not name (a lock, an
allocation, the render queue). With only 2.46 of 8 cores busy, raw capacity is not short, so where
the scheduler puts the EE thread (an A55, beside other work) is the first candidate; it was not
measurable on an unrooted phone. Present p50 27.9 ms
against a 16.7 ms vsync grid is why 64 % of the pictures land over 20 ms and the paced run's p99
(46.3) is worse than the uncapped run's (35.8): the paced field clock plus the 60 Hz display
quantises the missed frames into double intervals.

**1x uncapped (r2): same rate, more spin.** 27.55 ms a field with the EE's CPU up to 25.9 ms of
which 10.1 is spin, and `ee_wait_gs` 10.8 ms; GS busy 26.9 of 27.55, i.e. the GS thread waits for
the render thread / GPU, and the EE spins on the GS queue instead of pacing. It is *slower* than
paced (36.3 against 39.6 fields/s), the same effect finding 108 saw on the Mi 11i: removing the
field clock moves work into the busy-wait chain and buys nothing.

**2x uncapped (r3): the graphics chain, not the game.** 69.62 ms a field: GS busy 69.1 ms with only
17.4 ms of CPU (it is blocked in the renderer's present/submit), the EE 57.1 ms of CPU with 41.5 ms
of spin and `ee_wait_gs` 53.0 ms, the render thread only 7.9 ms of CPU. The internal picture is the
game's 640x448 doubled by `DHD_SCALE=2` (1280x896) and the GPU is a Mali-G57; the present p99
(157.7 ms) and max (181.6 ms) say the driver/GPU waits are long. So 2x costs 2.8x the field time of
1x for 4x the pixels.

## Clocks and thermals

`scaling_cur_freq`, sampled every ~3 s during each run (`samples.json`), has a **median of 1.82 GHz
on every core**, but the governor does not hold the A55 cluster there. The two A75s stay at the top
step; the six A55s drop to the 614 MHz minimum in a good share of the samples, and more often at
2x (where fewer threads are runnable):

| run | cpu0 | cpu1 | cpu2 | cpu3 | cpu4 | cpu5 | cpu6 | cpu7 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| r1-s1p, mean GHz | 1.59 | 1.60 | 1.71 | 1.52 | 1.60 | 1.73 | 1.79 | 1.82 |
| r1-s1p, samples at 1.82 | 77 % | 74 % | 87 % | 67 % | 77 % | 85 % | 92 % | 100 % |
| r2-s1u, mean GHz | 1.66 | 1.68 | 1.78 | 1.68 | 1.77 | 1.69 | 1.78 | 1.81 |
| r2-s1u, samples at 1.82 | 81 % | 84 % | 95 % | 84 % | 95 % | 89 % | 95 % | 97 % |
| r3-s2u, mean GHz | 1.49 | 1.50 | 1.49 | 1.52 | 1.45 | 1.60 | 1.81 | 1.81 |
| r3-s2u, samples at 1.82 | 59 % | 54 % | 60 % | 56 % | 52 % | 61 % | 94 % | 94 % |

`scaling_cur_freq` is an instantaneous read, so a low sample means the core was idle at that
instant (a schedutil ramp) rather than proving a running thread was at 614 MHz; without root there
is no `time_in_state` to give the residency. What it does show is that **the profile above is not a
fixed-clock measurement**: the A75s are pinned near 1.82 GHz, the A55s average 1.45-1.78, and any
matching of this phone by clock has to pin the clocks explicitly (which these runs did not do — no
setting was changed). The top step was available throughout: `scaling_max_freq` 1.82 GHz on every
core at capture, and no sample ever reported a thermal status other than 0.

Temperatures are `dumpsys thermalservice` medians over the in-window samples (the sysfs zones are
not readable); the battery is `dumpsys battery`:

| | soc-thmzone | cputop0-tzone0 | cputop1-thmzone | gpu-thmzone | battery | Thermal Status |
|---|---:|---:|---:|---:|---:|---:|
| idle, after 4 min rest | 44.4 | 44.4 | 43.9 | 43.2 | 35.0 | 0 |
| r1-s1p | 64.9 | 62.3 | 62.3 | 58.6 | 35.6 | 0 |
| r2-s1u | 68.0 | 64.9 | 64.9 | 61.2 | 36.5 | 0 |
| r3-s2u | 57.4 | 56.1 | 56.1 | 53.7 | 36.5 | 0 |

r3 is the coolest of the three because it ran at a quarter of the rate (1.59 busy cores against
2.46 and 2.87). The phone is not thermally limited at these rates: it idles warm (44 °C) and
reaches 68 °C at the heaviest load with no change in thermal status, well under the 110 °C
throttling thresholds `dumpsys thermalservice` reports for these zones.

## Comparison with the last measured numbers

The last phone measurement (2026-10-28, finding 112, Mi 11i, same uncapped recipe, GO-anchored
3,241 fields) is **115.2 fields/s at 1x and 93.0 at 2x**, with the big cores at 2.112/2.150 GHz.
This phone: **36.3 and 14.4** against its 1.82 GHz top step.

If the rate scaled with clock, 0.315 of the 888's 1x rate and 0.155 of its 2x rate at 0.85 of its
clock would make the T612/Mali-G57 pair about **2.7x slower per clock at 1x and 5.5x at 2x** than
the 888/Adreno 660. Two cautions on that: the T612's A55s actually averaged below 1.82 GHz (table
above), so using the top step overstates the clock the work ran at and understates its per-clock
rate; and rate is not linear in clock (memory latency and the GPU's fixed costs). Treat 2.7x/5.5x
as the shape of the gap, not a measurement. The 2x gap is the graphics side (the 888 has the
Adreno 660 and, on that build, the per-pass framebuffer work findings 109/112 removed was already
cheap); the 1x gap is the EE and VU threads on two A75s against one X1 plus three A78s.

Consequence for the next round: matching this phone by throttling the Mi 11i cannot be done by
clock alone — on the linear assumption the 888 would have to run at about **0.65 GHz at 1x** and
**0.33 GHz at 2x** to reach 36.3 and 14.4 fields/s, and no big core offers those steps; the match
will have to come from disabling cores or capping the GPU, or from accepting a rate target rather
than a clock target. The per-thread profile above (EE 14.9 ms, VU 12.2, GS 13.0, renderer 9.4 at
1x) and the 2x GS/present profile are what to compare against when it is throttled.

## What did not complete

- **Run 4 (2x paced 60) was not run.** The matrix was cut to three runs; run 3 finished before
  that, so its data is complete.
- **The calibration capture failed.** It was a 1x paced-60 run with `DHD_RSTAT=1` and
  `DHD_GPU_TIMES=120` over `DHD_BENCH=3400-6400`. The app was launched while the screen was off (a
  `KEYCODE_POWER` had just turned it off), so the activity went to the background at field 156: the
  log has `gs: in the background; the game waits` and `freeze: no field for 5 s at field 156`, the
  sampler's last reading was at 60.7 s, and the process did not survive. No `rstat:` block, no
  `gpu_times:` block and no timeline CSV were produced, so **there is no per-part
  present/submit/Z-probe split and no GPU time for any run**.
- What it did log before dying is worth keeping: `rstat: adapter Mali-G57 (Vulkan)
  TIMESTAMP_QUERY=yes TIMESTAMP_QUERY_INSIDE_ENCODERS=yes TIMESTAMP_QUERY_INSIDE_PASSES=yes` and
  `gpu_times: GPU time by pass kind every 120 frames (DHD_GPU_TIMES)`. **This driver supports the
  whole timestamp-query set**, so `DHD_GPU_TIMES` is a usable measurement on the Mali-G57; the
  capture simply never got a summary line. (Finding 108 had to conclude the same for the Adreno
  660 but never enabled it.)
- It was not repeated. The `DHD_RSTAT`/`DHD_GPU_TIMES` calibration is the first thing to redo on
  the next phone round.

## Choices made without asking

- The phone on the cable was used as it was (a Realme, not the Mi 11i the findings describe); the
  brief said to benchmark the connected phone.
- A 2,401-field anchored window instead of finding 108's 3,241, because the uncapped runs' GO at
  field 6,022 leaves less than that inside a 9,000-field bench window.
- The image went in over chunked `adb push` rather than the `adb shell` stdin route once the
  latter proved to mangle bytes (verified by md5, twice).
- Cooldowns were 3 minutes with the app stopped and the screen off, between every run.
- No phone setting was changed for performance: the governor and thermals were only read.

## Raw data

Under `build/bench/android/raw` (not committed; the logs hold the navigation and pad scripts, no
game data): `r{1,2,3}-*` logs, `*-timeline.csv`, `*-samples.json` and `*.ini`, the aborted
`cal-s1p*` set, `hw.txt`, `idle-thermals.txt`, `analysis.txt` and `analysis.json`. The runner and
analysis scripts are `build/bench/android/run_phone.py`, `analyze.py`, `push_iso.py`.
