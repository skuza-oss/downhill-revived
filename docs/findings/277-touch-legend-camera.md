# 277: touch: the menus' legend bar by taps (issue #120) and the camera button (issue #121)

Status: in progress

Branch `fix/touch-legend-camera` from `main` e272b2a2 (0.8.11 and finding 276), started 2026-11-22.
This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

- Issue #120: on a touch screen every word of the game's bottom legend that is shown ("▲ PREV ▼
  NEXT ✕ SELECT △ BACK" on the main menu, the rider screen's with □ INFO, the shop's, OPTIONS',
  the pause pages') works as a tap of its button, on every screen that draws it, through one
  mechanism where the game draws the legend; a desktop mouse gets the same. No new on-screen
  elements.
- Issue #121: the touch camera button (SELECT): find out why a press changes nothing and why the
  camera sometimes changes without one; fix a real cause, or say plainly what was tried.

Done when: a hidden check taps each legend word on the main menu, the rider screen and a pause
page and sees the game react; the existing touch checks (`touch_continue_check.py`,
`touch_left_right_check.py`, `touch_pause_check.py`) still pass; `check.py quick` and
`check.py docs`; boot and race state traces equal to the base's. Out of scope: gameplay.

## Progress

- [x] 2026-11-22: both causes, on the code and in hidden runs (below)
- [x] 2026-11-23: the legend bar's taps, the camera step, unit tests, `touch_legend_check.py`,
  the checks (Result)

## What was true

**#120.** The front end's legend at the bottom is one function, `func_001BAF10(obj, index)`: the
index-th of five retail texts (`D_0028EFA8` .. `D_0028F058`), left-aligned at view 0's x 25, y 199
(0-2) or, smaller, y 204 (3-4):

| Index | Text | Drawn by |
|---|---|---|
| 0 | ▲ PREV ▼ NEXT ✕ SELECT △ BACK | the shell's list screens (main menu, OPTIONS, modes, ...) |
| 1 | ◄ PREV ► NEXT □ INFO ✕ SELECT △ BACK | the rider screen |
| 2 | ◄ PREV ► NEXT ✕ SELECT △ BACK | the rider's page (INFO), some popups (`func_0018C558`), screens `func_001F8AC8` / `func_001F8F50` |
| 3 | ▲ PREV ▼ NEXT ✕ SELECT △ BACK (small) | the shop (`func_0013EF28`) |
| 4 | ◄ PREV ► NEXT △ BACK (small) | the track chooser (`func_001A0730`) |

Taps on it worked only where finding 092 had put a measured table: the rider screen
(`legend_button`), the shop and the track chooser (`game_popup_mouse`). On the main menu and every
other shell list (`shell_mouse`) a tap on the bar did nothing, as the reporter saw; a tap on a row
was the only way. The game's popups draw a second kind of legend, the popup's footer (+0x448: the
pause menu's "△ BACK ✕ SELECT", OPTIONS' "◂ LESS ▸ MORE ..."), whose words take taps since
findings 266 and 274 (`legend_tap`); the pause pages draw no bar.

**#121.** SELECT alone changes no camera. The game's own CONTROLS page says "camera w/ up,down or
L2 button": the camera is SELECT held + the d-pad's Up (the next view) or Down, look back SELECT +
L2. Measured in hidden races (`DHD_RACE_PAD`, window pictures at race times): SELECT alone, no
change; SELECT + Up, close; again, first person; again, far; again, the default (Up cycles four
views and wraps; Down goes back); SELECT + Up together in one field works too, even for 3 fields.
The left stick held up or down with SELECT (before it or after it) changes nothing. The touch
camera button sent SELECT only, and neither layout has a d-pad (the D-pad steering option has, but
nobody knows to hold the camera button and press its arm), so **the button could never change the
view**: "when pressing, nothing changes" is exactly the code.

