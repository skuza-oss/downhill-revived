# 210: EXIT GAME on Android aborts in the C library while the process ends

Status: current

Branch `fix/android-exit-abort` from `main` 524948ba, started 2026-11-12, merged into `main` the
same day (18f5e70c). This file was the lane's plan while it ran and is its finding now.

## Goal and acceptance

On Android (seen three times on a Xiaomi Mi 11i, Android 13, offline and after an online session)
EXIT GAME ends the app, and at that moment logcat shows `FORTIFY: pthread_mutex_lock called on a
destroyed mutex`, then `Fatal signal 6 (SIGABRT)` in the thread named `native` (the native
renderer's thread). Android's exit record still says the app exited by itself with status 0, so
nothing is visible to the player and no tombstone exists (the process is gone before crash_dump
can attach). Goal: the game ends in one order on every platform, with no thread running while the
process tears down its statics, and no slower than before.

Done when: the exit path and the threads alive at exit are written down here; the fix is in;
on Windows EXIT GAME, the window's close and a start followed by an immediate exit each end
promptly with dhd.log complete, settings written and no process left; a unit test fails without
the fix; the Android debug APK builds; `check.py port` and `check.py docs` are green. Out of
scope: the phone itself (no device in the lane), the game's own `Exit()` (module side, ends with
`host_exit` already).

## Progress

- [x] 2026-11-12: finding reserved (003496e8)
- [x] 2026-11-12: the exit path read, the fix written (3b6a2f6a): `port/host/host_end.c`
- [x] 2026-11-12: Windows hidden-desktop runs, unit tests, `check.py port`, Android debug APK
- [x] 2026-11-12: merged (18f5e70c) and checked on `main` on Windows ("Checked on the merged main")
- [x] the phone check (below), by the lead, on `main` 1140b23f: the FORTIFY abort was gone, but
  every EXIT GAME then aborted in `perf_exit` (`invalid pthread_t ... passed to
  pthread_getcpuclockid`): its new place in `HOST_END_REPORT`, after the joins, asked the joined
  threads' CPU clocks. Fixed by finding 217, which also has the phone runs.

## The exit path

EXIT GAME (`port/hle/exit_menu.c`, module side) calls `host_service_gs_window_close`
(`present.c` `gs_window_close`), which posts SDL's close request. The SDL thread (`sdl_host.c`)
hands it to `gs_window_event`, which sets `window_closed`. The next `gs_pump_messages`, on the
game thread, called `exit(0)`. The window's close button, the activity closing on Android (SDL's
quit event), `graphics_lost` (finding 171, `exit(1)`), `graphics_failed` and the main thread's
return (`launcher.c`, `android_main.c`) all ended the same way: C `exit()` on whichever thread
reached it. (The game's own `Exit()` and the test tools' stops run in the module and reach
`host_exit`, which is `_exit` and runs nothing; they never had the problem.)

`exit()` runs the `atexit` handlers, then every loaded library's destructors, then ends the
process. The threads alive at that moment:

| thread | state at exit |
|---|---|
| `native` (render, `native_core.inc`, detached) | drawing the next field, or in the driver's present |
| `vu`, `gs` (queue workers, `graphics_queue.c`, `gs_queue.inc`) | joined by an `atexit` handler first |
| SDL's audio thread | stopped by an `atexit` handler (finding on #56 in `sound_out_sdl.c`) |
| `sdl` (main: events, pads, window ticks) | looping until the process ends |
| log copier (`host.c`) | draining the pipe |
| `dhd_net` threads (Rust) | only online; blocked in sockets |
| disc check (finding 208), heartbeat, freeze watch, watchdog | sleeping or hashing |

Nothing stopped the render thread. It is detached (`native_thread_start`), and nothing set
`Native.quit`, which only the thread itself read.

## The mutex

Not named for certain: the phone gave only an address, no backtrace (no tombstone). The evidence
points at the GPU driver, not at the game's own locks:

- The aborting thread is `native`, which spends its life in `dhdr_native_*` and wgpu's Vulkan
  calls (dhd.log: `Adreno (TM) 660 on Vulkan, driver Qualcomm ... Adreno Vulkan Driver`).
