# 217: Android aborts when the perf log asks an ended thread for its CPU time

Status: in progress

Branch `fix/android-thread-clocks` from `main` c5f839a2, started 2026-11-12. This file is the
lane's plan while it runs and its finding when it merges.

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
- [x] 2026-11-12: cause found, fix and unit test (7e24f686)

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

## Surprises

## Result
