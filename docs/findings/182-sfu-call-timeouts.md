# 182: a slow Realtime API call no longer takes a racer out of the race

Status: current
Checked: 2026-11-09

Branch `fix/sfu-call-timeouts` from `main` 0574b05, started 2026-11-08. A server hotfix: no
protocol change, no `MIN_VERSION` change; 0.7.13 games keep working unchanged.

## Goal and acceptance

Production (0.7.13 server) took racers out of races with "out of the race: no Realtime connection"
after `datachannels/new` calls ran into the 15 s timeout. A slow Cloudflare API call must not take
a racer out:

- A definite failure (an item `errorCode`, a 4xx other than 409/429) fails the session, as before.
- An unknown outcome (a timeout, a network error, a 5xx, 409 or 429) does not: the subscriber's
  session is inspected (`GET sessions/{id}`) after a backoff, what exists is adopted, what the
  aborted call left twice is closed, the missing pairs are made in the next pass.
- The room's own calls never wait in the Workers runtime's connection queue (below).
- Each pass logs its outcome counts.

Checks: unit tests with the fake Realtime (`test/sfu-fake.ts`); `python tools/check.py server`; a
temporary test Worker (deleted after) with 6+ nettool bots that really connect, a normal race and
one with forced call timeouts, nobody out of the race. Out of scope: the publication call in
`onSfu` (`sfu_connected`), the game client.

## Progress

- [x] 2026-11-08: evidence from Workers Logs (below): 60a2272
- [x] 2026-11-08: the fix and its unit tests (`test/sfu-timeouts.test.ts`): 030f457
- [x] 2026-11-08: the spin the test Worker found (Surprises), with its test: c15e41c
- [x] 2026-11-08: test Worker runs (below); the test Worker deleted

## Evidence (Workers Logs, 2026-11-07 16:00 to 2026-11-08 22:00 local)

Method: every `Realtime`/`room ` line of the 30 h (`$workers.durableObjectId` groups them by
room), 14,166 API calls. A call's start is its log time minus its logged duration; a timeout is
logged without a duration and counted as 15 s.

**Exits.** The 0.7.13 server (version 09194df0, 11-07 20:12 to 11-08 21:57) logged 96 "out of the
race: no Realtime connection"; 93 of them follow a subscription timeout in the same room: 30 in
the same alarm tick (`subscribeGroup` failed the subscriber, `sfuFailed` → `retireSfu`), 63 later
at `SFU_SETUP_MS` (the pass gave up, the pair stayed missing). 3 had no slow call. 236 timeouts in
all, every one a `datachannels/new` subscription.

**Calls slow down with the calls in flight in the same room, not elsewhere.** `datachannels/new`
time by how many other calls of the same room were in flight when it started:

| In flight (same room) | Calls | Median | p90 | Timed out |
|---|---|---|---|---|
| 0 | 2,135 | 987 ms | 1,699 ms | 0 |
| 2 | 662 | 1,340 ms | 4,067 ms | 0 |
| 4 | 461 | 1,902 ms | 4,914 ms | 0 |
| 6 | 311 | 2,932 ms | 6,444 ms | 0 |
| 8-11 | 624 | 4,942 ms | 9,740 ms | 1.3 % |
| 12-15 | 374 | 7,836 ms | 13,859 ms | 7.5 % |
| 16+ | 573 | 11,977 ms | 15,000 ms | 34 % |

With no other call of its room in flight, a call is as quick with 8 or more calls of other rooms
in flight (944 ms median, 13 calls) as without (1,058 ms): the bottleneck is per room. Closes and
`sessions/new` behave the same (a close waiting behind subscriptions took 9-13 s).

