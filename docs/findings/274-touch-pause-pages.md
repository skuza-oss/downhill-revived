# 274: the game's own pause pages (MUSIC PLAYLIST, OPTIONS, TRICK LIST) on touch (issue #118)

Status: in progress

Branch `feat/touch-pause-pages` from `main` bbe4acc5 (0.8.10), started 2026-11-21. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Issue #118: a phone player asked for MUSIC PLAYLIST in the pause menu "as in the original game".
Every page of the game's own pause menu that a pad player reaches must be reachable by touch, in
every mode where the game offers it (single race, career, freeride; split-screen out of scope;
online keeps the panel it has). The pages are the game's own, drawn by the game. RESUME stays one
tap away, the race controls never fire under the menu, Android Back and the touch Pause button go
back one page and resume from the top.

Done when: `test_touch` covers the new row and the back paths; a hidden run with taps only
(`port/tools/touch_pause_check.py`) pauses a race, turns a song off in MUSIC PLAYLIST, changes a
volume in OPTIONS, opens and closes TRICK LIST, resumes; boot and race state traces equal to the
base's; `check.py quick` and `check.py docs` pass; `docs/PLAYING.md` says how.

## What was true

The issue is right. With touch controls a race's pause shows the overlay's own panel
(`port/hle/touch.c`, finding 141: RESUME, RESTART RACE, QUIT RACE, CONTROLS | CODES, BAIL OUT) over
the game's pause menu (`func_0015E238`: RESUME, OPTIONS, TRICK LIST, [NEW LEVEL], [RE-START],
MUSIC PLAYLIST, QUIT; the list differs per mode). Its rows are sent to the game as ↓ / ↑ steps
then ✕ (`touch_field.c` works out only where RESUME, RE-START and QUIT are), and while it shows the
overlay takes every touch, so OPTIONS, TRICK LIST, NEW LEVEL and MUSIC PLAYLIST had no way in
without a controller. Why the panel: finding 141's spec picture, phone-sized rows, and the
overlay's own things (CONTROLS, BAIL OUT, later CODES, finding 189); at the time the game's popups
did not take a touch player's taps as well as they do since findings 092, 266 and 268.

## Progress

- [x] 2026-11-21: cause checked (above), design (Decisions), the code and `test_pause_more`
- [ ] the hidden run, captures, state traces, checks

## Decisions

- **MORE, not a replacement.** The panel gets a seventh row, MORE, on BAIL OUT's line (its right
  half, as CODES shares CONTROLS'). It sends nothing: the game's menu is already open under the
  panel. The overlay goes to a new state (`PAUSE_MENU`) in which it stands aside as it does for a
  controller (`PAUSE_PAD`): the menus' mouse takes the taps, so the game's rows, values and legend
  words work as findings 092, 266 and 268 made them. Replacing the panel with the game's menu
  would have needed CONTROLS, BAIL OUT and CODES somewhere else and given phone players the game's
  small rows for RESUME; the panel's rows are not a second implementation of the game's (they send
  the game's own presses), so the one-path rule holds.
- **The Pause button stays** in `PAUSE_MENU`, drawn alone, and is △, the menu's own back: one page
  at a time, and on the top page the game closes its menu and the race goes on
  (`func_0015E630`, the vtable `D_00285040`'s cancel). Android Back (and iOS's edge swipe) is △
  there too. The host keeps the button's spot from the menus' mouse (`touch_menu_keeps`,
  `kept_mouse` in `touch_host.c`): otherwise the same tap would also click the game's menu,
  where a click beside a row's line is Left / Right (finding 268).
- **Leaving `PAUSE_MENU`**: the race clock moving again (RESUME or △ on the top page, a restart),
  a new track's flight (NEW LEVEL), the race gone (QUIT): the rules `PAUSE_PAD` already had. A
  controller used takes the menu over (`PAUSE_PAD`).
- **Not in online races**: the online pause is the room's own panel (finding 215), unchanged.

## Surprises

## Result
