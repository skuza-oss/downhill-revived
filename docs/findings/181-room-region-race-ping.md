# 181: the room's region chosen by its creator, a smarter Auto, and a race ping beside the server's

Status: in progress

Branch `feat/sfu-region-ping` from `main` db3702d (0.7.13), started 2026-11-08. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Players, above all in South America, Africa and the Middle East, see a high PING in rooms they
create. That number is the round trip to the room's Durable Object, which runs in about 31
Cloudflare cities, placed once from the creator's country or continent (finding 135); `sam`, `afr`
and `me` host none, so those rooms run on another continent. Race frames never touch the room:
they go through each player's nearest Cloudflare Realtime location (findings 158, 177). So players
judge races by the wrong number.

Three parts, all chosen by the owner:

1. **A race ping beside the server's.** In the room and in races the FPS & Ping counter shows the
   room server's round trip and the delay races actually have, each named so it reads right, at
   most 13 characters a line (finding 169's Rust test). dhd.log keeps lines for both.
2. **The creator picks the room's region.** Create gets a choice: Auto (the default) or one of
   the Durable Object regions that run objects. It becomes the `locationHint`, shows in the list
   and the room header, and dhd.ini keeps the last choice. Protocol: additions only; 0.7.13 keeps
   working against the new server and `MIN_VERSION` stays 0.7.13 (docs/online-protocol.md).
3. **A smarter Auto.** Place by the Cloudflare location (colo) the creator reached, mapped to the
   nearest Durable Object region by a table built from Cloudflare's published locations (and
   measured round trips where possible), falling back to country and continent as now. Unit tests
   for the mapping; replay the last days' real rooms from the logs.

Acceptance:
- Unit tests: server (placement, the region choice, a 0.7.13 hello and create unchanged) and Rust/C
  for the counter text.
- `python tools/check.py port` and `python tools/check.py server` pass; boot and race state traces
  equal db3702d's.
- A temporary test Worker (deleted after): rooms created from this PC with each region and Auto,
  lobby and race pings recorded; a race with the PC and nettool bots, 6+ racers.

Out of scope: P2P rooms (another lane, branch `feat/p2p-clean`), moving a room after it is made.

## Progress

- [x] 2026-11-08: plan (this file)
- [ ] Server: region choice on create, colo-based Auto, `area` names, tests
- [ ] Client: region row and dhd.ini setting, list and header, the counter's lines, dhd.log
- [ ] Checks: `check.py server`, `check.py port`, state traces
- [ ] Test Worker: pings per region and Auto, 6+ racer race
- [ ] Log replay: how past rooms would have been placed

## Decisions

## Surprises

## Result
