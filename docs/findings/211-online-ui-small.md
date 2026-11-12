# 211: Three small online and phone UI bugs: wrong race-end message, room stall, keyboard tab hint

Status: in progress

Branch `fix/online-ui-small` from `main` 18f5e70c, started 2026-11-12. This file is the lane's
plan while it runs and its finding when it merges. Set `current` at the merge, after the phone
checks below.

## Goal and acceptance

Three bugs seen on a phone (its dhd.log and screenshots) in an online P2P race:

- A. The phone hosted, one racer finished, the finish window ran out with the phone's own rider
  still on the track (DNF TIME UP) and the phone showed "THE HOST ENDED THE RACE" although nobody
  ended it. Each way a race can end must say what happened, on the host and on a guest.
- B. A 2.9 s menus stall (`perf:` window `max=2883.9ms`) going from the race back to the room.
  Find what blocks and fix the avoidable part, with before and after timestamps on Windows.
- C. OPTIONS > SETTINGS shows the keyboard hint "A / D Tabs" with touch controls on and no
  keyboard. Show what a touch player can do; make the tab names tappable if tabs cannot be changed
  by touch; check the other port pages for the same wrong hint.

Done: each bug's cause below; A shown on host and guest for each way a race ends; B timed on
Windows; C captured before and after at 20:9 with a tab changed by a posted touch;
`python tools/check.py port` and `check.py docs` green. Out of scope: `touch.c`, `touch_field.c`,
`touch_host.c`, `port/render/settings/src/table.rs` (another lane works there): not touched.

## Progress

- [x] 2026-11-12: finding reserved (1c374a3c)
- [x] A: the end reason from the room's `ended` message (b49c2b65), the host's own end (833a8af0),
  scenarios in `race_menu_check.py` (583acca6)
- [x] C: the legend says "Tap a tab" (622b7ef1), `port/tools/settings_touch_check.py`
- [x] B: could not be reproduced on Windows; probes added (03fc5bdb); the phone's log lines below
- [ ] Phone checks (the lead): see Result

## Decisions

- A is a client fix, no protocol change: `ended` already carries `reason` (`done` or `void`) on the
  servers since 0.7.13. The client now keeps it (`NetLobby.ended`, appended at the end of the struct,
  and the browser's `net.js` writes it) and one pure rule (`port/hle/online_end.h`, unit test
  `test_online_end.c`) names the end. A server too old to send a reason shows "THE RACE ENDED", never
  the host. The release does not need to deploy the server for this.
- The host that presses END THE RACE leaves by its own menu at once (`race_menu_*`), so it shows no
  reason; only the others read "THE HOST ENDED THE RACE".
