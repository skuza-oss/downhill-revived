# 177: one Realtime session per member per room, set up before the race

Status: current
Supersedes: parts of 158 (its per-race Realtime sessions, set up from the race's start message) and of 175 (the rejoin's session restart, `sfu_restart`)
Checked: 2026-11-07

Finding 178 reviewed this design before 0.7.13; finding 179 fixed the three risks it left open
(notes below, under Sessions, Subscriptions and Rejoin).

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

- [x] 2026-11-07: finding reserved (15b35e1).
- [x] Question C against the real API, and the stream id reuse it found: 26dc9e0 (sctp-proto takes
      Cloudflare's stream resets in order), probe `probe_parallel_subscriptions`.
- [x] Server: 55dbca2 (sessions per room stay, the pre-setup, parallel subscriptions, protocol
      doc). Tests `online/server/test/room-sessions.test.ts` (5; finding 179 added four on failed
      subscription closes) and the updated SFU tests.
- [x] Client: 1f86802 (one association per room stay, per-race state, replacement after a failure,
      TURN renewal, stream id reuse). Rust tests `the_association_outlives_races_and_each_race_starts_its_own_state`,
      `a_closed_subscriptions_stream_id_serves_the_next_one`.
- [x] d8706ad: the client logs when a race's channels are ready and when it is released.
- [x] 5f749f2: a closed channel whose reset beat the room's `sfu_closed` (live run R2); Rust test
      `a_closed_subscriptions_stream_id_serves_the_next_one_reset_first`, probe
      `probe_publisher_leaves`.
- [x] `check.py port` and `check.py server` pass; boot and race state traces equal 562e627's (600
      and 5000 rows).
- [x] Live runs R1, R2 (failed, the bug above), R2b, R3 and an idle check on a temporary test Worker
      (deleted after).

## Design

**Sessions.** A member's game makes its association when the room's `welcome` comes and keeps it
until it leaves the room. Its `sfu_offer` makes the session (`sessions/new`), its `sfu_connected`
the two publications (`state`, `events`); between races nothing more happens. A later offer
replaces the session: the room closes the old one first (its channels, the others' subscriptions
to it with `sfu_closed`, its TURN credentials). Offers are capped at 6 in 5 minutes per member.
The client offers again after a failure, a lost association, a `sfu_reset` or a TURN renewal,
pausing 1 s, then twice as long each time up to a minute, back to 1 s once one connects. Since
finding 179 the SFU's address is looked up on a thread of its own, not on the send thread: a
replacement uses the last address found at once, the run's first association waits at most 300 ms
(`LOOKUP_WAIT`), and an attempt without an address fails and is retried after the pause.

**Subscriptions.** Made for a race's racers in one coalesced pass: at the grid's lock (the
countdown; rooms with the old rules: their countdown), at the start for what is missing (a racer
connected since), and when a racer of the coming or running race publishes. They stay after the
race. Each subscription remembers the publisher's session (server side only); one to an earlier
session of that member is stale and the pass closes it. At a race's start the subscriptions held
by members outside it are closed, so a member sitting a race out is sent no race frames
(egress is what Realtime bills); they come back at its next grid. A subscription whose close
fails (here, or when a publisher leaves or replaces its session) was forgotten until finding 179:
now it stays on its subscriber's record marked closing, the pass tries it again 5, 10, 20 and 40 s
later, and if it then still forwards a live publication, its subscriber's session is reset
(`sfu_reset`) once that member is outside the running race and the locked grid.

**Release.** After the start the game says `sfu_ready` with the race's marker once its own
channels and every other racer's are open; the room releases (`sfu_ready` with the marker) when
every racer acknowledged and every pair is subscribed both ways. For a room connected before the
lock nothing happens in between but the acknowledgements.

**Per race on the client.** Each `start` begins the race's own state: its racers, its
acknowledgement, queued frames, flood counters (`SenderRate`); `ended` closes it (an ended race's
frames are dropped both ways; a frame the game sends after it is dropped, where it used to fail
the association). The channels and the connection stay. The race marker on every frame tells the
races apart as before.

