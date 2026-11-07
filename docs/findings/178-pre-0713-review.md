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

Pending.

## Result

Pending review completion.
