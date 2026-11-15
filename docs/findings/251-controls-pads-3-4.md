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

- [x] 2026-11-15: finding reserved (a1025c96)
- [ ] 2026-11-15: four controllers in dhd_settings, the C interface, the host, the in-game page,
  the launcher, the test pads and the docs

## Decisions

- **The device table is the only list.** `controls.rs`' `DEVICES` has five rows (keyboard, `pad1`
  to `pad4`); `Controls`, `from_values`, `values`, the generated `controls_table.h`
  (`CONTROL_DEVICES = 5`, `CONTROL_PAD1` ... `CONTROL_PAD4`, the defaults and the keys) and the
  launcher's schema all follow its length. Player N's controller is `CONTROL_PAD1 + N - 1`.
- **The C interface takes an array.** `dhd_controls_layouts` and `dhd_controls_edit` took the
  keyboard's, pad1's and pad2's values as three arguments; they now take
  `const char *const values[DHD_CONTROLS_DEVICES]` (dhd_settings.h, 5; host.c checks it equals
  `CONTROL_DEVICES` at compile time), so a sixth layout would change no signature.
- **The host**: `pad_layout(port)` gives ports 0-3 their own layout (it gave ports 2 and 3
  Controller 1's defaults); the page's controller checks (`controls_pad_connected`, the capture's
  player) take players 1 to 4. The dhd.log line at a layout change names every device by its
  dhd.ini key: `controls: keyboard (defaults), pad1 ..., pad2 ..., pad3 ..., pad4 ...`.
- **The in-game page** needed no new code: its Device row already cycled `CONTROL_DEVICES`, its
  Reset row names the device. Square (clear) now reads all four players' held buttons, so the
  player holding Controller 3 can clear on its page.
- **The launcher's page** draws `schema.devices`, so it shows five choices; the tab's Reset now
  resets each device the schema lists (it had a literal 3). At the launcher's 960x600 window the
  five choices fit on one line beside the Bindings label. The phones' launcher has no Bindings
  block (`bindings` is a desktop cap, BRIDGE.md), unchanged.
- **Test pads 3 and 4**: `DHD_TEST_PAD3` and `DHD_TEST_PAD4` (autopad.c) drive players 3 and 4's
  controllers through `pad3` and `pad4` as `DHD_TEST_PAD`/`DHD_TEST_PAD2` do for 1 and 2. The state
  trace header lists `DHD_PAD3`, `DHD_PAD4`, `DHD_TEST_PAD3`, `DHD_TEST_PAD4` with the others.
- **An old dhd.ini**: a missing `pad3` / `pad4` line is the setting's default, "", which is the
  default layout; nothing is written until a player changes that controller.

## Surprises

- `python port/android/test_disc_image.py` failed to compile its JNI test on this PC: JAVA_HOME's
  JBR has no `include` folder. With `DHD_JNI_HEADERS` pointing at a folder holding `jni.h` (the
  variable the test already reads) it passes. Not this lane's change.
- `tools/worktree_setup.py` for a second worktree on F: could not make its junctions there ("The
  file or directory is corrupted and unreadable") and copied `tools/compilers` and `tools/ext`
  instead.

## Result
