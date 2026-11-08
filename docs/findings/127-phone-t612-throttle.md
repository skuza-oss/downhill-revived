# 127: The Mi 11i throttled to a Unisoc T612: where a 1x field goes, and 39.6 -> 55 fields/s

Status: current
Checked: 2026-11-04

2026-10-31, a test-sub-agent round. Finding 126 measured a low-end phone, the Realme RMX3930
(Unisoc T612, Mali-G57 MC1). This round makes a rooted Mi 11i (M2012K11G, Snapdragon 888, Adreno
660, adb `1ae461e`, Magisk) behave like it, so that the 60 fps work for low-end phones can be
measured on the Mi 11i. It then profiles one field at the calibrated profile, and optimises the
biggest costs (see "Optimisations" below).

## Summary

- **`port/tools/phone_throttle.py --apply t612 --big-max 1209600 --little-max 1497600`** matches the
  Realme at 1x paced 60: **39.55 fields/s against 39.61**. The thread split matches too: the game
  thread is busy 25.3 ms against 25.2 ms, with 13.9 ms of CPU against 14.9. That profile is cpu6
  and cpu7 offline, cpu4-5 (A78) at 1.21 GHz, cpu0-3 (A55) at 1.50 GHz and the Adreno held at kgsl
  level 9 (315 MHz). `--restore` put back every saved value.
- **The GPU cannot be throttled far enough.** At its lowest clock the Adreno costs 4.5 ms per
  megapixel (7.7 above 4x); the Mali-G57 MC1 costs 48.9. So the GPU side is emulated with the
  render scale. **Mi ~2x stands for the Realme's 1x, and Mi ~5.4x for the Realme's 2x** (6x
  measured 11.75 fields/s, 4x 24.2; the Realme does 14.4 at 2x).
- **The ~10 ms a field that finding 126 could not place is a sleep, not descheduling.** On the
  throttled Mi 11i the game thread spends a 25.8 ms field like this: 14.0 ms running, 1.1 ms
  runnable but waiting for a core, and **10.6 ms asleep in one `nanosleep` a field**.
  `simpleperf` puts that sleep in `host_pacing_wait` → `host_wait_wall`, the just-in-time pacing
  wait (port/hle/host.c), and finds no other voluntary sleep.
- **Biggest costs against 16.7 ms at 1x paced:**
  1. the pacing sleep, 10.6 ms;
  2. the EE emulation's CPU, 13.9 ms;
  3. on the real T612, the Mali's frame, estimated at 25-27 ms (it is 5.0 ms on the throttled
     Adreno).

  A code change can cut the first two. The third needs fewer or cheaper GPU passes, or a lower
  scale. The VU (13.1 ms), the GS thread (11.8) and the render thread (9.6) each fit in 16.7.
- **Optimised on the frozen low-end profile: 39.55 → 55.01 fields/s at 1x paced 60 (+39 %),
  median field 25.05 → 16.65 ms.** Three commits in port/: two pacing changes and one GS change.
  It does **not hold 60** yet: the mean field is 18.2 ms. The median field already makes 16.7, but
  a quarter of fields are over 20 ms. The game, VU and GS threads are each busy 14-16 ms a field
  on two big cores and four A55s. Thread placement and spin waits were tried and gain nothing.
  What is left is CPU work in the EE, VU1 and GS threads (see "What is left").

## The low-end profile (frozen)

Calibration stopped here at the user's request. This is the profile every optimisation below was
measured on:

    python port/tools/phone_throttle.py --apply t612 --big-max 1209600 --little-max 1497600

| | value |
|---|---|
| cpus online | 0-5 (cpu6 A78 and cpu7 X1 offline) |
| policy0 (cpu0-3, Cortex-A55) | schedutil, max 1,497,600 kHz (min left at 691,200) |
| policy4 (cpu4-5, Cortex-A78) | schedutil, max 1,209,600 kHz (min left at 710,400) |
| GPU (Adreno 660) | kgsl `min_pwrlevel` = `max_pwrlevel` = 9: 315 MHz |
| MIUI services | left running; the watchdog re-asserts the values every 0.5 s |

At 1x paced 60, GO-anchored (2,401 fields), before any optimisation:

| | Realme RMX3930 (finding 126) | throttled Mi 11i |
|---|---:|---:|
| fields/s | 39.61 | 39.55 (38.77 and 39.94 in two repeats) |
| p50 / p99 ms | 24.75 / 46.30 | 25.05 / 50.11 |
| game busy / CPU ms | 25.20 / 14.94 | 25.26 / 13.87 |
| vu / gs CPU ms | 12.15 / 13.01 | 12.81 / 11.82 |

