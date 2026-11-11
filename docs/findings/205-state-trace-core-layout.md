# 205: a state trace that does not depend on the core's layout

Status: in progress

Branch `fix/state-trace-core-layout` from `main` 1578b6c5, started 2026-11-11. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

The state trace (`port/hle/state_trace.c`, `port/tools/state_trace_run.py`, `state_diff.py`;
finding 081) of two builds whose game behaviour is equal must be equal, whatever the wasm core's
own layout (where its data, bss and function table entries sit), and it must still report every
real difference. Found while checking lane 203 (finding 203): main 1578b6c5 against its parent
f2dd2d4b gives boot, race, race2 and race6 equal, but race3 differs in 32 of 14000 rows, race4 in
6 and race5 in 5, although no game state differs: the patch moved the core's data by 16 bytes.

Acceptance:
- main against main without lane 203's patch: every recipe (`boot`, `race`, `race2` to `race6`)
  equal, with this lane's trace in both builds;
- a test that flips real game state (a rider word, a RAM word, a register) in a run and shows the
  diff reports it, and one that moves the core's data and shows the diff stays empty;
- `DHD_STATE_NORM=host` works or is gone;
- the players' game is the same game (the trace is a debug facility).

Out of scope: the online rider code (lane 202) and online Mosh Pit (lane 204).

## Progress

- [x] 2026-11-11: finding reserved

## Decisions

## Surprises

## Result
