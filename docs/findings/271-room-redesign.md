# 271: the online room redesigned, and a results screen at the race end

Status: in progress

Branch `feat/room-redesign-271` from `main` 8d956bff, started 2026-11-17. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The race room (`port/hle/online_menu.c`, finding 115) laid out as the four pictures the owner
approved (lane 270's mock-ups v3: room before a race, race live, results screen, room after a
race): one layout for every phase, a header of one small and one large line with the countdown
bar, the track and the room's facts on the left with the room code under them, one list of ten
rows (one member a row, nothing shrinks, names whole up to 12 letters, longer ones cut with ".."),
a button bar (CHANGE RIDER, one large main button, an optional third, LEAVE apart) and one
sentence under it that says what happens next. When a room race closes, a results screen over the
darkened last race picture (8 s, Cross, a click or a tap skips) instead of the RACE OVER card.

Done when: hidden captures of every phase sit beside the mock-ups (`compare-*.png` in the lane's
scratch folder) with every difference listed; no text runs out of its box (the room's layout
check in the log says so, widest names); keys, mouse and touch work as before; a three-game room
race gives the base's results; `check.py quick`, `port`, `docs` pass; boot and race traces equal
the base. Out of scope: the watching strip in the race, the HUD's place on a finisher, the screens
before the room.

## Progress

- [ ] 2026-11-17: plan (this file)

## Decisions

## Surprises

## Result
