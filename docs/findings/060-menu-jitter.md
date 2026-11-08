# Native progressive field jitter

Status: current
Checked: 2026-11-04

## Measurement (round 44)

Release 0.2.2's clean object basis (`c4e2c707`), with only the GS diagnostic added:
`build/port/r44jitter/before.exe`. Settings: `DHD_SETTINGS=none`, `DHD_RENDER=native`,
`DHD_SCALE=2`, `DHD_WIDESCREEN=16:9`, `DHD_ASPECT=wide`, sharp font (default), no texture
pack, movies skipped, memory card disabled, virtual field clock. `DHD_NAV=0:triangle|cross`
stops at main menu 15. The small 960x540 window avoids taking over the player's desktop;
screenshots capture the full 1280x896 native output, before window scaling.

`DHD_FIELD_TRACE=prefix DHD_FIELD_TRACE_FIELDS=598-613` records raw draw vertices and
registers at each vsync. `DHD_SCREENSHOT=prefix DHD_CAPTURE_FIELDS=600-611` captures twelve
consecutive fields. CSVs, executables, screenshots and analysis scripts stay under ignored
`build/port/r44jitter/`. No benchmark was run.

Every field has PMODE `0x8066`, SMODE2 `3` (INT + FFMD), DISPFB1/DISPLAY1 zero, DISPLAY2
`0x1BF9FF0183227C`. DISPLAY2 decodes to DX=636, DY=50, MAGH=3, MAGV=0, DW=2559,
DH=447: a 640x448 output from a 640x224 framebuffer. DISPFB2 alternates `0x9446`/`0x948C`
(FBP=70/140, FBW=10, PSM=1, DBX=DBY=0): only the double-buffer address changes.

| Field | XYOFFSET_1 and _2 at vsync | DISPFB2 | Logo raw Y | Logo draw OFY | Legend/text draw OFY |
| --- | --- | --- | --- | --- | --- |
| 600 | 0x790000006C00 | 0x9446 | 32294 | 0x7908 | 0x7900 |
| 601 | 0x790800006C00 | 0x948C | 32294 | 0x7900 | 0x7908 |
| 602 | 0x790000006C00 | 0x9446 | 32294 | 0x7908 | 0x7900 |
| 603 | 0x790800006C00 | 0x948C | 32294 | 0x7900 | 0x7908 |
| 604 | 0x790000006C00 | 0x9446 | 32294 | 0x7908 | 0x7900 |
| 605 | 0x790800006C00 | 0x948C | 32294 | 0x7900 | 0x7908 |
| 606 | 0x790000006C00 | 0x9446 | 32294 | 0x7908 | 0x7900 |
| 607 | 0x790800006C00 | 0x948C | 32294 | 0x7900 | 0x7908 |
| 608 | 0x790000006C00 | 0x9446 | 32294 | 0x7908 | 0x7900 |
| 609 | 0x790800006C00 | 0x948C | 32294 | 0x7900 | 0x7908 |
| 610 | 0x790000006C00 | 0x9446 | 32294 | 0x7908 | 0x7900 |
| 611 | 0x790800006C00 | 0x948C | 32294 | 0x7900 | 0x7908 |

The logo is actually three textured PATH1 triangle-strip quads (TEX0 values ending
`DD309F80`, `DD309FC0`, `DD30A000`), rather than GS sprite primitives. Its sampled vertex
stays at raw Y 32294. Legend lines are PATH3 sprites (TEX0 `0x2003AFC5DD30A040`), with
raw Y 34144/34176 and 34320/34352. Text is PATH3 sprites with TEX0 `0x200352A666420080`;
the first main-menu row starts at raw Y 32496, and legend glyphs at 34160. Their raw Y
does not contain the alternating offset. Draw-time offsets matter: PATH1 and PATH3 in the
same recorded field can have opposite phases. Correcting a whole scanout by field parity
would therefore be wrong.

Consecutive-image comparison searches integer vertical shifts -5..5, using RGB mean absolute
error (0..255). On fields 600 -> 601, logo region (480,50,560,280) has unshifted MAE 11.1945,
best +2 pixels with MAE 0.1764. Background region (10,160,380,570) has MAE 4.8030,
best +2 with MAE 0.0715. The next pair reverses to -2. **The 3D background moves too.**
The remaining small differences are the scene's animation. The legend contains mixed phases
and sprite snapping, so one global shift does not align all its parts. Some main-menu glyphs
already round to the same row; the reported elements do not all have identical motion.

