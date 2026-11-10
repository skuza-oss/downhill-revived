# 196: foliage sprites pixelated at high resolution scale (native render-target textures)

Status: in progress

Branch `fix/native-rt-texture-scale` from `main` f06ffba7, started 2026-11-10. This file is the lane's plan
while it runs and its finding when it merges. Someone who has only this file and the git log must
be able to pick the lane up.

## Goal and acceptance

Owner report (2026-11-10, 0.7.14 and current main): on Mt. Quintanaroo FR ("Peru FR") at 8x
resolution the grass/shrub sprites beside the track look very pixelated (blocks about 20 px at
1080p, about 6-7 PS2 pixels) until the rider is very close, while everything else is sharp. The
dirt particles behind the bike also look low quality. PCSX2 at 8x upscaling looks clearly better
on the same spots. The same look is on Red Pass FR tree canopies (trunk sharp, canopy blocky,
about 3:29 race time), so it is likely every FR track's foliage sprites, not Peru only.

Done means:

- the root cause is named with evidence: which draws (GS dump), which TEX0/TEX1/CLAMP, which
  texture source (uploaded texture vs render-target conversion in
  `port/gs/native_core.inc` `native_texture()` / `native_convert_texture()`), which filtering/LOD;
- every other place the same mechanism applies is listed (other tracks, effects, menus);
- if the fix is clear and contained it is implemented on this branch: `python tools/check.py port`
  passes, boot and race state traces equal to main's (picture-only change), before/after
  presented pictures at 8x (and 2x) for Peru FR grass and Red Pass FR canopies under
  `F:\Temp\peru-grass\` (never committed), and no new GPU performance cliff on this PC
  (timed race segment before/after).

Out of scope: changing gameplay, the exact renderer, texture packs, Android/browser.

## Progress

- [x] 2026-11-10: branch created from main f06ffba7; worktree setup run; this finding reserved.
- [ ] reproduce on Peru FR (--right 9) and Red Pass FR (--right 3) at scale 8 with
  DHD_NATIVE_TEXLOG / DHD_NATIVE_WHY, presented captures at the grass (about 1:40 race time,
  full-pedal pad) and the Red Pass canopy (about 3:29).
- [ ] GS dump at the grass moment; find the foliage and dirt draws; compare with PCSX2 replay
  at 8x if practical (finding 085 referee flow).
- [ ] root cause, blast radius, fix or proposed fix, checks.

## Decisions

- (recorded as made)

## Surprises

- (recorded as found)

## Result

Filled in at the merge: what changed, what was measured and how, what is left. Then set the
Status to `current` (or `rejected`).

## Test build handoff (2026-11-10)

The implemented fix is commit 74c38086: native bilinear sprites retain subpixel corners and
smooth texture-coordinate interpolation when upscaled. Nearest-filtered sprites and copies
retain their existing snapping. The original render-target conversion hypothesis was not the
implemented fix. The earlier native-transfer run reported smooth sprite interpolation passing
on both backends after retaining snapping for nearest-filtered copies.

At the owner's request, further game runs and testing stopped; a Windows player ZIP is being
packaged for manual testing. Full port checks, state-trace comparisons and performance
acceptance remain incomplete; this is a test build, not a validated release.

Packaging exposed an existing explicit source table: online_menu.c tracks has the career-order
ids also found at retail address 0x26F150. The exact 99-byte match (including three leading NULs)
is recorded in scan_retail.py SOURCE, alongside the existing source constants. No retail bytes
were added to the build, and the scan still checks every packaged file.
