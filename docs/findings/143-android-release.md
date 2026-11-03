# 143: the Android APK as a release asset (release key, update notice, no online on phones)

Branch `feat/android-release` (from local main baa24d5), 2026-11-03. (Number 143: 142 is the
delta-time replays; no branch used a higher one.)

Goal: the next release ships an experimental Android APK next to the zips, signed with a release
key, listed in `latest.json`, and a phone (which has no launcher) learns of newer releases. The
lead added two things on the way: no online play on phones yet, and none in the browser either.

## 1. The release-signed APK

`port/android/build_apk.py --release` runs Gradle's `assembleRelease`. `app/build.gradle`:

- `release`: `debuggable false`, `jniDebuggable false`, `minifyEnabled false`, `shrinkResources
  false`. R8 stays off: SDL's Java (`org.libsdl.app`) and `DhdActivity`'s methods are reached from
  C through JNI by name (`touchScreen`, `touchBuzz`, `updateNotice`, SDL's own `nativeSetenv`
  callbacks...). R8 would need keep rules for every one of them and nothing proves it safe; the
  APK's Java is a few hundred kB, so there is nothing to gain.
- `signingConfigs.release` reads four environment variables, and nothing else names the key:
  `DHD_ANDROID_KEYSTORE` (the file), `DHD_ANDROID_KEYSTORE_PASS`, `DHD_ANDROID_KEY_ALIAS`,
  `DHD_ANDROID_KEY_PASS`. Without them the release build type is unsigned, and `build_apk.py
  --release` refuses to start (it also refuses a keystore inside the checkout). Signature scheme
  v3 (`enableV3Signing`): it lets a later release rotate the key, and every Android this APK runs
  on (11 and later) verifies it. AGP then leaves v2 out at minSdk 30 (seen: with the flag the APK
  is v3 only, without it v2 only); v1 is off from minSdk 24.
- The debug mode is as before (`downhill-revived-<version>-debug.apk`, the debug key).

