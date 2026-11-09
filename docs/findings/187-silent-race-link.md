# 187: a racer whose race link delivers nothing no longer races alone

Status: current

Branch `fix/silent-race-link` from `main` 3518ce84, started 2026-11-09. A hotfix: the server part
deploys first and keeps 0.7.13 games working as they do; the game part ships in the next release.
No change to race synchronization, snapshots or puppets; P2P rooms untouched.

## Goal and acceptance

Production, 2026-11-09 04:50-05:10 UTC, public room A2VUXN (runs in ATL): a player in Brazil
(0.7.13, Windows) raced twice with an SFU association up over UDP, channels allocated and the race
released, and received nothing from the other racers: "0 messages received" for the whole race,
the `SFU 30 s` lines with datagrams in about half of out (Cloudflare's SCTP acks only). The start
handshake (LOADED and START events over the SFU, `port/hle/online.c` `start_step`) got nothing, so
the game waited 75 s and started alone ("countdown starts (waited 75.0 s, +0.0 ms from the agreed
moment)"): a parallel race, the others finishing while this player was halfway down.

1. Find the cause, or say what was ruled out.
2. A racer whose race link delivers nothing from the others is detected within seconds and
   repaired, or taken out of the race cleanly, never left racing a ghost race.
3. No solo start: the agreed start moment reaches every racer over the room WebSocket too; a game
   that never got it does not start alone after 75 s.
4. 0.7.13 games keep working exactly as today against the new server (new fields and messages
   only).

Checks: `python tools/check.py port` and `python tools/check.py server`; boot and race state
traces equal 0.7.13's; server tests for the report, the repair, its failure and the moment over
the room; client tests for the detection and the no-solo-start rule; a forced silent subscriber
(a test switch) on a local or test server: detected, repaired or retired, no solo start; a 0.7.13
release game racing on the new server.

## Progress

- [x] 2026-11-09: the evidence (below), the cause found and reproduced on real Realtime
  (`probe_unannounced_subscription_close`): a4ba8ae2
- [x] 2026-11-09: the server: poisoned sessions replaced, the silent-link report and its repair or
  retire, the start moment over the room, the welcome's features (`test/silent-link.test.ts`, 7
  tests; `check.py server` OK, 223 tests): 8e53665c
- [x] 2026-11-09: the game: the detection (`sfu.rs`, test `a_silent_race_link_is_reported_once`),
  the moment over the room (`client.c`, `NetLobby.race_go_at`, `test_session.c`), no solo start
  (`online_start.h`, `test_online_start.c`, `start_step`); `check.py port` OK: 5bb351ce
- [x] 2026-11-09: runs on a local server (this branch, `wrangler dev`) with real Realtime (below)
- [x] 2026-11-09: boot (600 fields) and race (5000) state traces of the final build equal 0.7.13's
  release `dhd.exe` (`state_diff.py`: all rows match)

## Runs (local server, real Realtime, this PC)

`port/tools/local_server.py -- port/tools/online_race.py --bots 2 --minimized`: two games (host
and guest, the scripted `DHD_NET_CREATE` / `DHD_NET_JOIN` flow) and two nettool bots, 4 racers.

| Run | Guest | What happened |
|---|---|---|
| 0.7.13 | both games the 0.7.13 release (`--exe`) | released 12 ms after the start; countdowns 1.5 s after loading, GOs within 0.4 ms; frames both ways (the guest 1,389 messages by race clock 661); results to both. Nothing new in the room's log |
| repaired | this build, `DHD_NET_SFU_SILENT=1` | "no race frame from any of the 3 other racers 8 s after the release; the room is told"; room: "member 3 hears nothing ... a new session for its race"; the game's new session connected and was released 4.7 s after its report (1 s of it the game's pause); "the start moment came from the room (none over the race link)"; "countdown starts (waited 1.514 s, +0.2 ms from the agreed moment)"; 256 messages received by race clock 121; every ride "the player's choice" |
| retired | this build, `DHD_NET_SFU_SILENT=2` | the same report and new session, silent again: "still silent on its new session", "member 3 out of the race: no Realtime connection"; the guest got "Realtime connection failed: out of the race."; the host's game: "player 3 (Guest) left at race tick 0", its rider the AI's |

