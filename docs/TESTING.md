# Testing the port

How to build, run and check the game from a script: the tools, the switches the game reads for
tests, and the traps that have cost time. Building from scratch: `docs/BUILDING.md`.

## Setup once

Set these for your user (Windows: System Properties > Environment Variables; elsewhere your shell
profile). Tools read them; the game reads none of them.

| Variable | What |
|---|---|
| `DHD_DEVCACHES` | downloaded toolchains and caches: wasi-sdk, WABT, SIMDe, binaryen, the object and cargo caches (default `~/DevCaches`) |
| `DHD_SCRATCH` | scratch folder for test runs, server state and temporary files (default the system temp) |
| `DHD_TEST_ISO` | your Downhill Domination (USA) disc image, for the test tools |

Never set `DHD_ISO` for your user: the game reads it, and it overrides dhd.ini in every game
started from that environment, your own included. Keep all three off a small system drive.

A git worktree lacks the files that are never committed (asm/, assets/, iso/SCUS_971.77):
`python tools/worktree_setup.py` copies them from the main checkout and, for a worktree on the
system drive, moves build/ and node_modules/ to `DHD_SCRATCH` by junctions.
`tools/worktree_setup.py --list` shows every worktree, merged or not.

## Build

    python port/tools/build.py                 # the core (build_wasm.py), then dhd64.exe (build_host64.py)
    python port/tools/build.py --package       # the release zip
    python port/tools/build.py --web           # the browser build
    python port/tools/build.py --no-core       # the game again from the current core

The core id (`build_wasm.py` prints `core id: ...`) is what online rooms compare: two games share
rooms only when their cores match. dhd.log's first line has the executable's own hash, not the core
id. nettool sends the `DHD_VERSION` it was compiled with: rebuild it (`port/net/build_tests.py`)
after a version change before a live check.

## Tools

| Tool | For |
|---|---|
| `port/tools/local_server.py -- CMD` | the room server of this checkout on a free port for CMD's length (sets `DHD_NET_SERVER`); fresh state each run; stops workerd too |
| `port/tools/live_game.py OUT --script FILE` | a game on a hidden desktop run by commands (keys, waits, shots, nettool bots) |
| `port/tools/hidden_game.py` | the hidden-desktop game itself (Python API: keys, mouse, shots) |
| `port/tools/game_lock.py` | the one-game-at-a-time lock every timed or watched run takes; frees itself when its holder is gone |
| `port/tools/cross_race.py` | the release check for online changes: a PC<->Mac room race, run once with each side hosting |
| `port/tools/online_race.py` | two local games through a server, optionally with nettool bots |
| `port/net/build_tests.py` | the network client's unit and loopback tests; builds nettool |
| `online/server`: `npm test` | the server's own tests |
| `port/tools/state_trace_run.py` | the state trace, the check for changes that must not change the game's behaviour |

A typical in-game check:

    python port/tools/local_server.py -- python port/tools/live_game.py build/checks/room1 --script check1.txt DHD_LAUNCHER=1

with check1.txt like

    wait online_menu:_room_\w{6}_\(host\) 150
    bot ANNA --pick 3 --ready-after 3 --races 1 --finish-after 30
    wait back_to_the_room_after 400
    shot room
    quit

## Switches the game reads for tests

Menus and riding:

| Variable | What |
|---|---|
| `DHD_TEST_INPUT=1` | keys posted to the window count without focus (hidden-desktop runs) |
| `DHD_NAV` | menu script by the screen shown, `field@ADDR=value:button,...` (autopad.c) |
| `DHD_AUTOPAD_FRAMES` | button script by field count, `field:length:button,...` |
| `DHD_AUTOPAD` | button script by seconds |
| `DHD_RACE_PAD` | in-race script by race clock seconds, `start:length:button,...` |
| `DHD_RACE_SYNC=1` | race scripts run on the race clock |
| `DHD_FRAME_CLOCK=1` | the game clock from field counts, not the wall clock (repeatable runs) |
| `DHD_HEADLESS=1` | no message boxes (a bad disc only logs), and no launcher (Linux smoke runs) |

