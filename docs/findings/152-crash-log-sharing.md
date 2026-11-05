# 152: Crash logs players can keep and share (Android first)

Status: in progress

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

- [ ] 2026-11-05: finding reserved

## Decisions

## Surprises

## Result
