# 214: A D-pad as a touch steering choice, and the touch gaps beside the flyover

Status: in progress

Branch `feat/touch-dpad` from `main` dbddf61e (0.7.16 plus findings 199-207 and 209), started
2026-11-12. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Part 1 (GitHub issues #88, #86): a traditional four-button D-pad (up, down, left, right) as a third
choice of the setting that picks the touch steering (`touch_steer`: Stick, Tilt, D-pad), in both
presets (Classic pad, Thumb zones) and both hands. Part 2 (from finding 209's open list): where a
controller's button does something in the front end and a finger cannot: the startup movies and the
attract demo, the results screens, replays, "press START" prompts; each reachable by touch in the
overlay's own style (209's SKIP button is the model).

Acceptance: on Windows with the touch controls at 20:9, the D-pad shown and steering a rider in
both presets and both hands, switched live from the game's settings page, a diagonal and a slide in
the touch log, the menus still navigable; captures at 4x in a scratch folder; unit tests
(`test_touch`) and the settings crate's tests; the Android debug build compiles; the browser
build compiles if its corpus changed; `python tools/check.py port` and `check.py docs` green;
`docs/PLAYING.md` updated. Nothing changes with a controller or keyboard, nor offline without
touch (state traces: left for the lead). Out of scope: a phone (the lead's), changing what is
skippable.

## Progress

- [x] 2026-11-12: finding reserved (1a554fd9)
- [x] `touch_steer` takes `dpad`: settings crate, generated C table, browser corpus, tests (fa30f123)
- [x] the D-pad in `touch.c`, the TOUCH CONTROLS page, `test_touch` (3c67dbd0)
- [x] SKIP also for the movies; `DHD_TOUCH_SCRIPT` for held and dragged fingers in checks (fefdc0f1)
- [x] replay and attract demo: see "Part 2" below
- [x] checks: `check.py port` OK on e15d335c (includes `test_touch: all passed`), `check.py docs` OK, the Android debug APK builds (`python port/android/build_apk.py --wasm build/wasm/dhd_core.wasm`, not installed), `dhd_settings` tests pass; the browser build was not touched (its corpus has the new case; `port/web/check_settings.mjs` was not run)

## Decisions: Part 1, the D-pad

- **What the game receives: the pad's own d-pad buttons, not the stick at full deflection.** How a
  race reads a pad (finding 059, keyboard.c's header): in a race every steering action first reads
  its buttons and only if none is down the stick; a digital press is `1` (full lock: `func_0012DEB0`
  keeps the digital 1 as 1.0, only a stick is scaled), up and down lean as the stick's up and down
  do, and a left or right *double press* is the game's powerslide (a PS2 pad's own d-pad shortcut).
  The keyboard turns its arrows into a ramped stick on purpose (a keyboard has no analogue
  steering and full lock at once is harsh); a D-pad on a touch screen is exactly the PS2 d-pad, so
  it sends `PAD_UP/DOWN/LEFT/RIGHT` (with the pressure byte `touch_host_merge` already sets) and the
  game does what it does for a PS2 pad: nothing new, same inputs. Sending the stick at full
  deflection would have lost the powerslide double tap and the digital read, and would have made
  the D-pad a worse Stick. The trick stick (air), attacks, PEDAL and the other buttons are untouched.
- **Where it sits.** At the stick's home (`TC_STICK`, so the layout editor, `touch_edits` and Edit
  Layout keep working with the same control name; the editor labels it D-PAD): Classic bottom left
  under the L bar, Thumb zones where the stick's ring is; a left hand mirrors the *place*, never the
  arrows (a left arrow is still ←). The default place is kept below the Classic bar (or the top
  buttons) plus the 3 mm gap, above the screen's bottom and clear of the left gesture strip, and in
  Classic clear of L3 by the 3 mm gap (moved left as far as the strip allows; on a phone with a left
  cutout the layout's cutout rule can still push it back toward L3: a gap under 3 mm, never an
  overlap, to check on the Mi 11i).
- **Sizes in mm.** Four rounded arms 9 mm square at button scale 1 (the panels' 9 mm targets),
  `DPAD_HALF_MM` 4.5, scaled by the buttons' scale (`touch_button_scale`, so the same 87 % at a
  62 mm screen) and by the editor's size for the control; arms 3 mm apart (`GAP_MM`, the same
  minimum gap as every control), so an arm's centre is 12 mm from the rest point and the pad is
  36 mm across at Size 100 on the 69.5 mm phone. Kept to the room below the Classic bar / top
  buttons, so at Size 70 the arms shrink (the test asks for at least 5 mm) rather than touch the bar. Opacity
  follows the opacity setting; arms use the buttons' box style, a pressed arm goes green like a
  pressed button; a faint ring marks the rest point.
