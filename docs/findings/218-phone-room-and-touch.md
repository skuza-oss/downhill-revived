# 218: READY on a phone in an online room, the attract demo's SKIP, the D-pad's place and taps on settings rows

Status: current

Branch `fix/phone-room-and-touch` from `main` c5f839a2, started 2026-11-12. This file is the lane's
plan while it runs and its finding when it merges. Merged into `main` on 2026-11-12 (63a24aa9) and
checked there the same day ("Checked on the merged main" below).

## Goal and acceptance

The phone check of main 1140b23f (Xiaomi Mi 11i, release-signed APK) found four things; an outside
review of lane 214's code added a fifth:

- A. In a P2P room joined from the phone, a tap on READY did nothing: phones could not race
  online. Find the cause, fix it, and add a touch walk of the room page that readies a member.
- B. The title's attract demo showed no SKIP on the phone. Make it show and work, find why finding
  214's unit test passed, and add a run that checks it in the real game.
- C. The D-pad (finding 214) overlaps the game's own HUD in Classic, left hand; the TOUCH
  CONTROLS page's rows unavailable under D-pad do not look dimmed; rows on that page did not take
  taps.
- D. A double tap left or right on the D-pad showed no powerslide.
- E. In a replay at more than 60 updates a second, MENU flickers and a held tap is cancelled.

Done: each item's cause, fix and proof (Windows captures, phone screenshots), `check.py port` and
`check.py docs` green.

## Progress

- [x] 2026-11-12: number reserved (d9ff9736)
- [x] B: the attract demo's SKIP (978d0f10)
- [x] C: the D-pad clear of the HUD (360e1bcc); dimmed rows and the tap log (ec9147e0, 61c89259)
- [x] D: D-pad taps reach the game; the double tap (edef2fca)
- [x] E: the replay's MENU at high rates (3bfe0fb1)
- [x] A: READY logged and answered on the page (6dbe12a5); the READY touch check (ba44b9fc);
      ONLINE takes taps again after a room (37e8c1a1)
- [x] Phone session on a release-signed APK of this branch (Mi 11i)

## A. READY in a P2P room on a phone

**Not reproduced, on Windows or on the phone.** The client sends READY without conditions
(`client_ready`, `port/net/client.c`); the server refuses it only in a running race or while the
member is choosing (`room.ts`, case `"ready"`), and it answers a refusal with an error the client
logs. The phone check's log has neither a refusal nor any sign the tap reached the page (no line
logged a tap then). On Windows, a guest with touch controls at the Mi 11i's shape that taps READY
in a scripted host's P2P room on a local server readies at once (`room_ready_touch_check.py`), and
on the phone with this branch READY by tap worked both as the guest (the PC's room) and as the
room's creator (the phone's room): `online menu: READY on by tap`, `the room has this player
ready`, the race started, both sides got the room's results. Nothing merged since dbddf61e changes
the room page's tap path: lane 214's overlay stands aside in the menus (in the Windows check the
overlay never took the touches between joining and the race: no `touch: the overlay takes the
touches` line), lanes 211 and 206 did not touch `room_mouse` or its hit boxes, lane 215's input
gate (`race_menu_pad`) runs only in a race.

**What the check's session had that this one did not**: the official server's Realtime sessions
failed for both sides, then `Too many Realtime sessions`. That error is the server's answer to an
`sfu_offer` (`room.ts`), not to READY, and READY does not wait for the race link. The most likely
cause left is the one found below: a page whose taps go to a room's hit boxes after the room is
gone. In the check, the phone had already been in and out of online pages that session.

**Found on the way, fixed (37e8c1a1)**: after leaving a room that was open over the rider screen,
ONLINE took no taps at all: the room installs its own tap handler and drawing (`room_open`), the
room closes the page without `go(HOME)`, and `port_online_menu` never cleared them, so every tap
on ONLINE went to the room's hit boxes (pads still worked). Seen on the phone: after leaving the
first room, Create A P2P Race did nothing and the log said nothing.

**So a dead READY can never be silent again (6dbe12a5)**: each READY is logged with how it came
(`READY on by tap` / `by pad`) and the room's answer (`the room has this player ready / not
ready`); a READY the room does not show within 4 s puts `READY DIDN'T REACH THE ROOM: TRY AGAIN`
on the page, and the room's errors (only logged before) show as a note. The touch host logs when
the overlay takes or gives back the touches, and the settings pages log each tap's place and row
(`menu mouse: click at x,y: row N`), so a report says where a tap went.

