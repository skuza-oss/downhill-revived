# 271: the online room redesigned, and a results screen at the race end

Status: current

Branch `feat/room-redesign-271` from `main` 8d956bff, started 2026-11-17. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The race room (`port/hle/online_menu.c`, finding 115) laid out as the four pictures the owner
approved (lane 270's mock-ups v3: room before a race, race live, results screen, room after a
race): one layout for every phase, a header of one small and one large line with the countdown
bar, the track and the room's facts on the left with the room code under them, one list of ten
rows (one member a row, nothing shrinks, names whole up to 12 letters, longer ones cut with ".."),
a button bar (CHANGE RIDER, one large main button, an optional third, LEAVE apart) and one
sentence under it that says what happens next. When a room race closes, a results screen over the
darkened last race picture (8 s, Cross, a click or a tap skips) instead of the RACE OVER card.

Done when: hidden captures of every phase sit beside the mock-ups (`compare-*.png` in the lane's
scratch folder) with every difference listed; no text runs out of its box (the room's layout
check in the log says so, widest names); keys, mouse and touch work as before; a three-game room
race gives the base's results; `check.py quick`, `port`, `docs` pass; boot and race traces equal
the base. Out of scope: the watching strip in the race, the HUD's place on a finisher, the screens
before the room.

## Progress

- [x] 2026-11-17: plan (this file)
- [x] the rows, names, times and results as data: `port/hle/room_view.h`, unit test
      `port/hle/test_room_view.c` (8e17cee3, f088118c)
- [x] the room page (8e17cee3), the results screen (f088118c), WATCH THE RACE from lane 269 as
      the main button (6684e874)
- [x] layout from the first captures, menu rectangles over a race view (0ba9d261, ac2c5d64)
- [x] main merged twice (269, 268); `room_watch_check.py` takes the watcher's window and scale
      (99d6c5d3)
- [x] proof: captures at 8x beside the mock-ups, function tests, three-game race, checks, traces
      (below); from them: no rider name in a member's row while they choose (the chip says
      CHOOSING, the name ran out of its column), the main button's check mark as wide as tall

## Where today's facts and actions went

The room page before this lane (8d956bff) and where each piece is now:

| Before | Now |
|---|---|
| title RACE ROOM / RACE LIVE / NEXT RACE IN | the header's large line: "N OF M READY", "RACE STARTS IN n", "RACE LIVE m:ss", "RACE ENDS IN m:ss", "NEXT RACE IN m:ss" |
| the countdown cell, START cell's states | the header's bar (green: ready share or the 5 s start; lilac: the 30 s to the next race; blue: the finish window) and the small line above the title |
| notes stacked at the top ("X LEFT", "X IS READY") | one note at a time, the newest, in the small line (they no longer pile up over the header) |
| players count, the room's area or region | the list's head: "PLAYERS n/m" and the area at its right |
| each member's card: name, HOST, rider, ready, ping number, finish place and time | one row each: place or slot number, the name (12 letters, ".." past them, other clients' characters drawn as "-"), HOST / YOU badges, the middle column (rider before a race, last time or DNF after one, empty while live), the state chip (READY, WAIT, CHOOSING..., RACING, the time, OUT - reason, NOT IN THIS RACE, READY NEXT), ping as three bars (the number too from 150 ms) |
| "RACE RESULTS - LIVE" / "RESULTS - LIVE" | the list's head while racing: "LIVE RESULTS" and "N STILL RACING"; after: "LAST RACE - TRACK" and "N READY" |
| track name and picture, TRACK row | the left panel: "NEXT RACE" / "THIS RACE", the picture (finding 132, middle lines when the box is wider), the name in capitals; the host's arrows and Cross for the track list as before |
| ROOM RULES row (finish window, next race, then) | three panel rows, FINISH WINDOW, NEXT RACE (AUTO, 30 S / HOST STARTS), THEN; Cross opens the rules page as before; a change during a race marks them " *" |
| COMPUTER RIDERS, CHEATS, PUBLIC rows | panel rows COMPUTER RIDERS, CHEATS, ROOM (P2P, PRIVATE / PUBLIC / PRIVATE); the host changes them with left/right, a click or the wheel as before |
| the code and COPY | the code panel: "ROOM CODE - INVITE FRIENDS" (shortened to fit), the code large, "◯ COPY CODE", COPIED / COULDN'T COPY |
| CHANGE RIDER / CHOOSE RIDER | the bar's first button |
| READY | the main button: yellow READY, green with a check mark once pressed; WATCH THE RACE while a race this member is not in runs in an SFU room (finding 269), READY NEXT the third button then |
| START NOW (host) with its reason | the third button for the host: START NOW with RACE LIVE / STARTING / NOBODY READY / N NOT READY under it |
| LEAVE and its question | LEAVE at the right, the same STAY / LEAVE question |
| the P2P address notice | the sentence, in amber for 15 s after the room opens |
| hints ("PRESS READY TO RACE THE NEXT ONE", "WAITING FOR THE HOST...") | one sentence under the bar for the phase |
| the legend (Cross, Square, Circle, Triangle) | unchanged |
| the RACE OVER card at a room race's close (2 s) | the results screen (8 s) |

