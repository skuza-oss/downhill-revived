# 096: Sky mosaic from high jumps (issue #2), native renderer

Status: current
Checked: 2026-11-04

Public issue #2 (0.6.0, RTX 3070, scale 3 at 1080 lines, Smooth): "When jumping from high cliffs,
the sky textures become blurry", on the first career race (Italy freeride, track 0). The player's
pictures show the sky broken into rectangular blocks of different sizes and brightness. It happens
with the native renderer only; `DHD_RENDER=exact` is right at the same field.

## Reproduction

The x64 race route (`race_compare.py`'s NAV, `DHD_RACE_PAD=2:1200:cross`, seed 81257,
`DHD_FRAME_CLOCK=1`, hidden desktop, scale 3) reaches the cliffs around race clock 4:30-5:00.
Screenshots every 120 fields from 7500 showed the mosaic at fields **19740** and **21180-21540**
(scale 1 and 3 alike). Exact at the same fields has none.

## How it was found

1. GS capture of fields 21178-21181 (`DHD_GS_CAPTURE*`) replayed native and exact
   (`replay.exe` run directly, so `DHD_*` variables reach it): the blocks are in one layer drawn
   over the sky.
2. A per-draw log in the native front end and a kill bisect (skip draws by texture base) found
   the layer: sprites into the CT24 frame (FBP 112) textured from TBP **16256 / 16320**
   (pages 508 / 510), 64x64 PSMCT32 buffers.
3. The field trace (`DHD_FIELD_TRACE`) shows how those buffers are made each field
   (field 21179, in order):

   | Draw | Target | Texture | What |
   |---|---|---|---|
   | 278-279 | page 508, CT32 | PSMT8 9952 | the rider into the buffer |
   | 280-281 | 510, 508, CT32 | the other buffer, CT32 | offset copies (blur) |
   | 282-286 | frame 112 | 16256 | the rider shadow on the ground |
   | 287-288 | 510, CT32 | PSMT8H TBP 0 | the buffer redrawn through a CLUT |
   | **289** | **510, CT16** | **16320 as CT16, 8 pixels right** | **G moves into the 32-bit alpha** |
   | 290 | 510, CT32 | PSMT8H TBP 64 | RGB redrawn, alpha kept |
   | 291-294 | 508 | the same with 16256 | |
   | 296, 298 | frame 112 | 16256, 16320 | the layer over the sky |

   Draw 289/293 is the PS2 channel-shuffle trick: the 32-bit buffer is drawn onto itself through
   a PSMCT16 view of the same memory, shifted 8 pixels, which moves bits of G into the alpha
   byte. The layer over the sky then uses that alpha.
4. The native front end keeps one surface per layout (`wide`: CT16 surfaces have 64x64 pages,
   CT32 64x32). When a page last drawn by a surface of the **other** layout was needed,
   `native_import_page` imported it from the CPU copy of GS memory. Native draws never reach
   that copy, so the CT16 surface started from stale memory, and so did the CT32 surface after
   it. The alpha the sky layer blends with came from whatever was in pages 508-511 long ago:
   the blocks are 16-bit and 32-bit page pieces of old content.
5. A synthetic referee (no game data): `gs_fixture.make_dump` with a random 64x64 CT32 image,
   the same CT16 self-copy shifted 8 (TEXA TA1 0x80, nearest and bilinear), an RGB-only
   redraw (ATST NEVER, AFAIL RGB_ONLY), then the buffer's alpha shown as grey. Exact: alpha ==
   the expected G bits for every pixel. Native before the fix: 0.3% of pixels right (mean error
   129). Native after: 100%, both filters.

## The fix (`port/gs/native_core.inc`, `port/gs/native.wgsl`, `port/render`)

- `native_import_page`: if the previous owner is a colour surface of the same layout, copy as
  before; if it is a colour surface of the other layout, `native_import_converted` rebuilds the
  page on the GPU from that surface in this surface's layout and draws it in with a sprite
  (scissored to the page, colour mask RGBA, DECAL/TCC). Only when the field's conversion slots
  (`NATIVE_CONVERTS`) are used up, or the owner is depth, does it still import the CPU copy.
- `native_convert` (native.wgsl) learns 16-bit colour sources (slot flag 2): a 32-bit word is
  rebuilt from the source's two CT16 pixels (5:5:5:1 each, as the GS stores them). The inverse
  table gains a third part, the 64x64 PSMCT16 page (block and column swizzle as `memory.c`
  `pixel_address`), so it is 8192 words (`INVERSE_WORDS`); the binding size follows it.
- Rust test `inverse_table_covers_pages`: each part of the table is a full permutation of its
  page.

Only the native renderer changes. Exact mode and every readback the game observes (Z probe,
CSR/FINISH/SIGNAL, vblank) are untouched.

## Checks

Release exes from `package_release.py` (scale 3, hidden desktop, the race route above): main
7f7e8689e (`exe-main`; for the before shots, the branch base 3cd9b6504 on track 0, same renderer
code), fix e571afc04 (this branch with main merged). Mean absolute difference from exact over the
picture (top half, where the sky is, in brackets):

| Field | Before | After |
|---|---|---|
| Italy FR 19740 | 5.92 (6.86) | 3.60 (2.81) |
| Italy FR 21180 | 14.48 (21.97) | 9.85 (12.83) |
| Italy FR 21300 | 7.50 (8.50) | 6.54 (6.65) |
| Italy FR 21420 | 5.59 (6.26) | 5.20 (5.66) |
| Utah FR 8000 | 12.07 | 6.51 |
| Utah FR 11000 | 7.24 | 5.47 |
| Utah FR 5000, 6500, 9500 | 6.15, 5.63, 6.23 | 6.14, 5.63, 6.22 |

- Italy: the block mosaic is gone at every field; the haze over the sky is now shaped like
  exact's. Utah FR (selector row 3, `state_trace_run.track_nav(3)`, full pedal): the same buffers
  make the dust cloud of a crash or landing (8000, 11000), which native did not draw at all
  before; now it does. Fields without the effect are unchanged.
- Exact mode: screenshots of the fix exe at the four Italy fields are byte-identical to the base.
- State traces, main exe against fix exe: `state_trace_run.py race` (5000 fields, seed 81257) and
  `boot` (600): `trace_columns.py` PASS, all 13 columns identical. The mask is unchanged.
- Synthetic referee (above): native == exact bit for bit, nearest and bilinear.
- `cargo test` in `port/render`: `inverse_table_covers_pages`, `shifts_get_parentheses`,
  `native_wgsl_keeps_its_meaning` pass.

## Left

- The haze and dust are now in the right place and shape, but their inside is blotchier than
  exact's (darker streaks where exact is smooth; Italy 21180 top half still 12.8 from exact).
  The likely source is draws 287/290: the buffers redrawn through a CLUT from a PSMT8H texture at
  TBP 0 / 64, the top byte of pages 0 and 2, which the CT24 frame never writes (uploads in the
  alpha byte, as in finding 068). How native reads those bytes there is the next thing to check.
- CT16S and other 16-bit layouts: `native_import_converted` assumes PSMCT16 for wide surfaces
  (the only one this game was seen to use here).
- A page last drawn by a depth surface still imports the CPU copy, as before.
- The browser build was not rebuilt or checked here (native.wgsl is shared).