- **Diagonals, slides, opposites: by angle from the rest point.** A touch that starts on the pad
  (its area is the pad's extent grown 20 %, as every touch area) keeps the D-pad job until it lifts
  (lift to change, as the stick). Each move presses every direction whose axis is within 67.5° of
  the thumb's angle from the rest point: one direction in the 45° band round an axis, two
  neighbours in the 45° band between two arms (a thumb resting between two arms: a real d-pad's
  diagonal), and never two opposite ones (their axes are 180° apart and the sticky reach is at most
  71.5°). Sliding from one arm to the next passes through the diagonal without a gap. Inside 60 %
  of the way to the arms' inner edge (4.5 mm, 3.6 mm once a direction is held) nothing is pressed,
  so the middle is a rest. Bands are 4° sticky (a direction already held keeps up to 71.5°, a new
  one needs 63.5°), so a thumb resting on a band's edge doesn't flap. A thumb that drifts far from
  the pad keeps pressing by its angle, as a held stick keeps its direction. A second finger on the
  pad, or one that lands far from it, is not the pad's. The arms are drawn as four buttons but the
  hit test is the angle, not the boxes: boxes 3 mm apart would leave dead strips between arms where
  a diagonal thumb rests. One implementation: the CODES page's d-pad keeps its own 9 mm boxes and
  taps (it is a paused-panel button grid, not a held control; the two share the order up, down,
  left, right and the pad bits).
- **The setting.** `touch_steer` has three values, Stick, Tilt, D-pad (`dpad`): one definition in
  `port/render/settings/src/table.rs`, the generated `settings_table.h` follows. An older dhd.ini
  has no line for the value and keeps the stick; `touch_host.c` reads it (`TouchSettings.dpad`, with
  `tilt` 0). Android's SETTINGS and the game's SETTINGS > Touch Controls page (page 1, Steer With)
  are one page (`touch_settings.c`); the launcher and the browser's page don't list touch settings
  (they have no touch rows), the browser's corpus (`web_corpus.json`) has a case for a dhd.ini with
  the new value. Stick and Stick Range (page 2) dim with the D-pad (hint "Not with the D-pad"); a
  phone without a tilt sensor skips Tilt when stepping the row (before, Tilt refused the step and
  left the player on Stick: the D-pad would have been out of reach with the arrows).
  Switching live: the page saves and applies at once (`TOUCH_COMMAND_SETTINGS`), `touch_configure`
  lets go of the steering touch when the preset or the steering changes.
- **Menus.** The overlay isn't drawn in the front end (it shows with the race HUD, the pause panel,
  and the one-button moments below), so no menu needs the D-pad: the front end is tapped as before
  (finding 092's mouse layer); the paused panel's rows and CODES page are unchanged. Tested as
  such: the pause rows and the codes page work with the D-pad selected (unit test: the pad lets go
  while paused).
- **The test hooks.** `DHD_TOUCH_SCRIPT` (host, `docs/TESTING.md`): fingers by the race clock.
  Posted mouse messages on a hidden desktop can't hold a finger or drag it (after each posted move
  Windows reports a mouse leave and SDL drops the focus: a later up or move is lost, the mouse
  jumps to the OS cursor) and a mouse is one finger anyway. The script queues the same events a
  screen gives, so a thumb is held and dragged and two fingers can touch together. The
  mouse-as-finger path also drops the OS cursor's jump on a test desktop now, as `present.c` does
  for the menus' mouse.

## Decisions: Part 2, touch gaps beside the flyover

What a controller does where a finger cannot, checked in the decomp and (where it could be) on
Windows with the overlay (`DHD_TOUCH_MOUSE=1`, 20:9, 4x):

