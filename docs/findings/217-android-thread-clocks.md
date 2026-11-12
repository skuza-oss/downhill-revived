# 217: Android aborts when the perf log asks an ended thread for its CPU time

Status: current

Branch `fix/android-thread-clocks` from `main` c5f839a2, started 2026-11-12. This file is the
lane's plan while it runs and its finding when it merges. Merged into `main` on 2026-11-12
(3e80813e) and checked there the same day ("Checked on the merged main" below).

## Goal and acceptance

On Android (Mi 11i, main 1140b23f, release-signed) every EXIT GAME ended in
`invalid pthread_t ... passed to pthread_getcpuclockid` (SIGABRT, a tombstone, Android's record
CRASH_NATIVE status 6, the "closed unexpectedly" dialog at the next start), and once the same
abort came from the render thread 0.9 s after a clip in OPTIONS > VIDEO CLIPS ended. Goal: no
thread is ever asked for anything by handle after it ended, on any platform; `perf: session`
lines stay complete; the exit stays fast and Android's exit record says a clean exit.

Done when: the cause is written here; the fix is in with a unit test that fails without it;
Windows exit runs (hidden desktop) end with exit code 0 and a complete log; on the phone three
EXIT GAMEs (main menu, after a race, after a clip and two minutes in the menus) leave no abort, no
tombstone and a `previous run:` that says a clean exit; `check.py port` and `check.py docs` green.
Out of scope: the touch controls and the online room page (lane 218).

## Progress

- [x] 2026-11-12: finding reserved (37c3e46a)
- [x] 2026-11-12: cause found, fix and unit test (7e24f686); the cause and finding 210's
  corrections (790f032e)
- [x] 2026-11-12: `check.py port` green on 7e24f686 (with `test_host_threads`); Windows exit runs;
  release-signed APK; the phone proof (below)

## The cause

The perf log (`port/gs/perf_log.c`, finding 129) takes a snapshot once a second (render thread,
`perf_presented`) and at the exit (`perf_exit`): `host_thread_cpu` asked each named thread's CPU
clock by the handle it stored when the thread named itself (`host_thread_name`). Nothing ever
took a thread out of that list. On Linux and Android the handle is a `pthread_t`; once the thread
is joined (or a detached one has exited) it names nothing, and bionic (apps targeting API 26 and
later) checks every `pthread_t` it is handed against its thread list and aborts on an unknown one
(`__pthread_internal_find`, the tombstone's frame #01). glibc does not check (it reads the freed
thread struct, usually harmless), macOS's `pthread_mach_thread_np` reads the same freed struct, and
Windows kept a duplicated handle, which stays valid after the thread ends: so only Android aborted.

