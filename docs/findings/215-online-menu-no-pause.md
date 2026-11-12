# 215: Online, the race menu does not pause the race

Status: current

Branch `feat/online-menu-no-pause` from the online riders lane's head 86c3f4a1 (local main
dbddf61e plus finding 202's round 4), started 2026-11-12, merged into `main` the same day
(4bb35678). This file was the lane's plan while it ran and is its finding now.

## Goal and acceptance

The owner's decision (2026-11-12): in an online race with two or more humans, START opens the
room's menu over the running race and nothing stops: this game's rider coasts with no input while
the menu is open, the computer riders keep racing, nobody else sees a freeze. A player who is the
only human still racing pauses for real, as offline.

Done: a measurement of today's pause (the "before" table), the same scenarios after, every row
of the menu with two humans, the alone case, a Mosh Bowl round with the menu opened mid-round,
captures and a clip, unit tests for the pure rules, `check.py port` and `check.py docs` green.
Out of scope: the touch controls' own online panel (finding 141), which already never paused.

## Progress

- [x] 2026-11-12: finding reserved (ca211da4)
- [x] 2026-11-12: `DHD_NET_TEST_MENU`, presses on the wall clock from a race second (6328a749)
- [x] 2026-11-12: the before measurement (below)
- [x] 2026-11-12: the menu over the running race, the alone rule, `race_menu.h` and its test (95145fae)
- [x] 2026-11-12: `port/tools/race_menu_check.py` (the rows in a room from the menus), `mosh_round_check.py --host-env / --guest-env`
- [x] 2026-11-12: the after measurement, the rows, the alone case, the Mosh Bowl round, captures (below)
- [x] 2026-11-12: merged (4bb35678) and checked on `main` 18f5e70c ("Checked on the merged main")

## Before: what START did online (measured)

