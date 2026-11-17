# 269: watch a running room race from the room (issue #109)

Status: in progress

Branch `feat/room-spectate-269` from `main` 8d956bff, started 2026-11-17. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

A member who waits in a room while a race runs (joined late, or sat it out) can watch that race:
WATCH in the room's bar, the race's track loads, the riders are puppets from the same snapshots the
racers get, and the camera works as finding 122's (best-placed human first, Left / Right cycles, a
finished or departed rider is left after the same delays). The watcher has no rider of its own in
the race: not in the results or the placing, sends no snapshots or events, hits nobody, and never
holds the start, the finish or the close of the race. It returns to the room with everyone else
when the race closes, or by the race menu's BACK TO THE ROOM / LEAVE THE ROOM. No free camera, no
change to how the race plays for the racers.

Acceptance: `check.py quick`, `port`, `server`, `docs`; boot and race state traces equal the base;
three hidden games on a local server (two race, one watches) with the racers' results equal to the
same race without a watcher, the watcher's camera changes logged, the watcher back in the room and
racing the next race, a watcher who leaves mid-race and a racer who leaves while watched; no
refusal, `freeze:` or `crash:` lines, no address or id of another member in any log.

## Progress

- [ ] 2026-11-17: what happens today (below, "Before")
- [ ] server: `watch`
- [ ] game: the watcher's race
- [ ] room page: WATCH
- [ ] checks and the three-game proof

## Before

## Decisions

## Surprises

## Result
