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
- [x] 2026-11-21: the hidden run (`touch_pause_check.py`) found MUSIC PLAYLIST's custom lists out of
  reach (Surprises); fixed in `menu_page.c` / `menu_pads.c`; the run passes; state traces; checks

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

- **MUSIC PLAYLIST, fixed where taps could not go** (`menu_page.c`, `menu_pads.c`). The page
  picks SET 1-4, RANDOM, CUSTOM or NO LIST (rows: taps already worked). Songs are on and off only
  in a custom list: CUSTOM (USER 1-4) > MODIFY > the playlist editor, whose songs come from the
  song list (ADD). Three gaps, each fixed by the rule the other menus follow:
  - CUSTOM draws its own legend lines ("✕ USE", "△ BACK", "○ MODIFY" from view x 30, y 160 / 170 /
    180, `func_0019A850`); MODIFY is the popup manager's button 6 (+0x45C, ○) and no tap reached
    it. A tap on a line now presses its button (`music_custom_legend`, as 268's measured BONUS
    PURCHASES legend).
  - The editor's legend "○ ADD □ CHANGE R1 DELETE △ BACK ✕ SELECT" is a footer, but finding 266's
    legend taps acted on messages only. They now act on every game popup's footer (a tap on a
    legend word presses its button; between words nothing), and the legend reader knows R1
    (byte 0xB9, `GLYPH_R1`) and ◂ / ▸ (PREV / NEXT, LESS / MORE): before, DELETE fell into CHANGE's range. So
    the song list's □ PREVIEW (268's open "jukebox" Square) and OPTIONS' LESS / MORE take taps too.
  - In the editor ✕ saves the list and leaves (`func_0019B638`), while CHANGE and DELETE act on the
    cursor's song: a tap (hover, then ✕) on a song left the editor. There, and only there, a tap on
    another row now just moves the cursor and a tap on the cursor's row is ✕ (hover moves nothing),
    as in 268's BONUS PURCHASES.
- **Which glyph is R1**: from the retail text of that legend, the bytes before its words: 0xAB ADD,
  0xA0 CHANGE, 0xB9 DELETE, 0xA7 BACK, 0xAD SELECT (the others are the glyphs 266 knew).

## Surprises

- The issue's "turn a song off" is the custom lists' DELETE: the playlist page itself has no song
  switches (SET 1-4 are fixed sets).
- Over a race the popups are laid out in a 512-wide view (the pause menu centred at x 256), not the
  640 of the shell: the check measures taps from the trace, 846 view pixels across a 1600-wide
  window.
- On a hidden desktop SDL reports a posted click where the OS cursor is (finding 189's trap), so
  the kept Pause spot is tested at the posted move's place in the desktop-mouse path; on a phone
  the touch's own mouse event has the place.
- `RACE_NAV`'s last step (Cross in the flight) repeats every 30 fields while the screen stays: in a
  pause menu it would choose rows. The check leaves it out and waits for the flight to end.

## Result

Commits on `feat/touch-pause-pages`: MORE and the game's menu by taps (with `test_pause_more`);
MUSIC PLAYLIST's custom lists by taps, the check tool and this result.

- What a touch player reaches now, by taps only: every row of the offline pause menu (RESUME,
  OPTIONS, TRICK LIST, NEW LEVEL, RE-START, MUSIC PLAYLIST, QUIT, the mode's others), OPTIONS'
  values (beside a row, finding 268, or the legend's LESS / MORE), MUSIC PLAYLIST's sets, CUSTOM's
  USE / MODIFY, the editor's ADD / CHANGE / DELETE / SELECT and the song list's PREVIEW. Pause and
  Back are △ on every page. Out of scope and unchanged: split-screen, online (finding 215's panel).
- Unit tests: `test_touch` `test_pause_more` (both presets: MORE sends nothing, the overlay keeps
  only Pause and its spot, the race controls' places and MORE's place send nothing, Pause and Back
  are one △ each, the race going on gives the race back with the resume guard, a controller takes
  over as `PAUSE_PAD`, a new track's flight ends it); `test_menu_pads`: the editor's legend, each
  word its own button (DELETE R1's), and LESS / MORE as Left / Right.
- Hidden run (`port/tools/touch_pause_check.py`, 1600x720 at 20:9, picture scale 2, Classic pad,
  no controller): PASS. Pause, MORE; MUSIC PLAYLIST, CUSTOM, USER 1, MODIFY, ADD, the first song
  (added: the editor lists it), a tap on it (the cursor only), DELETE (gone), Pause back three
  pages to the pause menu (no click reached the menu under Pause); OPTIONS, a tap right of Music
  Volume 50 → 51, Pause back; TRICK LIST, Pause back; Pause on the pause menu: the race goes on;
  again Pause, MORE, RESUME tapped in the game's menu: the race goes on. The base build has no MORE
  (the panel's five rows only).
- State traces: boot (600 fields) and race (5000) equal to the reference of main 524948ba
  (the shared reference traces `main-524948ba-ref-v3`, format 2).
- `check.py quick`, `check.py docs`.
- Not run: a phone (the lead's), iOS's edge swipe, NEW LEVEL and QUIT through MORE (their rows are
  the same taps; the overlay's exit rules are `PAUSE_PAD`'s and unit-tested), career and freeride
  pause menus (the same popup with other rows).
