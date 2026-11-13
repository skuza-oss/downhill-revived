# 229: Touch buttons that show what they do

Status: in progress

Branch `feat/touch-action-icons` from `main` 6e47e82a, started 2026-11-13. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The owner's decision (2026-11-13): the shared in-game touch controls (`port/hle/touch*.c`, Android
and iOS) get a second look in which each button shows what it does (a pedal on ✕, fists on ◻ and
○, a brake on △, trick marks on the shoulders) with a small badge of the pad's own name, as a
community iOS fork showed players in the project's Discord. Our own drawings; the idea is credited
to the fork (`bagasromadon/downhill-revived-ios-newbutton`). A setting, Button Style, chooses
Actions or Symbols (today's look). Actions is the default only if it reads at a phone's default
button size; otherwise Symbols stays the default.

Done when: the action table (what each button does on the ground and in the air) is written from
the game; the pictures switch with the rider's ground/air state without flicker and never under a
held finger (unit tests); the setting exists in the game's TOUCH CONTROLS page and the launcher
(eight languages, `tools/lang_check.py` clean); captures of both styles at five screen sizes judged
at real physical size; `check.py port` and `check.py docs` pass; boot and race state traces equal
the reference; the phone and the iOS Simulator build checked.

Out of scope: new gameplay, a per-slot action swap (finding 226), portrait layouts.

## Progress

- [ ] 2026-11-13: finding reserved

## Decisions

**A moved button never takes steering away (the review of 2026-11-13, fixed in this lane at the
lead's request).** An outside review of main found that lane 226's free placement let Classic's L
bar be dragged to the screen's bottom, and the steering area was "below the L bar" measured from
the bar's edited place: its top fell below the screen, no finger could start steering, and the
saved layout kept it so after a restart. `dpad_room` read the edited bar too, and its arm size
could go negative. Thumb zones had the same dependence: its area ended short of the right
cluster's edited controls (a HOP dragged left would have narrowed it to nothing) and, for a 0.8.0
layout, started under the edited camera button. Now:
- every control keeps its place and size before the player's edits (`Ctl.x0, y0, r0, hh0`), and
  the steering area, the D-pad's room and the D-pad's default home are measured from those, both
  presets and both hands. A button dragged into the area takes the touches on itself, as before
  (`route_classic` and `route_thumb` try the buttons before `steer_begins`);
- the D-pad's arms have a floor (`DPAD_HALF_MIN_MM`, 2 mm half size), so no screen makes them zero
  or negative;
- a guard for the whole class, `steering_ok`: with the stick, some 10 mm square of the steering
  area that no other control's touch area covers; with the D-pad, its rest point and four arms on
  the screen, out of the gesture strips and under no other control. The editor refuses an edit
  (`control_fits`, a drag stops at its last good place, + stops growing) when that control is what
  takes steering away; a layout that already failed for another reason (a tiny screen) never locks
  the editor;
- a loaded layout that fails the guard (written before the fix, or dhd.ini edited by hand) is not
  thrown away: its edited controls go back to their default places one at a time, in the controls'
  order, until steering is usable, for that run only (dhd.ini keeps the edits until the editor's
  Done saves), and `dhd.log` says so once (`touch_take_note`: "touch: the saved layout left no
  room to steer; at their default places for now: L3"). If even the defaults leave no room,
  nothing changes.

Tests (`test_touch.c`): `test_moved_bar_steers` (Classic, the Mi 11i and a 13-inch tablet, stick
and D-pad, either hand: the L bar dragged across and down to the bottom in the editor and saved,
then a finger in the lower left steers, or the D-pad's right arm presses right, and the pad keeps
arms of a usable size; the D-pad's floor on a 30 mm high screen); `test_saved_layout_steers` (a
saved string with the bar at the bottom loads and steers, either hand, with no note; L3 grown to
600 % over the steering area by a hand-edited string loads with L3 at its place, one note, and
steers, configured again no second note; in the editor L3 grown there stops while a free spot
remains; the D-pad's home dragged onto L3 stops short and the pad still presses). With the area's
top put back on the edited bar, 8 checks fail; every earlier test passes unchanged
(`check_layout_clear` is untouched: the new checks are separate).

## Surprises

## Result