It is within 0.2 % of the Realme in fields/s, and within 1 ms per thread on the game, VU and GS
threads. The GPU side does not match: the Adreno at 315 MHz is about ten times faster per pixel,
so this profile stands for the T612's CPUs, not its GPU (see "Calibration").

## The profile and how it is kept

Target (finding 126): T612 = 2x Cortex-A75 + 6x Cortex-A55, all at 1.82 GHz, Mali-G57 MC1. Mi
11i: cpu0-3 A55 1.8 GHz (policy0), cpu4-6 A78 2.42 GHz (policy4), cpu7 X1 2.84 GHz (policy7),
Adreno 660 315-840 MHz (kgsl, 10 levels).

`phone_throttle.py` (adb + `su -c`):

- `--apply t612 [--big-max KHZ] [--little-max KHZ] [--gpu-level N]` first saves every value it
  touches or checks: cpu1-7 `online`, each policy's governor, min/max, `cpuinfo_*` and schedutil
  tunables, core_ctl `enable/min_cpus/max_cpus`, and kgsl `max_pwrlevel/min_pwrlevel` (plus the
  thermal level, for the record). The 55 values go to a JSON file on the PC (`DHD_SCRATCH`) and a
  copy on the phone. It then takes cpu6/cpu7 offline, writes the policy0/policy4 `scaling_max_freq`
  caps and sets kgsl min = max = level 9. Governors stay schedutil (the T612 runs schedutil too)
  and minimum clocks are not touched.
- `--restore` stops the watchdog, brings every cpu online, writes back the user max requests and
  the kgsl levels, then puts back any governor, schedutil tunable or core_ctl value that changed.
  Taking policy7's only cpu offline exits its governor, so its tunables come back as kernel
  defaults. Finally it reads all 55 values again and fails if one differs.
- `--show` prints the state, the thermal cooling devices that are engaged and the watchdog.

**What MIUI does to sysfs, and how the profile holds.** The phone's thermal and perf services
(`mi_thermald`, `android.thermal-hal`, `perf-hal-2-2`, `joyose`, `powerkeeper`) were **not
stopped**, so the phone keeps its thermal protection. Instead a root shell loop on the phone
(`/data/local/tmp/phone_throttle_wd.sh`, every 0.5 s; 1 s in the first two runs) rewrites any
value that drifted and logs it. Over the round it fixed 22 values, **all of them cpu6/cpu7 brought
back online**. Every fix came within a few seconds of the game starting or exiting; MIUI onlines
the big cores around an app launch. **None fell inside a measuring window**: the fix counter was
the same at the start and end of every window. All 280 in-window samples read cpus `0-5` online
and the GPU at 315 MHz, and the caps showed as the median clocks: 1.498 / 1.21 GHz, or 1.805 /
1.44 GHz in run 1.

**The max that reads back is not the request.** When the round started, policy4 read 2.112 GHz
and policy7 2.150 GHz while idle. Within minutes policy4 read 1.88, then 2.42. policy7's cap is
the kernel cooling device `thermal-cpufreq-7` (level 7 = 2.150 GHz), a frequency QoS cap that sits
beside the user's request. Writing the read value back would leave a user cap of its own until
reboot. So `--apply` writes `cpuinfo_max_freq` and reads back. If the value does not move, the
cap is a kernel cap and the request to restore is the hardware max. If it moves, it was a
userspace write and goes straight back. `--restore` accepts any value between the saved read and
that request. After the round, policy4 reads 2.42 GHz (2.11 at save time, under MIUI's moving
cap) and policy7 still reads 2.150 GHz (the same cooling device). Every other value is identical.

