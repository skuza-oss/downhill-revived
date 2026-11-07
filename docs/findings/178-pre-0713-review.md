# 178: pre-release online lifecycle review

Status: in progress

Branch `review/pre-0713` from `main` 84558c4, started 2026-11-07. This file is the lane's plan
while it runs and its finding when it merges. Copy it to
`docs/findings/NNN-short-name.md` on the lane's first commit: that reserves the number. Someone
who has only this file and the git log must be able to pick the lane up.

## Goal and acceptance

Review the changes since v0.7.12, focusing on protocol 4 and SFU room sessions.
Fix demonstrated defects with regression tests. Run `python tools/check.py port -j 6`
and `python tools/check.py server`. Server-only fixes do not alter the game core.
No deployments, remote machine runs, pushes or release actions.

## Progress

- [x] 2026-11-07: reserve the review number; base 84558c4, release diff inventoried.
- [ ] Review session replacement, subscription passes, cleanup, race resets and protocol boundaries.
- [ ] Demonstrate and fix defects; run required checks.

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

## Result

Pending review completion.
