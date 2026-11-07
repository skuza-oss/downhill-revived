# 177: one Realtime session per member per room, set up before the race

Status: in progress

Branch `feat/sfu-room-sessions` from `main` 562e627, started 2026-11-07. 176 is taken by
`fix/powervr-oom`.

## Goal and acceptance

Every race built its Realtime setup from scratch from the race's start message: a session per
racer (`sessions/new`, 0.3-0.5 s), its publication after its ICE connect (about 0.9 s), then every
pair's subscription (0.4-2 s each, 3-6 s across continents). Production's releases (2026-11-07):
2 racers 3-7 s, 4 racers 17.6 and 20.4 s; 7-8 racers missed the 25 s deadline before the hotfixes.
Players wait on a frozen load screen meanwhile. The owner chose all three of:

- **A.** One Realtime session per member per room stay, not per race: its session and two
  publications stay for the whole stay, subscriptions stay too, each race adds only the missing
  pairs; a member who leaves (or whose session fails) has its session and the others'
  subscriptions to it closed. Race frames keep their 8-byte race marker; the client resets its
  per-race state each race without tearing the association down.
- **B.** The coming race's missing subscriptions are made at grid lock (the countdown before the
  start message), so a room whose members are all connected is released within about 1-2 s of the
  start message.
- **C.** Whether concurrent `datachannels/new` calls on one subscriber session are safe, tested
  against the real API many times; if so a subscriber's pair calls in parallel (about 4 at once).

Done:
- server tests: sessions kept across races, only missing pairs subscribed, a leave closes its
  subscriptions, the grid-lock pre-setup, the release within the race; client tests: the
  association survives races and per-race state resets;
- `python tools/check.py port` and `python tools/check.py server` pass; boot and race state traces
  equal 562e627's;
- a setup-time table from a temporary test Worker (deleted after) with 6+ racers that really
  connect (nettool bots, the PC and the Mac) over 3+ consecutive races: start message to release,
  first race and later ones; with a racer far away or on forced TURN; with a racer joining between
  races; a member leaving mid-session and a rejoin.

## Progress

- [x] 2026-11-07: finding reserved.

## Decisions

## Surprises

## Result