## Decisions

- **The layout's numbers live in one enum** (`online_menu.c`, the 640 x 224 popup view mapped to
  the 4:3 page): the panel 147 wide at x 45, the list 390 at x 205, rows 12 apart and 11 high, the
  bar at y 171. Text is centred by the font's own offset: a glyph's middle sits about 5.4 units
  (at 100 %) under the y it is drawn at.
- **Every text in a box of its own is measured** (`layout_check`): one wider than its box logs
  "online menu: layout: ..." once. The capture runs with the widest names and the function test
  must log none. Widths come from the game's font data in the core, so there is no GPU-free unit
  test of them; `test_room_view.c` tests the texts (names cut to 12, times, rows, gaps).
- **Results screen, the race-end path.** state_sync's close (`race_end_reason`, finding 269's
  close for a watcher) opens it for RESULTS_SECONDS = 8 instead of the 2 s card when the race
  ended normally or by the host (not a lost link or a removal, not the arena: its COMBAT RESULTS
  stay). It draws in the race screen's own HUD pass over the race, darkened; the HUD and the
  watcher's label hide meanwhile. Cross or a click leaves at once; Square, Start or a tap on its
  words is READY for the next race.
- **No server change.** The server's intermission stays 30 s from the close (`room.ts` closeRace,
  AUTO rooms). The results screen takes about 6 s more of it than the card did, so the room page
  opens with about 18 s left instead of 24. The time to get ready is not shorter: READY works on
  the results screen, and when a countdown starts while it shows and this player is ready, the
  race screen leaves at once.
- **One finish time format, M:SS.CC from ticks** (room_view.h `room_time`), in the rows, the
  notes, the results and the log. The game's own CONGRATULATIONS line keeps its H:MM:SS
  (sixtieths): 0:30:04 there is 0:30.06 here.
- **Menu rectangles set ALPHA_1** before a blended one, as the game's boxes do (func_00190E48):
  over a race view the GS was left in another mode and the darkening drew nothing.
- **The browser** offers no online play: `net_layout.js` is only `net.h`'s byte layout, unchanged;
  the page is drawn by the core, so a browser build that offered rooms would draw this page.

## Surprises

- A blended rectangle is two qwords longer; with 8 qwords a slot the DMA's spare qword past the
  packet (its tag counts itself) became the next packet's header: fonts drew as blobs and the
  room vanished. Slots are 10 qwords now.
- `fit()` and `first_fit()` take sizes from 100 %: three callers passed a style already at 55-90 %
  and got text twice as small.
