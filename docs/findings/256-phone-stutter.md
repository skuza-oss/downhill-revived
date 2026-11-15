# 256: the slow frames on weak phones

Status: in progress

Branch `perf/phone-stutter-256` from `main` c2e92600 (release 0.8.5 plus lanes 247 and 249-253),
started 2026-11-15. This file is the lane's plan while it runs and its finding when it merges.
Someone who has only this file and the git log must be able to pick the lane up.

## Goal and acceptance

The speed lanes (findings 195, 247, 249, 253) raised the average. Their run lines also show what a
player feels on the throttled Mi 11i (`phone_throttle.py --apply t612`, 4x, Native VU on): a p99
frame of about 29 ms, a worst 1 % of 77 to 95 ms over the route, and one frame of about 1.5 s in
every run. This lane lowers the slow frames without changing what the game does.

Method: the base first (p50, p99, the worst 1 %, the frames over 33, 50 and 100 ms and the longest,
for the race start and mid-race, from the runs' `DHD_TIMELINE`); then every frame over 50 ms
attributed to a cause before anything changes; then one change at a time, the largest share of the
lost time first. A change is kept when the count of slow frames falls clearly outside the runs'
spread, the average does not drop and the boot and race state traces on the PC equal the base's.

Acceptance: `python tools/check.py quick` and `port`; boot (600 fields) and race (5000) state
traces equal to `main` c2e92600's; the Android unit tests.

Out of scope: gameplay, the picture's content, players' settings, the average speed (findings
247, 249 and 253), what those lanes tried and dropped.

## Progress

- [x] 2026-11-15: finding reserved
- [ ] the base measured and every frame over 50 ms attributed
- [ ] changes, one at a time
- [ ] the checks

## Decisions

## Surprises

## Result
