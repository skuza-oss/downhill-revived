# 236: the background noticed only at the return (Android, in a race)

Status: in progress

Branch `fix/android-background-miss` from `main` f9cbce3d, started 2026-11-14. Follows finding 227
(the render hold, `app_away_wait`).

## Goal and acceptance

On the Mi 11i, Home in a SINGLE EVENT race and back: some cycles (3 of 6 on the published 0.8.2,
3 of 7 on f9cbce3d) logged `freeze: no field for 5 s` and `gs: in the background` only at the
return (`back in the foreground after 0.1 s`). The game thread must reach the background's wait
well under 1 s after Home every time. Done: at least 20 Home/return cycles on a release-signed APK
of the branch with no `freeze:` line, `gs: in the background` within 1 s of Home each time, PAUSED,
picture and sound back; `check.py quick`, `port`, `docs`; the boot state trace equal to the base.

## Progress

- [x] 2026-11-14: cause established on the phone (temporary `diag236:` lines, not committed)
- [x] the fix (`graphics_away_wait`), unit test in `test_graphics_queue.c`
- [ ] checks
- [ ] 20 cycles on the phone

## The cause

The platform's callback holds the render thread (finding 227) but does not wait for the game
thread; the game thread is meant to reach `app_away_wait` at its next field (`gs_vsync`). In the
deferred vsync the game thread first waits for the drawing (`graphics_vblank`'s wait for the
picture of the field before last, a fence, a full queue: `graphics_queue.c`). With the render
thread held, the field it has queued is never taken, the GS worker waits at its handoff
(`native_core.inc`, one field pending) and the picture marks stop: when that happens before the
game thread's own `gs_vsync`, its wait lasts the whole absence. The return's `gs_app_back`
releases the hold, the wait ends, and only then the game thread reaches `app_away_wait` ("after
0.1 s"). The diagnostic build on the Mi 11i, 2 bad cycles of 4:

```
diag236: 179.882 will enter background
diag236: 179.886 the GS worker waits at the handoff, the renderer held (field 8599)
diag236: 179.900 the app is away while the game thread is in a wait for an earlier picture
freeze: no field for 5 s at field 8601: menu 9 phase 3, render flags 0 0 0
gs: in the background; the game waits (the renderer held, no surface)
gs: back in the foreground after 0.1 s; the game clock goes on from now
```

Clean cycles are those where the game thread got to `gs_vsync` before the pipeline ran dry. Only
the time is lost (the sound pause and the freeze watch's "away" came late); nothing was drawn while
away. iOS has the same order (its callback holds the render thread and waits only for it to be
quiet), so the same miss is possible there while the app is inactive but not yet suspended.

## Decisions

- The fix is where the game thread waits, not in the hold: `graphics_queue.c`'s EE-side waits
  (`graphics_wait`, `graphics_gs_wait`, a full ring, `wait_mark`) call `graphics_away_wait`, which
  present.c sets: on the game thread with the app away it runs `app_away_wait` there (the same wait
  as at a field: the sound paused, the freeze watch told, the surface made again, the hold released
  for that absence), and the drawing wait then finishes on the new surface. Letting the held render
  thread take fields instead would drop their uploads or draw while away (finding 227's rule).
- One path for Android and iOS: `app_away()` is the same test there, so iOS gets it too. Desktops
  never set it.
- The log line says where the wait was: `gs: in the background; ... no surface), from a wait for an
  earlier picture`.

## Surprises

## Result