- **At every exit.** Finding 210 moved `perf_exit` from `atexit` (where, registered after the
  queues' handler, it ran first, before any join) to `HOST_END_REPORT`, after `HOST_END_WORKERS`
  (the `vu` and `gs` threads joined) and `HOST_END_RENDER` (the `native` thread told to quit and
  ended). Its snapshot then asked the joined threads: abort on the thread `game` in
  `host_end` -> `perf_exit` -> `snap` -> `host_thread_cpu` -> `pthread_getcpuclockid`, 3 of 3
  EXIT GAMEs on the phone. No `end: handlers done` line, CRASH_NATIVE status 6, the dialog after
  every normal exit.
- **After a movie.** The movie decoder thread (`port/hle/movie.c`) names itself "movie" and is
  joined when its clip ends (`release`). The next once-a-second snapshot on the render thread asks
  its `pthread_t`: abort in `native_thread` -> `perf_presented` -> `snap`, 0.9 s after the clip.
  This one is old: the released **0.7.16 has it** (tag `v0.7.16`: `decoder_thread` calls
  `host_thread_name("movie")` since 0.2.0, `release` joins it with `SDL_WaitThread`, and
  `perf_presented` takes a snapshot every second through the same `host_thread_cpu`). Any Android
  player who sees a movie end (VIDEO CLIPS, or the startup movies with Skip Movies turned off; the
  Android default skips them) gets the abort within a second, on 0.7.16 and every build since the
  perf log (finding 129). Desktop players were not affected.

## The fix

`port/host/host_threads.c` (the named-thread list moved out of `port/hle/host.c`, so a unit test
can link it):

- `host_thread_name` puts the thread in the list under a spinlock (never destroyed) and sets an
  SDL TLS value with a destructor. SDL runs TLS destructors on the thread itself after its
  function returns (`SDL_RunThread` -> `SDL_CleanupTLS`), before it ends and before any join can
  return. The destructor takes the lock, reads the thread's own final CPU time
  (`CLOCK_THREAD_CPUTIME_ID`, or `GetThreadTimes` of the current thread), keeps it and marks the
  entry ended (Windows: closes the duplicated handle).
- `host_thread_cpu` and `host_thread_cycles` ask only running entries, under the same lock: a
  thread cannot get past its destructor while it is being asked, so a running entry always names
  a live thread. An ended entry answers its kept time.
- A new thread of an ended thread's name (the next movie) takes that entry and adds to its time:
  the list no longer fills up after 16 registrations (each movie took a new slot before).
- `android_perf.c` (thread placement and performance hints) forgets an ending thread's tid and
  clock too (`android_perf_thread_ended`), so no affinity call or hint reads a dead thread.
- `host_thread_running(name)`: the number of running entries of a name (the unit test's check).

Every thread that names itself is an SDL thread (`movie`, `vu`, `gs`, `native`, `texpack`, the
game thread of `sdl_host.c`), except the process's main thread (`sdl`), which lives until the
process ends. The Rust side names none; the disc check (finding 208) and the end watchdog are not
named.

## The other end stages and ways out (checked, nothing else of this class)

Each handler of `host_end` (finding 210) against what an earlier stage ended:

| stage | handler | touches after the earlier stages | verdict |
|---|---|---|---|
| WORKERS | graphics queues (`graphics_shutdown`) | joins `vu`, then `gs` (`gs_queue_shutdown`), destroys their semaphores and frees the rings | nothing later uses them: the game thread is the one ending, the render thread takes frames from the native renderer's own queue; `graphics_wait_seconds` (the perf snapshot) reads atomic counters only |
| WORKERS | disc check (`disc_stop_at_exit`) | sets a flag | the thread is not named and not joined; `host_exit` ends it |
| RENDER | render thread (`native_stop`) | `quit`, waits up to 1 s for `thread_exited` under `queue_lock` (never destroyed) | the thread's TLS destructor runs after `thread_exited`; a snapshot in between asks a live thread (the lock) |
| REPORT | perf summary, native report, native VU1 counters, op trace | counters, `dhdr_perf_read` (atomics in the Rust library), files | the perf summary was the bug; the others read memory the stopped threads leave alone |
| DEVICES | audio (`host_stop`) | `SDL_DestroyAudioStream` | SDL unbinds the stream from its own audio thread under SDL's lock; no later stage plays sound |
| DEVICES | presence, vibration | the Rust presence worker's stop; pads under gamepad.c's `lock` (never destroyed) | the SDL thread keeps polling pads under the same lock; fine |
| LOG | log | drains the pipe | the copier thread is not named; `host_exit` ends it |
| after | end watchdog | detached SDL thread, not named | `host_exit` from it, or it returns |

The ways out without EXIT GAME all reach the same `host_end` on the game thread: the window's
close and Android's activity end (SDLActivity's `onDestroy` sends SDL's quit and waits up to 1 s
for the SDL thread: `gs_window_event` -> `window_closed` -> `gs_pump_messages`), the lost device
(`graphics_lost`, after its message box) and a failed start (`graphics_failed`). Each runs the
same stages, so each had the same abort until this fix; none needs more.

## Decisions

- **Keep each thread's final time when it ends**, rather than taking the session snapshot before
  the threads are joined: it covers every thread that ends at any time (the movie in mid-session),
  not only the exit, and `perf_exit` stays after the render thread's stop, so the session lines
  include the last frames. The `perf: session` lines are as complete as before finding 210.
- **SDL's TLS destructor** rather than a call at the end of each thread function: it runs on
  every return path of every SDL thread, so a thread added later cannot forget it.
- **A spinlock held across the clock reads** (at most 16 short system calls once a second): it is
  what makes "running" mean "alive"; a reader that only checked a flag could still be overtaken by
  the thread's end between the check and the call.

## The unit test

`port/host/test_host_threads.c` (run by `test_runtime.py`, so by `check.py quick` and `port`):
a thread names itself, burns about 120 ms of CPU and is joined: the list no longer counts it as
running (only running entries are asked by handle, so no handle of it is used after the join,
which is what can be checked on Windows, where a dead thread's handle still answers), and two
snapshots after the join report the same kept time of at least 60 ms. A second thread of the same
name takes the ended entry and its time adds. Then 600 short threads (4 at a time) name themselves
and are joined while another thread takes snapshots without pause (millions in the run): on
Android this is the movie abort, many times over; on Windows the check is the running count.
With the TLS destructor taken out (the old behaviour) the test fails 6 checks, among them
`FAIL: a joined thread is no longer running in the list`, `FAIL: the second thread of a name takes
the ended entry (2 entries)` and `FAIL: 600 joined threads: none running in the list (12)`. With
it: `joined: kept 0.125 s of 0.125 s burned`, `again: 0.125 s -> 0.250 s`, `ok`.

## Windows runs (hidden desktop, untimed, 2026-11-12)

Finding 210's driver, this branch's `dhd64.exe`. Each ended by itself with exit code 0, no
process left, the log ending with `end: closing (code 0)`, the session lines with every thread's
column filled, and `end: handlers done in N ms`; no watchdog line:

| run | ended after | handlers | session line |
|---|---|---|---|
| EXIT GAME from the main menu, sound on (muted) | its menu | 32 ms | `perf: session menus 7 s ... game=3.38/1.96 vu=3.00/3.19 gs=1.57/0.84 render=2.38/1.36` |
| the window's close in the menus | 0.15 s | 33 ms | `perf: session menus 7 s ... render=2.55/1.33` |
| the window's close in a race (GO passed) | 0.15 s | 3 ms | `perf: session race 46 s ... game=4.11/2.59 vu=3.49/3.30 gs=2.13/1.17 render=2.33/1.24`, then `perf: session menus 24 s ...` |
| the window's close at the renderer's start | 0.10 s | 1 ms | (no picture yet) |

## The phone (Mi 11i, Android 13, release-signed APK of 7e24f686, installed with `install -r`)

Before the first start the installed build was main 1140b23f, whose last run had aborted
(12:02:45); the first start of this APK showed the "closed unexpectedly" dialog for that run, as it
should. Then (logcat from just before each exit; `tombstone_09`, the old build's, stayed the
newest tombstone throughout):

| run | logcat and dhd.log at the exit | Android's record | next start |
|---|---|---|---|
| 1: EXIT GAME straight from the main menu | `end: closing (code 0)`, `perf: session menus 50 s ... game=6.91/3.65 vu=6.51/6.83 gs=1.74/1.92 render=4.07/2.79`, `end: handlers done in 63 ms` | `EXIT_SELF` status 0 | `previous run: how it ended: normally (2026-11-12 12:46:57 +0100)`, `previous run: system record: EXIT_SELF, status 0, importance 100 (on the screen), process 31162`; no dialog |
| 2: one race (Single Event, 44 s ridden, QUIT RACE), then EXIT GAME | `perf: session race 66 s ... render=5.18/4.74`, `perf: session menus 148 s ...`, `end: handlers done in 64 ms` | `EXIT_SELF` status 0 | `previous run: how it ended: normally (2026-11-12 12:50:53 +0100)` ... process 19233; no dialog |
| 3: OPTIONS > VIDEO CLIPS > INTRO, ended with SKIP (`movie: mov/attract.pss skipped`), 2 min 20 s in the menus (five `perf: menus 30 s` lines after the clip: about 140 snapshots after the movie thread's join), then EXIT GAME | `perf: session menus 264 s ... vu=6.31/6.64 gs=1.68/1.91 render=3.73/2.24`, `end: handlers done in 61 ms` | `EXIT_SELF` status 0 | `previous run: how it ended: normally (2026-11-12 12:55:34 +0100)` ... process 19234; no dialog |
| Home, then the task removed from the recents | MIUI kills the process at once (`ProcessSceneCleaner: OneKeyClean: kill procName=app.downhillrevived`, `Killing 19239 ... (adj 905): OneKeyClean`); no `onDestroy`, so no `host_end` and no end lines (dhd.log stops at `gs: in the background; the game waits`) | `OTHER KILLS BY SYSTEM`, status 0, importance 400, description `OneKeyClean` | `previous run: how it ended: normally (2026-11-12 12:57:37 +0100)`, `previous run: system record: OTHER, status 0, importance 400 (in the background), process 19239`, `previous run: description: OneKeyClean`; no dialog: right, the player removed it |
| 4: a start, then EXIT GAME | `end: handlers done in 60 ms` | `EXIT_SELF` status 0 | (not started again) |

In every exit's logcat: no `SIGABRT`, no `pthread_getcpuclockid`, no `FORTIFY`, no `Fatal signal`,
no `Tombstone written`. The md5 of `files/dhd.ini` and of the four files under `saves` were the
same before and after the session (nothing was saved). The swipe itself could not be sent: MIUI's
recents ignored the device tool's pan and fling on the card twice; its clear button, with the game
as the only card, removes the same task, and MIUI kills a removed task's process the same way.

## The `previous run:` order (finding 152)

Not an ordering bug. The check's note said a start after the crash at 11:41 described the older
crash of 11:23; the phone's own files show the chain is right: `dhd-previous.log` (the run that
crashed at 11:41) begins with `previous run: ... it crashed (2026-11-12 11:23:15 +0100)`, the run
before that one, and `dhd.log` (the run that crashed at 12:02) begins with
`previous run: ... 11:41:41`. A log's `previous run:` lines describe the run before that log, so
`dhd-previous.log`'s lines are two runs back from the newest start. Android writes the record at
the crash itself (`Force finishing activity` 0.3 s after the abort; `dumpsys activity exit-info`
lists every crash with its own time), long before a player can start the game again, so the 2 s
wait in `DhdActivity` is not involved. Nothing changed.

## Surprises

- The movie abort is in the released 0.7.16 (and older): it was not seen because Android's
  default skips the startup movies and VIDEO CLIPS is rarely opened.
- The list filled up: each movie took a new slot and none was given back, so after 16 named
  threads in a run the later ones were not counted (now an ended entry is reused).
- MIUI's recents kill a removed task with SIGKILL; the game gets no chance to end itself, and
  Android's record is `OTHER KILLS BY SYSTEM` (not "user requested"), which `CrashReport` already
  counts as a normal end.

## Checked on the merged main (2026-11-12)

On `main` 3073462c (c5f839a2, then the merges of lanes 217, 221, 220 and 219), by the lead's
verifier.

- `python tools/check.py port -j 6` (with `test_host_threads`) and `check.py server`: OK. The seven
  state-trace recipes equal the format-2 references made from 524948ba: 7 of 7 PASS.
- Windows, hidden desktop, pictures presented (no GS captures): EXIT GAME from the main menu
  (`DHD_NAV`: row 4, Yes, Quit) ended by itself with exit code 0, no process left, `end: closing
  (code 0)`, `perf: session menus 6 s ... game=1.75/1.45 vu=2.97/3.17 gs=0.85/0.95 render=2.20/1.21`
  and `end: handlers done in 33 ms`. The window's close (WM_CLOSE) in a race after GO (the state
  trace's race route and pad, 30 s after the race's loop began): ended 0.15 s after the close, exit
  code 0, no process left, `perf: session race 30 s ... game=2.34/2.06 vu=2.62/2.78 gs=1.40/1.34
  render=2.27/1.11`, then `perf: session menus 24 s ...`, `end: handlers done in 12 ms`. No
  watchdog line in either.
- macOS (M1, its own build of 3073462c, the app's executable started from the command line, about
  45 s, then SIGTERM, which SDL turns into its quit event as the window's close does): exit code 0,
  gone within a second, `end: closing (code 0)` and `end: handlers done in 12 ms`. No `perf:` line
  at all, at the end or before: the Mac's session was locked (`CGSSessionScreenIsLocked`), so no
  picture was presented, and the perf log counts presented pictures (its first one registers the
  session report). The session lines on macOS were therefore not seen; the earlier PC <-> Mac runs'
  Mac logs have none either, for the same reason.
- Not run here: the phone (another worker had it; the lane's own phone runs above stand).

## Result

The named-thread list lives in `port/host/host_threads.c`; an ended thread is never asked by
handle on any platform and keeps its CPU time, so the exit's `perf: session` lines are whole
again and the movie abort of 0.7.16 is gone. On the Mi 11i three EXIT GAMEs (main menu, after a
race, after a clip and two minutes of snapshots) and a fourth end with `end: handlers done in
60-64 ms`, Android's `EXIT_SELF` status 0 and no dialog; the task removal is recorded as the
player's, without a dialog. Windows exits as before (1-33 ms handlers).

For the lead: the seven state-trace recipes (`boot`, `race`, `race2` to `race6`) against the v3
references. They cannot differ: the change touches only which threads the perf log asks for CPU
times and when (host code that reads clocks and writes log lines); nothing in the core, the
game's memory or the order of game work changes, and no game state reads thread CPU times.