## Cause and correction

`native_record` subtracts OFY from each vertex. An extra 8 in the GS's 12.4 fixed-point OFY
is half a framebuffer line, doubled at scanout and magnified by scale 2 to two output pixels.
The sprite shader's ceil can turn it into a whole framebuffer line (four output pixels).
`display.c` and `native_scanout` repeat lines but cannot undo a displacement already rasterized.
DISPLAY DY/DBY, widescreen X remapping, UVs, font sharpening and the presenter are not the source.

The native coordinate conversion now removes the half-line fractional OFY component before
sprite rounding, for INT+FFMD draws to screen-sized buffers (FBW >= 8). All primitive types,
including world triangles, use it. Small shadow/blur targets retain their coordinates. No
register, raw vertex, UV, GS VRAM, exact rasterizer, probe replay or EE readback is changed.
The correction applies at 1x too; at 1x some sprite edges hide the original displacement.

Initial after-main fields 600-611: best shift 0 for logo, background, legend and text.
On 600 -> 601, unshifted MAEs are 0.1794, 0.0725, **0.0000**, 0.0279 respectively.
The entire cropped legend is byte-identical across that pair. Full validation follows below.

## PCSX2 research

Read upstream revision `091ab68708a555198d80a7216965481c7ddd0d98` (2026-10-17):

