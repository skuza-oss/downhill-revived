# 153: The native renderer's pipelines on Mali's r54p1 shader compiler

Status: in progress

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

- [ ] 2026-11-05: finding reserved; the tester's log is in the lead's notes

## Decisions

## Surprises

## Result
