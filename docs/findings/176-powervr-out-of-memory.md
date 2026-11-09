# 176: PowerVR GE8320 phones close in the Bike Shop ("Out of Memory")

Status: current

Branch `fix/powervr-oom` from `main` b0812d6, started 2026-11-07. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

Issue #56 (OPPO CPH2269, Helio G35, PowerVR Rogue GE8320, driver "PowerVR Rogue Vulkan Driver
1.13@5776728", Android 11, 2.7 GB RAM). With finding 170's test build (0.7.12, its fix and the
pipeline trace) the game starts and reaches the menus, then closes when the rider enters the Bike
Shop. The shared log (kept by the lead, not in the repository) ends with two pipeline traces,
`native draw`, native_vertex and `native_pixel_u` (the specialised one with `native_spec`, then the
generic one), colour blend Zero / One Add, alpha One / Zero, write mask 7 (RGB), Depth32Float
GreaterEqual without writes, and then `dhd_render: wgpu error in dhdr_native_picture_indexed: Out of
Memory` (fatal through `guard.rs`'s uncaptured-error handler; the process used 374 MB PSS).

Goal: the game plays on these phones (the Bike Shop, a race) with one code path for every GPU,
pictures unchanged on other GPUs, and a pipeline the driver still refuses fails that one draw with
a log line naming it, not the game.

Acceptance: the cause or ranked candidates with evidence; the fix; `check.py port`; the
`native_spirv` checks; a release-signed test APK (this branch plus the pipeline trace patch, a local
test branch); on the Mi 11i (Adreno) it starts and reaches the Bike Shop.

## Progress

- [x] 2026-11-07: finding reserved.
- [x] 2026-11-07: the driver reports a refused shader as out of memory: in the driver's
  `vulkan.mt6765.so` (the same 1.13@5776728 build, from a public MT6765 firmware dump), the
  pipeline functions set their result to -1 (VK_ERROR_OUT_OF_HOST_MEMORY) when `BILParseStream`
  returns NULL or `PVRUniFlexCompileToHw` returns non-zero.
- [x] 2026-11-07: why "Out of Memory" ends the game: wgpu-core loses the device on any HAL
  out-of-memory (`Device::handle_hal_error`), and the draw pipeline's error scope took only
  validation errors, so the error went to the fatal handler.
- [x] 2026-11-07: the pipeline: in the log, the same fragment stage (`native_pixel_u`, the same
  `native_spec` and texel scale, the same vertex stage and layout) was built with no blend and mask
  RGBA in the run before; depth GreaterEqual without writes, src Zero with OneMinusSrcAlpha, and
  One / One with mask RGB were all built in this run. Zero / One appears only in the last two
  traces (Result, candidate 1).
- [x] 2026-11-07: the fix, 9c2349e: Cs * 0 + Cd * 1 (or Cd * 1 - Cs * 0) is drawn with no blend
  and only its alpha channel written (`pipelines::without_kept_colour`), and a draw left with no
  channel and no Z write is not drawn (`render.rs` `draw_passes`). The Bike Shop's draw (mask RGB,
  no Z write) is not drawn at all; with alpha written it is the plain mask-A pipeline, a form this
  phone builds.
- [x] 2026-11-07: the soft failure, 58281b5: the vendored wgpu-hal reports vkCreate*Pipelines'
  out-of-memory results as that pipeline's linkage error (an internal error in wgpu-core, which
  then neither loses the device nor goes to the fatal handler); the native renderer builds its
  draw pipelines in a Validation and an Internal error scope (`init::scoped_pipeline`), so a refused
  pipeline is logged once, with what it is, and its draws are skipped:
  `gs native: draw pipeline KEY: native pipeline KEY: ... the driver did not create the pipeline
  (ERROR_OUT_OF_HOST_MEMORY) (fs native_pixel (texels) spec ... texel scale 1, blend Zero/One Add,
  write mask 7, depth GreaterEqual)`, or `specialised pipeline ...: ...; the generic one draws` for
  the worker's. The exact renderer's pipelines take the internal error as they took out of memory
  (an Err from `Pipelines::create`). The temporary shader modules are now destroyed on failure too.
- [x] 2026-11-07: the audio crash at exit, d3ec394 (Result, "AudioTrack").
- [x] 2026-11-07 checks: `check.py port` OK (core, runtime tests, game, net tests, `test_gs_wgpu
  --skip-hardware`, wasm2c chunks). `native_spirv` OK (no shader changed). The native pictures:
  `test_native_transfers.py` with four new scenes of the kept colour (efb0f1d: alpha written with Z
  writes, alpha masked with Z writes, alpha masked read only, AFAIL FB_ONLY), run with this branch
  and with `draw_passes` as on main (the blend kept): all 37 pictures and their VRAM dumps, with
  and without dual-source blending, are the same bytes (RTX 3070, DX12; the harness does not pass
  DHD_WGPU_BACKEND on). No game state can change (the native renderer and the sound output only),
  so no state trace.
- [x] 2026-11-07: the test build for players: local branch `test/powervr-oom-1` (this branch plus
  the pipeline trace patch 1165468, as e3c81d4), built as 0.7.12's APK is (package_release.py,
  the phone profile, the release key), versionCode 712.