- The game's own locks (`context_lock`, `queue_lock`, the queue semaphores) are never destroyed
  anywhere in the code (grep `SDL_DestroyMutex`: only `port/net/client.c`, not used here).
  The renderer is leaked at exit on purpose.
- The audio stream was closed 11 ms before the abort (logcat: `AAudioStream_close`), so the
  `atexit` handlers were running; the abort comes after them, in the destructor phase.
- bionic's destroyed-mutex check applies to any `pthread_mutex_t` that `pthread_mutex_destroy`
  touched; a driver library's static C++ object (a `std::mutex` wrapper, a pthread mutex in a
  singleton) is destroyed by its library's destructor during `exit()` while `native` is inside
  `vkQueueSubmit` or the present. The address `0x7af2...` is in the range of loaded libraries and
  heap, not of the game's statics (not provable without the maps).

So: the mutex is most likely the Vulkan driver's (or the Vulkan loader's), destroyed by `exit()`.
Confidence: moderate. The fix does not depend on the name: after it no thread runs any library
code when destructors would run, and no destructor runs at all.

Windows, macOS and Linux have the same order of events (`exit()` under a drawing render thread);
glibc and the Windows CRT do not abort on it, so it was silent there.

## The fix

`port/host/host_end.c` (`host_end.h`):

