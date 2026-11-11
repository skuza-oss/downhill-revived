# 209: Touch controls can skip the flight over the track

Status: in progress

Branch `fix/touch-skip-flyover` from `main` ad0135bd (0.7.16 plus findings 199-206), started
2026-11-12. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

GitHub issue #89 (Android): the camera's flight over the track before every race cannot be skipped
with the touch controls. On a PC START skips it; on a phone nothing is shown at that moment and a
tap does nothing.

Acceptance: with the touch controls on at a phone's shape (20:9) the flight is skipped by touch in
both presets (Classic pad, Thumb zones), shown in picture captures at 4x; `test_touch` covers the
new rules; the Android target compiles; `python tools/check.py port` and `check.py docs` pass;
`docs/PLAYING.md` says what the touch controls show. Nothing changes with a controller or a
keyboard, nor offline without touch (state traces: left for the lead). Out of scope: the game's and
the room's rules about when the flight may be skipped; a phone run (the lead's).

## Progress

- [x] 2026-11-12: finding reserved
- [ ] cause found in the decomp and the touch code
- [ ] fix and unit tests
- [ ] Windows captures, both presets
- [ ] checks: `check.py port`, `check.py docs`, the debug APK

## Decisions

## Surprises

## Result
