# 282: touch: AUDIO / OTHER's rows (issue #125) and the custom playlist's songs (issue #118)

Status: current

Branch `fix/touch-menu-taps` from `main` aded9f56 (0.8.12 and finding 281), started 2026-11-24.
This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

- Issue #125: with touch (no controller), a tap on a row of OPTIONS > AUDIO / OTHER left the page.
  A tap on a switch row (Vibration, Stereo Sound, Stretchy Mode, Sparky Tricks) must choose the row
  and change it, a tap on a volume row (Music, Speech, SFX) must choose it only (its value goes by
  the taps beside it, finding 268), and the page stays open. Keyboard and pad unchanged.
- Issue #118 (its last comment, with a video): in pause > MORE > MUSIC PLAYLIST > CUSTOM > MODIFY
  > ADD, a tap on the song shown added another song. The tapped (shown) song must be the one added,
  in the pause menu's and the front end's MUSIC PLAYLIST alike.

Done when: a hidden check taps each AUDIO / OTHER row (switches change, volumes are chosen, the
page stays) and adds songs by taps after moving through the song list (the song shown is the song
added); the existing touch checks pass; `check.py quick` and `check.py docs`; boot and race state
traces equal to the base build's. Out of scope: gameplay, the pad and the keyboard.

## Progress

- [x] 2026-11-24: both causes, on the code and in hidden runs (What was true); the fix (e0573986)
- [x] 2026-11-24: `touch_menu_taps_check.py`, the existing touch checks, state traces, the checks
  (Result)
- [x] 2026-11-24: review round, two fixes (Review: 172c5928, a895d83d)

## What was true

**#125.** The menus' mouse (`menu_page.c` `popup_mouse`, findings 092 and 268) takes a tap on a
game popup's row as hover then Cross: Cross chooses the row on every list page. AUDIO / OTHER
(vtable `D_00284FB8`) is no list: its rows are values, changed by Left / Right (its own slots 9 and
10, `func_00261B00` / `func_00261B30` into `func_0015ED18`, for the switches; the update
`func_0015E750` for the volumes, read as held), and its Cross is the page's: `func_0015F410` keeps
the values and closes it (Triangle, `func_0015F038`, puts back the values the page opened with and
closes it). So every tap closed the page; a hidden run of the base build shows each row's tap as
`menu mouse: click at 320,98: row 1`, then `popup closed`. A desktop mouse did the same (the same
path); keyboard and pad never go through it. Of the game's popups only two have their own Left /
Right slots: this page and `D_00287D98`, a picker (below).

**#118.** The video (2340x1080, 12.8 s; frames read here) shows ADD TRACK with one song between
arrows, "◂ LOUDER "IN TRACTION" ▸", and the editor then getting Krush "Survive", then Blood "Feel
the Rage": the first and second songs of the list. ADD TRACK (`D_00287988`, built by
`func_0019B780`) is of the class `func_0018F210` builds: a one-row picker. Its row drawer
(+0x4C8 → `D_00287308`, `func_0018FB68`) draws the cursor's row alone at the rows' place, Left /
Right move the cursor (+0x4A0 / +0x4A4 = 2 / 3) and Cross adds the cursor's song
(`func_0019BA40`). The menus' mouse measured it as a list (`row_at`: row i at y + i × step), so a
tap on the shown song was row 0 (its hover put the cursor there, its Cross added the list's first
song) and a tap a line lower row 1. Not an offset, a scroll or the preview: the shown song is never
the tapped row unless the cursor is on the first.

## Decisions

- **Pickers by their drawer, one rule**: a game popup whose row drawer is `D_00287308` takes a tap
  on its shown line as the line's buttons: left of its words (the ◂ and beyond) Left, right of them
  Right, on them Cross; hover moves nothing; off the line the legend's words (finding 266/274); the
  wheel is Left / Right. Every picker of the game gets it (ADD TRACK, MUSIC PLAYLIST's lists, the
  others of that class), a desktop mouse too.
- **Value pages by their slots**: a game popup (not a picker) whose vtable has its own Left / Right
  takes a tap on a row as the cursor there and, unless the row's value is a number
  (`menu_value_is_number`: its text ends in a digit), the pad's Right: the switches toggle, the
  volumes are only chosen (the reporter's ask; their value goes by the taps beside the row or the
  legend's LESS / MORE). The page's Cross (keep and close) stays the legend's SELECT, its Triangle
  (put back and close) the legend's BACK, Android Back and the touch Pause. Not a vtable list: the
  game's own data says which pages are values.
- The reporter's patch (read as text only, not applied) marked a finger's click in the host and
  special-cased the page's vtable and the volume text ids; not taken: the cause is the same for a
  mouse, and the slot rule needs no ids or host change.

## Surprises

- The issue's wrong songs were never about scrolling or the two editor columns: ADD TRACK has no
  list on screen at all, one song between arrows; the first add of the old check (finding 274's
  `touch_pause_check.py`, "a tap on the first song adds it") passed only because the cursor starts
  on row 0.
- AUDIO / OTHER's Triangle puts back the volumes, Vibration and Stereo Sound the page opened with
  (`func_0015E6E0` keeps them at +0x510..+0x520, `func_0015F038` restores them); Stretchy Mode and
  Sparky Tricks stay. So Pause, Android Back and the legend's BACK leave without the sound changes,
  as △ does on a pad, and the legend's SELECT keeps them. Unchanged (the game's own behaviour);
  `docs/PLAYING.md` says it.
