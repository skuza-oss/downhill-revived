# 202: online riders that look right: a bench of today's puppets, then a rebuild

Status: in progress

Branch `feat/online-riders-rebuild` from `main` 6e2f7d23, started 2026-11-11. This file is the
lane's plan while it runs and its finding when it merges. Someone who has only this file and the
git log must be able to pick the lane up.

## Goal and acceptance

The owner's complaint: in online races other riders teleport back and forward and half of them
sink a little into the ground. Today a remote rider (a puppet, findings 044, 169) skips its input
and physics and is drawn at its newest snapshot carried forward in a straight line (at most
250 ms), with corrections fading in 80 ms and anything over 8 units shown at once.

1. **A bench first.** Real rides (AI riders and scripted riding on at least three tracks: one with
   big jumps, one twisty) recorded at full rate as the truth, replayed onto puppets through an
   injected link (delay, jitter, loss, bursts, reordering, 60/30 snapshots a second, the receiver
   at 30/60/144 fps, clock cases), deterministic by a seed. Numbers per scenario: distance from
   the truth, below ground (how far and how often), jumps and twitches, time frozen, how far
   behind. The same injection as a test switch on the live receive path for real two-game races.
   Gate: if the bench does not show sinking and teleporting at realistic settings, stop and report.
2. **The rebuild**, chosen on the bench against today's predictor and a pure interpolation
   buffer: clearly better at 80 and 150 ms with steering and jumps (no visible sinking, no
   teleports outside respawns, no back-and-forth), graceful on a bad link; the old predictor
   deleted; the clock cases fixed.

Done: bench + how-to in `docs/TESTING.md`; new model in, old deleted; unit tests;
`python tools/check.py port` (and `server` if the protocol changed) and `docs` green; a
before/after table here; a two-game online race through a local server under the injected link
with equal results and hits landing. Out of scope: the room server, the SFU/TURN transport and
the P2P star (unless a bug there is part of the problem).

## Progress

- [x] 2026-11-11: lane plan (this file).

## Decisions

## Surprises

## Result
