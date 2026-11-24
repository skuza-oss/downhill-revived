# 284: two launcher settings (Stick, Graphics API) and the touch overlay drawn as triangles

Status: current

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

- [x] 2026-11-24: finding reserved (ae848370).
- [x] 2026-11-24: #127, the launcher's Stick row (8d1a81f5).
- [x] 2026-11-24: #129, Graphics API: the setting, the launcher row, the fallback, the game's row.
- [x] 2026-11-24: #119, the overlay's corners from a vertex buffer.
- [x] 2026-11-24: checks (Result).
- [ ] the reporter's run of a build with the fix (unproven on the device).

## Issue #119: which construct the driver gets wrong

The player's picture (0.8.12, the layout editor over the title): every box, disc and ring of the
overlay and the editor's toolbar and panel is one triangle with the shape's top edge whole and its
apex at the middle of its bottom edge; inside that triangle the shape is drawn right (the glyphs,
the rims and the arcs are cut by it, so arcs look like wedges). The game's 3D picture and its own
2D menus are right.

How the paths draw (0.8.12):
- The touch overlay (`port/render/src/present/touch.rs`, every touch control, the editor's
  toolbar, panel and labels, the pause panel): one draw of 6 vertices per instance (`draw(0..6,
  0..count)`), a triangle list, the shapes an instance-rate vertex buffer, and each vertex's
  corner taken from a constant array indexed by the vertex index:
  `let corners = array<vec2f, 6>(...); corners[i]` with `@builtin(vertex_index) i`. naga makes that
  a function-local array with an initializer and an `OpAccessChain` by the index.
- The present pass (`present.wgsl`), the FPS counter (`counter.rs`), the native renderer's full-
  screen passes (`native.wgsl` `fullscreen_vertex`, `surfaces.rs`): one triangle from the vertex
  index's bits (`(i << 1) & 2`, `i & 2`), no array.
- The game's own draws (native renderer): vertex buffers.
The overlay is the only vertex stage that indexes an array of its own (now a test,
`shaders.rs` `no_vertex_local_array_indexing`).

Which vertices come out wrong: the corners are, in order, (-1,-1) (1,-1) (-1,1) | (-1,1) (1,-1)
(1,1), y down. The picture's triangle is (-1,-1), (1,-1), (0,1): vertices 0 and 1 right, vertex 2
with x = 0 instead of -1. If vertices 3 and 5 come out the same way, (0,1), the second triangle
has no area and is not drawn, which is the picture: every quad's lower corners land at (0,1). So
the driver reads the array right for the elements whose y is -1 and returns x = 0 for the ones
whose y is +1 (elements 2, 3, 5): a wrong load from a dynamically indexed constant array in the
vertex stage. Nothing else in the path depends on the vertex: the instance data (position, size,
kind, colours) is the same for the six vertices, and the shapes sit where they should, so the
instance-rate attributes are read right.

