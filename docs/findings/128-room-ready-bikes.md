# 128: Ready for the next race, MOSH by choice only, bikes and jerseys online

Status: current
Supersedes: parts of 092
Checked: 2026-11-04

Three room changes decided with the lead (2026-11-01), branch `feat/room-ready-bikes` from
public main 6beb2c5 (0.7.6).

## 1. READY means "count me in for the next race"

Before, the server (`online/server/src/room.ts`, `onSettings`) unreadied everyone outside the
running race whenever the host changed `track` or `laps`. `laps` carries the room's options
(online_menu.c: `ROOM_NO_BOTS` 1, `ROOM_CHEATS` 2), so a new track, NEXT / RANDOM picks, and
computer riders on/off all sent every ready player back to WAIT.

Now:
- Kept: a new track (the host's Left/Right, the track list), computer riders on/off.
- Cleared, as before: cheats on/off (`laps` bit 2 changes: another kind of race), a player who
  opens the rider screen (`choosing`), the end of a race for those who raced, retiring to the
  room (`back`), the host's END, and joining (a new member starts unready).
- The session timer rule stays: a track change during the AUTO intermission leaves at least
  15 s (`nextRaceAt`).
- New: a track change during the 5 s start countdown locks the grid again (`lockGrid`), so the
  countdown runs in full from the change and the new track's limit is checked as at any grid lock
  (a 6-rider track with 8 ready switches to the next fitting one, finding 123). Before, the
  unready ended that countdown; with Ready kept, it would otherwise have started a race on a
  track the players had seen for under a second.

The game: a ready player is told. When the room's track changes and this PC didn't choose it,
the room shows `TRACK CHANGED: <NAME>` for 5 s in the amber notes at the top right (the same
toasts as `... TAKES 6: RACING ...`, `note_for`), plays `S_NEXT`, and logs
`online menu: TRACK CHANGED: <NAME>`. A grid switch (`track_switch:` event) shows only its own
note. The host's own change shows nothing (`sent_track`, 10 s).

## 2. MOSH (track 23) only when the host picks it

MOSH is the Mosh Pit arena, where riders knock each other around: not a race. It came up by
surprise through the room's Then rule (NEXT / RANDOM TRACK after a race), which walks the
track list the host's game sends once (`rotation` with `trackLimits`, finding 123).

- The host's game still sends MOSH in that list, so the server knows its limit (6). If it were
  left out, the server would take it for a 10-rider track and never switch a full grid away
  from it. Its byte now has bit 7 set, and both bridges (native `port/net/client.c`
  `client_rotation`, browser `port/web/www/net.js`) send those ids in a new optional settings
  field, `pickOnly`. The server keeps it with the rotation (`st.pickOnly`), and
  `fittingRotation`, which NEXT, RANDOM and the grid switch use, skips those tracks.
  `docs/online-protocol.md` has the field.
- In the room, Left/Right on TRACK steps over MOSH too. It is pickable only from the track list
  (Cross on TRACK), labelled `MOSH PIT (ARENA)`. The list's labels shrink to fit their 126-pixel
  cell (`fit`).
- Scripted tests can set the host's Then rule: `DHD_NET_TEST_THEN=same|next|random` (with
  `DHD_NET_SCRIPTED`).

## 3. Bike, jersey and bike colour online (GitHub issue 18)

### The shop runs online

Until now the room caught the ride to the shop (menu 18) before the shop opened (finding 092,
section 3), so online players kept a default bike (issue 18's fix: pros on their own, the
others on the stock 17 4 0). Now the shop's own pages run online, as in a single race.

- `func_001F1500` (menu 18) runs the shop modally, `func_00139BF8(0)` → popup `D_003642D0`.
  Its START (`func_0013BA60`, button 8) first commits the bike as retail always does:
  `func_0013A170` writes the setup's bike triple (`+0x11C`: frame, fork, wheels), the outfit's
  jersey (`func_001B9AE8`) and the bike colour (`+0x254` = the BIKE COLOR page's cursor,
  `D_0036ABD0[0]`), then `func_00140C38`. Retail then opens the level chooser (`D_004C7A30`),
  whose camera was the problem in finding 092.
- Patch (`port/patches.py`, `0013BA60.cpp`): online (`port_online_shop_commit`, only while the
  room's selection runs), right after that commit, the game keeps the choice (`keep_selection`
  reads the setup now, after retail's commit; finding 092's attempt read it too early) and pushes
  the shop's own exit page `D_00362490`. That is what the shop's Back does from its header
  (`func_0013C970`, the shop's v7; from inside the BIKES/JERSEYS/BIKE COLOR list, v7 only leaves
  the list, which the first try ran into). The exit page fades out and closes the popup as
  cancelled (`func_00190528(D_003F52D0, 0, 0)`). So `func_00139BF8` takes its cancel path
  (`func_00228BE0(1)` and the rest), and `func_001F1500` goes back to the previous shell screen
  (`D_004B25A0.xA08`, menu 11 here) with retail's own camera handling. `port_online_shell` then
  sees `shop_done`, runs the rider screen's Back cleanup and flies the camera to the rider's
  stand (`port_online_choose_begin(0)`, unchanged), and the room opens over it.
