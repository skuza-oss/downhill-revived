# 251: Bindings for controllers 3 and 4

Status: in progress

Branch `feat/controls-pads-3-4-251` from `main` cae6f05c, started 2026-11-15. GitHub #99. This
file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

The game plays up to four players in split-screen, but the bindings (finding 157) had layouts for
the keyboard, Controller 1 and Controller 2 only; players 3 and 4 always used the default layout.
Controllers 3 and 4 get their own layouts, set exactly like 1 and 2 wherever those are set: the
launcher's Controls tab, the game's REVIVED SETTINGS > Controls, dhd.ini (`pad3`, `pad4`), with the
same defaults, Reset and rules.

- One path: the device table in `port/render/settings/src/controls.rs` grows to five rows; nothing
  is copied per controller.
- An old dhd.ini (no `pad3`, `pad4`) gives controllers 3 and 4 the defaults; 1 and 2 are unchanged.
- Tests: the settings round trip for four controllers and an old file, controllers 3 and 4's
  layouts reaching the game's pads 3 and 4 (`DHD_TEST_PAD3`, `DHD_TEST_PAD4`, docs/TESTING.md), the
  launcher's tests.
- Checks: `check.py quick`, `port`, `docs`; the launcher's and the Android unit tests; boot and race
  state traces equal to the base build's.

Out of scope: issue #94's gamepad picture, new actions, any change to how 1 and 2 behave.

## Progress

- [ ] 2026-11-15: finding reserved

## Decisions

## Surprises

## Result
