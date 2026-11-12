# 210: EXIT GAME on Android aborts in the C library while the process ends

Status: in progress

Branch `fix/android-exit-abort` from `main` 524948ba, started 2026-11-12. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

On Android (seen three times on a Xiaomi Mi 11i, Android 13, offline and after an online session)
EXIT GAME ends the app, and at that moment logcat shows `FORTIFY: pthread_mutex_lock called on a
destroyed mutex`, then `Fatal signal 6 (SIGABRT)` in the thread named `native` (the native
renderer's thread). Android's exit record still says the app exited by itself with status 0, so
nothing is visible to the player and no tombstone exists (the process is gone before crash_dump
can attach). Goal: the game ends in one order on every platform, with no thread running while the
process tears down its statics, and no slower than before.

Done when: the exit path and the threads alive at exit are written down here; the fix is in;
on Windows EXIT GAME (menus and a race's pause menu), the window's close button and a start
followed by an immediate exit each end promptly with dhd.log complete, settings written and no
process left; a unit test fails without the fix; the Android debug APK builds; `check.py port`
and `check.py docs` are green. Out of scope: the phone itself (no device here), the game's own
`Exit()` (module side, ends with `host_exit` already).

## Progress

- [ ] 2026-11-12: finding reserved (this commit)

## Decisions

## Surprises

## Result