Choices made: the services are kept running (safety; a watchdog keeps the profile).
`stay_on_while_plugged_in` was already 7. Partway through, the user asked for the screen never to
turn off and for other apps to be closed before each run. Since then `screen_off_timeout` is
2147483647 (it was 600000; left so, as asked) and `screensaver_enabled` 0. Cooldowns kept the
screen on (runs 1-3 had used finding 126's screen-off rest). Before every run from run 4 on,
`cool.py` force-stopped third-party and foreground apps (Instagram, Facebook, Chrome, Spotify,
Discord, camera, ...) and ran `am kill-all`. Runs 1-3 had those apps in the background; the
calibration repeated within 2 % with them closed (runs 6 and 7).

## Calibration

Same APK as finding 126 (0.7.5 / 705, the same build, `libmain.so` build ID
`d915ab80471a8ac4cabd5ff2c7c3d9f3749a3c67`), same scene (`bench_race.SCENES['race']`, seed
81257), same ini recipe and the same GO-anchored window, GO+360..GO+2760 (2,401 fields).
`run_phone.py`/`analyze.py` from finding 126 were adapted for this phone. Root adds a sample of
each port thread's `schedstat` (run ns, run-queue wait ns) and last cpu every ~3 s. One run per
adjustment, a 180 s rest between runs.

1x paced 60 (what a player gets):

| | Realme r1 (126) | Mi run 1: 1.44 / 1.80 GHz | **Mi run 2: 1.21 / 1.50 GHz** | Mi run 6 (profile) | Mi run 7 (sched trace) |
|---|---:|---:|---:|---:|---:|
| fields/s (anchored) | 39.61 | 45.52 | **39.55** | 38.77 | 39.94 |
| p50 / p99 ms | 24.75 / 46.30 | 20.06 / 43.77 | 25.05 / 50.11 | 25.09 / 54.92 | 24.97 / 49.05 |
| 1 % low ms | 54.62 | 55.11 | 59.67 | 66.62 | 63.11 |
| pictures over 20 ms | 64.0 % | 50.2 % | 67.4 % | 68.0 % | 66.6 % |
| game busy / CPU ms | 25.20 / 14.94 | 21.93 / 11.72 | 25.26 / 13.87 | 25.77 / 13.85 | 25.02 / 13.44 |
| vu busy / CPU ms | 12.31 / 12.15 | 11.85 / 10.85 | 14.10 / 12.81 | 14.42 / 13.09 | 14.21 / 12.92 |
| gs busy / CPU ms | 13.29 / 13.01 | 10.15 / 10.04 | 11.97 / 11.82 | 11.97 / 11.84 | 12.27 / 12.05 |
| render thread CPU ms (whole window) | 9.44 | 6.73 | 7.24 | 7.57 | 7.29 |
| process ms/field, busy cores | 69.4, 2.46 | 48.1, 1.91 | 55.6, 1.93 | 56.1, 1.92 | 55.6, 1.94 |
| whole bench window fields/s | 35.48 | 39.67 | 34.62 | 34.27 | 34.90 |

Run 6 had `DHD_RSTAT`, `DHD_GPU_TIMES=120` and 20 s of `simpleperf` at 1 kHz on. Run 7 had a
sched_switch trace on the game thread. Both cost ~0-2 % against run 2.

The remaining differences:

- The render thread uses less CPU on the Mi (7.2 against 9.4 ms). That is the Adreno driver
  against Mali's, and the Mi's fast GPU makes its submits cheaper.
- Process CPU is lower (55.6 against 69.4 ms). Part of it is that thread, part is the Realme's
  ADPF session and its A55s.
- The 1 % lows are 5-12 ms worse. The A78 cap shows on the longest fields.

The game, VU and GS threads and the field rate match, and those are what the 1x CPU work will be
measured on.

Uncapped (`DHD_FPS=120 DHD_FRAME_CLOCK=1 DHD_VSYNC=0`), and the GPU:

| | fields/s | field ms | game CPU (spin) | vu CPU | gs busy / CPU | EE wait for GS |
|---|---:|---:|---:|---:|---:|---:|
| Realme 1x (r2) | 36.30 | 27.55 | 25.86 (10.1) | 18.66 | 26.90 / 17.30 | 10.8 |
| Mi 1x (run 3) | 45.98 | 21.75 | 19.85 (6.84) | 17.68 | 19.08 / 17.89 | 5.84 |
| Realme 2x (r3) | 14.36 | 69.62 | 57.14 (41.5) | 18.31 | 69.14 / 17.38 | 53.0 |
| Mi 4x (run 4) | 24.21 | 41.31 | 34.23 (20.20) | 19.87 | 40.90 / 15.17 | 24.53 |
| Mi 6x (run 5) | 11.75 | 85.10 | 66.29 (52.67) | 19.72 | 84.72 / 15.21 | 68.52 |

The Realme is GPU-bound already at 1x uncapped: its GS thread is busy 26.9 of 27.55 ms, blocked
in the renderer. The throttled Mi is not (19.1 of 21.75). That is why uncapped 1x does not match
when paced 1x does. At 4x and 6x the Mi has the Realme 2x shape: the GS thread blocked in the
renderer (busy 41-85 ms on 15 ms of CPU) and the EE spinning on the GS queue.

Per megapixel of picture (640x448 x scale²): the Realme costs (69.62 - 27.55) / 0.86 MPix = **48.9
ms/MPix**. The throttled Adreno costs 4.5 ms/MPix from 1x to 4x and 7.7 from 4x to 6x, so about a
tenth. Interpolating:

- **the Realme's 2x (69.6 ms) is Mi ~5.4x** (5x would be about 61 ms, 16.4 fields/s; 6x measured
  85 ms, 11.75);
