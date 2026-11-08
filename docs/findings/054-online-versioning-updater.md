# 054 — Online start gates, the version gate and the auto-updater (round 40, 0.2.0)

Status: partly superseded by 093 (the game's own updater)
Checked: 2026-11-04

The game's own updater (`port/hle/update.c`) was removed on 2026-10-27: the launcher
(`port/launcher`, finding 093) is the updater on every platform, with the same rules.

Three pieces for 0.2.0's online track:
- every rider on its own start gate online, the same on every PC;
- one version number and a server that turns away old releases;
- an updater in dhd.exe.

## 1. Riders sharing a start gate online

What players saw: in an online race two riders stood on the same spot at the start, and at GO
a puppet jumped onto another rider.

The start grid in the game:
- `func_0012C450` puts each rider on a start point of the race's list (`func_001A0498`: the
  index at +0xC, the points at +0xB0). Online, `hle_puppet_start_point` (finding 047) gives
  rider slot s the point of its global id: the host is player 0, the guests 1.., and the AI
  riders keep their slot. That alone would be the same on every PC.
- Then `func_0011AEC0` swaps each human (`D_004B25A0.x288`) with a random rider,
  `r = func_00222BD8(0, count - 1)`, through `func_0011B030` (it swaps the two riders' poses and
  their order in `game->riders`). Offline this is the "random start gate".

Online this swap is the bug. The draw is different on each PC, and each PC's human is its own
slot 0. Each PC moved its own player somewhere else, and nobody moved the puppets the same way.

Evidence: EE dumps of a two-instance race (`DHD_RACE_DUMP`) at countdown +150. The positions
are rider + 0x4270 (riders at 0x2AF2C0, 0x7BD0 each); the start points are 6 units apart in y.

| | host's PC (A) | guest's PC (B) |
|---|---|---|
| host's rider | y -1291 (own, slot 0) | y -1321 (puppet) |
| guest's rider | y -1327 (puppet) | y -1315 (own, slot 0) |
| AI rider at y -1315 | slot 5 | — |

So:
- A and B disagree about where both humans stand.
- On A, the guest's real place (-1315) is where an AI rider stands.
- At GO the snapshots start, every puppet moves to its owner's real place, and so lands on
  someone.

Fix (port only, nothing in `src/`):
- `port/patches.py` adds `&& !hle_puppet_keep_grid()` to the swap's condition in
  `unsorted/0011AEC0.c`.
- `hle_puppet_keep_grid` (`port/hle/puppet.c`) returns 1 online. The draw is still made, so the
  game's random sequence is unchanged; only the swap is skipped.
- Every rider stays on the start point of its global id: the humans side by side at the front,
  the AI behind them, and the same grid on every PC.
- The fallback without a start-point list (`hle_puppet_grid`, a node of the path) also uses
  the global id now.

Offline play is untouched (`pp.online` is 0).

Test: see section 5.

## 2. One version, and the server's minimum

- `port/include/version.h`: `#define DHD_VERSION "0.2.0"`, the only place the release number
  is written.
  - The online version string becomes `dhd-port 0.2.0 <exe hash>` (`port/hle/online.c`); rooms
    still need the same exe. Since 0.7.3 it is `dhd-port <version> <module hash>`: the exe hash
    differs per platform, so a Mac guest was refused in a Windows host's room. `build_wasm.py`
    writes the finished module's SHA-256 (first 8 bytes, hex) over `hle_core_id`'s mark, and
    every platform runs wasm2c's C of that module.
  - The hello sends `"clientVersion":"0.2.0"` (`port/net/client.c`).
  - The updater compares against it.
  - `port/tools/package_release.py` takes the version from it (`--version` must agree). It
    refuses an exe without `dhd-port <version> ` in it.
