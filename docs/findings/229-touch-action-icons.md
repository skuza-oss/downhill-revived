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

- [x] 2026-11-13: finding reserved (4a839835)
- [x] the review's steering fix with its tests (cbca82ba)
- [x] the pictures, their generator and the shader's TS_ICON (39c57b82)
- [x] Button Style in the overlay, the ground/air pictures, the TOUCH CONTROLS row, tests (f5f74413)
- [x] the launcher's row in eight languages (3d9b47c7)
- [x] main b4ec64d7 (lane 227) merged in (13987911): only the generated findings index collided
- [x] readability rounds at real size: a bolder fist, larger badges and pictures (252b0a43); badges
  that follow Button Icons, a size floor for pictures (f9840650)
- [ ] captures and sheets, `check.py port`, state traces, the phone, the Simulator build: Result

## What each button does (the action table)

From the game's own CONTROLS page (its strings in the executable: "R2 button: bunny hop (hold to
charge)", "tricks (add △ for superstyle)", "brake/ superstyle", "l. combat", "r. combat", "pedal/
(2x for sprint)", "steering (push in for powerslide)", "sprint (push in)", "bail out", "camera w/
up,down or L2 button"), the trick names the game prints (its table at `D_0026ED40`), finding 141's
tables and two runs of our own (below).

| Pad | Thumb zones | On the ground | In the air | Picture (ground / air) |
|---|---|---|---|---|
| ✕ | PEDAL | pedal; press twice and hold: sprint | nothing useful (PEDAL dims) | pedal on its chainring / same |
| ◻ | flick left off PEDAL, or a button | attack the rider on the left (punch, kick ... as the combat power-up gives) | (not checked) | fist facing left |
| ○ | flick right, or a button | attack the rider on the right | (not checked) | fist facing right |
| △ | BRAKE | brake | added to held trick buttons: superstyle | brake lever / star |
| R2 | HOP | hold to crouch, let go to hop (a bunny hop, charged by the hold) | alone: no trick (held through a whole jump, nothing happens); with R1: Can Can, with L2: Bar Kneel, with L1: Nac Nac | bike hopping / two links |
| L1 | trick stick ↖ | (nothing alone) | One Hander; with R1 Superman, with L2 Table Top | open hand |
| R1 | trick stick ↗ | (nothing alone) | One Legger; with R2 Can Can | boot |
| L2 | trick stick ↙ | with SELECT: look back | Saran Wrap; with R2 Bar Kneel, with R1 Candy Bar | a loop |
| L3 | SLIDE | powerslide (with the stick) | — | wheel sliding with its spray |
| R3 | (none) | sprint | — | two arrowheads |
| SELECT | camera | camera (with ↑ ↓), twice: bail out | same | camera |
| START | Pause | pause | pause | Pause |
| left stick | the stick | steer, lean ↑ ↓ (stoppie, manual) | spin and flip | a handlebar on the knob |

The signature trick is all four shoulders with a full meter. Our runs (the PC build, hidden
desktop, `DHD_RACE_PAD`): R1 held 0.4 s in a jump scored a trick ("81 PTS", then "FAILED TO LAND
TRICK" as it was still held at the landing); R2 held 0.2 s in one jump and 1.2 s through a 2 s jump
showed no trick, no points and a normal landing.

## How the pictures are made

Fourteen pictures, drawn by hand as vector shapes in `port/render/src/present/touch_icons.txt`
(strokes, discs, rings, arcs, rounded boxes, polygons, and cut-outs), original work for this
project under GPL-3.0-or-later; no picture, font glyph or game art of anyone else is in them, and
none of the fork's pictures was looked at beyond its contact sheet. `port/tools/touch_icons.py`
turns them into `touch_icons.wgsl`, one function of signed distances per picture, which the
overlay's shader (`present/touch.rs`) includes; a new shape kind `TS_ICON` (11) draws one in a
single instanced quad like every other shape, white with the dark edge the overlay's letters have.
`check.py quick` checks the generated file against its source; the tool also draws preview sheets
with the same distances in numpy.

Why distances and not an atlas: the overlay already draws everything as signed distances, so the
pictures need no texture, no build step for images and no extra binding, and they are exact at any
size from the editor's 25 % to 600 % and at any screen density (an atlas would need several sizes
or a distance field texture and its sampling). Cost: no memory beyond the shader's code (about 300
lines of WGSL); per frame about twenty more quads, each evaluating its picture's 5-15 shapes for
the pixels it covers only (a phone's button is roughly 100 x 100 pixels).

## Decisions

**The badge.** A small dark disc in the button's upper right, inside its rim (where neither thumb
covers it and no neighbour is touched: the + spots and the diamond's other buttons stay clear), at
least 2 mm in radius and at most 48 % of the button's; it shows the pad's symbol (△ ◻ ○ ✕) or name
(L3, R3, R2 on HOP, L3 on SLIDE). The shoulder bars write each part's name beside its picture.
Button Icons (finding 134) applies: with Xbox or Nintendo chosen, the badges and the bars' names use
those names by place (A, B, X, Y, LB, LT, RB, RT, LS, RS; B, A, Y, X, L, ZL, R, ZR); Auto keeps the
PS2 names, as the game's prompts do on a touch screen while no controller is in use (finding 227).
SELECT and START keep their names (finding 134's icons have none for them).

**A picture's smallest size.** At real size a picture under about 2.1 mm half size does not read:
such a control shows its pad name, as in Symbols. At the default Size every control is larger; at
the smallest Size (70 %) on a phone L3, R3 and the shoulder bars fall back to names, the face
buttons keep their pictures. The trick stick's ring keeps its names in both styles: a picture in
its band never read.

**Ground and air.** The air pictures (△'s star, R2's link) come after the air flag has held for
the air wait (Thumb zones' air layout uses the same flag and wait, 150 ms), the ground ones after
`TOUCH_GROUND_MS` (120 ms) on the ground, so a bump or a rail's edge does not flicker them; a
control under a finger keeps the picture it had until the finger lifts. The race leaving the
screen and the layout editor show the ground pictures.

**Thumb zones** keeps its words, with the picture above the word and the badge: its areas are named
by action already, the word is what reads at every size and on the small phone, and pictures alone
would make it lose that; with the pictures the two presets share one look (Symbols keeps Thumb
zones exactly as before). The flick hints beside PEDAL show the fists.

**The setting.** `touch_style` (`DHD_TOUCH_STYLE`, `actions` or `symbols`) in the settings table;
an older dhd.ini has no line and gets the default; the style changes no place, size or saved
layout (`test_button_style`). The game's row is on TOUCH CONTROLS 1/3; the launcher's Touch tab
has it with the hint in eight languages (the row's name is the game's English, as the other rows).


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
