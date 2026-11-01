# 135: The server drops a racer who goes silent; host handover mid-race is tested

## A connection that dies without closing

The server removed a racer only when their socket closed or errored (`webSocketClose`,
`webSocketError` in `online/server/src/room.ts`). A connection that dies without a close (Wi-Fi
gone, a laptop lid shut, a router dropping the flow) stays open on the server's side until the
platform notices. During a race the server sent nothing that could make it notice: lobby pings
stop once the race starts. Until then:

- the other players' screens kept that rider as a puppet frozen at its last snapshot, because
  `left` never came, so the host's AI never took the rider over (`check_left`,
  `port/hle/online.c`);
- the room went on relaying frames to the dead socket.

The client already covers the opposite case: it gives up on a server it hasn't heard from in 10 s
(`SILENT_MS`, `port/net/client.c`). While racing it pings every 500 ms (`PING_RACE_MS`) from the
network thread, through loading screens and results too, so a live racer is never silent for long.

Fix: `relay()` notes when each slot last sent any valid frame (`heardAt`). The room's 2 s alarm,
which already ran during races, now drops each racer silent for more than `RACE_SILENT_MS`
(10 s, as the client's rule) through the usual `leave()`. That means `left` with
`reason: "timeout"`, the host handover if needed, then a close with 1001. Clients ignore
`reason` (`client.c`), so no client change is needed. The timestamps start afresh at each race
and after a wake-up (an evicted object has none), so eviction never counts as silence.
`RACE_SILENT_MS` can be overridden through the environment the way `COUNTDOWN_MS` is; the tests
use 1 s.

## Host leaving mid-race

Finding 047 says a guest taking over as host is "coded, not run". The server and client agree
on who it is:

- the server makes `members[0]` the host, the earliest to join of those left;
- race slots are the host first, then join order, so that is the lowest slot still racing;
- the client's `check_left` picks the lowest slot not gone.

A new test checks the server's half: `left` for slot 0, the room snapshot naming slot 1's player
as host, the relay going on between the others, and `end` from the old guest refused while the
new host's `end` works.

## Checks

- `npm test` in `online/server`: 35 of 35, 6 runs in a row. `npm run typecheck` passes.
- Negative control: with the alarm's `dropSilent` call removed, only `drops a racer who goes
  silent without a close` fails.
- Not deployed, and not tried with the game. Two games through the real server
  (`port/tools/online_race.py`) with one killed hard would show the AI takeover end to end.