- [GSState.cpp, GSPCRTCRegs::SetRects](https://github.com/PCSX2/pcsx2/blob/091ab68708a555198d80a7216965481c7ddd0d98/pcsx2/GS/GSState.cpp#L6819)
  halves the framebuffer read height when INT+FFMD is enabled and tracks display and framebuffer
  offsets separately.
- [GSRenderer.cpp, Merge](https://github.com/PCSX2/pcsx2/blob/091ab68708a555198d80a7216965481c7ddd0d98/pcsx2/GS/Renderers/Common/GSRenderer.cpp#L144)
  selects bob/blend/adaptive modes, adds an FFMD field-dependent merge offset unless
  `DisableInterlaceOffset` or deinterlacing Off is set, then invokes device deinterlacing.
- [GSRendererHW.cpp, GetOutput](https://github.com/PCSX2/pcsx2/blob/091ab68708a555198d80a7216965481c7ddd0d98/pcsx2/GS/Renderers/HW/GSRendererHW.cpp#L138)
  obtains the display target and handles framebuffer offsets. The current interlace-offset
  option is in the common renderer, not in this HW output method.

These distinguish PCRTC placement from draw coordinates; the port already omits the merge
offset, so merely implementing Disable Interlace Offset at scanout would not fix this bug.
The sources are GPL-3.0-or-later; no source code was copied or adapted.

## Consecutive-field validation

The before/after diagnostic builds share the clean 0.2.2 object basis and differ only in the
nine-line native correction. Main menu uses fields 600-611. OPTIONS uses fields 700-711,
with `DHD_NAV=0:triangle|cross,15:down,15:down,15:cross` (menu 2 reached at field 275).
Race captures use the standard `race_compare.NAV`, `DHD_RACE_SYNC=1`, seed 81257, no race
pad input, `DHD_RACE_CAPTURE=g+120`, `DHD_RACE_CAPTURE_WINDOW=5`: eleven consecutive fields
3335-3345 at GO+2 seconds, the same on both builds.

All screenshots use `DHD_SETTINGS=none`, scale 2 and 16:9 widescreen. Full RGB MAE means
over the eleven adjacent pairs in each menu and ten in the race:

| Scene / region | Before MAE | After MAE | Before best Y shifts | After best Y shifts |
| --- | ---: | ---: | --- | --- |
| Main / logo | 11.1501 | 0.1796 | +2,-2 repeating | all 0 |
| Main / legend | 8.0233 | 0.0000 | +2,-2 repeating | all 0 |
| Main / text | 3.2750 | 0.0275 | all 0 (background still moves) | all 0 |
| Main / 3D background | 4.8160 | 0.0732 | +2,-2 repeating | all 0 |
| OPTIONS / logo | 11.1223 | 0.1332 | +2,-2 repeating | all 0 |
| OPTIONS / legend | 8.0233 | 0.0000 | +2,-2 repeating | all 0 |
| OPTIONS / item text | 14.8101 | 0.0442 | +2,-2 repeating | all 0 |
| OPTIONS / 3D background | 4.8829 | 0.0817 | +2,-2 repeating | all 0 |
| Race / world | 1.4165 | 0.1528 | -2,+2 repeating | all 0 |
| Race / speedometer | 12.3757 | 0.0674 | -2,+2 repeating | all 0 |
| Race / place | 6.2927 | 0.3296 | -2,+2 repeating | all 0 |

Region rectangles (x,y,width,height), in raw output pixels: main text (580,390,400,300),
OPTIONS item text (600,360,400,410); both menus' logo/background as above and legend
(190,795,875,55). Race world (120,80,650,380), speedometer (855,704,150,160), place
(12,740,80,125). Screenshots are 1280x896 in menus and 1024x896 in the race.

The legend has **zero changed pixels in every after-menu pair**. The OPTIONS text's neutral
bright-pixel mask (min RGB > 80, max RGB - min RGB < 10) is also identical for 700 -> 701,
where the before mask differs by 10,768 pixels unaligned. This separates letter stability
from the remaining legitimate background animation.

Same-moment race comparison: speedometer and place crops are byte-identical before/after
for all six even deltas. On the five odd deltas, aligning by +2 output pixels leaves place
MAE 0..0.0004 and speedometer MAE 0.00036..0.00044. World crops have same-moment MAE
0.0127..0.0475 on the unchanged phase, 0.2502..0.2539 after aligning the corrected phase.
There is no persistent HUD/world displacement or added blur: the correction picks the original
integer OFY phase, and adds no filter, UV rounding, history or resampling operation. It also
removes the race's original alternating displacement, rather than limiting the fix to menus.

Local evidence, all under `build/port/r44jitter/`:

- `before-main/shot-f000600.bmp` through `shot-f000611.bmp`, and `after-main/` equivalents;
- `before-options/shot-f000700.bmp` through `shot-f000711.bmp`, and `after-options/` equivalents;
- `before-race/shot-g+120+0.bmp` through `shot-g+120+10.bmp`, and `after-race/` equivalents;
- each folder's `field-a.png`, `field-b.png` (lossless versions of the first two fields), and
  `adjacent-diff.png` (absolute RGB difference, no gain);
- `before-main/trace-registers.csv`, `trace-draws.csv`, OPTIONS equivalents; the after-main
  raw trace still shows the game's unchanged alternating registers and raw vertices;
- `analyze.py` and `image-metrics.json`: every region, pair, shift, MAE and changed-pixel count;
- `build_probe.py`, `run_shots.py`, and each folder's `run.log`: exact build/run commands.

## Clean build and game-state gate

`python port/tools/build_clean.py --tree build/port/clean/tree-r44jitter -j 4` on correction
commit `00c532326696a644556ca0e75d95584c3129cbc6`: all 3,797 units compile, zero errors.
It uses a fresh checkout. No recomp basis is used.
Executable: `build/port/clean/00c53232/dhd.exe`, SHA-256
`86de860ac429944a3a38c97b866b0eb959569ee4c83420119691f9788905da78`.

With DHD_ISO pointing at the player's local disc image:

```text
python port/tools/gate_native_renderer.py --exe build/port/clean/00c53232/dhd.exe --out build/port/r44jitter/gate --env DHD_SCALE=2
```

**NATIVE RENDERER GAME-STATE GATE MATCH**, result.json `PASS`, no failures:

- both arms match the unchanged default `build/port/compare/r40-plain` at GO+0/+2/+12/+16;
- exact and native EE dumps match at all six moments, through GO+4500;
- **READBACK MATCH: 1,168 records, 1,168 Z reads, through virtual GS field 9051**;
- native presentation was exercised; both final captures are kernel field 7715.

The gate runs without widescreen, as required by the canonical EE reference: widescreen itself
deliberately patches game memory. The separate visual checks above exercise 16:9 at 2x.
No gate code, exclusions or reference was changed. The twelve before/after main-menu register
snapshots also match value for value, including the original alternating OFY and FIELD.

`DHD_RENDER=exact` has no changes; the deliberate progressive correction is only in native
vertex recording. No src/ or proprietary artifacts were staged.
All launched game processes were stopped by their specific PID (including their relaunch
children). No fps benchmarks, push, release, deployment or publication were performed.