- MUSIC PLAYLIST itself (SET 1-4, RANDOM, CUSTOM, NO LIST; vtable `D_00287B20`) is a list, not a
  picker: its row taps were right and stay so. The front end's OPTIONS has no MUSIC PLAYLIST row
  (REVIVED SETTINGS, CONTROLS, DIFFICULTY, AUDIO / OTHER, VIDEO CLIPS, POSTERS, PICKUP EDITOR,
  LOAD / SAVE GAME DATA, LOAD REPLAY); CONTROLS ("SELECT CONTROL OPTIONS") and DIFFICULTY are shell
  screens (13 and 19, finding 092's `shell_mouse`), not popups, so neither rule touches them. The
  CODES page is the touch overlay's own (finding 189), not a game page.
- A long song's words ("Melissa "Do You Wanna Fly?"") reach 150 view pixels right of the middle: a
  tap there is on the words (Cross), as it should be; the check taps near the view's edges.

## Review

A review of the branch found two races with the press queue (a press is held 3 fields and let go
3, `menu_page.c` `press`), both real and both reproduced on the branch's build before the fixes by
the check tool's new cases:

1. AUDIO / OTHER reads a held Right on whatever row the cursor is on (`func_0015E750`), and a tap or
   a hover moved the cursor at once: a tap on Vibration and, a field or two later, on Music Volume
   gave Music Volume 50 → 51 and Vibration unchanged (front end and pause alike). Now a press can
   carry its popup and row (`press_at`): the cursor goes to the row when the press starts, a press
   whose popup closed is dropped, and on a value page a hover waits until the queue is empty.
2. ADD TRACK: three quick taps right of the song and one on it: the queued Rights moved the picker
   before the Cross went out (shown Stick It "Here We Go" at the tap, added Melissa "Do You Wanna
   Fly?"). The picker's Cross now carries the row shown at the tap and puts the cursor back there
   when it starts: the song added is always the one shown at the tap (the picker may show the
   later songs for a few fields before it).

## Result

Commits on `fix/touch-menu-taps`: the lane plan; the fix (`menu_page.c`, `menu_pads.c`
`menu_value_is_number`, a `test_menu_pads` case); the check tool, the picker mark in the popup trace
(`... align 0/0 picker`, `picker shows row N: <text>`), `docs/PLAYING.md`; this result.

- Unit test: `test_menu_pads` (a value row with a number, with tabs and trailing spaces as the
  trace shows them, is a number; ON / OFF, empty and NULL are not).
- Hidden runs (`port/tools/touch_menu_taps_check.py`, 1600x720 at 20:9, `DHD_TOUCH_MOUSE=1`, no
  controller): `audio` PASS: the front end's AUDIO / OTHER, a tap on each row: Vibration OFF → ON,
  Stereo Sound ON → OFF, Music / Speech / SFX Volume chosen with their values unchanged, the page
  open after each; the legend's SELECT closes it and it opens again with the changed values. The
  base build: FAIL at the first row (`a tap on 'Vibration: OFF' closed AUDIO / OTHER`). `pause` PASS:
  a race, Pause, MORE, OPTIONS: the same five rows over the race, the legend's BACK; MUSIC PLAYLIST,
  CUSTOM, USER 1, MODIFY, then ADD TRACK three times: the first song shown (1 Louder "In
  Traction", the reporter's own case, which the base build answered with Krush "Survive"), after five
  taps right of the line and one left (row 4, Goddess "You'll See"), and the last (row 22, The
  Herbaliser "Time 2 Build"): each time the editor got the song shown and only it. Since the
  review also: a switch then a volume tapped about 26 ms apart (the switch changes, the volume keeps
  its value, the cursor ends on the volume) in both scenarios, and three taps right of the song then
  one on it as fast (the song shown at that tap is the one added).
- Existing checks on this build: `touch_continue_check.py`, `touch_left_right_check.py`,
  `touch_pause_check.py`, `touch_legend_check.py`: PASS.
- State traces: boot (600 fields) and race (5000) equal to the base build's (aded9f56, built here).
- `check.py quick`, `check.py docs`: OK.
- Not run: a phone (none connected). Unproven on a real phone: the taps at a phone's own aspect and
  density (the same view mapping as every menu tap, findings 266-277), AUDIO / OTHER's pause page in
  a mode where it has Stretchy Mode and Sparky Tricks (their switches go through the same slot,
  `func_0015ED18`, as Vibration and Stereo Sound), the game's other pickers (`D_00285618`,
  `D_00288238`, `D_00287D98`; the same rule, not driven here).
