# 213: stack locals that the EE frame makes one object (the class of 094 and 207)

Status: in progress

Branch `fix/stack-overrun-sweep` from `main` b8c4cf88, started 2026-11-12. This file is the lane's
plan while it runs and its finding when it merges. Someone who has only this file and the git log
must be able to pick the lane up.

## Goal and acceptance

The retail code sometimes writes (or reads) past a stack local into the one the EE frame puts
after it; in the port clang lays locals out as it likes, so the access lands elsewhere: a crash
when it runs off the fiber's stack slot (094, 207), silent corruption otherwise. Find the rest
before players do and make the class hard to bring back:

- a static inventory of every place a local's address escapes, with the local's size and the EE
  frame around it, narrowed to real overruns, with the counts at each step;
- a test-only build mode that catches such writes at run time, walked through as much of the game
  as the harness reaches;
- each real overrun fixed by a port patch (`src/` keeps matching);
- the static check in `check.py port`, the run-time mode in `docs/TESTING.md`;
- the seven state-trace recipes equal to the format-2 references (or each difference explained).

## Progress

- [ ] 2026-11-12: finding reserved

## Decisions

## Surprises

## Result