- Server (`online/server`): `MIN_VERSION` under `vars` in `wrangler.jsonc`, `MAJOR.MINOR.PATCH`,
  default `0.1.0`.
  - A `hello` below it is refused with `client_outdated`, close code 4009, and
    `{minVersion, clientVersion}` in the error frame.
  - A hello without `clientVersion` is 0.1.0. An unparseable one counts as outdated.
  - `/v1/health` and `/v1/lobbies` show `minVersion`.
  - Tests: protocol (parse, compare, legacy, configured value) and room (a legacy hello refused
    with 4009 and the frame's fields; 0.1.5 and "nightly" refused; 0.3.0 welcomed; health and
    lobbies show it). 34 tests pass, `tsc` clean.
- The text is "A new version is out. Update to play online." (44 characters).
  - The sentence first asked for, "A new version is available. Update to play online.", is 51
    characters. The online menu's footer holds about 45 at 640 pixels: on the 0.1.0 exe it ran
    off both edges.
  - The 44-character text fits (pixels 29-613 of 640).
- 0.1.0 knows no `client_outdated`, but any refusal goes to `NET_FAILED`, and its online menu
  shows the error's `message` as the HOME footer. So 0.1.0 shows the server's text: a readable
  refusal, no hang.
- 0.2.0 recognises the code. It shows its own copy of the text, adds an **Update Now** row
  (row 5) and puts the cursor on it (`port/hle/online_menu.c`).

## 3. The auto-updater (`port/hle/update.c`)

It runs in the first dhd.exe process, before the relaunch (`boot.c`):
1. `update_save_environment()`: the environment before dhd.ini, for the restart.
2. `host_settings_load()`.
3. `update_at_start()`:
   - Clean up what the last update left: delete `dhd.exe.old`, retrying for 3 s while the old
     process ends, and the `dhd-update\` folder.
   - Unless `auto_update = off`, or a tool drives the game (`DHD_NAV`, `DHD_RACE_SYNC`,
     `DHD_NET_CREATE`, ... so gates and tests never touch the network), ask the source on a
     thread and wait at most 6 s. No answer, no network or a bad answer: the game starts as
     always.
   - If the release is newer than `DHD_VERSION`: with `ask`, a message box "Downhill Revived
     X.Y.Z is available (this is 0.2.0)" with **Update now** / **Later**, the buttons renamed by
     a CBT hook; with `on`, no question.

The source:
- `update_url`, empty: `https://api.github.com/repos/skuza-oss/downhill-revived/releases/latest`.
- In GitHub's release JSON, the `latest.json` asset is fetched if there is one. Otherwise the
  `DownhillRevived-*-win.zip` asset is used, with its `digest` (`sha256:...`, which GitHub
  gives every asset) and the version from `tag_name`.
- Anything else is read as a `latest.json`: `{"version", "url" (may be relative to the
  manifest), "sha256"}`.
- The version must be digits, the sha256 64 hex digits and the url http(s).

The update (`install`):
1. Download into `<game>\dhd-update\` with WinHTTP (system proxy settings), hashing with
   BCrypt as it goes. A SHA-256 mismatch stops it.
2. Unpack with Windows' own `System32\tar.exe -xf` (Windows 10 1803 and later; no unzip code
   in dhd.exe), then find the folder that holds `dhd.exe`.
3. Rename the running `dhd.exe` to `dhd.exe.old`. Windows allows renaming a running exe, not
   overwriting it.
4. Move the new one in. On failure, put the old one back.
5. Copy the release's other plain files (README.txt, LICENSE.txt, ...). Never dhd.ini, dhd.log
   or iso-check.txt, and no folders, so `saves\` is never touched.
6. Start the new dhd.exe with the original command line, environment and std handles, and end.
7. The new process deletes `dhd.exe.old` and `dhd-update\` at its start.

Any failure:
- removes the work folder and keeps the old exe;
- says why in a box, with the release page;
- starts the game as it was.

From the online menu, Update Now (`update_request`):
- The relaunched game ends with exit code `0x55504454` ("UPDT").
- The first process (`update_after_game`) looks up the release and updates without asking.
- If no newer release is found, a box says so and names the release page; the game then
  starts again.
- Without a first process (`DHD_RELAUNCHED` unset), the process does it itself.

dhd.ini keys (the template written on the first start documents them):
- `server =` the race server; empty: the official one.
- `update_url =` the update source; empty: the official GitHub releases.
- `auto_update = ask | on | off`, default `ask`.

`DHD_UPDATE_NOW=1` checks and updates without asking, even under a tool (tests).

`package_release.py` writes `latest.json` beside the zip:
`{name, version, url, sha256, size, page}`. The url is
`https://github.com/skuza-oss/downhill-revived/releases/download/v<version>/<zip>`, or
`--base-url` + the zip's name for a self-hosted source.

