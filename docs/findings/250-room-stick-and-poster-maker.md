# 250: the online room takes the stick; Poster Maker's picture

Status: in progress

Branch `fix/room-stick-poster-250` from `main` cae6f05c, started 2026-11-15. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

Two player reports:

- **#103:** the online Race Room and its "Leave the room?" question move the cursor only with the
  d-pad; the game's own menus take the left stick too. A generic USB pad in its digital mode (its
  d-pad comes as the stick) moves nothing there. Every screen of the online menu must take the
  stick as the game's menus do, through one shared input function, with a test that fails before
  and passes after.
- **#102:** Poster Maker (a race's pause menu) shows a black camera preview, and the saved poster
  (OPTIONS > POSTERS) is black too (0.8.2, Windows 11, Intel UHD on Dx12, native renderer, Native
  VU on, 2x, widescreen). The poster must show the scene in the preview, after saving and after a
  restart.

Done means `python tools/check.py quick`, `check.py port` and `check.py docs` green; boot and race
state traces equal to the base build's; the new tests green. Out of scope: the online race menu
over a running race (`online.c`, its own stick reading), other OPTIONS pages.

## Progress

- [x] 2026-11-15: #103 read and fixed: `port/hle/menu_pads.c`, test `port/tools/test_menu_pads.py`
- [ ] 2026-11-15: #102 reproduced on the hidden desktop
- [ ] #102 fixed

## Decisions

- **#103, one reader for our screens that read the pads themselves.** The reporter's reading is
  right: `room_input` read only the button word (`func_00220230`). The game's popup menus take a
  direction when the button is down or the left stick is past 120 from the centre
  (`func_00220640` mode 0 calls `func_002209A8`; the popup manager's reader `func_001909A8`).
  `menu_pads_held()` (`port/hle/menu_pads.c`) is that word with `func_002209A8` itself asked for
  each direction, so the threshold is the game's own, and a pad in its digital mode (mode 4) has no
  stick, as in the game. The room, the leave question, the track list, ROOM RULES and the rider
  screen's Back read it (`online_menu.c`'s `pads_held` is it now). The online menu's other pages
  (HOME, the lists, QUICK, NAME and CODE entry) are popup-manager pages: the game's reader already
  moves their cursor with the stick, unchanged.
- **#103, repeat.** The room's own repeat (a direction held 24 updates, then every 6) now applies to
  the stick as to the d-pad: the stick is the d-pad there. The game's popup menus do not repeat a
  held direction at all (their latch `D_00660D80` holds until the pad is let go), so the room
  keeps its own rule for both.

## Surprises

## Result
