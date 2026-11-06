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
      DTLS - 40 SCTP = 1,062). The bundle header carries a server-clock stamp (item 3 says which
      moment). The riders' full pad refreshes (834 bytes) start at different
      phases (AI rider i at 30 - 3i), so two never share a tick; unit test `test_state_bundle.c`:
      ten riders with eight changing pad words fit at most 3 bundles a tick, the largest 987 bytes,
      never two full refreshes in one tick. Protocol 4 (server and client). nettool's replay and
      bot modes read and send bundles.
- [x] Item 2, 60 a second for each player's own rider: sent every race tick, the AI riders every
      second tick (30 a second); the own rider's full pad refresh once a second (every 60 sends),
      the AI's every 30; the adaptive delay's floor 30 ms (was 50); the SFU client's flood limit
      240 messages a second per bundle index (4x the own rider's 60).
- [x] Item 3, a shared clock: a bundle's stamp is the moment its snapshots show on the server clock
      (the sender's GO there plus its race time), and each puppet places a snapshot on its own race
      clock by that stamp and this PC's GO (`hle_puppet_go`), instead of estimating the sender's
      clock from arrival times. A snapshot's age on arrival is now measured; the interpolation
      delay is that age (0-300 ms) plus the wait for the next one (interval + 2 x jitter, 30-150
      ms). The room's binary PONG carries the server's time (8 bytes after the ping's), and during
      a race the client follows it from round trips no slower than its median, by at most 2 ms a
      PONG (a step would move this PC's puppets on every other screen). Unit test in
      `test_session.c`. The game's `online:` line gives each puppet as "shown behind / snapshot
      age" in ms; `port/tools/puppet_lag.py` (and `online_race.py --lag`) measure independently.
- [x] Item 4, prediction: by default each puppet shows its newest snapshot carried to the present
      (`port/hle/puppet_predict.h`): position along the velocity, the orientation rows turned by the
      world-space angular velocity, at most 250 ms ahead. When a newer snapshot moves the
      prediction, the rider is first shown where it was and the difference fades (1/e in 80 ms); a
      jump over 8 units (a respawn, a long loss) and the first pose after a local crash are shown
      at once. The pad words, State, crash and progress come from the newest snapshot as they
      are. `DHD_PUPPET_PREDICT=0` interpolates in the past as item 3 does. Unit test
      `test_puppet_predict.c` (a straight line, a turn in the right sense, a correction that starts
      where the rider was and fades, a respawn that snaps).
- [x] Item 5, the counter: in a race the FPS & Ping counter's second line reads "RIDERS N MS",
      the slowest other player's median age of their snapshots on arrival (the last 64, players
      heard from in the last 2 s; `net_riders_ms`): the network delay to the other riders and the
      span the puppets are predicted across. Outside a race it stays "N MS PING" (the room's
      server). dhd.log's `net:` lines keep the ping and add each sender's age on arrival ("on
      arrival N ms"). Rust test `online_line_names_what_it_measures_and_fits` (13 characters at
      most, the bitmap's width). `src/abi.rs` gained the counter's and the touch overlay's
      function types, and `test_dhdr_abi.py` reads `f32`/`f64` (both functions were missing from
      the ABI check because it could not).
- [x] The flood limit no longer silences a sender for the rest of the race (Surprises): messages
      over 240 a second per bundle index are dropped, the first drop is logged.
- [x] Checks, measurements, state traces, the Mac check (Result).

## Decisions

- Item 3: the stamp is the snapshot's moment on the race clock carried onto the server clock (GO
  + race time), not the server clock when packed. The first build stamped at packing; the puppets
  then thought themselves ~15 ms nearer the present than the measurement showed, because a race
  clock trails the wall clock by the pacing's few ms ("ms late" 6-14 here). Both sides now compare
  race time with race time, and the game's own estimate matches the measurement (44-53 against
  45-48 ms).

- Bundles are made in the game (`online.c`), not in the transport: the game knows every rider of a
  tick at once, while the transport's send thread wakes per message and would split a tick's
  bundle at random. The transport keeps the newest message per bundle index (byte 3, where the
  rider id was), and its flood limit counts per bundle index.

- 2026-11-06 (the user): all five, then a PC↔Mac comparison of 0.7.11, 0.7.12 and this lane.

- Item 4, which predictor: chosen offline from two recorded paths (`DHD_PUPPET_RECORD`, every
  race step, base race). Error of the pose h after a snapshot against where the rider really was,
  game units (riders move about 95 units a second), p50/p95:

  | h | drawn as it was (no prediction) | velocity | velocity + acceleration |
  |---|---|---|---|
  | 33 ms | 3.0 / 3.7 | 0.04 / 0.22 | 0.05 / 0.28 |
  | 50 ms | 4.6 / 5.6 | 0.06 / 0.41 | 0.08 / 0.55 |
  | 80 ms | 7.6 / 9.3 | 0.38 / 0.80 | 0.36 / 1.16 |
  | 120 ms | 9.6-10.9 / 13.5 | 0.39-0.51 / 1.19-1.25 | 0.52-0.56 / 1.42-1.50 |

  Acceleration from the last two snapshots makes it worse (velocity noise), so velocity only. The
  angular velocity is world-space and turns the rows (R Q^T): one step ahead p50 0.10 degree
  against 0.16 for no turn, the three other conventions 0.13-0.25.
- Item 4, hits and collisions with predicted puppets. Rule kept from finding 044: a hit by this
  PC's rider on a puppet is decided here on the puppet as drawn, and the victim's PC applies the
  EVENT to its own rider as it is when the event arrives; a puppet never hits or is hit locally,
  and riders bump puppets where they are drawn. What prediction changes is where the puppet is
  drawn: interpolating it was 45-90 ms in the past, 3.6-8.8 units (p50) behind its rider's real
  position; predicted, 0.22-0.25 units p50 and 1.3-1.5 p95 from it (table below). So a punch,
  kick or bump lands where the victim really is, about 20 times closer than before, and the victim
  no longer gets hit by someone who on their own screen was a bike length or more away. The rare
  misprediction (a sudden turn, a hit the victim took a moment ago) can put the puppet up to
  about a unit off for one snapshot interval, and a hit during a fading correction lands on a
  pose up to 80 ms into the fade; both are far below the 3.6-8.8 units every hit had before. Not
  chosen: letting the victim's PC check the hit against its own rider (it would refuse hits the
  attacker saw land, and the network delay makes such a check as wrong as the drawing it checks),
  and rewinding ("lag compensation": deciding on the victim's past pose), which only made sense
  while the puppets were drawn in the past.

## Surprises

- Snapshot stalls inside the receiving SCTP stack. In some local runs (the base build's too: age
  p99 380-585 ms in a late window) a sender's STATE messages stop coming out for 0.5-6 s and then
  arrive all at once. The datagrams keep arriving meanwhile (the guest's SFU line: ~100 a second
  in, as usual), and our loop never waits (poll gap at most 34 ms, input wait at most 15 ms): str0m
  holds the unordered messages until something unblocks it, most likely a lost chunk waiting for
  its FORWARD-TSN. Not fixed here (a transport lane: str0m's PR-SCTP receive path, or sending
  STATE so it can't wait); while it lasts both drawing modes hold the rider (interpolation after
  carrying it 0.2 s, prediction after 0.25 s), and it shows as the p99s and the "stalled steps"
  in the tables.
- A stall's backlog froze a rider for the rest of a race: 360 STATE messages (6 s of the host's)
  came out of the guest's SCTP stack at once, over the flood limit of 240 a second for bundle 0,
  and the limit (finding 158 round 2: "silences that member's snapshots and events for the rest
  of the race") silenced the host. 0.7.12 has the same hazard at its 120 a second per rider. The
  limit now drops the excess of that second only (0e9c5fd); the same kind of run afterwards kept
  every rider moving.
- This PC's own rider's pad sender (`hle_puppet_start` began at slot 1) was never reset at a race
  start, while every receiver clears its mirror: pad words unchanged since the last race reached
  the others only at the next full refresh, up to a second into the race. Item 1 resets it with
  the others.

## Result

Five items and one fix on `feat/online-latency`: 108a0cd (bundles, protocol 4), 00ca19e (60 a
second), 94c5462 (shared clock, PONG time, `puppet_lag.py`), a1ed16c (prediction), 106e312 (the
counter), 0e9c5fd (the flood limit), and this file with the docs.

### Local numbers (one PC, two games, the real SFU)

`port/tools/online_race.py --lag` through `local_server.py` (control plane on `wrangler dev`, race
frames through the nearest Cloudflare location), both games minimised at 1x, scripted riding,
finish at 70 s, eight host AI riders, WARP off, the PC shared with other lanes. Messages and ages
from dhd.log's `net:` lines (the 30 s windows between GO and the finish); "behind" and "from the
rider" from `puppet_lag.py` over race seconds 5-68 (both directions: host showing guest; guest
showing host). Riders move about 95 units a second.

| Build | STATE messages a second: host / guest sends | Snapshot age p50 / p95, ms | Puppet drawn behind real time p50 / p95, ms | Distance from the real rider p50 / p95, units | Jerks per 1000 steps (outside stalls) |
|---|---|---|---|---|---|
| Base (fce8a1f, 0.7.12) | 270 / 30 | 5-38 / 27-62 | 92 / 103; 52 / 56 | 8.8 / 10.8; 4.4 / 6.1 | 2.9; 2.4 |
| Items 1+2 (00ca19e) | 90 / 60 | 11-17 / 24-26 | 46 / 52; 48 / 51 | 3.5 / 5.0; 4.6 / 5.5 | 3.2; 1.4 |
| Item 3 (94c5462) | 89 / 60 | 20-24 / 30-40 | 45 / 48; 48 / 51 | 3.9 / 5.1; 4.7 / 5.3 | 4.0; 1.7 |
| Lane, `DHD_PUPPET_PREDICT=0` | 89 / 60 | 21-41 / 36-48 (one window 633: a stall) | 44 / 48; 50 / 56 | 3.7 / 4.9; 4.4 / 5.7 | 1.5; 8.5 |
| Lane, prediction (default) | 89 / 60 | 20-33 / 34-71 | 0 / 5; 0 / 4 | 0.20 / 0.96; 0.18 / 1.02 | 1.5; 2.0 |

The last two rows are the final build (0e9c5fd), one run each, back to back; an earlier pair of the
same code before the flood-limit fix gave the same picture (prediction: 0 ms, 0.22-0.25 / 1.3-1.5
units; interpolation 44-61 ms, 3.8-5.1 units). The base's ages use finding 158's estimator (GO +
race ms), the others the bundle stamp; both are the server-clock age of the newest snapshot
sampled every network tick. With prediction the age no longer adds to what players see: it is
the span the rider is carried across. Every race ended with both riders at 1:10.03 and the same
results on both games; hits landed in every run (the guest's 2-3 punches, the host AI's 9-14 on
the guest), each applied by its victim's PC.

The counter in a race (guest, prediction on): "RIDERS 33 MS", "RIDERS 12 MS" (captured with
`DHD_CAPTURE_PRESENTED`).

### Checks

- `python tools/check.py server`: OK (135 tests, typecheck). `python tools/check.py port -j 6`
  (it runs `quick` first: docs, core, runtime tests with `state-bundle` and `puppet-predict`): OK
  on 106e312. After 0e9c5fd (Rust only): `rustgs.py test -- -p dhd_net -p dhd_render --lib` 14 + 9
  passed, the game rebuilt; the final check runs are listed below.
- State traces (`state_trace_run.py`, the base build fce8a1f against the lane at 106e312, same
  recipe): boot all 600 rows match, race all 5,000 rows match. Offline and deterministic runs are
  unchanged: every change is in online-only paths (`pp.online`, `on.*`, the network library) or
  the counter's text.
- macOS: `rustgs.py check --target aarch64-apple-darwin` cannot run on Windows as it stands: ring's
  C needs Apple's SDK header `TargetConditionals.h` (no worktree on this PC has ever finished that
  target). With ring compiled freestanding as rustgs does for Linux (`-ffreestanding -nostdlibinc
  -DRING_CORE_NOSTDLIBINC=1`) and a stand-in for that one header (only for the check, not
  committed), the whole library checks for aarch64-apple-darwin. The C changes have no platform
  code. A real Mac build is the PC<->Mac run's.

### For the lead

- Protocol 4: the release must raise `MIN_VERSION` to it; 0.7.12 and this lane cannot race
  together (the server refuses the other protocol).
- The PC<->Mac comparison (acceptance 4) is the other worker's run of this build; record it here.
- The receive-side SCTP stalls (Surprises) are worth a transport lane; with the flood-limit fix they
  only hold a rider while they last.
- `rustgs.py check --target aarch64-apple-darwin` needs a decision: ship a stand-in
  `TargetConditionals.h` for Windows checks, or say in `port/render/AGENTS.md` that the Mac check
  runs on the Mac.
- Hits keep finding 044's rule on the predicted pose (Decisions); a victim-side check was not added.