- The 2 s the reason stays on screen is the existing rule (finding 118), not touched.
- C: the page's tabs were already tappable (`page_header_mouse`, finding 163): a posted touch on
  "Graphics" changed the tab in the 0.7.16 build too. So the bug is the legend only. With touch
  controls on (`DHD_TOUCH`: a phone, or a desktop's mouse as a finger in checks) and no pad it says
  "Tap a tab"; with a pad it keeps the pad's buttons (as before). The keyboard A / D stay on a PC.
- Other port pages checked for keyboard-only hints with touch: the Controls page's "Esc always goes
  back in menus and pauses a race" and "Press a key" are for the keyboard device, a page a phone
  does not offer (its Controls tab has Vibration, Dead Zone, Button Icons and Touch Controls); the
  online pages, the room card and the race menu use the PS2 legend (its words are tappable,
  `page_legend_button`) and the other hints are not about keys; the window title's "(or Enter)" is
  the PC window's. "LEFT / RIGHT - CHANGE RIDER" (watching after a finish) names the game's own
  buttons, which the touch pad has. Nothing else to change.

## Surprises

- A's cause: `online.c` state_sync told "TIME UP" from "the host ended it" by whether the room's
  `lastRace.id` had changed since the race loaded. The server sends `ended` first and the room
  snapshot with `lastRace` after it (`closeRace`: `ended` is broadcast, then the caller's
  `changed()` stores and sends the snapshot). The client set the lobby state on `ended`, so a game
  that looked in that gap saw "not racing, no new result": the host. The gap is as long as the
  snapshot's storage write, a few ms to tens of ms on the real server, against a frame of 16 ms: on a
  phone it is hit. The phone's log shows it (the results lines come at the same second as `race
  closed`, then "DNF TIME UP" in the room). On this PC's local server the gap is too short to hit:
  every run's `race closed` line already had `result 1`. The unit test feeds the rule that very
  state (`ended` done, no new result) and the old rule's answer for it was the host.
- B: the 2.88 s picture gap is not at the race's end on Windows and could not be reproduced.
  Numbers, Windows, local server, P2P room, all four ways a race ends: the race screen shows the
  reason for 2 s (kept on purpose), then `this PC's race is over; back to the room` came 934 ms
  after the race screen was asked to leave (the game's own level unload and front end's load; the
  pictures there are the game's loading picture, then the rider screen: 0 `stall:` and 0
  `perf: long frame` lines in any of the runs). The phone's log has the same steps: the leave
  request at about 01:57:59.2, `race is over; back to the room` at 01:58:00.331 (1.1 s), the rider
  screen at 00.340, the room's results at 01.130 and the `perf: render` line 2 s later. The 2.88 s
  gap cannot fit in the 1.1 s unload, so it reaches into the front end's first moments, where the game
  thread was alive (it logged at 01.130) while no picture was presented: a render-side wait (a
  texture or pack decode, a pipeline, the driver), not a blocking call on the game thread. Code
  audit of the way back: `client_back` queues one message (`ws_send` only queues), `hle_puppet_offline`
  and the race state reset are memsets, `ws_stop` (which waits up to a second for the queue) runs only
  on LEAVE THE ROOM; the only waits on the game thread in the online code are the lobby wait and the
  riders' exchange at the race's START (the 3 s `stall:` every race has on Windows, at the load).
  Not the screenshots of the phone run either: the same harness took screenshots every 10 s during
  the race windows, whose longest picture was 40 ms.
- `gen_net_layout.py --check` fails on this clang for the wasm32 target (`stdlib.h` not found in
  freestanding mode, `net.h` includes it since finding 204): not from this lane, the layout file
  itself regenerates (`ended` at offset 5236, 4 bytes).

## Result

A. `NetLobby.ended`, `online_end.h`, the log line `race closed: back to the room (WHY: ended E, link
L, own STATE/REASON, result R)`. Windows, hidden desktop, local server, P2P room, two games
(`race_menu_check.py --scenario ... --scale 4 --interval 1`, all PASS): `end` (host's END THE RACE):
guest "THE HOST ENDED THE RACE" (ended 2); `timeup` (host finishes at 20 s, guest never): guest
"TIME UP - DNF", host "RACE OVER"; `timeup-host` (the phone's case: the host never finishes, the
guest does at 10 s): host "TIME UP - DNF", guest "RACE OVER"; `finished` (both finish): "RACE OVER"
on both. A lost connection says "CONNECTION LOST" (the host's removal "THE HOST REMOVED YOU"), a
rider out while the race goes on says "YOU LEFT THE RACE" or "LINK TO THE OTHERS LOST" from its own
entry. The connection cases are in `test_online_end.c` and the client's reading in `test_session.c`;
not run live. Wire: nothing changed. Server: not touched, nothing to deploy; the release needs no
`MIN_VERSION` change.

B. Not reproduced; no behaviour changed. Added to the log (logging only): `stall: field N came T ms
after the last (menu M phase P)` (a gap over 0.4 s between fields, `kernel.c`), `perf: long frame: a
picture T ms after the one before (menus|race, field N)` (500 ms and more, `perf_log.c`, at most 40
a run) and the leave-to-room time in `this PC's race is over; back to the room (N ms after the race
screen was asked to leave)`. On a phone, look for them around the line `online menu: back to the
room after the race`: a `perf: long frame` there with no `stall:` line is a render-side wait (the
renderer, not the game); a `stall:` line is the game thread, and its menu and phase say where.

C. `pc_settings.c` page_draw. Windows, 1600x720 (20:9), `DHD_TOUCH_MOUSE=1`, no controller seen,
`settings_touch_check.py`: before (the 0.7.16 build) the legend reads "A / D Tabs", after "Tap a tab";
a posted tap on the Graphics tab's name changes the tab in both. The pictures were checked by eye
and are not kept in the repository.

State traces (the lead; none run here): `boot`, `race`, `race2` to `race6`, `split` and `memcard` against
the references of the v3 rule in `docs/TESTING.md`. They cannot change: the online code runs only
online; `stall:` and the leave time are logging that reads two words and writes none; the legend
differs only with `DHD_TOUCH` set (traces run without it); the `NetLobby` field is read by online
code only. The core's layout did move, so `split` and `memcard` need a base built at the merge base
(same layout rule, finding 205).

Phone checks (the lead): (1) an online P2P race on the phone as host where the phone's own rider is
still riding when the finish window runs out: the screen must say "TIME UP - DNF" and the log
`race closed: back to the room (TIME UP - DNF: ended 1 ...`; the finisher "RACE OVER". (2) The log's
`stall:` and `perf: long frame` lines on the way back to the room, with the line numbers or logcat
times of `race closed` and `back to the room after the race`. (3) OPTIONS > SETTINGS with no pad:
"Tap a tab"; a tap on a tab's name changes it. (4) With a pad connected the pad's own L1 / R1 words.
