# 106: Discord Rich Presence

Status: current
Checked: 2026-11-04

2026-10-28, branch `desk/discord` (from `desk/settings`). The player's Discord profile shows what
they play: the menus, a race with its track and place, the career, an online room or race, a movie,
with the time since the race (or the session) began, the project's logo and two buttons. One
implementation for every desktop platform; Android and the browser do nothing.

## Shape

| Part | Where | What |
|---|---|---|
| The state | `port/hle/presence.c` (in the game core, wasm) | twice a second (every 30th field, from `pad_vsync`) it reads the game's own state and calls `host_service_presence` when the result differs from the last one sent |
| The boundary | `port/host/host_api.h` | `host_service_presence(uint32_t state)`, a guest address of `HostPresence` (52 bytes: kind, place, riders, members, room size, track name, join code) and the kinds `PRESENCE_*`; `gen_host64_imports.py` checks the struct's whole span |
| The native host | `port/hle/host.c` | at the first call starts `dhd_presence` with DHD_DISCORD's value (nothing more when it is off) and registers its stop with `atexit`; then hands each state over |
| Discord | `port/render/presence` (`dhd_presence`, in the one Rust static library through `dhd_render`'s `native` feature, header `port/render/include/dhd_presence.h`) | the lines, the elapsed time, the pacing, the IPC on a thread of its own |
| The browser | `port/web/www/worker.js`, `port/web/run_headless.mjs` | `host_service_presence` is a no-op |
| The setting | `discord` in `dhd_settings` (on/off, the player's default on); the launcher's Online tab: "Discord status" | the row's choices come from `settings_schema` like every other |

No Discord SDK: the `discord-rich-presence` crate (1.1.0, MIT, vionya) writes Discord's local IPC, a
named pipe (`\\?\pipe\discord-ipc-0` to `-9`) on Windows and a Unix socket (under `XDG_RUNTIME_DIR`,
`TMPDIR`, `TMP` or `TEMP`, Flatpak and Snap folders included) on macOS and Linux. Its dependencies
new to the library: serde_json 1.0.154, serde_repr 0.1.21, uuid 0.8.2 (the request nonce, on
getrandom 0.2.17, already linked through ring), memchr 2.8.3, zmij 1.0.23; serde, serde_derive,
thiserror and log were there already. All MIT or MIT/Apache-2.0 (the release's notice gate takes
them from cargo metadata).

The platform code is one line: `dhd_presence_start` returns 0 on Android (`cfg!(target_os =
"android")`), where there is no Discord to reach. The crate picks the pipe or the socket itself.

## What is shown

| Kind | When (the game's state) | First line | Second line |
|---|---|---|---|
| Menus | anything below | In menus | |
| Race | the race screen (menu 9), not the title's attract demo (mode 10) | Racing: Alpine MX | 2nd of 6 |
| Career | the same with the setup block's career flag (`D_004B25A0`+0x110 = 1, set by the career menus) | Career: Peru TDH | 1st of 10 |
| Online room, public | the lobby is joined (`NET_LOBBY`, `NET_RACING` or `NET_RECONNECTING`, with a code), the room public, no race | Online room K7M2QX | 3/6 riders |
| Online room, private | the same, the room private (the lobby's `is_public` 0) | Online room | 3/6 riders |
| Online race | the race screen in an online race (`pads_online()` or `NET_RACING`) | Online race: BC TDX | 3rd of 4 |
| Movie | `host_service_movie_playing()` | Watching a movie | |

- The place is the player's rider's (rider 0, also online) at +0x798C, with the count of riders at
  `D_002AF2C0`+0x501A8 (finding 040), shown in race phases 2 to 4 (the race). In the countdown
  (phase 1) the game still has every rider 1st (a run: place 1 of 10 at phase 1, 10 of 10 from
  phase 2 with the rider standing still), so no place there, nor in the flight or the results.
- The track's name is the online launcher's (`online_menu.c`, `port_track_name`), title-cased by
  the crate ("ALPINE MX" as "Alpine MX": words of 4 letters or more); the three tracks that list
  lacks fall back to the loaded level's name.
- A private room's join code never leaves the core: `presence.c` copies the code into
  `HostPresence` only for a public room, so the native side, the crate and Discord never see it.
- The elapsed time counts from the start of the session, and in a race from the moment its kind or
  track was first seen (a new place keeps it).
- Large image `logo` with the text "Downhill Revived"; buttons "Get Downhill Revived"
  (https://downhill-revived.pages.dev/) and "Join the Discord" (https://discord.gg/XrSeTyzfr2).
  No art of the game's.

## Pacing and failures

- Only a change is sent, at most one update per 15 s (Discord allows about 5 per 20 s); changes
  inside the gap merge, the last one wins (`Pacer`).
- Each update reads Discord's answer, so the pipe never fills; an `ERROR` answer is logged once.
- Discord not running, or the connection gone: one log line, a try every 30 s, then the wanted
  state goes again. No error box, nothing on screen.
- At exit (`atexit`: the window's close button and EXIT GAME both end through `exit`) the presence
  is cleared and the connection closed, waiting at most 0.5 s. A process that ends some other way
  (the launcher's update, `host_exit`) leaves it to Discord, which clears it when the pipe closes.
- discord-rich-presence logs through the `log` crate (one warning per failed try on Unix);
  `dhd_render`'s logger drops its target (`log.rs`), so dhd.log has only `dhd_presence`'s lines:
  - `discord: presence set: Racing: Alpine (10th of 10)` (the first update after connecting, then
    one per change of kind, not of place);
  - `discord: presence cleared` (at exit);
  - `discord: not running; the presence waits for it (a try every 30 s)` (once);
  - `discord: the connection closed; ...` (once per loss).

## The setting

`discord` was already in `dhd_settings` (finding 101) with the player's default on. Its built-in
value, which applies only without a dhd.ini (the tools' runs: `DHD_SETTINGS=none`, the race
harnesses), is now off, so no test run on a PC with Discord shows up on that PC's profile. Players
always have a dhd.ini (the launcher, and Android's start, make one), so for them the default is on.
`port/hle/settings_table.h` was regenerated for it.

## Changes outside the feature

- `port/render/src/log.rs` (locked; a condition added): the crate's log target dropped.
- `port/tools/rustgs.py`: `PRESENCE` in `MEMBERS` (cache key and include gate cover its sources).
- `port/tools/build_wasm.py`: `hle/presence.c` in the module's units.
- `port/tools/gen_host64_imports.py`: the span check of `host_service_presence`.
- `port/tools/package_release.py` (locked): the official address's run-on (below).
- `port/hle/pad.c` (one call), `port/hle/hle.h` (its declaration), `port/hle/online_menu.c`
  (`port_track_name`), `port/hle/host.c` (the glue, 16 lines).
- The browser's two no-op lists.
- The core's imports change (one more, `host_service_presence`): the core must be rebuilt
  (`build_wasm.py`) with this branch.

## Checks (2026-10-28)

- `cargo test -p dhd_presence`: 8 pass (with public and private rooms: the code, or none and
  not anywhere in the activity's JSON): the lines of each state, the place's limits, track names
  and ordinals, the elapsed time, the pacing (the first update at once, changes merged inside 15 s,
  nothing for no change, the clear, a resend after a lost connection), the setting's reading, and
  the activity's JSON (details, state, timestamps, `logo`, both buttons). `cargo clippy -p
  dhd_presence --all-targets`: clean. `cargo test -p dhd_settings`: 8 + 1 pass (header regenerated).
- `build_wasm.py`: 133 imports (132 before, plus `host_service_presence`), strict link and offset
  scan clean. `build_host64.py`: PASS, 133 generated imports, 37 host objects.
- dhd64.exe 26,602,496 bytes against 26,466,304 for `desk/settings` built the same way (+136 KB:
  the crate, serde_json, uuid). The launcher is unchanged (4,818,432).
- `package_release.py`: passes (zip 21.7 MB, dhd.exe Windows DLLs only); dhd.exe holds both
  addresses, `discord-ipc-` and the application ID; the notices list discord-rich-presence and the
  new crates. The launcher check needed one fix that `desk/settings` already needed (below).
- Runs, minimised (no focus), `DHD_SETTINGS=none`, the standard DHD_NAV route into a single race on
  ALPINE, closed after 100 s by WM_CLOSE (the close button's path), with Discord running here
  (`\\.\pipe\discord-ipc-0`):
  - `DHD_DISCORD=on`: `discord: presence set: In menus`, `discord: presence set: Racing: Alpine`,
    `discord: presence cleared`; exit 0.
  - `DHD_DISCORD=off` (60 s): 157 log lines, none about Discord; no thread, no pipe opened
    (`dhd_presence_start` returns before either).
  - Rooms, against a local race server only (`online/server`, `wrangler dev` on 127.0.0.1:8799,
    the runner refusing any other address), through the ONLINE page by DHD_NAV with
    `DHD_RENDER=native` (60 s each):
    - Create A Private Race: room SF5CNR; `discord: presence set: Online room (1/4 riders)`, then
      `presence cleared`; the code is in the game's own log lines, in none of the presence's;
    - Create A Public Race: room SP87VU; `discord: presence set: Online room SP87VU (1/4 riders)`.
  - One earlier try of the private room (without `DHD_RENDER=native`, so no room) ended after
    40 s on `GraphicsCommandList::close failed ... 0x8007000E` (the GPU out of memory, other work
    sharing the PC); the renderer's, not the presence's; the runs after it were clean.
  - A build with a temporary log line in `presence.c` (not committed): kind 1 (menus) at the start,
    kind 2 track ALPINE at the race screen, place 1 of 10 in the countdown, 10 of 10 from phase 2
    (the rider standing still), which is why the place shows from phase 2.

## The launcher's host check

`package_release.py` refused the launcher already on `desk/settings`: dhd_settings' table puts
`wss://dhd.skuza.app` just before the key `name`, so the bytes read
`wss://dhd.skuza.appnameDHD_NET_NAME` and the check took `dhd.skuza.appnamedhd` for a host. Now
letters only after the official address (no dot, port, digit or dash after them) end it there, as
the check already did for a bare `ws://` running into the next constant. `dhd.skuza.app.evil.com`,
`dhd.skuza.appx.com`, `dhd.skuza.app:8080` and `dhd.skuza.app-x.io` are still refused. No allow-list
needed the landing page or the invite: the game is outside that text check, and the launcher had
`discord.gg` already.

## Open

- macOS and Linux: compiled (cargo check), not run against a Discord there.
- Restarting the same race (TRY AGAIN) keeps the elapsed time; the core does not tell a restart.
