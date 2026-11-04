# 093: the player launcher (DownhillRevived.exe)

Status: current
Supersedes: parts of 050, 054
Checked: 2026-11-04

Players needed one place for settings, the rider name, updates and the Discord server, on every
platform the game reaches. `port/launcher` is that window. It opens before the game and starts it.

## Shape

- **Tauri 2.** A Rust core (`src-tauri/src`) and an HTML/CSS front end (`ui/`), one codebase for
  Windows, macOS and Linux. On Windows it uses the system's WebView2, so nothing is bundled.
- **Size.** The release exe is about 4.7 MB. It imports only Windows' own DLLs (comctl32, secur32
  and shlwapi beyond the game's list).
- **HTTPS** goes through ureq's own rustls on ring (its `tls` feature) on every platform, with
  Mozilla's root certificates bundled (webpki-roots 1.0.11, the version the game's `dhd_net` pins):
  one rule for the launcher and the game, the same on Windows, macOS, Linux and Android, with no OS
  certificate store and no OpenSSL. (Until 2026-10-27 the launcher checked certificates through
  rustls-platform-verifier and its own `tls.rs`; both are gone.) The live GitHub source answers
  through it (`cargo test -- --ignored`).
- **Window.** 960x600, not resizable, no system title bar. The page draws its own title bar, a
  sidebar (Play, Settings, Patch notes, the rider card), the main pane and a footer. The footer
  holds the update state, Discord and PLAY on every page.
- **Design.** The design came from mockups in the site's palette (night, ember, gold, lilac;
  Archivo), checked against the Impeccable design skill's rules:
  - real Lucide icons, no glyphs;
  - no eyebrow labels and no card grids;
  - solid panels, no glass;
  - every state drawn.

  A second pass (an Impeccable-rules review, then v4 of the mockup) made it plainer:
  - no screenshot art;
  - every control on one 8 px radius; PLAY is an ordinary ember button, and the italic wordmark
    is the only slanted thing;
  - one primary button whose label follows the state (Play, Choose disc, Update);
  - the focus ring only after a key press, with PLAY focused at start so Enter starts the game;
  - errors stay in the footer until dismissed, with their fix as a button (Open folder, Retry);
  - closing a running game asks first, in the footer.

## What it does

- **The data folder** (`game.rs`, `Place`): one rule, decided by the launcher. The player's files
  (dhd.ini, `saves/`, dhd.log, iso-check.txt, the update's work folder) go in the game's folder
  when it is a plain folder the player can write to: the Windows and Linux zips, portable as
  always. Otherwise they go in the system's folder for app data: from a macOS app bundle (never
  written to) `~/Library/Application Support/Downhill Revived`; from a read-only folder such as
  Program Files `%APPDATA%\Downhill Revived` (Linux: `$XDG_DATA_HOME/downhill-revived`). The game
  is started in that folder with `DHD_SETTINGS` (dhd.ini), `DHD_MEMCARD_DIR` (`saves`, unless
  already set) and `DHD_DATA_DIR` (dhd.log and iso-check.txt, read by `host.c` and
  `online_menu.c`). The launcher makes a dhd.ini (its two comment lines) at its first start: the
  game applies its player defaults whenever dhd.ini exists, and keeps the tools' defaults without
  one (its own disc dialog, which made the file, is gone).
- **Settings** are read and written through `dhd_settings` (`port/render/settings`, finding 101),
  the game's own crate: a save rewrites only the key's lines and keeps every other line as it was.
  The tabs are:
  - Graphics: scale, `DHD_ASPECT`, `DHD_PRESENT_FILTER`, fps, vsync, textures, font;
  - Display: window, width and height, widescreen;
  - Game: iso via a file dialog, skip_movies, vibration, unlock_all, auto_update;
  - Online: name, server.

  Each row's choices and labels, a switch's two values and the defaults (shown where `dhd.ini`
  has no value, and set by Reset tab) come from the crate (`settings_schema`); the tabs keep their
  rows, labels, hints and controls.
- **Rider name** is `name`: up to 12 of A-Z, 0-9, space and dash, the same rule as
  `online_menu.c`. It can be edited in place on the rider card or on the Online tab.
