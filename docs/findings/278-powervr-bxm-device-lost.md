# 278: a PowerVR B-Series phone loses the GPU device in the menus

Status: current

Branch `fix/powervr-bxm-device-lost` from `main` e272b2a2 (0.8.11 plus finding 276), started
2026-11-22. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Issue #119 (0.8.11, a MediaTek MT6855 phone with a PowerVR B-Series BXM-8-256, driver "PowerVR
B-Series Vulkan Driver 25.1@6715691", Vulkan 1.3.303, Android 16): two runs end in the menus with
`gs: the GPU device was lost (Unknown): Unexpected error variant (driver implementation is at
fault)` and the lost-device box. There is no such phone here: the cause comes from the shared log
(the lead keeps it), the code and what others found.

Acceptance: the place and the cause with the evidence, or what narrows it; a fix in the one path
it belongs to, or the log lines that decide it next time; `DHD_BUILD_SLOTS=4 python tools/check.py
port -j 6` and `check.py docs`; boot and race state traces equal to the base if anything that can
change game state or drawing changed. No phone, no timing benchmark, no visible window.

## Progress

- [x] 2026-11-22: finding reserved (8f02acdd).
- [x] 2026-11-22: the log read (below: "What the log shows").
- [x] 2026-11-22: the cause, with outside evidence (Result).
- [x] 2026-11-22: the fix and its log line, feace4bb: the vendored wgpu-hal makes every
  vkCreate*Pipelines result but VK_ERROR_DEVICE_LOST that pipeline's error, and logs the Vulkan
  result behind any other "Unexpected". `tests/pipeline_errors.rs` (renderer crate) checks the
  mapping: VK_ERROR_UNKNOWN, VK_ERROR_INITIALIZATION_FAILED, VK_ERROR_INVALID_SHADER_NV and both
  out-of-memory results give a `Linkage` error naming the result; VK_ERROR_DEVICE_LOST stays
  `DeviceError::Lost`. With 176's mapping VK_ERROR_UNKNOWN was `Device(Unexpected)`, which the
  first test refuses.
- [x] 2026-11-22 checks: see "Checks".
- [ ] the reporter's run of a build with the fix (unproven on the device).

## What the log shows

