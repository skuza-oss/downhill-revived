# 262: three small fixes after 0.8.7: the guard in a shared folder, 258's Result, software GPUs

Status: in progress

Branch `fix/small-262` from `main` e9bba855 (v0.8.7), started 2026-11-16. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

1. **The pipeline guard and two games in one data folder** (seen by the 0.8.7 release job): a
   second game started from the same folder read the first game's live `pipelines-building.txt`
   as a dead run's, and later could not write it (`Access is denied`). Two such starts in a row
   could refuse a healthy pipeline (`!KEY`) and, for a mesh key, turn Native VU off. Wanted: a
   key in the marker counts only when the run that wrote it is over; a second game in the same
   folder does not write the guard's files; a failed write is logged once and never becomes a
   refusal. Unit tests, and on the PC (hidden) two games from one folder, twice in a row, leave
   no `!KEY` and no `?KEY`.
2. **Finding 258's Result** is filled in from its Progress and the 0.8.7 release job, and
   `port/tools/host64_selftest.py`'s Linux variant passes with the two-part guest layout (or is
   deleted if nothing uses it).
3. **A software adapter** (WARP / Microsoft Basic Render Driver, llvmpipe, SwiftShader): the
   player is told plainly why the game is so slow, in the game and in the launcher when it can
   know; never on a real GPU. A unit test for the decision; a hidden run forced onto WARP shows it.

`check.py quick`, `port`, `docs`; boot and race state traces equal to the base.

## Progress

- [x] 2026-11-16: finding reserved
- [ ] the guard's lock (job 1)
- [ ] finding 258's Result, host64_selftest's Linux variant (job 2)
- [ ] the software adapter's notice (job 3)
- [ ] checks, state traces, hidden runs

## Decisions

## Surprises

## Result

Filled in at the merge.