After the build `check_release` refuses the APK unless: `apksigner verify --print-certs` passes
with one signer that is not the Android debug certificate and scheme v2 or v3; `aapt dump badging` shows
package `app.downhillrevived` with the versionCode and versionName of `version.h`, no
`application-debuggable`, and `native-code: 'arm64-v8a'` only. The APK is then
`build/android/DownhillRevived-<version>-android-experimental.apk` (the release assets' naming).

**versionCode** was already derived from the version: `MAJOR * 10000 + MINOR * 100 + PATCH`
(0.7.7 = 707). It grows with every release as long as MINOR and PATCH stay under 100;
`version()` now refuses a version where they don't (0.7.100 would be 800 = 0.8.0).

**The key** (made once, 2026-11-03, with the JDK the Android build uses, Android Studio's JBR
keytool): PKCS12 keystore, RSA 4096, SHA384withRSA, valid 30 years (to 2056-11-03), alias
`downhillrevived`, `CN=Downhill Revived, O=Downhill Revived`; a random 40-character password.
Kept by the maintainer outside the repository, with a backup on another drive. Its certificate
(public):

    SHA-256 D7:61:3E:E0:F0:93:95:22:D8:3B:06:CE:8A:05:ED:9D:03:E4:A9:96:8E:EA:2F:9C:83:98:58:94:14:56:AE:1B
    SHA-1   40:AB:92:5D:1C:99:78:9A:A1:9A:44:56:DF:5B:6F:56:35:B5:74:93

Every later release must be signed with this key: Android installs an update only from the same
signer. A phone that has a debug build installed must uninstall it first, which deletes the app's
files (the disc copy, dhd.ini, the saves); the notes say so.

## 2. Packaging

`package_release.py --platform android` (the platform is in `PLATFORMS` and `EXPERIMENTAL`; there
is no launcher) builds the release APK through `build_apk.py --release --assets`, which puts
`LICENSE.txt` and `THIRD-PARTY-NOTICES.txt` (with the Rust crates linked into `libmain.so` for
`aarch64-linux-android`) into the APK's assets, as every zip carries them. Or `--apk PATH` takes a
ready one. It checks `check_release` again and every file in the APK as the zips' files are
checked: game-data names, runs of the retail ELF (`libmain.so`'s Rust library bytes exempt, read
from build_host64's `link.rsp`), other servers and local paths (the shared `names_server_or_path`,
moved out of the zip loop), `libmain.so`'s version string, absolute paths and servers
(`exe_problems`; `binary_kind` and `code_ranges` know ELF arm64 now). Then the APK goes to
`build/release/DownhillRevived-<version>-android-experimental.apk` and `latest.json` gets
`platforms.android` {url, sha256, size} like the others. Launchers never ask for that key.

**SHA256SUMS.** Every `package_release.py` run (and `--merge`) writes `build/release/SHA256SUMS`:
each `DownhillRevived-<version>-*` file there except the link map, and `latest.json`, in
`sha256sum` format. `--sums` writes it again after the other machines' files are copied in.

**The server's backup** (`online/server/src/latest.ts`): `checkManifest` takes any platform key
of `[a-z0-9-]{1,32}` whose url is this repository's release asset, so it already serves the
`android` entry. A test now says so ("serves the Android APK's entry like the others"). No
change, no deploy.

## 3. Telling a phone about a newer release

The game had nothing for this: the updater is the launcher's (`update.rs`), and Android has no
launcher. Added, Android only (the desktop builds keep the launcher):

- `port/host/android_update.c`, started by `android_main.c` after dhd.ini exists. At most once a
  day (`update-check.txt` in the app's files folder holds when the last check started), never
  when dhd.ini says `auto_update = off`. On a thread of its own, so the start never waits.
- It asks `https://dhd.skuza.app/v1/latest` (finding 138's backup) first and, when that fails,
  GitHub's `releases/latest/download/latest.json`. The launcher's order is the other way round;
  a phone network shares one address among many phones, and GitHub's API allows an address 60
  requests an hour, so the phone starts with the server. The HTTPS client is the game's own
  (`http_get`, `dhd_net`): 5 s to connect, 10 s without data, so an offline phone gives up quietly.
  The client now follows redirects (at most five, never from https to http), which GitHub's
  download address needs.
- The rules are `port/net/update_check.c`: versions compared as `update.rs` compares them (three
  numbers, an optional `v`); a release counts only when it is newer and has a
  `platforms.android` entry with an https url; the page shown is the manifest's when it is this
  repository's, else the releases page; the once-a-day rule (a clock set back counts as due).
  Unit tests: `port/net/test_update_check.c` (`python port/net/build_tests.py test_update_check`).
- The notice: `DhdActivity.updateNotice(version, page)` adds one line at the top of the screen,
  "Downhill Revived 0.7.8 is out - tap for the download page", for 15 s; a tap opens the page in
  the browser (`ACTION_VIEW`). It opens only pages of this repository. Nothing is downloaded or
  installed. dhd.log gets `update:` lines.

On Android dhd.ini is not reachable for a player and the phone's SETTINGS page has no
auto_update row, so in practice the check is always on there; `auto_update = off` is for tests.

## 4. No online play on phones or in the browser (lead's decision)

Online waits on phones until their speed and the touch controls are good enough; the browser
never had it working (its CSP blocks the race server's socket). The menu code is in the shared
core (one wasm32 module for every platform; its core id must not differ by platform), so the
switch is the host's: a new host service, `host_service_net_offered()`.

- Windows, macOS, Linux: `port/net/net.c` answers 1. Unchanged.
- Android: `net.c` answers 0, decided at compile time (`#ifdef __ANDROID__`, `NET_OFFERED`), and
  `net_create_room`, `net_join_room` and the lobby list refuse there whatever asks.
- The browser: `www/net.js` answers 0 (`ONLINE_OFFERED`), and its connect and lobby-list refuse
  (`check_net.mjs` tests both, and keeps testing the session code with `online: true`).
- The core (`port/hle`, patches in `port/patches_exit.py`): at 0 the main menu has four rows,
  SINGLE PLAYER, MULTI PLAYER, OPTIONS, EXIT GAME: `port_main_rows()` gives the count to the
  game's menu (`func_001F7420`), `port_main_choice()` maps a shown row to the row the patched
  menu acts on, and `port_exit_label` names it. At 1 they give 5 and the row itself, as before.
  `online.c` ignores `DHD_NET_CREATE` / `DHD_NET_JOIN` at 0, so neither dhd.ini's `server` nor any
  `DHD_NET_*` can start a session. It is not a setting, so no dhd.ini line turns it back on.

The desktop state traces are unchanged (below).

## How it was checked

- **The release APK**, built from this branch with the real key (`package_release.py --platform
  android`, version 0.7.7 as a test; the release builds it after the version bump):
  `apksigner verify --print-certs`: verifies, one signer, `CN=Downhill Revived, O=Downhill
  Revived`, certificate SHA-256 as above, RSA 4096, scheme v3. `aapt dump badging`:
  `app.downhillrevived` versionCode 707 versionName 0.7.7, sdkVersion 30, targetSdkVersion 35,
  `native-code: 'arm64-v8a'`, no `application-debuggable`. Inside: `libmain.so`, `libSDL3.so`,
  `classes.dex`, `assets/LICENSE.txt`, `assets/THIRD-PARTY-NOTICES.txt`. Every package check
  passed (game data, servers, local paths, libmain.so's version). `latest.json` got the `android`
  entry and `SHA256SUMS` checks with `sha256sum -c`. The first attempt failed in Gradle: no
  statement may come before `plugins {}` (fixed).
- **The online switch, desktop unchanged**: state traces of this branch's x64 game against a build
  of baa24d5 made the same way: boot (600 fields) and race (5000 fields), every row equal.
- **The switch at 0**: a throwaway x64 build with `NET_OFFERED 0` (not committed), run to the main
  menu with Up pressed, RAM dumped at field 880: the menu's last row index (`D_004C6CA8 + 0xC`)
  is 3 (four rows; 4 in the normal build) and Up wraps over four rows (three presses: row 1;
  normal build: row 2 of 5). Not looked at on a screen; the phones will show it.
- **The browser's switch**: `node --test port/web/check_net.mjs`: not offered, create, join and
  the lobby list refuse and no socket is made; the session test still passes with `online: true`.
- **The update check**: `test_update_check` (versions, manifests, page rule, the day rule) and
  `test_json` pass (`port/net/build_tests.py`); `cargo test -p dhd_net` (the redirect rules) passes.
  A throwaway x64 program with the same C path (`http_get` + `update_from_manifest`) against the
  live `https://dhd.skuza.app/v1/latest` (1232 bytes): "0.7.7 is newer than 0.7.6" for `linux`,
  nothing for 0.7.7, nothing for `android` (0.7.7 has no APK). GitHub could not be reached from
  this PC during the work (curl got no connection either), so the redirect was not exercised
  against GitHub itself. The notice's Java (`updateNotice`) is built but not yet seen on a phone.
- `python port/tools/test_runtime.py`: all pass. Server: `npm run typecheck` clean, vitest 89/89.

## Open

- The lead's phone checks: the release APK installs and runs (Mi 11i, the T612 phone), no ONLINE
  row, the notice (needs a release newer than the APK with an `android` entry, or a test APK built
  with a lower version), and that a phone with the debug build must uninstall first.
- The release itself: after the version bump, `package_release.py --platform android` with the
  key's variables, then publish the APK, `latest.json` and `SHA256SUMS`; paste
  `port/release/ANDROID-NOTES.md` into the release page.
- The phone's SETTINGS has no row for the update notice (see section 3).
