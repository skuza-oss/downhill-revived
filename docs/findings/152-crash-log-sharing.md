# 152: Crash logs players can keep and share (Android first)

Status: current

Branch `feat/crash-log` from `main` f7be0e2, started 2026-11-05. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

On Android a player cannot reach `dhd.log`: it lives in the app's private files folder, a new
start overwrites it (so the crash report of the run before is gone), and the app has no way to
share it. Issue #47 (Mali-G57 driver crash at startup) only has a log because its reporter used
adb; a Discord player with the same phone (Galaxy A07) could send nothing. Goal: after any crash a
player can send the log in two taps, with no computer and no new library.

1. **The run before is kept, every platform.** At start, before the new `dhd.log` is opened, an
   existing `dhd.log` becomes `dhd-previous.log` (replacing an older one). One code path
   (`port/hle/host.c` `host_console_init` / `log_start`), Windows, macOS, Linux and Android alike.
   Tools that hand the game their own log file or pipe are unchanged.
2. **Why the last run ended (Android).** The first activity (`SetupActivity.java`, which runs
   before any native code) reads `ActivityManager.getHistoricalProcessExitReasons` (API 30, our
   minSdk) for the app's last exit. A crash, a native crash, an ANR, low memory or an
   excessive-resource kill counts as "closed unexpectedly"; a normal exit, the player swiping it
   away or a user-requested stop does not. The reason, its description, its time, and for native
   crashes the system's trace (`getTraceInputStream`, a tombstone: written as a readable summary
   if it is the protobuf form, else as text, capped in size) are written to
   `files/dhd-exit.txt` and, at the next native start, quoted into the new `dhd.log`
   (`previous run: ...`), so a log always says how the run before it ended.
