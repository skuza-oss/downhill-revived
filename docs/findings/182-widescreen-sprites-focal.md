# 182: widescreen sprite width table and menu focal projection

Status: current
Checked: 2026-11-08

Branch `fix/shared-widescreen-sprites` from `main` db3702d, started 2026-11-08.

## Goal and acceptance

Improve widescreen visual correctness across all platforms by:
1. Scaling all 6 billboard sprite width immediates in VU1 microcode packets (`ws_sprite_widths[]`),
   preventing horizontal distortion on tree sprites, particle flares and X-scale billboard elements.
2. Isolating menu camera focal length scaling (`dist == 688.0f`) via `port_ws_menu_focal` and
   `port_ws_set_focal` with an active rebuild guard, ensuring menu scenes preserve correct FOV without
   double-scaling across dynamic projection updates.
3. Providing `vif1_update_widescreen_sprites()` to update uploaded microcode on live aspect changes.

Acceptance:
- 16:9 and ultrawide render billboard sprites with correct 1:1 geometry instead of stretched aspect;
- Menu rider and background 3D models retain calibrated FOV matching the widescreen viewport;
- `python tools/check.py docs` passes with 0 errors;
- Leaves Windows state traces unchanged.

## Decisions

- **Structured table `ws_sprite_widths[]`:** Grouped the 6 EE address, VU offset and upper opcode words
  in `port/hle/widescreen_sprites.h` rather than scattered individual constants.
- **Rebuilding guard:** `port_ws_menu_focal()` checks `rebuilding_projection` before scaling 688.0f to
  prevent cumulative scaling during in-game aspect adjustments.

## Result

- `port/hle/widescreen_sprites.h`: declared `ws_sprite_widths[]`.
- `port/hle/widescreen.c`: implemented `port_ws_menu_focal()`, `port_ws_set_focal()`, and sprite width scaling.
- `port/hle/vif.c`: added `vif1_update_widescreen_sprites()`.
- `port/include/port.h`: declared `port_ws_menu_focal()` and `port_ws_set_focal()`.
- `port/patches.py`: routed `001F84A8.c` and `0023D8D8.c` through the focal helpers.
