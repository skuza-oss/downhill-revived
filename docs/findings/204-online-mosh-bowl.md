# 204: online MOSH PIT rounds run by the game's Mosh Bowl rules

Status: in progress

Branch `feat/online-mosh-bowl` from `main` 1578b6c5, started 2026-11-11. This file is the lane's
plan while it runs and its finding when it merges. Someone who has only this file and the git log
must be able to pick the lane up.

## Goal and acceptance

An online race on the Mosh Pit arena (track 23, `MOSH PIT (ARENA)`, the host's pick only, 128)
ends the way the original game's arena mode ends: the game's Mosh Bowl rules (mode 15), a
two-minute timed contest with its own scoring, then the results. Today the room runs every track
in Single Event mode (12), where the arena has no finish (203).

Acceptance: a two-game online Mosh Pit round in a local room (and one with bots up to the limit)
starts, plays two minutes by the mode's rules with hits scoring, ends by itself, and shows the
same score table on both games and in the room; offline Mosh Bowl unchanged; unit or tool tests
for the score messages and the final table; `check.py port`, `check.py server` (if the server
changed) and `check.py docs` green.

## Progress

- [ ] 2026-11-11: lane started, finding reserved

## Decisions

## Surprises

## Result
