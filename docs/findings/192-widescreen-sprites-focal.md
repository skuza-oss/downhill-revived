# 192: widescreen sprite width table and menu focal projection

Status: in progress
Checked: 2026-11-09

Contributed by ribeirogustav in pull request #75 (commit 7e537cce, numbered 182 on their branch,
which main had already given to finding 182); finished on branch `fix/pr75-widescreen-sprites`
from `main` 34cf1293, started 2026-11-09. The branch merges their commit unchanged (`--no-ff`, so
GitHub marks #75 merged) and fixes the review's points on top.

## Goal and acceptance

Improve widescreen visual correctness across all platforms by:
1. Scaling all 6 billboard sprite width immediates in VU1 microcode packets,
   preventing horizontal distortion on tree sprites, particle flares and X-scale billboard elements.
2. Isolating menu camera focal length scaling (`dist == 688.0f`) via `port_ws_menu_focal` and
   `port_ws_set_focal` with a guard, so a focal length the bike shop has already scaled is not
   scaled again by the setter.

Acceptance:
- 16:9 renders billboard sprites in the same proportion as the scene around them instead of
  4/3 wider; before/after captures of the same scene (a race with trees and particles, the title
  menu) show it;
- `python tools/check.py port` passes;
- boot and race state traces equal to main's (widescreen is off in traces, so nothing changes).

## Progress

- [x] 2026-11-08: the contributor's commit 7e537cce (sprite width table, menu focal guard,
  `vif1_update_widescreen_sprites`).
- [x] 2026-11-09: merged onto main 34cf1293; finding renumbered 182 -> 192.
- [ ] the view table's real size and every check against it
- [ ] the sprite width loop idempotent (original immediates recorded)
- [ ] `vif1_update_widescreen_sprites` removed
- [ ] before/after captures at 16:9, race and title
- [ ] `check.py port`, boot and race state traces against main

## Decisions

- **Structured sprite table:** the six EE addresses of the VU1 lower words (the `LOI` immediates),
  with the upper instruction word each pairs with, rather than scattered constants (contributor).
- **Focal guard:** `port_ws_menu_focal()` scales the title and shell menus' 688 in the projection
  setter unless the call comes through `port_ws_set_focal()`, the bike shop's, whose focal length
  is already times s (contributor; kept, see Surprises).

## Surprises

## Result
