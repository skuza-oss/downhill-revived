# 226: Touch layout freedom and tablet defaults

Status: partly superseded by 228 (the launcher's entry point on Android: "The launcher's entry point, and main's")

Branch `feat/touch-layout-freedom` from `main` 5a5b0cb8 (0.8.0 plus the iOS platform), started
2026-11-13. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

The owner's decision (2026-11-13): two ideas of the community iOS fork's touch controls (finding
224, "His touch controls and launcher next to ours") move into our shared overlay
(`port/hle/touch.c`), for Android and iOS both: per-control size and place as free as his, and
defaults for tablets and iPads. Plus: undo and redo and a size readout in the editor, an entry
point that starts the game straight into the editor (for the shared mobile launcher, lane 225),
and tilt steering on iOS (CoreMotion through the host call Android uses).

Done when: the unit tests in `port/hle/test_touch.c` cover each new or changed rule; a 0.8.0
`touch_edits` string loads and lays out exactly as before; `check.py port` and `check.py docs`
pass; a boot and a race state trace equal the reference; captures before and after at a phone, a
small tablet and a 13-inch tablet size; the phone shows the editor, a race with an edited layout
and the owner's layout unchanged; the iOS Simulator app builds on the Mac from the branch.

Out of scope: portrait layouts, languages (other lanes or not chosen), the launcher itself
(lane 225), a per-slot action swap (only if nearly free: it is not, see Decisions).

## Progress

- [x] 2026-11-13: finding reserved (2b7e60cf)
- [x] the overlay's rules: sizes 25-600 %, placement, undo/redo, readout, pinch, held -/+,
  tablet defaults, the 0.8.0 layouts kept (9a07041b)
- [x] the editor alone for the launcher, `DHD_TOUCH_EDITOR=1` / `--touch-editor` (c003d77c)
- [x] tilt steering on iOS through CoreMotion (03fe2614)
- [x] docs: PLAYING, STATE, TESTING, the Android and iOS notes (d541df81)
- [x] the iOS tests' path check on macOS (b3cc2015)
- [x] the phone proof, the captures, `check.py port`, the state traces, the Simulator build
- [x] the lead's report of the first on-screen iOS run: top buttons over the HUD's PUNCH meter,
  SLIDE on HOP, SKIP over the demo's text; fixed for phones and tablets (02a5f2f7)

## Decisions

**The size range.** A control's own size (`TouchEdit.size`) runs from 25 % to 600 %
(`TOUCH_SIZE_MIN`, `TOUCH_SIZE_MAX`), as his. The limit is the screen: the edited control must lie
wholly on it. His lets a 600 % button pass the view's edges with its centre clamped
(`limitedCenter`); ours keeps the whole control visible, because a control partly off the glass
loses touch area where the player cannot see it. So on a 154.5 x 69.5 mm phone PEDAL stops near
the screen's height (about 290 %), a small button or the trick stick's spot reaches 600 %; on a
13-inch tablet much more fits. - and + step 5 % up to 100 %, 10 % to 200 % and 25 % above (37
steps over the range), and a held button repeats (400 ms, then every 90 ms); + at an edge moves
the control inward instead of refusing; past what fits, + stops at the largest size that fits
(a search, not a refusal). The Size setting (one number for all) still multiplies every control.

**Overlap.** Two controls may touch but not overlap their drawn shapes (finding 141 wanted 3 mm
between every two). His allows any overlap. Ours routes a touch in two touch areas to the nearer
control, so overlapping shapes would make one control press another where they meet, out of
sight under the other's symbol: what the player sees must be what a touch presses. The default
layouts keep their 3 mm gaps (`test_tablet` checks them on the tablets).

**Placement, his against ours** (read in his `TouchLayout.swift`, `ControlsCustomizationView.swift`
and our `touch.c`):

| | His | Ours before | Ours now, and why |
|---|---|---|---|
| Screen edges | centre clamped, a larger control may pass them | a drop off the screen went back to its start | the control stays wholly on the screen; dragged past an edge it stops against it (a corner is one drag) |
| Margins | the editor's canvas inset by the safe area plus 16 points | none | none: every millimetre of the screen is the player's |
| Snap | none (dashed centre guides) | none | none; the editor alone draws faint centre lines |
| Gaps | none | 3 mm between any two | no overlap of drawn shapes (above) |
| Gesture strips | none | the stick's home and the trick spot may not start in them | kept: a drag that starts in a strip is the system's (back gesture), it never reaches the game |
| Camera cutout | the safe area | a control over it moves inward | kept: nothing can be seen or pressed there |
| Flick lanes | none (no flicks) | no control in PEDAL's lanes | kept: a flick through a button would press it |
| A drop where it does not fit | — | back to its start | to the last place on the way where it fitted |
| The editor's bar | separate panel | not checked | the edited control may not cover the bar's buttons (they must stay usable) |
| HUD | — | the D-pad's default clear of the track bar | unchanged: only defaults avoid the HUD; a player's own drag over it is kept |

**The editor** (his against ours; taken where cheap in our overlay):
- taken: undo and redo (32 steps; his 40) of the preset's edits, a gesture, a run of - / + or a
  RESET being one step each, and a new change dropping what was undone;
- taken: a readout of the selected control in per cent beside its millimetres, with its name
  (SQUARE, CAMERA, SELECT, ...: the old one had empty names for the symbol buttons);
- taken: hold - / + to repeat, coarser steps above 100 % (his slider has 1 % steps; a slider in
  our 8x8-font overlay would be fiddly on a phone, the pinch does the fine sizing instead);
- added (his has none): a two-finger pinch on the held control;
- not taken: his control picker list (needed in his because controls can hide under others; ours
  cannot overlap), hiding controls (not asked for), Discard Changes (UNDO covers it), separate
  portrait layouts (not chosen).
- **An action swap per slot is not nearly free**: each control's press is fixed in the routing
  (`route_thumb`, `route_classic`: PEDAL is ✕ with its sprint and hold-back guards, its flick
  attacks, Classic ✕'s slide onto ◻ / ○ and the + spots, SELECT's bail guard, HOP's air-wait
  tap); a swap needs an `action` per `TouchEdit`, the saved format grown, a picker in the editor,
  and every guard keyed to the action instead of the control, with their tests. About a day's
  lane; left for the owner to ask for.

**Tablet defaults.** A screen whose short side is `TOUCH_TABLET_MM` (90 mm) or more is a
tablet's, from the size the host reports, never the platform's name: phones end near 78 mm
(iPhone Pro Max 70.7, large Androids about 75), the smallest tablets start near 94 mm (7-inch
16:10), an 8.3-inch iPad mini is 115.9 mm, a 13-inch iPad about 199 mm. On a tablet:
- the base size is `TOUCH_TABLET_SCALE` (1.25, his iPad `baseScale`); the Size setting is a share
  of it (Auto and 100 % are 125 % of a phone's), buttons by the phone's rule relative to the base,
  so the 3 mm gaps hold at every Size;
- the bottom clusters (anchored to the lower corners as on phones) move up a quarter of the height
  past 80 mm (at most 28 mm) and in a tenth of the width past 160 mm (at most 12 mm): on a 13-inch
  iPad the stick's home sits about 58 mm up and 51 mm in (his 0.16 / 0.75 of the view: 42 mm in,
  50 mm up), PEDAL about 50 mm up and 40 mm in;
- the top buttons and SKIP move as on phones (below), SKIP 40 % up instead of at the bottom;
- the picture and the screen: the touch controls are placed in the screen's millimetres, not the
  picture's. A 13-inch iPad's 264.8 x 198.6 mm screen is 4:3, so the game's 4:3 picture fills it
  and the controls lie over the picture's lower corners, as on a phone over a wide picture. Where
  the picture is narrower than the screen (a 4:3 picture on a 16:10 tablet with Widescreen off),
  the side bands are the screen's: the clusters anchored to the screen's lower corners sit partly
  or wholly in them, which covers less of the picture; nothing is placed relative to the
  picture's edges;
- Hand: Left mirrors all of it; Android tablets and iPads run the same code (`big_screen`,
  `lay_out`, `tablet_top_place`, `skip_box`).

**Default places on every screen (the first on-screen iOS run, the lead's report).** The
iPhone 17 Pro Max Simulator (153.6 x 70.7 mm, the Dynamic Island's 9.9 mm band) showed three
faults of the default places that phones of that shape share:
- The top buttons in the top-left corner covered the end of the moved HUD's PUNCH meter and its
  label (finding 141's HUD hook puts the speedometer and that meter in the corner; together they
  reach about 0.9 of the screen's height across). They now sit in a row just above the action
  cluster's touch areas, Pause at the action side's usable edge (less the gesture strip and a
  cutout band on that side; `top_place`, `action_edge`), on phones as on tablets; Hand: Left
  mirrors them to the top left, where its HUD is not (it goes to the top right). The editor's bar
  avoids the row's default place; the Thumb zones steering area takes the whole height.
- The camera cutout pushed only the control over it, into its neighbours: Thumb zones with Hand
  Left put SLIDE on HOP (the island on the left), and on the Mi 11i (its cutout band on the
  right) Classic's ○ moved within 3 mm of ✕ and the + spot, which also locked 0.8.0's editor on
  that phone (its check of the whole layout failed after any drag or size step: found when the
  phone's 0.8.0 editor reverted every drag in this lane). Now the whole half moves away from the
  cutout together (`cutout_shift`, from the places before edits); only an edited control still
  over it gets the old push.
- SKIP, low in the middle, covered the attract demo's own PRESS ANY BUTTON TO EXIT (centred,
  about 35-65 % across): it now sits under the action thumb, 8 mm in from the usable edge, at 86 %
  of the height (40 % up on a tablet).
`test_phone_defaults` checks the three on the iPhone (island on either side) and the Mi 11i, every
preset, hand and attack mode. A preset with 0.8.0's edits keeps 0.8.0's places (top buttons and
the cutout's push included); SKIP is not part of a layout and moves for everyone.

**Saved layouts.** `touch_edits` keeps its format (`preset.control=dx,dy,size;...`); the parser
takes sizes 0.25-6.00 and moves up to 1000 mm. A preset laid out on the current places also writes
`preset.layout=2`, which 0.8.0's parser skips. A preset whose edits come without it was edited on
0.8.0's places: it keeps them on every screen (`TouchSettings.legacy`), so no update moves a
player's layout, on a phone (nothing changed there) or on a tablet (it would have moved with the
new defaults); RESET puts it on the current places, as one undo step. A 0.8.0 build reading a new
string ignores the mark and any size outside 0.5-2.0 (that control shows at its default).

**The editor alone (lane 225's entry point).** `DHD_TOUCH_EDITOR=1`, or the argument
`--touch-editor` (`touch_host_args`, from `launcher.c`'s main: the desktop and iOS's `dhd_main`
argv; Android's `SDL_main` passes it on from the activity's arguments). The game starts muted
(`DHD_SND_MUTE`) with its movies skipped, the host measures the window before any touch, opens the
editor over an opaque plain background with faint centre lines once the screen's size is known,
and after Done or Back has saved `touch_edits` it closes the game as EXIT GAME does
(`gs_window_close`, `host_end`), so the launcher that started it shows again. The game's boot runs
behind the editor (nothing of it shows or plays); the editor needs no disc menu or race. Android:
the launcher's activity starts `DhdActivity` with `--touch-editor` among `getArguments()`; iOS: the
launcher calls `ios_start_game` with it in argv (or sets `DHD_TOUCH_EDITOR=1`) and gets control back
when the process would end (iOS: the game process is the app; lane 225 decides how the app returns
to its launcher).

**The launcher's entry point, and main's (lane 225, the mobile launcher).** At this lane's merge
main's launcher still started the game with `DHD_TOUCH_EDIT_AT_START=1` (the editor over the
game's menus). Finding 228 joined the two: the launcher's "Edit the touch layout" now starts the
editor alone through `--touch-editor` and shows the launcher again after Done;
iOS's launcher (finding 227) keeps `DHD_TOUCH_EDIT_AT_START`: its app can't come back after the
editor alone ends the process.

**Tilt on iOS.** `metal_bridge.m` gives `touch_host.c` the two calls Android's `DhdActivity`
gives it: `ios_touch_tilt_on` (CMMotionManager device motion, else the accelerometer, started and
stopped on the main thread without waiting) and `ios_touch_tilt` (the roll from gravity in the
screen's axes by the interface's orientation, Android's `atan2(-sx, sy)`). The host's tick now
drives both platforms through one block. Unlike Android, the screen is not locked to its
landscape side while tilt steers: iOS turns the interface only well past any steering tilt.

## Surprises

- The old editor test "dropped onto PEDAL it snaps back" passed only because the drop went back
  to its start; with the new last-fit rule the control stops at PEDAL's rim. The test now checks
  that it is never over PEDAL and no further than where it started.
- A race leaving the screen dropped every finger, the editor's too (the editor's drag stopped
  250 ms after the editor opened over a race that had just ended): the editor's fingers are now
  its own.
- The host learned the screen's size only from the first touch or a window event; the editor
  alone needs it before any touch, so the tick now measures the first window (desktop and
  Android; iOS already measured each second).

- On the Mi 11i, 0.8.0's editor reverted every drag and every - / + in Classic pad: its cutout
  band (149.3-154.4 mm) pushed ○ within 3 mm of its neighbours, so 0.8.0's whole-layout check
  failed after any change (seen on the phone with the published 0.8.0 APK in this lane). The
  lane's build drags, sizes and saves there (the per-control check, the cluster shift).
- `package_release.py --platform android --out X` writes the native symbols to X and the APK to
  `build/android/DownhillRevived-<version>-android-experimental.apk`; installing X fails with
  INSTALL_PARSE_FAILED_NOT_APK.
- iOS: `test_explicit_module_is_used` failed on the Mac before this lane too (macOS's temporary
  folders resolve to /private/var).

## Result

What a player can now do in the layout editor (TOUCH CONTROLS > Edit Layout), on Android and iOS:
size each control from 25 % to 600 % as far as the screen holds it (hold - / + to repeat, or
pinch with a second finger), place it anywhere wholly on the screen (a drag past an edge stops
against it; it may touch another control but not cover it), undo and redo the last 32 changes,
and read the selected control's name, millimetres and per cent. Tablets (90 mm or more on the
short side) get larger defaults nearer the thumbs; on every screen the top buttons sit above the
action cluster, clear of the HUD's PUNCH meter, the cutout moves a whole side, and SKIP is under
the action thumb. `touch_edits` written by 0.8.0 loads and lays out as before (unit test with
0.8.0 strings; the phone's own empty layout stayed byte-identical in dhd.ini).

Checked:
- `port/hle/test_touch.c`: every earlier test plus `test_size_range`, `test_screen_bounds`,
  `test_tablet`, `test_phone_defaults`, `test_old_edits`, `test_undo_redo`, `test_editor_alone`
  (Windows and the Mac).
- `python tools/check.py port -j 6`: OK on 1e601e82 (the last code is 02a5f2f7); `check.py docs`
  on the last commit.
- Boot and race state traces of 1e601e82's build against `main-524948ba-ref-v3`: boot 600 of 600
  rows equal, race 5000 of 5000 (`state_diff.py` PASS; touch is off in them).
- Windows captures at 4x (hidden desktop, `DHD_TOUCH_MOUSE=1`, `DHD_TOUCH_SCREEN_MM`), before
  (5a5b0cb8's build) and after, each at a phone (154.5 x 69.5 mm), the iPhone 17 Pro Max
  (153.6 x 70.7), an iPad mini (176.4 x 115.9) and a 13-inch iPad (264.8 x 198.6), Thumb zones
  and Classic pad: the flight (SKIP) and the race; the editor alone after. Kept outside the
  repository. The desktop has no cutout: the cutout's cases are the unit tests'.
- The Mi 11i, a release-signed APK built as 0.8.0's gate APK was: the editor (✕ 25 % by a held -,
  one UNDO back to 100 %, L3 dragged into the corner and grown to 456 % where it meets the L bar,
  RESET), Done saving `classic.layout=2;classic.cross=0.0,0.0,1.20`, a SINGLE EVENT race with
  that layout (pedalling at 35 mph, the HUD readable, START top right), then RESET through the
  editor: dhd.ini and the four save files byte-identical to before; the published 0.8.0 APK back.
- iOS: the Simulator app builds on the Mac from 1e601e82 (the Windows build's core parts, which
  equal lane 224's); `port/ios/tests` 23 tests OK there; `test_touch` passes there. Not run on a
  simulator's screen (the brief's on-screen worker does that).

Not checked here: the editor alone on a phone (checked by finding 228 on the Mi 11i), tilt on an iPhone (no device; the Simulator has no motion), a pinch with
real fingers (the unit test drives it; the device tool's pinch is symmetric about a point).
