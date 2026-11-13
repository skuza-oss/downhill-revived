# port/android/: the Android app and APK

The root and `port/AGENTS.md` rules apply. Design: findings 091, 143; phones' speed: 107, 108,
126, 127, 146-149.

- Build: `python port/android/build_apk.py` (debug; the native libraries come from
  `port/tools/build_host64.py --target android-arm64`), `--release` for the release APK,
  `--check APK` to check one. `port/tools/package_release.py --platform android` puts the release
  APK, its `latest.json` entry and `SHA256SUMS` in `build/release/`.
- The release key never enters the repository: the `DHD_ANDROID_*` variables name the keystore
  outside the checkout and its passwords. The release check pins the release certificate;
  changing the key needs `--new-signing-key` on purpose (finding 143).
- Online on phones is P2P rooms only (`host_service_net_offered`, findings 180, 186): Create A P2P
  Race and Join With A Code; SFU rooms (public lists, Quick Race) are refused. Do not offer them
  without a decision (root AGENTS.md, current decisions).
- The app's entry is the launcher (finding 225): `LauncherActivity` shows the desktop launcher's
  page (`port/launcher/ui`, packed as `assets/launcher`) in a WebView, answers its calls
  (`LauncherBridge`, the list in `port/launcher/BRIDGE.md`) with the shared native code
  (`Native` -> `port/host/android_launcher.c`: dhd_settings, the disc checks, `update_find`), imports
  the disc (`DiscImport`, on a worker thread) and starts `DhdActivity` from PLAY (and ends). "Edit
  the touch layout" starts the game as the layout editor alone (`--touch-editor` from
  `DhdActivity.getArguments`) with the launcher left behind it: the editor's Done ends the process
  through `host_end`. `DhdActivity` is `singleInstance`, in a task of its own; the launcher shows
  again because Android brings back the launcher's task and starts it from its saved state. Nothing
  guarantees that on every maker's Android (and not when the task was removed): then the player
  lands on the home screen, and the durable mark `LauncherActivity.editorMark` (the app's private
  preferences) opens the launcher at its Touch tab at the next start, even with the launcher set
  to be skipped (finding 228).
  No second UI in Java: a launcher change goes in `port/launcher/ui`. The WebView loads only the app's own files
  (no JavaScript interface; the bridge is a MessagePort posted to that origin). Nothing is
  downloaded by the app: the launcher (or, on a direct start, the game once a day) only says a
  newer release is out.
- The surface's formats are fixed (RGBA8, no format query; finding 145).
- Low-end phones are named by device only: the Realme RMX3930 (Unisoc T612); its profile is
  reproduced on the rooted Mi 11i with `port/tools/phone_throttle.py --apply t612`.
- Phone measurements: screen on, other apps closed, unlocked fps for speed runs
  (`DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1`), and the window must reach the race itself
  (finding 147). One phone run at a time.
- Tests: `python port/android/test_build_apk.py`, `test_disc_image.py` (JVM tests).
