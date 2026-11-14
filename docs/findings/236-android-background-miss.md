# 236: the background noticed only at the return (Android, in a race)

Status: current
Checked: 2026-11-14

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
- [x] `check.py quick`, `port`, `docs` OK; the boot state trace equals `main-524948ba-ref-v3`
  (600 rows)
- [x] 22 cycles on the phone (release-signed APK of a18319e3, Mi 11i), below

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

- The miss is common, not rare: 16 of the 22 fixed cycles entered the background from the VBLANK's
  wait for an earlier picture, the path that used to block. A cycle was clean before only when the
  game thread reached `gs_vsync` first.
- `test_graphics_queue.c` now models it: a consumer stuck in its packet (the held renderer) until
  the away hook lets it go, behind a fence and behind a queue filled past its size; without the
  hook the wait never reaches it and the consumer gives up after 3 s (a failure).

## Result

The Mi 11i, a SINGLE EVENT race, one process: 22 Home/return cycles (2 s x9, 10 s x8, 30 s x3,
two quick Home presses once, the recents screen once): 22 `gs: in the background` (16 `from a
wait for an earlier picture`), 22 `back in the foreground` (1.3-1.4 s for 2 s away, 9.4-9.5 s for
10 s, 29.4-29.5 s for 30 s: the wait began within about 0.5 s of the Home command being sent),
22 `touch: back from the background in a race: the game's pause`, the media stream started again
after each return (`dumpsys audio`), the picture back, zero `freeze:` lines, no new tombstone.
EXIT GAME, the next start "how it ended: normally". The diagnostic build before the fix: 2 of 4
cycles bad, as above.

Checked:
- 0.8.3 gate on 128a743a: `check.py port -j 6`, `check.py docs` and `check.py server` OK; the seven state traces
  (`boot`, `race`, `race2`..`race6`) equal `main-524948ba-ref-v3`.
- Mi 11i, release-signed PGO APK, a SINGLE EVENT race: 8 Home/return cycles (2 s x4, 10 s x3, 30 s x1; the log's time
  away 3.2 to 31.4 s): 8 `gs: in the background`, 8 `back in the foreground`, 8 PAUSED
  screens, zero `freeze:` lines.
