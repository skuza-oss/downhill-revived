# 251: Bindings for controllers 3 and 4

Status: current
Checked: 2026-11-15
Supersedes: 157 in part (players 3 and 4 kept the default controller layout)

Branch `feat/controls-pads-3-4-251` from `main` cae6f05c, started 2026-11-15. GitHub #99.

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
- [x] 2026-11-15: four controllers in dhd_settings, the C interface, the host, the in-game page,
  the launcher, the test pads and the docs (5bd5345a)
- [x] 2026-11-15: the checks below; finding 157 marked partly superseded, STATE and STATE-LOG

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

What changed: see Decisions. New dhd.ini keys `pad3` and `pad4` (variables `DHD_PAD3`,
`DHD_PAD4`), in the same form as `pad1` and `pad2` (docs/PLAYING.md, Controls); new test variables
`DHD_TEST_PAD3`, `DHD_TEST_PAD4` (docs/TESTING.md). Checks, 2026-11-15, Windows:

- Unit tests: `dhd_settings`' bindings tests, with two new ones: `four_controllers` (the five
  devices, a change on Controller 3 or 4 leaves the others as they were, the same rules and
  messages, the five lines read back to the same layouts, a broken `pad4` line resets Controller 4
  alone, Reset per controller) and `an_old_dhd_ini` (a file with `keyboard`, `pad1`, `pad2` and no
  `pad3`, `pad4`: those two are the defaults, the others as written; a change on Controller 3 adds
  its line and leaves the others' lines as they were). The launcher's `cargo test` (30 passed; the
  schema has five devices, edits on Controllers 3 and 4 write `pad3` and `pad4`, Reset of 3 leaves
  4), `node port/launcher/test_controls.js` with and without the launcher's schema (five devices
  with the same defaults; the tab's Reset resets each of the five).
- `python tools/check.py quick`, `port` and `docs`: OK. Android: `port/android/test_build_apk.py`
  and `test_disc_image.py` pass (the latter with `DHD_JNI_HEADERS`, Surprises).
- State traces with the defaults against the base build (cae6f05c built in its own worktree):
  boot 600/600 and race 5000/5000 rows equal.
- Players 3 and 4's layouts reach the game (`state_trace_run.py split` with `DHD_PADS=4` and the
  four-player route, `0:triangle|cross,15:down,15:cross,10:down,10:down,10:cross,16:cross,24:cross,
  11:cross,34+240:cross,9/5:cross`, 6000 fields): a race where players 3 and 4 get no race pad and
  press `east` and `north` through `DHD_TEST_PAD3`/`DHD_TEST_PAD4` with `DHD_PAD3=cross:east
  circle:south` and `DHD_PAD4=cross:north triangle:south` equals the race where all four pedal with
  Cross by `DHD_RACE_PAD2`/`DHD_RACE_PAD3`: 6000/6000 rows. The same presses without the layouts
  differ from field 1236 (east is Circle, north Triangle), and so does the run with the two
  layouts swapped between pads 3 and 4 (each pad reads its own). dhd.log: `test pad 3 ...
  inputs=00000004 buttons=4000` (Cross) with the layout, `buttons=2000` (Circle) without, and
  `controls: keyboard (defaults), pad1 (defaults), pad2 (defaults), pad3 cross:east circle:south,
  pad4 cross:north triangle:south`.
- The launcher's Controls tab with Controller 3 chosen, drawn headless (Edge, a stand-in Tauri
  answering from the launcher's own schema dumps, over the real ui files) at its 960x600 window:
  the five device choices on one line, Controller 3's rows, its stick and shoulder options and
  "Reset Controller 3". Pictures kept outside the repository.

Left open:
- No real controller was pressed: four pads on one PC (SDL slots 3 and 4, a multitap's players in
  a split-screen race, Controller 3's page waiting for that pad's press) are unproven on hardware.
  The page's wait for Controller 3 or 4 takes that player's pad when it is connected, else any
  pad, as for 1 and 2.
- The in-game page with Controller 3 or 4 chosen was not driven in a hidden-desktop run in this
  lane; its Device row cycles `CONTROL_DEVICES`, unchanged code, and every change it makes goes
  through `dhd_controls_edit`, whose rules the unit tests cover for Controllers 3 and 4.
