# 182: a slow Realtime API call no longer takes a racer out of the race

Status: in progress

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

- [x] 2026-11-08: evidence from Workers Logs (below)
- [ ] the fix, unit tests
- [ ] test Worker runs

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

## Surprises

- A `datachannels/new` on a session whose PeerConnection is not connected answers 425
  (`session_error`, "Session is not ready yet"), here after 11.5 s: the concurrency can't be
  probed with bare sessions, only with real connected endpoints.

## Result