Two games on this PC in a P2P room (`online_race.py --hidden --lag --link delay=80
--a DHD_NET_CREATE=p2p`, computer riders on, both pedalling), the base build (6328a749: the
lane's head with only the test hook). One game opens the pause menu at race second 25 for 2 s and
at race second 60 for 15 s (`DHD_NET_TEST_MENU=25:start,w2000,cross;60:start,w15000,cross`),
then RESUME. The numbers are each game's own log: the race clock against the wall clock (the test
hook's twice-a-second line) and the live score of each other rider (`online: race clock ...
puppets off by (units)/snapshot age (ms)`, once a second).

| | host pauses | guest pauses |
|---|---|---|
| paused game's race clock during the pause | stands still (tick 1500 for 2.2 s, 3600 for 15.2 s) | the same |
| its race clock after RESUME | rate 1.00; behind the wall clock by the pause for the rest of the race (offset 49.9 s, then 52.1 s, then 67.3 s): **no time is given back** | the same (48.5, 50.7, 65.9 s) |
| the other game during the pause | holds every rider the paused game runs: the host pausing, its rider **and all eight computer riders** on the guest (9 holds each pause) | holds the guest's rider |
| the other game after RESUME | every rider of the paused game drawn **2.3 s, then 17.6 s in the past** for the rest of the race (snapshot age 105-115 ms before, 2315-2330 ms after the first pause, 17546-17575 ms after the second), with continuous catch-ups (3098 in 30 s after the first pause, 1551 later) and riders off by up to 340 units; puppet_lag: the guest drew the host's rider and AI with a p50 error of 3771 / 4460 units, 77-79 % of steps underground | the host draws the guest 2.3 s, then 17.6 s late (737 catch-ups after the first pause) |
| what the paused game draws at RESUME | the other's snapshots arrive from its future (age -2100 ms, then -17340 ms): placed at the newest one, never caught up | the same: every rider the host runs, 17.3 s "ahead" |
| results | both games list the same order and times | the same (forced finishes at 100 / 110 s of each own clock) |

So the resume question: the pause's time is never given back. The paused game's race clock stays
behind the shared clock by the pause for the rest of the race; its snapshots are stamped that far
in the past, and it is not penalised in its own finish time either (its time excludes the pause).
A 15 s host pause left every guest drawing the whole field 17 s late until the finish. The
first try of this run over the local SFU was dropped: its DTLS handshake timed out (also over TURN)
and no frame crossed; every run here is in a P2P room, whose gameplay sync is the SFU's.

## After

Same runs on 95145fae, the lane's menu.

| | host opens the menu | guest opens the menu |
|---|---|---|
| its race clock | runs on (tick 1500 to 1633 in the 2.2 s; offset to the wall clock 48.75 s before, during and after both menus) | runs on (1500 to 1633; offset 46.7 s throughout) |
| the other game | no hold, no catch-up, no placement: snapshot age 106-124 ms all race, rider offsets as before the menu | no hold, no catch-up, no placement: snapshot age 109-126 ms all race |
| puppet_lag | the guest draws the host's rider at p50 0.14 units, its AI riders 0.32 (0.4 % underground); the host draws the guest at 0.18 | the host draws the guest at p50 0.16 units; the guest draws the host at 0.17, its AI at 0.31 |
| results | the same on both games (1. Host 1:40.03, 2. Guest 1:50.03) | the same on both games |

The rider with the menu open gets a neutral pad: it stops pedalling and rolls on with the slope
(21 mph 1.7 s into the 15 s menu on the first track), steers nowhere, crashes where an
unridden bike would. At the start gate it would stand until the menu closes (the countdown's
hold already keeps START away while the game waits for the others).

The arena's round (finding 204) with the menu opened mid-round (`mosh_round_check.py --scenario
ride --guest-env DHD_NET_TEST_MENU=30:start,w10000,cross --host-env
DHD_NET_TEST_MENU=60:start,w10000,cross`, computer riders on): PASS. The guest's menu was open
from tick 1800 to 2414 and the host's from 3600 to 4213, the round's clock running under both;
both games reached time up at tick 7199 together and listed the same table (0.0 s after time up)
and the same room results. The host's first round, before the guest was in, was a room of one:
its START was the game's own pause ("alone in the race"), the clock held at tick 3600 for the
10 s and went on from there.

The rows in a room from the ONLINE menus (`race_menu_check.py OUT --scenario S --link delay=80`,
P2P room, computer riders on, forced finishes at 60 s / 70 s):

| scenario | what happened | |
|---|---|---|
| resume | the guest's menu at 20 s for 2 s (tick 1200 to 1333), the host's at 25 s for 10 s (1500 to 2113), both over the running race; the same finishes and results on both games | PASS |
| room | the guest's BACK TO THE ROOM (Down, Cross, Down to Yes, Cross): `race menu: back to the room`, back in the room; the host raced on and saw "player 1 (GUEST) left" | PASS |
| end | the host's END THE RACE: both games "race closed: back to the room" | PASS |
| leave | the guest's LEAVE THE ROOM: out of the room ("back to offline play"); the host saw it leave | PASS |
| closed | the guest's menu open from 18 s; the host's END THE RACE at 22 s: on the guest `race closed: back to the room`, `race menu: closed (the race screen ends)`, back in the room | PASS |
| alone | the guest finished at 12 s; the host's START at 30 s: `alone in the race: the game's own pause`; its race clock held at tick 1801 for 5.2 s, then rate 1.00 with the pause left out of its time (offset to the wall clock 224.6 s, then 229.8 s); the finished guest, watching, drew the host's riders 5.3 s late from then on (HOLD, then catch-ups), as the alone rule accepts | PASS |

In each run the host's first race, before the guest was in, was a room of one: its START was the
game's own pause ("alone in the race"), whose rows the scripts then drove (in `end`, END THE RACE
from the game's own menu closed that race).

Pictures (4x internal resolution, `--scale 4 --interval 1`): the host's room menu (four rows)
over the running race in a 1280 x 720 window, and in a 1600 x 720 (20:9) window with the touch
controls on (`DHD_TOUCH_MOUSE=1`, Classic pad); a 5 s clip from per-field race captures of the
menu opening, its cursor moving and RESUME while the rider rolls on. Kept with the lane's run
files, not in the repository. The touch controls' own online panel is not in a picture: the
posted tap on their START did not reach the hidden game (no touch down in its trace).

## How the menu is drawn, and why that way

The game's pause menu runs outside the race loop: START ends `func_001B40A0` with code 2 and the
race screen opens the menu in the front end, so it cannot stay open over a running race. Running
the race loop's frames under the game's menu instead would mean the menu and the rider reading the
same pad, with the menu's update inside the loop's own popup handling (`func_0015FC70` pauses the
loop for its popups through `D_00398C40` +0x5BE4). The port already draws its own pieces over a
running race (`hle_online_hud`'s banner) and has the popup pieces in the game's look
(`menu_page.c`: the panel `menu_draw_box`, the cursor bar `menu_draw_bar`, the font in the menus'
styles), so the online menu is the port's own:

- `port/hle/race_menu.h` (pure, `test_race_menu.c`): the rows, the alone rule, and the pad's gate.
  START opens it; while open the race gets no buttons; up / down move, Cross acts, Triangle or
  START close; the room's rows ask No / Yes (on No). When it closes, everything then held stays
  away from the race until let go: RESUME's Cross does not pedal, a direction held through RESUME
  is no new press (finding 165's rule).
- `port/hle/online.c` `race_menu_pad` (each field, player 1's pad, after the countdown's hold and
  the test hook): the gate, the stick as up / down, the rider's neutral pad, the menu's sounds;
  `race_menu_chosen` runs a row: BACK TO THE ROOM as the touch panel's Leave race
  (`leave_race`, then `back_to_room` as the pause menu's Yes did), END THE RACE sends
  `host_service_net_end_race` first, LEAVE THE ROOM sets `leaving_room` (`race_over`).
  `race_menu_draw` from `hle_online_hud` (view 0): the popup pieces draw in the popup manager's
  view (`D_003F52D0` +0x20), which is set to the race's view while they are queued.
- `menu_page.c`: `menu_game_style` (the menus' styles from a SharedMenu constructed once and never
  run) and `menu_game_text` (a text id's string: RESUME 0x19, the question's NO 0x13 / YES 0x12).
- The rows' words are the room's pause menu's; the panel's title is "THE RACE GOES ON" instead
  of "PLAYER 1 PAUSED", which would be untrue. A race outside a room (the tests' `DHD_NET_CREATE`)
  has RESUME and LEAVE THE ROOM (which leaves that race, as QUIT did).
- One code path: Windows, macOS, Linux, Android with a controller, the browser's P2P rooms.
- The offline pause, and the game's own pause online when alone, are untouched.

## OPTIONS online

Left out of the menu. The game's OPTIONS page and REVIVED SETTINGS both run modally
(`func_00190108`: the popup manager's own frame loop until the page closes), so any of them stops
the race loop exactly as the pause did. Which REVIVED SETTINGS rows apply at once does not help:
the page itself is the stop. Alone, the game's own pause offers OPTIONS as before.

## The alone rule

`race_alone` (online.c) with `race_menu_alone` / `race_menu_racing`: a human (slot below
`on.nslots`) other than this game's player is still racing when it has no finish
(`on.finish[j]`; in the arena's round no final score), did not leave (`lobby.racer[j].left`, set by
the server's "left", which BACK TO THE ROOM and LEAVE send too) and was not seen to go
(`on.gone[j]`). With none, START is the game's own pause. This game's own rider does not count: a
finished player who watches with another still racing gets the menu, since a host still runs the
computer riders the others see. Decided at each START, so a race only becomes "alone" (nobody
joins mid-race; a spectator is not racing): a menu opened while not alone stays the overlay; the
next START then pauses. Players watching after their finish see the alone player's riders held
(HOLD), then drawn behind by the pause, as before (the table above); nobody racing depends on them.
The paused race clock stays behind afterwards and the race's time excludes the pause, as offline.
A room of one is alone from GO.

## Touch

Not changed (`port/hle/touch*.c` untouched). The touch controls' Pause online already opened their
own panel (Close / Bail out / Leave race) over the running race and never paused; a controller on
a phone gets this menu. Alone on a phone the touch Pause still opens the panel, not the game's
pause: a follow-up if wanted (the touch lane owns those files).

## No marker

No word over a rider whose player has the menu open: nothing on the wire carries it today, and a
coasting rider looks like any slow rider. Not worth a field.

## Decisions

- The port's own menu over the race, not the game's pause menu under a running loop (above).
- OPTIONS out of the online menu (above).
- The panel's title "THE RACE GOES ON": the game's "PLAYER 1 PAUSED" would be untrue.
- Measured in P2P rooms: the local SFU's DTLS handshake timed out on this PC (one run dropped).
- `race_menu_check.py` drives two games from the menus in a P2P room with forced finishes
  (60 s / 70 s) so each scenario takes minutes, not the first track's four and a half.

## Surprises

- The pause's time is never given back (finding 139's catch-up is for a game that could not
  step, not for a race clock that stood still), so before this a single 15 s pause desynchronised
  that player's riders by 15 s for the rest of the race on every screen.

## Not checked here

- An SFU room: the local server's SFU handshake timed out on this PC (above); every run is P2P.
  The menu touches nothing transport-specific.
- Played by hand: a real controller, the keyboard (Esc is START while riding, so Esc opens and
  closes the menu; arrows and Backspace as up / down and Triangle), the mouse (the menu takes no
  clicks: the mouse does nothing while it is open), and the menu's sounds (`menu_play`; the test
  games run without sound).
- The touch controls' online panel in a picture (above), Android, the browser, macOS, Linux.

## Checked on the merged main (2026-11-12)

On `main` 18f5e70c (with finding 202's round 4 and finding 210), by the lead's verifier:

- `python tools/check.py port -j 6` and `python tools/check.py server`: OK.
- The seven state-trace recipes (`boot`, `race`, `race2` to `race6`) equal the format-2 references
  made from 524948ba: 7 of 7 PASS. Every new line runs only online (`race_menu_pad` in
  `pads_read_now`'s online branch, the draw behind `online_mode()`, the test hook behind
  `DHD_NET_TEST_MENU`).
- `race_menu_check.py` with `--link delay=80`, every scenario (`resume`, `room`, `end`, `leave`,
  `closed`, `alone`), P2P rooms: PASS. `mosh_round_check.py` (default): PASS.
- A P2P room of six racers (two games, four nettool bots, computer riders) with the host's menu
  open at race second 30 for 5 s: the host's race clock ran on (tick 1800 to 2114 in 5.2 s), the
  guest held nobody and caught nobody up, every racer finished, the results the same on both
  games (finding 202, "Round 4 checked on the merged main").
- PC <-> Mac (P2P rooms, `cross_race.py` with `DHD_NET_TEST_MENU` on the host), each way: the
  menu opened over the running shared race (ticks 600 to 913) on Windows and on macOS, and the
  other machine held, caught up and placed nothing; in the host's first race, alone, START was
  the game's own pause.

## Result

START in an online race with another human still racing opens the port's own menu over the
running race (RESUME, BACK TO THE ROOM, END THE RACE for the host, LEAVE THE ROOM); the race goes
on, this game's rider coasts on a neutral pad, and the other games see no stop. Alone in the race
it is the game's own pause, whose time stays out of the race clock as offline. Checked in every
row from the menus, in a room of six, in the arena's round and between Windows and macOS.

Open: an SFU room (the local server's SFU handshake still fails from this PC; every check ran in
P2P rooms), the touch panel on a phone and a phone with a controller, Linux, the browser, and play
by hand (a real controller, the keyboard, the mouse, the menu's sounds).
