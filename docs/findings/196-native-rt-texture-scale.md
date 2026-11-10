# 196: smooth bilinear sprites at high resolution scale

Status: current
Checked: 2026-11-10

Branch `fix/native-rt-texture-scale` from `main` f06ffba7, started 2026-11-10. Merged into `main` at
2426a716 on 2026-11-10; ships in 0.7.15.

## Goal and acceptance

At 8x the owner saw blocky grass and shrubs on Mt. Quintanaroo FR, blocky tree canopies on
Red Pass FR, and coarse dirt particles behind the bike. Other geometry stayed sharp, and
PCSX2's upscaled picture looked better. The owner confirmed the Windows test build fixes the
reported visual problem on 2026-11-10 and requested merging to main.

## Root cause and fix

The native renderer marked GS sprites with flag 1024 and applied two adjustments at scales
above 1x: snapping their corners to whole GS pixels in `native_vertex`, and snapping texture
sampling to a GS pixel in `fetch` and `fetch_u` for textures at their original resolution.
Even bilinear texture samples were consequently constant across an upscaled GS pixel. Small
foliage and particle sprites could not gain smooth interpolation from the higher resolution.

`port/gs/native.wgsl` now applies those adjustments only when the bilinear flag (32) is clear.
That flag comes from TEX1 MMAG in `native_texture` (`port/gs/native_core.inc`). Bilinear
sprites retain subpixel corners and interpolate UVs at output resolution. Nearest-filtered
sprites and copies retain the old snapping. Both float and integer texture-fetch paths use
the same condition. The exact renderer, game readbacks and gameplay code are unchanged.

The scope is all bilinear GS sprites at scales above 1x, on any track or screen using them;
there is no FR-track special case. Triangles, nearest-filtered sprites and scale 1 are
unchanged. An exhaustive inventory of every sprite effect and track was not completed.

## Progress

- [x] Reserved this lane and investigated foliage/dirt captures.
- [x] Implemented the shader fix in 74c38086 and a synthetic sprite-gradient regression check.
- [x] Built and packaged a Windows test build; owner confirmed the visual fix.
- [x] Incorporated main's existing track-table representation fix (1708297a).
- [x] Merge checks: python tools/check.py port -j 6 and native-transfer regression tests passed.
- [ ] State-trace comparisons and timed race benchmarks: not completed; the owner stopped
  further game testing and supplied manual visual acceptance instead.

## Decisions

- Keep snapping for nearest-filtered sprites. Removing it from every sprite initially broke
  synthetic copy/transfer cases; the bilinear-only change preserves their existing behavior.
- Do not redesign render-target texture conversion for this fix. Reinterpreted targets being
  converted at their texel size was an early hypothesis, not the cause addressed here.
- Remove the test-build packaging exception for the track-order table from `scan_retail.py`.
  Main already stores those ids as bytes (1708297a), avoiding the retail-data match without
  allowing another retail byte range. The final lane retains no scanner exception.

## Evidence and limits

`sprite_gradient` in `port/gs/test_native_transfers.c` draws a two-GS-pixel bilinear sprite
across a black-to-white texture at 2x. Four adjacent output samples must increase, with a
range greater than 100. The fixed shader produced 0, 31, 95, 159. During merge preparation,
`python port/tools/test_native_transfers.py` passed with and without dual-source blending:
38 pictures, zero pictures with RGB differences and zero CPU VRAM differences between runs.

Before/after captures were made at 2x and 8x during research; some independently timed
captures differed in game position and cannot serve as pixel-identical state evidence.
GS-dump replays did not provide a useful PCSX2 comparison. No claim is made that traces,
every track or performance were validated. The owner's subsequent visual confirmation is
the acceptance of the actual reported bug.

## Result

The native sprite fix is accepted by the owner for merging. Compile/unit check results are
recorded in the merge entry in `docs/STATE-LOG.md`. No additional game runs or benchmarks
were made during merge preparation.
