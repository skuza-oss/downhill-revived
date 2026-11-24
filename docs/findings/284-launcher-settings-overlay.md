# 284: two launcher settings (Stick, Graphics API) and the touch overlay drawn as triangles

Status: in progress

Branch `feat/launcher-settings-overlay` from `main` aded9f56 (0.8.12 plus finding 281), started
2026-11-24. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Three issues:
- #127: the launcher's Touch page offers the steering stick's Fixed / Floating (`touch_stick`,
  already in dhd.ini and the game's TOUCH CONTROLS pages). The default stays Floating.
- #129: on Windows, the graphics API (Direct3D 12, the default, or Vulkan) is a dhd.ini setting
  with a row in the launcher's Graphics tab beside Resolution; `DHD_WGPU_BACKEND` still wins. Not
  shown and ignored elsewhere. A Vulkan that cannot start falls back to Direct3D 12 with a log
  line instead of the game failing to start.
- #119: on a PowerVR B-Series BXM-8-256 (Redmi Note 14 5G, driver 25.1@6715691, 0.8.12) every
  shape of the touch overlay (the controls, Edit Layout's toolbar and panel) is drawn as one
  triangle: the top edge whole, the apex at the bottom's middle; the game's own 3D and 2D look
  right. No such phone here.

Acceptance: the launcher tests (`node port/launcher/test_controls.js`, `test_handoff.js`,
`test_launcher_ui.js`), the settings crate's tests; hidden runs where dhd.ini selects Vulkan (the
log says so), a bad value, and a Vulkan that cannot start (both Direct3D 12, said in the log); the
overlay and the editor captured before and after the change, pixel-identical on this PC;
`DHD_BUILD_SLOTS=2 python tools/check.py port -j 4`, `check.py docs`; boot and race state traces
equal to the base build.

## Progress

- [x] 2026-11-24: finding reserved.

## Decisions

## Surprises

## Result
