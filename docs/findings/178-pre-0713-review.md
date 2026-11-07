# 178: pre-release online lifecycle review

Status: current

Branch `review/pre-0713` from `main` 84558c4, started 2026-11-07. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

Review the changes since v0.7.12, focusing on protocol 4 and SFU room sessions.
Fix demonstrated defects with regression tests. Run `python tools/check.py port -j 6`
and `python tools/check.py server`. Server-only fixes do not alter the game core.
No deployments, remote machine runs, pushes or release actions.

## Progress

- [x] 2026-11-07: reserve the review number; base 84558c4, release diff inventoried.
- [x] Review session replacement, subscription passes, cleanup, race resets and protocol boundaries.
- [x] Demonstrate and fix three defects with five regression cases (commits below).
- [x] Full server tier: 162 tests and typecheck pass.
- [x] Core build, runtime tests, native host build, C network tests and chunk tests pass.
- [x] Boot 600 and race 5000 fields: every state-trace row matches the shared base build.
- [x] Rust SFU tests: 12 pass; 6 live probes ignored (no external credentials used).
- [ ] Full port renderer gate: blocked by the explicit WARP-off constraint.

## Decisions

Keep state sync, relay-only transport and the existing game clock. Leave design changes
for the lead. Review our SCTP patches, excluding unchanged upstream implementation.

## Surprises

P1, `online/server/src/room.ts`: failures completing after a session replacement
were attributed to the member's new session. A failed old offer/publication could fail
its healthy replacement; an old subscription failure could retire the replacement racer.
Three deterministic held-API tests failed on the base and pass with generation checks.
The subscription pass also retains the failed session identity across its storage await.

Research: Cloudflare's [in-memory state reference](https://developers.cloudflare.com/durable-objects/reference/in-memory-state/)
and [Workers best practices](https://developers.cloudflare.com/workers/best-practices/workers-best-practices/).
No external implementation copied.

P2, `online/server/src/room.ts`: repeated `sfu_connected` during the publication API
await allocated duplicate channel pairs. Admission now marks the session connected before
awaiting and ignores repeats. A held-publication test observed two calls without the fix,
one with it, and exactly two retained channels.

P2, `online/server/src/room.ts`: a mint completing after a newer session received TURN
credentials overwrote its cleanup record before detecting the stale attempt. The new
credential was then untracked and could not be revoked on leave. Validate ownership
before assigning; revoke a stale result through a detached record. The held-mint test
fails without the fix and verifies both the retained replacement and the old revocation.

## Review coverage and lead decisions

Inspected the release log and diff, server await boundaries, rejoin/release persistence,
SFU allocation projection and cleanup, Rust association lifetime and race reset, bundled
STATE validation and prediction, the shared clock and counter, device-loss handling,
shader workaround and test changes, worktree setup, and our SCTP patch diff against its
unchanged vendor-import commit.

Found sound in the inspected paths: allocation projection strips session identifiers;
private SDP and TURN messages go to their owner; no peer signaling path was introduced;
race markers bound delayed events to their race; STATE bundles have a checked datagram
budget; rate counters reset by time and race; the release is persisted and sent once per
racer; the reset-before-close handling preserves the stream reset lifecycle. State sync
and the delta-time clock remain intact.

Unchanged risks for the lead (design/lifecycle changes need separate integration coverage):

- **P2, rejoin during channel churn**, `port/render/net/src/sfu.rs`, full `sfu_channels`:
  the reconciliation considers an allocation known by stream ID alone. If the SFU recycles
  an ID while the room socket is down, a full list with a different member/direction/name
  on that ID can keep the old channel identity. Decide whether mismatched allocations
  should replace individual channels or restart the association; test a dropped room
  connection overlapping a departure and newcomer. Not reproduced against the live SFU.
- **P2, failed subscription closes**, `subscribeGroup` / `clearSfu`: failed remote closes
  are forgotten rather than persisted for retry. In particular pruning a sitting-out
  member can leave live subscriptions receiving traffic until its session ends. Decide
  whether to track these independently or reset the subscriber on a failed prune. The
  current cleanup queue only guarantees retries for whole sessions and TURN credentials.
- **P2, transport thread DNS**, `Sfu::start`: synchronous `to_socket_addrs` runs on the
  sole callback/poll thread during replacement. Slow DNS can pause room processing and
  rejoin handling. Decide whether to resolve asynchronously with a bounded wait.

## Result

Fixes: `d9efff3` (stale failures), `a540278` (duplicate publish), `21fe897` (stale TURN
mint). Five regression cases fail on the corresponding unfixed code and pass with the
fixes; the full server tier passes 162 tests and typecheck.

The core module's SHA-256 equals the shared main build's; port source is unchanged
by this branch. The shared executable supplies baseline boot (600 fields) and race
(5000 fields) traces. Every boot and race row matches (`state_diff.py`: PASS for both).
Rust SFU tests pass: 12 passed, 6 ignored live probes, 9 unrelated tests filtered out.
No build or game process started by this review remains running.

The stock port tier invokes WARP explicitly in
`test_gs_wgpu.py`, even with `--skip-hardware`. The user's WARP-off constraint takes
precedence: the checker was stopped during the host build, before the renderer step;
the host build was allowed to finish and all other steps were completed separately.
The full port tier therefore is **not passed**; do not modify or weaken the renderer gate.