3. **The dialog.** When the last run closed unexpectedly, before the game starts, a plain dialog:
   "Downhill Revived closed unexpectedly last time" plus the reason in a few words (crashed / the
   graphics driver crashed / ran out of memory / stopped responding), with **Share log** and
   **Play**. Shown once per such exit (remember the exit's timestamp). Back = Play.
4. **Share log.** Android's share sheet (`Intent.ACTION_SEND`, `Intent.createChooser`) with one
   text file attached: `dhd-previous.log` (or `dhd.log` if there is no previous one), with
   `dhd-exit.txt`'s text at its top, copied into a share file first. Served by a tiny
   ContentProvider of our own (read-only, one folder, `grantUriPermissions`, not exported), not
   androidx FileProvider. The file name says the version and date, e.g.
   `downhill-revived-0.7.10-log-2026-11-05.txt`.
5. **Always reachable.** A static app shortcut (long-press the icon): "Share log", opening the
   same share flow without starting the game, for a phone that crashes before any dialog helps.
6. **Readable backtraces.** The release keeps the unstripped `libmain.so` (and the desktop
   builds' symbols where they exist) beside the release files, so `libmain.so+0x...` lines can be
   mapped to functions (`llvm-symbolizer` / `llvm-addr2line`); `docs/BUILDING.md` "Release
   steps" and `docs/RELEASE-CHECKLIST.md` say so. Check what build_apk.py / build_host64.py
   already keep before adding anything.

Acceptance:
- `python tools/check.py quick` and `python tools/check.py docs` pass; `check.py port` passes on
  the merged tree (the lead runs it).
- `python port/android/build_apk.py` builds; the APK has the provider and the shortcut.
- On a phone (the lead): start, force a crash (an adb `kill -SEGV` of the game process), start
  again: the dialog appears once with "crashed", Share log offers apps, the shared file has the
  crash lines and the exit reason; the icon's long-press shortcut shares it too; a normal exit
  shows no dialog.
- Desktop: a second start leaves `dhd-previous.log` with the first run's lines.

Out of scope: uploading logs anywhere (nothing leaves the phone except through the share sheet the
player opens), a crash dialog on desktop, changing what the crash handler writes, any game-state
change.

## Progress

- [x] 2026-11-05: finding reserved
- [x] 1. `host.c` `log_keep_previous`: dhd.log becomes dhd-previous.log in both `console_open`
  branches (Windows `MoveFileExW`, POSIX `rename`), only when the game opens dhd.log itself. The
  launcher's updater (`update.rs` KEEP) and the uninstaller (`windows.iss` IsPlayerFile) count it
  as a player's file; README, PLAYING.md and the bug report form name it.
- [x] 2. `LastExit.java` reads `getHistoricalProcessExitReasons` once per process before native
  code (SetupActivity, and DhdActivity for a USB-controller start that skips it) and writes
  `files/dhd-exit.txt`; `android_main.c` `quote_last_exit` puts its lines in the new dhd.log as
  `previous run: ...`. The tombstone (protobuf) summary and the reason rules are plain Java
  (`CrashReport.java`) with a JVM test (`port/android/test/CrashReportTest.java`, run by
  `test_disc_image.py`).
- [x] 3. SetupActivity's dialog (`showClosedUnexpectedly`), once per exit (SharedPreferences
  `crash-log`/`shown` = the exit's timestamp); Share log keeps the dialog for Play; Back = Play.
- [x] 4. `LogShare.java` + `LogProvider.java` (not exported, read only, `cache/share/` only,
  answers OpenableColumns); file `downhill-revived-<version>-log-<date>.txt`.
- [x] 5. Static shortcut `res/xml/shortcuts.xml` (Share log) on SetupActivity, action
  `app.downhillrevived.SHARE_LOG`: the share sheet only, no game.
- [x] 6. `package_release.py --platform android` keeps `DownhillRevived-<version>-android-libmain.debug.so`
  beside the APK when its GNU build ID is the APK's libmain.so's; `write_sums` leaves out
  `KEPT_SUFFIXES` (`.map`, `.debug.so`). BUILDING.md "Release steps" and RELEASE-CHECKLIST.md
  item 7 say how to keep and use them.
- [x] 2026-11-05 checks: `check.py quick` OK; `test_disc_image.py` (CrashReportTest PASS);
  `build_apk.py` debug APK built, its manifest has `LogProvider` (exported false,
  grantUriPermissions) and the `share_log` shortcut on SetupActivity; `build_host64.py` win-x64
  builds. Not run: the game itself on a desktop (the dhd-previous.log check) or on a phone.
- [x] 2026-11-05 on the Mi 11i (Android 13), release-signed build over 0.7.10 (the lead): a crash
  of the game thread at start (a test-only switch on a test branch) is recorded by Android as
  `APP CRASH(NATIVE)` with a tombstone; the next start shows the dialog ("It crashed."); Share log
  opens the share sheet with `downhill-revived-0.7.10-log-2026-11-05.txt` holding Android's record,
  the tombstone summary ("Cause: null pointer dereference", frames with build IDs) and the run's
  full crash report; Play starts the game; a start after a user stop shows no dialog; the
  `SHARE_LOG` action opens the share sheet only and Back ends it; the `share_log` shortcut is
  registered. `check.py port` OK on the merged tree. Three faults were found and fixed on the way
  (Surprises). Not run: the desktop dhd-previous.log check by hand.

## Decisions

- **The crash handler hands the crash on (Android).** host.c's POSIX `crashed` ended with
  `_exit(3)`: Android then records `REASON_EXIT_SELF` status 3 and writes no tombstone, so item 2
  had nothing to read. Now, after its report (unchanged) and `host_log_drain`, on Android it puts
  back the handler that was installed before it and returns: the faulting instruction runs again
  and faults into that handler (a signal sent with kill is raised again instead). Guest memory's
  handler (`port/host64/guest_memory_posix.c`) does the same with the one before it for a fault
  that is not its own, down to the system's crash dump (debuggerd), which writes the tombstone and
  records `REASON_CRASH_NATIVE`. A desktop exits with code 3 as before (no macOS crash reporter
  dialog, no core dumps). EXIT_SELF status 3 and SIGNALED by a crash signal still count as
  "crashed".
- **Which log is shared.** The plan said dhd-previous.log, but the dialog runs before the native
  game, when dhd.log is still the run that crashed. The shared file holds both logs: first the run
  that ended (dhd.log, or dhd-previous.log when a dhd.log was written since this process started,
  i.e. the game is running now), then the other; each over 1.75 MB keeps its first 256 KB and last
  1.5 MB. Above them: the phone, and how the last run ended (dhd-exit.txt).
- **Low memory and excessive resource use** count only when the game was on the screen
  (importance <= VISIBLE): Android ends a game left in the background for memory all the time.
- **"The graphics driver crashed"**: a native crash whose frame #00 is in a GPU driver library
  (Vulkan loader, Mali, Adreno's libgsl, PowerVR, Xclipse; `CrashReport.DRIVER`).
- **A stale record** (older than dhd.log's last write by over 5 s) is not about that run:
  dhd-exit.txt then says Android kept no record.
- **The shortcut targets SetupActivity** (already exported, the launcher activity) with its own
  action, so no new exported component; the provider is the only new one and is not exported.

## Surprises

- The crash report could miss dhd.log on Linux, macOS and Android: `crashed` called `_exit` with
  the report still in the log pipe (the copying thread dies with the process). It drains first now.
- Found on the phone (2026-11-05), all fixed:
  - SetupActivity called `hideBars` before any `setContentView` on the dialog and Share log paths:
    the window has no decor view yet, `getInsetsController` threw, and every start after a crash
    closed again at once. A plain backdrop view comes first now.
  - The report's stack walk (`host_readable`) probes past the stack's end with a fault guard, but
    inside the crash handler the fault signal was still blocked, and a fault on a blocked signal
    ends the process at once: a crash near the top of a thread's stack left only the report's
    first line and no tombstone. The handler unblocks SIGSEGV and SIGBUS before the report. This
    predates the lane (every POSIX platform).
  - The lane's first chain called the handler before ours directly; on Android that handler is
    guest memory's, not debuggerd's, and it answered a fault not its own with the default action:
    Android recorded `SIGNALED` and wrote no tombstone. Fixed as in Decisions.
  - An external `kill -SEGV` lands on the Java main thread and is no test of a native crash; the
    phone check used a test-only switch that faults the game thread at start.
- `at += length()` in Java reads `at` before `length()` moves it: the JVM test's tombstone caught
  it (every length-delimited field skipped was off by its length's bytes).

## Result

2026-11-05: after a crash on Android the next start says the game closed unexpectedly and why
(crashed, the graphics driver crashed, ran out of memory, stopped responding), with Share log
(Android's share sheet, one text file: the phone, Android's record of how the run ended with the
tombstone's summary, then the logs) and Play; the icon's long-press has Share log too. Crashes now
reach Android's crash dump (tombstone, `CRASH_NATIVE`), and the crash report always reaches
dhd.log. Every platform keeps the run before as dhd-previous.log. The release keeps the unstripped
libmain.so beside the APK (never published) to name backtraces' frames.
