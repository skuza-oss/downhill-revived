# 242: Two test tools fixed, one Share-log builder, one phone disc-import rule set

Status: current
Checked: 2026-11-14

Branch `cleanup/shared-phone-rules` from `main` 7906a93f, started 2026-11-14.

## Goal and acceptance

Cleanup audit A03 and A08, plus the two broken tools finding 233 noted. (1) `native_replay` builds and
runs again, its command head taken from one definition; `gs_golden.py` usable on this PC's current
driver. (2) The Share log file assembled once in shared C for Android (JNI) and iOS. (3) The phones'
disc-import rules (room, completeness, the copy's check, constants, superseded picks) once in shared C;
the document adapters stay per platform. No player-visible change except where stated here.
Gate: `check.py quick`, `port`, `docs`; Android native + Java build and tests; the iOS Simulator app
and `port/ios/tests`. No run on the owner's phone (the lead checks it after the merge).

## Progress

- [x] Part 1: the two test tools (and `replay_gs.py` in a worktree)
- [x] Part 2: one Share-log bundle (A08)
- [x] Part 3: one disc-import rule set (A03)

## Decisions

- **native_replay**: `COMMAND_HEAD` is `offset_of!(DhdrCommand, texture)` (abi.rs pins the layout at
  compile time), so it follows the struct. The trace version goes to 2 (head 184 bytes). Version 1
  (168-byte head, before finding 168's mesh constants) is refused: only the retired C recorder wrote
  it and no such trace is kept, so a v1 reader would have nothing to read (NTRACE.md, "Versions").
  Its `--selftest` (31 cases, synthetic traces, no GPU, no window, 2 s warm) is in `check.py port`;
  a fresh worktree builds the tool once (about 3 minutes).
- **gs_golden**: a golden's directory is keyed on, and compare pins, the adapter's driver (and WARP
  for test-gpu). Measured on this PC: driver 32.0.16.1742 -> 32.0.16.2114 and WARP 10.0.26100.9278 ->
  .9818 left `test-gpu`, `test-gpu-ee` and `native-transfers-stress` byte-identical. The pins stay
  (the tool's rule: no check relaxed); new `gs_golden.py adopt --from OLD GROUP[=SOURCE]...` makes
  the new driver's golden only from groups a current run proves identical to the old golden, hard
  links (no extra disk), a meta.json naming both drivers and the groups left out, written last.
  Any differing group refuses the whole adopt. Selftest cases cover differ, other device, same
  driver, success, left-out group and existing golden. Not in `check.py` (needs a GPU run and a
  local golden); `docs/TESTING.md` says when to run it.
- **replay_gs.py**: refused every output in a worktree (`build/` is a junction, the prefix was
  resolved and the root was not); both sides are resolved now.
- **Share log** (A08): `port/host/log_bundle.c` writes the file for both phones (Android:
  `Native.logBundle` from `LogShare`; iOS: `shareLog` directly) and launcher.log's lines
  (`launcher_log_line`, iOS's `llog`, Android's import steps from part 3). The platforms keep their
  share sheets, the device's words, whether a game runs in this process (Android: a dhd.log written
  since the process started; iOS: never, the launcher runs before the game) and the files they write
  themselves (dhd-exit.txt: `LastExit.record`, `run_record.c`). The file: `Downhill Revived V log,
  shared DATE`, `device: ...`, then dhd-exit.txt, launcher.log, the run that ended, the other run,
  each `==== TITLE (NAME), last written DATE ====`; a log over 1792 KiB keeps its first 256 KiB and
  last 1536 KiB with `[... N bytes left out ...]`, copied through one 64 KiB buffer.
  `test_log_bundle.c` (test_runtime): header, order with and without a running game, missing files,
  a long log's exact head, gap and tail, nothing to share, launcher.log's stamp and restart.
  `CrashReport.copyLog` and its Java test went (the C test covers it).
- **What changed in the file**, today's inputs:
  - Android: `phone: ...` is now `device: ...`; the exit section is titled `how the last run ended
    (dhd-exit.txt), last written DATE` (was `(Android's record)`, no date), its text read from the
    file `LastExit.record` writes (the same text); launcher.log is new (part 3's import steps). The
    logs' titles, order and trimming are as before.
  - iOS: the header is the two lines above (was `Downhill Revived V on iOS X (iPhone)`); the device
    line adds the model identifier (`uname`); sections carry titles and dates (were `==== NAME ====`);
    a long log keeps its start too (was its last 1792 KiB only: the settings and device lines were
    lost); logs are streamed (were read whole into memory); with no log at all an alert says "There
    is no log yet." (was a header-only file).
- **Disc import** (A03): `port/ios/host/disc_copy.c` became `port/host/disc_import.c`, linked into
  both phones (`PHONE_UNITS`): the constants (block 1 MiB, progress 16 MiB, margin 128 MiB, "out of
  space" under 64 MiB), the room rule (`disc_room_needed`), completeness with an explicit
  exact-versus-provider size (`disc_completeness`: a provider's SIZE may be run past, never stopped
  short of; iOS passes exact), the copy's verdict (`disc_copy_verdict`: the copy's own reason first
  for the game or a cut copy, finding 234), iOS's FILE copy loop and the publish generations. The
  volume comes from `disc_check_image` (`disc_check_file`), so the second PVD parser in C and the
  Java one (`DiscImage.volumeBytes`' own walk) are gone. Android reaches it through `DiscImage`'s
  natives in `disc_check_jni.c` (`rules`, `roomNeeded`, `verdictNative`, `publishBegin`, `publish`;
  `checkNative` now also returns the volume); `DiscImport` keeps the SAF stream, the provider's
  size and name, `StatFs` and its copy loop over the stream (the constants from C). Each pick takes a
  generation and lands through `disc_publish`, under its `JOBS` lock as before. iOS keeps the
  security-scoped URL, file coordination and the in-place move. Tests moved, not dropped:
  `test_disc_import.c` (test_runtime; room, both size kinds, the verdict, the racing publishers) and
  `DiscImageTest` (the JNI natives: constants, room, publication, the verdicts).
- **Android's import steps in launcher.log**: picker shown / cancelled / chosen / none, each import's
  document name and size (exact, a provider's, unknown), each problem, done. Names only, no URI.
- **Found by itself, not on Android**: the app's files folder is private and `Android/data` is
  closed to file managers since Android 11 (13 refuses even the picker), so a player has no folder
  to put an `.iso` in for the launcher to find; the picker stays the one way in. Not built.
- **What a player can see change**: Android's "copy the file onto the phone first" reads "onto the
  device first" (the one shared text); on iOS a copy cut short of its volume says the copy's reason
  ("stopped early" / "incomplete: try again") instead of the image's "incomplete" text, as Android
  does. Nothing else.

## Surprises

- The other golden groups are stale for reasons other than the driver: the replay goldens' captures
  (`gs-g+0.gs`, `gs-g+3600.gs` of a scratch folder) are gone and the meta has no capture hash, and the
  two capture sets kept beside the goldens give other pictures (`replay` 1020 / 222186 pixels,
  `replay-native` 1768); `native-transfers` and `-presented` gained scenes since (finding 176, 196:
  38 pictures, golden 33, renumbered). These need a recording from the oracle; they are left out of
  this PC's adopted golden (`10de-2484-32.0.16.2114-dx12`: test-gpu, test-gpu-ee,
  native-transfers-stress, 71 items).

## What was run

- Windows: `check.py quick`, `port` (with `native_replay`'s selftest and the new host tests
  `log-bundle`, `disc-import`) and `docs`; `native_replay --selftest` 31 cases; `gs_golden.py
  selftest`, `gs_golden_selftest.py --synthetic-only` (65 cases); `test_gs_wgpu.py --only gpu` (also
  with `DHD_TEST_EE_MXCSR=1`), `test_native_transfers.py` (plain, `--stress`, `--presented`) and
  `replay_gs.py` on the kept captures for the golden comparisons above; the adopt of this PC's golden.
- Android: the debug APK (native + Java, `build_apk.py --wasm`), its libmain.so exporting every new
  native (`DiscImage_*`, `Native_logBundle`, `Native_launcherLog`); `test_build_apk.py`;
  `test_disc_image.py` (`DHD_JNI_HEADERS` set: the Studio runtime has none) with the real USA disc:
  all passed. No phone touched.
- Mac: `port/ios/tests` (46, 2 skipped); the Simulator app from this branch with 7906a93f's core
  parts, no warnings. "DHD Lane 224" (iOS 26.5), through the launcher's test switch
  (`DHD_LAUNCHER_TEST`, the page's text and calls; no taps: no device host for the Mac in this
  session), the device's own disc and logs set aside and put back after:
  - `random dump.ISO` (fixture `other`) in Documents: the page said "It is another PlayStation 2
    disc (SLUS_203.12), not Downhill Domination (USA), SCUS-97177." and "Choose another file";
  - then `My Dump (USA).iso` (fixture `good`) beside it: moved into place as SCUS-97177.iso, "Ready
    to ride" (and the online check's "Offline only" for a fixture); the wrong file was not checked
    again (launcher.log);
  - `share_log`: the file `downhill-revived-0.8.3-log-2026-11-14.txt` with `device: Apple iPhone
    (arm64), iOS 26.5` and the sections dhd-exit.txt, launcher.log, dhd.log ("the last run"),
    dhd-previous.log ("the run before it"): the same four files in the same order as before.
  The work folder and the cold Rust build's cargo folder were removed after; the Simulator shut down.

## Result

Part 1: `native_replay` builds and its selftest is in the port tier; `gs_golden.py` works on this PC
again for the groups that a driver change does not touch (adopted), and says why for the rest.
Part 2: one Share log file for both phones. Part 3: one set of import rules for both phones;
Android's import steps are in Share log. Not proven here: Android on a device (no phone run in this
lane), taps in the iOS picker, a large real log in Share log on a phone.

Lead's checks on the owner's phone after the merge (read-only: its disc must not be replaced):
1. Install the merged build's APK; the launcher opens with the owner's disc accepted ("Ready to
   ride"), PLAY starts the game, EXIT GAME, back.
2. Help > Share log: the share sheet opens. The file (shared to a local target, e.g. saved to
   Files) starts `Downhill Revived 0.8.x log, shared DATE` / `device: <maker model>, Android ..
   (API ..)`, then `==== how the last run ended (dhd-exit.txt) ...` when there is one, `==== the
   launcher's steps (launcher.log) ...` when the picker was used, `==== the last run (dhd.log) ...`,
   `==== the run before it (dhd-previous.log) ...`; a dhd.log over 1.75 MB shows the
   `[... N bytes left out ...]` line with the settings lines above it.
3. The icon's long-press Share log shortcut: the same file, the launcher closes after the sheet.
4. Choose disc, then Back out of the picker (no file chosen): the disc stays accepted; Share log's
   launcher.log section then has `picker: shown` and `picker: cancelled`.

Checked:
- 0.8.4 gate on ad4b92f9: `check.py port`, `check.py server` and `check.py docs` OK; boot and race state
  traces equal the reference build.
- Mi 11i, release-signed PGO APK: Share log from the launcher made one bundle; Android unit tests green.
