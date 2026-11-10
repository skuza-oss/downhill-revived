# 195: the phone's biggest remaining costs, measured and cut

Status: in progress

Branch `perf/phone-fable` from `main` f06ffba, started 2026-11-10. This file is the lane's plan
while it runs and its finding when it merges. Someone who has only this file and the git log must
be able to pick the lane up.

## Goal and acceptance

The owner's minimum target on Android is 60 fps at 2x on a Redmi 9 (Helio G80, Mali-G52). The
Redmi 9 is not attached, so this lane measures on the Mi 11i (Snapdragon 870) with the stand-ins
earlier findings validated:

- the Mi 11i throttled to the t612 profile at 2x (`port/tools/phone_throttle.py --apply t612`,
  findings 146-149, 172, 194): the CPU-bound stand-in;
- the Mi 11i at 4x, unthrottled (finding 174): the GPU-bound stand-in.

The work: a fresh profile of `main` in a race (start and middle) on both stand-ins, with Native VU
off (what players get) and on; the costs ranked; the biggest ones that can be fixed, fixed; each
gain proven by timed runs (uncapped, the screen on, other apps closed, before/after/before
interleaved).

Acceptance:

- Each change keeps the game's state: boot and race state traces equal to `main`'s.
- The picture stays correct (identical where the change is meant to be invisible).
- Each kept change has a measured gain on a named stand-in, Native VU off and on.
- `python tools/check.py port` passes; the release APK builds.

Out of scope: gameplay, the desktop builds' speed (kept equal or better), anything findings 168 and
172 dropped, unless a new idea differs from what they tried (said where it does).

## Progress

- [x] 2026-11-10: finding reserved

## Decisions

## Surprises

## Result
