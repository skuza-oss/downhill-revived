# port/ios/: the iOS app and its unsigned IPA

The root and `port/AGENTS.md` rules apply. Design: finding 224. The platform parts come from the
iOS port by Gustavo Ribeiro (GitHub `ribeirogustav`); keep his notices and credit.

- Build on a Mac with Xcode (iOS 17 SDK or later), xcodegen, CMake, Ninja, Python 3.12 and rustup
  with the targets `aarch64-apple-ios` and `aarch64-apple-ios-sim`. The game's core comes from the
  Windows build as wasm2c's `core_*.c`, as the macOS release's does:
  `python3.12 port/ios/build_ios.py --simulator --wasm core_*.c` (the iOS Simulator) or
  `--unsigned --wasm core_*.c` (the device build and `build/ios/DownhillRevived-<version>-unsigned.ipa`).
  `port/tools/package_release.py --platform ios --core core_*.c` makes and checks the release IPA.
- One code path: iOS uses the shared in-game touch controls (`port/hle/touch*.c`, fed by
  `port/host/touch_host.c`'s iOS part from `render/metal_bridge.m`), the game's own SETTINGS
  pages (Android's rows: `port/host/phone.c`) and the shared mobile launcher (finding 227): the
  app opens into `port/launcher/ui` in a WKWebView (`app/launcher.m`, the transport
  `app/bridge_ios.js`), whose calls `host/launcher_calls.c` answers with the shared code Android's
  launcher calls (the settings crate, `host_disc_check`, `dhd_disc_check`, `phone_update_find`).
  Do not grow a second launcher or settings UI, or logic the shared code has, in Objective-C here.
- The app's lifecycle is the game's: present.c's `app_away_wait` (fingers let go, sound paused, an
  offline race paused at the return, no game time lost or caught up) waits while
  `ios_game_is_active` is false; `host/run_record.c` keeps how a run ended (the next start's
  `previous run:` lines and the launcher's note).
- Online on iOS is P2P rooms only, through the same host switch as Android
  (`port/net/net.c` `NET_OFFERED`, findings 143, 180).
- The IPA is unsigned: no Apple ID, team, certificate or provisioning profile ever enters the
  repository or the build; players sign it when they sideload it (`docs/PLAYING.md`).
  `package_unsigned.py` refuses a bundle that names a home folder, a device or a team;
  `DHD_IPA_PRIVATE` adds a builder's own strings to that check, outside the repository.
- The app's Documents folder stands for the executable's folder: dhd.ini, dhd.log, the saves and
  the disc copy live there (visible in the Files app).
- The game ends the process itself (`host_end`, finding 210); the app does not restart a core in
  the same process.
- Tests: `python -m unittest discover -s port/ios/tests` (no Mac needed for most; the C checks
  need a compiler named `cc`; `test_launcher_ios.py` runs `test_bridge_ios.js` with node);
  `port/tools/test_runtime.py` runs the launcher's calls, the disc copy and the run record in C;
  the Simulator and device builds need the Mac. Simulator checks: `docs/BUILDING.md` (iOS) for
  the test switches and the silent audio driver.
