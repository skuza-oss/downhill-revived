# 189: Swap L1/L2 and R1/R2 per controller, and the game's codes on touch

Status: in progress

Branch `feat/shoulder-swap-touch-codes` from `main` ac62c78d (0.7.14), started 2026-11-09. This
file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

1. Issue #69: "Swap L1/L2" and "Swap R1/R2" beside Swap Sticks, off by default, saved per
   controller in dhd.ini as Swap Sticks is (`pad1`, `pad2`; finding 157), in the game's REVIVED
   SETTINGS > Controls page and the launcher's Controls tab. For generic pads that report a
   shoulder as its trigger and the trigger as the shoulder.
2. Issue #70: on Android the touch controls can't enter the game's codes (cheats, finding 117):
   the buttons the codes need are not on the touch layouts. Make code entry possible on touch in
   both presets (Classic pad, Thumb zones) without changing the race controls or the game's feel.

Acceptance: `python tools/check.py port` passes; `test_gamepad mapping`, the dhd_settings tests,
the launcher's tests and `test_touch` cover the new parts; the debug APK builds
(`port/android/build_apk.py`); boot and race state traces equal to the base build's (ac62c78d).
Out of scope: phone runs (the lead's), the browser's own pad layout (finding 157 left it fixed).

## Progress

- [x] 2026-11-09: finding reserved

## Decisions

## Surprises

## Result