**The test that would have caught a dead READY**: `port/tools/room_ready_touch_check.py`. The
online tools' guest route (`cross_race.P2P_GUEST_NAV`) ends with a Cross that lands in the room on
READY and readies by itself, so no earlier single-PC check ever tested the tap; this one leaves it
out.

## B. The attract demo's SKIP

Cause: `touch_field.c` read the demo as `port_in_race() && mode == 10`, but `port_in_race`
(`fps.c`) leaves mode 10 out by design, so the flag was never set on any screen. Lane 214's unit
test set `game.attract` by hand and never ran `touch_field`. Fix: `port_attract_demo()` (the race
loop runs mode 10). Test: `port/tools/attract_skip_check.py`, a real game on the title with
`DHD_TEST_ATTRACT_AFTER=5` (the demo after 5 s; the title's own check is `func_00238F48` more
than 3600 ticks past `D_0029DE9C`, `001EB1D8.cpp`). On a test desktop a posted press now lands
where its posted move put the finger (`touch_host.c`), as `present.c` already did for the menus.
Phone: SKIP showed over the demo after the minute on the title and one tap ended it.

## C. The D-pad's place, dimmed rows, taps on value rows

**The HUD.** Measured on the phone's own picture of a race (2400 x 1080): the track bar starts at
52 % of the height, 3.2 mm in from the edge, and leans outward to 12.4 mm at the bottom; the place
number sits inside that line. Before: Classic's D-pad's outer arm was 6.3 mm from the edge (on the
bar) at the default size and 2-3 mm at Size 50-70, either hand; Thumb zones' at Size 50 and 70
too. `dpad_home` (`touch.c`) now takes the nearest place to the stick's home where every arm is
clear of that keep-out (as fractions of the height: the HUD scales with it) and of L3 by the
buttons' 3 mm gap; Classic's arms shrink to fit between the bar and L3 (7.9 mm at full size on the
reference phone instead of 9.0), the pad staying on the edge side of L3 like a pad's d-pad beside
its stick; at small Sizes, where the cluster shrinks into the corner and the HUD does not, it may
sit past L3. The speedometer and place digits at the top, the PUNCH text and the messages are
nowhere near the D-pad in any layout. The layout test now checks the keep-out at Size 50-150 on
two screens. Windows at the Mi 11i's shape, 4x: all four layouts clear of the bar and number;
phone: Classic right and left in a race.

**Dimmed rows.** The game's popup draws a disabled row in the normal colour (only its cursor skips
it). `menu_page_dim` gives the game a blank row as wide as the text (the panel keeps its width)
and draws the text at half the row colour; TOUCH CONTROLS uses it for every unavailable row.

**Taps on value rows** work and always did (finding 092: a tap on a value row presses Right, on its
`<` Left): on Windows and on the phone Steer With went Stick, Tilt, D-pad by taps, each logged.
The check's "no row reacted" is most likely the same dead-handler state as in A (the page reached
after online pages), or taps landing between rows; now every tap is logged with its row.

## D. The D-pad's double tap

