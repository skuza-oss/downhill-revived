# 175: online follow-ups after 0.7.12 (rejoin at a race start, TURN check, clock, flood limit)

Status: in progress

Branch `fix/online-followups` from `main` 9298a71, started 2026-11-07. (174 is taken by
`perf/mali-gpu`.) This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Six follow-ups from the PC<->Mac runs A-D (findings 169 and 173):

1. A player whose room WebSocket drops at any phase (lobby, race start/loading, racing, results)
   gets back into the room and the race; server and client tests for it; whether the abuse guard
   was involved in runs A and C.
2. A PC<->Mac race over forced TURN (both ways, 3+ races each) and 2+ UDP races each way, on the
   final build, with a result table.
3. The shared clock (finding 169's `clock_sample`) under a slow one-way path: a test switch that
   delays this PC's room pings one way, a local two-game race with it, symmetric ages and no
   puppet drawn ahead or behind; the base build for comparison.
4. A bug report draft for Cloudflare Realtime (outside the repository).
5. `tools/worktree_setup.py` leaves `online/server` usable off the system drive.
6. The SFU flood limit (`SenderRate`) keeps the newest STATE messages over the limit.

Done: `python tools/check.py port` and `python tools/check.py server` pass; boot and race state
traces equal 9298a71's.

## Progress

- [x] 2026-11-07: finding reserved

## Decisions

## Surprises

## Result
