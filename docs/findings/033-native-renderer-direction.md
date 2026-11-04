# Native renderer direction

Status: history

The 2026-10-15 decision to move from the round-27 compute rasterizer (finding
032) to hardware rasterization, Windows first on Direct3D 11. Built as the D3D11 hardware bridge
(finding 035), then the D3D11 native renderer (finding 038); all D3D11 code is retired. Today:
the Rust wgpu renderer with an exact front end (`port/gs/gpu_exact.inc`) and a native, upscaled
one (`port/gs/native_core.inc`, the default), findings 084, 085, 088.

## The direction (what still holds)
- Keep the exact GS path as the reference and fallback; hardware paths must pass the same
  state, image and readback checks. The lens flare's Z readbacks change game behaviour, so
  "looks the same" is not enough.
- Native rasterization does not reproduce by itself: GS coverage, integer depth interpolation,
  alpha-failure actions, channel masks, blend rounding, framebuffer/depth aliasing. Keep
  unsupported cases on the reference path; never loosen tolerances or exclusions to pass.
- Treat CPU uploads, palette changes, render-to-texture, local copies and depth readbacks as
  explicit coherence events; never keep a stale decoded texture.
- Stages planned: hardware raster on a bounded subset → versioned decoded textures/targets →
  mesh/material submission above GIF/VU packets → presentation/interpolation last. The first
  two happened (035, 038, then 085/086); game-level mesh submission was never built (finding 036,
  037 explored it); interpolation was tried and rejected by the user (finding 049).

## Measurements that justified the first step
- Round 27's compute rasterizer: 44.98 → 63.24 fps uncapped (RTX 3070), still doing coverage,
  depth, texturing and blending in a compute shader.
- The RTX 3070 offers ROV and conservative rasterization tier 3 at FL11_1 and up, not at the
  port's FL11_0 device then. A synthetic prototype (hardware conservative raster + exact integer
  edge test + ordered per-pixel writes) passed 1,516 checks over 12,419,072 readback words
  (`test_hw_coverage`, removed with D3D11). ROV order protects overlapping screen pixels only,
  not different pixels aliasing one VRAM word: alias checks stay.

Full text: `git show 860ab6b:docs/findings/033-native-renderer-direction.md`
