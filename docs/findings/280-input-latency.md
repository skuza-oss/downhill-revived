# 280: input latency (issue #123): where a press waits, and what of it is ours

Status: in progress

Branch `perf/input-latency` from `main` d170cb59, started 2026-11-23. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

A player on Windows (issue #123): "at 60 fps the game runs so smooth, but there is a noticeable
input lag in the controls"; the owner feels it too with a pad. Map the path from a button to the
picture as stages, measure each in fields and milliseconds, remove the delay that is the port's,
and say what is the original game's own. Default PC settings first (60 fps, V-Sync on, native
renderer, a pad through SDL), then 120 fps, then V-Sync off.

Done when: a before/after stage table from `DHD_INPUT_LATENCY=1` on the same hidden-desktop run,
`check.py port` and `check.py docs` pass, and the boot and race state traces equal the base
build's. No gameplay or feel change, no change to the online rules.

## Progress

- [x] 2026-11-23: the path mapped (below); `DHD_INPUT_LATENCY=1`, one dhd.log line per sampled
  press of player 1 with the time in each stage (`port/hle/host.c`)

## Decisions

## Surprises

## Result
