# 209: Touch controls can skip the flight over the track

Status: in progress

Branch `fix/touch-skip-flyover` from `main` ad0135bd (0.7.16 plus findings 199-206), started
2026-11-12. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

GitHub issue #89 (Android): the camera's flight over the track before every race cannot be skipped
with the touch controls. On a PC START skips it; on a phone nothing is shown at that moment and a
tap does nothing.

Acceptance: with the touch controls on at a phone's shape (20:9) the flight is skipped by touch in
both presets (Classic pad, Thumb zones), shown in picture captures at 4x; `test_touch` covers the
new rules; the Android target compiles; `python tools/check.py port` and `check.py docs` pass;
`docs/PLAYING.md` says what the touch controls show. Nothing changes with a controller or a
keyboard, nor offline without touch (state traces: left for the lead). Out of scope: the game's and
the room's rules about when the flight may be skipped; a phone run (the lead's).

## Progress

- [x] 2026-11-12: finding reserved (17f844f6)
- [x] cause found in the decomp and the touch code (below)
- [x] fix and unit tests: SKIP button, `test_flyover` in `port/hle/test_touch.c`
- [x] Windows captures, both presets (Decisions, "What was run")
- [ ] checks: `check.py port`, `check.py docs`, the debug APK (Result)

## Cause

The flight is race phase 5 (`D_0043F348`, the race start object's +0x488: 0 loading, 5 the flight,
1 the countdown, 2-4 the race). The race loop (`func_001B40A0`) runs `func_001B4900` every second
frame: for each pad it tests the pad button 8 (`func_00220618(8, ...)`, START) on its press edge,
and a press ends the camera's flight (the same button 8 the online code's `test_skip_pad` and the
harness' `9/5:start` press; finding 047). So START is the one input: Cross and the other buttons do
nothing there.

The touch controls showed nothing in the flight and sent nothing:

- the overlay shows only where the race HUD is drawn (`touch_hud_seen`, in a race from phase 1,
  `port_race_running(1)`: finding 141, 159); the flight draws no HUD and phase 5 is not "running";
- so the overlay is hidden and `touch_owns_touches` is false: a tap goes to the menus' mouse
  layer (finding 092), which does nothing while a race plays, and Classic's START / Thumb zones'
  Pause button are not on screen.

## Decisions

- **A SKIP button, shown only during the flight, sending START.** The smallest change that makes
  the existing input reachable and discoverable: a button labelled SKIP in the overlay's box style,
  low in the middle of the screen (under either thumb, clear of the game's track name at the top
  and of the race controls that come next), one tap = START as on a pad. The same in both presets,
  both steering choices and both hands: nothing of the race layout shows in the flight, and the
  button does not depend on them.
- **Why not "tap anywhere".** A finger already down when the flight begins (the load ends under
  the thumb that tapped the menus) would skip it at once, and a tap meant for nothing would skip a
  flight the player wanted to watch. A button needs a deliberate touch and says what it does.
  No hint box: the label is the hint.
- **Fires on the lift, on the button it started on** (as the pause rows' taps); a finger that was
  already down when the flight began is never SKIP's (`JOB_IGNORED`), a drag off the button
  cancels it.
- **START is a 3-field press (`PRESS_FIELDS`) as every other overlay press, and it is taken back
  when the flight ends.** The game reads START every second frame, so three fields always hold an
  edge. If the flight ends while the press still goes (it does, in the next field: the game moves to
  the countdown at once) the press is cleared in that update, and a finger still on SKIP is
  ignored from then on: the countdown never sees a START (it would pause the race), and a thumb
  held on the button when the race starts does nothing. Shown in the run: `sent 0008` at the lift,
  `flyover 0` 1-32 ms later, `sent 0000`; nothing after.
- **What the game reads** (`touch_field.c`): `HostTouch.flyover` (the field that was `reserved`,
  the struct stays 64 bytes) = a race's loop runs (`port_in_race()`, finding 159: not the menus
  where a phase 5 may be left over, not the attract demo) and the phase is 5. Online too: the room
  decides when a flight may be skipped; the touch only presses the same START (online.c's
  `test_skip_pad` does the same, and its `hle_online_countdown_hold` still strips a START the room
  holds back, in the countdown).
- **Nothing else changes in the flight.** While it plays the race layout is not drawn, the race
  touches (PEDAL, stick, tilt, hints, the pause panel) do nothing, as before; Android Back is
  still the menus' Back (△), unchanged. A pause panel left open by a restart is closed when the
  flight starts. With a controller or a keyboard in use the overlay hides as in a race (setting
  "With a controller"), the first touch brings it back and is not sent; the button's code runs
  only when the overlay shows, and nothing is sent without touches, so a PC without touch is
  untouched.
