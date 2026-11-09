# 185: online code we don't maintain, removed

Status: current

Branch `chore/online-cleanup` from `main` 32d8a8cd, started 2026-11-09. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

The owner's decisions (2026-11-09): the game's minimum is 0.7.13 / protocol 4 (`MIN_VERSION` in
`online/server/wrangler.jsonc`); online code that exists only for older releases, for designs
that were replaced, or for one-off measurements goes:

1. the WebSocket race relay in the room (race frames travel over the SFU since 0.7.12, P2P rooms
   over peer connections), and any sender of race frames over the room WebSocket;
2. paths for protocol 2-3 clients and older releases (the "no clientVersion" 0.1.0 path, the
   lockstep frame types' leftovers, comments and branches about 0.7.11 and protocols 2-3); an
   outdated client still gets `client_outdated` (Update Now);
3. the puppet interpolation mode (`DHD_PUPPET_PREDICT=0`): only predict-to-now stays;
4. one-off benchmark code (`DHD_NET_BENCH`'s CSV probes in the client, `bench.h`, `test_bench.c`,
   `cross_bench.py`), keeping the bots and loss knobs the tests use.

Out of scope: the P2P design (`src/p2p.ts`, `p2p-entry.ts`, `wrangler.p2p.jsonc`,
`port/render/session`, `peer.rs`/`p2p.rs` beyond bench counters), the browser's online code beyond
what 1-4 make dead, and any wire format 0.7.13 clients or test-p2p-1 testers use.

Done: `python tools/check.py port` and `python tools/check.py server` OK; boot and race state
traces equal 0.7.13's (`docs/TESTING.md`); a local normal-room race between this build and the
0.7.13 release build passes; the native P2P room tests still pass.

## Progress

- [x] 2026-11-09: finding reserved (0dec6e22)
- [x] 1. The room's WebSocket relay (d33d4789). Nothing sends race frames on the room socket:
  `port/net/client.c` hands STATE/EVENT to the transport, and `dhd_net` (`port/render/net/src/
  lib.rs` `flush`) sends every type-5/6 message to the SFU association, never the WebSocket; the
  browser (`port/web/www/net.js`) offers P2P rooms only, its frames go to the page's peer
  connections (`p2p.js`); nettool and the Python tools go through the same client; only
  `online/server` tests sent them, to check they are dropped. What was left of the relay in the
  room was the PING answer's slot bookkeeping (`slotOf`, `raceSockets`, `findSlot`,
  `rebuildRace`): the room now answers any member's PING with its own bytes and the clock, as a
  P2P room does (0.7.13 never reads the PONG's slot byte), and messages to a race's racers go
  through `sendRacers`. `MAX_BINARY_BYTES`, `FRAME_STATE`/`FRAME_EVENT` and the lockstep types'
  note left `protocol.ts`; `dhd_net` no longer filters race frames arriving on the WebSocket.
- [x] 2. Old versions (2e1878aa): a hello without `clientVersion` is no longer "0.1.0", it is
  unreadable and so outdated (the same `client_outdated` refusal, before the protocol check as
  before); the default minimum is 0.7.13; the server tests run at MIN_VERSION 0.7.13 with
  0.7.12 / protocol 3 as the refused release; the client takes a PONG only with the server's
  clock; the protocol notes about 0.1.0, 0.7.11 and protocols 1-3 went.
- [x] 2b. The room rules for releases before 0.7.4 (adde66e1): every room has the race room
  rules (finding 115); the `session` and `countdown` hello features are ignored (below).
- [x] 3. Puppets (6d74f379): `DHD_PUPPET_PREDICT` and `DHD_PUPPET_DELAY`, the delay estimator
  only they used (jitter, interval) and the interpolator's carry-on branch only they reached.
  `puppet_predict.h` and its unit test are the prediction itself and stay.
- [x] 4. Benchmark (1ec4be46): `bench.h`, `test_bench.c`, the client's CSV probes, echoes and
  fixed seed, `cross_bench.py`, `analyze_netbench.py`.
- [x] Docs leftovers (c23674f1): the server's README, AGENTS.md and load test said "since
  protocol 3".
- [x] The loopback test's fake server answered PINGs without the clock (d2c16d4a), found by
  `check.py port`.
- [x] `online_race.py` bots without a recording, joining before the guest (3bba1374), for the
  mixed check below.
- [x] 2026-11-09: the checks (Result).

## Decisions

- **The PING answer has no slot gate.** The room answered only racers' PINGs and wrote their
  slot into byte 1. The game sends PINGs only while racing, puts its own slot there, and never
  reads it back (0.7.13 neither); P2P rooms answer any member. Any member's PING is answered
  now, with its bytes as sent: the PONG's format is unchanged.
- **The room rules for releases before 0.7.4 went too** (item 2's "older releases"): a room
  without the creator's `session` feature had an immediate host Start, a countdown only when
  every member's hello listed `countdown`, no joins during a race and no racing or full rooms in
  the list. Every client the server admits (0.7.13+, the browser, nettool) sends both features.
  Games keep sending them: a game of this build may meet a server that predates it (the official
  one until the next deploy), which would give it the old rules without them. The room still
  writes `session: true`, so a previous version that loads the room during a rollout
  (finding 184) keeps the same rules.
- **`race_rejoin` stays a feature**: it is a capability, not a version; the browser doesn't have it.
- **The per-peer transport counters stay** (`port/render/net/src/diagnostics.rs`):
  `p2p_crowd.py`, kept, reads the host's to report each peer's path and upload. They were under
  `DHD_NET_BENCH`; their switch is now `DHD_NET_PEER_STATS=1` and their lines `netstats:`.
- **`client.c`'s `relayed` frame queue keeps its name**: the SFU and a P2P host relay the frames.
- **Client tolerance of servers without newer fields stays** (no locations, no `now` in the
  list): it is plain optional-field reading, not a version path.
- `online_race.py` bots without `--replay` echo a host AI rider (as `p2p_crowd.py`'s), so a
  release's games can race with this checkout's client code in one room; nettool's no-op `--bot`
  flag went.

## Surprises

- The room's "relay" had already stopped relaying in 0.7.12: only the PING answer was left of it.
- A room can't hold a 0.7.13 game and a game of this build: `hello.version` names the exact
  executable and the room refuses another (`version_mismatch`, finding 065). The mixed check is
  therefore 0.7.13's games with this checkout's client code in nettool bots (which claim the
  host's version), in a normal room on this checkout's server.
- **A P2P room loses its results when the finish window ends the race** (not this lane's, seen
  on main 32d8a8cd too). In a P2P room of two games and a bot that finished at 1:00, the window
  closed the race at 2:00 (time_up): the host's game logged `online: an event (type 7) could
  not be sent` and showed the results, the guest's game got nothing more from the host and raced
  on until the run ended. Same result twice on this branch and once with main's game and nettool
  (`online_race.py --a DHD_NET_CREATE=p2p --bots 1`, local server). The host's own client stops
  racing before it sends the results event. Left to the lane that replaces P2P rooms (the P2P
  design is out of this lane's scope); SFU rooms are not affected (their results come from the
  server).
- Most server tests used rooms with the old rules (their hellos had no features) and passed
  unchanged under the race room rules; five tested the old rules themselves.

## Result

Merged into `main` 2026-11-09.

Removed (`git diff --stat main`, 41 files, 419 insertions, 1,028 deletions):

| Commit | What | Files, +/- |
|---|---|---|
| d33d4789 | 1. the room's WebSocket relay (`room.ts` slot state, `MAX_BINARY_BYTES`, `FRAME_STATE`/`EVENT`, dhd_net's WS race-frame filter) | 13, +52 -108 |
| 2e1878aa | 2. the 0.1.0 hello path, protocol 2-3 notes, the client's protocol-3 PONG | 10, +57 -60 |
| adde66e1 | 2b. the room rules for releases before 0.7.4 (`countdownReady`, `FEATURE_COUNTDOWN`, every `!st.session` branch, old-rule tests) | 6, +145 -255 |
| c23674f1 | 2. server docs' protocol-3 notes | 3, +9 -9 |
| 6d74f379 | 3. `DHD_PUPPET_PREDICT`, `DHD_PUPPET_DELAY` and what only they used | 5, +24 -50 |
| 1ec4be46 | 4. `bench.h`, `test_bench.c`, the client's probes, `cross_bench.py`, `analyze_netbench.py` | 12, +13 -534 |
| d2c16d4a | the loopback test's fake PONG | 1, +4 -2 |
| 3bba1374 | `online_race.py` echo bots (test tool) | 2, +23 -13 |

`room.ts` went from 2,270 to 2,156 lines;
`port/net` lost `bench.h` and `test_bench.c` and 70 lines of `client.c`.

Kept on purpose: the PING/PONG (the race's clock, protocol 4); `race_rejoin` (a capability);
the per-peer counters as `DHD_NET_PEER_STATS` (`p2p_crowd.py`); the games still sending
`countdown`/`session` (servers before this lane); `session: true` in the room's storage
(rollouts); the client reading optional fields an older server may lack; the loss knobs and bots
(`DHD_NET_DROP_STATE`, `DHD_NET_SFU_DROP_IN`, `DHD_NET_SFU_UDP`, `DHD_NET_P2P_UDP`,
`DHD_NET_TEST_*`, `DHD_NET_LOG_CODE`, nettool, `online_race.py`, `cross_race.py`,
`p2p_crowd.py`). No wire format changed: hello, the room's messages, PONG and the race frames
read the same to 0.7.13 and to test-p2p-1; the P2P server is untouched.

Checks (2026-11-09, this PC, local servers from `local_server.py`):

| Check | Result |
|---|---|
| `python tools/check.py port` | OK (after d2c16d4a; the first run failed `test_loopback`, the fake PONG) |
| `python tools/check.py server` | OK, 216 tests |
| Boot (600 fields) and race (5000) state traces, this build against the 0.7.13 release's `dhd.exe` | every row equal |
| Normal (SFU) room: two 0.7.13 release games and a nettool bot of this branch (`online_race.py --exe <0.7.13 dhd.exe> --bots 1`) | all three raced, the bot finished, both games got the results |
| Normal room: two games of this build and a bot | all raced, results on both |
| P2P room: two games of this build | raced 7 minutes direct over the LAN; nobody finished within the run's 420 s (the scripted riders are slow) |
| P2P room: two games and a bot | raced, host and bot got the results, the guest didn't (Surprises: the same on main) |
| `p2p_crowd.py --bots 5 --turn-bots 1` | 6 racers, all 5 bots raced and finished; paths 4 direct LAN, 1 TURN over TLS; countdown 41 s after the room opened; host 79 % of a core, 148 kB/s up; snapshot age at the host p50 20, p95 35-43 ms; no freeze |

A room can't mix a 0.7.13 game with a game of this build (`version_mismatch`), so the mixed
check is 0.7.13's games with this branch's client code in the bot. Not run: a PC/Mac race (no
online behaviour changed for a game; the server's rules for 0.7.13 games are the ones they
already had).
