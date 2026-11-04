# 143: the Android APK as a release asset (release key, update notice, no online on phones)

Status: current
Supersedes: parts of 091
Checked: 2026-11-04

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
with one signer, the release key's certificate below (`RELEASE_CERT_SHA256`; review fixes), and
scheme v2 or v3; `aapt dump badging` shows
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
  download address needs. Since 2026-11-03 (review Muse #12) it takes every Location RFC 9110
  allows: absolute (the scheme in any case, `HTTPS://` too), a path on the host, or a path relative
  to the request's folder (`latest.json`, `../latest.json`, `?page=2`), dot segments resolved;
  before, a relative Location or an upper-case scheme failed as "unusable redirect". Still refused:
  https to http in any case, other schemes, and another host without a scheme (`//host/...`).
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
auto_update row, so in practice the check is always on there; `auto_update = off` is for tests
(why there is no row: "Review fixes" below).

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
- The phone's SETTINGS has no row for the update notice (section 3; why: "Review fixes", 4).

## The first start's screen (2026-11-03)

The disc image used to be asked for with SDL's message box (`SDL_ShowSimpleMessageBox`), which on
Android draws the platform's oldest dialog style, then copied with nothing on the screen for a
minute or two, and a phone without room failed only at the end. `SetupActivity.java` is now the
launcher activity, in the launcher's colours (`port/launcher/ui/style.css`):

- with the copy (`files/SCUS-97177.iso`) present it starts the game at once;
- otherwise it shows the free space and asks for the .iso through Android's document picker;
- it checks the chosen file before copying (an ISO 9660 volume whose SYSTEM.CNF boots
  SCUS_971.77, as host.c's `disc_problem`; another disc is named by its BOOT2 serial) and that
  the phone has room for it plus 128 MB, with Try again / Choose another file when it hasn't;
- it copies with a progress bar (through a .part file, checked again, then renamed), and offers to
  delete the chosen original when its document provider allows it (Download does), so the
  2.5 GB isn't kept twice.

`android_main.c` still checks the copy (and the core the game's data), and keeps its own picker
for a start without this screen (a USB controller starting `DhdActivity` directly). Checked on a
Redmi 9 (Helio G80, Android 11): the screen, the cutout filled, the release APK signed with the
release key.

## Review fixes (2026-11-03)

Branch `fix/review-android` (T3 names it `t3/fix/review-android`), from local main 60ba1c2. Three
reviews of v0.7.7..HEAD (Muse, Opus, Sol); each finding below was confirmed in the code first.

### 1. The first start's copy (Opus 10, 11; Sol 1, 5, 6)

All real. `SetupActivity` opened the document and read it on the UI thread (a cloud provider that
downloads the file in `openFileDescriptor` froze the screen into "app isn't responding"); its
positional reads refused a document a provider streams through a pipe or socket (allowed by
`ContentResolver.openFileDescriptor` and `DocumentsProvider.openDocument`); a copy that ended early
counted as done (`done` was never compared with the size, and the check reads only the first
sectors), so "Delete it and play" could delete the only complete copy; and every copy used the same
`SCUS-97177.iso.part`, so an old copy's clean-up could delete a new copy's file. Now:

- **One copy job per pick** (`SetupActivity.Copy`), on its own thread: it opens the document, asks
  the provider for its size when the descriptor has none (`OpenableColumns.SIZE`), checks it, checks
  the room, copies and checks the copy. The screen shows "Checking the file..." (an indeterminate
  bar; "a file on a cloud drive is downloaded first") from the moment the picker returns, then the
  copy's progress. The UI thread only draws; asking whether the original can be deleted (a provider
  query) is the job's, and deleting it runs on a thread of its own while the game starts.
- **A streamed document** (`getStatSize()` is -1 for anything but a regular file) is copied first,
  from the one descriptor, and only the local copy is checked. A regular file is still checked
  before copying (a wrong disc is refused without a 2.5 GB copy); if its positional reads fail
  anyway, it is copied and its copy checked.
- **A short copy fails** (`DiscImage.completeness`): with the size from the file itself (stat) the
  copy must be exactly that long; with the provider's `SIZE` it must be at least that long (the
  provider's word, so a longer copy is not refused on it); and in every case the copy must hold the
  whole ISO 9660 volume, the primary volume descriptor's volume space size (offset 80) times its
  logical block size (offset 128). So a stream with no size that ends early fails too. The retail
  disc's volume is exactly its file length (2,530,869,248 bytes = 1,235,776 x 2048, checked on the
  real image). "Delete it and play" is only ever offered after the complete copy passed both checks
  and was renamed into place.
- **Each job owns its temporary file**: `SCUS-97177.iso.<time>-<n>.part`, a name no other job has,
  registered while the job runs. Only that job renames or deletes it. `cancel()` (the screen's
  `onDestroy`, or a new pick) and the job's last check before the rename take the same lock, so a
  cancelled job can never publish; it checks cancellation again before validating and shows
  nothing once cancelled (`post` drops the screens of a job that is no longer the screen's). A new
  screen deletes left-over `.part` files (and the older name) except those of jobs still running.
- **configChanges**: the setup activity now has DhdActivity's (`layoutDirection`, `locale`,
  `grammaticalGender`, `fontScale`, `fontWeightAdjustment`, ...) plus `density`, `mcc` and `mnc`, so
  a font-size, language, display or SIM change during the copy keeps the activity and its copy.
- The rules (the disc check, the volume size, the completeness rule, the copy loop) moved to
  `DiscImage.java`, which uses no Android API, so a plain JVM runs them:
  `python port/android/test_disc_image.py [ISO]` (javac and java from `JAVA_HOME`, out to
  `build/android/disc-image-test`; no Gradle, no junit). `port/android/test/DiscImageTest.java`
  builds synthetic images: the right disc passes with an exact, a provider or no size; no volume,
  another serial and no BOOT2 are named; the reviewer's case (49,152 of 131,072 bytes, the metadata
  sectors intact) fails with a known size, with the provider's size and with none; a copy longer
  than an exact size and a dump shorter than its own volume fail; the copy loop's count, a cut
  stream, cancelling before the next block and the progress steps. With an ISO (or `DHD_TEST_ISO`)
  it also checks the real image. All pass.

