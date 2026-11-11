# 203: Online MOSH PIT closes the game at the race start (issue #84)

Status: in progress

Branch `fix/online-mosh-start` from `main` f2dd2d4b, started 2026-11-11. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

GitHub issue #84: on 0.7.16 an online race on MOSH PIT (Arena) closes the game as it starts,
every time, with 2 or 3 players; other tracks are fine. Reproduce it on main, find the cause, fix
it, and check the neighbours (the room's other special tracks, every player count the room
allows, computer riders on and off, high rider and bike ids): an online race on each starts and
runs to the results. Offline play and deterministic runs stay unchanged.

## Progress

- [ ] 2026-11-11: lane started, number reserved

## Decisions

## Surprises

## Result