**Signaling changes** (protocol 4, unreleased; `docs/online-protocol.md`): the welcome always says
`"sfu":"ready"` (the room holds a session for this member: a rejoin keeps its association) or
`"setup"`; `race` in a rejoin's welcome is only `{startAt}`; `sfu_ready` both ways carries `race`;
`sfu_reset` (new) tells a game its session ended; `sfu_restart` is gone (an offer replaces);
`sfu_turn` once per session; `sfu_offer`, `sfu_connected` and `sfu_turn` are taken from any
member, not only racers.

**Rejoin (finding 175).** The session is kept through a room drop, in a race or not. The rejoin's
welcome says `"sfu":"ready"`; its client keeps the association if it has its answer, sends what it
said meanwhile, and gets its whole channel list (`full`), then `sfu_ready` if its race was released
and it had acknowledged. A client without a usable association offers anew. A member not back in
20 s leaves, and its session closes. The client first kept a listed channel by its stream id
alone; since finding 179 it keeps one only if its whole allocation is listed (id, member, name,
direction) and the SFU hasn't reset its stream, and closes the others as `sfu_closed` would, so an
id Cloudflare recycled while the room connection was down opens for its new allocation.

**TURN.** Credentials (TTL 1 h) are minted once per session and revoked when the session ends. A
relay can't take new credentials (RFC 8656: the allocation's username for every request), and its
Refresh and ChannelBind renewals already run (`turn.rs`: Refresh a minute before the allocation's
10 minutes, ChannelBind well before its permission's 5). So between races, once its credentials
are 35 minutes old (`TURN_RENEW`), the client offers a new session, which asks for TURN at once
(no 2 s UDP wait: the last one needed the relay).

**Costs.** Realtime bills egress only, at $0.05/GB after 1,000 GB a month shared with TURN;
publishing is free (Cloudflare's pricing page, read 2026-11-07). An idle session sends nothing but
the transport's keepalives (ICE consent, SCTP heartbeats). The limits page documents no lifetime
for a connected DataChannel-only session ("the media inactivity timeout does not define the
lifetime of a connected session without media or a DataChannel-only connection"); only a session
that lost its connection ends (30 s reuse window). So nothing keeps it alive beyond what str0m
already sends; an association idle for 13 minutes raced at once (Live runs).

**Hard rules.** No player receives another's address, SDP, TURN credentials, session id, address
hash or install id: allocations go to their own endpoint without the publisher's session id
(`clientChannels`), answers and credentials only to their owner. No peer-to-peer path.

## Question C: concurrent subscriptions on one session

`probe_parallel_subscriptions` (`port/render/net/src/probe.rs`, ignored by default, needs the
Realtime credentials): N real associations (the game's own `Sfu`) connect and publish; each round
every subscriber subscribes to every other publisher with that many `datachannels/new` calls on
its session at once, all subscribers at the same time; then every channel must open and three
events from every publisher must reach every subscriber; then the subscriptions close and the next
round makes them again on the same sessions.

| Run | Sessions | Calls at once per session | Rounds | Calls | Failed calls | Rounds with a channel not open | Rounds with an event missing |
|---|---|---|---|---|---|---|---|
| 1 | 5 | 4 | 20 | 400 | 0 | 0 | 0 |
| 2 | 8 | 7 | 30 | 1,680 | 0 | 0 | 1 (one pair, see below) |
| 3 | 8 | 7 | 40 | 2,240 | 0 | 0 | 0 |

Every call succeeded (call times p50 171-191 ms, p95 317-395 ms). In run 2, round 27, one
subscriber had not got one publisher's three events within 5 s (all other 55 pairs had them); the
probe did not yet wait longer then (it does now: late or lost). Verdict: concurrent `datachannels/new` on one connected
session is safe (no SDP exchange is involved); the room runs a subscriber's calls 4 at once
(`SUBSCRIBE_PARALLEL`), subscribers in parallel.

## Live runs

All on 2026-11-07, through a temporary test Worker running this branch's server (rate limiters on,
no D1; deleted afterwards), the method of finding 175's runs (a local copy of its driver, with
nettool racers that connect to the SFU, joining late or leaving). The PC (headless, one game) and
the M1 Mac share one home connection; nettool bots run on the PC. Each race: the host finishes at
40 s, the Mac at 42 s, the bots never finish (the race closes at the end of the 60 s finish
window). The release time is the room's own log line (from its start message to `sfu_ready`);
each client logs the same from the moment its start message arrived.

| Run | Racers, transport | Race | Racers | Calls at the grid's lock (time) | Calls after the start | Release after the start message (room) | Clients' range |
|---|---|---|---|---|---|---|---|
| R1, PC hosts | PC, Mac, 4 bots, UDP | 1 | 5 (the Mac not yet in) | 20 (0.89 s) | 0 | 66 ms | 46-140 ms over the whole run (33 releases) |
| | | 2 | 6 (the Mac's first race) | 10 (0.89 s) | 0 | 101 ms | |
| | | 3, 4, 5 | 6 | 0 | 0 | 156, 108, 72 ms | |
| | | 6, 7 | 2 (bots done) | 0 | 0 | 74, 74 ms | |
| R2b, PC hosts | PC, Mac on forced TURN (`DHD_NET_SFU_UDP=0`), 3 bots; BOT4 leaves after race 2, LATE joins during race 2 | 1 | 5 | 20 (1.01 s) | 0 | 67 ms | 53-159 ms (32) |
| | | 2 | 6 (the Mac's first race) | 10 (0.62 s) | 0 | 64 ms | |
| | | 3 | 6 (LATE in, BOT4 gone: ids reused) | 10 (0.93 s) | 0 | 166 ms | |
| | | 4, 5 | 6, 5 | 0 | 0 | 64, 66 ms | |
| | | 6, 7 | 2 | 0 | 0 | 71, 63 ms | |
| R3, Mac hosts | Mac, PC, 4 bots, UDP; the PC's room connection cut 1.5 s after its first start message, the Mac's 60 s into its first race; QUIT leaves mid-race 2 | 1 | 5 | 20 (0.91 s) | 0 | 65 ms | 57-87 ms (25) |
| | | 2 | 6 (the PC's first race) | 10 (0.47 s) | 0 | 69 ms | |
| | | 3, 4 | 5 | 0 | 0 | 64, 63 ms | |
| | | 5, 6 | 2 | 0 | 0 | 64, 65 ms | |

- **Before** (production, 2026-11-07): 2 racers 3-7 s, 4 racers 17.6 and 20.4 s, 7-8 racers over
  25 s. **Now** every release of these runs came 46-166 ms after the start message, the first race
  included (its members had connected on joining and were subscribed during the countdown), and
  every later race made no subscription call at all unless its racers changed.
- Forced TURN (R2b): the Mac's association connected over TURN/TLS 3.0 s after its answer, once,
  on joining; its races released like the others'. No racer far from the room was available (all
  machines share one connection); the room's work for a far racer now happens during the
  countdown, before the start message.
- Joining between races (R2b's LATE, R1's and R3's late Mac and PC): its own session on joining,
  its pairs at its first grid (10 calls in 0.6-0.9 s, inside the 5 s countdown).
- Leaving: BOT4 between races (R2b) and QUIT mid-race (R3): their sessions and the others'
  subscriptions to them closed, the next race released in 64-166 ms, LATE's subscriptions reusing
  BOT4's channel ids.
- Rejoin (R3): both cut connections rejoined into their race ("back in the race"), kept their
  sessions (`"sfu":"ready"`, no new session in the logs) and their next races released in 61-87 ms.
- Idle: a nettool room whose host's association had been idle 793 s (13 minutes) raced at once:
  released 53 ms after the start message, its frames flowing. No keepalive beyond str0m's own is
  needed for that.
- R2 (before 5f749f2): BOT4 left, and every client's next subscription on its reused ids waited
  10 s and failed its association; the replacements cascaded (each new session closes and remakes
  the others' subscriptions to it), releases took 2.8-17.9 s and the PC froze up to 29 s at the
  ride wait. Reproduced without the game (a nettool room where a bot leaves after race 1 and one
  joins), fixed, and the same nettool room then released 56-65 ms in every race; R2b is R2 again.

## Surprises

- **Cloudflare reuses a closed subscription's channel id at once** (probe: 760 of 760 ids came
  back in the next round). Per race this never mattered (a new association each race). With one
  association per room stay the client must reuse a stream id after its reset, and it failed: from
  the second round on, channels on reused ids never opened (str0m held the ids), and creating a
  channel on a stream str0m still held panicked inside str0m ("sctp_stream_id exists already").
  Instrumenting the vendored sctp-proto showed why: on a close Cloudflare resets its streams with
  several RE-CONFIG requests in flight at once, against RFC 6525 5.1.1, and they arrive out of
  order; sctp-proto answered the early ones "Bad Sequence Number" (or "Request already in
  progress"), Cloudflare never retried them, so those streams never finished their reset. Fixed in
  the vendored crate (26dc9e0, `PATCH.md`): early requests are held and processed in order. The
  client also leaves the reset to Cloudflare's (closing from both sides at once collided) and
  resets a stream itself only if Cloudflare's hasn't come in 2 s; a channel waits until str0m let
  go of its id; one that doesn't open in 10 s fails the association, which is replaced.
- Cloudflare's datachannels/close of a subscription resets the stream on the subscriber's
  association (most within milliseconds); without the client's own close the old allocation goes
  as soon as that reset arrives. That reset usually beats the room's `sfu_closed` over the
  WebSocket: the client first took such a channel for one that never opened (its id then never
  used again), which broke R2 (5f749f2 fixed it; the probe had closed before polling, so it never
  saw that order).
- The ride wait of a game's first race on the Mac still took about 26 s ("countdown starts (waited
  26.3 s)") while its release came in 50-60 ms: the Mac's first track load, not the SFU (later
  races: 1.5 s).

## Decisions

- Connect on joining the room, not at the first grid lock: the first race gets the same head start,
  and an idle session costs about nothing.
- Subscriptions are made only for a race's racers (not every member pair): a member sitting a race
  out would otherwise be sent every racer's frames.
- One code path: `sfu_restart` removed rather than kept beside "an offer replaces".
- TURN renewal by a new session between races rather than a second allocation on the same
  association (a relayed candidate swap mid-association is untested in str0m).
- A racer still connecting at SFU_CONNECT_MS leaves the race but keeps its session for the next;
  a racer whose channels never opened by SFU_SETUP_MS has its session reset.

## Result

Commits on `feat/sfu-room-sessions` after the reservation: 26dc9e0 (sctp-proto), 55dbca2 (server),
1f86802, d8706ad, 5f749f2 (client), and this finding.

- **A:** one Realtime session per member per room stay: made on joining, kept across races with its
  publications and subscriptions, closed on leaving, failure or replacement; per-race state on the
  client starts over at each start. Two interop problems with keeping an association found and
  fixed on the way: Cloudflare's out-of-order stream resets (vendored sctp-proto) and the reset that
  beats the room's message (client).
- **B:** a race's missing pairs are subscribed at the grid's lock; with members connected on joining,
  every release in the live runs came 46-166 ms after the start message, first race included (was
  3-20 s and more).
- **C:** concurrent `datachannels/new` on one connected session is safe (4,320 calls, none failed);
  the room runs 4 at once per subscriber.

Open:
- Probe run 2 had one pair (of 1,680) whose events had not arrived 5 s after its channels opened;
  the probe did not wait longer then and runs 1 and 3 had none. In a race the acknowledgement and
  release come between, and the live runs never showed it; if it recurs, Cloudflare's
  `waitForAck` on subscriptions is the documented gate.
- A member sitting a race out loses its subscriptions at the start and gets them back at its next
  grid (calls at that lock); kept, it would be sent every racer's frames.
- No far-away racer in the live runs (forced TURN only).
- Old clients: protocol 4 is unreleased, so no compatibility path was kept.