"It sometimes changes when not pressing": not reproduced. With SELECT needing d-pad Up/Down, a
camera change without a camera press is impossible from touch; with Steer With: D-pad a camera
press that waited for the bail window (a second tap inside 30 fields, sent up to half a second
later) can meet a thumb on the D-pad's up or down arm, which then changes the view after the
finger has left the button. That is the one path found; the reporter's layout and steering mode
are not known. Placement was checked too: the button's touch area is the drawn one (both from
`ctl[TC_CAMERA]`), it moves with a cutout on either side (Android gives the cutout's safe inset
left or right; landscape's other side is a screen change, laid out again), and the steering area
never reaches it (the top buttons are on the action side). Nothing there takes or eats a press.

## Decisions

- **The bar, one mechanism**: a patch reports each `func_001BAF10` draw (`port_legend_bar`, host
  memory only); `menu_mouse_field` takes a tap on the bar's line (from 8 above its text to the
  picture's bottom) and on one of its words before any screen's own rules, measuring the drawn
  text with the bar's own sizes and finding 266's word ranges (`menu_legend_ranges`: glyph to word
  end, never overlapping, nothing between words). The reader learnt ▲ (0x90) and ▼ (0x9D) as Up and
  Down. Every screen that draws the bar gets it, a desktop mouse too. The rider screen's measured
  table (finding 092's `legend_button`) is deleted: the bar is the legend it measured. The tables
  for legends that are not the bar stay: the shop's own line ("△ BACK ✕ SELECT START", drawn by
  the shop, not by `func_001BAF10`), BONUS PURCHASES' items, the track chooser's ✕ (its bar 4 has
  no SELECT word); where a bar word lies under one of them, the bar's tap comes first and presses
  the button the bar shows.
- **The camera step on the lift**: a camera press with nothing else pressed while it was down ends
  as SELECT + d-pad Up for PRESS_FIELDS, SELECT held throughout (one SELECT: never a bail); a press
  the game never read goes as both at once. A press combined with another (a button, PEDAL, the
  D-pad, the trick stick; `camera_mark`) stays SELECT alone, so look back (SELECT + L2) and the
  D-pad's own camera work as on a pad. On the lift, not the touch: only then is it known that
  nothing else came. Up and not Down: Up cycles all four views and wraps.
- **Log, don't ask**: each camera press says what it sent (`touch: camera: the next view (SELECT +
  Up)`, `..., after the bail window`, `... SELECT with another button`), and on phones the screen
  line (size, dpi, gesture insets, cutout) is in dhd.log at every change, not only with
  `DHD_TOUCH_LOG`; `touch: the camera button at X,Y mm` beside Pause's place.

## Surprises

- The rider's page (INFO) has no BACK of its own: △ there leaves the rider screen for the main
  menu (a pad's △ does the same).
- The bar's pictures on a 16:9 phone (the issue's screenshot) and the trace's ranges agree: view x
  25 of the 640-wide shell is the 4:3 picture's left edge plus 4 %.

## Result

Commits on `fix/touch-legend-camera`: the lane plan; the legend bar by taps; the camera step; the
check tool, the docs and this result.

- Unit tests: `test_menu_pads` (the bar's texts 0 and 1: every letter of each word, each glyph,
  PREV Up / NEXT Down on 0, Left / Right and INFO's Square on 1, the gap before SELECT nobody's);
  `test_touch` `test_camera_step` (both presets: a camera tap is one SELECT with Up joining it for
  3 fields after the lift, never a SELECT gap; a tap the game never read is SELECT + Up at once; a
  camera press with L2's bar (Classic) or PEDAL (Thumb zones) down during it sends no Up).
- Hidden runs (`port/tools/touch_legend_check.py`, 1600x720 at 20:9, `DHD_TOUCH_MOUSE=1`, no
  controller): `menus` PASS: on the main menu NEXT (cursor 0 → 1), PREV (→ 0), SELECT (the rider
  screen); there NEXT (rider 1 → 2), PREV (→ 1), INFO (the rider's page, bar 2), BACK (the main
  menu), each tap logged `the legend bar's <button>`. `pause` PASS: no bar over the pause menu; OPTIONS
  by its row, the footer's BACK (the pause menu again), the footer's SELECT on RESUME (the race goes
  on). `camera` PASS: four taps at race 12, 15, 18, 21 s, each changes the picture (mean difference
  13.5, 17.2, 29.8, 27.1 against 3.1 with no tap; pictures: close, first person, far, default), two
  taps 0.25 s apart: the second waited for the bail window and stepped the view, no bail. Before the
  fix (the same touches on this branch's build before the camera change, and `DHD_RACE_PAD` SELECT
  alone): no view change.
- Existing checks on this build: `touch_continue_check.py` (shop, four card taps),
  `touch_left_right_check.py` (shop, audio), `touch_pause_check.py`: PASS.
- State traces: boot (600 fields) and race (5000) equal to the base build's (e272b2a2, built here).
- `check.py quick`, `check.py docs`.
- Not run: a phone (none connected). Unproven on a real phone: the bar's taps at a phone's own
  aspect (the trace's ranges are view coordinates, the same mapping as every other menu tap), the
  camera step on HyperOS (the reporter's Redmi Turbo 3), the reporter's "changes when not
  pressing" (not reproduced; the D-pad path in "What was true" is the one found). The next dhd.log
  from a phone says each camera press (`touch: camera: ...`), the screen's gesture insets and cutout,
  and where the camera button is.
