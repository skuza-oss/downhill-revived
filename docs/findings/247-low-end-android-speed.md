# 247: more race frames a second on weak Android phones

Status: in progress

Branch `perf/low-end-android-247` from `main` 02a8bb3e (release 0.8.4), started 2026-11-14. This
file is the lane's plan while it runs and its finding when it merges. Someone who has only this
file and the git log must be able to pick the lane up.

## Goal and acceptance

More fields a second in a race on weak Android phones, without changing what the game does. The
product target is 60 fps at 2x on the Redmi 9 (Helio G80, Mali-G52 MC2). That phone is not
attached, so the lane measures on the stand-in finding 174 calibrated: the Mi 11i with
`port/tools/phone_throttle.py --apply t612` (the profile's own clocks) at `DHD_SCALE=4`, Native VU
on (the players' default since finding 198). Finding 174's limits apply: the stand-in is good for
the whole-race gain of a change, not for absolute fps; mid-race the render thread limits it, at the
race start the GPU.

Method: the base first (the published 0.8.4 release APK, which is `main` 02a8bb3e built with the
profile `dhd-197.profdata`), then a profile of where a field goes, then one change at a time, the
biggest measured cost first. A change is kept only when the whole race gains about 3 % or more,
clearly outside the runs' spread, and the boot and race state traces on the PC equal the base's.

Acceptance: `python tools/check.py quick` and `port`; boot (600 fields) and race (5000) state
traces equal to `main` 02a8bb3e's; the Android unit tests.

Out of scope: gameplay, the picture's content, players' settings, anything findings 146-149, 172,
174, 183, 195, 197 and 198 tried and dropped, unless a new idea differs from what they tried.

## Progress

- [x] 2026-11-14: finding reserved

## Decisions

## Surprises

## Result