The scripted flow has no room to return to: a guest taken out of a race there starts its
countdown at once (`waited 0.000 s`) and rides on alone, as it always did for a racer out of the
race. A game in a room (from the ONLINE menu) leaves the race screen for the room 2 s after the
room takes it out (`state_sync`), the path every "out of the race: no Realtime connection" takes.

## The evidence

The reporter's dhd.log (kept outside the repository), member MIOJAUM: joined A2VUXN at about 04:52
while a race ran; was in the grid locked at 04:56 (its own subscription call is one of the room's
10 that pass, below) but went to the rider screen before the start, so sat that race out; raced at
05:01 (5 racers) and 05:07 (6 racers). Both races: `SFU channels allocated: 10 ([1, 3, 5, 7, 9, 11,
13, 15]; 0 waiting for their id)`, `channels ready 83 ms after the start message`, `race released
314 ms after its start message`, then `0 messages received` to the end; the 05:07 race added
RIDER's channels `[17, 19]`, as silent. The rider exchange waited its full 30 s both times (`waiting
for 4 players' riders`, a 29 s `freeze:` line, every puppet on its default ride), then the 75 s
start wait.

The room's Workers Logs (Durable Object 555c61cd...):

- 04:56 grid pass: 10 `datachannels/new` calls, one per grid member (4 started at once, then 4,
  then 2: the gate of finding 182), during a slow spell (8-10 s each); 4 ran into their 10 s
  timeout ("outcome unknown; inspected later"). MIOJAUM's game logged no `SFU channels allocated`
  then: its call was one of the 4.
- The race (9 racers, MIOJAUM not among them) started meanwhile; the pass ended at 04:56:59
  ("pairs ok 29 unknown 31; inspected 2 adopted 22 closed 4") and the next, at the release, at
  04:57:00: "inspected 2 adopted 16 closed 20". The inspection of MIOJAUM's session found the 18
  channels its aborted call had made (9 publishers, 2 each), none wanted any more (MIOJAUM had
  left the group), and closed them: unrecorded, never announced to its game. The other 2 closed
  were another racer's subscription to MIOJAUM made the same way.
- 05:01 grid pass: 2 calls, "pairs ok 5": MIOJAUM's 4 publishers in one call (200, 3-5 s), and one
  racer's subscription to MIOJAUM; the other 3 racers already had theirs from 04:56. Cloudflare
  answered 200 to everything and gave MIOJAUM ids 1-15: the ids of the 18 it had closed at 04:57.

## The cause (reproduced)

**Closing a subscription its game never had leaves those stream ids dead on that session.**
Cloudflare gives a closed channel's id to the next allocation on the session (finding 177), the
API answers 200 and the session listing shows it `active`, but nothing is ever forwarded on it.

Why: closing a subscription makes Cloudflare reset its SCTP stream (an Outgoing SSN Reset Request,
RFC 6525). A data channel closes when both sides have reset their outgoing stream (RFC 8831 6.7):
the game's SCTP stack answers a peer's reset with its own reset of the same stream, but only for a
stream it has (`port/render/vendor/sctp-proto/src/association/mod.rs` `reset_streams_if_any`:
`if self.streams.contains_key(&id)`). For a stream it never had (no channel made, no data
received) it answers "performed" and sends no reset of its own, so Cloudflare's side of that stream
never finishes closing; the next subscription on that id gets nothing.

Reproduced with the real Realtime API (`port/render/net/src/probe.rs`
`probe_unannounced_subscription_close`, 2026-11-09, three sessions on this PC, UDP):

