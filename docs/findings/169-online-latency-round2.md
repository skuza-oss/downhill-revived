# 169: online riders closer to now: bundled snapshots, 60 a second, shared clock, prediction

Status: in progress

Branch `feat/online-latency` from `main` fce8a1f (0.7.12 plus the server's refusal order), started
2026-11-06. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

0.7.12 (finding 158) carries race frames over Cloudflare Realtime SFU. Measured PC↔Mac, another
rider is shown about 80-110 ms behind real time: network ~10-15 ms, the wait for the next snapshot
~17 ms on average (30 a second), and the adaptive puppet delay 49-73 ms. State sync stays
(finding 044): each PC races its own rider, the others are puppets.

The lane does five things, each its own commit or commits:

1. **Bundle a player's riders again.** 0.7.11 sent all of a player's riders in one STATE frame;
   finding 158 round 1 split them into one message per rider when a snapshot was 834 bytes, and
   round 2 shrank them to ~150 bytes without bundling again (a host with 9 AI riders sends 270
   messages a second). Pack riders into messages that each fit one datagram on the unordered,
   no-retransmit channel (a message larger than one datagram is lost whole when any piece is);
   spread the riders' full 834-byte refreshes over different sends so one never pushes a bundle
   past a datagram. Fix the stale comments that claim bundling (`port/net/net.h`,
   `online/server/src/protocol.ts`) to say what is true.
2. **60 snapshots a second for human riders** (each player's own rider); AI riders stay at 30.
   Let the adaptive delay's floor follow the shorter interval (about 30 ms instead of 50).
3. **A shared clock.** The puppets estimate each sender's clock from arrival times. Use the
   server's time (the room already sends `serverTime`; ping/pong round trips give the offset) so
   every PC stamps snapshots on one clock; this lets the delay go lower safely.
4. **Prediction ("show riders at now").** Instead of drawing each puppet in the past, predict it
   forward from its newest snapshot to the present (position, velocity, angular velocity, the
   existing extrapolation code as the base) and blend corrections smoothly when new data arrives.
   Hits and collisions with puppets must not get worse: decide how hits use the predicted pose
   (for example, the victim's PC still decides from its own rider, as today) and say so. A switch
   (`DHD_PUPPET_PREDICT=0`) keeps interpolation for comparison. Crashes, respawns, the grid
   before GO and a rider's finish must look right.
5. **A ping that means something.** The FPS & Ping counter shows the round trip to the room's
   lobby server, which no longer carries race data. Show the time to the other riders instead (for
   example the median snapshot age or the round trip through the SFU), named so players read it
   right; dhd.log keeps both.

Hard rules: no player ever receives another's address, SDP, TURN credentials, address hash or
install id; no lockstep, no shared tick (finding 044); offline and deterministic runs unchanged
(state traces equal); the game's feel unchanged (only how other riders are drawn changes).
Protocol: if the wire format changes, `PROTOCOL` becomes 4 and the release raises `MIN_VERSION`.

Acceptance:
1. `python tools/check.py quick`, `port`, `server` pass; unit tests for bundling (sizes stay under a
   datagram, refreshes spread), the clock offset, and the predictor (straight line, turn, crash).
2. Boot and race state traces equal the base build's.
3. A local two-client race (`port/tools/online_race.py`) with numbers: messages per second per
   player, snapshot age, and how far behind a puppet is drawn, with prediction on and off.
4. The PC↔Mac comparison (another worker runs "before" and "0.7.12"; this lane's build is run the
   same way at the end) is recorded here.

Out of scope: TURN/TLS changes, room placement, phones and the browser.

## Progress

- [x] 2026-11-06: lane plan (this file).
- [x] Item 1, bundling: a PC's riders of one tick go out as STATE bundles (payload version 6,
      `port/hle/state_bundle.h`), first fit largest first, each at most 990 bytes of payload so the
      message with its race marker stays at or under 1,000 bytes, one SCTP chunk (str0m: 1150 - 48
      DTLS - 40 SCTP = 1,062). The bundle header carries the sender's server clock when it packed
      the tick (used by item 3). The riders' full pad refreshes (834 bytes) start at different
      phases (AI rider i at 30 - 3i), so two never share a tick; unit test `test_state_bundle.c`:
      ten riders with eight changing pad words fit at most 3 bundles a tick, the largest 987 bytes,
      never two full refreshes in one tick. Protocol 4 (server and client). nettool's replay and
      bot modes read and send bundles.

## Decisions

- Bundles are made in the game (`online.c`), not in the transport: the game knows every rider of a
  tick at once, while the transport's send thread wakes per message and would split a tick's
  bundle at random. The transport keeps the newest message per bundle index (byte 3, where the
  rider id was), and its flood limit counts per bundle index.

- 2026-11-06 (the user): all five, then a PC↔Mac comparison of 0.7.11, 0.7.12 and this lane.

## Surprises

- This PC's own rider's pad sender (`hle_puppet_start` began at slot 1) was never reset at a race
  start, while every receiver clears its mirror: pad words unchanged since the last race reached
  the others only at the next full refresh, up to a second into the race. Item 1 resets it with
  the others.

## Result
