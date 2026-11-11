# 199: Native VU in the browser

Status: in progress

Branch `feat/native-vu-browser` from `main` 6e2f7d23 (0.7.16 plus findings 200 and 201), started
2026-11-11. This file is the lane's plan while it runs and its finding when it merges. Someone who
has only this file and the git log must be able to pick the lane up.

## Goal and acceptance

Native VU (findings 164, 167, 168, 194, 198: the VU1 programs' batch loops drawn by the native
renderer's shaders, `port/hle/vu1_mesh.inc`) works in the browser build (WebGPU) through the same
code as every other platform: the browser's VU worker runs `vu1_mesh.inc`'s translation, the
browser's renderer makes the mesh layouts and pipelines, and the same setting (`native_vu`, on by
default, live, with the automatic fallback to the emulated VU when the GPU refuses the mesh
pipelines) is offered in the browser's settings. This is the step before the emulated VU draw path
can be deleted everywhere: the browser must no longer need a path of its own.

Acceptance:

- The browser build compiles and runs with Native VU on: boot, menus, the Bike Shop, a race start
  and mid-race, pictures within finding 164's tolerance (per field MAE <= 0.1, at most 0.1 % of
  pixels off by more than 8) against the browser's own pictures with it off, through the repo's
  headless browser tools (`port/web/check_browser.mjs`, `run_headless.mjs`).
- Off still works; a refused mesh pipeline falls back as on the native platforms
  (`DHD_TEST_NO_MESHES`).
- Native platforms unchanged in behaviour (the shared code changes are behaviour-neutral there; the
  lead runs the Windows traces and pictures).
- WebGPU limits checked against what the mesh path needs; mismatches solved in the shared path.
- `python tools/check.py port` and `check.py docs`, `rustgs.py test`, `rustgs.py check --target
  wasm32-unknown-unknown`, the browser build and its headless checks with Native VU on and off.

Out of scope: deleting the emulated path; timing (the owner was using the PC: no benchmarks).

## Progress

- [x] 2026-11-11: finding reserved (`b08f8ca3`)
- [x] the renderer: mesh layouts in the browser, the awaited mesh pipeline probe, the browser's
  `DHD_TEST_NO_MESHES` (`79e7e4a6`)
- [x] the VU worker's translation, the two shared words, mesh items through the queue, the
  setting offered and live (`7d5644c8`)
- [x] the picture harness: `check_browser.mjs --capture`, `native_vu_pictures.py compare` reads
  its PNGs (`c4bd815d`); `check_browser.mjs --keys` for the settings page (`414d9a84`)
- [x] Node state traces (boot, race), browser pictures (base, off, on in both modes, title,
  fallback), the live row, the one-worker build, `check.py port`, `rustgs.py test`, the wasm32 check
- [x] docs: AGENTS.md, STATE.md, PLAYING.md, this finding

## What changed

### The VU worker translates (`port/hle/vu1_mesh.inc`, `port/gs/gs_mesh.h`, `gs.h`, `gs.c`)

`vu1_mesh.inc`'s browser stub is gone: the browser's VU (`vu.wasm` in the threaded build,
`graphics.wasm` in the one-worker build) compiles the same translation (its SSE intrinsics through
SIMDe, as the rest of `vif.c`). It needs two words the native threads share: `gs_mesh_setting` (the
setting, written where the environment is read) and `gs_mesh_mode` (whether the renderer draws
meshes). In the threaded browser build the GS and the VU are two wasm modules on one shared memory,
neither can name the other's variables, so under `DHD_WEB_THREADS` the two words are macros for two
fields of the queue's first control block (`port/web/web_queue.h`, `WebQueue.mesh_setting`,
`mesh_mode`), a fixed place both modules know, as the queue itself. Every other build (native, the
one-worker browser) keeps them as `gs.c`'s variables. The atomics are the same code; on the shared
memory they are wasm's atomic instructions.

### The GS worker reads the setting and draws the items (`port/web/gpu_rust_web.c`, `host_graphics.c`, `web_queue.c`)