- Back (Triangle) in the shop is retail's: back to the rider screen to choose another rider.
  Triangle there opens the room with the previous choice.
- Scripted runs (`DHD_NET_SCRIPTED`) press nothing in the shop: START on its first frame
  (`port_online_shop_auto`), so cross_race.py and other scripted room runs go on as before.
  `DHD_NET_TEST_SHOP=1` leaves the shop to the pads.
- The menu-18 catch is gone. The GET READY (menu 9) catch stays as a fallback; the shop no
  longer reaches it online.

### Every bike and jersey

The 18 bits online's unlock_all sets in `D_004325A0 +0x2188` turned out to be the shop's
JERSEYS list (`func_0013A578` enables `D_00365DE0`'s rows from them; `func_0013A170` commits
its cursor as the outfit). So every jersey was already there. The BIKES page is not unlock bits:
it lists the save's bike garage (profile `+0x11C68`, `{frame, fork, wheels}`, count at its
`+0x4A4`, at most 63; `D_004C8508[3]` points the shop at it, `func_0013A480` / `func_0013D750`
sort it: entry bikes, own builds, bonus bikes). A new save has 3 bikes there ("Entry Bike 3/3").

Online now uses the garage list of PC SETTINGS Unlock All (finding 095, `patches_unlock.py`
`port_unlock_list`, read-only: nothing is written to the save). Its readers `func_001F9B98` /
`func_001F9BF0` answer from it while `port_online_bikes()` (online's unlock period). The list is:
- the save's own bikes;
- online only, the single race's stock bike 17 4 0 (`func_001B99B8`, issue 18's default for the
  ten regular riders);
- the 41 preset bikes the retail unlock-all code adds (`D_0026DEA0`, count `D_0029C3D4`): the
  entry bikes, the bonus bikes and every pro's bike (frames 26-32 and 34).
A new save's 3 bikes are among the presets, so its shop shows 42 ("Arcade Pro Bike (10/42)" in
the check below). The pro bikes are free for every rider, as with
the retail cheat. No host lock.

The shop starts its BIKES page on the setup's bike online (patch in `00139BF8.c`: after
`func_0013CDB0`, the career path's own `func_0013A270`, which finds the setup's triple in the
list). Before the shop opens (`shop_prepare`, first frame of menu 18), the setup's bike is set
to this rider's last choice, else to the rider's default (`hle_online_default_bike`: a pro's own
triple from `func_001B7C08`, the others 17 4 0). A player who changes nothing keeps issue 18's
defaults. The colour starts on the frame's own (`func_001FA7C0`), as in retail.

### Everyone sees it

`EVENT_SETUP` (online.c `exchange_rides`) already carried the rider, the outfit and the bike
triple from each PC's setup block at the load screen. The colour was not carried: puppets got
their frame's own colour (`func_001FA7C0`). The event is now 34 bytes with the colour
(`+0x254`) last. A 30-byte event (an older build) still parses, and its puppet keeps the
frame's colour. `hle_online_riders` writes it to the puppet's colour word (`+0x254` per rider,
`g + 0x264 + 4k` for AI slot k). Computer riders and a player whose ride never came keep issue
18's rule (pros on their own bike, others stock, the frame's colour). The local setup gets the
chosen colour at load as it gets the bike (`hle_online_selected_ride`); without a choice,
issue 18's default bike with its frame's colour.

Logs: `online menu: selection rider R (carousel place P, player 0), outfit a b c, bike f k w,
colour C` when the shop commits; `online: this PC rides ...`; `online: player N rides ...,
colour C` when an EVENT_SETUP arrives; `online: slot S (player N) rides NAME, outfit ..., bike
..., colour C (the player's choice)` where the puppet's ride is set (`(default)` when none came).

## Checks

Builds: `port/tools/build.py` (wasm core, then dhd64.exe), last core `fd3b59539aba0b59`, PASS (138
imports). LLVM came from scoop's folder (not on this shell's PATH); SDL3's CMake configure failed
once in a fresh worktree ("CMAKE_C_COMPILER not set" mid-way), so the main checkout's identical
SDL3 build (same recipe stamp) was copied into this worktree's build/. One build hung for 45 min
in a `wasm-objdump -x` of an 819-byte object (signature step); killed, the next build went through.

