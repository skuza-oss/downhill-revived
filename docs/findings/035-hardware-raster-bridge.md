# Opt-in hardware coverage bridge

Status: history

A 2026-10-15 D3D11 bridge that let hardware rasterization generate the pixels
of the round-27 compute shader's exact pixel function (finding 033's first stage). Default-on
then (`DHD_GS_HARDWARE`, explicit `0` for compute). Retired with Direct3D 11 (finding 088): the
Rust renderer's exact and native front ends replaced it (findings 084-086).

## What it was
- Triangles only, on FL11_1 devices with ROV and conservative rasterization tier 3; other devices
  kept compute. GS 12.4 positions mapped to a fixed 2048x2048 viewport (`position/16 + 0.5`),
  exact in float for coordinates within ±65535; conservative raster gave a coverage superset and
  the pixel shader ran the same `draw_pixel` as compute (exact integer edges, double depth plane,
  texturing, fog, alpha actions, blending, masks). No hardware depth, blend or attribute
  interpolation.
- Eligible: CT32/CT24 with Z32/Z24, scissor/stride proving one word per pixel, frame and depth
  pages disjoint. Packed formats, aliasing, other primitives used compute for the whole batch,
  because ROV order covers overlapping screen pixels, not different pixels on one VRAM word.

## Results
- Fields 6000-7000 uncapped: +6.48 % in one paired measurement (63.43 fps off); 67.54 fps in
  another run. Exact in the live state/image/depth gate and on raw-VRAM fixtures.
- Sampled-frame costs that pointed at the next work: 3.3 ms in compute batches rejected for one
  zero-area triangle member; ~3.4 ms on T8 and ~2.4 ms on T4 hardware draws (texture-format
  specialisation; the Rust renderer's specialised pipelines, finding 131, are the later answer).
- Candidates never made default: zero-area members on hardware (`DHD_GS_HW_ZERO_AREA`, exact on
  82 raw-VRAM cases and the live gate, 64.34 → 65.44 fps, +1.71 % in one pair: within run
  variation) and tile-list elision for hardware batches (`DHD_GS_HW_BIN_ELISION`, never measured
  live). An FXC trap found on the way: the preprocessor treated a negative macro sentinel as
  unsigned and picked the wrong branch (fixed with an explicit `defined` guard).

Full text: `git show 860ab6b:docs/findings/035-hardware-raster-bridge.md`
