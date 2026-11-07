# 179: the three SFU risks the pre-release review left open

Status: in progress

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

- [x] 2026-11-07: finding reserved.
- [ ] Risk 1: whole-allocation reconciliation, Rust test.
- [ ] Risk 2: failed subscription closes tracked and retried, server tests.
- [ ] Risk 3: the SFU address resolved off the transport thread, Rust test.
- [ ] Checks, state traces, live check.

## Decisions

## Surprises

## Result
