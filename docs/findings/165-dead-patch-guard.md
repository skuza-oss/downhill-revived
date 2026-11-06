# 165: no port patch in code the port does not compile

Status: in progress

Branch `fix/dead-patches` from `main` 1ff885d, started 2026-11-06. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

Finding 159 (Surprises) found that `func_001B40A0` (the race loop) is NONMATCHING C, so the core
runs its recompiled asm and the patch in `port/patches.py` that calls `hle_online_watch_resume()`
there never ran. The lane:

1. Makes the build refuse any patch whose text falls in source the port does not compile (an
   `#ifdef NONMATCHING` block, whose function the core runs from recompiled asm, or any other
   branch the port's preprocessor drops), naming the patch and the function; a unit test for the
   guard; every patch it finds fixed.
2. Moves the spectate resume (finding 122) to a hook that runs, with an online run against a
   local server and a bot that shows the bug and the fix.
3. Checks: `python tools/check.py port` and `check.py server` pass; boot and race state traces
   equal to the base build's.

Out of scope: any other change to online state sync.

## Progress

- [ ] 2026-11-06: finding reserved

## Decisions

## Surprises

## Result
