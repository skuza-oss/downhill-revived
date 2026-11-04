# 141: touch controls for phones

Status: current
Checked: 2026-11-04

Branch `feat/touch-controls` (from `main` 0330eab, 0.7.7), merged into main and run on the Mi 11i
(section "Phone test (Mi 11i)"). Built for Windows x64 and Android arm64; unit tests pass. No
change to `src/`.

The design is the reviewed touch-controls spec (round 14: two presets, Thumb zones and Classic
pad, tilt as a setting, every touch plain PS2 pad input, no assists). This finding says what was
built from it, where, the two game reads, the choices made where the spec left room, and what is
left.

## What was built

| Piece | Where |
|---|---|
| The rules: layout and scaling, the steering stick, PEDAL (✕ with the hold-back, the sprint guards, the flick-lift hold), flick attacks, HOP / BRAKE / SLIDE, camera with the bail guard, Pause, the air layout and the trick stick, Classic pad (shoulder bars with the both-press middle, + spots, ✕ slide-on, L3/R3, SELECT/START), tilt, Hand (mirror), size / opacity, the pause rows as ↓ / ↑ steps then ✕, Controls and Bail out, the online panel (Close / Bail out / Leave race with tap-again arming), the resume guard, hiding for a controller, Android Back, vibration, trick labels, hints, the layout editor, and the overlay's shapes | `port/hle/touch.c`, `touch.h` (platform-neutral: no SDL, no OS, no game memory) |
| Unit tests: scaling, flick lanes, sprint and resume guards, the trick stick (push, roll through the centre, overshoot before the gate, first-latch travel and the 12° hysteresis, landing, signature, the air wait), pause-row sequencing, Classic slide-on and bars, the online panel, the layout text | `port/hle/test_touch.c` (`test_runtime.py` runs it as `touch`) |
| SDL glue: finger events, the desktop's mouse as a finger, Back, controller use and loss, the screen in mm, settings in and out of dhd.ini, the overlay's hand-off, player 1's pad merge | `port/host/touch_host.c` |
| Android: density, the system gesture insets and the cutout (JNI), the vibrator, tilt (gravity sensor, screen locked to its side while tilt is on) | `port/host/android_touch.c`, `DhdActivity.java` |
| The game side once a field: the air flag, the HUD hook, the race clock, the pause menu's rows, Leave race, the hint's hold | `port/hle/touch_field.c` (core), `pad.c` calls it |
| The HUD hook and move | `port/hle/widescreen.c` (`port_ws_hud_offset`, `port_hud_text_c/_l`, `touch_hud_move`) |
| TOUCH CONTROLS 1/3-3/3 in the game's popup style | `port/hle/touch_settings.c`, from PC SETTINGS' new Touch Controls row (`pc_settings.c`); the page is SETTINGS on Android |
| Settings keys | `port/render/settings/src/table.rs` (`touch_*`; `settings_table.h` and the browser corpus regenerated) |
| The overlay's drawing | `port/render/src/present/touch.rs` (`dhdr_surface_touch`), font `present/font8x8.rs` |
| Leave race online | `online.c` `port_online_touch_leave` (as BACK TO THE ROOM's Yes, without the question) |
| No catch-up after the hint's hold | `fps.c` `port_clock_rebase` (main's, finding 139) |

## The two game reads

1. **The rider's air flag**: player 1's rider is slot 0 of `D_002AF2C0` (10 riders, 0x7BD0
   each, as `puppet.c` and `online.c` have it). Its rigid body is at +0x40C0 and the body's
   +0x4A4 is the airborne age, a short that `func_00135A30` counts up every frame in the air and
   resets on contact (finding 049, "Airborne frame counter"; finding 090 names it rider
   +0x4564). Above 60 fps `port_air_tick` (finding 067) keeps it, so the flag is
   `*(int16_t *)(rider0 + 0x4564) != 0`, read on the race screen only (`D_0029DED8` = 9, phase
   `D_0043F348` 1-4). Read in `touch_field.c`.
