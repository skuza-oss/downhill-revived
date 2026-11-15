# 255: menus, HUD and world at the same speed at 60 and 240 (issue #76)

Status: in progress

Branch `fix/high-fps-sweep-255` from `main` 34d6273d, started 2026-11-15. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

At 120/144/240 fps some things still move at a different speed than at 60 (issue #76). The
known open case: the title screen's pulsing PRESS X TO START box runs faster at high rates. The
birds of Mt. Konawaki TD and the hovering Red Pass MX helicopter (finding 193's leftovers) are
fixed already: the sweep only confirms them. The camera wobble of finding 193 is checked once.

1. The title pulse at the same speed at every rate, on the delta-time clock (finding 139), in
   `port/patches_fps.py`; `src/` keeps matching.
2. A repeatable sweep: the same scene at 60 and at 240 on the deterministic clock for the same
   fields, every EE word's travel per field compared (oscillators included), scenes: the title,
   the menus, rider and bike choice, the track chooser, every track's first minute, a result
   screen and a replay. Every difference above about 3 % listed with its size.
3. What the sweep finds fixed, largest first, one commit per cause; what is left listed with
   where its step lives.

Done when: `check.py quick`, `check.py port`, `check.py docs` pass; boot and race state traces
equal the base build's (`state_trace_run.py boot|race`, `state_diff.py`); the sweep in
`port/tools/` with an entry in `docs/TESTING.md`; the table below filled. Out of scope: frame
times and fps (this is a check of speeds, not a benchmark); anything right at 60.

## Progress

- [ ] 2026-11-15: lane started, finding reserved.

## Decisions

## Surprises

## Result