**Why per room.** A Durable Object request has at most 6 outgoing connections waiting for
response headers (Workers limits, "Simultaneous open connections"; Durable Objects limits,
"Simultaneous outgoing connections/request: 6"); a 7th `fetch` waits until one gets its headers.
A Realtime call waits for headers its whole length. `Realtime.call`'s `AbortSignal.timeout(15000)`
starts when `fetch` is called, so a call queued behind six slow ones spends its 15 s mostly in the
queue. The pass made every missing pair at once: `SUBSCRIBE_PARALLEL` (4) per subscriber, all
subscribers together, so 16 to 32 calls after a grid of fresh sessions. Cloudflare's own guidance
is also to serialize the mutations of one session (Realtime SFU "Negotiation and session
lifecycle: Serialize mutations per session"); 4 at once on a subscriber's session went against it.
Finding 177's probe (question C) ran 56 calls at once from a PC at 171-191 ms p50: no Workers
connection limit there, so it could not see this.

**Example, room T7496B, 2026-11-08 21:52-21:53, 6 racers.** All six reported ready at the
connect check (`setup after 10000 ms: 39/SA:ready 54/NA:ready 55/SA:ready 59/SA:ready 63/AF:ready
66/AF:ready`). The grid pass sent 16 calls at once at 21:52:55; they ended after 6.0, 6.2, 6.8,
7.1, 9.5, 12.7, 13.8 and 13.9 s, and 7 more timed out at 15 s ("16 calls in 15001 ms"); the two
subscribers whose calls timed out (63 and 66, both in Africa) were failed and taken out. Closes
sent meanwhile took 11.6-12.8 s, queued behind them. The logs carry no session ids, so whether
those two lost because of their region or because their calls were the queue's tail can't be
told; the in-flight count alone explains the times.

**Time of day.** Timeouts came in the busy hours with big rooms (11-08 07:00-08:00: 69; 16:00-21:00:
133); quiet hours had calls of ~1 s median and none.

**Calls with at most 3 others of the room in flight** (what a capped room sees): `datachannels/new`
p50 1,054 ms, p99 5,637 ms, p99.9 8,286 ms, max 9,362 ms (4,363 calls, none over 10 s); closes p99
1,150 ms, max 5,977 ms; `sessions/new` p99 1,042 ms, max 2,316 ms.

## Decisions

- **One gate for the room's calls, not a longer timeout.** `CallGate` (sfu.ts): at most 5 Realtime
  and TURN calls at once (`ROOM_CALLS`; the runtime allows 6 waiting for headers, one is left for
  anything else), subscription work (subscriptions, closes, inspections) at most 4
  (`BACKGROUND_CALLS`). A member's own session, publication and TURN calls are urgent: they go
  first and always find a slot, because the connect check (`SFU_CONNECT_MS`, 10 s) waits for
  them. A call's timeout starts when it leaves the gate, so it measures the call, not the queue.
- **One call per subscriber and pass.** A remote `datachannels/new` batch may name several
  publishing sessions (Realtime SFU API, "Batch resource operations"); the pass asks once per
  subscriber for every channel it lacks: 8 calls for a fresh grid of 8 instead of 56. Checked on
  the real API (test Worker): the answer's items name their `sessionId` and come in request
  order; the room matches them by publisher session and name. The client already takes several
  members' channels in one `sfu_channels` (port/render/net/src/sfu.rs, at most 20 channels).
- **One change at a time per session** (`serial` in room.ts), as Cloudflare asks ("Serialize
  mutations per session"): a departure's closes and a pass's calls on one subscriber no longer
  overlap. Finding 177's 4 calls at once per subscriber are gone.
- **What counts as unknown.** No answer (timeout, network error, an answer cut short), 5xx, 409
  and 429: the session waits (no change on it), is listed with `GET sessions/{id}` after 2 s
  (doubling to 16 s for outcomes in a row), channels the record lacks are adopted when they lead
  to a member of the group and the record has no live one for that publisher and name, otherwise
  closed unannounced; then the missing pairs are made in the same pass. Cloudflare: "An empty
  snapshot cannot resolve an uncertain request", so the session is listed once more 10 s later
  (`RECHECK_MS`) to close a second subscription the aborted call made late. Definite (an item
  `errorCode`, another 4xx, an answer that makes no sense): the session fails, as before.
- **Timeout 10 s in the pass** (`SUBSCRIBE_TIMEOUT_MS`; 15 s for a member's own calls, as
  before). Ungated calls with at most 3 others of their room in flight ended within 9.4 s (p99.9
  8.3 s, 4,363 calls), so a longer limit buys almost nothing, and a run-out call now costs only
  a retry: at a race start a newcomer's 10 s, 2 s of backoff, a GET (~0.2 s) and the retry
  (~1 s) end near 14 s, inside `SFU_SETUP_MS` (25 s), with room for a second unknown.
- **Deploy storms need nothing more.** After a deploy every member offers a new session: those
  calls are urgent; the pass then makes one call per subscriber, 4 at once, only for the race's
  racers or the locked grid (`sfuGroup`). Spreading the rebuild over time would only delay it.
- **The publication call** (`sfu_connected`) still fails the session on any error: one call per
  session, made with nothing queued in front of it now, and its failure only means a new session.
- **Test switch** `SFU_TEST_LOST_ANSWERS=N` (a var on a test Worker, never in `wrangler.jsonc`):
  the room's first N subscription calls are carried out by Cloudflare, their answer dropped, and
  the call waits for its timeout: the worst case, an allocation the room doesn't know of.

## Surprises

- **A pass that made no call ran again at once, without end** (test Worker, first forced run).
  Every subscriber still missing a pair waited for its inspection; `releaseSfu` asked for a pass
  for the missing pair, the pass did nothing and asked again, with no I/O between: "Durable
  Object exceeded its CPU time limit and was reset", then "overloaded" for 30 s, and a racer was
  out 52 s after the start. Now a pass repeats only after one that made a call, the release asks
  only for pairs a pass can make now, and the alarm takes the rest (c15e41c; the test fails on
  the old code after 64 s).
- **4 lost calls hold the 4 background slots for their 10 s.** In the forced run the other
  subscribers' calls waited 10 s in the gate ("waited 10000 ms"). Only a forced worst case: a
  real call that long is rare once nothing queues (above).
- **This PC's path to Workers went slow for half an hour** (22:30-23:02): TLS to a workers.dev
  host 1-1.5 s and requests 2-7 s (production as well), cloudflare.com 0.15 s, ICMP 8 ms, WARP
  off. Bot runs in that window had room pings of 1-9 s and room connections dropping; one forced
  run lost a racer that way (its own WebSocket dropped twice; the room's CPU per event stayed at
  p99 2 ms). The run was repeated once the path was quick again (below).
- A `datachannels/new` on a session whose PeerConnection is not connected answers 425
  (`session_error`, "Session is not ready yet"), here after 11.5 s: the concurrency can't be
  probed with bare sessions, only with real connected endpoints.

## Live runs (test Worker)

A temporary Worker running this branch (workers.dev, no D1, rate limits or cron; the test
Realtime app's secrets; deleted after), as in findings 175 and 181: 8
nettool bots on this PC (the finding 181 build), each a real SFU connection (UDP), one host and 7
joining, `--races 1 --finish-after 45`. Room in LHR or MAD, edge MRS/CDG. Logs: the Worker's
Workers Logs.

| Run | Subscription pass | Released after the start | Out of the race |
|---|---|---|---|
| Normal, 8 racers | grid: 8 calls in 2,563 ms, pairs ok 56 (4 calls of 7 publishers at 1.8 s, then 4 at 0.55-0.69 s after waiting 1.8 s) | 704 ms | none |
| `SFU_TEST_LOST_ANSWERS=5`, 8 racers | grid: 12 calls in 20,356 ms, pairs ok 21, unknown 35, inspected 4, adopted 56; then inspected 1, adopted 14; two rechecks closed nothing | 18,161 ms | none |

The forced run's five dropped answers had all made their channels on Cloudflare (70); every one
was adopted, none made twice. Its release came at 18 s because the fifth dropped call started
behind the first four (they held the four background slots for their 10 s) and the race started
during it. Every bot raced and finished; each that wasn't hosting received 732 frames (764 in
the normal run, whose traffic was released 17 s sooner).

Unit tests (`test/sfu-timeouts.test.ts`): a held call that times out doesn't fail its
subscriber, and the next pass lists the session and makes the pair; an aborted call whose
channels exist is adopted, not made again; a second pair an aborted call made after the
inspection is closed at the recheck, unannounced; a 429 is unknown, an item error fails the
subscriber; a pass whose missing pairs all wait ends without a call; the gate's limits and order;
a fresh 8-member grid makes 8 calls, 4 at once, while a newcomer's offer is answered at once.
`python tools/check.py server`: OK (183 tests, typecheck).

## Result

Filled in at the merge. For the lead: a server-only hotfix, no protocol or `MIN_VERSION`
change (0.7.13 games already take several members' channels in one `sfu_channels`). After the
deploy, the pass lines in Workers Logs read `pairs ok N unknown N failed N; inspected N adopted
N closed N`; "Realtime subscription outcome unknown" replaces the old "Realtime subscription
failed" for timeouts, and "out of the race: no Realtime connection" should follow only a real
connection failure or `SFU_SETUP_MS`.
