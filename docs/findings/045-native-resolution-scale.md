# Round 31: resolution scale for the native renderer

Status: partly superseded by 086 (scale 1x-4x, file names, the gate tool)
Checked: 2026-11-04

Now: the scale goes from 1x to 8x plus "Match window" (finding 086 S1, the `scale` row of
`port/render/settings/src/table.rs`, default 2x), the renderer is the Rust one
(`port/gs/native_core.inc`, finding 085), the GPU's memory can cap the scale and says so (findings
130, 136), and the gate tool is gone (state traces, finding 081). The rules below for which
surfaces scale and how coordinates map still hold.

`DHD_SCALE` (dhd.ini `scale`, launcher "Resolution (native)") draws the
native renderer (`DHD_RENDER=native`, port/gs/gpu_native.inc) at 1x (the PS2's 512 x 448), 2x,
3x, 4x, or "window" (the smallest of those whose picture is at least as tall as the window).
Default 2x (it costs nothing measurable; see "Checks"). Game state does not depend on it: `port/tools/gate_native_renderer.py` passes at 1x,
2x and 4x, and 1x pictures are bit-identical to the build before.

## Why game state cannot change

The native renderer only shows pictures. Everything the game reads back from GS memory (the
lens-flare Z probe, CPU-owned pages, every readback the gate records) comes from the exact
backend on its own device at 1x (finding 038). The native surfaces are never read by the EE, so
their size is free. The gate checks this directly: EE dumps at six moments and every readback
record match the exact arm, with `--env DHD_SCALE=N` on the native arm.

## Which surfaces are scaled

`DHD_NATIVE_SURFACES=1` prints every surface the renderer used: page, buffer width, the extent
drawn, how often it was a target, a depth buffer, a texture, a copy source. In the race:

| Surface | Pages | Width (bw) | Extent | Use |
|---|---|---|---|---|
| frame buffers | 112, 56 | 8 | 512 x 224 | drawn, scanned out, sampled by the haze pass |
| Z buffer | 0 | 8 | 512 x 224 | depth |
| menu frame buffers | 140, 70 | 10 | 640 x 224 | menus |
| shadow / blur targets | 506, 508, 510 | 1 | 64 x 64 | rider shadow drawn, blurred, sampled |

Surfaces with bw >= 4 (`NATIVE_SCALED_BW`) are drawn at the scale; narrower ones stay 1x. The
64 x 64 shadow and blur targets are drawn with 1:1 sprites and sampled with bilinear filtering
as a soft blob, so upscaling them gains nothing and would change the blur radius. Keeping
effect targets at native size is the idea of PCSX2's "Native Scaling" option. A Z buffer
takes its frame's width, so a target and its depth always share a scale; the sizes of the views
drawn together are unified (D3D11 needs equal-size target and depth views).

Surfaces are sized from the extents the GS worker claims (rounded up to 64 pixels, grown by
recreating the texture and copying the old contents), not 2048 x 2048, so 4x costs about
2048 x 1024 per frame buffer.

## Coordinates

A GS vertex at x is kept as x + 0.5 (the pixel centre); the viewport is 2048 * T wide, so it
lands at (x + 0.5) * T in a target of scale T. The scissor is multiplied by T. This keeps the
half-pixel convention of the 1x path, so no extra half-pixel offset is needed (PCSX2's
"Half-Pixel Offset" fixes the case where an upscaled renderer maps x to x * T and the picture
drifts by half a pixel per scale step).

Sprites (prim 6, flag 1024 to the shaders, only when T > 1):

- The vertex shader snaps sprite corners to whole target pixels (`ceil(position - 0.5)`), so two
  sprites that meet at a GS pixel edge meet at the same target pixel edge and no gap or overlap
  line shows. Same purpose as PCSX2's "Align Sprite".
- When the texture comes from a 1x surface or memory (E = 1 below), the pixel shader evaluates
  the texture coordinate at the corner of the T x T block the pixel belongs to (`ddx`/`ddy`), so
  a 1:1 sprite fetches exactly the texel the PS2 fetched, without the neighbour bleeding in at
  the block edge. Same purpose as PCSX2's "Round Sprite".

## Textures that are rendered surfaces

The shader gets `native_scale = T | S << 8 | E << 16`: T the target's scale, S the sampled
surface's, E = min(S, T). Texel addresses are computed in E-times GS texels, so wrapping and
clamping still work on whole GS texels; each address is then mapped into the S-scaled surface,
taking the middle of each S/E block. So:

- a 4x frame buffer read by a 4x target (the alpha haze, self-sampling copies) reads at 4x;
- a 4x frame buffer read by a 1x target (the 2:1 downsample into 510) reads each GS texel's
  middle, the same texel as at 1x;
- a 1x surface read by a 4x target (the shadow blob 508/510 over the frame) is filtered as at 1x.

`native_convert` (textures over GPU-drawn memory in another layout, finding 043) reads the scaled
source at the middle of each block, so it gives the 1x words. Copies between surfaces of
different scale, and imports from memory into a scaled surface (through a 64 x 64 1x staging
texture), go through a small blit shader; equal-scale copies stay `CopySubresourceRegion` with
scaled boxes.

## Output

Scanout reads the scaled frame buffer (`display_state[2].x = S`), so the picture handed to the
present path is S times larger in both directions and the window scaling (aspect, filter,
widescreen) is unchanged. Screenshots (`DHD_SCREENSHOT`, race captures) save the native picture
at its size (`gs_native_picture`).

"Window" decides at the first native frame: the smallest S in 1-4 with S * 448 >= the client
height (1280 x 960 -> 3x; 1080 lines -> 3x; 1440 -> 4x). Resizing later does not change it.

## Checks

- 1x, old and new build, field 4000: identical pixels.
- 4x, recorded race every 200 fields 2600-7000, compared against 1x by eye: road and ground,
  riders close up, the rider shadow, HUD (speedometer, position, track bar, messages), the
  power-up pickup, the "Riding Outside Boundary" darkening, the sun halo. No seams, no lines at
  sprite edges, no garbage at the screen edges. The power-up pickup's sparkle stays as chunky as
  at 1x: it is a small picture drawn at its own size, so it has no more detail to show.
- One ridge edge where snow and rock overlap at the same depth flickers between the two at 1x
  already (the game's own geometry); at higher scales the same thing shows as finer stripes.
- Match window at 1280 x 960 with interpolation (143 pictures/s) and widescreen: 3x, correct.
- Gate (exact arm against native arm, one frozen exe f300180a): `DHD_SCALE=1`, `2` and `4` all
  `NATIVE RENDERER GAME-STATE GATE MATCH` (EE at the four reference moments and all six
  moments, 1136 / 1133 / 1135 readback records, every Z read equal).
- Speed, `port/tools/bench_race.py`, fields 6000-7000 unpaced, focused window, idle PC, one run
  each (quick check, held BENCH.lock): 1x 71.99 fields/s (worst 1% 19.33 ms), 2x 76.44 (19.10),
  4x 66.69 (20.59). 2x is within the noise of 1x; 4x costs about 7%. The game thread (VU1) is
  still the limit in this heavy stretch at every scale (finding 043), so the GPU work of 4x
  shows only a little. Hence the default: 2x, with 3x, 4x and "window" as choices.

## Research

PCSX2 GS hardware renderer options, read for the ideas only (no code copied):
"Half-Pixel Offset", "Round Sprite", "Align Sprite" and "Native Scaling"
(pcsx2/GS/Renderers/HW/GSRendererHW.cpp and the upscaling fixes section of the PCSX2 GS
documentation). Our renderer needs none of their per-game tuning because its vertex mapping keeps
pixel centres and its texel fetch is integer, so each idea above is used only where it applies.
