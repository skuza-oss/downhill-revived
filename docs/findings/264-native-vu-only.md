# 264: Native VU only: nothing left on the emulated VU1, then the emulated path deleted

Status: in progress

Branch `feat/native-vu-only-264` from `main` e9bba855 (release 0.8.7), started 2026-11-16. This
file is the lane's plan while it runs and its finding when it merges. Someone who has only this
file and the git log must be able to pick the lane up.

## Goal and acceptance

Finding 253 left a work list of what still runs on the emulated VU1 with Native VU on (outside
races, and beside the translated batches). This lane finishes it in stages that each stand on
their own:

1. **Census**: every VU1 program and entry run across the game (menus, the poster maker, a race
   on two mountains, the other event types, a replay, split screen), with counts, native or
   emulated: `DHD_VU1_EMULATED=1` (finding 253) on each scene, as below. The yardstick.
2. **Translate what is left**, largest count first, each proven equal by the existing tools
   (`DHD_NATIVE_VU1_CHECK=1`, `native_vu_pictures.py`). One commit per group.
3. **Only when the census shows zero emulated runs** everywhere it can reach: delete the emulated
   VU1 path, the `native_vu` setting and its rows (settings page, launcher, PLAYING.md, tests).
   If anything cannot be reached or proven, the path stays and this file lists what blocks it.

Acceptance per stage: `python tools/check.py quick`, `port` and `docs`; boot (600 fields) and race
(5000) state traces equal to e9bba855's.

Out of scope: gameplay, the picture's content, speed work.

## Progress

- [x] 2026-11-16: finding reserved; the census counter records VI13 at every `JR vi13` door
- [ ] stage 1: the census
- [ ] stage 2: translations
- [ ] stage 3: the deletion, or what blocks it

## How the census is run

Each scene is the state trace's recipe (`state_trace_run.py`'s routes, pad scripts and seed) in a
window on a hidden desktop (`hidden_game.py`; headless has no Native VU), with
`DHD_NATIVE_VU1=on DHD_VU1_EMULATED=1 DHD_FRAME_CLOCK=1`, Native VU in skip mode as players have
it (no `DHD_EXACT_PROBE`). The game stops at `DHD_STATE_STOP`, which writes no end reports, so the
runs stop just past a 3000-field report and the census takes the last one.

## Decisions

## Surprises

## Result
