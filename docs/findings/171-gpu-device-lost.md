# 171: The picture freezes when the GPU device is lost

Status: current
Checked: 2026-11-07

Branch `fix/device-lost` from `main` fce8a1f (0.7.12), started 2026-11-06. This file is the lane's
plan while it runs and its finding when it merges. Merged into `main` on 2026-11-06 (9b88d82).

## Goal and acceptance

Issue #58 (Windows 11, Intel i5-1035G1 with Intel UHD Graphics, driver 30.0.101.3111 from 2021,
7.7 GB RAM): as soon as the game starts the picture freezes while sound and input go on. dhd.log
(kept by the lead) shows, right after the first present:
`dhd_render: ERROR wgpu_hal::auxil::dxgi::result: Present failed: The GPU device instance has been
suspended. Use GetDeviceRemovedReason ... (0x887A0005)` then
`dhdr_native_present: get_current_texture: validation error`, and the menus at 27 fps with 1 s
stalls. 0x887A0005 is DXGI_ERROR_DEVICE_REMOVED: the driver reset or removed the device (a TDR,
a driver crash, a driver update, a GPU hang); wgpu's device is then lost for good.

Goal: a lost device never leaves a frozen picture. At least: the game notices (wgpu's device lost
callback and the error scopes), logs `GetDeviceRemovedReason` (or the backend's reason) once, and
either recreates the device, surface and every GPU object and carries on (preferred, if the
renderer's state can be rebuilt from the GS memory it keeps), or shows a clear message ("The
graphics driver stopped responding; update it and restart the game") and exits cleanly. Also find
why this laptop loses the device at the first present (a hang in our first frame? TDR from a long
GPU job? the old driver with D3D12?) with evidence from its log and DxDiag (the lead keeps them),
and whether a fallback (Vulkan on Windows, `DHD_WGPU_BACKEND=vulkan`, which exists for development)
would be a sensible automatic retry. One code path; the AGENTS.md decisions stand (no D3D11).

Acceptance: a reproduction of device loss on this PC (e.g. a forced device loss: D3D12's
`ID3D12Device5::RemoveDevice` through wgpu-hal, or `dxcap -forcetdr`-style tooling if available,
on the hidden desktop: never a visible window or focus steal), the game recovering (or exiting with
the message) instead of freezing, the cause for #58 as far as the evidence goes, `check.py port`
passing, and pictures unchanged in normal runs.

## The cause for #58, as far as the evidence goes

- The player's DxDiag lists 10 Windows Error Reporting `LiveKernelEvent` records with code 141:
  `VIDEO_ENGINE_TIMEOUT_DETECTED`, a GPU engine that stopped responding for longer than Windows'
  timeout (TDR, 2 s by default), after which Windows reset the driver. Ten of them: about one per
  start of the game. With a TDR every D3D12 device on the GPU is removed; the next present gets
  0x887A0005 (`DXGI_ERROR_DEVICE_REMOVED`, the "device instance has been suspended" text in the
  log). So #58 is a GPU hang, not a driver update or a removed GPU.
- When: both logs (0.7.11) show it at the same place. The picture ran for more than 2 s (the
  `perf: render` line needs 2 s of the same size before it is written), the first 3D picture came
  at field 148 ("the full width again (VU1 primitives 9163)": the title screen's 3D scene), and the
  next present after `perf: render` failed. The menus' cards (field 10 on) drew and presented
  fine; the hang began with the first 3D field or within the 2 s after it, the TDR's own timeout.
- The machine: HP 240 G8, i5-1035G1 (Ice Lake, Intel UHD Graphics G1, Gen11), driver
  30.0.101.3111 (early 2022 build), WDDM 3.0, hardware scheduling off, 128 MB dedicated memory.
  The GPU's own time per picture before the hang was short (`gpu_ms/frame=5.68`): it is not a
  long but finite job; something never finished. Our settings there: native renderer, scale 2,
  dual-source blending (`GS blending with dual-source blending`), DXC shaders, Mailbox present.
- Which draw or dispatch hung is not in the evidence: D3D12 does not say, and wgpu-hal logs only
  the present's error. Known neighbours (searched 2026-11-06): wgpu #849, a D3D12 device lost
  (`DEVICE_HUNG`) on an Intel iGPU that the runtime's validation saw nothing wrong with
  (driver bug suspected); PCSX2 #9728, Intel D3D12 drivers crashing at a game's start, which
  PCSX2's maintainer put on the drivers; older Intel bugs with dual-source blending (PCSX2 #345,
  PPSSPP). Our own suspects, untested on that GPU: the exact renderer's compare-exchange loop in
  `raster.wgsl` (`atomicCompareExchangeWeak` until it succeeds: a spin a driver can schedule
  badly), and dual-source blending in the draw shaders.
- So the evidence: an Intel Gen11 GPU on a 2021-22 driver hangs on our first 3D picture, every
  start. Asking the player for a newer Intel driver (Intel's 31.0.101.2xxx for Gen11) and, if it
  still happens, one start with `DHD_GPU_DRED=1` (below) names the operation that hung;
  `DHD_WGPU_DUAL_SOURCE=0` tells whether dual-source blending is the trigger.

## Progress

- [x] 2026-11-06: finding reserved (923a6de)
- [x] 2026-11-06: evidence read: the cause section above
- [x] 2026-11-06: a lost device is noticed, its reason logged once, and the game ends with a
  message instead of a frozen picture; `DHD_TEST_DEVICE_LOST` and `DHD_GPU_DRED` (38a988c)
- [x] 2026-11-06: reproduced on this PC (hidden desktop, both renderers, DRED on); `check.py port`
  passes (`test_gs_wgpu` PASS); `rustgs.py check` passes for Linux and the browser
- [x] 2026-11-06: merged into `main` (9b88d82)
- [ ] the lead: one check on the merged branch; ask #58's player for a newer Intel driver, then
  (if it still hangs) one start with `DHD_GPU_DRED=1` and one with `DHD_WGPU_DUAL_SOURCE=0`

## How it works now

- wgpu's device-lost callback (`port/render/src/lost.rs`, installed by `device.rs` right after
  the device is made) logs wgpu's message once (`gs: the GPU device was lost (Unknown): ...`) and
  sets a flag. It runs inside the failing wgpu call (the present, a submit), so it does nothing
  else. A surface's acquire then fails at once.
- The present that failed (the native renderer's `native_present_draw`, or the exact renderer's
  `gs_gpu_present`) asks `dhdr_device_lost` (`dhd_render_ext.h`); its first yes logs the reason:
  on D3D12 `GetDeviceRemovedReason` with its meaning (`DXGI_ERROR_DEVICE_HUNG: the GPU stopped
  responding ...`), the GPU and the driver; elsewhere the backend, GPU and driver. C sets
  `gs_gpu_lost`.
- `present.c`'s `gs_pump_messages` (the game thread, every field) sees it: logs `gs: the GPU
  device was lost; the game ends`, hides the window, shows the box "The graphics driver stopped
  responding and was reset ... Update the graphics driver ... and start the game again" and exits
  with code 1 (as `graphics_failed` does when the graphics never start). The box is
  `host_error_box` (`port/host/host_sdl.c`, SDL's message box); `DHD_HEADLESS` shows no box.
- Where: Windows, macOS, Linux and Android share this path (`gpu_rust.c`, `present.c`); only
  D3D12 was tested (Reproduction). Elsewhere it ends the game when wgpu reports the loss through
  the callback; the reason logged is then the backend, GPU and driver. The browser does not end the
  game: its device has its own callback (`port/render/src/web/init.rs`: it logs `dhd_render:
  WebGPU device lost` and makes later reads refuse), and its native present's lost-device hook
  does nothing (`port/web/gpu_rust_web_native.inc`).
- `DHD_GPU_DRED=1` (Windows, D3D12): DRED's auto-breadcrumbs and page-fault reports are switched
  on before the device is made; the lost-device report then names each command list the GPU had
  not finished (wgpu's encoder labels: `native picture`, `present`, ...) and the operations around
  where it stopped (`>DrawInstanced` marks the first unfinished one), and a page fault's address.
  Off by default: DRED costs a little on every command.
- `DHD_TEST_DEVICE_LOST=N` (tests): at the N-th frame a surface acquires, the D3D12 device is
  removed with `ID3D12Device5::RemoveDevice`. That marks only this process's device removed (its
  fences signal, its calls fail): no GPU reset, no other program touched, so it is safe on a PC
  someone is gaming on. A forced TDR (`dxcap -forcetdr`) resets the GPU under every program and
  is never used. On Vulkan and Metal the switch only logs that it can't.

## Decisions

- **End the game with a message, not recreate the device.** The device is the process's one
  (`device.rs`: a `OnceLock`, every renderer object holds `&'static Gpu`), and the exact
  renderer's GS memory, which answers the game's Z probe (game state), lives in its buffers.
  Recreating means rebuilding every object and refilling GS memory from a CPU copy that the
  native path does not keep current, which is a rewrite of the device's ownership. And for #58 it
  would not help: the GPU hung on the same work at every start, so a new device would hang again
  and cost another TDR, a screen-wide driver reset each time. A box that says what happened and
  what to do, and a clean exit, is what the renderer can do soundly.
- **No automatic retry on Vulkan.** On Windows Vulkan is a development switch
  (`DHD_WGPU_BACKEND=vulkan`); making it a player's automatic fallback would be a second
  platform path on Windows nobody tests (finding 099, one code path). There is no evidence the
  hang is D3D12's: Intel's Vulkan and D3D12 drivers share the shader compiler and the hardware, so
  the retry could just hang the GPU once more, a second desktop-wide TDR the player did not ask
  for. A player can still try `DHD_WGPU_BACKEND=vulkan` by hand as a diagnosis.
- **No exit from inside wgpu's callback.** wgpu calls it with its locks held, from whichever
  thread hit the loss; a box there would hold the renderer's locks. The flag goes to the game
  thread, which ends the game the way the close button does.
- The reason is logged once, from `dhdr_device_lost` outside wgpu's locks (the hal device is
  reached with `Device::as_hal`, a hub lookup).

## Reproduction (this PC: RTX 3070, D3D12, driver 32.0.16.2101)

A hidden-desktop game (`hidden_game.Game`, no GS captures so the graphics stay threaded as a
player's, `DHD_SCALE=2`, `DHD_TEST_DEVICE_LOST=200`), the box answered with OK by a posted
`WM_COMMAND IDOK`, then the exit code read:

| Run | Log | Box | Exit |
|---|---|---|---|
| native renderer | `test: ... RemoveDevice`, `gs: the GPU device was lost (Unknown): Device is lost`, `gs native: presenting failed`, `gs: D3D12 removed reason 0x887A0005: DXGI_ERROR_DEVICE_REMOVED ...`, `gs: the GPU device was lost; the game ends` | after 14 s | 1 |
| exact renderer (`DHD_RENDER` unset) | the same, through `dhdr_present_exact` (`dhd_render: present: the GPU device was lost`) | after 12.5 s | 1 |
| native, `DHD_GPU_DRED=1` | the same, plus `gs: D3D12 DRED on` and the DRED summary (every command list finished: RemoveDevice met an idle GPU) | after 15.5 s | 1 |

Before (0.7.12, which has no test switch; the behaviour in #58's logs): after the loss the game ran
on with its last picture, `presenting failed; no more pictures are shown` and nothing else.

Not reproduced: a real GPU hang (an endless shader, a forced TDR), which DRED's "not finished"
lines are for. Both would reset the GPU for every program on this PC, the owner's game included,
so they were not run.

## Surprises

- wgpu says nothing after a loss: WebGPU's rules make every call on a lost device a silent no-op,
  so the uncaptured-error handler (which aborts on any validation error) never fired, and the
  only trace was wgpu-hal's present error and `get_current_texture: validation error` (wgpu-core's
  `check_is_valid` on the invalid device). The game ran on, sound and input fine, at 27 fps with
  stalls of 1 s (`max=1015.0ms`; probably waits on the dead device timing out, not measured).
- wgpu-hal's D3D12 adapter makes its `ID3D12Device` when adapters are enumerated, and
  `request_device` reuses it: DRED switched on just before `request_device` gave no breadcrumbs.
  It has to be on before the instance enumerates adapters.
- `RemoveDevice` reports `DXGI_ERROR_DEVICE_REMOVED` (0x887A0005) as its removed reason, the same
  code #58's present returned; after a TDR the removed reason is usually `DXGI_ERROR_DEVICE_HUNG`
  (Microsoft's documentation; not seen here).

## Result

- What changed: a lost device (any backend) is noticed through wgpu's device-lost callback; the
  failed present logs the backend's reason once (`port/render/src/lost.rs`,
  `dhdr_device_lost`); the game thread ends the game with a box and exit code 1
  (`port/gs/present.c`, `graphics_lost`). No device is made again and nothing falls back to
  Vulkan (Decisions). Normal runs are untouched: per acquired frame two atomic loads (the lost
  flag and the test switch's cached setting), per field one.
- Checked: the forced removal on both renderers (Reproduction); `python tools/check.py port` OK
  (docs, core, runtime tests, game, net tests, `test_gs_wgpu --skip-hardware` VERDICT PASS,
  wasm2c chunks); `rustgs.py check` for `x86_64-unknown-linux-gnu` and the browser
  (`wasm32-unknown-unknown`, web) OK.
- Not checked: `rustgs.py check --target aarch64-apple-darwin` stops in `ring`'s build script on
  this PC (no C compiler for Apple), before our code; the Mac takes the same non-Windows path as
  Linux. No state trace: nothing that runs before a loss changed. Not run on the player's Intel
  GPU, and no real hang was made here (Reproduction).
- Left: the cause of #58's hang itself (the cause section: a newer driver, DRED's report,
  dual-source off). A loss while the window is minimised is noticed at the next present.
