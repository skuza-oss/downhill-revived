# 282: touch: AUDIO / OTHER's rows (issue #125) and the custom playlist's songs (issue #118)

Status: in progress

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

- [x] 2026-11-24: both causes, on the code and in hidden runs (What was true); the fix
- [ ] the check tool, the existing checks, state traces, the checks

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

## Result
