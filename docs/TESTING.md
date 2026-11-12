# Testing the port

How to build, run and check the game from a script: the tools, the switches the game reads for
tests, and the traps that have cost time. Building from scratch: `docs/BUILDING.md`.

## Setup once

Set these for your user (Windows: System Properties > Environment Variables; elsewhere your shell
profile). Tools read them; the game reads none of them.

| Variable | What |
|---|---|
| `DHD_DEVCACHES` | downloaded toolchains and caches: wasi-sdk, WABT, SIMDe, binaryen, the object and cargo caches (default `~/DevCaches`) |
| `DHD_MODULE_CACHE` | `0`: build the core module even when `DHD_DEVCACHES/dhd-modulecache` holds one built from the same inputs (BUILDING.md) |
| `DHD_BUILD_SLOTS` | at most this many core or host builds at once on the machine (several worktrees); unset: no limit |
| `DHD_SCRATCH` | scratch folder for test runs, server state and temporary files (default the system temp) |
| `DHD_GAME_SLOTS` | at most this many untimed game checks at once on the PC (the game lock, below); default 1 |
| `DHD_TEST_ISO` | your Downhill Domination (USA) disc image, for the test tools |
| `DHD_MALIOC` | optional: Arm's Mali Offline Compiler (`malioc.exe` from Arm Performance Studio, free; keep it off the system drive), for the `native_spirv` test below |

Never set `DHD_ISO` for your user: the game reads it, and it overrides dhd.ini in every game
started from that environment, your own included. Keep all three off a small system drive.

