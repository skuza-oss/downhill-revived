# 266: touch players stuck on "PRESS X TO CONTINUE" messages (issue #105)

Status: in progress

Branch `fix/touch-continue-266` from `main` 3746e414, started 2026-11-16. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

Issue #105: on a phone, CAREER > SPECIALIZED CAREER > MOUNTAIN CROSS shows the message
"MOUNTAIN CROSS ENTRY BIKES ACQUIRED / PRESS X TO CONTINUE" over the Bike / Jersey screen, and
nothing on screen takes a tap: the player is stuck.

Done when: every message of the game that waits for one button (X to continue, unlocks and prizes,
yes/no questions, the title, the demo) can be answered by touch through one mechanism in the shared
menu / touch code; pad and keyboard play and game state are unchanged (boot and race state traces
equal to the base); a hidden touch run passes the Mountain Cross message with one tap, and two other
messages too; one tap never answers two boxes in a row. Out of scope: gameplay, the race HUD's
touch controls.

## Progress

- [ ] 2026-11-16: cause found
- [ ] fix, unit test, hidden runs, checks

## Decisions

## Surprises

## Result