- **Test hook** `DHD_TOUCH_WITH_SCRIPT=1` (`online.c` `pads_read_now`, docs/TESTING.md): the
  device's buttons (touches, and a keyboard or pad if used) are or-ed into player 1's pad also when
  `DHD_NAV` drives it, so a `DHD_NAV` route can end at the flight and touches take it from there
  (finding 189 had to drive the menus by posted keys, which cannot find the shop's moment). Only
  with the variable.

## Other moments where START or Cross does something (checked in the code, not on a phone)

| Moment | Game input | Touch now | Verdict |
|---|---|---|---|
| Flight over the track (phase 5) | START | nothing; now SKIP | fixed here |
| Rider's intro / countdown (phase 1) | START (finding 141 check 10 left open whether the countdown takes it) | overlay shows (Classic START, Thumb zones Pause), as before | no gap seen; the capture shows the controls at the countdown |
| Race, finish | START pauses | overlay + Pause, as before | no gap seen; the finish screens were not run |
| Results, shop, other menus | Cross / Triangle / START rows | the menus' mouse layer (finding 092: rows, legends, a click on the title is Cross) | not changed; results rows were not tried live |
| Replays | not read | the overlay shows if the replay draws the race HUD in a race loop (`port_in_race`) | not run, not read; left for the lead |
| Startup movies and attract demo | the movie's skip is `func_00190D60(pad, 8 or 4, 1)` (`func_001B53C0`, `001EB1D8`: START among them) | menus' mouse layer only: a click is Cross on screen 0 | possible gap on a phone, not run; left for the lead |
| Online flight | START, only the room's own hold in the countdown | same SKIP | same input as a pad |

## What was run

Windows, hidden desktop (`port/tools/hidden_game.py` driven by a script kept outside the
repository), `DHD_TOUCH_MOUSE=1 DHD_TOUCH_SCREEN_MM=154.5x69.5 DHD_TOUCH_WITH_PAD=show`, a
1545 x 695 window (20:9, `DHD_WIDESCREEN=1`), `DHD_SCALE=4`, `DHD_NAV` to the flight (Single
Event, Race, the shop's START and Cross), no `9/5` step, so only the touch can skip it; the tap
posted as one move, down and up. Mt. Midoule, Italy FR, Freeride.

| Preset | Result (touch log) |
|---|---|
| Thumb zones | `flyover 1`, the picture shows SKIP only; tap: `sent 0008`, `flyover 0` 32 ms later, `sent 0000`; the frame after is the countdown with Thumb zones' controls |
| Classic pad | the same: `sent 0008` at the lift, `flyover 0` 1 ms later; the countdown shows Classic's pad |

Captures (kept outside the repository): the flight with the button and the first frame after the
skip, per preset (`thumb-1/2`, `classic-1/2`). Unit tests: `test_flyover` (both presets: nothing
shown outside the flight; a finger already down is not SKIP's; taps on PEDAL / Cross / Pause do
nothing; the tap sends START for three fields, once; a drag off cancels; the flight ending right
after the tap takes START back; a finger on SKIP when the flight ends does nothing; a controller
hides the overlay, the touch that brings it back is not sent).

## Surprises

- The `host_pad_read` in `autopad.c` is not called by anything: the game's pads go through
  `pads_read_now` (`online.c`) or `pad_refresh` (`pad.c`), which is where a script or the device is
  chosen. The first version of the test hook sat in `host_pad_read` and did nothing.
- Posted mouse clicks: the move, down and up must be posted back to back; with a pause between
  them the up never arrived (the game's own touch log shows it), as TESTING.md's trap says.
- The flight ran 38 s of wall time in these 4x runs on a busy PC (it is a fixed number of game
  fields, so a phone at 60 fields a second sees it shorter); the button shows for all of it.

## Left for the lead

1. A phone (Android, both presets): a race; the flight shows SKIP low in the middle; a tap skips
   to the countdown; a tap elsewhere does nothing; the race controls then show; no pause.
2. Online (P2P room, phones or PC with `DHD_TOUCH_MOUSE=1`): the same, the room's countdown
   still starts for everyone (finding 047).
3. Startup movies and the attract demo by tap on a phone (table above).
4. State traces: the seven recipes (`boot`, `race`, `race2`..`race6`) against format-2
   references (docs/TESTING.md). The change cannot move them: `touch_field()` only reads the phase
   and `port_in_race()`, and returns its result into a host struct; without touch
   (`touch_on()` is 0, no `DHD_TOUCH_MOUSE`) `host_service_touch_field` returns at once and the pad
   merge sends nothing; `DHD_TOUCH_WITH_SCRIPT` is read only in `pads_read_now`'s script branch. A
   code address was added in the table? No: no function whose address is taken was added (finding
   141's check 22 cause), so the code-pointer words do not move.

## Result