A git worktree lacks the files that are never committed (asm/, assets/, iso/SCUS_971.77):
`python tools/worktree_setup.py` copies them from the main checkout and, for a worktree on the
system drive, moves build/ and node_modules/ to `DHD_SCRATCH` by junctions. There it also installs
`online/server` and `port/launcher` (`npm ci` in the junction's target, a folder named
node_modules, as wrangler needs; again whenever the lock changes, or with `--npm`). Never run
`npm ci` in such a worktree yourself: it deletes the junction and installs onto the system drive;
run worktree_setup.py again instead. `tools/worktree_setup.py --list` shows every worktree, merged
or not. After `git worktree remove`, `tools/worktree_setup.py --remove PATH` deletes its scratch
folders and the junctions git leaves behind (only the links: `tools/compilers` points into the
main checkout).

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
| `port/tools/hidden_game.py` | the hidden-desktop game itself (Python API: keys, mouse, shots, PrintWindow captures) |
| `port/tools/game_lock.py` | the PC's game lock (below): prints its holders, or `free` |
| `port/tools/cross_race.py` | the release check for online changes: a PC<->Mac room race, run once with each side hosting; `--backend p2p` in a P2P room (the guest joins by code), `--force-turn` with direct UDP off (an SFU room's TURN relay, a P2P room's SFU fallback) |
| `port/tools/p2p_crowd.py OUT` | a crowded P2P room (the 6+ racer setup check for P2P): this PC's game makes it (the hub), `--bots N` nettool bots join (`--sfu-bots K` of them with direct links off: they race over the SFU fallback, which needs `.dev.vars`), the Mac too with `DHD_REMOTE*`; setup time, the host's CPU and upload per peer, each peer's path, who raced over the SFU, snapshot age per sender (summary.json) |
| `port/tools/online_race.py` | two local games through a server, optionally with nettool bots; `--lag` measures the puppets (below); `--hidden` runs both on the hidden desktop; `--link` puts the STATE each receives through a test link (`DHD_NET_LINK`) |
| `port/tools/mosh_round_check.py OUT` | the arena's scored round online (finding 204): two games in a P2P room from the menus, on hidden desktops. The default `--scenario hit` has no computer riders, both riders braking at the gates and the guest punching and kicking: the guest must score on the host's rider, with the same finals, table and room results on both games (finding 206). `--scenario ride` pedals and attacks with computer riders (`--bots N`, `--then next`, `--no-ai`); `--offline` is the game's own Mosh Bowl |
| `port/tools/race_menu_check.py OUT --scenario S` | the race menu online (finding 215): two games in a room from the menus (P2P, `--sfu` a private SFU room) open the menu over the running race with `DHD_NET_TEST_MENU`; scenarios `resume`, `room` (BACK TO THE ROOM), `end` (END THE RACE), `leave` (LEAVE THE ROOM), `closed` (the race ends under an open menu), `alone` (the other human finished: the game's own pause), `timeup`, `timeup-host` and `finished` (why the race closed, finding 211: the log's `race closed: back to the room (WHY: ...)` line on each game); `--track`, `--link`, `--scale` and `--interval` for pictures, `--host-env` / `--guest-env`; `--lag` traces and records both games and scores the race both play as `online_race.py --lag` does (finding 216: e.g. `--scenario finished --lag --link delay=80` for a race's last seconds) |
| `port/tools/settings_touch_check.py OUT [--exe EXE]` | OPTIONS > SETTINGS with touch controls on at a phone's 20:9 (`DHD_TOUCH_MOUSE=1`, no controller seen): the legend must say "Tap a tab", and a posted tap on a tab's name must change the tab; pictures `settings.png`, `tapped.png` (finding 211) |
| `port/tools/attract_skip_check.py OUT [--exe EXE]` | the title's attract demo with touch controls on at a phone's 20:9 (`DHD_TEST_ATTRACT_AFTER=5`): SKIP must show over it and a tap must end it; pictures `demo-skip.png`, `after-skip.png` of the presented picture; exit 0 when the demo ended (finding 218) |
| `port/tools/room_ready_touch_check.py OUT [--exe EXE] [--race]` | READY by touch in a P2P room on a local server: a scripted host makes the room and readies, a guest with touch controls at a phone's 20:9 joins by code and taps READY (no script Cross in the room: `cross_race`'s guest nav ends with one that readies by itself); the guest must log `READY on by tap` and the room's answer, and the race must start (finding 218) |
| `port/tools/online_riders.py` | the race pads the online tools give their riders (`rider_env`: `DHD_RACE_PAD` with `DHD_RACE_SYNC=1`) |
| `port/tools/puppet_bench.py` | the puppet bench (finding 202, "The puppet bench" below): recorded riders replayed onto puppets through a seeded bad link, scored against the truth (distance, wheels in the ground, jumps and jolts, time frozen) |
| `port/tools/puppet_lag.py RUN [--riders]` | from an `online_race.py --lag` run: how far behind real time and how far from the real rider each game draws the other, and how often it jerks (finding 169); `--riders`: every rider (the other player's and the AI riders) with the bench's scores, and the re-syncs in the first 5 s after GO, later and after crashes (finding 202; `online_race.py --lag` prints it), then every rider from the room's end of the race to the race screen's close (finding 216, `end_report`) |
| `port/net/build_tests.py` | the network client's unit and loopback tests; builds nettool (`nettool --p2p host`, or `join` with a 7-character code: a P2P room's bot; `--rider N --bike A,B,C`: the rider and bike it says it rides) |
| `port/android/test_disc_image.py [ISO]` | the Android side's plain-JVM tests: the first start's disc rules (the check, a complete copy) on synthetic images, and the update notice's page rule (finding 143) |
| `port/android/build_apk.py --check APK` | a built APK checked as a release one: one signer, the release key's certificate (finding 143) |
| `port/android/test_build_apk.py` | that release check's rules with apksigner and aapt answers canned (no SDK or APK needed) |
| `online/server`: `npm test` | the server's own tests |
| `port/tools/state_trace_run.py` | the state trace, the check for changes that must not change the game's behaviour (below) |
| `port/tools/state_trace_layout_test.py` | the state trace's own test (finding 205): `flips` (a rider word, a RAM word and a register changed in a run must be reported), `build-pad` then `layout` (a build whose core layout moved must give equal traces) |
| `port/tools/options_walk.py` | every OPTIONS row opens and closes without a fault (finding 207: POSTERS closed the game): one hidden-desktop game per scenario (Unlock All on or off; a fresh card or a save that owns POSTERS and PICKUP EDITOR), each row's picture, the log free of a guest memory fault, and the OPTIONS list back after Triangle; `--scenario`, `--rows 5`, `--scale 4` for pictures, `--exe` another build (about six minutes for the four scenarios) |
| `port/tools/stack_escapes.py` | stack locals the EE frame makes one object (finding 213, the class of 094 and 207): every place a game function passes a local's address on, against the local's size and the EE frame (the matching compiler's `-gstabs` frames, a MIPS dataflow of what each callee writes and reads through its pointer arguments), and every `sprintf` whose format is longer than its local. `--check` (in `check.py port`) fails on a write overrun not in `port/tools/stack_escapes_reviewed.txt`; `--report FILE` lists every candidate, `--function NAME` explains one (frame, summaries, the callees that make a range); needs the matching toolchain; about 90 s the first time, 8 s after (cached in build/stack_escapes) |
| `port/tools/redzone_walk.py` | the run-time side of finding 213: `--build` makes a game with red zones around every address-taken local of the game's C code (`DHD_CORE_REDZONE=1`, `port/tools/redzone.py`: 16 bytes of 0xCD before, between and after them, checked when the function returns; a hit is a `redzone: FUNCTION: bytes a..b of the 16 after LOCAL (N bytes) written` line in dhd.log) into build/redzone/ and the players' build again; without it, the walk: the state-trace recipes and the OPTIONS walk on that game, then every hit once (about 40 minutes; `--recipes`, `--no-options`) |
| `port/tools/room_tracks_check.py` | every track an online room offers loads and races: the state trace's race recipe with `DHD_RACE_SETUP=<track>` (headless, about two minutes a track); `--tracks 23,17` picks some (finding 203) |
| `port/tools/test_scenery_rate.py` | the same race at 60, 144 and 240 (deterministic, headless): the helicopters', birds' and swooping attacker's speed in game time must match 60's (`DHD_SCENERY_TRACE`, finding 193); `--row N` / `--right N` pick the track (the chooser moved right N times; `--right` with every track unlocked) |
| `port/tools/rate_audit.py` | words of EE memory that move once per update instead of once per field, from a 60 and a high-rate run (finding 090, FPS1); `--row`, `--skip`, `--samples` pick the race and its window |
| `port/tools/route_profile.py EXE OUT [--profile]` | finding 156's route on a hidden desktop (timed lock) with the timeline: per scene (title, shop, race start, mid-race) fields/s and each thread's work and busy time; `--profile` also samples the game thread's call stacks in each scene (finding 162) |
| `port/tools/native_vu_pictures.py capture EXE OUT [--scale N] [--env DHD_NATIVE_VU1=on]`, `compare A B [--same]` | Native VU's picture check (finding 194): finding 156's route with the native pictures of eleven fields (exact Z probe, so every run's game state is the same), then two runs field by field against the native lane's tolerance (MAE <= 0.1, <= 0.1 % of pixels off by more than 8) or identical |
| `port/tools/gs_census.py DUMP.gs... [--group NAME=A-B] [--log game.log]` | what a GS stream uses, per field and group of dumps: primitives by type and path, textures and CLUTs, filters, blending, tests, render targets, render-to-texture, transfers, the display; reads the game's `DHD_GS_CAPTURE` dumps or PCSX2's uncompressed ones (finding 162) |
| `port/tools/sample_profile.py --pid N --stacks game` | whole call stacks of one thread's samples (dbghelp's StackWalk64): the report's `stacks` and `inclusive`; slower per sample, so shares only (finding 162) |
| `port/tools/vu1_seq_diff.py CAPTURE...` | the native VU1 kernels and the translated blocks against the interpreter, on VU1 runs captured from a game (`DHD_VU_SEQ=DIR DHD_VU_SEQ_AT=<game seconds> DHD_VU_SEQ_KICKS=N`, under build/): every register, the data memory and each kicked packet must be identical. `--specials SEED` puts exponent-255 and denormal values into registers and memory first, `--clean-check` compares every value a kernel writes as clean with its clamp, `--sticky-cycle` cycles the entry's sticky flags, `--negative` must fail; `--repeat N` times each microprogram key. `--target android-arm64` builds it for a phone: the arm64 code against the interpreter there, and CPU cycles per key (finding 197) |
| `port/tools/replay_check.py` | a race and its replay, headless (`--fps N`, `--paced --slow MS,JITTER`): every rider's position each field; the replay must be the race (finding 142) |
| `port/tools/audio_race_compare.py` | one race's sound in PCSX2 (an SPU2 capture hook) and in the port, compared by level and by voice (finding 133) |
| `port/tools/rustgs.py test --target x86_64-pc-windows-msvc -- --test native_spirv` | the native and the exact renderer's SPIR-V for every pipeline stage (raster.wgsl for every raster key too), made as wgpu-hal's Vulkan backend makes it, with no texture passed into a function (Mali's r54 compiler crashes on that, finding 153) and none of the forms LLVM 9 turns into `llvm.usub.sat`, `llvm.uadd.sat` or a funnel shift (PowerVR Rogue's driver 1.13 crashes on them, finding 170). `DHD_SPIRV_OUT=DIR` keeps the `.spv` files; with `DHD_MALIOC` every stage is also compiled by malioc for `DHD_MALIOC_CORE` (default `Mali-G57`); `DHD_NATIVE_WGSL=FILE` (`DHD_RASTER_WGSL=FILE`) compares another native.wgsl (raster.wgsl) |

## The state trace

The gate for a change that must not change the game (finding 081, the trace as it is since 205):
the same recipe on the base build and on the changed one, headless and deterministic, then

    python port/tools/state_trace_run.py race3 --name base-race3 --exe BASE/dhd64.exe
    python port/tools/state_trace_run.py race3 --name new-race3
    python port/tools/state_diff.py build/state-trace/base-race3.tsv build/state-trace/new-race3.tsv

`state_diff.py` must print PASS: every row equal. Recipes: `boot` (the menus, 600 fields),
`race` (the first track with the steering pad, 5000 fields), `race2` to `race6` (the other five
unlocked tracks, the full-pedal pad, 14000 fields each; there is no `race1`, `race` is the first
track), `split` (a two-player race) and `memcard` (save, then load). A change that can touch game
state runs at least `boot` and `race`; a change to code every track runs (loading, riders, a track's
own patch) runs `race2` to `race6` too. Both traces must come from the same trace format: the first
line says `state-trace 2`; a trace of format 1 (before finding 205) does not compare with it, so
regenerate a stored reference with its own executable. A stored reference (a release's traces kept
outside the repository to diff against) is valid only when it is of format 2 and was made with
the same `state_trace_codeptrs.txt` and `state_trace_mask.txt` as the run it is compared with;
keep a note of the commit whose lists made it beside the traces, and when either list changes make
new references (the old folder is then superseded, never mixed with the new one). The same holds
when a merged change alters the game state on purpose (finding 213 put the EE's stack objects
back, which changed every race): once the differences are explained and checked against PCSX2,
new references are made from the merged build and the old folder is superseded for the recipes
that changed; its README says so and names the new folder. `split` and
`memcard` still compare only builds with the same core layout (finding 205's Result).

The trace compares game state, not the core's layout: a register lane that holds a value in the
core's ranges is compared by raw value or by what it points at (the `regptrs` column), and nothing
the HLE gives the game points into the core's own data. A difference is therefore a difference
of the game; `state_diff.py --dumps-a/--dumps-b` with `--dump FIELD` runs names the words.
A change to the trace itself or to its lists (`state_trace_codeptrs.txt`, `state_trace_mask.txt`)
runs `state_trace_layout_test.py flips` and `build-pad` then `layout`: a trace that misses a real
change, or that differs only because the core's layout moved, fails there. The lists are part of
the trace: two traces compare only when both runs used the same lists.

A typical in-game check:

    python port/tools/local_server.py -- python port/tools/live_game.py build/checks/room1 --script check1.txt DHD_LAUNCHER=1

with check1.txt like

    wait online_menu:_room_\w{6}_\(host\) 150
    bot ANNA --pick 3 --ready-after 3 --races 1 --finish-after 30
    wait back_to_the_room_after 400
    shot room
    quit

## The puppet bench

How other riders look in an online race, measured on one PC with no network (finding 202). The
truth is a deterministic race with every rider recorded at full rate; the same race is run again
with every AI slot a puppet of its recorded rider, sent through a seeded bad link
(`port/hle/puppet_link.h`), and the puppets as drawn are scored against the truth.

    python port/tools/puppet_bench.py record --track 1                     # rows 0-2: Italy FR, MX, TD
    python port/tools/puppet_bench.py --label mine play build/puppet-bench/truth-t1/truth.bin \
        --link "delay=150,jitter=30,loss=2,reorder=2,seed=3" --fps 60      # prints the scores
    python port/tools/puppet_bench.py --label mine matrix --tracks 0,1,2   # every scenario: results.md
    python port/tools/puppet_bench.py --label mine matrix --rescore        # score saved traces again
    python port/tools/puppet_bench.py clip --track 1 --slot 2 --at 22 --link "delay=150,jitter=30" \
        --name mx-150 --out OUTSIDE_THE_REPO                               # truth | puppet, 4x, mp4

Every game runs on the hidden desktop at the frame clock (about 7x real time), untimed: the
numbers are positions on the race clock, not speed. `--env DHD_PUPPET_MAP=0:1` replays the
scripted human rider instead of the AI riders; `--exe` scores another build on the same truth
(the race is the same in every run). What each column means: `puppet_bench.py`'s docstring.

## The game lock

Every tool that starts the game for a check takes `port/tools/game_lock.py`'s lock, so lanes on one
PC don't disturb each other's runs:

- **Untimed** checks (nothing measured: captures, logic, online tests): `hidden_game.py` and so
  `live_game.py`, `state_trace_run.py`, `rate_audit.py` and `test_scenery_rate.py` (virtual
  clock, deterministic), `online_race.py` (one place for its two games). Up to `DHD_GAME_SLOTS`
  of them run at once (default 1: one game at a time).
- **Timed** runs (fps, frame times, CPU or GPU time): `bench_race.py`, `stutter_race.py`,
  `sample_profile.py --run`, and `hidden_game.Game(..., timed=True)`. A timed run has the PC to
  itself: it waits until every untimed check is done, and no new one starts while it waits or runs.
  Timed runs queue for each other.
- A holder that crashes or is killed frees its place at once (operating-system file locks in
  `DHD_SCRATCH/dhd-workers/game-slots`). `python port/tools/game_lock.py` lists the holders.
- Set `DHD_GAME_SLOTS` once for your user, the same for every lane: each holder counts against its
  own value. `port/tools/test_game_lock.py` tests the rules.

## Switches the game reads for tests

Menus and riding:

| Variable | What |
|---|---|
| `DHD_TEST_INPUT=1` | keys posted to the window count without focus (hidden-desktop runs) |
| `DHD_NAV` | menu script by the screen shown, `field@ADDR=value:button,...` (autopad.c) |
| `DHD_AUTOPAD_FRAMES` | button script by field count, `field:length:button,...` |
| `DHD_AUTOPAD` | button script by seconds |
| `DHD_TOUCH_WITH_SCRIPT=1` | the touch controls' buttons are merged into player 1's pad also when `DHD_NAV` / `DHD_AUTOPAD*` drive it, so a `DHD_NAV` route can end at a screen and touches (`DHD_TOUCH_MOUSE=1`) take it from there (finding 209) |
| `DHD_TOUCH_SCRIPT` | the touch controls' fingers by the race clock while the race HUD shows, `S:down\|move\|up:ID:X:Y;...` (S race seconds, X and Y in mm of the touch screen): the same events a screen gives, so a thumb can be held, dragged and two fingers can touch at once, which posted mouse messages on a hidden desktop can't (finding 214); needs `DHD_TOUCH_MOUSE=1` |
| `DHD_TEST_ATTRACT_AFTER=S` | the title's attract demo starts after S seconds idle (under 60) instead of the game's minute (finding 218) |
| `DHD_RACE_PAD` | in-race script by race clock seconds, `start:length:button,...`; does nothing without `DHD_RACE_SYNC=1` and a menu script (below) |
| `DHD_RACE_SYNC=1` | race scripts run on the race clock; without it `DHD_RACE_PAD` gives the rider no input (online tools: `online_riders.rider_env`, finding 206) |
| `DHD_KEYS` | (race clock) keys through the keyboard's layout, `start:length:key[+key],...` with dhd.ini's key names (`space`, `j`, `kp1`, ...) or `esc` |
| `DHD_TEST_PAD`, `DHD_TEST_PAD2` | (race clock) player 1's or 2's controller by its inputs' places, `start:length:input[+input],...` (`south`, `east`, `lt`, `dpad_up`, `lx=-50`, ...), through that player's layout (`pad1`, `pad2`) as a real pad's; logged as `test pad N` (finding 157) |
| `DHD_REPLAY_TEST=S` | at race second S the race's replay starts, as REPLAY in the results does; `DHD_REPLAY_TRACE=path`: the riders' positions each field (finding 142) |
| `DHD_FRAME_CLOCK=1` | the game clock from field counts, not the wall clock (repeatable runs) |
| `DHD_HEADLESS=1` | no message boxes (a bad disc only logs), and no launcher (Linux smoke runs) |

Online:

| Variable | What |
|---|---|
| `DHD_NET_SERVER` | the server (`ws://127.0.0.1:<port>` for a local one) |
| `DHD_NET_CREATE=1\|p2p`, `DHD_NET_JOIN=CODE` | a race outside the menus: make an SFU (`1`) or P2P (`p2p`) room, or join one (a 7-character code is a P2P room's) |
| `DHD_NET_NAME` | the player name |
| `DHD_NET_ISO_SHA256` | the disc's files hash the server compares (online_race.iso_files_sha256) |
| `DHD_NET_SCRIPTED=1` | the online flow takes its input from the scripts above, not the pad |
| `DHD_NET_TEST_READY=1` | (with SCRIPTED) presses READY in the room after every race |
| `DHD_NET_TEST_FINISH=S` | this PC's rider finishes at race second S |
| `DHD_NET_NO_AI=1` | (with `DHD_NET_CREATE`, or `DHD_NET_SCRIPTED` for a host's room from the menus) the room's computer riders off |
| `DHD_NET_TEST_TRACK=N` | (with SCRIPTED) a host's room starts on game track N |
| `DHD_NET_TEST_CODE=CODE` | (with SCRIPTED) JOIN WITH A CODE starts with this code typed (cross_race.py's P2P guest) |
| `DHD_NET_TEST_THEN=same\|next\|random` | (with SCRIPTED) a host's room's Then rule (the track after each race) |
| `DHD_NET_TEST_SHOP=1` | (with SCRIPTED) the shop waits for the pads; without it a scripted run presses START there at once |
| `DHD_NET_TEST_SKIP=S` | presses START S seconds into each flight over the track, as a player skipping it |
| `DHD_NET_AFTER_RACE` | (with SCRIPTED) the menu script after a race |
| `DHD_ONLINE_MENU_TRACE=1` | logs every online menu page and its rows |
| `DHD_PUPPET_CAMERA=1` | camera 0 follows a puppet rider (the spectating camera; `DHD_PUPPET_SLOT` picks it, offline too) |
| `DHD_PUPPET_RECORD=file` | each race step, rider `DHD_PUPPET_FROM` (default 0; `all`: every rider) with its wheels' clearance above the ground (`port/hle/puppet.c`) |
| `DHD_PUPPET_PLAY=file` | offline: the recorded riders onto puppets (`DHD_PUPPET_MAP=src:dst,...`; default every recorded AI rider onto its own slot), sent as online through `DHD_PUPPET_LINK` |
| `DHD_PUPPET_TRACE=file` | each race step, each puppet as drawn: race time, slot, position, the moment it should show, each wheel's clearance, crashing, its recorded rider |
| `DHD_NET_TEST_HITCH=S:MS` | once, at race second S, this game's thread stops MS ms (a hitch): the others must hold its riders (HOLD, finding 202) |
| `DHD_NET_TEST_MENU=S:step,...[;S:step,...]` | from race second S, presses on the wall clock (so they go on through a pause): `start`, `cross`, `circle`, `square`, `triangle`, `up`, `down`, `left`, `right` (0.1 s down, 0.1 s up) or `wN` (N ms); the script's own buttons are let go while a group runs; each race runs it again; also logs the race clock against the wall clock twice a second (finding 215) |
| `DHD_NET_LINK=link` | the STATE this game receives is held as the link says first: `delay=MS,jitter=MS,loss=PCT,burst=S:A-B,reorder=PCT,skew=MS,seed=N` (`port/hle/puppet_link.h`) |
| `DHD_NET_DROP_STATE=N` | N percent of this PC's outgoing STATE messages are dropped before they are sent |
| `DHD_NET_SFU_DROP_IN=N` | N percent of the DTLS records arriving from the SFU over UDP are dropped on arrival, a lossy link that makes Cloudflare retransmit (finding 173) |
| `DHD_NET_SFU_UDP=0` | every UDP datagram to and from the SFU is dropped: the TURN-over-TLS fallback must carry the race |
| `DHD_NET_PEER_STATS=1` | each second, a `netstats:` line per peer connection in dhd.log: its path (UDP or TURN over TLS), bytes each way and ICE round trip (p2p_crowd.py reads the host's) |
| `DHD_NET_LOG_CODE=1` | dhd.log shows a P2P room's code in full (players' logs show `AB*****`: the code lets anyone in, and so see the hub's address; finding 180); the tools that read the host's code from its log set it |
| `DHD_NET_P2P_UDP=0` | in a P2P room this game makes no direct link and says so at once, so it races over the room's SFU (the fallback, finding 186) |
| `DHD_NET_SFU_TURN=0` | the TURN fallback is never asked for; with `DHD_NET_SFU_UDP=0` a racer whose connection never comes up, whom the others must race without (finding 175) |
| `DHD_NET_TEST_DROP=S` | the room connection is cut once per room, S seconds after a race's start moment (negative: during its setup, the start message comes 3 s before), with no close frame as a network drop; the game must rejoin and race on (finding 175) |
| `DHD_NET_TEST_PONG_DELAY=N[@S]` | from S seconds (default 60) after joining a room, every pong this PC gets (the room's and the race's PONG) is held N ms plus 0-N/2 ms more before it counts: a slow return path one way, for the shared clock (finding 175) |

Pictures:

| Variable | What |
|---|---|
| `DHD_SCREENSHOT=PREFIX` | GS captures (BMP); `DHD_CAPTURE_AT`, `DHD_CAPTURE_INTERVAL`, `DHD_SCREENSHOT_WINDOW` |
| `DHD_RACE_CAPTURE=c+120` | a capture at a race clock moment (`DHD_RACE_CAPTURE_WINDOW`, `_FROM`, `_INTERVAL`) |
| `DHD_CAPTURE_PRESENTED=PREFIX` | PNGs of the presented picture, also while minimised |
| `DHD_PERF_COUNTER=1` | the FPS & PING counter |
| `DHD_PERF_LOG=0` | no `perf:` lines in dhd.log (on by default, finding 129) and none of their counting |
| `DHD_GPU_STATS=N` | every N-th picture, GPU time (and on Vulkan pixels shaded) per kind of draw in dhd.log (finding 131) |
| `DHD_TEST_DEVICE_LOST=N` | (D3D12) at the N-th presented frame this game's GPU device is removed (`ID3D12Device5::RemoveDevice`: this process only, no GPU reset); the game must log the reason and end with the lost-device box, exit code 1 (finding 171) |
| `DHD_GPU_DRED=1` | (D3D12) DRED's breadcrumbs and page faults: a lost device's report names the command lists the GPU had not finished and where they stopped (finding 171) |
| `DHD_VU1_LAYOUT=path` with `DHD_VU1_LAYOUT_FIELDS=a-b[,c-d...]` | per field window and kind of VU1 run (overlay, program, entry, MSCNT, VI13): the VIF1 commands before it (UNPACK forms, addresses, counts, masks) and the GIF tags it kicks; read only (finding 162) |
| `DHD_NATIVE_VU1=on\|off\|LOOPS[:skip\|:shadow][:gpuclip]` | Native VU (dhd.ini `native_vu`, REVIVED SETTINGS > Graphics; findings 164, 167, 194): `on` draws every translated VU1 loop with the native renderer; for tests a list of loops (`0170,1708`, groups `prelit`, `lit`, `shadow`, `all`), `:skip` or `:shadow` forcing a mode (without them: shadow under `DHD_EXACT_PROBE`, else skip), `:gpuclip` the near-bound batches too (a measurement); `DHD_NATIVE_VU1_CHECK=1` (shadow) compares each item with the VU's packet |
| `DHD_TEST_NO_MESHES=N` | the native renderer refuses the mesh draws at its N-th picture (0: at its start), as a driver refusing a mesh pipeline would: one dhd.log line, and the VU runs every program from the next batch (finding 194) |
| `DHD_NATIVE_SPEC=0\|sync` | `0`: the generic draw shader only; `sync`: specialised draw pipelines built at first use, not on a worker (replays and picture checks; finding 131) |
| `DHD_ENCODE_CUT=N` | where the native renderer cuts a large picture's command encoder, to finish its first part on a helper thread: at the first pass that begins once N percent of the picture's encoder work is behind (60 unless set; `0`: never, one encoder a picture as before, for a comparison; finding 195). On Arm's Mali GPUs never unless set: the Mali-G52 loses its device with cut pictures (finding 198) |

Sound evidence: `DHD_SND_WAV=path` (the final mix), `DHD_SND_VOICE_TRACE=path` (each sounding voice
every 800 samples), `DHD_SND_GAIN_TRACE`, `DHD_SND_REPORT`, `DHD_SND_TRACE=1` (sound.h).

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
- **Posted mouse clicks on the hidden desktop land at the real cursor** (SDL's auto capture and
  the window's mouse-leave read it): set `SDL_MOUSE_AUTO_CAPTURE=0`, post the move, down and up
  together, and tap again when the touch trace (`DHD_TOUCH_LOG=1`) shows no `up`. Touch runs
  (`DHD_TOUCH_MOUSE=1`) can't use `DHD_NAV` or the other pad scripts: they replace the live pad,
  touches included, unless `DHD_TOUCH_WITH_SCRIPT=1` merges them back in (finding 209); else drive
  the menus by posted keys (finding 189). A finger *held* or *dragged* by posted messages doesn't
  work either (SDL loses the mouse focus after each posted move, so a later up or move is lost;
  the OS cursor's jump after a move is dropped for the touch controls as for the menus' mouse,
  `touch_host.c`): hold and drag with `DHD_TOUCH_SCRIPT` (finding 214), tap with posted messages.
- **Posted keys don't reach the room under `DHD_NET_SCRIPTED`.** Use the scripted hooks
  (`DHD_NET_TEST_TRACK`, `DHD_NET_TEST_READY`) instead.
- **A pad script shuts the keyboard out.** With `DHD_NAV`, `DHD_AUTOPAD` or `DHD_AUTOPAD_FRAMES`
  set, `host_pad_read` takes the script's buttons and never reads the keyboard (autopad.c), all
  game long: a game driven by posted keys must have none (`hidden_game.Game.key` refuses then).
- **`DHD_RACE_PAD` without `DHD_RACE_SYNC=1` does nothing**, online too: the rider takes no input
  and an online test's humans neither pedal nor attack (finding 206). The online tools set both
  through `online_riders.rider_env`. An attack hits nothing before race tick 301
  (`func_0012E230`), and riders that only roll leave the gates apart: braking (Triangle) keeps
  two riders side by side for a hit.
- **`wait REGEX` matches the whole log**, earlier lines too: wait for something new.
- **A release `dhd.exe` opens the launcher** unless `DHD_LAUNCHER=1` or a script is set.
- **`DHD_MEMCARD=0` stops a release boot at "memory card not found"** on some paths: use
  `DHD_MEMCARD=1` with an empty `DHD_MEMCARD_DIR`.
- **A background `wrangler dev` outlives its shell** and keeps the port: use local_server.py.
- **nettool bots report place = slot + 1**, so results with bots show odd places.
- **Single-PC online tests miss cross-platform bugs** (the version string, POSIX putenv, pacing,
  all shipped once): every release with online or pacing changes runs cross_race.py both ways, and
  a `freeze:` line counts as a failure. From Git Bash set `MSYS_NO_PATHCONV=1`, or it rewrites the
  Mac's paths and the remote game never starts. A public room's grid locks 30 s after the host
  readies, so the host's first race is usually alone; it counts toward `--races`. To test the
  TURN/TLS fallback, give `--remote-app` a small script there that sets `DHD_NET_SFU_UDP=0` and
  `exec`s the game.
- **Never wait for a process by matching its command line in a loop whose own command line
  contains the same text.**

## Windows GPU selection for renderer measurements

Leave `DHD_WGPU_ADAPTER` unset for the existing player default (DXGI's first adapter).
`DHD_WGPU_ADAPTER=integrated` selects the first hardware adapter in DXGI's minimum-power
order; `discrete` uses its high-performance order. These are preferences: on a machine with
only one hardware GPU both can select it. The log records the preference and PCI IDs, then
`gs: GPUs` identifies the device actually used. If preference enumeration is unavailable the
explicit request fails rather than silently measuring the default GPU. `fallback` still selects WARP.
The same selected PCI IDs apply with `DHD_WGPU_BACKEND=vulkan`.

When the display is connected to another adapter, presentation includes a cross-adapter copy.
Keep the display and window configuration fixed across comparisons and report this limitation;
GPU pass timestamps do not measure all compositor/copy costs.

`DHD_SCALE_MEMORY_MB=N` overrides only the native resolution guard's memory estimate for
controlled developer measurements (1-65536 MiB). Integrated GPUs can expose very little dedicated
VRAM while using shared RAM; their normal dedicated-memory guard may clamp requested 6x/8x to
4x. Check `perf: render` for the actual scale. Choose a budget the machine can supply, keep it
constant across runs, and inspect the logged override. This does not alter player defaults,
texture-pack budgets or the device's texture-size limit.
