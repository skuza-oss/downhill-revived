# 283: touch: Classic's six shoulder buttons (#126) and HIDE in Edit Layout (#128)

Status: in progress

Branch `feat/touch-layout-buttons` from `main` aded9f56 (0.8.12 and a renderer fix), started
2026-11-24. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

- Issue #126 (also asked in #33 and #94): Classic pad's combined shoulder bar on each side (L2 | L1+L2
  | L1 and R1 | R1+R2 | R2, sections of one box) becomes six independent buttons: L1 above L2 with
  L1+L2 beside them, vertically centred; R1 above R2 with R1+R2 to their left. Each with its own
  touch area, moved, sized and saved on its own in Edit Layout; the bar's look and labels; Hand: Left
  mirrors them. One code path: the bar is replaced. What each button sends is what its section sent.
  Layouts saved with the bar load (migrated). Thumb zones unchanged.
- Issue #128: Edit Layout's bar gets HIDE before RESET and DONE. With HIDE on, a tap hides a control
  (it stays on for more); HIDE again leaves. A hidden control draws nothing and takes no touch. In
  HIDE mode hidden controls show as ghosts, and a tap on one shows it again. DONE saves visibility
  with the layout (per preset), RESET shows everything again. Pause and the steering stick cannot be
  hidden; the camera button can. The launcher's editor (the editor alone) gets the same.

Done when: unit tests in `port/hle/test_touch.c` (the six in both hands, sliding, migration, hidden
controls, the editor's HIDE, RESET); `port/tools/touch_layout_check.py` passes (a race where each of
the six sends its buttons; the editor alone hides, saves, starts again, shows one again, RESET);
the existing touch checks pass (`touch_continue_check.py`, `touch_left_right_check.py`,
`touch_pause_check.py`, `touch_legend_check.py`); `check.py quick` and `check.py docs`; boot and
race state traces equal to the base's. Out of scope: gameplay, Thumb zones' places, a real phone
(none connected: see Result).

## Progress

- [x] 2026-11-24: the six buttons, their places, the migration of saved bars, HIDE and its ghosts,
  unit tests (all pass)
- [x] 2026-11-24: the host's `touch: controls` and `touch: editor bar` lines, the desktop's cutout
  and strips for checks, `touch_layout_check.py`, PLAYING.md and TESTING.md
- [x] 2026-11-24: the checks on the built game (Result)
- [x] 2026-11-24: the review's six points (Review), each fixed with a unit test

## Decisions

- **Places** (reference phone 154.5 x 69.5 mm, `place_of`): each button 17 x 6.4 mm (11 % of the
  width, 9.2 % of the height; the bar was 30.9 x 8.7 mm, its end sections 11.4 mm wide), 3.2 mm
  apart. The right group sits left of △ (3.5 mm clear of it), R1 low enough that the top buttons fit
  above it under the Mi 11i's 5.2 mm top strip (`top_place` puts them over the highest action-side
  control); R1 and R2 above △ could not fit there (32 mm needed, 28 mm free). The left group is 6 % of
  the height higher than the right: below it the steering area and the D-pad's room start
  (`room_top`, the lowest of the three), and at the right group's height Classic's D-pad arms went
  from 7.9 to 6.9 mm; at 32 % they stay 7.9 mm. The L column starts 3 mm right of the game's
  speedometer (it reaches 0.3 of the height across; picture of the base build's race).
- **Height rule**: Classic now needs 65 mm (the R column and the top buttons above it), not 62:
  `touch_height_scale` divides by 51, not 48. Screens 62-65 mm tall get Classic slightly smaller;
  the Mi 11i (69.6 mm) and the iPhones (70.7 mm) are unchanged at 100 %.
- **Sliding**: lift to change, as the bar's sections: the button under the touch-down is the one
  held until the finger lifts; a thumb sliding from L1 onto L2 (or L1+L2) still holds L1 only. A held
  trick never changes under a rolling thumb; another press needs a new touch. ✕'s roll onto ◻ / ○
  stays as it was (finding 141).
- **Migration**: `classic.lbar` / `classic.rbar` entries (dx, dy, size) become the three buttons'
  entries: each moved by the bar's dx, dy and given the bar's size. A bar made larger also spreads the
  centres about the group's middle (the offset from the middle at the reference scale times 1.15 x
  (size - 1): 1.15 covers a tablet at Size 105, whose buttons grow 1.34 times where the places grow
  1.31), so the grown buttons stay apart; a smaller bar keeps the default spacing (pulling the centres
  in too stacked the three at a small Size setting: the review). A button's own entry wins, before or
  after the bar's. Out-of-range bars are dropped as any entry; "thumb.lbar" is nothing. The text
  written back names the buttons, never the bars.
