# 206: the online test tools drive their riders; the room card's state beside its tags

Status: in progress

Branch `fix/online-test-riders-and-room-card` from `main` 9b919298, started 2026-11-11. This file
is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

A. The online test tools' scripted riders take input. `DHD_RACE_PAD` applies only with
`DHD_RACE_SYNC=1` (`port/hle/autopad.c`), which `mosh_round_check.py` set only offline and
`cross_race.py` never: their online riders neither pedalled nor attacked (finding 204,
Surprises). Done when `python port/tools/mosh_round_check.py` (its default run) passes three times
in a row with a human scoring on another human's rider, and `docs/TESTING.md` says how scripted
pads and posted keys combine.

B. The room page's player card draws a racer's state (STARTING, READY, ...) over the HOST or YOU
tag on its first line. Done when no state the line can show overlaps the name or the tags, at
widescreen and 4:3 and the smallest name size, with captures before and after at `DHD_SCALE=4`.

Out of scope: the puppet code (lane 202), the state trace (lane 205), offline play.

## Progress

- [ ] 2026-11-11: finding reserved

## Decisions

## Surprises

## Result