| Moment | The pad | A tap before | Now |
|---|---|---|---|
| Startup movies (Skip Movies off), the attract movie, OPTIONS > VIDEO CLIPS | START or Cross (`func_001EB1D8` loop `func_00220618(8 / 0x4000)`, `func_001B53C0`; the legal screens only after 5 s) | nothing: no menu is up, the mouse layer has nothing to press | SKIP (209's button, same place), START on the lift; the press is taken back when the movie ends. Run: the logos and the attract movie show SKIP, a tap on the attract movie logs `movie: mov/attract.pss skipped` |
| The title's attract demo (race mode 10, after idling on the title) | any button ends it (`001A9EA8`, `001B4900`) | nothing: the demo is a race loop, the menus' mouse does nothing while riding; Android Back (△) ended it | SKIP, START on the lift. Not run live (the demo starts after 60 s on the title); unit-tested |
| A race's replay (results > REPLAY, the replay menu's RE-START) | START opens the replay menu (RE-START, SAVE, QUIT, CONTINUE; `0015E1A0`, `0015F730`) | the overlay drew the race controls (they do nothing: the pad is the recording's) and its Pause sent START, then its own PAUSED panel covered the replay menu, its rows pressing ↓ steps and ✕ into the wrong menu (capture: the panel over the replay menu) | MENU (the same button, the replay's START). While the replay menu is up the replay's clock has stopped and the overlay stands aside, so the popup takes taps (finding 092); CONTINUE brings the button back |
| Title "PRESS START" | Cross or START | a tap = Cross (the mouse layer, finding 092) | no gap: unchanged |
| The shop's START, the race setup screens | START | the screen's legend (START) is clickable (finding 092) | no gap: unchanged |
| After QUIT RACE (pause panel > QUIT RACE > YES) | the game's own question, then the title | n/a | no gap: run, the overlay's YES sent ↓ + ✕, the game went to the title ("PRESS ✕ TO START", a tap is Cross), the overlay gone; there is no results screen after QUIT |
| Results after a finish | popup menus (Cross / Triangle) | the mouse layer, if the overlay stands aside | **not run**: no finish could be reached on a hidden desktop (a pedal-only rider crashes within seconds); left for the lead (below) |

- **One mechanism.** The one-button moments are one test (`flyover_on` in `touch.c`: the flight,
  a movie, the demo, a running replay), one hit box (`skip_box`), one job (`JOB_SKIP`) and one
  START pulse that is taken back when the moment ends. `HostTouch` has `movie`, `replay` and
  `attract` beside `flyover` (read in `touch_field.c`: `host_service_movie_playing`, the recorder's
  `playing` byte at D_0043F9F0 + 0x44, race mode 10); no change to what is skippable.
- **Replay detection is the recorder's byte**, which `func_001A8958` clears at the start of every
  race (so a normal race after a replay is a normal race), and the overlay shows MENU only while
  the replay clock moves. In the replay's load and countdown (clock still) nothing shows: START
  there does what it does in any countdown.
