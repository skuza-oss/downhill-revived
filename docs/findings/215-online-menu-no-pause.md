# 215: Online, the race menu does not pause the race

Status: in progress

Branch `feat/online-menu-no-pause` from the online riders lane's head 86c3f4a1 (local main
dbddf61e plus finding 202's round 4), started 2026-11-12. This file is the lane's plan while it
runs and its finding when it merges.

## Goal and acceptance

The owner's decision (2026-11-12): in an online race with two or more humans, START opens the
room's menu over the running race and nothing stops: this game's rider coasts with no input while
the menu is open, the computer riders keep racing, nobody else sees a freeze. A player who is the
only human still racing pauses for real, as offline.

Done: a measurement of today's pause (the "before" table), the same scenarios after, every row
of the menu with two humans, the alone case, a Mosh Bowl round with the menu opened mid-round,
captures and a clip, unit tests for the pure rules, `check.py port` and `check.py docs` green.

## Progress

- [x] 2026-11-12: finding reserved

## Decisions

## Surprises

## Result