- **the Realme's 1x-uncapped 27.6 ms is Mi ~2.3x.**

DHD_SCALE takes whole numbers, so to load the GPU like the T612, use **2x for the Realme's 1x**
and **5x (light) or 6x (heavy) for its 2x**. 2x and 5x were not run, which kept the round to seven
runs. The lowest kgsl level is the GPU floor; nothing lower is offered (kgsl levels 315-840 MHz).

A side note: a fresh install renders at scale 2 on Android as on PC (`native_core.inc`,
`scale_setting = 2`). On a T612 that is the 14 fields/s row.

## Where a 1x paced field goes (run 6 and run 7, calibrated profile)

Mean field 25.79 ms (38.77 fields/s) against a 16.7 ms budget. The threads run side by side, so
their times overlap. The game thread is the chain that sets the rate.

| | ms per field | where it comes from |
|---|---:|---|
| **game / EE thread** | **25.8 wall** | |
| - running | 14.0 (13.85 CPU) | schedstat run 54.3 % of wall; timeline `ee_cpu_ms` |
| - runnable, waiting for a core | 1.1 | schedstat run-queue 4.4 %; 200 preemptions/s |
| - asleep in the pacing wait | **10.6** | the rest; one voluntary switch a field (33/s); `simpleperf -e sched:sched_switch` puts every voluntary switch in `host_pacing_wait` → `host_wait_wall` → `nanosleep` (12.5 % of all switches, the rest are preemptions; futex 0.45 %) |
| - counted waits (VU, GS, ring) | 0.03 | timeline |
| VU1 thread | 13.1 CPU, 14.4 busy | `native_geometry_loop` 19 %, `native_clip_run` 13 %, `unpack_stream` 7 %, `vif_feed` 5 %, `ps2_vu1_run` 5 % of its samples |
| GS thread | 11.8 CPU, 12.0 busy | `native_draw` 33 %, `gs_write` 29 %, `gs_feed` 26 % |
| render thread (C frame, per picture) | 9.6 busy, 19.6 waiting for the next field | Rust picture 4.9 (record 0.69, write 0.37, `encoder.finish` 2.15, `queue.submit` 1.58), present 1.4 (its GPU wait `psubmit` 1.26), show 0.98, pace 0.72, Z probe 1.54 (0.88 + 0.65) |
| GPU (Adreno at 315 MHz) | 5.0 (span 5.8) | `gpu_times`: draw 4.28 in 12 passes, conversions 0.20 in 10, scanout 0.14, present 0.23 |
| GPU on the real Mali-G57 (estimate) | ~25-27 | not measured on the Realme. Its 1x-uncapped run is GPU-bound at 27.55 ms a field; per-pixel: 0.29 MPix x 48.9 = 14 ms plus fixed costs |

Core placement: the game thread is on the A78s (cpu4-5) in 26 of 29 in-window samples. VU, GS and
the render thread move across all six cores (about half the samples on an A55). With only two big
cores and four runnable pipeline threads, the 1.1 ms of run-queue wait and the A55 time are the
cost of the T612's shape. No `DHD_AFFINITY` was tried.

ADPF: the Mi 11i's MIUI has no hint session ("the system has no hint support"); the Realme had one
(4 threads, target 13 ms). The Realme's A75s sat at 1.82 GHz anyway (finding 126), so the session
cannot have been holding its clocks down. The Mi reproduces the same ~10 ms gap without one, so
ADPF is not its cause.

