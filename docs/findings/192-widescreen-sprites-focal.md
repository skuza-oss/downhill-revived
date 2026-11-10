# 192: widescreen sprite width table and menu focal projection

Status: in progress
Checked: 2026-11-10

Contributed by ribeirogustav in pull request #75 (commit 7e537cce, numbered 182 on their branch,
which main had already given to finding 182); finished on branch `fix/pr75-widescreen-sprites`
from `main` 34cf1293, started 2026-11-09. The branch merges their commit unchanged (`--no-ff`, so
GitHub marks #75 merged) and fixes the review's points on top.

## Goal and acceptance

Improve widescreen visual correctness across all platforms by:
1. Scaling all 6 billboard sprite width immediates in VU1 microcode packets,
   preventing horizontal distortion on tree sprites, trackside markers and X-scale billboard
   elements.
2. Isolating menu camera focal length scaling (`dist == 688.0f`) via `port_ws_menu_focal` and
   `port_ws_set_focal` with a guard, so a focal length the bike shop has already scaled is not
   scaled again by the setter.

Acceptance:
- 16:9 renders billboard sprites in the same proportion as the scene around them instead of
  1/s wider; before/after captures of the same fields (races, the main menu) show it, against a
  4:3 capture of the same field;
- `python tools/check.py port` passes;
- boot and race state traces equal to main's (widescreen is off in traces, so nothing changes).

## Progress

- [x] 2026-11-08: the contributor's commit 7e537cce (sprite width table, menu focal guard,
  `vif1_update_widescreen_sprites`).
- [x] 2026-11-09: merged onto main 34cf1293 (54b24b05); finding renumbered 182 -> 192 (059df9a2).
- [x] the view table's real size and every check against it (49ff9e3f)
- [x] the sprite width loop idempotent, retail immediates recorded (281921bf)
- [x] `vif1_update_widescreen_sprites` removed (9fc3af67)
- [x] the menu focal guard checked and kept, its flag renamed (364aa975)
- [x] the VU1 blocks read LOI immediates from their words, so the widths take effect (fa26f3c4)
- [x] before/after captures at 16:9 and 21:9, races and the title and main menu
- [x] 2026-11-10: `check.py port` OK; boot (600 fields) and race (5000) state traces equal to main's

## Decisions

- **Six views.** `func_0023D6C8` clears views 0-5 at boot (`slti 6`, 20-byte stride), so
  `D_0075A9D0` has six entries, as the PR declared. The 2D, HUD and touch helpers still checked
  `view >= 32`; they now go through `ws_view()`, which checks `WS_VIEW_COUNT`.
- **Structured sprite table:** the six EE addresses of the VU1 lower words (the `LOI` immediates)
  with the upper word each pairs with (contributor), now also the retail immediate (0x438A8000,
  277.0). A word is written only while both words still hold their retail values, from the retail
  value, as the view table's words are: a second run changes nothing, and a word that differs is
  logged and left.
- **`vif1_update_widescreen_sprites` removed.** It had no caller, read the EE copy through an
  unfinished address, and worked out a "previous" width that was never passed. The aspect is fixed
  at boot (`host_widescreen_init`), and VIF1 uploads the package from the EE copy, so the
  boot-time change reaches micro memory (checked: a `DHD_VU_SEQ` dump in a race holds the changed
  words at VU 0x27C8, 0x2B50 and 0x2C60).
- **Focal guard kept.** The bike shop (`func_001F84A8`) zooms from 688 to 3000 and passes the focal
  length already times s; the setter scales the title and shell menus' 688. At any aspect one zoom
  step's scaled value can be exactly 688 and would be scaled twice. `port_ws_set_focal` marks its
  call so the setter leaves it (contributor's design; the flag is `focal_scaled` now).
- **The VU1 blocks read LOI immediates from their word arrays** (`port/tools/vu1_aot.py`), not
  from constants. See Surprises: without it none of the widths, main's three included, did
  anything on the 64-bit build. The arrays hold what micro memory holds whenever a block runs
  (`aot_find` compares them); with widescreen off they hold the retail words, so the result is
  the same as before. The blocks still match in widescreen, so no speed is lost.
- **No new unit test for the AOT change.** The test programs embed the retail words as constants
  (`PS2_VU_AOT_EMBED`), so they cannot hold a changed immediate; the proof is the capture
  comparison below and `DHD_VU_INTERPRETER=1`, which gives the same change.

## What the six immediates are

Overlay B (the package at 0x269C50, finding 019) has four sprite programs in its MSCALF table,
each scaling a sprite's world size by 277.0 (the 1P race's focal length at 4:3) and 1/w for a half
width in screen pixels, in X only:

