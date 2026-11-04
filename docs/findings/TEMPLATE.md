# NNN: <what this lane does, in a few words>

Status: in progress

Branch `<kind>/<name>` from `main` <short sha>, started YYYY-MM-DD. This file is the lane's plan
while it runs and its finding when it merges. Copy it to
`docs/findings/NNN-short-name.md` on the lane's first commit: that reserves the number. Someone
who has only this file and the git log must be able to pick the lane up.

## Goal and acceptance

What the lane is for, and how anyone can tell it is done: the exact commands and what they must
print (a state trace equal to the base build's, a test that passes, a measured number on a named
machine and scene). Say what is out of scope.

## Progress

- [ ] YYYY-MM-DD: step, with the commit that did it
- [ ] ...

## Decisions

Each choice made where the plan left room, and why (one or two lines each).

## Surprises

What turned out different from what was expected, with the evidence (numbers, how measured).
Tried and dropped, with the number that dropped it, so nobody retries it blindly.

## Result

Filled in at the merge: what changed, what was measured and how, what is left. Then set the
Status to `current` (or `rejected`), mark what it replaces (`superseded by NNN` /
`partly superseded by NNN (<part>)` there, `Supersedes:` here), and add the `docs/STATE-LOG.md`
entry.
