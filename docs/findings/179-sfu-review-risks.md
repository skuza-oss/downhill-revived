# 179: the three SFU risks the pre-release review left open

Status: current

Supersedes: parts of 178 (its three open risks)

Branch `fix/sfu-review-risks` from `main` c4f0776, started 2026-11-07. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

Finding 178's review listed three P2 risks in the room-stay SFU sessions of finding 177; the owner
chose to fix all three before 0.7.13:

1. **Rejoin reconciliation by stream id only** (`port/render/net/src/sfu.rs`, a rejoin's full
   `sfu_channels` list): a stream id Cloudflare recycled while the room connection was down keeps
   the old channel's identity, so a rider can go missing.
2. **Failed subscription closes are forgotten** (`online/server/src/room.ts`, `subscribeGroup`'s
   prune and `clearSfu`): a member sitting a race out can keep receiving (and costing egress for)
   race frames until its session ends.
3. **Synchronous DNS on the SFU transport thread** (`Sfu::start`, `to_socket_addrs`): a slow lookup
   during a session replacement blocks the only callback/poll thread.

Done:
- each fix with a test that fails without it;
- `python tools/check.py port -j 6` and `python tools/check.py server` pass; boot and race state
  traces equal c4f0776's;
- a short live check on a temporary test Worker (deleted after): the PC and 4+ nettool bots, 3+
  races with a bot leaving, a bot joining between races and one room-connection cut with a rejoin;
  releases still within about 0.2 s of the start message.

Out of scope: any change to state sync, the relay-only transport or the game clock.

## Progress

- [x] 2026-11-07: finding reserved (e22f76b).
- [x] Risk 1: whole-allocation reconciliation (7b0078c). Rust tests
      `a_rejoin_takes_a_recycled_stream_id_for_its_new_member`,
      `a_rejoin_takes_a_recycled_stream_id_reset_meanwhile` and a new case in
      `a_rejoins_full_channel_list_reconciles`.
- [x] Risk 3: the SFU address looked up off the send thread (80f0781). Rust test
      `a_hanging_address_lookup_never_holds_the_poll_thread`.
- [x] Risk 2: failed subscription closes kept and retried, then the subscriber reset (54b16e4).
      Server tests in `online/server/test/room-sessions.test.ts`, "subscription closes that fail";
      `docs/online-protocol.md` updated for both protocol-visible changes.
- [x] `check.py port -j 6` and `check.py server` (166 tests) pass; boot and race traces equal
      c4f0776's; live run L1 on a temporary test Worker (deleted).

## Decisions

- **Risk 1: replace the channel, don't restart the association.** A rejoin's whole list keeps a
  channel only if its whole allocation is listed (id, member, name, direction) and str0m still
  holds its stream (a channel that opened and whose stream the SFU reset is gone, even when the
  same allocation is listed again: the member sat a race out and got the same ids back). Anything
  else goes through `close_stream`, the path the room's `sfu_closed` takes since finding 177 (wait
  for Cloudflare's reset, reset from here after 2 s), and the listed allocation waits in
  `open_waiting` for the id. That path already handles a reused id in both orders (reset before or
  after the room's message, finding 177's tests and live run R2b); a restart would cost the member a
  new session (offer, publications, every other member's subscriptions to it) for one channel.
- **Risk 2: retry on the subscriber's own record, then reset its session.** A subscription whose
  close fails stays in its subscriber's `channels` marked `closeAt`/`closeTries` (so it is stored
  with the room and survives an eviction), is no longer counted as a subscription (`subscribed`,
  the API's duplicate check, the stale list, a rejoin's full list), and the subscription pass tries
  it again 5, 10, 20 and 40 s later; `sfuWork` makes the alarm (every 2 s) start that pass when one
  is due. Not the pending cleanup queue: its retries run in the SFU queue, outside the
  subscription pass, so a retry could race a new subscription on the same session; in the pass,
  closes and allocations on a session never overlap. After the fourth retry a subscription that
  still forwards a live publication resets its subscriber's session (`sfu_reset`) once the
  subscriber is outside the running race and the locked grid: the game drops its association and
  Cloudflare ends the session with the connection, whatever the API answers, so the egress stops
  even with the close endpoint down. A racer is not reset for this (it receives those publishers
  anyway; the stale one is a duplicate until the race ends). One to no live publication (its
  publisher left or replaced its session: nothing flows) is forgotten.
- Cloudflare gives an id only after its last channel closed, so a new allocation on an id still
  marked closing proves that close went through: `Realtime.allocate` drops the closing record
  there, and a later retry can never close the new subscription.
- **Risk 3: a lookup thread, the last good address, a 300 ms wait.** `Sfu::start` asks a
  process-wide `Route`: one lookup thread at a time; the last address found serves at once (each
  new association starts a fresh lookup that refreshes it); with none yet (the first association
  of the run) it waits at most 300 ms (`LOOKUP_WAIT`, well above a normal lookup), and a lookup
  still running or failed fails that attempt, retried after the send thread's pause (1 s, doubling).
  A replacement, the case the review named, always has an address: it never waits.

## Surprises