- [x] 2026-11-07: on the Mi 11i (Adreno 660): the test build starts and draws the title (64
  pipelines traced, no error). The same build with the local env-file hook of finding 154
  (`files/dhd-test.env`, never committed) and DHD_NAV
  `0:triangle|cross,15:cross,3:cross,16:cross,1:cross,11:cross` reaches the Bike Shop's inventory
  (rider, bike and jersey drawn; menus at 59.9 fps; 83 pipelines traced, none with blend Zero /
  One, no error). Afterwards 0.7.12 as shipped was installed again (its APK's hash checked).

## Decisions

- **The draw's form changes for every GPU**, no PowerVR switch: Cs * 0 + Cd * 1 draws as no blend
  with the colour channels masked (exact on a UNORM target), and a draw left with no channel and no
  Z write is not drawn (native.wgsl's fragment stages write nothing but colour; discard only drops
  writes).
- **A pipeline-creation out-of-memory is that pipeline's error**, in the vendored wgpu-hal (one
  code path): Vulkan leaves the device usable after a failed vkCreate*Pipelines, and wgpu-core's
  answer to a HAL out-of-memory is to lose the device. A true out-of-memory there loses one draw
  too, which is better than a lost device; every other result keeps upstream's mapping.
- **The Internal scope only around pipelines**, not in `scoped` (textures, buffers, shared
  objects): a real out-of-memory elsewhere stays fatal, as before.
- **Skipping the draw is exact**: native.wgsl's fragment stages have no storage writes or other
  side effects, and the draw has no stencil or queries; with no channel and no Z write it changes
  nothing. The C side already skips a draw with no mask and no Z write (`native_state`); the
  renderer now also does when its blend keeps the colour.
- **The pipeline trace stays out of the player build**: a test-branch patch, as in 153 and 170.

## Surprises

- **The missing "built in" line is not evidence**: the log only names pipelines that took over
  0.25 s, and 27 of the last run's 62 traces have none. The evidence is the state (above).

- **"Out of Memory" with a third of a 2.7 GB phone used**: the driver's graphics and compute
  pipeline functions return VK_ERROR_OUT_OF_HOST_MEMORY when its SPIR-V front end or its USC
  compiler fails, whatever the reason.

## Result

Merged into `main` 2026-11-09 for 0.7.14 (the testers of test-powervr-2 had not answered yet).

The cause, with the evidence:
1. **The error is a compiler's.** The driver (`vulkan.mt6765.so`, PowerVR Rogue 1.13@5776728, the
   build the player's log names; the dump's `libsrv_um.so` carries `1.13@5776728`): in its graphics
   pipeline path the result goes to -1 at `0x5c70c` (`PVRUniFlexCompileToHw` non-zero), `0x5d5f0`
   (another USC compile) and `0x5da14` (`BILParseStream`, its SPIR-V front end, returned NULL),
   and the function tests the result's sign on the way out (`0x5e6c0`); its compute path does
   the same for both (`0x61f2c`). -1 is VK_ERROR_OUT_OF_HOST_MEMORY: wgpu-hal maps it to
   `DeviceError::OutOfMemory`, wgpu-core (`Device::handle_hal_error`) loses the device and reports
   an out-of-memory error, and the draw pipeline's scope (Validation only) let it reach
   `guard.rs`'s handler: "wgpu error in dhdr_native_picture_indexed: Out of Memory", fatal. Its
   strings also show where blending goes: `PFOFrameBufferBlending`, `PFOGenerateProgram`,
   `CreateSpecialObjectPixelShader: PVRUniFlexCompileToHw failed`: the blend state is compiled into
   the fragment program (the pixel output program), so a blend state alone can make it fail.
2. **The pipeline is the Bike Shop's blend.** Ranked:
   1. Blend Zero / One (Cs * 0 + Cd * 1, the GS's (Cs - Cs) * As + Cd) with write mask RGB: every
      written channel ignores the source, so the colour output is dead in the program the driver
      builds. The same fragment SPIR-V, vertex stage, layout and depth state were built on this
      phone with other blends and masks; Zero as the source factor (with OneMinusSrcAlpha) and One
      as the destination factor (with One) were built too. Only this combination is new, and it is
      the last thing traced. The fix removes the blend (exact) and the draw, which writes nothing.
      Not confirmed on a GE8320 (no phone here; Imagination's offline compilers take SPIR-V only,
      no blend state).
   2. A draw with write mask 0 and Z writes (the GS's FBMSK all ones, or ZB_ONLY; with this fix
      also a kept colour with alpha masked and Z written): its colour output is dead too. No such
      pipeline is in the log; if the driver refuses one, the test build logs it and goes on
      without that draw (Z not written there).
   3. Excluded: real memory. 374 MB PSS of 2.7 GB; the arenas grow by doubling from 1 MiB
      (vertices from 8 MiB) to the need, a few MiB for a menu. Two threads compiling at once
      (Godot #123614, cinnabar #17 crash in `libufwriter.so` that way): the worker and the render
      thread built pairs at once about 30 times in the log without failing.
3. **Why it ended the game**: wgpu-core loses the device on any HAL out-of-memory; changed in the
   vendored wgpu-hal for pipeline creation (Progress, 58281b5).

**AudioTrack** (the run that ended with EXIT GAME): `crash: SIGSEGV at ... in thread AudioTrack`
after the session's perf line, so during `exit()`, after our atexit handlers; Android recorded
EXIT_SELF, status 0. `exit()` runs the libraries' static destructors while SDL's AAudio device
thread is still in its callback (the stream was never closed). The stream now closes in an atexit
handler registered when it opens, which runs before them. Likely, not proven: the log has no
frames for that thread.

Left for a phone test (the lead, with #56's player): the test build on the GE8320 phone, into the
Bike Shop and a race. If it still closes, Share log; a pipeline the driver refuses is now a
`gs native: draw pipeline ...` line naming it, and the game goes on.

How to look again (no driver binary or path in the repository): finding 170's "How to look
again" finds a dump; `vendor/lib64/hw/vulkan.mt6765.so` from the same dump, `llvm-objdump -d`
(NDK); the call sites are the `bl` to the `PVRUniFlexCompileToHw` and `BILParseStream` PLT
entries.