Why the pacing wait sleeps at 25 ms fields: this is from reading the code, not measured.
`host_pacing_wait` (port/hle/host.c:1411) uses the longest recent frame plus 1.5 ms as its work
estimate. More than 10 % of frames here are longer than a field, so that estimate falls back to a
whole period (host.c:1415-1426). The next frame then starts at `due - period` = `shown`, the time
the previous picture is (or was) shown, moved to "a period after the last present" when the
presenter was late (host.c:1443-1467). So on a phone that cannot hold 60, the EE starts its next
field only when the presenter has shown the last one on the 60 Hz grid. The EE's emulation and the
present chain, which together exceed a field, run in series instead of overlapping. The Realme's
gap (busy 25.2 against CPU 14.9 ms) is the same size on the same code. Its cause there is not
measured (no root), but nothing else in this round accounts for it.

simpleperf top functions, all threads (20 s, 38,475 samples; `build/bench/android/raw/p1-perf-report.txt`):

- share of samples: gs 28.6 %, vu 27.9 %, game 27.2 %, native 12.2 %, audio 2.8 %;
- the game thread: recompiled functions `rc_func_0021B8A8` 7.5 % and `rc_func_00213C08` 6.1 %,
  `w2c_core_render` 5.2 % (the SPU2 sound mixer, port/hle/sound.c, not graphics),
  `run_fast_program` 4.2 %, `ps2_vu0_run` 4.1 %, `__kernel_clock_gettime` 3.3 % (the call graph of
  run o6: the last-millisecond spin of `host_wait_wall` in the pacing wait, and `idle_wait`),
  `native0_transform` 2.9 %;
- the render thread: `memcpy` 5.7 %, kernel scheduler and locks ~8 %, `je_free` 2.7 %, the Z probe's
  `dhdr_native_zread_take` 2.4 %, wgpu and the Adreno driver spread out.

## The biggest costs, and which a code change can cut

1. **The pacing sleep: 10.6 ms a field.** Code can cut it. When a phone runs late, the
   just-in-time wait holds the next field until the last picture is shown. With the EE (14 ms) and
   the present chain (~10 ms) in series, that alone makes 25 ms. Overlapping them (no wait while
   frames are late, or a work estimate that keeps the pipeline full) could take the throttled Mi
   to the EE's own ~15 ms. `DHD_LATENCY=0` (the older "wait before VBLANK" pacing) confirmed it:
   54.5 fields/s. Fixed in code by optimisations 1 and 2 below.
2. **The EE's CPU: 13.9 ms of 16.7 on a 1.21 GHz A78 (~1.82 GHz A75).** Code can cut it: the hot
   recompiled game functions (`0021B8A8`, `00213C08`), `render`, the VU0 micro-programs it calls,
   and 3.3 % in `clock_gettime`. With 2.8 ms of headroom, every field over 16.7 drops a 60 Hz
   frame, so this is the next limit once the sleep is gone.
3. **The T612's GPU: ~25-27 ms for a 1x picture (estimated).** On the real phone this alone sits
   over 16.7 at 1x and makes 2x hopeless (69.6 ms). Code can cut it: the frame is 12 draw passes
   plus 10 conversion passes and a separate Z-probe submission. Settings can cut it too, with a
   lower scale. Measure it with `DHD_GPU_TIMES` at Mi 2x (the stand-in) or, better, on a Mali.
   Finding 126 showed its driver offers timestamps.

Below those, the VU (13.1 ms), GS (11.8) and render (9.6) threads each fit in 16.7 on their own.
They share two big cores, though, and half their time is on A55s.

## Optimisations (goal: 1x holds 60 on the low-end profile)

Each one was measured with one 1x paced 60 run on the frozen profile, the same scene and window
as above, with other apps closed. Each was checked with the state trace: `state_trace_run.py`
boot (600 fields) and race (5000) on the Windows build, against the unchanged build's traces. In
every case all rows match, and `state_diff.py` passes. The pacing code moves only wall time; under
the trace's frame clock it is not even reached, but both traces were run anyway. All changes are
in port/; src/ is untouched.

| # | commit | change | fields/s | p50 ms | over 20 ms | what moved |
|---|---|---|---:|---:|---:|---|
| 0 | (base) | the 0.7.5 build | 39.55 | 25.05 | 67.4 % | game thread asleep 10.6 ms a field |
| 1 | `e8725a5` | pacing: in a slow stretch, frames no longer wait for the last picture | **53.10** | 17.04 | 30.2 % | game thread runs 83 % of wall (was 54 %) |
| 2 | `b274e33` | pacing: a slow stretch keeps pace()'s grid, so quick fields repay slow ones | **54.57** | 16.62 | 24.6 % | the GS thread is now the longest (busy 16.5 of 18.3 ms) |
| 3 | `21c93d0` | GS: PACKED vertex registers stored inline, without gs_write's switches | **55.01** | 16.65 | 25.7 % | GS CPU 12.05 → 11.23 ms, busy 16.51 → 15.62 |

