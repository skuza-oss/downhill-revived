# 166: Online races on the tracks without a split-screen level (Salt City MX and TD)

Status: in progress

Branch `feat/online-more-tracks` from `main` 1ff885d, started 2026-11-06. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

Players asked for Salt City MX and Salt City TD online. The game has 26 tracks; three have no
split-screen (2-4 player) level file (`port/hle/online.c` `mp_level`: indices 17 CITY MX, 18 CITY
TDH and 21), so the original game's multiplayer lists leave them out, and the online room's
picker (finding 132, which reads the game's own lists) offers 23. Online races are not split
screen: each PC runs its own one-player race and shows the others as puppet riders (finding 044;
finding 123: "Online uses the ordinary single race"), so the one-player level files should do.

Goal: Salt City MX and Salt City TD (and index 21 if it is a real racing track; say what it is)
in the online room's picker, rotation and every screen that names a track (the room, the public
list, results, presence, the track-change notices), playable online with as many players as
their start places allow (finding 123's limits: MX tracks six, TD tracks eight; check the
markers), with nothing else changed. If one of them cannot work online, say why with evidence and
leave it out.

Acceptance: the picker shows them; an online race on each against a local server and bots
(docs/TESTING.md's online tests: start, puppets moving, finish, results, return to the room) on the
hidden desktop; the server's checks (track index ranges, capacity per track, `check.py server`)
accept them; `check.py port` passes; boot and race state traces equal to the base build (offline
untouched).

## Progress

- [ ] 2026-11-06: finding reserved

## Decisions

## Surprises

## Result
