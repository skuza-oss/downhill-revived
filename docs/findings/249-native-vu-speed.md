# 249: Native VU must not cost speed where the GPU limits

Status: in progress

Branch `perf/native-vu-speed-249` from `main` cae6f05c (release 0.8.5 plus lane 247), started
2026-11-15. This file is the lane's plan while it runs and its finding when it merges. Someone
who has only this file and the git log must be able to pick the lane up.

## Goal and acceptance

Finding 247 measured, on the throttled Mi 11i (`phone_throttle.py --apply t612`, `DHD_SCALE=4`), a
whole race at 57.0 fields a second with Native VU on (the players' default) against 60.4 with it
off: the default path is about 6 % slower than the emulated path it is meant to replace, where the
GPU limits. Finding 198 saw the same sign on the Redmi 9 at 2x (-5 %) and on an integrated Radeon
at 4x (-3 %).

Target: on that stand-in, Native VU on is at least as fast as off over the whole race and at the
race start, without changing what the game does. Deleting the emulated VU1 path is not this lane;
this lane removes the speed reason to keep it.

Method: the base first (both settings, interleaved), then a profile of where the difference is,
then one change at a time. A change is kept only when the whole race gains about 3 % or more,
clearly outside the runs' spread, and the boot and race state traces on the PC equal the base's.

Acceptance: `python tools/check.py quick` and `port`; boot (600 fields) and race (5000) state
traces equal to `main` cae6f05c's; the Android unit tests.

Out of scope: gameplay, the picture's content, players' settings, the deletion of the emulated
path, what findings 146-149, 174, 183, 195, 197, 198 and 247 tried and dropped.

## Progress

- [x] 2026-11-15: finding reserved

## Decisions

## Surprises

## Result
