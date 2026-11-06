# 166: Online races on the tracks without a split-screen level (Salt City MX and TD)

Status: current
Checked: 2026-11-06

Branch `feat/online-more-tracks` from `main` 1ff885d, started 2026-11-06. Merged into `main` at
fa9de1a on 2026-11-06; released in 0.7.12.

## Goal and acceptance

Players asked for Salt City MX and Salt City TD online. The game has 26 tracks; three have no
split-screen (2-4 player) level file (`port/hle/online.c` `mp_level`: indices 17 CITY MX, 18 CITY
TDH and 21), so the original game's multiplayer lists leave them out, and the online room's
picker (finding 132, which reads the game's own lists) offers 23. Online races are not split
screen: each PC runs its own one-player race and shows the others as puppet riders (finding 044;
finding 123: "Online uses the ordinary single race"), so the one-player level files should do.

Goal: Salt City MX and Salt City TD (and index 21 if it is a real racing track; say what it is)
in the online room's picker, rotation and every screen that names a track (the room, the public
list, results, presence, the track-change notices), playable online with as many players as
their start places allow (finding 123's limits: MX tracks six, TD tracks eight; check the
markers), with nothing else changed. If one of them cannot work online, say why with evidence and
leave it out.

Acceptance: the picker shows them; an online race on each against a local server and bots
(docs/TESTING.md's online tests: start, puppets moving, finish, results, return to the room) on the
hidden desktop; the server's checks (track index ranges, capacity per track, `check.py server`)
accept them; `check.py port` passes; boot and race state traces equal to the base build (offline
untouched).

## Progress

- [x] 2026-11-06: finding reserved (10997a3)
- [x] Index 21 identified: `DUAL`, the career's Dual Slalom; left out (Decisions)
- [x] Start places read from the running game for 17, 18 and 21 (Surprises)
- [x] Server: no code change; two tests pin the game's new list and its limits
- [x] Game: `port/hle/online_menu.c`: the list has 25 tracks, the picker 13 rows
- [x] Checks: boot and race traces equal to the base build, an online room race on each added
  track against a local server and bots on the hidden desktop, `check.py port`, `check.py server`

## Decisions

- **The list is the only game change.** Each PC's race is a one-player race (`SETUP+0x288` is 1
  online), so `online.c` `write_setup` already loads the one-player level (`D_0026EE50`: `CITYMX`,
  `CITYTDH`) and never reads `mp_level` online. Names (`D_0026EFE8`: "Salt City MX", "Salt City
  TD"), the picker's picture (the `LCITSE` landscape, already the destination of 17 and 18), the
  rider limit (`func_00182290`: 17 kind 0, six; 18 kind 1, eight), the rotation bytes (track ids
  fit the low five bits) and the online unlock (`0x035FFFFF` has bits 17 and 18) needed nothing.
  `online.c` is untouched, so the transport and guard lanes merge over this freely.
- **List order is the game's index order**: 17 and 18 sit between 16 (AUBER STD) and 19 (BC), so
  NEXT now goes AUBER STD, Salt City MX, Salt City TD, BC (skipping what is too small).
- **Picker: 13 rows of 10.5 units** (y = 45 + row * 21 / 2) instead of 12 of 11, so the 25 names
  end above BACK (y 184); the current track's underline moves up one unit (y + 9) to stay inside
  its own row. Hit boxes are 10 high.
- **21 (`DUAL`) stays out.** It is "Dual Slalom" (`D_0026EFE8[21]`), the level `LVL/DUAL.NGP`
  (Salt City's picture, destination 8): a career event where the player meets one pro head to
  head (`func_001B7198`, `x10C == 3`, level 21 adds riders 0x1A0 and 0x140). Retail keeps it out
  of Single Event (`func_001E3810` skips 0x15, as it skips 0x17 MOSH) and out of the unlock-all
  bits; a single race there gets no computer riders (`func_001B89C8`: kind 3 and not 23 gives 0);
  its physics has its own path (`func_00157498` calls `func_00157590` when `x2A0 == 0x15`); and
  it has **two start places** (below), while the room's capacity rule knows 6, 8 and 10 only
  (finding 123: the server refuses other `trackLimits`, the wire packs `(10 - limit) / 2` in two
  bits). Two riders would need a new limit on the wire and the server, for an event the game
  itself offers only as a one-on-one: not worth it now.

## Surprises

- **The level files are compressed**: a word scan of `LVL/*.NGP` for the start markers (the
  flags `func_0019FF58` tests) finds random hits only. The counts came from the running game
  instead: an offline single race on the base build with `DHD_RACE_SETUP=<track>` (the state
  trace's race recipe, `--dump 3600`), reading the race's start-point table (`D_0043EEC0` +0xB0,
  12 x 16 bytes, filled by `func_0019FF58`) from the RAM dump after the load:

  | Track | Level | Start places |
  |---|---|---|
  | 17 Salt City MX | `CITYMX` | 6, one row, 6 units apart |
  | 18 Salt City TD | `CITYTDH` | 8, one row, 6 units apart |
  | 21 Dual Slalom | `DUAL` | 2, 26.4 units apart (the two-gate marker `0x7E10`) |

  17 and 18 agree with finding 123's limits (six and eight).
- **Pausing once in an online race stalls the return to the room (not this lane's code; seen on
  Salt City TD; fixed by finding 165).** In one room race the host paused (START) and resumed; at TIME
  UP the log said `online: race closed: back to the room` and the screen showed `TIME UP - DNF`,
  but the race went on for six minutes until the next START press, which went straight to the
  room. The two races before it in the same room, without a pause, went back by themselves.
  `leave_race` (`online.c`) sets `quit_on_pause` and asks for the pause mode
  (`func_001B45F8(..., 2, 0)`); after a pause the player opened and closed, that request seems
  not to open the pause by itself. Not checked on the base build. This lane's diff touches only
  the menu's track list and picker drawing; the path lives in `online.c`, which the spectate and
  transport lanes change.
- nettool bots wait for `--players` (default 4) members before their first Ready, and leave at
  once when the room's last race id already reaches `--races`: in a room that has raced, give
  `--races` above the room's race count.

## Result

Online rooms offer 25 tracks: the 23 of before plus **Salt City MX (17, six riders)** and **Salt
City TD (18, eight riders)**, in the picker, the rotation (NEXT/RANDOM, the grid-lock switch),
the room's track line, the public list, the room's LAST RACE line, presence and the
TRACK CHANGED / TAKES N notices (all read `online_menu.c`'s list and `track_name`). Index 21
(Dual Slalom) is left out, see Decisions.

Changed: `port/hle/online_menu.c` (the list, its comment, the picker's rows);
`online/server/test/session.test.ts` (two tests). The server's code is unchanged, so it needs no
deploy for this and `MIN_VERSION` stays: the server takes any track byte and the limits 6/8/10
already, and rooms only hold games of one core id, so an older game never meets a room on 17 or
18.

Checks (2026-11-06, `DHD_BUILD_SLOTS=4 DHD_GAME_SLOTS=2`):

- Base build (1ff885d + the plan commit, core `b8cb47ff344b4d4a`) against the lane (core
  `23de88946a37ff5d`): `state_trace_run.py boot`: all 600 rows match; `race`: all 5000 rows match.
- Server: `npx vitest run test/session.test.ts`: 34 passed, among them "takes the game's list
  with both Salt City events and their rider limits" (the game's 25 tracks and limits accepted;
  `trackLimit` 6 and 8; eight ready on Salt City MX switch to Salt City TD at grid lock with
  `track_switch:17:6:18`; NEXT then BC) and "races six on Salt City MX without a switch".
- `python tools/check.py port`: OK (check_docs, check_tree, build_wasm, test_runtime, build_game,
  net_tests, test_gs_wgpu, test_wasm2c_chunks); `python tools/check.py server`: OK (npm test 122
  passed in 8 files, typecheck).
- Hidden desktop, a local server (`local_server.py` + `live_game.py`, keys posted, PrintWindow
  captures kept outside the repository), this game hosting a public room built from the menus:
  - `online menu: the room's track list: 25 tracks, MOSH PIT pick-only`.
  - The picker: 25 names in two columns of 13 rows, none overlapping, BACK clear; Salt City MX
    with the Salt City picture and `UP TO 6 RIDERS`, Salt City TD `UP TO 8 RIDERS`.
  - Salt City MX, 4 players (2 nettool bots echoing the host's computer riders, one still bot)
    + 2 computer riders: `race setup: 1 players, track 17 (CITYMX)`, a six-place grid (HUD
    1/6), start points 0-5 one per rider, the puppets' snapshots received (lag 117 ms), both
    echo bots finished (0:40, 0:50), back to the room with `TRACK CHANGED: Salt City TD` (NEXT).
  - Salt City TD, 4 players (3 echo bots) + 4 computer riders: track 18 (`CITYTDH`), start
    points 0-7, all three puppets moving (lag about 120 ms each), results 1-8 in the log, back in
    the room with `LAST RACE - Salt City TD`, the host `DNF TIME UP`, `TRACK CHANGED: Mt. McSchley FR`.
  - Eight players choosing Salt City MX: `Salt City MX TAKES 6: RACING Salt City TD` on screen,
    8/8 ready, the race on Salt City TD with eight humans on start points 0-7, results 1-8 (the
    pause stall above was in this race).

Left for the merge: set this Status to `current`, mark finding 132 `partly superseded by 166
(the list of 23 tracks)`, the STATE-LOG entry.
