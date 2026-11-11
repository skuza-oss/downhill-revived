# 203: Online MOSH PIT closes the game at the race start (issue #84)

Status: in progress

Branch `fix/online-mosh-start` from `main` f2dd2d4b, started 2026-11-11. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

GitHub issue #84: on 0.7.16 an online race on MOSH PIT (Arena) closes the game as it starts,
every time, with 2 or 3 players; other tracks are fine. Reproduce it on main, find the cause, fix
it, and check the neighbours (the room's other special tracks, every player count the room
allows, computer riders on and off, high rider and bike ids): an online race on each starts and
runs to the results. Offline play and deterministic runs stay unchanged.

Acceptance: `python port/tools/room_tracks_check.py` prints `25 of 25 tracks load and race`
(on f2dd2d4b track 23 fails with the guest memory fault); the online runs below reach the results.

## Progress

- [x] 2026-11-11: number reserved (8d3aefcb).
- [x] 2026-11-11: the place. The reporter's log, named with 0.7.16's linker map
  (`port/tools/crash_stack.py --map DownhillRevived-0.7.16-win.map dhd.log`):
  `func_0021AFF8` <- `func_00218E88` <- `func_001829C0` <- `func_00182838` <- `func_001B7198` <-
  `func_001B6708` (the race load). `func_00182838` loads the race's loading bar
  (`func_001829C0`: the bar's `RTX` file into a texture list, `D_004D30B0`, then
  `func_00218E88` relocates each texture file in it; `func_0021AFF8` reads a file's header word
  at `file + 16 * file->x4 + 8`). The fault address `0x12A0020D` is that read on a file whose
  `x4` is garbage.
- [x] 2026-11-11: the cause. The bar's name comes from `func_00182330(track, mode)`. Online races
  run in Single Event mode (`SETUP+0x10C = 12`, `port/hle/online_menu.c` `port_online_shell`),
  whose bar is `L%sSE` with the track's short name (`D_0026EF60`). The arena's short name is `MOS`,
  so the name is `LMOSSE`, and the disc's `LOADBAR` folder has no such file: it has `LMOSH` (the
  bar of the game's own Mosh Bowl mode, 15) and `LMOSHSE` (a small card, not a bar). The load of a
  missing file (`func_0020D728`) returns size 0 and leaves the bar's buffer as it was, which
  `func_00218E88` then walks as a texture list. Every other room track's `L<short>SE` bar exists.
  The players, riders, bikes and AI riders in the log play no part: the first fault is the bar.
- [x] 2026-11-11: reproduced on f2dd2d4b, two ways (commands below): online (a hidden-desktop host
  game and nettool bots in a local server's P2P room, MOSH PIT, 3 players, riders Mai, Kineticlops
  and Cosmo) and offline (the state trace's race recipe with `DHD_RACE_SETUP=23`, headless, no
  server). Both fault at `0x12A0020D` in the same functions as the reporter's log.
- [x] 2026-11-11: the fix (d2a66a50, narrowed in afc8311e): `port/patches.py` patches
  `func_00182330`'s Single Event path: track 23 returns `LMOSH` (`D_0029CCD8`), the arena's own
  bar, instead of building `LMOSSE`.
- [x] 2026-11-11: the test (2a52fa5a): `port/tools/room_tracks_check.py`, every room track through
  the real load code headless; two test switches for the online runs: `DHD_NET_NO_AI=1` (a
  scripted host's room with computer riders off) and nettool `--bike A,B,C`.
- [x] 2026-11-11: the neighbours, online and offline (tables below).

## Decisions

- **The arena's bar is `LMOSH`.** It is what the game itself shows while the Mosh Pit loads (mode
  15), the same format as the other tracks' Single Event bars (`LALPSE` and the rest: an RTX of
  132336 bytes); `LMOSHSE` is a 1136-byte card of the kind the Single Event menu shows.
- **Only the Single Event name changes.** A first version (d2a66a50) returned `LMOSH` for track
  23 before any mode check; that would also have changed modes whose bar falls through to
  `LOADBAR1`, a file the disc has. afc8311e moved the check into the `single:` path, so every case
  it changes is one that loaded a missing file and faulted. No working path, online or offline,
  changes: retail never reaches the arena in Single Event (`func_001E3810` skips 0x17, and no
  career list has it).
- **A plain patch, not online-only.** The changed case faults wherever it is reached (online, or
  offline through `DHD_RACE_SETUP=23`), so gating it on `online_mode()` would only keep a crash.
- **The test runs the game, not a copy of the rule.** A Python mirror of `func_00182330` checked
  against the disc's file list would pass whenever someone forgot to mirror a change.
  `room_tracks_check.py` drives the same load code an online race uses, headless and
  deterministic, and it fails on f2dd2d4b. It is a tool (about two minutes a track), not a
  `check.py` step.
- **P2P rooms for the online runs.** A local server's SFU rooms need the Cloudflare Realtime
  credentials in `online/server/.dev.vars`, which this worktree does not have (a bot was dropped
  with `Realtime connection failed`). The reporter's room was an SFU one; the bar is loaded before
  any race frame, by the same code in both kinds of room.
- **No protocol or server change.** The fix is in the core, so the core id changes, as with any
  core fix: players need the release with it to share rooms.

## Surprises

- The issue says 2 or 3 players; the player count does not matter. One offline rider faults the
  same way, at the same address (the run is deterministic, so the stale buffer is the same).
