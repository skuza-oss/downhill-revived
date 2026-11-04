# 147: Unlocked phone measurements must reach the race

Status: current
Checked: 2026-11-04

2026-11-04, from main 418e5f4. Target: 60 displayed fps in races at 2x and
4x on finding 127's frozen low-end profile. Investigation and experiments below.

## Measurement correction

Every speed run uses DHD_FPS=240, DHD_VSYNC=0, DHD_FRAME_CLOCK=1.
The supplied runner is used with --uncapped. No paced speed measurements.
The profile uses big-max 1209600 and little-max 1497600; observed medians
are 1209600/1497600 kHz, online CPUs 0-5, GPU 315 MHz.

The supplied 3400-5800 window does **not** reach GO at 240. Repeated runs
have countdown anchor field 6060 and GO anchor 6321. That window crosses
rider selection and the pre-race sequence. Its PGO perf lines are labelled
menus, 47.0/53.3 fps in one run and 51.9/54.1 in another. They are not
race throughput. The bench and displayed counters agree over the same
window: 62.264 fields/s versus 62.28 displayed fps. There is no factor-of-four
conversion to apply: bench_vsync counts kernel_vsync_count, incremented
once per VBLANK, whereas perf measures actual presents. The scene and
interval must agree before comparing their rates.

Extended runs use bench window 9000-14000, after GO in every recorded run.
They also retain every 30-second race perf line, including the expensive
first stretch before this bench window. A later-window average alone does
not establish holding 60 throughout a race.

## Baselines

Main PGO at 4x, successive race perf lines: 53.4, 36.1, 52.2, 54.1, 54.3,
58.5 fps. The second interval has GPU 19.68 ms/frame and VU busy 19.54
ms/field. Window 9000-14000: 55.878 fields/s, displayed 55.90 fps.

Non-PGO main (the final strip APK from finding 146):

| scale | successive race perf fps | window fields/s | displayed fps |
|---|---|---:|---:|
| 2x | 59.3, 31.5, 54.0, 60.5, 65.5 | 64.040 | 64.05 |
| 4x | 52.9, 31.5, 47.3, 53.0, 54.7, 57.6 | 55.505 | 55.53 |

At 2x the slow interval is VU-limited (21.65 ms busy, GPU 7.47 ms).
At 4x both CPU and GPU matter (VU 22.14 ms, GPU 20.38 ms).
The quoted 53-54 fps starting point describes menus; the actual race target
requires a larger improvement. PGO is separated from the code experiment.

## Clamp-hoisting experiment

Cache clamped matrix registers 1-4 per geometry-kernel invocation, refresh
them after a clipping-kernel reload, and clamp the memory coefficient vector
once before its three component uses. Original registers and arithmetic order
stay intact, and uses keep the FP-environment optimizer barriers. Tested on
the original geometry loop and the 1638 race kernel; neither edit is retained. No global clamp mode
or approximation changed. All edits are in port/runtime.

First version (original loop only): Windows boot 600 and race 5000 rows all
match the baseline. Extended version: boot 600 and race 5000 rows also all match.

A captured race sequence has 1000 runs with zero differences against the
interpreter. It exercises kernel 1638 (51 runs), not the original geometry
loop, as well as the other native kernels. This corrects the assumption that
the old menu profile necessarily identifies the race's hot kernel.
The 60 Hz correctness-recipe draw dump at fields 3000-3010 is byte-identical
for both versions. SHA-256 of the draws CSV:
fdbdd42ae13b2a077e038b13ef11e989a7caaa7e17a7eb1ac3868fdc571b2e29.
These are correctness runs; their bench stopwatch output is not speed evidence.
At 240 the independently launched draw dumps differ even with the exact probe;
no equivalence claim is made for those captures.

Host runtime tests: GS memory/GIF/CLUT, fragmented transfers, VU1, VIF,
graphics queue, gamepad and touch all pass. No screenshot speed judgments.

## Decision and measured effect

Rejected both clamp-hoisting edits: there is no throughput gain.

| scale | baseline -> candidate, window fields/s | change | slow race perf fps |
|---|---|---:|---|
| 2x | 64.040 -> 63.918 | -0.19% | 31.5 -> 31.1 |
| 4x | 55.505 -> 55.293 | -0.38% | 31.5 -> 31.4 |

At 2x window VU CPU is 13.264 -> 13.229 ms/field; at 4x it is
14.434 -> 14.396. These small savings do not improve throughput. No
performance code is retained. The candidate patch and signed APK are
kept only in scratch for reproduction. Only this finding and STATE change.
The goal of holding 60 throughout races is not achieved.

The actual race still needs substantial CPU work removed (the slow VU
interval is 21-22 ms without PGO), plus GPU work at 4x (about 20 ms).
Many native programs run there; hoisting clamps in one kernel does not
address the complete VU workload.

## Actual-race diagnostic profile

Main's PGO APK, 2x, unlocked, requested simpleperf cpu-clock sampling at
1 kHz for 25 seconds starting 141 seconds after launch. The app reaches
the field-9000 stop about 22 seconds later, so the recording ends with the
process. This diagnostic's stopwatch and perf lines are excluded from A/B
speed results. Symbols use that APK's matching debug library.

CPU sample shares: game 24.3%, VU 24.8%, GS 23.7%, renderer 20.8%.
VU runs on the two A78s for 82% of its samples; GS for 20%, renderer 17%.
PGO inlines native kernels into ps2_vu1_run (53.6% of VU samples);
vif_feed is 14.5%, native_clip_run 10.1%. Source attribution includes
the original native geometry loop, so the short sequence capture above
is not exhaustive kernel coverage of the complete race. Operand clamp
lines account for about 10%, but the measured hoisting experiment does
not turn that into throughput. GS: native_draw_strip 31.1%, gs_feed 26.0%,
native_draw 10.0%, gs_write 7.6%, memset 5.8%, gs_palette 3.9%, gs_color
2.2%. Renderer: rb_next 9.6%, memcpy 7.3%, GPU-address allocation 3.3%.
Game: ps2_vcallms 13.2%, idle_wait 12.9%, recompiled 0021B8A8 8.8%.
The next useful work is a kernel-by-kernel VU source profile across the
slow race interval, VIF unpack work, and the GS strip-recording hot path;
at 4x GPU shading also needs improvement. This investigation has not
exhausted those larger changes and does not claim the target complete.

## Final state

The throttle tool printed RESTORED and verified every saved value.
dhd.ini MD5 is 9af4f977637895d329ec56b472b53514. The installed APK is
main's release-signed PGO build (code unchanged by this documentation-only
branch), SHA-256 c20a2ba18dfc39882900424c11d9d2d329b8fef7d43c3ec6b4d70eabd9d334e2.
The phone lock was removed. No uninstall or disc-folder access.

## Sources

- Findings 111, 127, 129, 139 and 146; current kernel, FPS, bench, perf and VU
  implementations, and their interpreter comparison harness.
- [PCSX2 VU interpreter](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/VUops.cpp)
  and [floating-point documentation](https://wiki.pcsx2.net/PCSX2_Documentation/Nightmare_on_Floating-Point_Street):
  operand/result clamp semantics are constraints, not permission to disable
  them globally. No external code copied.

Raw APKs, logs, clock samples, traces and draw dumps remain in ignored scratch
and build folders. No game data, keys or machine paths are tracked.
