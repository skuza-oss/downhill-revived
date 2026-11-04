# 058: the game's font drawn sharp at high resolutions

Status: current
Checked: 2026-11-04

Round 42. Code: `port/gs/font_sharp.c`, its hooks in the native front end (then
`port/gs/gpu_native.inc`, now `native_core.inc` and `gpu_rust_shared.inc`: `native_decode`,
`native_replace`, `native_texture`), dhd.ini `font` (then `port/hle/host.c`, now `dhd_settings`),
PC SETTINGS > Font (`port/hle/pc_settings.c`).

## The problem
At 2x-4x the menus, popups, pause menu and race HUD text were hard to read. Letters lost strokes:
"RESOLUTION" read "Rcsolution", "SIZE" read "9izc". Two things cause it:
- **The text is drawn at 1x quality.** The native renderer samples an upscaled sprite where the
  GS samples its pixel (PCSX2's "round sprite", raster.hlsl), so HUD and text pieces fit without
  seams. Each glyph is a sprite, so it shows the 1x picture made bigger: blocky, and a thin
  stroke that falls between two 1x samples is lost.
- **The menus' font is rebuilt on the GPU every time.** It is a 512x512 4-bit texture stored in
  the top bytes of a depth buffer's memory (PSMT4HL), so `native_texture` sends it through the
  GPU conversion (`native_convert_texture`). That path has no texture pack replacement.

## The fonts
The text engine (finding 050) puts the font's TEX0 into its text packets, one command per
view: the pointers at `D_004F35B0[0..3]` (func_00210068), or a scene's font (func_00210250).
- **Mode 0, the executable's font.** 128x128 PSMT4 at 0x263C00, palette at 0x263BC0, 16x16 cells,
  glyphs about 11 pixels high. White fill inside a dark blue outline, soft edges.
- **Mode 1, the menus' and race's font.** A 512x512 PSMT4HL atlas from the scene, 32x32 cells: upper
  case, small capitals, digits, accented letters and button icons. The palette is black with alpha
  0, 0x15, 0x2B and 0x40 (solid). The colour comes from the vertices.
  - Seen at TEX0 `20034c2666420000` in the front end and `200352a666420080` later.
  - It lives in pages a depth buffer owns.
  - The CPU copy of GS memory holds it exactly as uploaded.

## What is done (approach A: the player's own font, made sharp in memory)
1. **Detection.** When `native_decode` decodes a paletted texture, `font_sharp_match` compares
   its TEX0 with the four text TEX0 commands, using TBP, TBW, PSM, TW, TH, CBP, CPSM and CSM. This
   reads EE memory and writes none.
2. **The large copy (`font_sharp_view`).** It is made from the decoded texture: the colours as
   the game's palette gives them.
   - **Magnification.** Each channel is magnified with a cubic B-spline, x8 for the 128x128
     font and x2 for the 512x512 atlas (at most 1024 a side).
   - **Edges.** The coverage (alpha / the texture's solid alpha) passes a smoothstep 1.5 large
     texels wide around 0.5. For a two-tone font (mode 0), so does the fill: the position
     between the outline colour and the fill colour, cut at 0.55 so strokes stay as thin as the
     original's. This is how a signed distance field is drawn (Green 2007, below). An
     interpolated coverage field is used instead of an exact distance transform: with 2-4
     levels of edge antialiasing in the source it gives the same edges.
   - **Mipmaps.** The copy gets a box-filtered mip chain.
   - **Cache.** The last 4 copies are kept by texture key, so decoding the same font again is
     free.
3. **Drawing.** `native_replace` draws the copy as it draws a texture pack's replacement: flag
   2048, normalised texture space, the replacement samplers, mipmaps, anisotropic filtering when
   the GS filters.
   - A pack entry for the same texture wins.
   - For the menus' font in depth-buffer pages, `native_texture` takes the CPU copy and its
     sharp version before the GPU conversion. If there is no sharp copy, the conversion runs as
     before.
4. **Settings.**
   - dhd.ini `font = sharp|original` (`DHD_FONT`). The default is sharp, also with no dhd.ini
     (the tools).
   - PC SETTINGS > Font: Original / Sharp, applied at the next start.
   - The dhd.ini template and README.txt mention it.

Nothing derived from the font is written to disk or shipped: the copy exists only in graphics
memory while the game runs. The executable contains only code. scan_retail.py: 14 runs, all
from the decompiled source. package_release.py passes.

## Results (29f9d938, quick checks, no benchmarks)
Before/after crops at 2x and 4x, widescreen 16:9: `build/port/r41font/shots/`
- `main_menu_{2,4}x.png`
- `quit_popup_{2,4}x.png`
- `pc_settings_{2,4}x.png`
- `pause_menu_{2,4}x.png`
- `race_hud_{2,4}x.png` ("PUNCH")

All of that text is now clean and readable: "RESOLUTION", "WINDOW SIZE", "QUIT DOWNHILL
REVIVED?", "PLAYER 1 PAUSED", the track name in the flyby. The speedometer digits and the place
counter are HUD textures, not the font, and are unchanged.
- `font = original`: menu screenshots byte-identical to c6b2f7ca's, apart from the new Font row
  on PC SETTINGS.
- The race reaches the same frames with either setting: the screenshots at the same game times
  show the same scene.
- The mode-0 font's two-tone path was tuned offline on that font (a throwaway script, nothing
  kept). In this run the scripted menus and race draw with the mode-1 atlas.

## Gate
`gate_native_renderer.py` on the clean build of 29f9d938 (dhd.exe 95dbcb03...) against the
default reference r40-plain: **NATIVE RENDERER GAME-STATE GATE MATCH**.
- Every EE dump matches, through g+4500.
- READBACK MATCH: 1166 records, 1166 Z reads, through field 9049.

The gate's runs have no dhd.ini (`DHD_SETTINGS=none`) and no DHD_FONT, so the native arm ran
with the sharp font, the default. The change is renderer-only: it reads the text TEX0 commands
from EE memory and writes no game state. The readbacks are Z reads, which text never touches.

## Considered and not used
- **xBRZ** (GPL-3, compatible). It is C++ with the standard library, and the port's host code is
  C linked with clang for i686 Windows, with no C++ runtime for host files. Its pixel-art rules
  also suit unfiltered sprites more than a font that is already antialiased, which is what the
  interpolate-then-cut approach needs.
- **An OFL font (approach B).** Not needed: approach A looks right and keeps the game's own
  letters.
- **Making the Font row apply at once.** A decoded texture keeps its sharp copy (or none) for its
  life in the cache. Switching live would mean replacing views the render thread may still be
  drawing with, so the row applies at the next start, like Texture Pack.

## Sources
- **Edge cutting.** Chris Green, "Improved Alpha-Tested Magnification for Vector Textures and
  Special Effects", SIGGRAPH 2007 (Valve): a magnified distance or coverage field cut with a
  smoothstep about one pixel wide gives sharp edges at any magnification.
- **Cubic filters.** Mitchell & Netravali, "Reconstruction Filters in Computer Graphics",
  SIGGRAPH 1988. B = 1, C = 0 is the cubic B-spline, chosen over Catmull-Rom and Mitchell
  because it does not ring, so contours come out smooth.
- **Sampling.** PCSX2's "round sprite" upscaling fix, for why sprites are sampled at 1x positions
  (raster.hlsl already cites it).
- **xBRZ.** sourceforge.net/projects/xbrz, read for the licence and API; no code used.

No third-party code is in font_sharp.c, so THIRD-PARTY-NOTICES.txt does not change.