Outside: no public report of this exact fault on PowerVR's Rogue or B-Series Vulkan driver was
found (searches of wgpu/naga, Dawn, ANGLE, Godot, emulators' workaround lists). The nearest are a
Mesa PowerVR SGX (GLES) compiler bug where a constant array indexed by `gl_VertexID` read the
wrong elements (the index's bits packed as a float), and PPSSPP's BXM-8-256 workaround
(`PVR_UNUSED_DYNAMIC_UBO_OVERRUN`, a different fault of the same driver family). naga's own
issues on `vertex_index` are validation problems, not wrong output. So the cause is inferred from
the picture and the code, not confirmed by the vendor.

Not finding 278's path: a pipeline the driver refuses is not drawn at all (`touch overlay: not
drawn, the device refused it (...)`, which names the pipeline and the error). A refused pipeline
cannot draw a wrong shape, so this picture is a pipeline that was built and runs wrong. No log
change is needed for that case.

## Decisions

- #119: one path for every platform: the six corners are a vertex buffer of their own (vertex
  rate, location 4) beside the shapes' instance buffer. The vertex shader no longer reads the
  vertex index at all. The values are the same floats in the same order, so every other
  platform draws the same pixels. No PowerVR branch.
- #129: the setting is `graphics_api` (`dx12`, `vulkan`; another value stands for `dx12`) on the
  existing variable `DHD_WGPU_BACKEND`, which as for every setting wins over dhd.ini. The
  launcher row needs a new capability, `graphics-api`, which only the Windows launcher reports.
- #129 fallback: in Rust (`device::init`): the renderer latches a failed device for the process,
  so C cannot retry after it. A Vulkan that finds no adapter, cannot make its device, finds only
  a software adapter (when WARP was not asked for), or cannot present to the game's window is
  retried on Direct3D 12 with one log line. The window check comes before the device is installed:
  C passes the game's window with the device descriptor (`DhdrDeviceDesc.window`), and Rust makes
  a surface on it with RGBA8 or BGRA8 and configures it to the window's size, then drops it.
  `DHD_TEST_VULKAN_NO_SURFACE=1` (tests only) fails that check.
  `DHDR_FEAT_VULKAN` (ABI 5) tells C which one runs; C sets `DHD_GRAPHICS_API_RUNNING` for the
  settings page.
- #129 in the game: REVIVED SETTINGS shows Graphics API, a "needs a restart" row like Resolution,
  Font, Frame Rate and V-Sync. It sits on the Display tab after V-Sync, because the Graphics tab
  is full (ten rows with the Performance heading). It shows only where the host says which API
  runs (Windows). Its choice is the saved one (`DHD_WGPU_BACKEND`), kept apart from the API that
  runs: after a fallback it reads "Vulkan *" with the hint "Vulkan did not start here: dhd.log says
  why", and Undo puts the saved choice back (review: it used to show the running API, so a step and
  Undo wrote `dx12`).
- `dhdr_device_init` checks the descriptor's size and ABI version before it reports a device that
  exists: the browser makes the device (`dhdr_web_init`) before the core's first call, so a core
  and renderer of two ABI versions used to pass (review; older than this lane).
- The launcher's dimmed segmented rows (Stick with the D-pad) have disabled buttons that save
  nothing (review).
- #127: the launcher's Stick row is dimmed while the D-pad steers, as the game's page dims it. The
  default stays Floating (the settings crate's, unchanged).

## Surprises

- Before this lane a Vulkan that could not start on Windows ended the game: with
  `DHD_WGPU_BACKEND=vulkan` and no Vulkan driver loaded (`VK_DRIVER_FILES` / `VK_ICD_FILENAMES`
  pointing at a file that does not exist), the base build logged `dhd_render: no device: no
  adapter (Backends(VULKAN)): ... vulkan drivers/libraries could not be loaded ...` and `gs: the
  graphics didn't start`, and showed its error box. A dhd.ini choice must not do that, hence the
  fallback.
- The editor-alone mode (`DHD_TOUCH_EDITOR=1`) draws over a plain background, so its pictures are
  the same from run to run (three captures in a run, and two base runs, byte for byte equal). That
  makes it the before/after check for the overlay: it shows boxes, discs, rings, the PS2 symbols,
  the font, and with Thumb zones and Actions the sectors and action pictures.

## Result

What changed:
- #127: `port/launcher/ui/app.js` (the Touch tab's Stick row after Steer With, dimmed with the
  D-pad), the string tables (`row.touchStick*`, all eight languages), `docs/PLAYING.md`.
- #129: `port/render/settings/src/table.rs` (`graphics_api` on `DHD_WGPU_BACKEND`, default
  `dx12`, another value `dx12`) and the generated `port/hle/settings_table.h`;
  `port/render/src/device.rs` (`direct3d_instead`, the fallback in `init`, a Windows Vulkan with
  only a software adapter is no start), `abi.rs` / `dhd_render.h` / `test_dhdr_abi.c` /
  `tests/abi_layout.rs` (`DHDR_FEAT_VULKAN`, ABI 5); `port/gs/gpu_rust.c` (the flag follows the
  API that runs, `DHD_GRAPHICS_API_RUNNING` on Windows, a log line for an unknown variable
  value); `port/hle/pc_settings.c` (Graphics API on the Display tab, Windows only); the launcher's
  Graphics tab row after Resolution (`graphics-api` capability, Windows' launcher only,
  `port/launcher/src-tauri/src/main.rs`, `BRIDGE.md`), strings, `docs/PLAYING.md`.
