# 269: watch a running room race from the room (issue #109)

Status: current

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

- [x] 2026-11-17: what happens today (below, "Before")
- [x] server: `watch` (c40bc979)
- [x] game: the watcher's race (7fc84c08 net client, e7a7f844 race)
- [x] room page: WATCH (e7a7f844)
- [x] checks and the three-game proof (dd468909, `port/tools/room_watch_check.py`)

## Before

What a member in the room during a race got before this lane (server 6b27ad44, game 0.8.9), from
`online/server/src/room.ts` and `port/net/client.c`:

- **Joining mid-race works.** Joins are accepted in every phase up to the room's size; the newcomer
  waits for the next race (`docs/online-protocol.md`, "Settings and phases").
- **The room page** shows RACE LIVE in the START cell, each member's RACING / DONE / OUT tag, and
  "RACE LIVE: PRESS READY TO RACE THE NEXT ONE"; READY and the rider screen work. Nothing else.
- **No race frames reach it.** `startRace` sends `start` only to the grid's members; the
  subscription pass makes pairs only among the race's entrants (`sfuGroup`) and, at the start,
  closes the subscriptions of members sitting the race out (finding 177). The game also drops a
  room snapshot's race for a member that is not `inRace` (`client.c` read_room: back to
  `NET_LOBBY`), so even the room's `started` flag never opens a race screen.
- **At the race's end** it gets `ended` (every member does) and the snapshot with `lastRace`: the
  room page shows the results, and its Ready (pressed during the race) puts it in the next race.

## Decisions

- **A pure watcher, no hidden rider.** The watcher's race keeps the racers' slot numbering: slot 0
  is the racer in lobby slot 0 as a puppet (`hle_puppet_watch`), the other humans and the AI riders
  are puppets as for any racer. So the watcher takes no slot (a full ten-rider race can be
  watched), is in no result, sends no snapshot or event, and hits nobody. The fallback the brief
  allowed (a hidden, frozen rider of the watcher's own) was not needed.
- **The server's `start` with `yourSlot: -1` and `watch: true`.** The watcher gets the race's
  usual `start` (track, seed, roster, `goAt`), then a `left` for every racer already gone and
  `race_go` if the race went. Snapshot stamps are the server's clock, mapped through this PC's
  own GO, so a watcher who comes in late lines up with the racers without anything new.
- **One-way subscriptions.** The room's subscription pass adds watcher <- racer pairs only; no
  racer ever subscribes to a watcher, so nothing from the watcher can reach a race. Watchers are
  kept out of the sit-out prune and out of `sfuGroup`.
- **The rides.** A racer sends its `ride` once at the load screen. The snapshot lists `watching`
  members, and while one is there each racer sends its ride again once a second (an event frame,
  the same message as today), so a watcher who arrives later still dresses every rider. The
  watcher waits up to 5 s for them and then goes on with defaults.
- **The camera is finding 122's** from the first field: `watched` set, the END hold already past,
  `watch_step` choosing the best-placed racing human, Left / Right through `watch_cycle`. The
  HUD's "WATCHING name" and "LEFT / RIGHT - CHANGE RIDER" are 122's.
- **The watcher's finish** comes from the room: finishes in the room snapshot end the watched
  puppets at once (no wait for the rider's own finish line), and when the room's race closes the
  watcher goes back with everyone (`ended`). BACK TO THE ROOM sends `back` (the server ends the
  watching; no `gave_up` since it never raced), LEAVE THE ROOM leaves.
- **Where it is refused.** Not in P2P rooms (the star's hub only connects entrants) and not in a
  scored arena round (`isScoredTrack`); the WATCH cell is not shown there. An older server (no
  `watch` in `welcome.features`) shows RACE LIVE as before.

## Surprises

- **Online results differ from run to run without any watcher.** The AI riders and the scripted
  racers' finishing times change between two runs of the same room on the same PCs (no watcher:
  GUEST 1:49.19, AI 1:06-1:13; with one: GUEST 1:39.11, AI 1:05-1:19), as online races are not
  deterministic (finding 139's clock, frame pacing). "The racers' results equal a race without
  the watcher" is therefore shown structurally: the racers' race has 2 slots in every run, no
  racer subscribed to the watcher, the watcher sent nothing, and host, guest and watcher logged
  the same results in each run.
- **The camera follows the watched player's own camera setting** (finding 122: "the view still
  follows the watched player's camera changes"); the scripted test racers' pictures show a high,
  far view.

## Result

A member in the room while a race runs presses WATCH (the START cell, which said RACE LIVE) and
watches the race from its countdown, or from where the race is when it comes in late, following
one rider with Left / Right, and comes back to the room with the results when the race closes.

Proof, 2026-11-17 on the PC (local server, three hidden games, `room_watch_check.py`, sound off):

- `watch`: the watcher watched HOST (0) -> Ajax (AI) (3) -> GUEST (1) -> Ajax (3) by Right,
  Right, Left, and -> GUEST (1) by itself when Ajax finished; it was back in the room at RACE OVER
  with the racers' results and raced the next race as player 2 of 3. Pictures of its view in the
  lane's scratch folder ("WATCHING GUEST / LEFT / RIGHT - CHANGE RIDER").
- `watcher-leaves`: the watcher's BACK TO THE ROOM mid-race; the racers' race ran on and closed.
- `racer-leaves`: the guest left while watched; the watcher logged `player 1 (GUEST) left` and
  went on watching HOST.
- `none`: the same race without a watcher, for comparison.
- No `refused`, `freeze:` or `crash:` line, and no line giving any game another member's address
  or ids, in any of the four runs.

Checks: `check.py quick`, `port`, `server`, `docs`; boot (600 fields) and race (5000 fields)
state traces equal `main` 524948ba. Server tests: `online/server/test/watch.test.ts`; client:
`port/net/test_session.c`.

Owed: Android and iOS (WATCH by touch), the Mac and Linux in a real race, a real pad's Left /
Right, a race with a real player across the internet on the deployed server.