- The setting is read (`gs_mesh_setting_read`) at the first `gs_reset`, whatever the renderer (as
  `host.c` does natively before the game runs; a headless run's forced `:skip` needs it too), and
  again when the page changes `DHD_NATIVE_VU1` (`web_env_changed`, the variable is in
  `settings.js`'s `RENDERER_LIVE`; the VU sees it at its next batch: live, as natively).
- A mesh item reaches the GS worker as any VU1 output, a `WQ_GIF` packet of the second ring; the
  GS side's `graphics_gif_feed` sends path `GS_PATH_MESH` to `gs_mesh` (as `gs_queue.inc`'s
  `gs_execute` natively), inside the same FP-mode switch as `gs_feed`.
- A shadow batch's kicks (`:shadow`, and the default when `DHD_EXACT_PROBE` answers the Z probe)
  carry `graphics_provenance.shadow` through the ring (`WQ_SHADOW`), so `gs_vu_shadow` is set for
  them as natively.
- The reactor's `hle_log` now writes every line, as `kernel.c`'s does natively (its other callers
  are VIF1's rare or diagnostic lines), so the browser's LOG has Native VU's lines.

### The renderer (`port/render/src/native/init.rs`, `pipelines.rs`, `render.rs`, `web/init.rs`)

- `mesh_support` no longer refuses wasm: the browser takes the same checks as every platform
  (below, WebGPU limits).
- **The refusal, asynchronously.** Natively a mesh pipeline the driver refuses is seen at once
  (`scoped_pipeline` pops its error scopes synchronously) and turns Native VU off there. The
  browser reports a GPU error only to a promise, which C's synchronous calls can't await (an error
  nobody captures is fatal there, `guard::install_error_handler`). So the browser's async init
  (`dhdr_web_init`) now calls `native::init::web_prepare`: it builds the native renderer's objects
  and probe pipelines as `Native::create` does, then the mesh draws' pipelines (`mesh_probe_keys`:
  every probe key's variant as a mesh draw, with a texture and with texels: the mesh vertex stage,
  its storage binding and every mesh fragment entry point the browser uses) inside Validation and
  Internal error scopes it awaits. A refusal there is `refuse_meshes` with the same dhd.log line,
  before any mesh is drawn, and the game emulates as on a native GPU that refused. `Native::create`
  takes these objects and pipelines (nothing is built twice). Natively `web_prepare` does not exist
  and the create is unchanged.
- **The test hook.** `DHD_TEST_NO_MESHES` (finding 194) is read by Rust from the environment, which
  the browser's Rust does not have: `graphics_common.js` passes the page's value to
  `dhdr_web_test_no_meshes` before the init. `test_refusal` (the N-th picture) now runs in the
  browser too.

### The setting (`port/web/www/settings.js`)

`DHD_NATIVE_VU1` leaves `BROWSER_WITHOUT` (the crate's default, on, now reaches the game), joins the
browser's `DHD_PC_SETTINGS_ROWS` (REVIVED SETTINGS > Graphics shows "Native VU:" above the
Performance group, as natively) and `RENDERER_LIVE`. The browser's stored dhd.ini gets
`native_vu = on|off` as the native one does.

### The harness (`port/web/check_browser.mjs`, `www/worker.js`, `www/main.js`, `port/tools/native_vu_pictures.py`)

- `--capture FIELDS --capture-dir OUT`: the browser has no screenshot files (`present.c`'s
  `DHD_CAPTURE_FIELDS` is native), so the game worker takes that variable as the fields at which it
  waits once the field's picture is shown (in its vsync, as for a slow present: the game's state is
  not touched); the page gets `capture`, the harness saves the canvas (DevTools screenshot clipped
  to it) as `OUT/raw/shot-fNNNNNN.png` and resumes. `native_vu_pictures.py compare` reads those as
  it reads the native BMPs.
- `--keys F:CODE,...`: key presses through the page's keyboard path at fields; REVIVED SETTINGS
  reads the controller itself (its tabs), which `DHD_NAV`'s pad does not reach.

## WebGPU against what the mesh path needs