## 4. Sources

- WinHTTP (Microsoft Learn): `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY` (Windows 8.1 and later)
  uses the system and per-user proxy settings; `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY` is the
  fallback. Redirects (GitHub's asset downloads go to another host) are followed by default.
- GitHub REST API, "Get the latest release": each asset has `browser_download_url` and, since
  2025, `digest` (`sha256:<hex>`). The current 0.1.0 asset shows
  `sha256:5434d5db...b033`, which equals the zip's hash.
- `tar.exe` (bsdtar) ships in `System32` since Windows 10 1803 and reads zip files.
- MessageBox button renaming with a thread-local `WH_CBT` hook on `HCBT_ACTIVATE`: the usual
  Win32 technique (no code taken).

## 5. Tests

Built with `build_clean.py`: the start-gate and most updater runs on 30222647 (dhd.exe
76fe6299...), the online menu runs, the gate and the package on ebf21db1 (4c8fe8a2...; only the
refusal text differs). Two local `wrangler dev`
servers: `MIN_VERSION` 0.1.0 (the default) and 0.3.0 (`--var MIN_VERSION:0.3.0`). Games driven by
`DHD_NAV` / `DHD_RACE_SYNC` only, no key presses; dialogs answered with messages to their own
buttons (`BM_CLICK`).

Start gates (`online_race.py`, host and guest, Alpine, dumps at countdown +150 and GO +20):
- Both log `puppet: online grid kept (no random start swap)`.
- Countdown +150, the two humans: the host at y -1321 and the guest at -1327 on both PCs (host's
  PC: slot 0 and 1; guest's PC: slot 1 and 0). The eight AI riders stand on the same points on
  both PCs. All ten points differ.
- GO +20: every rider has kept its lane (y within 1.5 units of its gate) on both PCs, the two
  humans 6 apart as at the gate: no puppet jumped onto anyone.
- Screenshots: the host's view shows its rider with the guest beside it, the guest's view the
  mirror (countdown "2" and GO +20).
- Before the fix: the table in section 1 (the same test on bb898f87, 0.1.0's code).

Version gate:
- 0.1.0 (bb898f87) against `MIN_VERSION` 0.3.0: ONLINE > Create A Public Race shows the text in
  the footer, whole (pixels 29-613). With the 51-character sentence it ran off both edges.
- 0.2.0 against 0.3.0: `online menu: refused: A new version is out. Update to play online.
  (client_outdated)`, the same footer, the Update Now row under Join With A Code, the cursor on it.
- 0.2.0 against 0.1.0: welcomed (the race above). Server: 34 tests.

Updater (a copy of the game in a test folder; a local source on Python's `http.server` serving
a manifest that names the newer release until its zip has been fetched once, a fake `DownhillRevived-0.2.1-win.zip` with a marked dhd.exe, a README, a dhd.ini and
a `saves\` file):
- `auto_update = on`, `update_url` = the local manifest: downloaded (1,957,600 bytes, sha256
  checked), installed, started. dhd.exe is the marked one, README replaced, LICENSE added; the old
  exe and `dhd-update\` deleted by the new process; dhd.ini and `saves\card1\save.bin` byte-identical;
  the zip's dhd.ini and `saves\evil.bin` not copied. The new process found 0.2.0 newest and started
  the game.
- `ask`: the dialog (Update now / Later); Later starts this version, nothing changed.
- A wrong sha256 in the manifest: "The update didn't work: the download is damaged...", dhd.exe
  unchanged, no work folder, the game starts.
- No network: a refused port (error 12029), an unknown host (12007), an unroutable address (no
  answer in 6 s): each logs `update: no check (...)` and the game starts.
- GitHub-style JSON: with a `latest.json` asset it reads the manifest; without, the zip asset and
  its `digest`, the version from `tag_name`.
- The official source (no `update_url`): `0.2.0 is the newest (the source has 0.1.0)`: GitHub's
  0.1.0 release, from its zip asset and digest.
- Online menu Update Now, against `MIN_VERSION` 0.3.0: refused, Update Now, the game ended with
  "UPDT", the first process downloaded, installed and started 0.2.1; that one deleted
  `dhd.exe.old`.
- `package_release.py --exe build/port/clean/ebf21db1/dhd.exe`: the zip and `latest.json`
  (version 0.2.0, the v0.2.0 release asset URL, the zip's sha256). `--version 0.2.1` and the
  0.1.0 exe are refused.

Network module tests (`port/net/build_tests.py`): lockstep, json, loopback ok.

Native renderer gate (`gate_native_renderer.py`, ebf21db1, reference r39-plain):
- The two arms agree: EE at every moment through GO+4500, and 1161 of 1161 GS readbacks.
- Both differ from the reference in the same 2 words at GO+0/2/12/16: EE 0x4F4324 (0x10 -> 0x24)
  and 0x4F4454 (0 -> 0x10). These are GS packet bytes, not race state.
- This branch changes nothing offline before the race (`hle_puppet_keep_grid` returns 0, the
  updater runs before the game), so the change comes from round 40's main menu work (5b00e37f).
  The reference is re-recorded once, at integration.

## 6. At release time

1. Build the release at the commit to publish. Since the 64-bit release (finding 088, CL3):
   `python port/tools/build_wasm.py`, then `python port/tools/package_release.py`. (Until 0.3.1:
   `build_clean.py`, then `package_release.py --exe build/port/clean/<rev>/dhd.exe`.) That writes
   `build/release/DownhillRevived-0.2.0-win.zip` and `build/release/latest.json`.
2. Publish GitHub release `v0.2.0` with both files as assets. 0.2.0 players then find newer
   releases there, and 0.1.0 players are told by the server once step 3 is done.
3. Set `MIN_VERSION` to `0.2.0` in `online/server/wrangler.jsonc` and deploy (`npx wrangler deploy`
   from `online/server`, logged in to the official account). 0.1.0 players then get the refusal
   text; 0.1.0 has no updater, so they download 0.2.0 by hand.
4. For later releases: bump `port/include/version.h` only, repeat 1-2, and raise `MIN_VERSION`
   when old releases should stop racing.

## 7. After the race the controller did nothing (0.2.0 bug)

Reported on 0.2.0: after an online race the game was back at the title and X did nothing.

Cause (`port/hle/online.c`): online mode never ended.
- `online` stayed 1, so `pads_read` kept using the race's menu script (`DHD_NAV`, set at the
  barrier) everywhere but the race itself. That script had run out, so it answered "nothing
  pressed" for the controller.
- The results' menu (TRY AGAIN ... QUIT, race phase 0) and the title were therefore deaf.
- The room, the puppets and the shared frame pacing also stayed on.

Fix:
- From the countdown on, the race screen reads the player's own controller, results and their
  menu included.
- `race_over` ends online mode when the game leaves the race screen. It also ends it when, after
  the results, TRY AGAIN or NEW LEVEL starts another race on the same screen (race phase back
  to the flight or the countdown).
- `race_over` leaves the room, turns the puppets off (`hle_puppet_offline`), ends the shared
  pacing, puts `DHD_NAV` and `DHD_RACE_SEED` back as they were and resets its state. The ONLINE
  menu can start another online race.
- Tests: `DHD_NET_AFTER_RACE` (with `DHD_NET_SCRIPTED`) is the menu script once the results are
  in.

Test:
- Two instances on a local server: race to the end (`DHD_NET_TEST_FINISH=10`), results, NEW
  LEVEL. Both log `the race is over; back to offline play` and `puppet: offline`, and the
  restored offline script starts the next race (Mt. Midoule) on both.
- QUIT on the results' menu did not respond to the scripted Cross in this harness (the cursor
  moves; the menu stays). Not resolved here; leaving the race screen by any way ends online mode.
