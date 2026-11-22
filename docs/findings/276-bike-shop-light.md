# 276: the bike shop's spotlight on the native renderer

Status: in progress

Branch `fix/bike-shop-light` from `main` 05cccc24, started 2026-11-22. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

A player reported that on the BIKE / JERSEY page of the career's bike shop the blue banner behind
the hanging bike is brightly lit in the original (a spotlight with a soft edge, the bike bright)
and dark in the port. Done when the native renderer draws the light as the original does, with no
special case for the screen, and nothing else changes:

- the shop's list state (cursor on BIKES, the GT entry bike) shows the light with Native VU on and
  off, as retail PCSX2 does;
- `state_trace_run.py boot` and `race` equal the base build's (`state_diff.py` PASS);
- the rider select screen and a race start, captured at the same fields, differ from the base only
  where the same blend is drawn;
- `check.py quick`, `check.py port`, `check.py docs` pass.

Out of scope: the other blends `native_blend` still approximates (below, "Surprises").

## Progress

- [x] 2026-11-22: reached the screen hidden (career, ride to the shop, close the popup, Down to
  BIKES: DHD_NAV `...,17:down,17:cross,18+200:cross,18:down,18+99999:cross`); the light shows
  only in the list state (`func_0013C128` runs animation clip 0x25C6 forward when the cursor enters
  the BIKES list, backward on the INVENTORY header).
- [x] 2026-11-22: retail on the PCSX2 compare adapter (software renderer) with the same route and
  bike: the light is there. Port native renderer, Native VU on and off: dark, every bike. Port
  Exact renderer: the light is there.
- [x] 2026-11-22: a GS dump of the native run (field 1600) replayed in PCSX2 and in our Exact
  replay shows the light: the game draws it, the native front end drops it.
- [x] 2026-11-22: fix in the native front end (this commit).

## Decisions

- The light is one textured sprite over the banner: TEX0 an 8-bit 128x128 texture whose CLUT is an
  alpha ramp (0..127, colour 8FD8DE), modulated by a vertex colour of (128,128,128,115), Z test
  GEQUAL, ALPHA = `(Cd - 0) * As + Cd` (A=1 B=2 C=0 D=1), that is Cd * (1 + As): the banner's own
  colour brightened by up to 2x where the ramp's alpha is high. No light object or lighting code is
  involved (`func_00217788`, the light gatherer, is never called on this page).
- `native_blend` maps a side whose factor is `1 + C` to ONE (counted in `blend_approximate`), so
  Cd * (1 + As) became Cd: the sprite had no effect. D3D/WebGPU have no `1 + As` factor, so the
  front end now writes As (as 0..255, 128 = 1.0) as the shader's colour and blends it with
  `src * Dst + dst * One` = Cd * As + Cd. A new constants flag (16384, `NATIVE_AS_COLOUR`,
  specialised as bit 21 of `native_spec`) tells `native.wgsl` to output As as the colour; the
  pipeline gets D3D11 factor 9 (DEST_COLOR) as wgpu's `Dst`. The same `native_state` serves GS
  draws and Native VU mesh draws, so both are covered.
- As above 128 is cut to 1.0 (an 8-bit target can't hold more); this sprite's As is at most
  115 * 127 / 128.
- `port/render/tests/native_keys.rs` checks the pipeline keys against the retired C tables' JSON,
  which never used factor 9 and recorded it as One; the test now expects `Dst` for it (one
  documented exception, the JSON is left as it was recorded).

## Surprises

- The light is a blend trick, not a light: the original's "spotlight" is the banner multiplied by
  up to 2 through an alpha ramp, which the GS does in one draw.
- A temporary counter over a shop visit and a race (native, Native VU off) found no other blend
  that `native_blend` approximates (`blend_approximate` stayed 0); the Cd * (1 + As) blend itself
  is drawn about once a field in the race too and on the shop's header state, so those pictures
  change where it is drawn.

## Result

Filled in at the merge.