Two runs (the player's Share log; 0.8.11; FPS 240 and 120; shadows, flares and particles off in
one, on in the other). The order of lines is the same in both:
- Start: device, render probe, the exact renderer (raster `main` takes 4.9-6.0 s to compile; the
  watchdog's "died at main" is that slow compile, it returned), the native renderer (0.9 s), then
  `pipelines.txt lists 123 draw pipelines of earlier runs; 123 are built ahead on a worker
  thread`. ThreadId(3) builds them one after another, about 0.1 s each (the 40 the startup
  breadcrumbs show took 3.97 s), half mesh pipelines (Native VU), half GS draws. 123 at that rate
  is about 12 s.
- The menus' cards draw and present for hundreds of fields (the breadcrumbs retire at picture 3).
- The first 3D picture (`the full width again (VU1 primitives 13901)`, field 353 in one run, 216
  in the other), 2 s later the `perf: render` line, then `gs: the GPU device was lost (Unknown):
  Unexpected error variant (driver implementation is at fault)`, found by `dhdr_native_present`,
  and at once `gs native: the 123 listed pipelines were built 10.5 s` (11.1 s) `after the
  renderer's start`. The freeze line puts the loss at field 536 (menu 0, the title) and field 691
  (menu 15, SELECT RIDER).
- So the loss is not tied to rider select (one run was still at the title) or to the 3D scenes'
  start (3.5 s after it in one run, about 8 s in the other), but to the same place in the
  worker's build: 10.5 and 11.1 s after the list was sent, about 1.5 s before the build would
  end. The "were built" line is written when the render thread finds the worker's queue empty;
  after a loss every remaining build fails at once, silently (a listed key's failure waits for its
  first draw, `Pipelines::receive`), so the line follows the loss straight away. The worker was
  building when the device was lost.
- "Unexpected error variant" is wgpu-hal's `DeviceError::Unexpected`: a Vulkan result the call is
  not mapped for (not out-of-memory, not device lost). A GPU hang or fault reports "Device is
  lost" (the Mali-G52 of finding 198, the Adreno 505 of finding 241). wgpu-core loses the device
  on `Unexpected` as on a real loss (`Device::handle_hal_error`).

## Decisions

- **Every pipeline-creation result but device lost is the pipeline's error, for every driver**
  (in the vendored wgpu-hal, as finding 176's out-of-memory). The Vulkan spec leaves the device
  usable after a failed vkCreate*Pipelines; of its results only VK_ERROR_DEVICE_LOST means the
  device is gone. No PowerVR switch: the mapping only matters when a pipeline fails.
- **What follows a refused pipeline is already there** (findings 176, 194): the native renderer
  logs `gs native: draw pipeline KEY: native pipeline KEY: ... the driver did not create the
  pipeline (ERROR_UNKNOWN) (fs ..., blend ..., ...)` once and skips that pipeline's draws; a mesh
  pipeline turns Native VU off for the session (`refuse_meshes`), so the VU's GS draws replace
  it; a specialised pipeline falls back to its generic one. The exact renderer's pipelines were
  already an Err on an internal error.
- **The Vulkan result behind any other "Unexpected" is logged** (`get_unexpected_err`, at most 8
  error lines: `dhd_render: ERROR wgpu_hal::vulkan: Vulkan returned ERROR_UNKNOWN (-13), which
  wgpu-hal reports as an unexpected device error`): if the loss comes from another call (a
  command buffer's begin or end, a fence reset), the next log says which result it was.
- **No other workaround is extended to the B-Series.** The keyed ones were read: Mali's
  one-encoder rule (finding 198, vendor 0x13B5), the GE8320 pipeline breadcrumbs (`trace_gpu`,
  #68), the Adreno 505 loss context (finding 241), Intel's dual-source off (finding 188). The
  Mali loss also came seconds after the first 3D picture, but as "Device is lost"; here the
  result and the timing point at a pipeline build, and extending the cut rule without that
  evidence would cost the speed it was measured for.
- **No state trace**: the change is in wgpu-hal's mapping of a failed Vulkan call; where nothing
  fails (this PC, every device that ran 0.8.11 fine) nothing runs differently, and the traces open
  no GPU device (finding 201).

## Surprises

- **Not rider select.** The screenshot is SELECT RIDER, but the other run lost the device at the
  title; the time into the background build is what the runs share.
- **The worker's refused pipelines are silent.** A listed key that fails is not logged until a
  draw needs it (`receive`), so with the device lost the log could not name the pipeline. After
  the fix its first draw rebuilds it on the render thread and logs it.

## Result

The cause, with the evidence (confidence: fairly high, about 75 %; never run on that GPU): the
driver refused one of the 123 draw pipelines the native renderer builds ahead on its worker thread
(about the 105th-110th in the list; which one the log cannot say) with a Vulkan result upstream
wgpu-hal does not expect from vkCreateGraphicsPipelines, most likely VK_ERROR_UNKNOWN, and
wgpu-core lost the device for it.
1. **The result**: "Unexpected error variant (driver implementation is at fault)" is what
   wgpu-hal's Vulkan backend makes of any result it does not map; on pipeline creation that was
   anything but out-of-memory (`map_pipeline_err` -> `map_host_device_oom_err`). Finding 176 made
   the out-of-memory results the pipeline's own, nothing else.
2. **This GPU's driver does it**: a wgpu program on a BXM-8-256 (openOMSI issue #1990) reports
   `vkCreateGraphicsPipelines failed: An unknown error has occurred` (VK_ERROR_UNKNOWN) for one of
   its render pipelines; a PSP emulator on a Dimensity 7025 BXM phone (Yakumo issue #372) the
   same; Imagination's forum has VK_ERROR_UNKNOWN from vkCreateComputePipelines on a BXM-4-64
   (driver 24.2) for valid SPIR-V. wgpu issue #10027 is the same chain on other GPUs: a failed
   pipeline creation, "Unknown: Unexpected error variant (driver implementation is at fault)",
   every error scope empty, the device gone.
3. **The timing**: the loss falls at the same point of the worker's build in both runs (10.5 and
   11.1 s into a build of about 12 s, the same list of 123 keys), not at a point of the game, and
   the build ends with it.

Ranked lower: a GPU fault in the 3D pictures (it would report "Device is lost", and the runs were
3.5 s and 8 s into them); the encoder cut (Mali's loss, finding 198, was "Device is lost"); thread
safety of the driver's compiler (Imagination's forum, BXM-8-256 on 1.15@6133110: a crash in
`BILParseStream` with concurrent pipeline creation; here one thread compiles at a time, and that
is a crash, not a result).

What changed: `port/render/vendor/wgpu-hal/src/vulkan/mod.rs` (`map_pipeline_creation_err`: every
result but VK_ERROR_DEVICE_LOST; `get_unexpected_err` logs the result; `map_pipeline_err` removed,
unused), `vendor/wgpu-hal/PATCH.md`, comments in `src/native/init.rs` and `src/exact/pipelines.rs`,
`tests/pipeline_errors.rs`.

What the player gets once it ships: if the cause is this, the game goes on; the refused
pipeline's draws are skipped (a missing piece of the picture) or, for a mesh pipeline, Native VU
turns off for the session. dhd.log names the pipeline (`gs native: draw pipeline KEY: ... the
driver did not create the pipeline (ERROR_UNKNOWN) (fs ..., spec ..., blend ..., write mask ...,
depth ...)`) and, for a mesh one, that Native VU went off. If the device is still lost, the new
error line names the Vulkan result of the call that lost it.

What the player can try on 0.8.11, by the evidence: SETTINGS, renderer Exact (the exact renderer
builds no native draw pipelines; its compute pipelines built on this phone; slower); with the fix,
Native again, and Share log after a run.

Still open: the reporter's run of a build with the fix; then, from that log, which pipeline the
driver refuses and what in it (a blend, dual-source output, a texel form), so that draw can be
made in a form the driver takes, as finding 176 did.

## Checks

On feace4bb, this PC (Windows, RTX 3070):
- `rustgs.py test --target x86_64-pc-windows-msvc -- --test pipeline_errors --test
  semaphore_values`: 6 tests pass.
- `rustgs.py check --target aarch64-linux-android`: OK, no warnings.
- `DHD_MODULE_CACHE=0 DHD_BUILD_SLOTS=4 python tools/check.py port -j 6`: every step OK
  (test_gs_wgpu, native_replay included).
- `check.py docs`: OK.
- Not run: state traces (Decisions), a phone (none attached; no PowerVR phone here), a forced
  pipeline failure in a game run (no way to make this PC's driver refuse a pipeline).

Sources: gfx-rs/wgpu issue #10027 (a failed vkCreateComputePipelines loses the device as
"Unexpected error variant"); openOMSI-org/openOMSI issue #1990 (BXM-8-256, vkCreateGraphicsPipelines
VK_ERROR_UNKNOWN); TeamGDB/Yakumo issue #372; Imagination's PowerVR developer forum threads
"PowerVR B-Series 1.15@6133110 on BXM-8-256 (Dimensity 7020): ten Vulkan driver issues with
evidence" and "BXM-4-64: vkCreateComputePipelines fails (VK_ERROR_UNKNOWN) on valid SPIR-V".