| EE word | VU | Program | Use |
|---|---|---|---|
| 0x26C448 | 0x27C8 | 0x2738 | vf7.w, the X half width (new in #75) |
| 0x26C5C0 | 0x2938 | 0x28D0 | the same; PCSX2 patch "Tree Sprite Width #1" |
| 0x26C700 | 0x2A78 | 0x28D0 | PCSX2 patch #2 |
| 0x26C758 | 0x2AD0 | 0x28D0 | PCSX2 patch #3 |
| 0x26C7D8 | 0x2B50 | 0x2B10 | vf8.x, the X half extent; vf8.y is LOI 138.0, left (new in #75) |
| 0x26C8E8 | 0x2C60 | 0x2C20 | the same (new in #75) |

The PCSX2 widescreen patch for the disc (SCUS-97177_5AE01D98.pnach, Aced14; finding 043) changes
only the three tree words. The other three are the same pattern, so Hor+ needs them too. What they
draw (track 5, `DHD_RENDER=exact`, `DHD_DRAW_SKIP_PROGRAM` per program: each removes pixels):
billboard trees (the main menu's mountain, 28D0), banner poles and small trackside markers and
flags (2B10, 2C20, 2738). They are not overlay A's particles. Calls per field in a race
(`DHD_VU1_PROFILE`): 2B10 and 2C20 on every unlocked track, 28D0 on tracks 0, 4 and 5, 2738 on 4
and 5.

## Surprises

- **None of the VU1 width patches did anything on the 64-bit build, main's included.** First
  captures (deterministic: the state-trace race recipe, `DHD_CAPTURE_FIELDS`) of main and the PR
  were pixel-identical at 16:9 and at 4:1, and so were all six widths times 4 on all six unlocked
  tracks, in both renderers, although the changed words were in micro memory. With
  `DHD_VU_INTERPRETER=1` the times-4 run changed five of six frames. Cause: the translated blocks
  wrote each LOI immediate in as a constant, and on a 64-bit host `ps2_vu_aot_load` fills their
  words from guest RAM at the first VU1 program, after `host_widescreen_init` has changed them, so
  the blocks matched the changed micro memory and ran with 277.0. The 32-bit build loaded the words
  from the image before the patch (blocks did not match, the interpreter ran), so the tree widths
  worked there; since dc85d04a (2026-10-26, the 64-bit build) they were ignored. Fixed in fa26f3c4.
- `DHD_DRAW_SKIP_PROGRAM` works only in the exact renderer; with `DHD_RENDER=native` it skips
  nothing.

## Result

Captures (game-derived, not in the repository; kept in the scratch folder of the machine that ran
this round): the hidden desktop, scale 2, `DHD_SCREENSHOT_WINDOW` (1280x720 at 16:9, 1680x720 at
21:9, 960x720 for the 4:3 reference), the state-trace recipe, so before (main 34cf1293) and after
(this branch) are the same field.

- Races: the markers, flags and banner poles are narrower after and match the 4:3 picture's shape
  (its scene is the middle 960 pixels of the wide one at the same scale). The green marker in track
  5 at field 4400: 12 px at 4:3; 18 px (main) and 10 px (branch) at 16:9; 20 px and 13 px at 21:9
  (a sprite this small rounds by a pixel or two at its edges).
- The main menu: the mountain's billboard trees were wider than at 4:3 on main and match it after.
- The title: no sprite program runs; identical before and after.

Left: none of the widths changes game state, but they are EE words, so a state trace with
widescreen on would differ from main's in those words (traces run with it off).