| need | WebGPU | how it is handled |
|---|---|---|
| a read-only storage buffer in the vertex stage (`mesh_records`, group 2) | core allows `read-only-storage` in the vertex stage; `maxStorageBuffersInVertexStage` equals `maxStorageBuffersPerShaderStage` (at least 8) in core; only compatibility mode (opt-in, `featureLevel: "compatibility"`) defaults it to 0 | the renderer asks for a core adapter; `mesh_support`'s checks (wgpu's WebGPU backend reports every downlevel flag, `maxStorageBuffersPerShaderStage` is read) and the awaited probe catch anything else |
| three bind groups (draws, texture, mesh) | `maxBindGroups` at least 4 | as natively |
| the picture's vertex buffer bound as vertex buffer and as read-only storage in one pass | both are read-only usages, allowed together in a usage scope | as natively (the buffer gets `STORAGE` where meshes are possible, `arenas.rs`) |
| the vertex buffer within one storage binding | the device asks for the adapter's `maxStorageBufferBindingSize` (`web/init.rs`) | `arenas.rs`'s existing check refuses meshes when a picture's vertices exceed it |
| the draw constants | a uniform at a dynamic offset (finding 195 undid finding 172's instance-indexed array) | the same constants every browser draw already uses; no instance index, no `first_instance` |
| dual-source blending | not requested in the browser | mesh draws take finding 188's two-draw form like any draw (every browser picture here) |
| WGSL for Tint (shifts in parentheses) | | `parenthesize_shifts` already covers the whole `native.wgsl`, mesh entry points included |
| a refused pipeline seen at once | errors only through promises | the awaited probe in `dhdr_web_init` (above) |

Nothing needed a browser-only variant of the mesh path: the vertex shader, the layouts, the draw
recording and the fallback are the shared ones; what is the browser's own is where the refusal is
detected (an async init can await, C cannot) and how the two cross-thread words are placed (two
modules instead of one process).

Sources: the WebGPU specification (`GPUBindGroupLayoutEntry` buffer binding types and visibility,
usage scopes), the gpuweb compatibility-mode proposal
(https://github.com/gpuweb/gpuweb/blob/main/proposals/compatibility-mode.md) and
https://webgpufundamentals.org/webgpu/lessons/webgpu-compatibility-mode.html (the vertex-stage
storage limits in core and compatibility mode), wgpu 29.0.3's `src/backend/webgpu.rs`
(`downlevel_capabilities`: every flag). No code was adapted from them.

## Checks

### State traces (Node, `run_headless.mjs`, the threaded browser modules)

| run | boot (600 fields) | race (5000 fields, `state_trace_run.py`'s race route) |
|---|---|---|
| this branch, off, against the base build (6e2f7d23's modules) | all 13 columns identical | |
| `DHD_NATIVE_VU1=all:shadow` against off | all 13 identical | all 13 identical |
| `DHD_NATIVE_VU1=all:skip` against off | every game column identical; vu0, gs_stream, gs_regs differ from field 29 | the same |

Finding 194's native result exactly (skip: the translated batches don't run on the VU and their
packets never reach the GS, by design). Node has no GPU, so the forced modes run without a renderer
("no mesh renderer: the items are dropped"). Boot VU1 steps: 15,674,262 off, 2,484,900 skip. The
per-loop counts (race, skip) are those of the native lane: 0170 916,109 batches translated, 195,996
on the VU (near bound), 08B0 / 0798 / 3140 none left on the VU.

### Pictures (headless Chrome, WebGPU, scale 2)

`check_browser.mjs --capture` over finding 156's route as `native_vu_pictures.py` runs it
(bench_race's race scene: its NAV, autopad, race pad, seed 81257), the virtual clock
(`DHD_FPS_WALL` empty), `DHD_SETTINGS=none` with `DHD_NATIVE_VU1` given, the eleven fields of
`native_vu_pictures.py`. Each picture is the presented canvas (1207 x 905 CSS pixels of the 1280 x
960 canvas, Smooth scaling), so a little softer than the native tool's display read. The browser's
route is faster through the menus than the native one: 700 is the rider select, 950 the Bike Shop,
3200 and 3500 the race start, 6400-9600 mid-race (the tool's scene labels are the native route's).

- **Base build (6e2f7d23) against this branch off**: identical, every pixel of the eleven fields.
- **Off against on**, finding 164's method (`DHD_EXACT_PROBE=1`: Native VU in shadow mode):

| field | scene | MAE | > 8 | > 32 | max |
|---|---|---:|---:|---:|---:|
| 700 | rider select | 0.001 | 0.001 % | 0.000 % | 12 |
| 950 | Bike Shop | 0.002 | 0.005 % | 0.001 % | 51 |
| 3200 | race start | 0.012 | 0.016 % | 0.001 % | 70 |
| 3500 | race start | 0.032 | 0.080 % | 0.002 % | 52 |
| 6400 | mid-race | 0.020 | 0.006 % | 0.000 % | 46 |
| 6600 | mid-race | 0.017 | 0.009 % | 0.003 % | 131 |
| 6800 | mid-race | 0.010 | 0.004 % | 0.000 % | 35 |
| 7100 | mid-race | 0.038 | 0.028 % | 0.001 % | 132 |
| 9000 | mid-race | 0.016 | 0.012 % | 0.002 % | 52 |
| 9300 | mid-race | 0.031 | 0.020 % | 0.001 % | 70 |
| 9600 | mid-race | 0.005 | 0.005 % | 0.001 % | 48 |

  Every field within the tolerance (native Windows, finding 194: worst 0.066 / 0.087 %).
- **Skip mode, what players get** (no exact probe: the VU skips the translated batches): the
  pictures are identical, pixel for pixel, to the shadow-mode ones, and off without the probe is
  identical to off with it: on this route the game's state does not move either way.
- **The title** (fields 60, 120, 170, skip mode): MAE at most 0.001, largest difference 33.
- **The fallback**, `DHD_TEST_NO_MESHES` through the page: `=0` refuses in `dhdr_web_init`'s
  objects (dhd.log: "gs native: Native VU can't run on this GPU (DHD_TEST_NO_MESHES); the VU runs
  every program", then "native VU1: Native VU needs the native renderer with indexed fields (or the
  GPU refused its draws); the VU runs every program") and every field is identical to off; `=900`
  has field 700 drawn natively (MAE 0.001 against off, as with it on) and every field after the
  refusal identical to off; the game ran to the route's end both times. As finding 194 natively.
- **The one-worker build** (`build.py --no-threads`: one module, the words as `gs.c`'s variables),
  fields 700-3500 in skip mode: off identical to the threaded build's off; on against off the same
  numbers as the threaded build's (worst 3500: 0.032 / 0.080 %); the loops line in its log.

### The live row (headless Chrome, players' settings)

`check_browser.mjs --keys` with `?env.DHD_SETTINGS=dhd.ini` on a fresh profile (the players'
defaults): the log starts with "native VU1: loops 0170 ... 2DE8 drawn by the native renderer, the
VU skips them" (on by default); X, Down, Down, X, X opens REVIVED SETTINGS, D the Graphics tab
(Resolution, Scaling, Font, **Native VU: ON**, then the Performance group), three Downs and Right:
"pc settings: Native VU: off", "native VU1: off; the VU runs every program"; Right again: "pc
settings: Native VU: on" and the loops line. The browser's stored dhd.ini then holds
`native_vu = on`.

### Builds and tests

- `python tools/check.py port`: every step OK, on the lane's code (`414d9a84`'s tree).
- The game core is byte-identical to the base's (sha256 `bc4d962e...`): `build_wasm.py` rebuilt it
  (gs.h is an input) to the same bytes.
- `rustgs.py test`: every test passes; `rustgs.py check --target wasm32-unknown-unknown`: OK.
- `port/web/build.py` (threaded, the default) and `--no-threads`: both build;
  `check_web_layout.py build/web --require-renderer` OK on both (with `vu` on the threaded one);
  `vu.wasm` imports nothing new (the build's own import check), about 18 KB larger.
- `check_settings.mjs` (no disc: the crate's web corpus through settings.js, now with
  `DHD_NATIVE_VU1` out of `BROWSER_WITHOUT`): OK, 8 cases, the native game's results.
- `check.py docs`: OK.
- Untimed, PC in use: the browser's status line read about 60 fields/s at the title with it on and
  off (paced to the display: says nothing about speed). The draws counter at field 1300 was 3.8 M
  with it on against 61.7 M off (mesh draws merge, finding 168).

## Native platforms: behaviour-neutral, and why

- `gs.h` now includes `gs_mesh.h` (declarations); `gs_mesh_mode` and `gs_mesh_setting` are
  macros only under `DHD_WEB_THREADS`, which only the threaded browser modules define; `gs.c` keeps
  both definitions with the same initial values everywhere else.
- `vu1_mesh.inc`: only the browser's `#if` branch was removed; the native code is unchanged.
- Rust: `mesh_support`'s removed early return was `cfg!(target_arch = "wasm32")` (false natively);
  `DHD_TEST_NO_MESHES` is read from the environment once natively, as `test_refusal` already did
  (`mesh_support` read it uncached before: the same value, the variable is never set at run time);
  `Native::create` without `web_prepare`'s objects builds as before; `web_prepare`,
  `Pipelines::extend` and `dhdr_web_test_no_meshes` exist only in the browser; a log text says "on
  this device" instead of "(browser)".
- The core module is byte-identical (above); `check.py port` passed.

## Decisions

- **On by default in the browser**, as everywhere: every scene within the tolerance, off identical
  to the base, the fallback as natively, the row live. No timing exists yet (the lead's timed run).
- **The two words at a fixed place, not a copy kept in step**: the VU reads them before every
  batch, natively from shared variables; a fixed place in the shared memory is the same thing for
  two modules, and the queue already lives there. The alternative (each module its own copy,
  synchronized by queue packets) would add messages and a second notion of "now".
- **The GS worker reads the setting**, not the VU worker: `gs_mesh_parse` lives in `gs.c` (the GS
  module), and natively the reading side is the game/renderer side too; the VU sees the new word
  at its next batch (a field later than natively at most, the queue's latency).
- **The probe at the async init, not per pipeline**: a pipeline that fails later can't be undone in
  the browser (its draw would invalidate the picture's command buffer, fatal there); the probe
  covers what is mesh-specific (the vertex stage, its storage binding, each mesh fragment entry
  point). Other keys of a mesh draw differ only in blend, depth and mask state, as every other
  browser pipeline does, which the browser also builds unscoped.
- **The harness extends `check_browser.mjs`** (the brief: no parallel harness); `DHD_CAPTURE_FIELDS`
  is reused with its native meaning (these fields' pictures), so `native_vu_pictures.py` compares
  browser and native runs alike.

## Surprises

- **`?iso=test` pages read no settings** (`DHD_SETTINGS=none`, the tools' built-in: off), so a test
  of the players' default needs `?env.DHD_SETTINGS=dhd.ini`; the first smoke run was the off path.
- **The setting was not read without a renderer**: the first browser change read it only where the
  native renderer is made; Node's headless runs (no GPU) then never translated even when forced.
  Read at the first `gs_reset` now, as `host.c` natively.
- **`check_settings.mjs --iso`'s route is stale** (not fixed here): REVIVED SETTINGS is OPTIONS'
  first item now, so its `2:up,2:cross` opens LOAD REPLAY; and its Right on the first row would
  change Widescreen (the Display tab comes first), not Resolution. It needs `--keys`-style input for
  the tabs. The corpus part (no disc) is unaffected.
- **`DHD_NAV`'s pad does not reach REVIVED SETTINGS' tabs** (the page reads the host's controller):
  hence `--keys`.

## Result

Lane state 2026-11-11 (not merged; the lead merges and closes the finding).

- **What changed**: the browser runs Native VU through the shared code: its VU worker translates,
  its renderer makes the mesh layouts and draws the items, a refused mesh pipeline turns it off at
  the init (awaited probe) with the native dhd.log lines, the setting is offered (REVIVED SETTINGS >
  Graphics), live, on by default. `check_browser.mjs` gained `--capture` and `--keys`.
- **The emulated draw path is no longer anyone's own path**: it is only the fallback and the
  setting's off, on every platform. The deletion lane can now treat the browser like the others.

At the merge (the lead): Status `current`; finding 198 `partly superseded by 199 (the browser left
out)`, and its and 194's lines saying the browser hides the row or runs the emulated path stop
stating that; a STATE-LOG entry.

Checks left for the lead, once, on the merged branch:

- Windows: boot and race state traces against 6e2f7d23 (`state_trace_run.py`), and
  `native_vu_pictures.py` off and on against 6e2f7d23 (expected identical; the shared changes are
  behaviour-neutral, above).
- A timed browser comparison, Native VU off against on (headless Chrome, `check_browser.mjs`'s
  status line or `--profile`, with `?env.DHD_FPS=144` or the display's rate so the pacing does not
  cap it), interleaved, on an idle PC.
- A phone run (Android: the native path, unchanged here; one boot to a race with the APK of the
  merged branch).
- The browser on another GPU (an integrated one; and Chrome on Mac if available): the probe line or
  the loops line in LOG.