How the changes work:

1. When more than one frame in ten is longer than a field, the just-in-time wait
   (`host_pacing_wait`) had fallen back to a whole period of work estimate. The next frame then
   started at the last picture's show time, so the EE's frame and the render thread's picture ran
   in series. A slow stretch now hands each picture on to be shown as soon as it is ready; the
   presenter still keeps pictures a field apart. Fast machines keep just-in-time frames
   unchanged: the slow branch is reached only when the frames are late anyway. Change 1 started
   each field at least a period after the last one's start.
2. Change 2 replaced that with pace()'s fixed grid: after a slow field the next ones start at once
   until they are back on it, and the grid starts again from now only when more than two periods
   behind. The change-1 build repeated at 53.10, 53.17 and 53.10 in three runs (runs o2, o5, o9),
   so change 2's +1.5 is above the noise.
3. `packed()` writes RGBAQ, ST, UV and XYZF2/XYZ2/XYZF3/XYZ3 through an inline `vertex_write`
   with gs_write's exact stores and vertex kick. It skips the out-of-line call and the
   state-register switch (none of them is a state register). The host GS tests (GS memory, GIF,
   transfer, CLUT, capture, VU1, VIF) pass as well.

Tried and not kept (config only, on the change-1 build; nothing committed):

| config | fields/s | why not |
|---|---:|---|
| `DHD_LATENCY=0` (old pacing) | 54.51 | the pointer to change 1 |
| `DHD_AFFINITY=game:4-5` | 50.70 | the pipeline threads are created later and inherit the mask: all four hot threads on two cores, 31-34 % run-queue waits. The game thread's CPU fell to 11.5 ms: an A55 costs it ~3 ms a field |
| `game:4-5,vu:4-5,gs:0-3,native:0-3` | 46.27 | the GS thread on A55s takes 18.6 ms CPU (6.3 on an A78) and becomes the limit |
| `game:4,vu:0-3+5,gs:0-3+5,native:0-3+5` | 53.17 | the same rate; fewer long fields (20.7 % over 20 ms) but the GS thread becomes the limit |
| `game:4,vu:5,gs:5,native:0-3` + `DHD_SPIN_US=0` | 38.56 | VU and GS on one core hand off 19,000 times a second |
| `game:4-5,vu:0-5,gs:0-5,native:0-5` | 50.77 | no gain over the scheduler |
| `DHD_SPIN_US=0` (Android's default is 3000) | 53.10 | no change: the spin costs nothing measurable |

So placement is not the lever. Default scheduling is as good as any pin tried: the work only moves
between threads, because two big cores serve four pipeline threads and the A55s are 2-3x slower
for this code.

## What is left, and what it would take

After the three changes, the mean field is 18.2 ms (55.0 fields/s) and the median 16.65 ms: half
the fields already make 60. Per field:

- the game thread: busy 15.7 ms, of which 15.1 ms CPU (2.4 ms of it spinning on the GS queue),
  so about 12.8 ms of EE work;
- the VU thread: busy 14.2 ms, 13.0 ms CPU;
- the GS thread: busy 15.6 ms, 11.2 ms CPU.

No single thread is over 16.7 on average. The mean is lost in the quarter of fields over 20 ms
(p99 39.5 ms), where the threads queue behind each other on the two big cores. Holding 60 needs
roughly 2 ms a field off each of the three, so that the long fields fit as well. None of this
changes how the game looks or plays; each item is CPU work done in a cheaper way:

1. **VU1 (13 ms):** the HLE geometry and clip loops, `native_geometry_loop` 17 % and
   `native_clip_run` 15 % of the VU thread's samples. These are the candidates for NEON
   vectorisation on arm64.
2. **GS thread (11.2 ms):** what remains of GIF parsing. `gs_feed`'s per-qword path could become a
   specialised PACKED loop that decodes a whole GIF tag's registers at once. Then `native_record`
   (the draw recording, ~34 %).
3. **EE (12.8 ms work):** the VU0 macro code on the game thread (`ps2_vu0_run`,
   `run_fast_program`, `native0_transform`, `vu0c_run`, about 15 %), the hot recompiled game
   functions `0021B8A8` and `00213C08` (~13 %), and `w2c_core_address` (3.5 %). These are in the
   wasm2c core, so they need a core rebuild and recompiler-level work.
4. On the real T612, **its GPU** (~25-27 ms a 1x picture, estimated) would still hold the real
   phone under 60 even with all the CPU work done. Measure it at Mi 2x (its stand-in) with
   `DHD_GPU_TIMES` before relying on the CPU fixes alone.

## The stock Mi 11i at 2x uncapped (2026-10-31, late)

The question: how many fields/s the unthrottled phone gives at 2x now. Two runs, both on the
stock Mi 11i: `phone_throttle.py --show` read cpus 0-7 online and no throttle applied, with
MIUI's own caps left as they were. The recipe is the uncapped one (`DHD_SCALE=2 DHD_FPS=120
DHD_FRAME_CLOCK=1 DHD_VSYNC=0`), with the same race scene, the GO-anchored window (GO 6022,
fields 6382-8782) and other apps closed. Before each run the phone rested 3 minutes, app stopped
and screen off. (a) is the reference 0.7.5 APK; (b) is the perf/phone-t612 build at `21c93d0`
(changes 1-3).

| | (a) 0.7.5 | (b) perf/phone-t612 | (b) - (a) |
|---|---:|---:|---:|
| fields/s | **114.76** | **112.31** | -2.1 % |
| median / 1 % low ms | 8.38 / 16.74 | 8.68 / 17.14 | +0.30 / +0.40 |
| p99 / max ms | 13.68 / 46.08 | 13.53 / 52.52 | |
| fields over 16.7 ms | 2 (0.1 %) | 3 (0.1 %) | |
| fields over 8.3 ms | 1,245 (51.9 %) | 1,557 (64.8 %) | |
| game CPU / busy ms (EE spin) | 8.43 / 6.32 (2.24) | 8.64 / 6.45 (2.30) | +0.21 |
| VU CPU / busy ms | 7.08 / 7.16 | 6.86 / 6.84 | -0.22 |
| GS CPU / busy ms | 5.23 / 8.28 | 4.57 / 8.48 | **-0.66** |
| render thread CPU (whole window) | 4.07 | 3.97 | -0.10 |
| EE wait for GS ms | 2.10 | 2.15 | |
| process ms/field, busy cores | 24.87, 2.80 | 24.35, 2.63 | |
| clocks, in-window median (cpu0-2 / cpu3 / cpu4-6 / cpu7 GHz) | 1.09 / 1.71 / 2.42 / 2.15 | 1.09 / 1.09 / 2.42 / 2.15 | |
| GPU clock (every in-window sample) / GPU busy median | 608 MHz / 89 % | 608 MHz / 84 % | |
| CPU zone / GPU zone / battery °C | 73.1 / 59.1 / 28 | 72.4 / 60.7 / 29 | |

Reading it:

- **Both runs had the same clocks.** The A78s were at their 2.42 GHz top with no cap. The X1 was
  held at 2.15 GHz by MIUI's `thermal-cpufreq-7`, which was already engaged at idle. The Adreno
  was at 608 MHz in every sample: `thermal-devfreq-0` level 4 was engaged at idle, against an
  840 MHz top. Neither run was throttled more than the other, and the temperatures match within
  1-2 °C.
- **The two builds are the same within one run's spread.** The pacing changes 1 and 2 are not
  reached here: uncapped runs use the frame clock, and the slow-stretch branch is in paced play
  only. Change 3 shows as expected: the GS thread does 0.66 ms less CPU a field (-13 %). The GS
  thread is not what limits 2x, though. It is busy 8.3-8.5 of 8.7-8.9 ms, waiting on the
  renderer, and the GPU is 84-89 % busy at its capped 608 MHz. So the saving does not reach
  fields/s, and the -2.1 % is noise: finding 108 saw up to 11 % between two runs of one
  configuration. One run each cannot separate 2 %.
- **Against the last unthrottled number:** finding 112 (2026-10-28) measured 93.0 fields/s at 2x
  uncapped. This round gives **112-115 fields/s, +21-23 %**. That run had its big cores at
  2.112/2.150 GHz (here the A78s ran at 2.42). It used a 3,241-field GO window against 2,401 here,
  and an older build (0.6.4 era, before the PERU fix and the 0.7.x work). So the gain is part
  clocks, part build, part window, and not one cause. What it shows is that **2x uncapped now runs
  well above 60 and just under 120 on a stock 888**: only 0.1 % of fields are over 16.7 ms. A
  120 Hz panel would still get about half its fields late (52-65 % over 8.3 ms).

## Runs (7 calibration and profile, 11 optimisation, 2 stock)

Optimisation runs (frozen profile, 1x paced 60, window 3400-9000, GO 3214, apps closed): o1
`DHD_LATENCY=0`; o2 change 1; o3-o5, o7, o8 the affinity tests; o6 a DWARF call-graph profile
(simpleperf at 400 Hz; 51.7 fields/s with the profiler on); o9 `DHD_SPIN_US=0`; o10 change 2;
o11 change 3. Stock phone, 2x uncapped, window 3400-9000, GO 6022: s2a-ref (0.7.5), s2b-opt
(perf/phone-t612).

Calibration and profile runs:

| run | config | throttle (big / little GHz) | window | GO | notes |
|---|---|---|---|---|---|
| c1-s1p | 1x paced 60 | 1.44 / 1.80 | 3400-9000 | 3214 | start profile; 15 % too fast |
| c2-s1p | 1x paced 60 | 1.21 / 1.50 | 3400-9000 | 3214 | **calibrated** |
| c3-s1u | 1x uncapped | 1.21 / 1.50 | 3400-9000 | 6022 | |
| c4-s4u | 4x uncapped | 1.21 / 1.50 | 3400-9400 | 6022 | apps closed from here |
| c5-s6u | 6x uncapped | 1.21 / 1.50 | 3400-9400 | 6022 | |
| p1-s1p | 1x paced 60 + RSTAT, GPU_TIMES, simpleperf | 1.21 / 1.50 | 3400-9000 | 3214 | profile |
| p2-s1p-sched | 1x paced 60 + sched_switch trace | 1.21 / 1.50 | 3400-9000 | 3214 | the sleep's site |

Temperatures stayed low throughout: CPU zones 44-53 °C, GPU 40-46 °C, battery 29-36 °C. No
thermal throttling changed a measured clock. The throttled phone runs far below its limits.

## What did not work or was not done

- `DHD_PACING_LOG` wrote nothing on Android. The "pacing: log seconds" line printed, but no
  `pace-ee.csv`/`pace-present.csv` appeared in the app's files. The log is opened from the
  wasm2c core's `host.c`, so its `fopen` path presumably does not reach the app's directory. The
  pacing sleep was placed with the sched_switch trace instead.
- Mi 2x and 5x were not run (the GPU mapping above is interpolated), and neither was 2x paced. The
  round was kept to seven runs.
- No GPU time from the Mali itself. The 25-27 ms is an estimate from finding 126's frame times.
- 1x does not hold 60 on the low-end profile: 55.0 fields/s after three changes. The rest is in
  "What is left".
- `dhd.log` in the app's files is the last run's (the 0.6.4 one was overwritten; it was a test log).
  The run CSV `andbench-timeline.csv` was deleted, and `/data/local/tmp` holds nothing from this
  round. The optimised debug APKs were only for the runs. At the end, finding 126's 0.7.5 (705) APK
  was installed again with `install -r`, which keeps the app's files. The disc image is unchanged
  (md5 `3654a6f4438c899be4a1f5236cfdcd74`).

## Phone state at the end

`phone_throttle.py --restore`: every cpu online (`0-7`). policy0 schedutil 0.691-1.8048 GHz,
policy4 0.7104-2.4192 GHz (2.112 at save time, MIUI's moving cap), policy7 0.8448-2.1504 GHz
(thermal cooling device). kgsl `max_pwrlevel` 0 / `min_pwrlevel` 9. schedutil tunables and
core_ctl as saved. The watchdog is stopped and removed. `dhd.ini` is put back byte for byte (884
bytes, md5 `e61ff162d66a8e57d0f769413d62df33`, mode 600, the app's owner). `screen_off_timeout`
is left at 2147483647 at the user's request (it was 600000).

## Raw data

`build/bench/android/` (not committed): `run_phone.py`, `analyze.py`, `profile.py`, `cool.py`,
`push_iso.py`. Under `raw/`: each run's `.ini`, `.log`, `-timeline.csv` and `-samples.json`; the
saved throttle states `throttle-saved*.json`; `p1-perf.data` with `p1-perf-report.txt`;
`p2-sched.data` with `p2-sched-report.txt`; `o6-prof-perf.data` with `o6-report.txt`;
`restore*.txt`; `saved-dhd.ini`. The state traces are under `build/state-trace/` (`base-*`,
`opt1-*`, `opt2-*`, `opt3-*`).