Online:

| Variable | What |
|---|---|
| `DHD_NET_SERVER` | the server (`ws://127.0.0.1:<port>` for a local one) |
| `DHD_NET_NAME` | the player name |
| `DHD_NET_ISO_SHA256` | the disc's files hash the server compares (online_race.iso_files_sha256) |
| `DHD_NET_SCRIPTED=1` | the online flow takes its input from the scripts above, not the pad |
| `DHD_NET_TEST_READY=1` | (with SCRIPTED) presses READY in the room after every race |
| `DHD_NET_TEST_FINISH=S` | this PC's rider finishes at race second S |
| `DHD_NET_TEST_TRACK=N` | (with SCRIPTED) a host's room starts on game track N |
| `DHD_NET_TEST_SKIP=S` | presses START S seconds into each flight over the track, as a player skipping it |
| `DHD_NET_AFTER_RACE` | (with SCRIPTED) the menu script after a race |
| `DHD_ONLINE_MENU_TRACE=1` | logs every online menu page and its rows |
| `DHD_PUPPET_CAMERA=1` | camera 0 follows a puppet rider (the spectating camera) |

Pictures:

| Variable | What |
|---|---|
| `DHD_SCREENSHOT=PREFIX` | GS captures (BMP); `DHD_CAPTURE_AT`, `DHD_CAPTURE_INTERVAL`, `DHD_SCREENSHOT_WINDOW` |
| `DHD_RACE_CAPTURE=c+120` | a capture at a race clock moment (`DHD_RACE_CAPTURE_WINDOW`, `_FROM`, `_INTERVAL`) |
| `DHD_CAPTURE_PRESENTED=PREFIX` | PNGs of the presented picture, also while minimised |
| `DHD_PERF_COUNTER=1` | the FPS & PING counter |

Plain settings for runs: `DHD_SKIP_MOVIES=1`, `DHD_SND_OFF=1`, `DHD_SETTINGS=none`, `DHD_MEMCARD=0|1`
with `DHD_MEMCARD_DIR`, `DHD_SCALE`, `DHD_FPS`, `DHD_WINDOW*`, `DHD_RENDER=native`,
`DHD_LAUNCHER=1` (a release `dhd.exe` starts without opening the launcher).

## Traps

- **Your pad drives a hidden game.** SDL reads joysticks in the background: set
  `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=0` (hidden_game does). `DHD_SDL=0` also kills the test keys.
- **Any `DHD_SCREENSHOT`, even empty, turns threaded graphics off**: the game runs at a fraction of
  its speed at high scales. For pictures as players see them use `DHD_SCREENSHOT=<unset>` (hidden_game
  drops `<unset>` variables) with `DHD_CAPTURE_PRESENTED`. Without GS captures there are no field
  counts in the log, so `field` and `shot` in live_game don't work: drive by `DHD_AUTOPAD_FRAMES`.
- **Posted keys don't reach the room under `DHD_NET_SCRIPTED`.** Use the scripted hooks
  (`DHD_NET_TEST_TRACK`, `DHD_NET_TEST_READY`) instead.
- **`wait REGEX` matches the whole log**, earlier lines too: wait for something new.
- **A release `dhd.exe` opens the launcher** unless `DHD_LAUNCHER=1` or a script is set.
- **`DHD_MEMCARD=0` stops a release boot at "memory card not found"** on some paths: use
  `DHD_MEMCARD=1` with an empty `DHD_MEMCARD_DIR`.
- **A background `wrangler dev` outlives its shell** and keeps the port: use local_server.py.
- **nettool bots report place = slot + 1**, so results with bots show odd places.
- **Single-PC online tests miss cross-platform bugs** (the version string, POSIX putenv, pacing,
  all shipped once): every release with online or pacing changes runs cross_race.py both ways, and
  a `freeze:` line counts as a failure.
- **Never wait for a process by matching its command line in a loop whose own command line
  contains the same text.**