- `evictDurableObject` never returns in the server's test pool once the object has called the
  test's mocked fetch (a minimal test: a room, one `sfu_offer`, evict: hangs; without the offer it
  returns). The eviction test therefore reloads the stored room the way the constructor does after
  an eviction (`storage.get("room")`, `rebuildRace`).
- A fresh worktree's `check.py port` fails at `test_runtime` ("no ps2_asm() calls in
  build/wasm/src") when the core comes from the module cache: the cache hit leaves no transformed
  sources. One `DHD_MODULE_CACHE=0` core build fixes it for that worktree.

## Tests that fail without the fixes

| Risk | Test | Without the fix |
|---|---|---|
| 1 | `a_rejoin_takes_a_recycled_stream_id_for_its_new_member` (a newcomer's subscriptions on the departed member's ids, room connection down meanwhile) | no frame from the newcomer; the game says `sfu_ready` only at READY_TIMEOUT ("ready without every racer's channels 8 s after the start") |
| 1 | `a_rejoin_takes_a_recycled_stream_id_reset_meanwhile` (the same allocation again on reset streams) | the same |
| 1 | `a_rejoins_full_channel_list_reconciles` (new case: listed ids with another member and another name) | the old channels kept |
| 3 | `a_hanging_address_lookup_never_holds_the_poll_thread` (a resolver that hangs 1.6 s, then hangs again on the refresh) | "the poll thread was held 1603 ms" (the lookup run inline, as `to_socket_addrs` was) |
| 2 | "a prune whose close fails is kept, survives an eviction, and is closed by a later pass" | the failed subscriptions are gone from the record at once (`expected [] to deeply equal [117, 118, 119, 120]`) |
| 2 | "a close that keeps failing is tried a bounded number of times, then the subscriber's session is reset" | no retry at all |
| 2 | "a gone session (410, 404) counts as closed: one more call, no reset" | no retry at all |
| 2 | "a closing id the SFU gives out again belongs to the new subscription only" | the closing record on the reused id stays (a later retry would close the new subscription) |

The server tests were run against the stashed `src/` of the branch's base; the Rust tests against
the code before each commit (risk 3: `Route::address` doing the lookup inline). Rust: 15 SFU tests
pass, 6 live probes ignored (`rustgs.py test -- -p dhd_net -p dhd_render --lib sfu::`).

## Live run

2026-11-07, run L1, through a temporary test Worker running this branch's server (rate limiters
on, no D1, deleted afterwards), the method of finding 177's runs without the Mac: the PC (hidden
desktop, one game) hosts; BOT1-BOT4, nettool racers that connect to the SFU, join at once; BOT4
leaves after race 2; LATE joins during race 2 (its first race is race 3); the PC's room connection
is cut 20 s into race 1 (`DHD_NET_TEST_DROP=20`). The PC finishes at 40 s, the bots never finish.

| Race | Racers | Subscription calls at the grid's lock | Release after the start message (room) | Clients (PC, bots) |
|---|---|---|---|---|
| 1 | 5 | 20 (1.70 s) | 62 ms | 61-62 ms |
| 2 (the PC rejoined in race 1) | 5 | 0 | 67 ms | 60-63 ms |
| 3 (BOT4 gone, LATE in) | 5 | 8 (1.40 s) | 74 ms | 65-68 ms |
| 4 | 5 | 0 | 69 ms | 62-70 ms |

- The cut: the room kept the PC's seat ("lost its connection in the race; kept 20 s"), the PC
  rejoined into its race ("back in the race"), kept its session and got its whole list of 10
  channels, all kept (no recycled id: the overlap of risk 1 is tested in Rust only, as a drop
  can't be timed against a departure from the harness).
- BOT4's departure closed the others' subscriptions to it; LATE's subscriptions on the PC reused
  its ids 13 and 15 at once.
- No close failed (Realtime answered every close 200), so risk 2's retries didn't run live; the
  room logged no "close failed". No address lookup failed or waited on the PC.
- Every release came 60-74 ms after the start message, as in finding 177 (46-166 ms).

## Result

Commits on `fix/sfu-review-risks`: 7b0078c (risk 1), 80f0781 (risk 3), 54b16e4 (risk 2), and this
finding.

- **Risk 1:** a rejoin's whole channel list keeps a channel only if its whole allocation is listed
  and its stream wasn't reset; anything else closes through the `sfu_closed` path and the listed
  allocation opens on its id. A rider no longer goes missing when Cloudflare recycles an id while
  the room connection is down.
- **Risk 2:** a subscription whose close fails stays on its subscriber's record (stored with the
  room), is tried again 5, 10, 20 and 40 s later (410/404: closed), and then, if it still forwards a
  live publication to a member outside the race, that member's session is reset, which ends the
  egress through the lost connection whatever the API does.
- **Risk 3:** the SFU's address comes from a lookup thread; a replacement uses the last address at
  once, a first association waits at most 300 ms; the send thread never waits on DNS for longer.
- Checks: `check.py port -j 6` and `check.py server` pass (166 tests); boot (600) and race (5000)
  traces equal c4f0776's (`state_diff.py`: PASS, PASS); live run L1 above.

Open: the risk-1 overlap and risk 2's retries were not seen live (they need a recycled id during a
drop and a failing Realtime API); their tests are the evidence.