- A cheats change unreadies everyone (the server's rule, finding 128): the function test saw it.
- The font's glyphs are white at alpha 0x40 in its texture, and the GS multiplies both by the
  style's colour and alpha (0x80 = 1.0): at the game's alpha every glyph covers half. On the dark
  room a 0x76 white name came out 118 of 255 and READY on the yellow button half yellow (olive).
  The room and the results screen draw solid (`menu_solid`, alpha 0xFF): names 233, the secondary
  texts 136,142,190 (the mock-up's #8a8fc0), READY 13,13,15 on yellow (the follow-up round after
  the merge). The large title and the button legend are drawn by the game's page and keep its
  look.
- The host's panel rows (COMPUTER RIDERS, CHEATS, ROOM) are 8 units tall as tap targets, the base's
  were 13: the rows are 8 apart, so taller targets would overlap; they cover each row's full pitch.
- The room's places come from the server and count computer riders: a player behind three of
  them is 4TH in the room, as on the results screen.
- A member who joins during the 5 s start can see one frame of "RACE STARTS IN 1" with "YOU'RE IN
  THIS RACE" before the room's next update says they are not; it is right again within 2 s.

## Result

The room and the results screen look like the four mock-ups in the game's own font. Captures:
hidden-desktop games at resolution scale 8 ("8x asked, this GPU allows up to 8x", an RTX 3070),
windows of 3840 x 2160 (16:9), 2880 x 2160 (4:3), 7680 x 2160 (32:9, the widest) and 4800 x 2160
(20:9, a phone's shape), the presented picture saved whole as PNG (`DHD_CAPTURE_PRESENTED`); each
`compare-*.png` puts the mock-up, scaled up to the picture's height, beside it. The pictures and
pairs stay in the lane's scratch folder (game pictures are not committed).

What differs from the mock-ups, on purpose or from the font:

- The game's font is wider than the mock-up's, so the left panel's code label reads "ROOM CODE -
  INVITE" (the full label does not fit; `first_fit` picks it) and long names are cut at 12
  letters with ".." where the mock-up showed them whole (XX-DARKRID.., ABCDEFGHIJ..: the test
  names are 15 and 24 letters; "_" draws as "-").
- The panel has a sixth row, ROOM (P2P / PRIVATE / PUBLIC), which the mock-ups leave out.
- The button legend (Cross, Square, Circle, Triangle) stays under the sentence.
- In a P2P room the sentence is the network-address notice for 15 s (amber), and WATCH THE RACE is
  not offered there (finding 269's rule): the race-live picture of a P2P room shows READY. The
  WATCH THE RACE picture comes from an SFU room (`room_watch_check.py`).
- The results screen's footer holds three things (BACK TO THE ROOM IN n, Square READY NEXT, Cross
  CONTINUE); the mock-up has two. The finisher's own row is a little larger, as in the mock-up.
- The header's bar runs over the page's full width above the panel and the list, as the mock-up.

Function tests (hidden games, local server): keys, mouse clicks and the wheel on every row and
button, START NOW, READY and CONTINUE on the results screen, the leave question, the host's
changes; READY by a tap (`room_ready_touch_check.py --race`); no "layout:" line with the widest
names. The three-game room race (`room_watch_check.py`, watch and none): every game logs the same
room results, the watcher is in none, no refused, freeze: or crash: lines. `check.py quick`,
`port` and `docs` pass; the boot (600 rows) and race (5000 rows) state traces equal main's
(524948ba), so no game state changed.

The arena's scored round keeps its COMBAT RESULTS; after it the room's head reads "LAST RACE -
MOSH PIT (ARENA)". The points in the rows ("1000 PTS", the gap "-90 PTS") are tested in
`test_room_view.c`; the capture's computer riders left after the round, so no picture shows them.

Proven where: the PC (Windows, these runs). Owed: a phone (Android, iOS) and the Mac look at the
room and the results screen; the layout is the same code, the phone shape held at 20:9 here.

## 2026-11-17, lane 272: the main button's words and check mark

The owner's two notes on the room's main button: READY on yellow read as one dark blob, and the
pressed state's check mark was blocks that did not suit the font.

- **The blob was the font's own edge.** Each glyph in the font's texture is a white body inside a
  dark (black) edge; the GS multiplies the texture by the style's colour, so the edge stays black
  whatever the colour. Round white words on a dark page that is the game's look; round black words
  on yellow it doubled every stroke, and the letters' edges met. Not a second pass and not the
  style: the selected and the row style are the same size (glyph 288 x 160, spacing -100, -32 at
  100 %). The main button now draws its words in ink (`menu_draw_text_ink`, `menu_page.c`):
  READY and WATCH THE RACE with ALPHA_1 (0 - Cs) * As + Cd, so the white body takes the yellow
  away (black words) and the black edge takes nothing; the pressed READY with Cs * As + Cd, white
  on green, no edge either. ALPHA_1 goes into the font's own batch (an A+D packet between two runs
  of glyphs) and is put back to the batch's 0x44, FIX 0x80 after the words. Trap: a run's glyph
  count (`D_00513AE0`) survives a colour change in state 1, so the inserted packet must zero it, or
  the next run's NLOOP counts the old glyphs too and the GS reads garbage (the first try drew the
  room as giant glyph-shaped holes and left the rider screen's foliage blended wrong).
- **The check mark is two slanted strokes** (`check_mark`, `online_menu.c`): each a strip of two
  triangles through the rectangle packets (`menu_draw_strip`, XYZ2 to 1/16 pixel, the widescreen
  map applied linearly between whole pixels), 4.2 units wide across their direction like the
  letters at 125 %, pure white as the words, 8 units tall, 6 before the words; the check and the
  words are centred together.
- The plain buttons (CHANGE RIDER, START NOW, READY NEXT, LEAVE) had the same smear only with the
  cursor on them (the selected style's olive words on the cursor's yellow bar): their words are
  dark ink there now, light words on the dark box as before. The results screen (light words on
  dark, where the edge does not smear) is unchanged; so are the host's panel rows and the track
  and rules lists under the cursor, which keep the game's selected look (not this lane's buttons).
- The main button's words sit on its middle at the size `fit` chose (WATCH THE RACE is smaller
  than READY and was drawn high).

Proof: hidden captures at 8x, 16:9 and 4:3, and at 1x (READY not pressed with and without the
cursor, pressed, WATCH THE RACE in an SFU room, CHANGE RIDER and LEAVE under the cursor),
`before-*` from this finding's pictures beside `after-*` in the lane's scratch folder; no
"layout:" line. At 1x the check is as stepped as the letters (the 640 x 224 view), still a check.
Drawing only: no game state changes, no state trace run.