2. **The HUD hook**: the race HUD is drawn by the HUD's own code: its 3D pieces (speedometer,
   track bar, place) through `func_00176930`, which `patches_widescreen.py` already routes through
   `port_ws_hud_offset`, and its text through `port_hud_text_c` / `port_hud_text_l` (units listed
   in `HUD_TEXT`). Each call now sets `touch_hud_seen`; on the race screen that is "the race HUD
   is drawn", which is when the overlay shows, when "mid-race" applies and when hint timers run.
   The same calls move the HUD while the touch controls are on (`touch_hud_place`): pieces whose
   place in the 4:3 view is in the bottom-right cluster (u > 2/3, v > 0.62: the speedometer below
   v = 0.755, the energy arrows above it) move to the top-left corner for the right hand (the
   arrows beside the speedometer), or straight up to the top-right for the left hand, where the
   left side's pieces mirror across. 3D pieces move in camera space (their 4:3 place from the
   view's focal length, the height three quarters of the width), text in the view's pixels;
   widescreen's anchoring then applies as before. The shifts come from the spec's pictures; they
   are a guess until check 7, so `DHD_TOUCH_HUD_SHIFT="su,sv,au,av"` (fractions of the view:
   the speedometer's and the arrows' shifts for the right hand, default `-0.75,-0.75,-0.65,-0.67`)
   tunes them without a build. `DHD_TOUCH_HUD=game` leaves the HUD alone and uses the spec's
   fallback layout (PEDAL at 80 % / 62 %, HOP, BRAKE, SLIDE above it).

Besides the two reads, `touch_field.c` passes the race clock (`D_0043EEC8`, fields since GO, as
ms through `port_field_seconds`; it stops while paused, so every window counts race fields, never
frames: findings 049, 067, 114) and whether it moved, and the offline pause menu's rows. The rows
are the spec's "pause popup hook" class of read (a menu read): `func_0015E238`'s own conditions,
re-evaluated from `D_004B25A0` (+0x10C, +0x110, +0x288) and the save's `D_004225A0` +0x12199 /
+0x1219F, give the row of RESUME (always 0), RE-START (0x22; none in mode 17) and QUIT (0x1A,
last), so a tap is sent as the right number of ↓ steps (finding 050 lists the rows).

## Decisions

- **Drawing: the Rust renderer's host overlay**, next to the FPS & Ping counter (`present/`):
  drawn after the game picture in surface pixels, so it is crisp at any resolution scale, never
  touches GS memory or game state, and costs one instanced draw. The shapes are signed distances
  (two-tone rims readable on snow, as the spec asks), so no textures or assets are needed; text
  uses the public-domain font8x8 table the counter already cited, now shared by both
  (`font8x8.rs`). The C side passes floats (16 a shape) rather than a struct, so the frozen ABI
  layouts (`abi.rs`) are untouched. Drawing through the game's own font was not chosen: it would
  need GS memory and game-time draws, and the overlay must show while the game is held for a hint.
- **The rules are one platform-neutral C module** (touch.c), host side, fed in mm and ms. The SDL
  thread only queues events; the game thread drains them once a field, so the module is
  single-threaded and the tests drive it exactly as the game does.
- **Two clocks**: motion (settle 70 ms, gate 60 ms, △ +33 ms, hold-back 120 ms, flick speed) in
  wall ms of the touch events; the game's windows (✕ double-tap 30 fields, SELECT twice 30
  fields, the flick-lift hold, the resume guard, the air wait) in race time, which stops while
  paused and during the hint's hold. Presses the game must see (3 fields) count the game's
  VBLANKs scaled to the frame rate, so they work in menus and paused too.
- **The SELECT double-press window** is taken as 30 fields like ✕'s (the spec asks check 3 to
  measure it); one constant in touch.c.
