# 187: a racer whose race link delivers nothing no longer races alone

Status: in progress

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
  (`probe_unannounced_subscription_close`)

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

## Surprises

- The finding 182 path that adopts an aborted call's channels also closes the ones it doesn't want,
  and those were never announced: that close is what poisons the ids. The protocol doc already said
  so ("never announced: its endpoint never heard of it") without seeing the consequence.

## Result

Filled in at the merge.
