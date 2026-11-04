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
- No online play on phones yet: the host does not offer it (`host_service_net_offered`). Do not
  enable it without a decision (root AGENTS.md, current decisions).
- A phone has no launcher: the game copies the disc chosen in the document picker on first start
  (on a worker thread), checks once a day for a newer release and only shows a notice.
- The surface's formats are fixed (RGBA8, no format query; finding 145).
- Low-end phones are named by device only: the Realme RMX3930 (Unisoc T612); its profile is
  reproduced on the rooted Mi 11i with `port/tools/phone_throttle.py --apply t612`.
- Phone measurements: screen on, other apps closed, unlocked fps for speed runs
  (`DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1`), and the window must reach the race itself
  (finding 147). One phone run at a time.
- Tests: `python port/android/test_build_apk.py`, `test_disc_image.py` (JVM tests).