- **An online MOSH PIT race has no finish.** On the arena in race mode no rider ever finishes:
  `DHD_NET_TEST_FINISH` finishes a scripted host or guest at the given second on ALPINE (control:
  both at 0:30.03) but never on MOSH, and in a 200 s run no AI rider finished. The race ends only
  when the server closes it (a nettool bot's fake finish starts the room's finish window; real
  players cannot), when the host ends it for everyone, or when the players go back to the room.
  This predates the crash (finding 128 already calls MOSH "not a race") and is unchanged here.
  The game's own arena mode, Mosh Bowl (mode 15), is a timed contest with its own rules
  (`func_001B7198` sets a 7200-tick, two-minute timer, `D_004B1F40[0] = 0x1C20`, and many
  functions branch on mode 15). Racing it in that mode online, or ending the arena's race some
  other way, is a design decision for the owner and is not part of this fix.

## The commands

Before (f2dd2d4b) and after, same commands; a local server per run (`port/tools/local_server.py`'s
`running()`), games only on a hidden desktop (`port/tools/hidden_game.py`), untimed game lock:

- Offline, deterministic: `python port/tools/state_trace_run.py race --name mosh --fields 4500
  --env DHD_RACE_SETUP=23`. Before: `run incomplete: expected 4500 rows, got 1348`, exit
  0xC0000005, `host64 memory: read of uncommitted guest address 0x012a0020d`. After: 9000 fields,
  racing (phase 3, race clock moving).
- `python port/tools/room_tracks_check.py --tracks 23,0 --exe <f2dd2d4b's dhd64.exe>`:
  `track 23: FAIL: guest memory fault ... 0x012a0020d`, `track 0: ok`.
- Online: a lane script (not committed) started the host game hidden with `DHD_NET_CREATE=p2p
  DHD_NET_SCRIPTED=1 DHD_NET_TRACK=23 DHD_NET_PLAYERS=3 DHD_NET_RIDER=1 DHD_RACE_PAD=0:600:cross
  DHD_NET_TEST_FINISH=40`, then nettool bots
  `nettool --players 3 --pick 16 --rider 15 --races 1 --ready-after 1 --echo-ai --finish-after 50
  join <code>` (Kineticlops) and `--pick 1 --rider 0` (Cosmo); with `--guest`, a second hidden
  game with `DHD_NET_JOIN=<code>`. Before: `online: slot 5 (AI 5) rides Kalolo ...`, then
  `host64 memory: read of uncommitted guest address 0x012a0020d` in `func_00218E88` <-
  `func_00182838` <- `func_001B7198`, as in the reporter's log.

## Result

The patch makes the arena's Single Event bar `LMOSH`; online MOSH PIT races start and run to the
results with every player count the room allows, computer riders on and off, a high rider id
(Kineticlops, 15) and a high bike id (34 21 6, the reporter's player's). No other room track was
affected: every other `L<short>SE` bar is on the disc, and all 25 load and race.

Offline, `python port/tools/room_tracks_check.py` (headless, all 25 room tracks): on the first
form of the patch `25 of 25 tracks load and race`; on afc8311e `--tracks 23,17`: both ok. On
f2dd2d4b track 23 fails (above).

Online (P2P room, local server, a hidden host game and nettool bots; "guest" is a second hidden
game). Rows marked * ran on afc8311e's build, the others on the first form of the patch (the same
Single Event path, the only one these runs take):

| Track | Players | Computer riders | Riders, bikes | Result |
|---|---|---|---|---|
| 23 MOSH PIT | 3 (host, 2 bots) | on | Mai, Kineticlops (15), Cosmo | results |
| 23 MOSH PIT | 2 | on | Kineticlops, bike 34 21 6 | results |
| 23 MOSH PIT | 2 | off | Kineticlops, bike 34 21 6 | results |
| 23 MOSH PIT | 3 (host, guest, bot) | on | Kineticlops, bike 34 21 6 | results on both games |
| 23 MOSH PIT * | 3 (host, guest, bot) | on | Kineticlops, bike 34 21 6 | results on both games |
| 23 MOSH PIT | 3 (host, guest, bot) | off | Kineticlops | results on both games |
| 23 MOSH PIT | 6 (its limit) | on | Kineticlops, Cosmo, Ajax, Jonnie, Kalolo | results |
| 23 MOSH PIT | 6 | off | the same, bike 34 21 6 | results |
| 23 MOSH PIT * | 6 | off | Kineticlops, Cosmo, Ajax, Jonnie, Kalolo | results |
| 17 Salt City MX | 2 | on | Kineticlops, bike 34 21 6 | results, all finished |
| 17 Salt City MX | 6 (its limit) | off | five bots | results, all finished |
| 18 Salt City TD | 2 | off | Kineticlops | results, all finished |
| 18 Salt City TD | 8 (its limit) | on | seven bots | results, all finished |
| 25 BC TDX | 6 (its limit) | on | five bots, bike 34 21 6 | results, all finished |
| 22 GLACIER T | 8 (its limit) | off | seven bots | results, all finished |
| 0 ALPINE (control) | 3 (host, guest, bot) | on | Kineticlops, bike 34 21 6 | results, all finished |

On MOSH the results came by the room's finish window after the bots' finishes; the games' own
riders did not finish (Surprises). Not run: an SFU room (no Realtime credentials here), more than
one game per PC beyond host and guest, 10-player tracks at 10 (nothing there changed), Android
and the browser (the same core).

Left for the lead:

- Boot and race state traces against f2dd2d4b (`state_trace_run.py boot` and `race`, and
  `race1`-`race5`): they must be equal. The patch changes only the arena's Single Event bar, which
  no offline route reaches.
- A real two-machine MOSH PIT race (an SFU room, the reporter's kind) from the room's menus.
- The owner's decision on how an online MOSH PIT race should end (Surprises).