Server (`online/server`, `npx vitest run`): 64 of 64 pass, three runs in a row; `npm run
typecheck` clean. New tests: track, NEXT/RANDOM picks, computer riders on/off keep Ready; cheats
on/off, choosing clear it (`laps` 1 / 0 / 2 / 3 / 1 sequence); retiring (`back`), the race's
end, the host's END clear it, a joiner starts unready; a track change during the countdown keeps
the grid ready and runs the countdown again in full (300 ms in the tests), a too-small track then
switches (`track_switch:9:6:12`); NEXT and RANDOM over four races never choose a pickOnly track,
whose limit (6) still counts; pickOnly must name rotation tracks; a rotation without pickOnly (a
0.7.6 client) clears it. Two old expectations changed with the rule (track/laps unready).

Network client (`port/net/build_tests.py`): unit and loopback pass; `test_session` checks the
rotation JSON: `pickOnly:[]` without bit 7, `rotation [0,23,2]`, `trackLimits [10,6,10]`,
`pickOnly [23]` with it.

Game, two games on hidden desktops against `local_server.py` (a scratch two-game driver over
`hidden_game.Game`, keys posted with DHD_TEST_INPUT, DHD_LAUNCHER=1, DHD_MEMCARD=1 with an empty
card folder each, SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=0), host creates a public room, guest joins
with Quick Race:
- Shop online: both reach the shop (popup `083642d0`), START closes it and the room opens over
  the rider screen (`the shop closed (menu 11): back to the room`) on the chosen rider. A first
  version called the shop's v7 and stayed in the shop when START was pressed inside the list;
  pushing the exit page fixed it.
- Guest changed bike (Right x3), jersey and bike colour: `selection rider 32 ..., outfit 0 0 1,
  bike 22 12 2, colour 2`; the host's log: `slot 1 (player 1) rides Cosmo, outfit 0 0 1, bike 22
  12 2, colour 2 (the player's choice)`. The other way: host on Brian Lopes, shop untouched:
  `the shop opens on bike 26 2 4 (rider 320)`, committed `bike 26 2 4` (his own, issue 18's
  default); the guest's log has `slot 1 (player 0) rides Brian Lopes, outfit 9 0 0, bike 26 2 4,
  colour 0 (the player's choice)`. Computer riders: `17 4 0`, colour 0, as before. An earlier run
  (Cosmo, untouched) committed 8 3 0 because retail's single-race shop starts on the list's first
  bike; the `func_0013A270` patch fixed that (scripted runs since: `bike 17 4 0`).
- Ready across track changes: guest ready, host Right x2 then Left x5 on TRACK. The guest's log:
  `TRACK CHANGED: MOAB / JUNGLE / MOAB / ALPINE / BC TDX / ALPINE T / GLACIER T` (Left from
  ALPINE T went to GLACIER T, over MOSH); the host's room picture shows the guest's card READY,
  1/2 ready, track GLACIER T. Then the host readied and the race started on GLACIER T.
- One host capture in the race with the camera on the guest's puppet (DHD_PUPPET_CAMERA=1): the
  puppet and its name, from behind and far, too small to judge the bike's look.
- NEXT / RANDOM, two scripted games through the room (DHD_NET_SCRIPTED, DHD_NET_TEST_READY, the
  host on GLACIER T 22 by DHD_NET_TEST_TRACK, finish at 15 / 17 s, the shop's START automatic):
  NEXT raced 22, 24, 25, 0, 1, 2, 3 (MOSH 23 skipped after 22); RANDOM raced 22, 5, 8, 13, 20,
  16, 8. MOSH never came.

## Not checked

- A real PC <-> Mac race (cross_race.py both ways): the lead runs it before release. The 34-byte
  EVENT_SETUP and the shop flow are the same code on every platform, but untested off Windows.
- The browser build (`port/web/build.py`) was not built; its bridge change (net.js pickOnly) is one
  line, checked by reading only.
- The bike's look on another PC beyond the logs: one far capture only (no cross-run picture
  comparisons, as agreed).
- The mouse in the shop online (finding 092's bridge) and Back (Triangle) inside the shop online
  (retail's path back to the rider screen) were not exercised.
- Cheats on/off with ready players in a live room (covered by the server tests only).
- Every pro rider's default: Brian Lopes only in game (the rule is issue 18's `default_bike`).

## Compatibility

The server change works with 0.7.6 clients unchanged, so it can be deployed first:
- Ready kept across track / computer-rider changes is server-side only; 0.7.6 shows the server's
  ready state as it comes (no TRACK CHANGED line: that is the new game's).
- 0.7.6 sends its rotation without `pickOnly`, so its rooms keep MOSH in NEXT / RANDOM as today;
  the new game is needed for that. A new game against an old server: the unknown field is ignored.
- The track change during the countdown re-locking the grid applies to every client.
The shop, the bike list and the colour need the new game; rooms only accept identical builds, so
an old and a new game never meet in one race (the 30-byte EVENT_SETUP still parses anyway).
