# 217: Android aborts when the perf log asks an ended thread for its CPU time

Status: in progress

Branch `fix/android-thread-clocks` from `main` c5f839a2, started 2026-11-12. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

On Android (Mi 11i, main 1140b23f) every EXIT GAME ends in `invalid pthread_t ... passed to
pthread_getcpuclockid` (SIGABRT, a tombstone, the "closed unexpectedly" dialog at the next start),
and once the same abort came from the render thread after a movie ended. Goal: no thread is ever
asked for anything by handle after it ended, on any platform; `perf: session` lines stay complete;
the exit stays fast and Android's exit record says a clean exit.

Done when: the cause is written here; the fix is in with a unit test that fails without it;
Windows exit runs (hidden desktop) end with exit code 0 and a complete log; on the phone three
EXIT GAMEs (main menu, after a race, after a clip and two minutes in the menus) leave no abort, no
tombstone and a `previous run:` that says a clean exit; `check.py port` and `check.py docs` green.

## Progress

- [x] 2026-11-12: finding reserved

## Decisions

## Surprises

## Result