### 2. The release check pins the release certificate (Muse 3, Opus 6)

Real: any non-debug signer passed. `build_apk.py` now has `RELEASE_CERT_SHA256` (the certificate
in section 1, public) and `check_release` fails on any other signer, naming both digests.
`--new-signing-key` (in `build_apk.py` and `package_release.py`, which passes it on and calls the
same `check_release`) lets a deliberate key change through, never the debug key, and prints a
note; the constant then changes with the release that starts using the new key. `build_apk.py
--check APK` runs the check alone on a built APK. Checked: the published 0.7.7 release APK passes;
a copy of it re-signed with a throwaway RSA key (made for the test, `CN=Not The Release Key`) fails
("signed by certificate SHA-256 f636bd53..., not the release key d7613ee0...") and passes only with
`--new-signing-key`, with the note; this branch's debug APK fails ("signed with a debug key",
its certificate 1df527f6... not the release key, "debuggable"), with `--new-signing-key` too.

### 3. The update notice asks GitHub when the server names nothing newer (Muse 6)

Real: `android_update.c` stopped at the first source that answered, and the server keeps serving
its last good latest.json while it can't reach GitHub (`X-DHD-Stale`), so a stale copy could hide
a release for as long as it stayed stale. `update_find` (`port/net/update_check.c`) asks the
sources in order until one names a newer release: GitHub is asked whenever the server doesn't
answer or answers with nothing newer (stale or not: the header needs no reading). The check is
once a day, so the second request costs little. `update_from_manifest` now takes the Android
entry's url only under `https://github.com/skuza-oss/downhill-revived/releases/download/` (the
launcher's `from_backup` rule) with no quote, space or backslash in it. `test_update_check` covers
the url rule (another host, another owner, a tag page, the bare prefix, GitHub's asset hosts) and
the order (a newer server answer means GitHub is not asked; a stale, broken or absent one means it
is, and its newer release counts). All pass.

### 4. Comments, and the update notice's row (Opus 17, 16)

The manifest's INTERNET comment said "the online menu"; it is the update notice now (phones have
no online play). An "Update Notice" row on the phone's SETTINGS does not fit cleanly, so it is not
added: the row list is the shared core's (`port/hle/pc_settings.c`), whose rows are found by their
environment variable, and `auto_update` has none (it is the launcher's setting: Ask / Automatic /
Off, where Automatic means installing, which a phone can't); the PC page already has 16 rows, the
popup's most, so the row would also need hiding everywhere but Android; and it would change the
core (a new core id, state traces). The notice is one line once a day that goes away by itself
after 15 s and never downloads, so on phones it stays always on, as section 3 says. If players ask
to turn it off, the clean way is a host-side switch on the notice itself (a long press: "don't
tell me again", writing `auto_update = off`), not a SETTINGS row.

### How the fixes were checked

- `python port/android/build_apk.py --wasm build/wasm/dhd_core.wasm` (debug): libmain.so with the
  new `android_update.c` and `update_check.c`, then Gradle, `BUILD SUCCESSFUL`, no warnings in
  the log; `downhill-revived-0.7.7-debug.apk` (30.4 MB) holds `DiscImage` and
  `SetupActivity$Copy` (classes2.dex). Not installed on a phone (the lead's checks, below).
- `build_apk.py --check` on the published 0.7.7 release APK (the real key): PASS; on the debug
  APK and the throwaway-key copy: refused, as in 2.
- `port/net/build_tests.py test_update_check test_json`: pass. `port/android/test_disc_image.py`
  with `DHD_TEST_ISO`: all pass, the real image's volume equal to its length.
- The core is not touched (no state trace needed): the changes are Java, the Android host's
  `android_update.c`, `port/net/update_check.c` (Android only, and the net tests) and the
  Python tools.

### Checks the lead still runs (phones)

- First start with no copy (clear the app's storage): pick the .iso from Download. "Checking the
  file..." shows at once, then the copy's bar; at the end "Ready to ride" with Delete / Keep, and
  the game starts. logcat (tag `DownhillRevived`): `disc image copied: 2530869248 bytes, checked`.
- The same from a cloud drive (Google Drive) if one has the image: the screen stays responsive on
  "Checking the file..." while it downloads (no ANR dialog).
- During the copy, change the system font size: the copy goes on (the screen is not restarted).
- During the copy, press Home and swipe the app away, then start it again: the intro shows, and
  the app's files folder (`adb shell run-as app.downhillrevived ls files`) has no `.part` left.

## Second review round (2026-11-03)

### 1. The update page can't leave the repository by dot segments (Sol 4, Muse F2)

Real: the page rule was "starts with `https://github.com/skuza-oss/downhill-revived/`" in
`update_check.c`, in `DhdActivity.updateNotice` and in the server's `latest.ts`, so
`https://github.com/skuza-oss/downhill-revived/../../attacker/fake/releases/latest` (or with
`%2e%2e`) passed all three, and a browser resolves it to `https://github.com/attacker/...`. The
launcher's `under()` (update.rs) already refused those. Now all four use that rule: after the
repository's address no `..`, no `%2e` in any case and no backslash (browsers read it as a slash);
the C and Java checks also refuse spaces, quotes and angle brackets. `update_check.c`'s `under()`
checks the APK url the same way; a page that fails becomes the releases page
(`UPDATE_RELEASES_PAGE`; in Java `UpdatePage.safe`, a small class without Android so a plain JVM
tests it); the server drops it. Tests: `test_update_check` (pages with `../`, `%2e%2e`, `.%2e`,
backslashes become the releases page; APK urls with them are refused), `UpdatePageTest.java` (run
by `test_disc_image.py` with the disc tests), the server's `latest.test.ts` (also that `new URL`
really resolves the first example to the other repository).

### 2. `build_apk.py --check` refuses a wrong package in words (Muse F3)

Real: `check_release` formatted the package mismatch with `%d %s` over the version, which
`--check` passes as `None`, so an APK of another package (or with no package line), the case the
check is for, ended in a `TypeError` traceback instead of the refusal. The line now names the
version only when there is one (`package line '...', not app.downhillrevived`). New
`port/android/test_build_apk.py` runs `check_release` and `check_only` with apksigner and aapt
replaced by canned answers: the release APK passes with and without a version; another package
and a missing package line are refused in one line on both paths; `--check` exits with "not fit
to publish"; a version mismatch is refused. It fails (the `TypeError`) on the old code.

Checks for the second round: `port/net/build_tests.py test_update_check`, `test_disc_image.py`
(both JVM tests, the real disc too), `test_build_apk.py`, the server's `npm test` (94 passed) and
`npm run typecheck`, and the debug APK built with the new `UpdatePage` (Gradle, BUILD SUCCESSFUL).