- **Updates** (`update.rs`) are the only updater, on every platform (the rules of finding 054):
  1. The source is update_url, or the GitHub releases API, with a latest.json asset or the
     running platform's zip (`-win.zip`, `-mac-experimental.zip`, `-linux-experimental.zip`; 0.6.x: `-mac.zip`, `-linux.zip`) and its digest.
  2. latest.json: `platforms.win|mac|linux` = `{url, sha256, size}`. The top level's url, sha256
     and size stay the Windows zip's, which is what the 0.6.x launchers read, so they keep
     updating. A latest.json without `platforms` (0.6.4 and before) names the Windows zip only.
  3. The zip goes to `dhd-update/` in the data folder and its SHA-256 is checked. It is a zip on
     every platform: one extractor (the zip crate keeps the unix modes the packages set).
  4. A folder install (Windows, Linux): the files of the zip's one folder are copied over the
     game's. dhd.ini, the logs and folders are never touched. A file in use (the launcher itself)
     is renamed `NAME.old` first and deleted at the next start.
  5. A macOS app bundle: the zip's `Downhill Revived.app` replaces the running one whole. The old
     one is renamed `Downhill Revived.app.old` (deleted at the next start, or put back when the new
     one can't be moved in), so no file inside the signed bundle is written. An app that macOS runs
     translocated (from a read-only copy, unmoved after the download) can't be replaced: the error
     says to move it to Applications first.
  6. The launcher then restarts itself (the same path, now the new release's launcher).

  `auto_update`: ask shows Update in the footer, on installs without asking, off skips the check.
- **Patch notes** are the release body from GitHub. Everything above `## Commits` is shown, as a
  small markdown subset. The Play page lists up to four bold section titles, each with the first
  sentence of its first bullet, and the release date (`published_at`).
- **Disc image.** `disc.rs` makes the game's own check (`host.c disc_problem`): an ISO 9660
  volume whose SYSTEM.CNF boots SCUS_971.77. It skips the game's last step, hashing the program
  and data. Without a usable `iso`, the Play page shows the two first-run steps and the primary
  button is Choose disc. The file dialog refuses a disc the game would refuse, with the game's
  words. The European and Japanese releases are named (finding 100): "This is the European disc
  (SLES-52202). Downhill Revived needs the USA disc (SCUS-97177) for now." with a link to
  finding 100. The table of other releases, `port/hle/other_releases.inc`, is the game's too
  (`include_str!` here, `#include` in host.c).
- **Online disc check** (finding 098). Online races need the USA disc's game files (since
  2026-10-28; before, the whole file's SHA-256). After a disc is chosen, and at every start,
  `check_disc` runs the game's own check, the `dhd_disc` crate (port/render/disc), on a thread
  (`disc-progress` events; choosing another disc stops it). It answers from `iso-check.txt` in the
  data folder (`DHD_DATA_DIR`) when that holds this file's result, else hashes the image's files
  and writes it there, so the game never hashes them again. The Play page shows the progress. A
  disc whose files aren't the USA disc's gets "Offline only: this disc image's game files are
  different from the USA disc's (SCUS-97177): a bad or modified dump. Make a new dump or choose
  another image." on the Play page and the Online tab; PLAY stays. The game has no disc dialog of its own (2026-10-27): started with a missing or unusable
  disc, it shows one message box (choose the disc in the launcher) and ends. Android keeps its
  own picker (`android_main.c`).
- **Race server.** The Play page shows the server's public lobby list (`GET /v1/lobbies` on
  dhd.ini's `server`, or wss://dhd.skuza.app): the public races and the riders in them. Private
  rooms aren't listed. When the server's `minVersion` is newer than the launcher's, that is shown
  instead.
- **Settings details.** Window size is greyed out unless Display is Window. Texture pack is greyed
  out when no pack is found (`texture_pack`'s folder, else textures/SCUS-97177/replacements).
  Each tab has Reset tab (the player defaults; the disc and the name stay).
- **PLAY** starts `dhd.exe` (`dhd` on macOS and Linux) with `DHD_LAUNCHER=1` and the data folder's
  variables. The launcher stays in sight while the game
  starts and minimizes when its window loses the focus (the game's window took it). When the game
  exits, the launcher comes back. The online menu's Update Now exits the game with
  `UPDATE_EXIT_CODE` (85); the launcher then checks and installs.
- **Controller** (`ui/pad.js`, the browser's Gamepad API):
  - the d-pad or left stick moves the focus; in Settings, up and down go row by row and land on
    the chosen option;
  - A (Cross) presses, B (Circle) goes back to PLAY, LB/RB (L1/R1) change the settings tab, and
    Start presses the primary button;
  - the first pad press shows the focus ring and the button hints in the footer, in Xbox letters
    or PlayStation shapes by the pad's name; a mouse click hides them.
- **Discord** opens https://discord.gg/XrSeTyzfr2. Only the project's own addresses can be
  opened.

## Game side

One launcher for every platform, and nothing of it in the game (2026-10-27). The game's own
updater (`update.c`, 955 lines, WinHTTP and Windows dialogs) and its start window (`splash.c`,
finding 070) are gone. What stays is `port/hle/launcher_link.c`:

- `launcher_open`, the first thing `main` does: dhd.exe started without `DHD_LAUNCHER` opens
  `DownhillRevived` (`.exe` on Windows) when it is beside it, then ends, so old shortcuts open the
  launcher. It runs before dhd.ini and the log, so the launcher gets the starter's environment
  and the last game's dhd.log stays. Tools never get it (`automated()`: DHD_NAV, DHD_HEADLESS
  and the other driving variables), nor does a set `DHD_LAUNCHER`. CreateProcess on Windows,
  posix_spawn elsewhere.
- `update_request` (the online menu's Update Now) ends the game with `UPDATE_EXIT_CODE`, 85
  ('U'). The old 0x55504454 would reach a POSIX parent as its low byte only.
- `auto_update` and `update_url` stay in dhd.ini for the launcher; the game skips them. The
  `launcher = off` setting is gone.
- `host_disc_ready` names the European and Japanese releases as the launcher does (one table,
  `other_releases.inc`). It now also checks dhd.ini's disc under `DHD_HEADLESS` (the box is
  skipped there and the message goes to the log); a `DHD_ISO` from the environment is still never
  questioned. Checked with the x64 build, minimized, `DHD_HEADLESS=1`, dhd.ini naming small
  synthetic images that boot `SLES_522.02` and `SCPS_150.62`: the log had "This is the European
  disc (SLES-52202). Downhill Revived needs the USA disc (SCUS-97177) for now. More: ..." (and
  the Japanese line), then the game ended (exit 0).

Gone with them: the relaunch that kept the PS2 RAM's addresses free (`host_relaunch_reserving`,
`ps2_low_view_reserved`; every 64-bit host reserves the whole space, so it never ran),
`DHD_RELAUNCHED`, and the log's append rules. The start window's place is the launcher, which
stays up until the game's window shows.

## Release

- `port/tools/build_launcher.py [--platform]` runs `npx tauri build --no-bundle` on the host's own
  platform with the renderer's flags (`+crt-static` on Windows, CARGO_HOME and RUSTUP_HOME
  remapped). It refuses an exe that names a folder of this machine or was built with another
  version (the version is read from version.h by build.rs).
- `package_release.py --platform win|mac|linux` (default: the host's) builds the launcher (or takes
  `--launcher PATH`), checks it and puts it in the platform's zip:
  - **win**: `DownhillRevived-<v>-win.zip`, unchanged (the new scripts made a byte-identical zip
    from bab47b24c's inputs).
  - **mac**: `DownhillRevived-<v>-mac-experimental.zip` (since 0.7.0; `-mac.zip` before) holding `Downhill Revived.app`: `Contents/Info.plist`
    (bundle id `app.skuza.downhillrevived` and the name from tauri.conf.json, the version,
    `LSMinimumSystemVersion` 11.0, `NSHighResolutionCapable`), `Contents/MacOS/DownhillRevived`
    (the bundle's executable), `Contents/MacOS/dhd` (the game: Apple's place for helper tools,
    which codesign signs as nested code), `Contents/Resources/icon.icns` and the text files. On a
    Mac it is signed ad hoc (`codesign --force --deep -s -`, then `--verify --deep --strict`).
    There is no Apple developer account, so no notarization: players allow it once with Open
    Anyway (README.txt). Packaged elsewhere it is unsigned and left out of latest.json.
  - **linux**: `DownhillRevived-<v>-linux-experimental.zip` (since 0.7.0; `-linux.zip` before), the Windows layout with `DownhillRevived` and
    `dhd` (mode 0755, unix zip entries) and LF text. No AppImage: a folder, portable like Windows'.
  - The game for mac and linux is `--game PATH` (build_host64's targets for them; `--exe` is the
    same option). `--rust-lib` names its Rust library for the retail scan; the library's own
    checks read the Windows `link.rsp` and are skipped there with a note.
  - The same checks on every platform: version, absolute paths (the code sections of PE, Mach-O
    and ELF skipped), hosts, the retail-bytes scan, the crates' licences for the platform's
    target. The DLL allow-list's equivalents: `otool -L` or `llvm-objdump --macho --dylibs-used`
    (only `/usr/lib` and `/System/Library`), `llvm-readobj --needed-libs` or `readelf -d` (the C
    runtime; for the launcher also WebKitGTK 4.1 and the GTK stack).
  - Each run adds its platform to a `latest.json` of the same version; `--merge OTHER.json` adds
    another machine's. Publish every zip with the one latest.json.
- ring's C core (the static library its build script compiles) holds the words 0..16, as a run of
  the ELF's `.data` does (65 bytes at `.data+0x3B33`; dxcompiler.dll holds the same words). The
  launcher's native libraries now count as linked libraries in the retail scan
  (`build_launcher.native_libraries`), as dhd_render.lib does for the game, so `--launcher` must be
  the exe in its cargo target folder.
- THIRD-PARTY-NOTICES now also lists the launcher's crates.
- MPL-2.0 is allowed for the HTML parser crates Tauri uses (cssparser, selectors, dtoa-short,
  option-ext), unmodified. A crate that ships no MPL text (selectors) is listed under the copy
  another crate ships.
- Tauri's `compression` feature is off: brotli's alloc crates have no licence file, and the UI is
  small.

## Checks

- `cargo test` covers:
  - dhd.ini round trip, and the dhd.ini made at the first start;
  - the disc check on small synthetic ISOs (USA, another game, the European and Japanese boot
    files) and the online check through `dhd_disc` (the answer kept in iso-check.txt, stopping,
    an image without a file system); the hash itself and iso-check.txt are `dhd_disc`'s tests
    (finding 098);
  - the data folder rule (a plain folder; an app bundle);
  - versions, relative URLs, and latest.json with and without `platforms`;
  - a full download, checksum and install from a local HTTP server, where dhd.ini survives;
  - the same for an app bundle: replaced whole, the old one kept until `clean_up`;
  - with `-- --ignored`, the live GitHub source (0.6.4, 20,889,136 bytes, notes; through rustls).
- `cargo check --target aarch64-apple-darwin` passes on Windows (ring's C compiled against stub
  SDK headers there; a Mac has the real ones).
- The front end was screenshotted headless with a mocked Tauri API: all pages, the update state
  and the online tab.
- The real exe was captured off-screen with PrintWindow. It read dhd.ini (rider name), fetched
  the live notes and showed the no-dhd.exe state.
- `update.c` and `host.c` pass a clang-cl syntax check. The game hasn't been rebuilt or run with
  the launcher yet.

## 0.7.0 (branch `release/0.7.0`, 2026-10-28)

`desk/integrate4` (finding 099, "Integration round 4") plus the version and the experimental names.
macOS and Linux are experimental in 0.7.0: `package_release.py` names their zips
`DownhillRevived-0.7.0-mac-experimental.zip` and `-linux-experimental.zip` (`EXPERIMENTAL`), and
latest.json's `platforms.mac` and `platforms.linux` name them; the keys stay the platforms'. The
launchers already take each entry's url as written; the GitHub-assets fallback (a release without
latest.json) now also takes `-<PLATFORM>-experimental.zip` (`update.rs platform_zip`). The test
`experimental_platforms` reads 0.7.0's latest.json and GitHub's assets: each platform gets its own
zip, a Windows launcher never an experimental one, and a release with only experimental zips is
no update for Windows. README.txt says the two are experimental.

Built (compile and package, short runs; no benchmarks). Windows and macOS from 9f11d4e1d, the
version commit; Linux from 79c2a1e6f, the same game and launcher source plus the glibc check and
SDL's file-prefix map below:

| Platform | Where | Zip | Checks |
|---|---|---|---|
| Windows x64 | Windows 11, `package_release.py --platform win` | `-win.zip` 22,380,821 bytes, sha256 `0cde930e...a6d8` | every check; dhd.exe imports Windows DLLs only; the launcher's `cargo test` 12 + 1 ignored |
| Linux x64 | Ubuntu 22.04 (a WSL distribution of its own), game, launcher and package in one checkout | `-linux-experimental.zip` 16,218,642 bytes, `85a8e8d7...738f` | every check; `dhd` and the launcher need GLIBC_2.34 and GCC_4.2.0 at most; `dhd` needs libc, libm, libgcc_s and ld-linux only |
| macOS arm64 | the M1, the core's C from wasm2c here, `-j4` | `-mac-experimental.zip` 15,384,204 bytes, `216598a0...2868` | every check; ad hoc signed, `codesign --verify --deep --strict` on the unzipped app; system libraries only; the launcher's `cargo test` on macOS 12 + 1 |

The core: 132 host_api imports, sha256 `ab02479e...1b63`. `--merge` put the three entries in one
latest.json (each entry's size and sha256 checked against its zip).

Runs:

- Windows: the zip unzipped in a scratch folder; dhd.exe started as the launcher starts it (no console,
  DHD_LAUNCHER and the folder as DHD_SETTINGS, DHD_DATA_DIR, DHD_MEMCARD_DIR): dhd.log's first line
  `Downhill Revived 0.7.0 (dhd-port 0.7.0 3090cffaf10875b4)`; minimised with DHD_NAV through the
  title and menus for 45 s. The launcher opens (its window and WebView2) and ends cleanly.
- Linux (the packaged `dhd` from the 22.04 build, headless, DHD_NAV, presence on), in Ubuntu 22.04
  (glibc 2.35) and in Ubuntu 26.04 (glibc 2.43): the race (DHD_NAV's last step, field 3676) in
  11 s, 40 s with no fault; `discord: not running`. The launcher opens its window (960x600) under
  WSLg in 22.04 and ends on SIGTERM (143).
- macOS (the unzipped app's `dhd`, headless, the same): the countdown at field 2954 in 10 s, 40 s
  with no fault.
- Updates from a local server (latest.json with the three zips, urls pointing at it; dhd.ini's
  `update_url`, `auto_update = on`): 0.6.4's Windows launcher (the 0.6.4 package built on
  `desk/release`) fetched latest.json,
  downloaded `-win.zip` only, put 0.7.0 in place (dhd.ini kept, `DownhillRevived.exe.old` for the
  next start), restarted and found itself current. A Linux launcher built as 0.6.4 (round 3's, which
  reads `platforms`) on WSLg downloaded `-linux-experimental.zip` and put 0.7.0's launcher and game
  in place.

The first Linux zip (16,397,384 bytes, `f7a23588...8712`, never published) was linked on Ubuntu
26.04, whose headers bind newer symbol versions: the game needed glibc 2.43 (the C23
`__isoc23_*` functions and `strlcpy`, 2.38; `acosf`, `asinf`, `atan2f`, `coshf`, `sinhf`,
`acoshf`, `log10f`, 2.43) and the launcher 2.39 (`pidfd_spawnp`), so it ran on Ubuntu 26.04 or a
current Arch only. The published one is built on Ubuntu 22.04 and runs on glibc 2.35 and later
(Ubuntu 22.04, Debian 12, Fedora 36, Linux Mint 21 and later):

- `package_release.py` reads each Linux binary's version needs (`readelf -V`) and refuses any GLIBC
  version above 2.35, any GCC_ above 4.2.0 and any other library's (`LINUX_SYMBOLS`). The 26.04
  binaries are refused (game GLIBC_2.38 and 2.43, launcher GLIBC_2.39).
- On 22.04 (gcc 11 builds SDL3) the static SDL3 kept `SDL_gpu.c`'s and `SDL_sysgpu.h`'s full paths
  in assertion strings, and the package refused the game for its checkout folder. `sdl3.py`'s POSIX
  build passes `-ffile-prefix-map` from SDL's source folder to `SDL3-<version>`. gcc 15 on 26.04
  had dropped those strings: the first Linux zip and the Mac zip hold no build path.
- A checkout directly under `/root` can't be packaged: the forbidden folders include each folder
  less its first two characters (meant for a Windows drive letter and colon), which for `/root` is
  `oot`, in every binary. The build used `/opt/dhd/dhd-070`.
- The 22.04 distribution: the official WSL rootfs imported as a distribution of its own, LLVM
  21.1.8 from apt.llvm.org (the same clang as the 26.04 build), the SDL3 and WebKitGTK 4.1
  development packages, Rust 1.97.1 through rustup, Node.js 24.21, WABT 1.0.42 built from source,
  Python 3.14 (uv). docs/BUILDING.md.

## Open (as written for 0.6.x; since then the launcher ran on the M1 and Linux and 0.7.0 shipped
for all three, finding 099 and "0.7.0" above; dhd.log is written on every platform and the
texture-pack folder follows `DHD_DATA_DIR`, `docs/STATE.md`)

- **macOS and Linux need a run.** The launcher, its updater and the packages are ready; the
  game builds for them come from build_host64. Not yet seen on a real Mac: Gatekeeper's Open
  Anyway with the ad-hoc signature, the game as a helper in Contents/MacOS (its window shows
  under the bundle's Dock icon), and the bundle swap under `/Applications`.
- **Game side.** The default texture pack folder (`textures/SCUS-97177/replacements` beside the
  exe, `texpack*.c`) should follow `DHD_DATA_DIR` too, and the game should write dhd.log there on
  POSIX as well (today only Windows opens dhd.log; elsewhere stderr goes where the launcher's
  goes). `mc.c` joins the card folder with a backslash.
- **Online player count.** Only public races are counted; a count of all riders online would
  need a new server endpoint.
- **Untested flows.** The Update Now flow under the launcher, and a real update from 0.5.0 to the
  first release that has the launcher, still need a run.
