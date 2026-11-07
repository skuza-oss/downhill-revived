# 175: online follow-ups after 0.7.12 (rejoin in a race, a racer that never connects, TURN check, clock, flood limit)

Status: in progress

Branch `fix/online-followups` from `main` 9298a71, started 2026-11-07; local main b0812d6 (the
lead's SFU_CONNECT_MS hotfix) merged in at 1246a15. 174 is taken by `perf/mali-gpu`. This file is
the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Six follow-ups from the PC↔Mac runs A-D (findings 169 and 173), and one from the lead:

1. A player whose room WebSocket drops at any phase (lobby, race start, racing, results) gets back
   into the room and the race; server and client tests; whether the abuse guard was involved in
   runs A and C. Added by the lead (0.7.12 live failure, rooms of 4-7): a racer whose setup fails
   must never take the others down, on the client side too.
2. A PC↔Mac race over forced TURN (both ways, 3+ races each) and 2+ UDP races each way, on the
   final build, plus a racer that never connects; result table.
3. The shared clock (finding 169's `clock_sample`) under a slow one-way path: a test switch, a
   local two-game race with it, symmetric ages, no puppet drawn ahead or behind; base build
   for comparison.
4. A bug report draft for Cloudflare Realtime (outside the repository).
5. `tools/worktree_setup.py` leaves `online/server` usable off the system drive.
6. The SFU flood limit (`SenderRate`) keeps the newest STATE messages over the limit.

Done: `python tools/check.py port` and `python tools/check.py server` pass; boot and race state
traces equal 9298a71's.

## Progress

- [x] 2026-11-07: finding reserved (1a8f986).
- [x] Item 1, what happened in runs A and C (below).
- [x] Item 1, server: bf6f1f1 (a racer with `race_rejoin` keeps its race through the drop, the
      rejoin's catch-up, `sfu_restart`), tests `online/server/test/race-rejoin.test.ts` (5).
- [x] Merge of local main b0812d6 (1246a15): `sfu_restart` and the missed-start reset also forget
      `sfuConnectedRace`.
- [x] Item 1 (lead's addition), client: 8715952, `sfu_ready` 10 s after connecting without a
      missing racer's channels (`READY_TIMEOUT`); Rust test
      `ready_without_a_racer_that_never_connects`.
- [x] Item 1, client: ddfe0fc (the SFU lives through the socket drop, the in-race rejoin, rate
      limit retry, backoff, the end of the attempts); tests in `port/net/test_session.c` and
      `test_loopback.c`, Rust test `a_rejoins_full_channel_list_reconciles`.
- [x] Test switches: 29aa442 `DHD_NET_TEST_PONG_DELAY` (item 3), bd4a473 `DHD_NET_TEST_DROP`
      (items 1-2), both in TESTING.md.
- [x] Item 6: ac2c9b6, test `snapshot_flood_keeps_the_newest`.
- [x] Item 5: 32980fa, checked on a fresh worktree on C: (then removed).
- [x] Item 4: the draft is in `realtime-datachannel-ordering.md`, with the lead (not
      in the repository, not sent).
- [ ] Item 3: local two-game race with the pong delay, this build and the base.
- [ ] Live checks of the drop switch (local, then PC↔Mac).
- [ ] Item 2: PC↔Mac TURN and UDP runs, with a racer that never connects.
- [ ] `check.py port`, `check.py server`, state traces.

## What happened in runs A and C (item 1)

From the logs of finding 169's PC↔Mac runs, kept outside the repository (A-machost-r1/r2, A-pchost-try1, C-pchost, C-machost-try1) and the
code at 9298a71:

- Both games lost the room WebSocket at the same moment, during a race's countdown ("get ready",
  then `connection closed (1006)`: `IO error ... forcibly closed by the remote host (os error
  10054)` on the PC, `Connection reset by peer (os error 54)` on the Mac). In C-machost-try1 the
  drop hit both too: the PC, in the room at that moment, logged `rejoining` and was `back in room`
  at once; the Mac, in its race, was not.
- **No rejoin was ever tried in a race.** `client_on_closed` (port/net/client.c) treated any close
  while `racing` as final (`offline`, `NET_FAILED`, the "protocol 1" rule), and the Rust send
  thread dropped its SFU association whenever the socket was down (`if !connected { sfu = None }`)
  and at every `welcome`. The "refused" in the logs (`online menu: refused: connection closed
  (1006: ... 10054)`) is the room menu showing that same drop's own reset after the race closed,
  not a refused rejoin.
- On the server, a 1006 in the member's race made it leave at once (`dropSocket` → `leave`): both
  members went, the room ended with them, so a rejoin could not have found it anyway.
- **The guard was not involved.** No connection was attempted after the drop, so no limit could
  refuse one. Also, the temporary Worker of runs A-D (`wrangler.measure.jsonc`) had no rate
  limiters and no D1: no limits per address, no bans, no kill switch. With production's limits
  (30 connections a minute per address hash, `RL_CONNECT`) two games on one address making the
  client's 5 attempts each stay well under; the server test sends 6 failing tries plus both
  rejoins from one address and all get in. A `rate_limited` refusal during a rejoin is now
  retried.
- The drop itself (both games and, once in A, the wrangler tail's own socket at the same moment)
  points at the shared home uplink or Cloudflare's edge; nothing in the room's logs.

## What the fix does (item 1)

Server (`online/server/src/room.ts`, protocol doc "Limits"):
- A hello listing `race_rejoin` marks the member. Its socket dropping (1006) in its race no longer
  takes it out: it stays away, in the race, for the 20 s grace (REJOIN_GRACE_MS), while its game
  races on over the SFU (the others keep seeing its rider move). Not back in time: out as any
  racer who leaves (`left`, the host's AI takes the rider).
- The rejoin's hello sends `raceStartAt`. The welcome says `race: {startAt, sfu}` (`ready`: its
  session is up, keep it; `setup`: start it again), then what it missed: `left` for each racer gone
  since, its full channel list (`sfu_channels` with `full`) and `sfu_ready` if the race was
  released; or, when the hello names another race or none (it missed the `start`), the `start` again.
- `sfu_restart` (at most 3 per racer per race) clears a session the rejoined game can't keep, so a
  new `sfu_offer` is taken. The setup deadlines still count from the race's start.
- Games without `race_rejoin` (0.7.12) keep the old rule: their SFU died with the socket, so
  keeping them would only show a frozen rider for 20 s.

Client:
- The send thread keeps the association while the socket is down: race frames (STATE, EVENT) are
  still taken (`ws_send`), the SFU's signaling waits for the rejoin's welcome. The welcome keeps it
  (`ready`, same race), or sends `sfu_restart` and starts a new association from the race's saved
  start (`setup`, or the association was lost here), or drops it (no race).
- `client.c`: a drop in a race starts a rejoin; the lobby state stays `NET_RACING`, so the race
  goes on in the game (`online.c` sees nothing); a finish or BACK said while the socket was down
  is said again. A refusal (other than `rate_limited`) or the end of the attempts makes it
  `offline` as before, and the race goes on alone.
- A connection that ends within 5 s no longer resets the backoff (a refusal used to make the
  retries every 1 s forever), and when the attempts end the state becomes `NET_FAILED` (in the
  room it stayed `NET_RECONNECTING` before).

A racer whose setup fails (the lead's addition):
- The server side is b0812d6 (SFU_CONNECT_MS, 15 s), kept through the merge.
- The client: a game said `sfu_ready` only with every racer's channels, so a server without
  SFU_CONNECT_MS still hangs every game until its 25 s deadline and takes them all out. Now
  READY_TIMEOUT (10 s) after its own association connected, with its own channels open, a game
  says `sfu_ready` all the same (logged: `ready without every racer's channels`). The room still
  releases only when every connected racer is subscribed both ways (`missingSubscription`), so no
  event can reach a subscriber before its channel. Against an older server only the broken racer
  is out at 25 s.

## Decisions

- Item 1: the race goes on over the SFU through the drop rather than pausing; the WebSocket only
  carries the room and the signaling. One rejoin path for room and race (the token, finding 137).
- Item 1: `race_rejoin` is a hello feature, not a protocol bump: 0.7.12 games keep working and keep
  the old behaviour.
- Item 1: a session still being set up at the rejoin is started again rather than resumed: the
  answer or the channels may have been lost with the socket, and a new one costs one API call.
- Item 1: no client-side wait beyond the existing 5 attempts (1, 2, 4, 8, 8 s): they cover the
  server's 20 s grace.
- Lead's addition: 10 s for READY_TIMEOUT, longer than a TURN setup (about 5 s) and shorter than
  the room's 15 s SFU_CONNECT_MS, so on a current server it never decides anything.
- Item 3: the switch delays incoming pongs (a slow return path), the direction that skewed run C:
  the Mac's offset was early, its ages too small, as when the pong is late.
- Item 5: the install runs in a staging folder (copies of package.json and the lock) whose
  node_modules is the junction's target, rather than guidance alone: nobody has to remember not to
  run `npm ci`. A rerun installs again only when the lock changed.
- Item 6: over the limit, a message newer than the newest taken still counts, up to as many again
  a second (a flood stays at twice the limit); without a stamp or not newer, dropped as before.

## Surprises

- The client never tried a rejoin in a race at all; the "refused" in the logs is the drop's own
  reset shown later by the room menu (above).
- In the room, a rejoin whose attempts ran out left the client in `NET_RECONNECTING` forever, and
  a refusal reset the Rust backoff to 1 s (the connection had opened), so a `rate_limited`
  refusal would have been retried every second.
- str0m asserts (panics) on a duplicate negotiated stream before the association is up: our
  duplicate check only saw open streams; it now checks our own channel list too.
- `git worktree remove` leaves the junctions (untracked) and their folders; one points into the
  main checkout's `tools/compilers`, so a recursive delete there would delete the main checkout's
  toolchain. `worktree_setup.py --remove PATH` now unlinks them.
- During this lane, creating a junction on F: started to fail with "The file or directory is
  corrupted and unreadable" (any target, from cmd, PowerShell or Python), while a junction on C:
  pointing at F: and plain folders on F: work, and Windows logged no NTFS or disk event. Not this
  lane's to repair; it doesn't affect worktree_setup (its junctions are on C:).

## Result
