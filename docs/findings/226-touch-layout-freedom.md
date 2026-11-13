# 226: Touch layout freedom and tablet defaults

Status: in progress

Branch `feat/touch-layout-freedom` from `main` 5a5b0cb8 (0.8.0 plus the iOS platform), started
2026-11-13. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

The owner's decision (2026-11-13): two ideas of the community iOS fork's touch controls (finding
224, "His touch controls and launcher next to ours") move into our shared overlay
(`port/hle/touch.c`), for Android and iOS both: per-control size and place as free as his, and
defaults for tablets and iPads. Plus: undo and redo and a size readout in the editor, an entry
point that starts the game straight into the editor (for the shared mobile launcher, lane 225),
and tilt steering on iOS (CoreMotion through the host call Android uses).

Done when: the unit tests in `port/hle/test_touch.c` cover each new or changed rule; a 0.8.0
`touch_edits` string loads and lays out exactly as before; `check.py port` and `check.py docs`
pass; a boot and a race state trace equal the reference; captures before and after at a phone, a
small tablet and a 13-inch tablet size; the phone shows the editor, a race with an edited layout
and the owner's layout unchanged; the iOS Simulator app builds on the Mac from the branch.

Out of scope: portrait layouts, languages (other lanes or not chosen), the launcher itself
(lane 225), a per-slot action swap (only if nearly free).

## Progress

- [x] 2026-11-13: finding reserved

## Decisions

## Surprises

## Result
