# 211: Three small online and phone UI bugs: wrong race-end message, room stall, keyboard tab hint

Status: in progress

Branch `fix/online-ui-small` from `main` 18f5e70c, started 2026-11-12. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

Three bugs seen on a phone in an online P2P race:

- A. The phone hosted, one racer finished, the finish window ran out with the phone's own rider
  still on the track (DNF TIME UP) and the phone showed "THE HOST ENDED THE RACE" although nobody
  ended it. Each way a race can end must say what happened, on the host and on a guest.
- B. A 2.9 s menus stall (`perf:` window `max=2883.9ms`) going from the race back to the room.
  Find what blocks and fix the avoidable part, with before and after timestamps on Windows.
- C. OPTIONS > SETTINGS shows the keyboard hint "A / D Tabs" with touch controls on and no
  keyboard. Show what a touch player can do; make the tab names tappable if tabs cannot be changed
  by touch; check the other port pages for the same wrong hint.

Done: each bug's cause below; A shown on host and guest for each end (log lines, captures at
`DHD_SCALE=4`); B before/after timestamps; C captures before/after at 20:9 and a tab changed by a
posted touch; `check.py port`, `check.py server` (if the server changed) and `check.py docs`
green. Phone checks are left for the lead. Out of scope: `touch.c`, `touch_field.c`,
`touch_host.c`, `port/render/settings/src/table.rs` (another lane works there).

## Progress

- [ ] 2026-11-12: finding reserved (this commit)

## Decisions

## Surprises

## Result
