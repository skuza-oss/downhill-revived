# 214: A D-pad as a touch steering choice, and the touch gaps beside the flyover

Status: in progress

Branch `feat/touch-dpad` from `main` dbddf61e (0.7.16 plus findings 199-207 and 209), started
2026-11-12. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Part 1 (GitHub issues #88, #86): a traditional four-button D-pad (up, down, left, right) as a third
choice of the setting that picks the touch steering (`touch_steer`: Stick, Tilt, D-pad), in both
presets (Classic pad, Thumb zones) and both hands. Part 2 (from finding 209's open list): where a
controller's button does something in the front end and a finger cannot: the startup movies and the
attract demo, the results screens, replays, "press START" prompts; each reachable by touch in the
overlay's own style (209's SKIP button is the model).

Acceptance: on Windows with the touch controls at 20:9, the D-pad shown and steering a rider in
both presets and both hands, switched live from the game's settings page, a diagonal and a slide in
the touch log, the menus still navigable; captures at 4x in a scratch folder; unit tests
(`test_touch`) and the settings crate's tests; the Android debug build compiles; the browser
build compiles if its corpus changed; `python tools/check.py port` and `check.py docs` green;
`docs/PLAYING.md` updated. Nothing changes with a controller or keyboard, nor offline without
touch (state traces: left for the lead). Out of scope: a phone (the lead's), changing what is
skippable.

## Progress

- [x] 2026-11-12: finding reserved

## Decisions

(filled in as the lane goes)

## Surprises

## Left for the lead

## Result
