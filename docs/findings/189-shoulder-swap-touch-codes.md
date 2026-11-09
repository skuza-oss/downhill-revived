# 189: Swap L1/L2 and R1/R2 per controller, and the game's codes on touch

Status: current
Checked: 2026-11-09

Branch `feat/shoulder-swap-touch-codes` from `main` ac62c78d (0.7.14), started 2026-11-09. Merged into `main` at 9609ea6f on
2026-11-09; not yet released.

## Goal and acceptance

1. Issue #69: "Swap L1/L2" and "Swap R1/R2" beside Swap Sticks, off by default, saved per
   controller in dhd.ini as Swap Sticks is (`pad1`, `pad2`; finding 157), in the game's REVIVED
   SETTINGS > Controls page and the launcher's Controls tab. For generic pads that report a
   shoulder as its trigger and the trigger as the shoulder.
2. Issue #70: on Android the touch controls can't enter the game's codes (cheats, finding 117):
   the buttons the codes need are not on the touch layouts. Make code entry possible on touch in
   both presets (Classic pad, Thumb zones) without changing the race controls or the game's feel.

Acceptance: `python tools/check.py port` passes; `test_gamepad mapping`, the dhd_settings tests,
the launcher's tests and `test_touch` cover the new parts; the debug APK builds
(`port/android/build_apk.py`); boot and race state traces equal to the base build's (ac62c78d).
Out of scope: phone runs (the lead's), the browser's own pad layout (finding 157 left it fixed).

## Progress

- [x] 2026-11-09: finding reserved (e8e21152)
- [x] 2026-11-09: Swap L1/L2 and Swap R1/R2: dhd_settings, the C table, gamepad.c and the
  CONTROLS page's wait, the game's Controls page, the launcher, docs; tests in dhd_settings
  (`shoulder_swaps`), test_gamepad (`run_shoulder_swaps`), the launcher's Rust and JS tests
  (1c4f50e8); `check.py port` OK
- [x] 2026-11-09: the touch pause panel's CODES page (`port/hle/touch.c`), `test_codes` in
  test_touch with a model of the game's code reader; docs/PLAYING.md
- [x] 2026-11-09: checks (both parts in the build): `check.py port` OK; the debug APK builds
  (`build_apk.py --wasm build/wasm/dhd_core.wasm`); state traces against a build of ac62c78d made in
  its own worktree: boot 600/600 and race 5000/5000 rows equal; the hidden-desktop run below

## Decisions

### Part 1: Swap L1/L2 and Swap R1/R2 (issue #69)

- **What a swap is: the controller's own inputs, as Swap Sticks.** With Swap L1/L2 on, what the
  pad reports as its left trigger (`lt`) counts as its left shoulder (`lb`) and the shoulder as
  the trigger, before the layout reads them (`gamepad_place` in `port/hle/gamepad.c`); Swap R1/R2
  the same for `rb` / `rt`. The trigger's pressure goes with it (L1's pressure byte is the
  trigger's value). Why not swap the PS2's L1 and L2 after the layout: then a binding named `lb`
  would drive L2, the button icons (they show the main input's place) would be wrong, and the
  page would say one thing while the game did another.
- **With rebinding (finding 157): the bindings stay, the wait goes through the swap.** A binding
  names the place the player presses (after the swap). Turning a swap on or off changes no
  binding, Reset turns the swaps off with the rest. The CONTROLS page's wait
  (`host_service_controls_capture`, `pad_held` in `port/hle/host.c`) and the launcher's
  (`padInputPressed`, `placeOf` in `port/launcher/ui/controls.js`) map the pressed input through
  the swaps of the layout being changed, so the input a press binds reads back as that same
  button. So a player with the issue's pad turns Swap L1/L2 and Swap R1/R2 on and needs no
  rebinding at all; one who already rebound the four buttons by hand keeps that working with both
  swaps off.
- **dhd.ini**: `l1l2:swap` and `r1r2:swap` in the controller's line (`pad1`, `pad2`), written after
  `sticks:swap` and `invert:`, as Swap Sticks' `sticks:swap`; `normal` or no entry is off. The
  keyboard has none (refused as "not a button of the keyboard" in dhd.log).
- **The layout keeps its 52 bytes**: `ControlsLayout.unused[2]` became `swap_shoulders` (bits
  `CONTROL_SWAP_L` 1, `CONTROL_SWAP_R` 2) and `unused`; `dhd_controls_edit` takes edit 5
  (`CONTROL_SWAP_SHOULDERS`, target = the bit), the launcher `edit: 'shoulders'` (input = the bit).
- **In the game**: REVIVED SETTINGS > Controls, a controller's second Show page (now named "D-pad
  and Options" for a controller; the keyboard's stays "D-pad and Sticks") has Swap Sticks, Swap
  L1/L2, Swap R1/R2, then the four Inverts: 13 rows. The Buttons page already had 15 rows, the
  most a screen holds. In the launcher the two switches follow Swap sticks, each with a line
  saying what it is for.

### Part 2: the game's codes on touch (issue #70)

How the game reads a code (decomp; finding 117 had the outline): the rider's pad reader
`func_001E6F58` runs in the race update (`func_0011A100` -> `func_0017DD70`), so not while the
game is paused. Every frame it keeps the PS2 pad's buttons (`func_00220230`); its stick part is
dead code (the value it tests is always 0), so **only buttons count, never the stick**. A press
opens a group; the group closes when a button is let go with others still down, or 3 frames
after the press (`D_0029D7D0` = 2), as the OR of the last frames' buttons (`func_001E7160`,
`D_0029D744` = 2 frames). After 20 quiet frames (`D_0029D7D4`) the groups are matched against
the 26 codes (`func_001E72F0`) and cleared (`func_001E7440`); besides, the last 8 new presses of
the d-pad and face buttons (`func_001E72A8`, mask 0xF0F0) are matched every frame
(`func_001E73B0`: only 8-press codes can match there, as shorter rows end in zeros). The code
table (`D_00272448`, read from the retail ELF) uses **only Up, Down, Left, Right, Triangle,
Circle, Cross and Square**. Touch sends no d-pad bit in a race in either preset (the steering
stick is analog; the face buttons are there: Classic's four, Thumb zones' PEDAL, BRAKE and the
flicks), so no code could be typed.

- **The design: a CODES page in the touch pause panel.** The panel's CONTROLS row is split in
  two, CONTROLS and CODES (the panel has room for five rows, finding 141's layout). CODES shows
  the eight buttons the codes use (↑ ↓ ← → in a row, △ ○ ✕ □ below, 9 mm targets), the presses
  typed so far (up to 8, the longest code), CLEAR, BACK and ENTER. ENTER sends the Resume row's
  presses (the cursor to RESUME, ✕), waits 30 fields, then sends each typed press as a short
  press (`PRESS_FIELDS` down, `PRESS_FIELDS` up, counted in the game's VBLANKs at its frame
  rate), the same way the pause rows already send their presses (`seq_*`). Both presets share
  the pause panel, so both get it. Android Back on the page is BACK.
- **Why not buttons in the race**: a d-pad and the face buttons on the race layout would change
  the race controls (the issue's constraint) and need room the layouts don't have; showing them
  only "where codes are entered" means showing them in the race itself, the only screen the
  reader runs on. The paused panel is the overlay's own screen, so nothing about riding changes.
  Typing on a paused game also makes the reader's timing (each press inside 20 frames of the
  last) the overlay's job instead of the thumb's.
- **Why the 30-field lead**: the Resume row's ✕ is still down when the race goes on, so the
  reader may take it as a press; 30 fields (at least 30 frames at any rate) of nothing is longer
  than the reader's 20 quiet frames, so that ✕ is matched alone (no code) and cleared before the
  code's first press.
- **The race touches let go while a code goes** (about a second): a held PEDAL would put ✕ in
  every group (the reader ORs all buttons of a group's frames) and no code would match, as for a
  pad player who keeps Cross down while typing. ENTER drops the touches still down
  (`drop_race_touches`) and new touches are ignored until the presses are sent; a controller used
  meanwhile stops the code (it is the controller's game now). The stick sends nothing then
  either: the rider coasts for that second, as with a pad player's thumbs on the d-pad.
- **Offline only**: the online panel (Close / Bail out / Leave race over the live race) has no
  CODES. Online, codes only work in rooms with CHEATS on (finding 117) and the race never
  pauses; it can be added there later if players ask.
- **No list of codes**: the page types what the player knows, as a pad does; it doesn't name or
  offer the game's codes.

- **The test reads the codes as the game does**: `test_codes` (port/hle/test_touch.c) carries a
  model of the reader above (the decomp's functions, the constants read from the ELF, three rows
  of the table: Codes, Super Bunny Hop, Always Stoked) and feeds it the overlay's pad output,
  one game frame per update, for both presets at 60 and 144 fps, with PEDAL held through the
  pause for one code. Typed by a pad at its own pace the model reads every code, with ✕ held none
  (the reason the touches are let go). With the lead cut to 1 field 8 of the 12 runs read no code;
  without letting go of the touches the PEDAL-held runs (4) read none: the test fails on both.

### The CODES page in the game (hidden desktop, Windows)

`hidden_game.Game` with `DHD_TOUCH_MOUSE=1 DHD_TOUCH_SCREEN_MM=154.5x69.5`, a 1545 x 695 window,
`DHD_TOUCH_LOG=1`, the menus by posted keys to a race (Classic pad, then `DHD_TOUCH_PRESET=thumb`),
then taps: Pause (START), CODES, ↑ △ ↓ ✕ ← ○ → □, ENTER; again Pause, CODES, ↓ □ □ ← ○, ENTER.
Both presets, dhd.log: `cheats: code 1 (Codes), code entry on; on: +7BB3`, then `cheats: code 19
(Always Stoked), code entry on; on: +7BB2 +7BB3`; the touch trace shows the Resume ✕, the quiet
time, then each press 50 ms down and 50 ms up. Presented captures show the pause rows (CONTROLS
and CODES sharing a row) and the page with the typed presses. The script and pictures were kept
outside the repository.

## Surprises

- **Mouse clicks posted to a hidden-desktop game don't land where they are posted** (SDL 3.4 on
  Windows; docs/TESTING.md, Traps): SDL's mouse auto capture reads the real cursor
  (`GetCursorPos`) after a button press, and the window's `WM_MOUSELEAVE` (the real cursor is
  never over it) moves SDL's mouse to the real cursor and drops the focus, after which a release
  is lost. What worked: `SDL_MOUSE_AUTO_CAPTURE=0`, the move, down and up posted together, and a
  tap again when dhd.log's touch trace shows no `up` (a tap without its lift does nothing on the
  overlay's panels). `DHD_NAV` (and the other pad scripts) replace the live pad, so touches are
  not merged in such a run: the menus were driven by posted keys instead.

- The reader's stick part is dead code (it tests a local that is always 0), so a code can only be
  typed with buttons: steering the stick never counts, on a pad either.
- A press of one frame is lost by the reader when nothing else is held: the group closes 3 frames
  after the press and ORs only the 2 frames before, already empty (a release to no button at all
  doesn't close a group). The overlay's 3-field presses are at least 3 frames at any rate.

## Left for the lead (needs hardware)

1. A real pad that reports L1/L2 the other way round (issue #69's CAPSYS GS509, or any pad with
   a mapping that swaps them): Swap L1/L2 and Swap R1/R2 on Controller 1 in REVIVED SETTINGS >
   Controls (D-pad and Options) make the shoulders right in a race, the button icons stay L1/R1,
   and pressing the pad's L1 on the Buttons page's L1 row binds `lb` (the same with the launcher's
   switches and its wait). No virtual pad reaches the game here (finding 157).
2. On a phone, both presets: Pause, CODES, type ↑ △ ↓ ✕ ← ○ → □ and ENTER: the game says CODES
   UNLOCKED; then another code (Always Stoked: ↓ □ □ ← ○). The targets are 9 mm and the page's
   arrows are the 8x8 font's `^ v < >`: readable on the phone? Android Back on the page goes back
   to the rows.

## Result