- **Pause rows**: the overlay draws its own PAUSED panel (the spec's picture) over the game's
  menu; Resume / Restart race / Quit race are sent as ↓/↑ steps from where the overlay last left
  the cursor, then ✕; Restart and Quit then show the game's question as an overlay No / Yes:
  No sends △, Yes sends ↓ + ✕ (assumes the question opens on No: check 10). Pause on the panel
  is Resume. Back on the rows is Resume, on the question No.
- **The menu-open chain**: the overlay's own START; if the race clock was running, the menu
  counts as open once the clock stops, and if it is still running 0.5 s later START was ignored
  and the race touches work again. If the clock moves again while the panel shows (a controller
  closed the menu), the race resumes with the resume guard.
- **Synthesized mouse**: SDL turns touches into mouse events too. While the overlay shows they
  are dropped (the game's popups would take them as clicks); in the menus they stay, so the menus
  (finding 092) take taps as before.
- **Leave race** is not a pad input: it calls the same path as the room's BACK TO THE ROOM Yes
  (`leave_race`), since a press sequence through the online pause menu and its question would
  depend on unread menu state.
- **Android Back**: a race pauses (offline) or opens the panel (online); in menus it is △ (it
  did nothing before, as `SDL_HINT_ANDROID_TRAP_BACK_BUTTON` keeps it from closing the game).
- **Tilt**: full lock at 40° (strength 0 %) to 10° (100 %), 25° at the default 50 %; on the
  ground only (the stick's left/right is ignored there), blended over 100 ms at take-off and
  landing. Set Tilt Centre stores the current roll.
- **Settings**: 3 screens of the PC SETTINGS page (the game's popups need rows of one page:
  `menu_page_rebuild`), every change saved at once. On Android the page is SETTINGS and drops the
  Display and Window Size rows (one screen; the popup holds 16 rows). The 9 mm half-split value
  targets of the spec are the menus' existing mouse layer (a click on the value's arrows steps it;
  Cross steps forward).
- **Hints**: tip boxes in the overlay, each once (dhd.ini `touch_hints`), never online. The
  trick-stick hint holds the game in `touch_field.c` (the VBLANK waits, three VBLANKs after the tip
  appears so the pictures showing it are on screen; nothing is presented during the hold) until the first push, a lift, PEDAL, Back or 5 s; afterwards `port_clock_rebase`
  makes sure no game time is caught up (above 60 fps the wall clock would otherwise repay up to
  half a second).
- **Touch on a desktop** (`DHD_TOUCH_MOUSE=1`): the left mouse button is one finger, for the
  lead's checks and development; `DHD_TOUCH_SCREEN_MM=154.5x69.5` makes the window the Mi 11i.

## Stubbed or left out

- The left-handed fallback layout's notch in the steering area (only with `DHD_TOUCH_HUD=game` and
  Hand Left): not built; the spec calls it rare.
- "Holding repeats" on the settings rows: the menus' own left/right repeat applies to pads and
  keys; a held tap does not repeat.
- Trick labels show the name in the 8x8 font with `*` for ★ (the font has no star).
- The lanes and touch areas are not drawn in the race (the spec's red wedges are explanatory);
  the editor enforces them.
- Rolling and styled tricks' hints "in later races": only the first trick-stick hint and its
  repeat are built.
- Menu taps in the game's own menus beyond what finding 092's mouse layer already does are, as the
  spec says, a design of their own.

## Checks for the lead

Builds: `python port/tools/build.py -j 6` (Windows), `python port/android/build_apk.py --wasm
build/wasm/dhd_core.wasm` (APK). On the PC every check below can run with the mouse as a finger:
`DHD_TOUCH_MOUSE=1 DHD_TOUCH_SCREEN_MM=154.5x69.5 DHD_SCALE=2 DHD_WIDESCREEN=1` (a 20:9 window, e.g.
`DHD_WINDOW_WIDTH=1540 DHD_WINDOW_HEIGHT=693`), started by hand or with `hidden_game.Game(..., env=...)`
and its `move` / `click`; the phone with the APK. dhd.log has `touch: ...` lines (layout, screen in
mm, saves, the hint's hold) and `touch settings: ...` for each change.

The spec's own game checks first (the PC build with scripted input answers them; no touch needed:
`DHD_RACE_PAD=start:length:button,...` in a race, `DHD_RACE_SYNC=1`, as finding 134's checks):
1. Shoulder chord window: `DHD_RACE_PAD` pressing L1 then R2 30, 70, 150, 250 ms apart in one air:
   one Nac Nac or two tricks? Only a window covering 150-250 ms lets the 70 ms settle shrink (touch.c
   `SETTLE_MS`).
2. △ added during a held trick: R1+R2 held, △ 100 ms later: styled Can Can?
3. The ✕ double-tap and SELECT double-press windows: ✕ press-release-press at 25, 30, 35 fields
   at 60 and 144 fps (`DHD_FPS=144`): sprint up to 30 fields (finding 049)? From the release or the
   press; does a long first hold count; do they run while paused? SELECT twice at 20/30/40 fields
   (touch.c `SELECT_FIELDS`, `DOUBLE_TAP_FIELDS`). Does the Resume row's ✕ reach the race's
   double-tap check?
4. Does the game print trick names? (If so, `touch_labels` defaults to off.)
5. ✕ held in the air, at the countdown, during an R2 crouch, a powerslide (L3) and braking (✕ + △):
   harmless? Does ✕ still pedal during R2 / L3? Do ◻ / ○ act in the air?
6. The air flag over bumps and on rails: log `rider+0x4564` each field (or watch the overlay switch);
   steady means `DHD_TOUCH_AIR_WAIT=33`.
7. The HUD move: race with `DHD_TOUCH_MOUSE=1` (the HUD moves while the overlay shows): the
   speedometer and arrows at the top left, nothing else moved; Hand Left (`DHD_TOUCH_HAND=left`):
   top right, the track bar on the right. Tune with `DHD_TOUCH_HUD_SHIFT`; if it can't look right,
   `DHD_TOUCH_HUD=game` (fallback layout).
8. R2 alone in the air (and near a rail), L1+R1+R2 and L2+R1+R2 in the air; is R3 used?
9. The signature: all four shoulders then △ 33 ms later with a full meter; all four without △ or
   without the meter.
10. The pause menu: rows and the cursor's start row (`touch_field.c pause_rows` assumes
    func_0015E238's order and row 0); wrap; is START ignored at the countdown, in a crash, after the
    finish; does the race HUD stop drawing while paused; do Restart and Quit ask (and does the
    question open on No: the overlay's Yes sends ↓ + ✕); does △ close the pause menu (Back on the
    rows sends Resume's ✕ instead).

Then the touch build itself (PC with the mouse, then the phone):
11. A race, Thumb zones: the overlay appears with the race HUD (also in the countdown) and goes
    with it; menus get taps as before (PC SETTINGS by tapping).
12. Steering: a drag in the left area steers and leans; past 12 mm the stick's centre follows; ↑ tap
    stoppie, ↓ manual.
13. PEDAL hold pedals; double tap and hold sprints (the second ✕ ~120 ms late); after a HOP tap a
    quick PEDAL double-tap does **not** sprint; ✕, BRAKE, ✕ does.
14. Flicks: a fast swipe left/right off PEDAL hits ◻ / ○ (attack power-up) while pedalling; slow or
    steep swipes don't; Attacks = Buttons shows ◻ / ○ buttons and no flicks.
15. Air: 150 ms after take-off the trick stick replaces HOP/BRAKE/SLIDE; push → and hold = Can Can
    (label CAN CAN), past the edge = styled (`*`); a roll ↖ centre ↘ = Nac Nac; ← then → through the
    middle then out with a full meter = the signature; landing with the thumb down bails as on a pad.
16. Pause (button and Android Back): the PAUSED panel; Resume resumes and a PEDAL held through the
    pause doesn't sprint; Restart race → the question → Yes restarts; Quit race → No returns; Controls
    shows the page; Bail out bails.
17. Online (`port/tools/local_server.py` room race): Pause opens Close / Bail out / Leave race over the
    live race; Leave race needs a second tap within 3 s and returns to the room (DNF); no hints.
18. Classic pad (`touch_preset = classic`): bars (middle = both), + spots, ✕ sliding onto ◻ adds ◻
    without a second ✕, L3/R3, SELECT/START (START = pause).
19. Settings: TOUCH CONTROLS pages 1-3 change dhd.ini at once; Hand Left mirrors the controls (◻ stays
    left of PEDAL) and the HUD; Size and Opacity; Edit Layout drags controls (a drop too close or into
    a flick lane snaps back), Air tab moves the trick stick's faint ring, Done saves `touch_edits`.
20. A controller: a button press hides the overlay (and the HUD goes back); a touch brings it back
    without pressing anything; unplugging it mid-race (offline) pauses.
21. Phone only: vibration ticks (press, double on a trick-stick add, strong on △); Tilt steers on the
    ground and locks the screen side, Set Tilt Centre; gesture strips: a drag starting at the screen's
    edge opens Back/home, not the stick; the hint for the trick stick holds the game until a push and
    the race goes on at normal speed after.
22. No touch, no change: without `DHD_TOUCH_MOUSE` on the PC, the race state trace equals main's
    (`port/tools/state_trace_run.py`, the shared reference build): touch_field only reads, and the
    HUD hooks return their input with the touch controls off.

## Phone test (Mi 11i)

Run on 2026-11-02 on the Mi 11i (Android 13, 2400 x 1080, 394.7 dpi, gesture navigation), the
debug APK of this branch, offline races on Mt. Midoule FR (Cosmo) and one online race against a
nettool bot through `port/tools/local_server.py` (`adb reverse tcp:8787`). Touches were sent by
the device panel's tap and long press and, for two fingers and exact timing, by root `sendevent`
and a small evdev writer on the touch panel (`fts_ts`), with a uinput gamepad for check 20 (test
tools only, not in the repo). Menus were driven with keys where the touch path was not under test.
`DHD_TOUCH_LOG=1` (2: every move as well) traces each touch in mm, the air flag and every change
of the buttons and stick sent, which is how the checks below were read.

The screen reads 154.4 x 69.6 mm; PEDAL is 23 mm wide (the editor's readout); the cutout is the
left 5.1 mm band (80 px) and nothing sits in it; the gesture insets the activity reports in
immersive mode are 0 0 0 0 at the sides and bottom (top 5.2 mm). The moved HUD needs no tuning:
the speedometer and arrows sit at the top left clear of the camera and Pause buttons, the track
bar and place stay at the left edge clear of the steering ring; Hand Left puts them top right and
the track bar right. The defaults of `DHD_TOUCH_HUD_SHIFT` stay.

| Check | Result |
|---|---|
| 11 Overlay with the race HUD, menus by tapping | pass after a fix: the overlay shows in the countdown and race, hides in menus and paused game menus. Menus took no first tap after a start or a race (the menus' mouse used it as its baseline): fixed, the touchscreen is the mouse from the start |
| 12 Steering | pass: drag steers (stick x 155-250, y 61/209 for ↑/↓), the centre follows past 12 mm |
| 13 PEDAL, sprint and its guards | pass: hold sends ✕; double tap: the second ✕ 117-135 ms late inside the window; after HOP the second ✕ waits for the window (284 ms); ✕, BRAKE, ✕ sprints. A HOP, BRAKE or SLIDE tap shorter than a field, or one inside the air layout's 150 ms wait, sent nothing: fixed |
| 14 Flicks | pass: a fast swipe left sends ◻ (c000), right ○ (6000) with ✕ held; a slow or 50° swipe sends none |
| 15 Air, trick stick | pass for the push (R1+R2, label CAN CAN), past the edge (△ added, 5a00) and the hint's hold; roll and signature only in the unit tests. The trick-stick hint stayed on screen after its hold ended without a push, covered the stick's ring, and a 450 ms hop used it up: three fixes |
| 16 Pause | pass after two fixes: Resume, Restart → Yes, Quit → No / Yes, Controls, Bail out (YOU BAILED!), Back; PEDAL held through the pause waits out the window. Restart left a held PEDAL sending nothing in the new race (race-ms times not moved back): fixed. After Quit → No the game's cursor is on RESUME, not QUIT, so the next Resume opened OPTIONS: fixed. The Controls page's longest line ran to the panel's edge: fixed |
| 17 Online | pass: the panel opens over the live race (Close / Bail out / Leave race), Leave race needs a second tap within 3 s and returns to the room as DNF GAVE UP |
| 18 Classic pad | pass: bars L2 / L1+L2 / L1 / R1 / R2, ✕ sliding onto ◻ (c000), L3, R3, SELECT, START opens the pause panel. SELECT and START labels touched their box rims: fixed |
| 19 Settings | pass after a fix: TOUCH CONTROLS 1/3-3/3 by tapping, each change in dhd.ini at once (`touch settings:` lines), Hand Left mirrors controls and HUD, Opacity, Edit Layout drag and Done saves `touch_edits`, a drop in a flick lane snaps back, the Air tab shows the trick stick. The editor's bar lay over the camera and Pause buttons: fixed (it now sits beside them) |
| 20 Controller | pass: a pad press hides the overlay and the HUD goes back, a touch brings it back sending nothing, unplugging mid-race pauses (each pad event is logged twice: SDL sends a gamepad and a joystick event; harmless) |
| 21 Phone only | pass: vibration TICK, DOUBLE_CLICK on a trick-stick add, HEAVY_CLICK on △; Tilt turns the gravity sensor on, locks the screen side and steers on the ground (blended in over 100 ms at landing); a swipe in from the left edge is the system's Back, not the stick; the hint holds the game and the race goes on at normal speed. Not checked: tilt direction by hand (the phone lay still) and Set Tilt Centre |

Frame rate, rough: one race spot (Classic, rider stuck), 60 fps setting at 2x: 55.3-55.6 fps
with the overlay, 56.1-56.6 with it hidden by a key press; GPU 3.49-3.53 ms a frame both ways;
the race is `limit=cpu:vu` there. No measurable cost.

Left open: the overlay's labels have a thin dark edge and are harder to read over bright sky;
Set Tilt Centre and the tilt direction need a hand on the phone; the trick roll (Nac Nac) and
the signature were not done on the phone.

### After the phone test: the top buttons moved left

The phone's screenshots showed Pause (41 %) over the game's own messages centred at the top ("YOU
BAILED!", "FAILED TO LAND TRICK", trick names), which start at about 36 % across on a 20:9 screen.
Thumb zones' camera and Pause are now at 22.5 % and 30 %, Classic's SELECT and START at 23 % and
32 %: both end before 36 %, right of the moved speedometer and PUNCH bar.

### Check 22: the state trace after the merge, and its one difference

On local `main` e5b2b17 the race trace (`state_trace_run.py race`, defaults, no touch) differed
from 01f8d2b's from field 1453 (the race load ends: screen 9, phase 0 to 5) to the end, boot
matched. Over all 5000 fields only `ram` (and so `combined`) differed; scratch, hw, cpu, vu0, the
GS stream, registers and VRAM, sound and the code section were equal in every row. At field 1453
the raw EE dumps differ in 2453 words, each by exactly +1, and all but ten of them are words the
trace already lists as code pointers (`state_trace_codeptrs.txt`, the data image's fixups): the
dumped `code.tsv` gives the same function identity at offset 0 for every one in both builds. The
ten others are rider slot k of `D_002AF2C0` +0x4578 (0x002B3838 + k x 0x7BD0): the same callback
(identity 8E5595E4, also at the listed 0x0029C394) that the game stores into each rider at the
race load, 0x12906 in 01f8d2b and 0x12907 in e5b2b17.

Cause: in this build a code address is an index into the wasm function table, which holds every
function whose address is taken. The touch controls added one: PC SETTINGS' Cross handler
`page_accept` (`pc_settings.c`; the page had `accept = NULL` before, and Cross on the Touch
Controls row now opens its screens). It sits at table index 74611, so every later index moved up
by one (the indices up to 74037 are unchanged, the ones from 74644 on all +1). Nothing that runs
changed: `touch_field()`, the HUD hooks and `host_service_touch_field` (which returns at once
without touch) leave the game alone. A bisecting build that turned parts of `touch_field()` off
was not needed in the end; the dumps' code identities answered it.

The fix is the trace's, as `state_trace_codeptrs.txt` asks ("add the words that a diff shows
storing a code address at run time"): the ten rider words are listed, so they are hashed by
identity like the other callbacks. Changing the game code to avoid the table entry would only hide
it until the next callback anywhere in the module.

Result (the existing exes, with the new list): 01f8d2b and e5b2b17
match row for row, race 5000 fields and boot 600. `test_runtime.py` passes (`test_touch: all
passed`). Note: a reference trace made with the old list (e.g. the lanes' shared next/0.7.7 reference)
hashes these ten words raw, so a trace with the new list differs from it at 1453 in ram and code
even for the same exe: run the reference again with the new list before comparing.

### Review fixes (2026-11-03)

The three reviews of v0.7.7..main (Muse, Opus, Sol) found these in the touch code; each is fixed
with a unit test in `test_touch.c`.

- **L3 / R3 layout edits came back as defaults.** `touch_layout_parse` read the control's name
  with `%31[a-z.]`, which stops at the digit of `l3` / `r3`, so their edits were written to
  `touch_edits` and dropped on the next read. The name now takes digits too; the round-trip test
  checks every control of both presets, L3 and R3 included.
- **A full touch queue dropped ups and cancels, leaving a press or the stick held** (all three
  reviews; Sol's repro: a down on ✕, 255 moves and the up during a stall left ✕ held after the
  lift). The queue between the SDL thread and the field is now `TouchQueue` in `touch.c`
  (`touch_queue_push`, `touch_event`; `touch_host.c` only locks around it). Moves and controller
  use may fill only the first 224 places; then a finger's moves in a row are merged into its last
  one, so the finger still ends where it was, and the last 32 places are for downs, ups, cancels,
  Back and a controller's loss. A controller held at full tilt queues one "pad used", not one per
  axis event (only a down, Back or a controller's loss changes what a second one would do). If a
  stall fills even the reserve with taps, the queue becomes one cancel of every finger (the player
  lifts and touches again) with the Backs and controller losses kept after it; `dhd.log` says
  `touch: the touch queue filled ...`. And a finger id that goes down again without its up now
  ends its old job first (`touch_down` calls `end_finger_job`): before, the stick stayed deflected.
  Tests: `test_queue` (the stuck ✕, merged moves give the same stick as every move, one pad-used,
  the cancel with Back kept, the re-down).
- **Back on the pause question could leave the overlay and the game on different menus** (Muse).
  `touch_back` answered the Restart / Quit question with No and then set the overlay to the pause
  rows even when No had not been sent (a tap or Back is ignored while the presses that opened the
  question still go, about 100 ms). The game kept the question while the overlay showed the rows,
  so the next tap sent a row's presses into the question. Back now leaves that to
  `pause_row_tapped`, which moves the overlay on only when it queues the △; a Back inside that
  window does nothing and the next one answers No. (Back on the Controls page is the overlay's own
  and still returns at once.) Test: in `test_pause`, Back during the ↓ ↓ ↓ ↓ ✕ sends no △, the
  next Back sends one.
- **Tilt steering with no sensor (or before its first reading) left no steering on the ground**
  (Muse, Opus). `tilt_step` blended the stick's left/right out on the ground whatever the sensor
  said, so with no gravity or accelerometer sensor the rider went straight. Now the host passes
  whether a reading has come since tilt went on (`touch_tilt(t, known, degrees)`; `touch_host.c`'s
  `tilt_known`), and until then the stick steers. `DhdActivity.touchTiltOn` returns whether the
  phone has a tilt sensor (and does nothing without one; `android_touch_tilt_on` asks it on the SDL
  thread's first pass); the host puts that in `DHD_TOUCH_TILT_SENSOR` (a desktop: 0) and logs once
  `touch: tilt steering is on, but this device has no gravity or accelerometer sensor` when tilt is
  wanted without one. TOUCH CONTROLS' Steer With then stays on Stick (Tilt is refused, an old
  `touch_steer = tilt` can still be changed back) and its hint reads "No tilt sensor on this
  device". Set Tilt Centre without a reading changes nothing and says so in `dhd.log`. Test:
  `test_tilt` (no reading: the stick steers; a reading takes over; gone again: the stick).
- **The status bar stayed over the game after tilt's rotation** (lead, Redmi 9, MIUI, Android 11:
  tilt on, the game started from a portrait home screen). `touchTiltOn` asks for LANDSCAPE /
  REVERSE_LANDSCAPE, the screen turns, and MIUI brings the status bar back; SDL hides the bars only
  when the game goes full screen. `DhdActivity` now hides them again (WindowInsetsController,
  systemBars, shown for a moment by a swipe, as `SetupActivity.hideBars`) after a configuration
  change and when the window gets its focus back, now and once more after the new layout, only while
  SDL's full screen is on (`mFullscreenModeActive`); both overrides call SDLActivity's first.
- **The legend's PREV / NEXT pressed up / down, and a row's hint hid the legend** (Opus; the lead
  saw it on the phone). On our pages (`menu_page.c`, text 0xA6) the legend reads "◂ PREV ▸ NEXT
  △ BACK ✕ SELECT" with left and right arrows (a PC SETTINGS capture shows them; the game's own
  shell screens draw ▴ ▾ there, but those are not our pages), and left / right is what changes a
  value, so a tap on PREV / NEXT now presses ← / → (`page_legend_button`). On a row without a value
  every page ignores them (PC SETTINGS' `change` finds no row, TOUCH CONTROLS' buttons have no key,
  the online pages step only the name / code entry). And a row's hint ("Phones go up to 6x",
  "* Applies when the game starts again", "Thumb zones only", ...) used to be the footer, which
  replaced the legend, so on a phone that row had no SELECT or BACK to tap. A page now has
  `p->hint` beside `p->footer`: the hint is drawn on a line of its own 11 units above the legend, in
  the legend's style, and the legend stays (`p->footer` stays for pages whose footer is their own
  legend, as the online room's). A page that shows hints sets `p->hints` (PC SETTINGS, and so its
  TOUCH CONTROLS screens); its last row then ends one row step above the hint line, its title may
  go up to 0x0C instead of 0x10, and a page that still doesn't fit draws its rows closer: on x64
  PC SETTINGS' 16 rows keep their 10-unit step (the title 4 units higher), with Touch Controls
  (17 rows, `DHD_TOUCH_MOUSE=1`) the step is 9; the phone's shorter page is unchanged. Checked by
  hidden-desktop captures of PC SETTINGS (`live_game.py`, before and after).

Checks for these fixes: `test_touch` (all passed; each new test fails on the code before its fix),
`test_runtime.py` (exit 0), and the state trace (`state_trace_run.py` boot and race, the current
`state_trace_codeptrs.txt`) of this branch against a build of 60ba1c2: boot 600 and race 5000 rows
match. The Android-only code was compiled (NDK clang for `android_touch.c` and `touch_host.c`'s
Android half, javac for `DhdActivity.java`), not run: the tilt sensor's absence, the status bar on
MIUI and the legend taps need the phones.

### Second review round (2026-11-03)

- **A tap the game never read was lost for PEDAL, Classic's ✕ and the camera** (Sol 2). The host
  hands the rules every queued event before the field's pad read, so a tap whose down and up
  both came since the last read (a quick tap, or any tap during a stall of the game thread) ended
  before any read saw it. The first round's fallback pulsed such a tap for HOP, BRAKE, SLIDE and
  the like, but left out ✕ and SELECT (their double-press windows) and PEDAL, so those taps
  vanished, and a lost first PEDAL tap also lost the sprint of the double tap after it. Now any
  button press (PEDAL or a button) the game hasn't read when its finger lifts stays, its finger
  kept as *lifted* (no longer found by its id, so the same finger id can press again) until the
  press has been in PRESS_FIELDS pad reads, then lets go in a field of its own: `cross_track`
  sees that release, so the ✕ and SELECT windows count what the game saw. A newer press of the
  same buttons waits for that release (`after_lift`) and then starts by its own rule (`press_rule`,
  the rule a new touch gets): two PEDAL or ✕ taps in one stall are two presses with a one-field
  release between them, inside the double-tap window, so a sprint, as a double tap without the
  stall; two camera taps in one stall are two SELECTs, the second after the bail window (the
  existing camera rule). A held-back second PEDAL tap (Thumb zones, flicks on) that lifts before
  its 120 ms without flicking now goes too (it was dropped: a quick second tap didn't sprint); one
  that flicked still sends no ✕. A guarded press that lifts goes when its window closes, as a held
  one would have. The pulses (flick attacks, the air wait's HOP tap) now count their fields from
  the first pad read that has them, not from the last field's VBLANK: after a stall of 3 fields
  or more a pulse made from its events was cleared before any read, and without a stall it lasted
  2 reads, not PRESS_FIELDS' 3. `test_unread_taps` (both presets): PEDAL / ✕ and camera quick
  taps, a tap queued in a 100 ms stall, a double tap in one stall (same finger id: two ✕ presses,
  a release between, under 30 fields apart), two camera taps in one stall (the second ≥ 30
  fields later), a first tap lost to a stall then a held second tap (Thumb zones holds it back:
  the window opened), the held-back second tap lifted early (goes) and flicked (no ✕).
  `test_quick_tap` and `test_air_wait_tap` now count three pad reads. Phone check for the lead:
  a quick PEDAL double tap (both taps short) sprints; check 13's "quick PEDAL double-tap after
  HOP does not sprint" still holds (the second tap waits for the window, then goes once).
- **A controller answering the pause question left the overlay on it** (Sol 1). `pause_step`
  noticed a race the controller resumed only from the rows (`PAUSE_OPEN`, `PAUSE_CONTROLS`), not
  from the question (`PAUSE_CONFIRM`), and any pause state kept the overlay drawn and owning the
  touches, so after a controller's Yes the restarted race's touches went to the question's
  buttons; after its No the overlay still showed the question while the game showed its menu.
  Now a controller used while the overlay's rows, Controls page or question are up takes the
  game's menu over (`PAUSE_PAD`): the overlay's own presses stop, it hides and takes no touch as a
  row (it can't know where the controller left the game's menu), Back there is the menu's △; and
  the race clock running again ends every pause state (Yes: when the restarted race runs; No:
  when the controller resumes). The rows come back with the next pause. `test_pause_controller`:
  Restart race's question answered Yes and No by a controller (hidden at once, a tap where Yes was
  sends nothing, Back is △, PEDAL works again after the race goes on), and a controller used
  while the rows' ↓ presses still go (they stop). It fails on the code before.
- **Stick = Fixed steered only after the thumb moved** (Sol 3). Both presets gave a new steering
  touch the stick and its centre and returned; the deflection was computed only by a move, so a
  thumb that landed 8 mm right of a fixed stick's centre and stayed still sent a centred stick until
  the touchscreen reported a move. `stick_begins` (both presets) now computes it on the touch-down
  (`stick_follow`); the floating stick still centres under the thumb, so it starts neutral.
  `test_fixed_stick`: both presets, fixed and floating, an off-centre touch held still for 10
  fields (fixed: 212 at once; floating: 128).
- **A finger's first move could still be lost to a full queue** (Muse F1). Past the moves' part
  of the queue a move only rewrote its finger's last queued move; a finger that pressed during the
  stall had none (its down was its latest event), so its first move was dropped, and a thumb then
  held still on the stick never steered until it moved again. Now such a move stays: a queued move
  whose finger lifts or presses again later in the queue gives its place (`queue_drop_old_move`:
  that up or down says where the finger went), else it takes a reserve place; the finger's next
  moves rewrite it, so a finger holds at most one reserve place. A queue full of presses alone
  still drops it (the finger's up says where it ended). `test_queue`: Muse's repro (127 taps of
  another finger, then a stick down and its first move: kept, the stick past 200), and the
  completely full queue (another finger's stroke gives its move's place, its up stays).
- **A failed tilt-sensor ask was retried every SDL pass** (Muse N2). When
  `android_touch_tilt_on` answered -1 (the activity not ready, or the Java call failing), the
  next pass (about every 4 ms) asked again, each a JNI call and a posted UI-thread task. A failed
  ask now waits 250 ms before the next; a successful one is not delayed. Android only: compiled by
  the debug APK build, not run.

Checks for the second round: `test_touch` (all passed; each new test fails on the code before its
fix), `test_fps.py` at every rate, `test_runtime.py` (exit 0), and the state trace (boot and race,
the current `state_trace_codeptrs.txt`) of this branch against a build of b25cbb2: boot 600 and
race 5000 rows match. The debug APK builds (`build_apk.py --wasm`: libmain.so with the Android
`touch_host.c`, then Gradle, BUILD SUCCESSFUL). Not run on a phone: the controller hand-over,
quick taps and tilt need the lead's phone checks.