- **Android Back** in these moments is unchanged (△, the menus' back).

## What was run

Windows, hidden desktop (`port/tools/hidden_game.py`, driven by scripts kept outside the
repository), `DHD_TOUCH_MOUSE=1 DHD_TOUCH_SCREEN_MM=154.5x69.5`, 1545 x 695 (20:9), `DHD_SCALE=4`,
`DHD_TOUCH_LOG=2`, the menus by `DHD_NAV` + `DHD_TOUCH_WITH_SCRIPT=1` (finding 209's route), the
flight skipped by a tap on SKIP, pedalling by `DHD_TEST_PAD`, the D-pad's fingers by
`DHD_TOUCH_SCRIPT`.

- **The D-pad, all four layouts** (Thumb zones and Classic pad, right and left hand), 4 s holds on
  each arm, each diagonal, and a slide not lifted from the right arm round to the left. Masks in
  the touch log (`sent`): →`0020`, ←`0080`, ↑`0010`, ↓`0040`, up-right `0030`, down-left `00c0`,
  and the slide `0020 0030 0010 0090 0080 0000` (right, up-right, up, up-left, left, lifted):
  every step one or two neighbours, never a gap or opposite pair. Captures at 4x in the scratch
  folder: each layout's arms, diagonals and slide; the rider turns, leans and slides as a pad's
  d-pad does.
- **Part 2**: the movies (the three startup movies and the attract movie with Skip Movies off:
  SKIP shown; a tap on the attract movie logged `sent 0008` and `movie: mov/attract.pss skipped`);
  a replay (`DHD_REPLAY_TEST=8`, Thumb zones): before, the overlay's race controls over the replay
  and its Pause sent START, then its PAUSED panel covered the replay menu
  (`replay-thumb-1/2/3`); after, the replay shows MENU only and its HUD in the game's place,
  a tap sent START (`sent 0008`), the game's REPLAY MENU (RESUME, QUIT, RE-START, SAVE) opened
  and the overlay went away (`visible 0`; `replayfix-thumb-1/2`). A tap on that menu's RESUME by
  posted mouse messages did not act on the hidden desktop (the same click path as the menus'
  mouse layer, finding 092; the touch down arrived, the menu stayed): not shown, so "the
  popup takes the taps" is by construction (the overlay is neither visible nor owning touches,
  `touch_owns_touches`, the state the front-end menus are in), left for the phone. The "before" for
  the movies is the same screens without the button (a touch-off run is identical: the overlay
  never showed there). QUIT RACE: see the table.
- **The settings page, live**: REVIVED SETTINGS > Controls > Touch Controls by posted keys on a
  hidden desktop (the keys drift: several runs landed on the wrong row, one reached it), Steer With
  stepped with Right: the page shows `< D-pad >` (Tilt skipped, its hint "No tilt sensor on this
  device") and dhd.log has `touch settings: touch_steer dpad` (capture `e3-steer-dpad`). The race
  with the D-pad was not run in that same session (the menus' keys and the race route don't mix
  without `DHD_NAV`): the D-pad races above start with `DHD_TOUCH_STEER=dpad`; the live path from the
  page into a race is the lead's phone check, and in the unit tests (`touch_configure` with the
  steering changed lets go of the thumb and lays the pad out or the stick back). The Stick rows'
  dimming under D-pad was not captured.

## Surprises

- The first D-pad runs showed `sent 0030` for every arm: not the layout, the hidden desktop's OS
  cursor jump after each posted move (see the test hooks), which the mouse-as-finger path took as a
  finger move. Fixed in the host's test path; a real screen never does it.
- The unit-test build links `touch.c` alone: it can't call `hle_log`, so the D-pad's trace is the
  host's `sent` lines.
- A hold begun in a frame stall (a 4x run on a busy PC, frames up to 350 ms) is lost: the overlay
  counts as hidden when the race HUD was last drawn over 250 ms ago. Harmless on a phone; the
  script repeats a hold at its end.

## Left for the lead

1. A phone, both presets, both hands: the D-pad's thumb feel (9 mm arms, the 4.5 mm dead zone, the
   67.5° reach), the diagonals, the slide, double tap left or right = powerslide, steering at full
   lock (is it too harsh for riding? it is what a PS2 d-pad does), the arms' place against the left
   cutout (Classic: L3), Size 70 and 105.
2. The settings page on a phone: Steer With cycles Stick, (Tilt), D-pad; the Stick rows dim.
3. Results after a finish and after QUIT RACE by tap with the overlay (the results popup should be
   tappable as the other popups; if the race HUD is still drawn there the overlay would own the
   touches: not checked).
4. Movies by tap on a phone (OPTIONS > VIDEO CLIPS), the attract demo (idle 60 s on the title) and
   a replay (results > REPLAY, MENU, the menu's rows by tap, CONTINUE).
5. State traces: the seven recipes (`boot`, `race`, `race2`..`race6`) against format-2 references
   (docs/TESTING.md). The change cannot move them: with the touch controls off (`touch_on()` is 0,
   no `DHD_TOUCH_MOUSE`) `host_service_touch_field` returns at once, no pad is merged, the new
   `touch_field.c` reads (`host_service_movie_playing`, two game words) only fill a host struct
   and change nothing in the game; no function whose address is taken was added to the core (finding
   141's check 22 cause), so the code-pointer words do not move. `DHD_TOUCH_SCRIPT` is read only
   with touch on.

## Result

(at the merge)