- `host_end(code)` ends the game from any thread. It runs registered end handlers in stages
  (`HOST_END_WORKERS`: the VU and GS queues, the disc check's flag; `HOST_END_RENDER`: the render
  thread; `HOST_END_REPORT`: the closing log lines (`perf: session`, native VU1 counters, the
  native report; they run after the threads are gone, so they may not ask a thread by its handle:
  the named-thread list keeps an ended thread's time, finding 217); `HOST_END_DEVICES`: audio, presence, vibration; `HOST_END_LOG`: the log's last
  lines), flushes every open file (`fflush(NULL)`), and leaves with `host_exit` (`_exit` /
  `ExitProcess`): no `atexit`, no library destructors, whatever threads are still running.
- The render thread (`native_stop` in `native_core.inc`) is told to quit (`Native.quit`, which
  the loop already read) and waits up to 1 s for `thread_exited`. The queues stop first, so the
  render thread still takes their last fields.
- A watchdog thread (`DHD_END_TIMEOUT_MS`, 3000 by default) ends the process with the same code
  if a handler hangs, logging which one. A handler that calls `host_end` again, or another
  thread doing so, does not deadlock.
- Every old `atexit` registration moved to `host_end_add` (`graphics_queue.c`, `perf_log.c`,
  `native_core.inc`, `vu1_mesh.inc`, `gpu_exact.inc`, `gamepad.c`, `sound_out_sdl.c`,
  `host_sdl.c`, `host.c`'s presence and log). `host_end_add` also hooks `atexit` once, so a path
  that still calls `exit()` runs the same handlers once (then the destructors, with the render
  thread stopped).
- `gs_window_close` (no window), `gs_pump_messages`, `graphics_lost`, `graphics_failed`,
  `android_main.c` (no disc, the game's return) and `launcher.c`'s `main` call `host_end`.
  On the browser (`__wasm__`) `host_end_add` is `atexit`.

Why `_exit` and not only a tidy join: joining the render thread removes the abort seen, but the
SDL thread, the log copier and the Rust threads run beside the destructors too, and every library
loaded into the process (the driver, SDL, the C++ runtime) has destructors the game does not own.
Nothing in them is needed: the log, the settings (`dhd_settings` writes a temp file and renames it
at the call) and the
memory card (`host_file_*` FILEs, flushed by `fflush(NULL)`) are written by the handlers and the
flush. Android's exit record is unchanged: the process still ends by itself with status 0 (`exit`
ended with the same `exit_group` call). (Not so on `main` from this merge until finding 217:
the report stage aborted, and Android recorded CRASH_NATIVE status 6.)

## What was run (Windows, hidden desktop, untimed, 2026-11-12)

Each run: the game ends by itself, `end: closing` then the `perf: session` lines then
`end: handlers done in N ms` are in the log, exit code 0, the process is gone.

| run | ended after | handlers |
|---|---|---|
| EXIT GAME from the main menu (`DHD_NAV`, Yes, Quit) | at once; whole run 8 s | 4 ms |
| EXIT GAME with sound on (muted) | at once | 27 ms |
| window close in the menus | 0.3 s | 26 ms |
| window close in a race (GO passed) | 0.46 s | 8 ms |
| window close in a race's pause menu | 0.15 s | 12 ms |
| window close the moment the window exists, and at the renderer's start | 0.10 s | 2 ms |

The pause menu has no EXIT GAME row (only the main menu does, `patches_exit.py`), so the pause
menu was ended with the window's close button. The unit tests (`port/host/test_host_end.c`, run
by `test_runtime.py`): handler order by stage, a missed `exit()` path runs the handlers once, a
thread joined before the destructor runs, the same thread without the stop handler sees the
destructor under it (`unjoined`: the abort in miniature; exits 42), a hanging handler is cut off
by the watchdog, a handler that ends the process itself, and a buffered file reaching the disk.
`check.py port` green. `build_apk.py` builds the debug APK (`host_end` is in `libmain.so`).

## Decisions

- `host_end` runs the handlers on the calling thread and leaves with `host_exit`, rather than
  joining and then calling `exit()`: see "The fix". The cost is that a library's own destructors
  never run; none is needed.
- One registry with stages, not `atexit` order: the render thread must stop after the queues
  (they hand it fields) and before the closing log lines; `atexit` order follows the first use.
- The fatal `exit(1)` calls in `gpu_rust.c` (a failed readback, on the GS worker) stay: they run
  on a thread that the end would have to join. They hit the `atexit` hook and the watchdog.
- The SDL thread keeps running during the end (an idle-while-ending flag was tried and dropped:
  the present may need its window events to finish a frame).

## Surprises

- The old exit already joined the queue workers and closed audio at `atexit` (finding on #56),
  so the render thread was the one left. The same bug class, one thread later.
- On Windows a worker thread's `_exit` races the exiting thread's `ExitProcess` (the first test
  version reported exit 0 although the violation was seen); the test now ends the process from
  the exiting thread.

## Checked on the merged main (2026-11-12)

On `main` 18f5e70c (with finding 202's round 4 and finding 215), by the lead's verifier, Windows,
hidden desktop, untimed:

- `python tools/check.py port -j 6` (with `test_host_end`) and `check.py server`: OK. The seven
  state-trace recipes equal the format-2 references made from 524948ba: 7 of 7 PASS.
- Each run below ended by itself with exit code 0, no process left, and dhd.log ending with
  `end: closing (code 0)`, the `perf: session` line (where a session ran) and
  `end: handlers done in N ms` (1-6 ms); no watchdog line.

| run | ended | data at the next start |
|---|---|---|
| EXIT GAME from the main menu after DIFFICULTY changed and SAVE GAME DATA to an empty card | by its menu | dhd.ini (`game_options`, `install_id`) and the card's four files present; the next start restored the options (`options: restored`, the same values) and the card's files kept their hashes |
| the window's close in the main menu | 0.15 s after the close | nothing saved in that run; the next start then EXIT GAME, clean |
| the window's close in a race (GO passed) | 0.15 s | |
| the window's close the moment the window exists | 0.30 s | |
| a P2P room made from ONLINE (the network threads running), LEAVE in the room, then EXIT GAME | by its menu | next start then EXIT GAME, clean |
| the launcher (`DownhillRevived.exe --play`, a packaged copy), EXIT GAME | the launcher ended with code 0, 9.8 s after its start | no process left from the package |

Open: the phone check below.

## Result

The game ends through `host_end` on every platform. State traces cannot differ: nothing runs
before the end of the process changes; the registrations replace `atexit` calls, the render
thread's loop is untouched until `quit` is set, and a trace run ends through `host_exit` (module
side) or `host_end` after its last field with its files flushed. The phone check (done, see Progress and finding 217):
EXIT GAME three times, offline and after an online session, with `adb logcat` free of `FORTIFY`
and `SIGABRT`, dhd.log ending with `end: handlers done`, and the next start's `previous run:`
lines still saying the app exited by itself.
