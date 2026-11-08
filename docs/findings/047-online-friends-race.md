# 047 — Online races with friends: start, riders, grid, hits, finish, leaving, launcher

Status: partly superseded by 118 (the boot-time lobby, scripted menus, the friends' zip, the 30 fps limit)
Checked: 2026-11-04

The race protocol here (SETUP, LOADED, START on the server clock, GO, HIT, FINISH, RESULTS,
leaving, the grid) is still how online races run. Changed since: there is no boot-time lobby or
menu script (players create or join rooms in the game's ONLINE menu, findings 065, 092, 118, 119),
the shared-clock rule of step 8 is replaced by the delta-time clock and race time in ms (findings
114, 139: no two-field doubling, no 30 fps floor), snapshots and events carry later versions
(wire version 3, `port/hle/puppet.c`), and the friends' package below is gone: players use the
release with its launcher (findings 053, 093), and rooms match by release version and
`MIN_VERSION` (finding 054), not by dhd.exe's hash.

2026-10-16. Builds on the puppet riders (finding 044). The goal: a build that 4 people on 4
PCs can race. Everything here is in `port/hle/online.c`, `port/hle/puppet.c`,
`port/hle/kernel.c` (pacing), `port/gs/present.c` (window), `port/net/nettool.c` (bots),
`port/patches.py` (hooks) and `port/online/` (launcher). The relay server didn't change: the
new messages are all EVENT frames (type 6) the server already passes on.

## The flow

1. **Lobby** (then): the barrier at boot; the host starts when everyone is ready.
2. **Menus**: (then) the built-in script went to a single race. On the load screen (menu 9) each PC
   writes the lobby's track and its player's rider into the setup block `D_004B25A0`, then sends
   **SETUP** (its rider id, the rider/outfit triple +0x1B8, the bike triple +0x11C) and waits up
   to 30 s for everyone's.
3. **Riders**: `hle_online_riders`, patched into `func_001B7198` right after `func_001B74E8`
   chose the AI riders, rewrites slots 1-9: a slot that is another player's puppet gets that
   player's rider id (+0x32C), rider/outfit (+0x1E8) and bike (+0x14C, +0x264); the host's AI
   riders get the remaining regular riders in the game's order from +0x368, on the host's bike,
   the same on every PC. +0x2A8 (rider id → slot) is rebuilt.
4. **Flyby**: race phase 5 is the camera's flight over the track. START skips it (`9/5:start`
   in the menu script, every PC). The countdown sync below happens after it, so whether or when
   a PC skips can't change its GO.
5. **Grid**: `func_0012C450` takes each rider's start from the race's start-point list
   (`func_001A0498`: points handed out in placement order, slot 0 first). Online each PC's own
   rider is slot 0, so `hle_puppet_start_point` sets the list's index to the rider's global id
   first (lobby slot for humans, slot for AI). The first call still runs point 0, which sets the
   start orientation for all.
6. **Start**: at the countdown (phase 1) each PC sends **LOADED** and waits. When all have (or
   after 60 s) the host sends **START** = server time + 1.5 s; every PC holds until the server's
   clock (`net_server_ms`, the lobby's clock offset) reaches it, then lets the countdown run.
7. **Race**: snapshots as in 044, now with the global rider id, the checkpoint count (+0x7990,
   so places and the finish order agree), the attack kind the combat power-ups gave (+0x6390:
   0 punch, 1 kick, 2 check, 3 stick, 4 bottle; `func_00179A60` shows it on the HUD) and the
   bottles left (+0x7ADC); wire version 2. A puppet attacks as its owner can. Each PC sends
   **GO** (its GO on the server clock) so both sides log the skew.
8. **Shared clock** (then; replaced by findings 114 and 139): from the agreed moment the frame pacing keeps to it
   (`kernel_pace_shared`). A PC that falls behind (a hitch, a stall, a PC too slow for the race)
   runs without waiting, and while more than a field late each game frame gets two fields: the
   game then measures the frame as two fields and moves everything twice as far, as it does on a
   PS2 that drops a frame. The race clock counts both fields (the scripted pad, keyed to the race
   clock, kept 30 fields per 30 ticks while fields were doubled). It never restarts from now
   online: an early version restarted after 10 s late, and that PC stayed about 10 s behind the
   other for the rest of the race. A run that also allowed three fields a frame ended with the two
   race clocks about 2 s apart while both PCs were under 1 s late, so it is two at most.
   So a PC keeps up as long as it renders at least 30 frames a second.
9. **Hits**: as in 044: the attacker's PC sends **HIT** (owner PC, victim and attacker global
   ids, kind, direction); the victim's PC applies it to its own rider.
10. **Finish**: each PC sends **FINISH** (global id, race ticks) for the riders it runs. A
    puppet's finish is written into the race's time table (`D_0043EEC0` +0x1C), and 60 ticks
    after a reported finish a puppet that hasn't crossed is finished (+0x62). The game's
    end-of-race countdown (`func_0019F9B0` → `func_001B45F8`) is held (`hle_online_race_end`)
    until the host's **RESULTS** arrive: the host sends them when every player has finished or
    left, or 90 s after the first finish, ordered by time, then by the game's places.
    Every PC then releases the end (4 s) and shows the order in the window title.
11. **Leaving**: every 15 fields each PC checks the lobby; a player who left is marked gone,
    the host moves to the lowest remaining player, and the riders the leaver ran (their own;
    the AI if the host left) become the new host's AI riders (`hle_puppet_drop`). Since 0.7.3 a
    player who had already finished is the exception: the AI raced their rider on past the line
    (seen when a 240 fps guest crashed after its finish). Their rider stays a puppet on its last
    snapshot on every PC, run and sent by none, keeps its time in the results, and goes to the
    host's AI only in the next race.

EVENT payloads: byte 0 version 2, byte 1 type: 1 HIT, 2 LOADED, 3 SETUP, 4 START, 5 GO,
6 FINISH, 7 RESULTS. The relay stamps the sender's lobby slot.

## The window and stalls

Windows runs a modal loop inside `DispatchMessage` while a window is moved or resized, so with
the message pump on the game thread the game stopped until the mouse was let go. The window now
has its own thread that only handles messages (`present.c`, `window_thread`); the game thread
draws and presents and exits when the window closes. Measured with `WM_SYSCOMMAND SC_MOVE`
held 10 s and the game's frame counter read from outside: 60.0 frames/s before, 59.9 during,
60.0 after. The WebSocket has its own I/O thread, so frames keep arriving during any stall and
wait in the queue; the shared clock above then brings the PC back.

## Tests (two instances on one PC, through the deployed relay)

`port/tools/online_race.py` runs a host and a guest, each on a scripted line (the guest punches
and kicks for the first 40 s), with `DHD_NET_TEST_FINISH` to finish the riders early,
`DHD_NET_RECORD` to record the guest for bots, `DHD_PUPPET_CAMERA=1` to have the guest's camera
follow the host's puppet, and `h+N` capture moments (N ticks after the first hit). Two full
games on one PC each ran slower than real time (the log's "ms late" grew to 3-14 s over a
race; for part of the runs another worker's two games ran too), so lag after the first seconds
measures this PC, not the method.

| Run | What it showed |
|---|---|
| r1 | Before this round's fixes: synced start (GO 0 ms by the old measure), riders and AI picks the same on both PCs; but both own riders on start point 0, the flyby before the sync, no hits. |
| r2 | Grid: each PC places host on point 0, guest on point 1. Flyby skipped (countdown at field 1465 instead of 2954). GO skew 58 ms. Hit: the guest punched the host at tick 74; the host's PC applied it at 82. Finish and results: Host 0:50.06, Guest 0:55.06, then the AI by place, the same list on both PCs; the end screen waited for it. |
| r3, r4 | Characters: the host's rider on its own screen and the host's puppet on the guest's (camera on the puppet) look the same (rider, helmet, clothes). Hits: 4 per run, on the host and on two of the host's AI riders, all applied by the host's PC. The victim's screen shows "PUNCHED BY DRUD!" and the rider knocked to the side; on the guest's screen the host's puppet veers the same way about 15 ticks later. GO skew 55 and 41 ms. Both runs: the guest's game crashed at race tick 1240 / 1761 (below); the host logged "player 1 (Guest) left ... host now player 0", "slot 1 (player 1) is this PC's AI now", and raced on. |
| r5 | Four players: 2 games + 2 nettool bots replaying the r4 guest recording. All four loaded and sent their riders (Mai, Jonnie for the bots); finishes Bot0/Bot1 0:33.69, Host 0:45.06, Guest 0:50.06; the same results on both games; the bots' leaving afterwards handled. No crash. |

Lag, in ticks (1/60 s) by which a puppet is shown behind the viewer's own race clock (the
6-tick interpolation delay included), over the first 8 s of the race while both PCs were
within about a second of the shared clock:

| Run | Guest's rider on the host's screen | Host's riders on the guest's screen |
|---|---|---|
| r2 | 3.8-13.5 | 5.1-14.8 |
| r3 | 12.7-14.2 | 4.0-5.0 |
| r4 | 2.0-14.7 | 5.0-13.0 |

That is 0.03-0.25 s, mostly around 0.1-0.2 s. Later in these races the two games drifted
seconds apart, because each fell behind real time by a different amount (this PC can't run two
games at full speed); one game per PC is what the method is for.

**The crash.** Both guest crashes were a NULL read in the VU0 macro code of `func_00165408`
(the impulse that knocks a breakable trackside object away), reached through
`func_00192D50` → `func_00126F38`. `func_00192D50(object, direction, rider)` called
`func_00126F38(object->x230)` with the object only; on the PS2 the direction and rider were
still in `$a1`/`$a2`, in the port they were junk. Passing them on compiles to the same bytes
(`tools/try_func.py`: MATCH func_00192D50, whole image MATCH). Not online-specific: any rider
breaking such an object could hit it. r5 ran with the fix and didn't crash.

## Limits

- **Speed** (then; finding 139 removed the floor): a PC had to render at least 30 frames a second in the race to keep to the shared
  clock (two fields a frame). Slower, it falls behind and the others see its rider late.
- **Leaving**: tested only as the guest's crash (the host took its rider over). The host
  leaving (a guest becomes host and takes the AI) is coded, not run. A deliberate kill
  (`online_race.py --kill-guest-at`) wasn't run either.
- **Results screen**: the host's order goes into the window title and each PC writes the
  others' finish times into the race's table before its end screen; that the game's own
  results screen lists the same order wasn't checked on screen.
- **Bikes**: each player's bike triple travels, but a fresh save has only the starter bike
  (8, 3, 0), so every run had the same bike; other bikes are untested.
- **Power-ups**: the attack kind and bottles travel; no run collected one (the scripted lines
  punch with the starting punch), so a puppet's kick or throw wasn't seen. Pickups on the
  track are each PC's own. This limitation is superseded by finding 144 (shared pickup events).
- **Bots** replay one recorded line, all on the same path; they test the protocol, not riding.
- **4 PCs**: not tried on 4 real PCs yet.

## The friends' package (gone)

`python port/tools/package_online.py` (with `DHD_NET_SERVER` set, so it can check) writes
`build/dhd-online.zip`: dhd.exe, `Play-online.cmd` + `Online.ps1` (create or join with a
code; name, server, ISO, rider, resolution, display, widescreen), `Settings.cmd` +
`Launcher.ps1` (the dhd.ini launcher) and README.txt. No dhd.ini, no game data, no server
address: the owner sends the address privately, the player pastes it into the Server box once
and it stays in that PC's dhd.ini (keys `server`, `name`, `rider` map to `DHD_NET_SERVER`,
`DHD_NET_NAME`, `DHD_NET_RIDER`). Every PC must use the same zip: the room version is dhd.exe's
hash. The picture settings are each player's own.

### The first friend zips ran at half speed

The first two friend zips held a bare `link.py` executable linked without `DHD_BUILD_VU_AOT=1`,
so the VU1 mesh programs ran through the interpreter (no AOT blocks in its map). Same bench
(`bench_race.py`, fields 6000-7000, unpaced, one run each): 34 fields/s against 93 for main's
round 32 build. In a real race that is about 30 fields a second, half speed, at any resolution
or picture rate. Rebuilt with `build_clean.py --recomp-basis build/port/r29/native16/objects.rsp`
at 618b0a3d: 96 fields/s. `package_online.py` now packages only a `build_clean.py` executable
whose manifest says VU1 AOT.

## Sources

- Microsoft, "WM_ENTERSIZEMOVE" and "Using Messages and Message Queues" (learn.microsoft.com):
  the modal move/size loop runs inside DefWindowProc on the window's thread.
- Microsoft, "DXGI flip model" and "Multithreading and DXGI" (learn.microsoft.com): the
  thread that owns the window must keep handling messages; presenting from another thread is
  allowed.
- Downhill Domination controls (GameFAQs guide by Menji; Baidu Baike): Square attacks to the
  left, Circle to the right; the attack kind follows the combat power-ups.
- Findings 041, 042, 044.
