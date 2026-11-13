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
  pages and the shared mobile launcher once it exists. `app/main.m` is only the first-start disc
  screen until then; do not grow a second launcher or settings UI here.
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
  need a compiler named `cc`); the Simulator and device builds need the Mac.