The game's double tap (`func_0017EDE8`, per button) wants a new press with the button up the field
before, under 30 fields after the release, the first press not held past 60 fields. The D-pad sent
where the thumb was at each pad read and dropped its buttons the moment the thumb lifted: a tap
whose down and up fell between two reads (a quick thumb; the phone's tap tool) never reached the
game, and a second tap while the first still showed got no field up. Fix (`dpad_field`): each new
direction lasts at least PRESS_FIELDS (as the other touch buttons), and a field up comes first when
it is pressed again while still showing; a thumb sliding on lets a direction go once shown, so a
slide never sends opposite directions. Unit test `test_dpad_double_tap` feeds the output to a
model of `func_0017EDE8`: before, neither tap reached it; after, one double tap at every gap.
Windows touch-script run (two 50 ms taps 150 ms apart on the left arm): `sent 0080@8006 0000@8056
0080@8156 0000@8206` (race ms), press, up, press, nine fields apart. Not shown: the slide itself
in a picture at the right moment (pictures came once a second; the race-moment capture did not
fire under `DHD_NAV`).

## E. The replay's MENU at high frame rates

`flyover_on` took a replay for its menu whenever one update's clock delta was 0, and above 60
updates a second most updates advance no whole field: MENU vanished on them, a held finger went
`JOB_SKIP` to `JOB_IGNORED`, a START pulse could be taken back. The replay's menu is now the popup
manager's own state (`menu_popup_up`, `HostTouch.menu_up`); MENU waits for the replay's clock to
have moved once (`replay_ran`), so the load and countdown still show nothing; and `clock_moving`
means "advanced within the last 1.5 fields' time", which the pause rows and hints read too. The
flyover's SKIP (race phase 5) and the movies' (the host's movie state) never used the clock and
hold across updates. Unit test `test_replay_high_rate`: 120 updates a second, a finger held on
MENU, one START. Not run on the phone (no replay reached); the phone's Frame Rate is 60 fps (its
default), so the phone's own replays were not at risk.

## Decisions

- A: no server change. The server already refuses READY only for two reasons and says so; the
  client now shows what the server says and notices a READY that was never answered.
- C: the HUD's place is a measured keep-out, not read from the game: the HUD's pieces are drawn by
  the game's own code with no layout the port can ask; the numbers are fractions of the height.
- C: arms shrink rather than the pad moving past L3 or onto the bar (7.9 mm, the panels' targets
  are 9 mm; still above finding 141's minimum).
- D: the minimum press is PRESS_FIELDS like every other touch button; no new constant.

## Surprises

- The phone check's dead READY did not come back with this branch, on the official server, in two
  rooms; but a dead ONLINE page did, reached by leaving a room.
- Lane 214's guest test route readies by a script Cross, so READY by tap had never been tested.
- The game remembers the single-player mode: on the phone a stray tap on CAREER sends later rider
  selections straight to CAREER's NEW / CONTINUE screen (not touched: it leads to the save), so
  the Thumb-zones layouts were checked on Windows only.

## Result

Eight commits of fixes and two check tools (`attract_skip_check.py`, `room_ready_touch_check.py`). `check.py port` and `check.py docs` on the last
commit. State traces: the lead's seven recipes against the v3 references; nothing here can change
them: every change runs only with touch on (`DHD_TOUCH`), in the Online menu, on a settings page
with dimmed rows (only TOUCH CONTROLS asks), or with a test switch; `touch_field` reads the game
and the host clock and writes no game memory; the tap log runs only on a click.

Phone (Mi 11i, release-signed APK of this branch, owner's settings): dhd.ini and the four save
files have the same md5 before and after; Layout, Hand and Steer With were changed by tap for the
test and put back (Classic pad, Right, Stick).

Open:

- The original dead READY on the phone was not reproduced; its cause is unproven (the stuck tap
  handler fixed in 37e8c1a1 is the likely one; the page now says when READY does not reach the
  room).
- Thumb zones were not checked on the phone (Windows only).
- A replay's MENU was not reached on the phone (unit test and Windows only; the phone runs at 60).
- The visible slide of a D-pad double tap was not captured in a picture (the touch log shows the
  presses).

## Checked on the merged main (2026-11-12)

On `main` 63a24aa9, by the lead's verifier (Windows, hidden desktops, no timing runs, the game
lock respected).

- `python tools/check.py port -j 6` (`DHD_BUILD_SLOTS=2`): OK in 16 min (`build_wasm` 218 s,
  `test_runtime` 182 s, `build_game` 283 s, `test_gs_wgpu` 67 s, `test_stack_gate` 127 s). The
  touch unit tests run inside `test_runtime` and show as the line `test_touch: all passed`
  (`port/hle/test_touch.c`, with `test_dpad_double_tap` and `test_replay_high_rate`; the
  gamepad mapping's lines before it are another test).
- State traces: `boot`, `race` and `race2` to `race6` each equal the format-2 references made from
  524948ba: 7 of 7 PASS (`state_diff.py`), as the lane predicted.
- `attract_skip_check.py`: PASS, SKIP ended the attract demo (`touch log: attract 1`, then
  `attract 0` after the tap; pictures `demo-skip.png`, `after-skip.png`).
- `room_ready_touch_check.py --race`: PASS, READY by tap took and the race started (the guest's
  log has `online menu: READY on by tap` and `the room has this player ready`).
- Two games in a P2P room on a local server (`online_race.py --hidden`, the host's rider finishing
  at 70 s and the guest's at 72 s): results equal on both games (`result 1. Host 1:10.03`,
  `result 2. Guest 1:12.03`, then the computer riders) in two runs. With the test link of the
  earlier checks (`--link delay=80,jitter=15,loss=1`) neither log has a `refused` line. Without a
  link the logs hold `HOLD refused` lines (1 in the host's, 9 in the guest's) at race second
  74.7, after both riders had finished (the other sender's last snapshot stamp, quiet 1.07 s):
  finding 220's rule meeting a finished sender; no code of 220 differs from the build the earlier
  link runs used, and nothing of lane 218 touches the puppets. Not looked into further here.
- Not run: the phone, Thumb zones, a replay, PC <-> Mac.