| Step on subscriber session 0 | Ids | 0 received (of 40 events each) |
|---|---|---|
| subscribed to 1, its game told; closed, its game told | 1, 3 | 40 from 1 |
| subscribed to 1, its game never told; closed | 1, 3 | (nothing to receive) |
| subscribed to 1 again (ids reused) and to 2 (new ids), its game told | 1, 3 and 5, 7 | **0 from 1**, 40 from 2 |
| that pair closed (its game told) and made again on the same session | 1, 3 | **0 from 1** (Cloudflare didn't even reset the streams: "not reset by the SFU in 2 s") |
| a new session for 0, both pairs made | 1, 3 and 5, 7 | 40 from 1, 40 from 2 |

Cloudflare's listing of session 0 after the reuse: `1:state:remote:active, 3:events:remote:active,
5..., 7...`: nothing tells a dead id from a live one, so the room can't see it by listing. Only a
new session clears it. Rebuilding the subscriptions on the same session does not.

What else could be ruled out: not the client's data path (the association was up, Cloudflare acked
every datagram it sent, publications worked); not reused subscriptions as such (the reused pairs of
the other racers worked); not the host change (the host left only at the race's end).

## Decisions

- **The repair is a new session, not new subscriptions.** The probe showed a rebuild on the same
  session lands on the same dead ids again (Cloudflare gives the lowest free ids); only a new
  session clears them. The room's existing `sfu_reset` does that, and 0.7.13 games already handle
  it in a race: their send thread makes a new association from the stored start message.
- **Prevention on the server, for every release (8e53665c).** A session on which the room closes
  subscriptions its game never had (`adopt`'s extras) is marked `poisoned`: the pass makes no
  subscription on it (one would land on a dead id), and `replacePoisoned` resets it at once when
  its member is outside a running race (MIOJAUM's case: sitting the 04:56 race out), during its race
  only if it needs a pair it lacks (`sfuRenew`), else when its race is over. A racer's working pairs
  are never touched mid-race. The listing can't tell a dead id (it says `active`), so marking at the
  close is the only server-side signal.
- **Detection in the game's transport, the report to the room.** The Rust `Sfu` sees every frame;
  `SILENT_AFTER` (8 s) after both the release and the race's startAt (START_LEAD, 3 s after the
  start message) with not one frame of this race from any other racer still in it, it sends
  `sfu_silent` once per association and race. Every game sends its ride (SETUP) at its start and
  LOADED every 0.5 s from its load, so 8 s of nothing is not a slow racer. The room renews the
  racer's session once per race (`SFU_RENEW_MS` 20 s to be acknowledged again; the setup checks
  spare it meanwhile); a second report or the deadline takes it out of the race as at
  `SFU_SETUP_MS`, so its game goes back to the room instead of racing a ghost race. With only one
  other racer the cause could be that racer's upload; the bound keeps the cost one renewal.
- **Features in the welcome, both ways.** The server lists `["sfu_silent","race_go"]` in the
  welcome; a game sends either only to a server that lists them (an older server refuses unknown
  types: `sfu_*` with an error, others by closing the socket). The game lists `race_go` in its
  hello; the room sends `race_go` only to games that asked. 0.7.13 sends neither and gets neither.
- **The start moment over the room is the host's own.** The game that names the moment (the host,
  or once it left the lowest slot still racing: `port/hle/online_start.h` `start_namer`, the same
  rule in the room) sends it to the room once; the room keeps the first valid one per race and
  sends it to the racers that take it, and again in a rejoin's catch-up. The room names no moment
  itself: it can't know who has loaded, and 0.7.13 hosts never send one.
- **No solo start.** A game with no moment `START_GIVE_UP_S` (75 s) after it came to the start
  leaves the race (`back`, the banner "NO START FROM THE OTHERS") instead of starting alone; with a
  moment from the room it starts with the others. The namer change keeps a race whose host left
  during the load from sending everyone back: the next slot names it (before, all started alone).
- **A new association mid-race expects only the racers still in it** (`lib.rs` drops a `left`
  member from the stored start), so a renewed game says `sfu_ready` as soon as it has the others'
  channels, not after the 8 s `READY_TIMEOUT`.
- **Not done: a client-side root fix.** Answering Cloudflare's reset of a stream the game never had
  with a reset of our own (`sctp-proto` `reset_streams_if_any`) would clear Cloudflare's side, but
  after the game's own close of a stream (unregistered on the peer's acknowledgement), the peer's
  reciprocal reset would look the same and draw a stray reset that could kill a new subscription
  Cloudflare already put on that id. The server never closing an unannounced subscription on a
  session it keeps is the safe fix.

## Surprises

- The finding 182 path that adopts an aborted call's channels also closes the ones it doesn't want,
  and those were never announced: that close is what poisons the ids. The protocol doc already said
  so ("never announced: its endpoint never heard of it") without seeing the consequence.
- Cloudflare's listing (`GET sessions/{id}`) can't help the room find a dead id: every channel of
  the probe's poisoned session was listed `active`. The second close of the same pair got no stream
  reset from Cloudflare at all (the game reset it itself after its 2 s grace).
- The reporter's game also froze 29 s in each race's rider exchange (`freeze:` lines): that wait
  (`exchange_rides`, 30 s) blocks the game thread by design and only ran out because no ride came
  over the silent link. Not changed here; after a repair a game misses the others' rides (sent once)
  and shows their default bikes and outfits.
- `port/tools/gen_net_layout.py --check` fails on any checkout since d9be6bf6 (`net.h` includes
  `<stdlib.h>`, absent from the freestanding wasm32 check); no check tier runs it. The layout
  itself regenerates fine (NetLobby 5144 to 5152 bytes, `race_go_at` at 5144).

## Protocol additions (protocol stays 4; `docs/online-protocol.md`)

- welcome: `"features":["sfu_silent","race_go"]` (what the server takes).
- hello `features`: `"race_go"` (the game takes the start moment from the room).
- client → server: `{"type":"sfu_silent","race":<startAt>}`, `{"type":"race_go","race":<startAt>,"at":<server ms>}`.
- server → client: `{"type":"race_go","race":<startAt>,"at":<server ms>}`, only to games that
  listed `race_go`; `sfu_reset` mid-race (already understood by 0.7.13).
- Stored state: optional `SfuSession.poisoned`, `Member.raceGo` (only when true), `renewedRace`,
  `renewBy`, `Race.goAt`; the previous server version ignores them.

## For the lead: deploy

1. Server first, before the game release: `python tools/check.py server`, then
   `online/server/tools/deploy.mjs` as usual (10% canary watched in Workers Logs, then 100%). No new
   Durable Object class, no `wrangler.jsonc`, `MIN_VERSION`, secret or var change (`SFU_RENEW_MS`
   is a test override only). 0.7.13 games keep working: they never send or get the new messages
   and ignore the welcome's new field (the 0.7.13 run above).
2. In Workers Logs after the deploy: "N subscriptions its game never had are closed; its session
   is replaced when that costs no race" (the A2VUXN trigger, now harmless), then "session has dead
   stream ids; a new session" or "needs a pair its session can't take"; from new games only: "hears
   nothing from the other racers (sfu_silent); a new session for its race", "still silent on its
   new session", "the race's start moment, from member N".
3. The game part ships with the next release; no `MIN_VERSION` raise is needed.

## Result

Merged into `main` 2026-11-09. A subscription the room closed without telling the game leaves
dead stream ids on that Realtime session: Cloudflare reuses them for later subscriptions, answers
200 and forwards nothing (room A2VUXN, MIOJAUM, 2026-11-09: 0 messages received, a 75 s solo
start). The room no longer subscribes on such a session (a new one instead); a new game reports a
silent link 8 s after the release (`sfu_silent`) and gets a new session in the race, or leaves the
race cleanly; the start moment also travels over the room (`race_go`), and a game without one
leaves the race after 75 s instead of starting alone. 0.7.13 games get the prevention only.

What is left, known now:

- 0.7.13 games never report a silent link: for them the server's prevention is the fix, and a
  silent link from another cause still races alone there until they update.
- A subscription recorded while its member's room connection was down (no `sfu_channels` sent:
  the rejoin's catch-up would) and closed before the rejoin is also closed unannounced; the room
  doesn't mark that session. Rare (an away racer's pairs change mid-race); a new game's detection
  repairs it.
- A false report (a two-racer race whose other racer's upload died) costs one renewal, then the
  reporter is out of the race; with more racers, silence from all of them is the link.
- A game whose session was renewed misses the others' rides if they were sent before the repair
  (sent once): their default rides then, as before.
