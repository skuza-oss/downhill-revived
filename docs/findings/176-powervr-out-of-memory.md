# 176: PowerVR GE8320 phones close in the Bike Shop ("Out of Memory")

Status: in progress

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
- [ ] the fix: no blend for a blend that keeps the colour; a pipeline the driver refuses fails
  softly (this branch's commits).
- [ ] checks: `check.py port`, `native_spirv`, `test_gs_wgpu`, native pictures.
- [ ] test APK, Mi 11i run.

## Decisions

- **The draw's form changes for every GPU**, no PowerVR switch: Cs * 0 + Cd * 1 draws as no blend
  with the colour channels masked (exact on a UNORM target), and a draw left with no channel and no
  Z write is not drawn (native.wgsl's fragment stages write nothing but colour; discard only drops
  writes).
- **A pipeline-creation out-of-memory is that pipeline's error**, in the vendored wgpu-hal (one
  code path): Vulkan leaves the device usable after a failed vkCreate*Pipelines, and wgpu-core's
  answer to a HAL out-of-memory is to lose the device.

## Surprises

- **"Out of Memory" with a third of a 2.7 GB phone used**: the driver's graphics and compute
  pipeline functions return VK_ERROR_OUT_OF_HOST_MEMORY when its SPIR-V front end or its USC
  compiler fails, whatever the reason.

## Result

(at the merge)