- #119: `port/render/src/present/touch.rs` (the quad's corners a vertex buffer, `CORNERS`; the
  vertex shader no longer reads `vertex_index`), `port/render/src/shaders.rs` (the test
  `no_vertex_local_array_indexing`, which finds the old shader).

Proof on this PC (RTX 3070; hidden desktop, untimed):
- Overlay before (base aded9f56) and after, the layout editor alone at 1600x720: Classic pad on
  Direct3D 12, Thumb zones with Button Style Actions on Direct3D 12, Classic pad on Vulkan: every
  capture byte for byte equal to the base's (`PIL ImageChops.difference(...).getbbox()` is None).
- dhd.ini `graphics_api = vulkan`: `settings: ... WGPU_BACKEND=vulkan`, `gs: GPU NVIDIA GeForce
  RTX 3070 on Vulkan`. `graphics_api = metal`: `WGPU_BACKEND=dx12`, on Dx12. `vulkan` with no
  Vulkan driver loadable: `gs: Vulkan could not start (no adapter (Backends(VULKAN)): ...); the game
  runs on Direct3D 12 instead (graphics_api in dhd.ini, or DHD_WGPU_BACKEND)`, then on Dx12, and
  the editor opened. `DHD_WGPU_BACKEND=metal`: `gs: DHD_WGPU_BACKEND=metal is neither dx12 nor
  vulkan: Direct3D 12`.
- REVIVED SETTINGS > Display shows `Graphics API: < Direct3D 12 >` after V-Sync (nine rows, the
  tab fits).
- Launcher: `node port/launcher/test_controls.js`, `test_handoff.js` (its new `capsRows`: Graphics
  API after Resolution with `graphics-api` only; Stick after Steer With on a touch device, dimmed
  with the D-pad), `test_launcher_ui.js` (Chromium 80 denylist included): ok. `tools/lang_check.py`:
  0 errors.
- Settings crate tests (`DHD_SETTINGS_BLESS=1` regenerated the C table), renderer unit tests
  (`shaders::tests`, `device::tests`).
- `DHD_BUILD_SLOTS=2 python tools/check.py port -j 4`: OK (every step); `check.py docs`: OK.
- State traces against the base build: `boot` and `race`, `state_diff.py` PASS for both.

Review round (same checks again, all OK, traces PASS against the base):
- Undo: dhd.ini `graphics_api = vulkan`, no Vulkan driver loadable (Direct3D 12 runs), REVIVED
  SETTINGS > Display: the row reads `Vulkan *` with "Vulkan did not start here: dhd.log says why";
  one step writes `dx12`, Undo writes `vulkan` back (the build before the fix wrote `dx12`).
- The window check: `graphics_api = vulkan` with a working driver passes it and runs on Vulkan;
  with `DHD_TEST_VULKAN_NO_SURFACE=1` the log says `gs: Vulkan could not start (NVIDIA GeForce
  RTX 3070 cannot present to the game's window: ...); the game runs on Direct3D 12 instead ...`,
  the game runs on Dx12 and the editor's picture equals the plain Direct3D 12 run's byte for byte.
  A bad value and a Vulkan with no driver still take Direct3D 12 as before.
- `device_smoke` `exact_device_global_init`: after the device exists, a descriptor of another ABI
  or size gets 0; the same test fails against the old order (`another ABI`, 1 instead of 0).
  `abi_layout` and the device unit tests pass; the wasm32 renderer compiles
  (`rustgs.py check --target wasm32-unknown-unknown`).
- The launcher's `capsRows` test clicks the dimmed Stick row's buttons: disabled, nothing saved
  (it fails against the old `app.js`).

Unproven: the overlay fix on the phone. The cause is inferred from the picture and the code (no
public report of the driver fault). The next picture from that phone with this build should show
whole rectangles, discs and rings; if it still shows triangles, the fault is not the array (the
next suspects: the instance-rate attributes, or the triangle list's second triangle being
dropped), and the log's `gs: GPU ... driver ...` line and `touch overlay:` lines go with it.
