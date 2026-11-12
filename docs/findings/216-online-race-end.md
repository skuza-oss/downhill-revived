# 216: an online race's last seconds, the finish on every screen, a reset as a respawn

Status: in progress

Branch `fix/online-race-end` from `main` 1140b23f, started 2026-11-12. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

The lead's PC to Mac check of 18f5e70c (finding 202, "PC <-> Mac") found three things:

1. In the about 2.8 s between the room ending a race and the race screen closing, the other
   riders drift 13-72 units (p95) from their owners: both games go on sending, neither receives.
   Finding 202 round 4's window in `port/net/client.c` (`race_frames_until`) never sees a frame:
   the transport drops them first.
2. In 3 of 4 Mac-hosted races the host's race screen never showed the PC guest's finish.
3. An owner's own reset to the track (a 364.8-unit jump, standing) was placed as `far`, not as a
   respawn.

Done: the drift reproduced on this PC in a P2P room made from the menus (two hidden games, a local
server, the 80 ms test link) with the live scoring extended to the seconds after the end (a column
of `puppet_lag.py`), and gone after the fix in at least four races (host and guest views, one ended
by the finish window, one where everyone finishes); the finish shown on the other screen in each; a
reset counted as a respawn; Rust and C unit tests for the rule; `check.py port` and `check.py docs`
green. Out of scope: the SFU from this PC (failing for an unknown reason), host stops' steps.

## Progress

- [ ] 2026-11-12: lane started, finding reserved

## Decisions

## Surprises

## Result
