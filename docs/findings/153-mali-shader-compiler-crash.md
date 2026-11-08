# 153: The native renderer's pipelines on Mali's r54p1 shader compiler

Status: current

Branch `fix/mali-shader` from `main` 91f4f2a, started 2026-11-05. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

Phones with a Mali-G57 on Arm's driver r54p1 close at startup with the native renderer (issue
#47: Samsung Galaxy A07, Helio G99, Android 17; a Discord tester's Redmi with a Unisoc UMS9230E,
Android 16). The exact renderer starts. The crash is a null pointer read inside the driver's
shader compiler, `libGLES_mali.so` `cmpbe_v2_compile_multiple_shaders`, under
`vkCreateGraphicsPipelines`.

A test build that logs each pipeline before it is created (the "pipeline trace" lines, a test
patch on the vendored wgpu-hal, not on main) shows on the Redmi, in order:

1. render "render probe": present.wgsl, `present_vertex` + `present_fragment`, RGBA8, no blend: OK
2. compute "scanout", compute "main" (constants ADDRESS_SHIFT, EXCLUSIVE_WORD_STORE, EARLY_DEPTH),
   compute "local_copy" (ADDRESS_SHIFT): OK (the exact renderer's compute module)
3. render "native_blit": `port/gs/native.wgsl`, `fullscreen_vertex` + `native_blit`, RGBA8, no
   blend, no depth: **the crash**. It is the first render pipeline from native.wgsl, and a trivial
   one, so the cause is likely something every pipeline of that module carries, not this shader's
   own code.

Goal: every native.wgsl pipeline compiles on that driver, with the picture and game state
unchanged everywhere, one code path for every platform (AGENTS.md: no Mali-only second renderer;
a narrowly scoped driver workaround is acceptable only if no common form works).

Acceptance:
- The cause named with evidence: ideally the crash reproduced off the phone with Arm's Mali
  Offline Compiler (malioc, Arm Performance Studio; it carries the same `cmpbe` compiler) on the
  SPIR-V exactly as wgpu-hal 29.0.3's Vulkan backend makes it, for Mali-G57 and the newest driver
  revision malioc offers, and gone after the fix. If malioc does not reproduce it, say so and give
  the reasoning and the candidates ranked.
- After the fix, every pipeline native.wgsl can make (the init objects and the draw pipelines'
  variants: entry points, `native_texel_scale`, `native_spec`, dual-source, the texel paths)
  compiles in malioc for Mali-G57 without error.
- `python tools/check.py port` passes; the renderer's pixel tests (`test_gs_wgpu`) are unchanged.
- No game-state change (renderer only; the lead runs the boot and race state traces if the change
  can touch timing).

Out of scope: the exact renderer, other GPUs' problems, performance work.

## Progress

- [x] 2026-11-05: finding reserved; the tester's log is in the lead's notes
- [x] 2026-11-05: the SPIR-V as the phone gets it. `port/render/tests/native_spirv.rs` makes every
  stage the renderer builds the way wgpu-hal 29.0.3 does (`vulkan/device.rs` compile_stage:
  `process_overrides` on one entry point, then `spv::write_vec` with `vulkan/adapter.rs`'s options
  and the layout's binding map) for three device kinds (Vulkan 1.3 with and without robust
  access, Vulkan 1.1): init.rs's objects, the draw entry points with 37 specialisations and every
  texel scale, the dual-source module, and present.wgsl's probe: 667 stages per device kind.
  All pass `spirv-val --target-env vulkan1.3` (the NDK's SPIRV-Tools).
- [x] 2026-11-05: malioc does not reproduce it (Surprises): every stage compiles for Mali-G57 and
  for the r54p0, r56p1 Valhall cores.
- [x] 2026-11-05: the cause by elimination and a matching public report (Result): the six
  native.wgsl stages that pass a texture into a function (`load_rgba`, `load_u`, `load_depth`):
  native_blit, native_upload_alpha, native_grow_depth, native_scanout, native_scanout_fast,
  native_convert. native_blit is the first pipeline the renderer builds, and the first of them.
- [x] 2026-11-05: the fix: each texture has its own load (`load_blit_source`, ...,
  `load_convert_depth3`) reading the global; the bounds test is `outside(xy, size)`. Checks: the
  crate test `shaders::tests::no_texture_arguments` (every WGSL module the renderer builds: no
  function takes a texture or a sampler) and native_spirv's SPIR-V scan (no OpFunctionParameter
  of an image, sampler or UniformConstant pointer type); both fail on main's native.wgsl and pass
  now. All 2001 stages compile in malioc for Mali-G57 after the change.
- [x] 2026-11-05 checks: `test_gs_wgpu.py --skip-hardware` VERDICT PASS. The native pictures:
  `test_native_transfers.py` and `--presented` (WARP; 33 pictures, with and without dual-source)
  run with main's native.wgsl and with this one give the same bytes in all 396 dump files
  (pictures and VRAM). gs_golden's native-transfers goldens could not be used: they belong to the
  NVIDIA driver of 2026-10-25 and compare refuses another. `check.py quick` and `check.py docs` OK.
  `build_apk.py --jobs 6` builds the debug APK (0.7.10, arm64-v8a); its libmain.so holds the new
  native.wgsl (`load_blit_source`, no `load_rgba`). Not run: `check.py port` (the lead's
  merged-tree check), a phone.

## Decisions

- **One load per texture, not a branchless select.** Each `load_<texture>` is the old helper's
  body with the global in place of the argument (same test, same early return, same load), so the
  maths and the control flow are the old ones and the pictures cannot change. RapidRAW's fix
  (below) did the same.
- **The rule is checked for every module, not only native.wgsl**: raster.wgsl, present.wgsl, the
  dual module and the inline shaders (`DEPTH_FILL_WGSL`, `ZREAD_WGSL`, the counter's, which moved
  to a `SHADER` constant, and the touch controls'). None of the others passes a texture today.
- **Bools and the other constructs stay.** `||` in `outside` still makes a bool variable merged
  across branches (cinnabar's Mali crash was bool struct members merged across early returns,
  below), but the old helpers had the same and ran on the Realme's r41p0, and nothing ties it to
  this crash. Unchanged too: the dynamic-offset uniform, two descriptor sets, FORCE_POINT_SIZE.
- **malioc stays outside the repository**; the test finds it through `DHD_MALIOC`
  (docs/TESTING.md). Without it the test still writes every stage and scans it.

## Surprises

- **malioc cannot compile for Mali-G57 with an r54 compiler.** Every Arm Performance Studio from
  2025.2 to 2026.6 (Mali Offline Compiler 8.7.1 to 2026.5.0) compiles Mali-G57 (and the other
  Bifrost and first Valhall cores) with its r51p0 library only; the newer library it carries
  (r54p0 in 2025.2-2025.4, r55p0 in 2025.6-2026.1, r56p0/r56p1 since 2026.3) refuses Mali-G57 as
  "an invalid target core", even put in r51p0's place. Results on main's shaders, all 2001 stages
  compiled: Mali-G57 (r51p0, 2026.6), Mali-G710 (r56p1, 2026.6), Mali-G510, G310 and G710
  (r54p0, 2025.4). malioc also compiles one stage at a time without the pipeline layout, while
  the phone dies in `cmpbe_v2_compile_multiple_shaders` with both stages and the layout.
- **native_blit has not changed since a Mali-G57 ran it.** Finding 126's Realme RMX3930 (Mali-G57,
  driver r41p0) ran the native renderer on 2026-10-31. native_blit's and fullscreen_vertex's
  SPIR-V then and now differ only by two unused constants (present.wgsl's `FILL_SIZE` and
  `FILL_SHADE` since 2026-11-03: naga's compaction keeps named constants in every stage) and by
  id numbering. So the trigger is the driver: r41p0 compiles it, r54p1 does not.
- **wgpu-hal compiles every module per entry point** (`SEPARATE_ENTRY_POINTS` is set on every
  Vulkan device), so present.wgsl's probe goes through the same path as native.wgsl; the path
  is not the difference.
- **The exact renderer's pipelines are no evidence either way**: raster.wgsl reads storage
  buffers and one storage texture, all as globals, in one descriptor set.

## Result

The cause, with the evidence there is (no phone or malioc reproduction here):
- The crash: `SIGSEGV`, null pointer read at 0x40, in `libGLES_mali.so` under
  `cmpbe_v2_compile_multiple_shaders` (r54p1, Mali-G57), creating native_blit's pipeline.
- RapidRAW (also wgpu and naga) hit the same: on Pixel 7 to 10a (Mali-G715 and others, driver
  r54p3), a null read at 0x40 in `spir2lir::image_addr_is_yuv` -> `cmpbep_node_get_child` under
  vkCreateComputePipelines, from `fn sample_gf_tex(tex: texture_2d<f32>, ...)`. One function per
  texture fixed it, checked on a Pixel 10a with a wgpu 29.0.4 harness (old shader SIGSEGV, new
  one compiles); malioc 8.8.1 compiled both, as here
  (https://github.com/xerootg/RapidRawCloud/pull/45, upstream CyberTimon/RapidRAW#1954).
  naga writes a texture argument as an `OpFunctionParameter` pointer into UniformConstant; the
  likely reading (not checked in the driver) is that the image check walks from the image operand
  to its variable and, through a parameter, finds none.
- Of all the stages the renderer builds, exactly the six named in Progress have such a parameter
  (native_spirv's scan on main's native.wgsl); the probe, the draws and the exact renderer have
  none, and they compile on the phone or were never reached. native_blit is the first of the six
  the renderer builds.
- Other reports of a crash in this compiler entry, ranked below this one: cinnabar's (bool struct
  members merged across early returns, r49p1/r51p0; it reproduced in malioc r51p0, ours does not;
  https://github.com/cinnabar-mc/cinnabar/pull/2498), port0r's (ClipDistance, which Mali-G57
  lacks; our stages declare no such capability; https://github.com/jacquesdupontd/port0r/issues/2),
  pipelines built on several threads at once (cinnabar's wgpu fork, PR #7; native_blit is the
  native renderer's first pipeline, built at create, before its specialisation worker exists).

Confirmed on a phone (2026-11-05): a Discord tester's Mali phone that closed at startup with
0.7.10 starts and plays with the test build carrying this fix (test-crashlog-2: main 86faa23 plus
the pipeline trace patch). On the Mi 11i (Adreno 660) the same build creates all six changed
pipelines and draws the title screen. The phone-test plan follows for the record.

Left for a phone test (the lead): the Redmi (UMS9230E) or the Galaxy A07, 0.7.10's native renderer
on r54p1, with this branch's build: the game starts with RENDER=native and draws a race (the blit,
upload alpha, Z growth, scanout and conversion pipelines are all built at start, so a start that
reaches the menus has compiled all six); with the pipeline trace patch the log shows every native
pipeline after native_blit. Any Mali phone on r54 (a Pixel 7 or later) would show the same.