- **Narrow screens**: the groups hang from opposite corners, so on a 16:9 phone (104 x 58.5 mm) the
  default L1+L2 and R1+R2 overlapped by about 10 x 2 mm. Where a tenth smaller would part their touch
  areas by GAP_MM, Classic is made that much smaller (an iPad mini at Size 105: 1.31 to 1.26);
  past that, L1+L2 goes to the far side of its L1 / L2 column (`combo_out`; on the SE its top stays
  below the speedometer's corner) and a width limit keeps the column's touch areas GAP_MM from R1+R2's
  (an iPhone 8 Plus shape at Size 105: 1.05 to 1.03). Shrinking alone would have cost the SE a
  further eighth (0.87 to 0.76). A moved L1+L2's move across is stored mirrored, as the left hand's
  are (`edit_sx`), so a drag follows the finger and a migrated bar's spread takes it away from the
  column.
- **Saved visibility**: `TouchEdit.hidden`, saved as `preset.control=dx,dy,size,hide`. Older builds
  read the three numbers and skip the rest (they show the control). A hidden control counts as edited
  (the `layout=2` mark). It travels with the edits: UNDO and REDO, the steering rescue, RESET (all
  zero).
- **What can hide**: every control but Pause and the steering stick (the player would be locked out
  of the pause menu or steering) and the pieces that go with another: the trick stick's spot (the Air
  tab's HIDE is dimmed) and Classic's + spots, which hide with ✕ or with their own face button.
  PEDAL and ✕ can hide: the player's choice.
- **Ghosts**: in HIDE mode hidden controls draw at 28 % of their alpha; a tap (down and up on the
  control's touch area) hides or shows; nothing is dragged or resized in HIDE mode (- and + dim).
  Outside HIDE mode the editor shows the layout as the race does: hidden controls neither show nor
  move. A hidden control is no obstacle in the editor (its room is free); a ghost shows again only
  where it fits (`control_fits`, the drop's rule); where another control has taken its room it stays a
  ghost, with a double buzz, until that one moves (the review: L1 at 130 % shown over Pause took
  Pause's taps). The dimmed - and + do nothing in HIDE mode. A hint about a hidden control waits,
  unseen (PEDAL hidden: no "HOLD PEDAL TO PEDAL"); the tap attacks' hint needs ◻ or ○ shown.
- **The editor's bar**: nine buttons now, 1.5 mm apart (2 before), GROUND 12, AIR 9, - 9, + 9, UNDO
  10, REDO 10, HIDE 10, RESET 11, DONE 10 mm, their words fitted to them. Classic on an iPhone with its
  island on the action side has 104 mm beside the top buttons; the bar takes 102.5. Every arrangement
  keeps to the usable width (the gesture strips and a cutout's band left out); the old one row across
  (138 mm) put GROUND and DONE off a 104 mm screen. Too narrow for the row beside the top buttons, the
  row narrows, down to 0.7 (a 16:9 phone: 0.72, targets 6.5 to 8.6 mm, about the 44 pt Apple asks
  for), over no control: a second row there lay over R1 all but a 1.4 mm strip (the picture of the
  first try). Narrower still (a 4-inch phone), two rows on the top buttons' wide side: GROUND .. REDO,
  then HIDE RESET DONE next to the top buttons and the readout beyond (its words fitted to it), down
  to 3/4; the second row lies over shoulders there, each partly uncovered to grab.
  `touch_editor_button` gives a bar button's box (the tests).
- **The log**: the host writes `touch: controls name x,y ...` (hidden ones `name hidden`) when it
  lays the controls out, and the editor alone `touch: editor bar ...`; the checks aim with them.
  `DHD_TOUCH_STRIPS_MM` and `DHD_TOUCH_CUTOUT_MM` give the desktop a phone's strips and cutout.

## Surprises

- The editor's bar placed itself from the top buttons' places before the cutout's push
  (`top_place`), so on an iPhone with its island on the action side it lay 2.7 mm over the camera
  button (both presets, before this lane too). It now reads the laid-out places (`Ctl.x0`, which
  carry the push).
- Between L1 and L2 (3.2 mm apart) the touch areas (20 % larger) leave a 1.9 mm strip that is
  neither's; a touch there starts the steering (Classic's steering area is below the L group's
  default places, so only after a move).

## Review (2026-11-24)

Six points, each checked in the code, fixed in its own commit with a unit test that fails without it:
a migrated bar under 100 % stacked its buttons (`test_bar_migration_sizes`); a ghost shown over Pause
took its taps (`test_editor_show_refused`); the bar's one-row fallback put DONE off a 104 mm screen
(`test_editor_bar_small`: six small shapes, with and without a cutout or a strip, the top buttons
uncovered); the default L1+L2 and R1+R2 overlapped on a 16:9 phone (`test_shoulders_narrow`: eight
shapes, Size 70-105, both hands, a drag of the moved L1+L2); - and + sized in HIDE mode
(`test_editor_hide_no_size`); a hidden PEDAL's hint showed and was marked seen
(`test_hints_skip_hidden`).

## Result

On the lane's build (Windows, hidden desktop, 2026-11-24):

- `test_touch`: all pass (the six in both hands, sliding, migration, hidden controls, the editor's
  HIDE with UNDO, REDO, DONE and RESET, the editor alone, the bar on three phone shapes).
- `touch_layout_check.py`: shoulders PASS (L1 0400, L2 0100, L1+L2 0500, R1 0800, R2 0200, R1+R2
  0a00, nothing else sent); hide PASS (`classic.layout=2;classic.r1r2=...,hide;classic.l3=...,hide`,
  then only R1+R2 after L3's ghost was shown; in the race R1+R2's place sent nothing and L3 sent
  0002; RESET left `touch_edits` empty and nothing hidden); pictures PASS (20:9 and the Mi 11i's
  shape with its cutout, either hand: no control by the cutout).
- `touch_continue_check.py`, `touch_left_right_check.py`, `touch_pause_check.py`,
  `touch_legend_check.py`: PASS.
- Boot (600 fields) and race (5000 fields) state traces: every row equal to the base build's
  (aded9f56).
- `check.py quick` and `check.py docs`: OK.

Not proven on a real phone (none was connected): how the six feel under a thumb (17 x 6.4 mm, 3.2
mm apart, the 1.9 mm strip between L1 and L2 that is neither's), the HIDE taps on a touch screen
(the checks post mouse clicks through `DHD_TOUCH_MOUSE`), the places against a real cutout and
gesture strips (given to the desktop by `DHD_TOUCH_STRIPS_MM` / `DHD_TOUCH_CUTOUT_MM`), and the
launcher's "Edit the touch layout" on the phones: Android starts the editor alone (`--touch-editor`,
the path the check runs on Windows), iOS opens the same editor over the menus
(`DHD_TOUCH_EDIT_AT_START`); both are `touch.c`'s editor, with HIDE, but neither ran on a device.
