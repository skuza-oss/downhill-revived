# 154: The rider missing from the inventory screen on phones

Status: current

Branch `fix/menu-rider` from `main` 738ad0b, started 2026-11-05. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

On Android (the Mi 11i, Adreno 660, and players on other phones, it seems all of them), the
career's INVENTORY screen (Bikes / Jerseys / Bike Color; the "Bike / Jersey" board, BANK at the
bottom) shows the bike but not the rider standing beside it: of the first (default) character only
the goggles are visible, a small purple patch left of the bike. The owner's screenshot from the
Mi 11i is kept by the lead (locally, not in the repository). Whether a PC or Mac shows the same
is not known yet.

Goal: the rider is drawn on that screen on phones, as on the PS2 (PCSX2 is the referee: replay the
same GS stream before blaming the renderer, finding 025), with one code path for every platform
and no change to game state (unless the cause is a game-state divergence, in which case the
divergence is the bug).

Acceptance:
- Where the bug lives, with evidence: the core (EE/VU results differ on arm64, e.g. floating point
  contraction), the native renderer (state the phone path takes), the exact renderer, a setting
  the phone uses by default (RENDER=native, SCALE=2, WIDESCREEN=1, TEXTURES=on, ...), or a
  phone-only code path (findings 146-149).
- The fix, and a check that shows the rider on that screen where it was missing (the lead
  confirms on the Mi 11i).
- `python tools/check.py port` passes; boot and race state traces equal to the base build's when
  the change can touch game state.

## Progress

- [x] 2026-11-05: finding reserved
- [x] Windows (02fb762's code, `live_game.py` on a hidden desktop, the race route of
  `state_trace_run.py` up to the shop: `0:triangle|cross,15:cross,3:cross,16:cross,1:cross,11:cross`):
  the rider stands beside the bike with RENDER=native SCALE=2 WIDESCREEN=1 TEXTURES=on FONT=sharp
  and with RENDER=exact. Not a setting, not the renderer on x86-64.
- [x] The Mi 11i (0.7.10 as installed, Single Event > Race, the same shop screen): rider gone, goggles
  and a dark wedge, with dhd.ini's default renderer (native) and with `renderer = exact`. The
  carousel and START NEW GAME screens draw the same rider correctly. So the bug is upstream of both
  renderers: the core, VU1, VIF or the GS front end.
- [x] A diagnostic APK (a local patch, not committed: `DHD_*` lines of `files/dhd-test.env` become
  the environment) on the phone: still broken with `DHD_VU1_NATIVE=0` and with `DHD_VU_INTERPRETER=1`,
  so not the native VU1 kernels nor the translated blocks.
- [x] The state trace over the shop route (boot recipe, `DHD_NAV` above, 1400 fields) on the phone
  and on Windows: all 1400 rows equal (EE RAM, CPU, VU memory, every GS packet, GS registers and
  VRAM), once the Windows run hides ONLINE as Android does (a local switch). With the recipe's
  default settings (no widescreen) the phone draws the rider. arm64 computes what x86-64 does.
- [x] Windows with WIDESCREEN=1 in a 20:9 window (1200x540, the phone's shape): the same missing
  rider and dark wedge. In a 16:9 window (`widescreen: aspect 1.7778 (s 0.7500)`) the rider is
  there. The bug is widescreen at shapes wider than about 16:9, not arm64; phones meet it because
  their screens are 20:9 and widescreen is on by default.
- [x] The cause: the bike shop's live widescreen word 0xA60038 (`host_widescreen_vsync`), which
  PCSX2's 16:9 patch calls "Bike Shop Shadow Shape/Width" and sets to -111.0 (retail -95.886). It
  is not a width: 0xA60030 holds a VIF `UNPACK V3-32` of 8 vectors (0x680840B5) and 0xA60038 is the
  first vertex's second coordinate, the far corner of the dark panel (the shade behind the rider).
  Our line through (4:3, -95.886) and (16:9, -111) in 1/s gives -126.1 at 20:9. Windows at 20:9
  with the word forced (a local switch): -95.886, -111 and -116 draw the rider; -120 hides most of
  him; -123 and -126.1 are the phone's picture.
- [x] The corner's place, from the GS stream of deterministic 20:9 runs that differ only in that
  word (state-trace dumps at field 1300; one vertex differs): x, y from the picture's centre in GS
  pixels -230.6, -113.8 at -95.886; -851.8, -511.4 at -111; at -116 X needs more than 16 bits and
  Z flips sign; at -120 and -126.1 the vertex is mirrored to the right (+910, +350) with a negative
  Z. The vertex crosses the camera's plane between -111 and -116, so the strip's first triangles
  are drawn mirrored across the screen (the dark wedge) and in front of the rider.
- [x] The fix, 4209d6d (`port/hle/widescreen.c`, `host_widescreen_vsync`): the corner follows the
  line up to 16:9 and keeps the patch's -111 for wider shapes. The HUD bars still scale with 1/s.
- [x] Checks: `check.py port` OK; the state traces, Windows shapes and the Mi 11i in Result.

## Decisions

- Past 16:9 the corner stays at the patch's -111; up to 16:9 the line is kept (unchanged output
  at 16:9 and narrower). Why -111 is enough: the corner's distance from the centre scales with s,
  and at -111 it is -1419.6 GS pixels at 4:3, so it stays past the picture's left side (-320) for
  every s above 0.2254, a shape up to 5.9:1; the port caps widescreen at 4:1 (s 1/3, the corner
  at -473). Up to 16:9 nothing changes: the line's values lie between -95.886 and -111, in front of
  the plane. A fitted projective rule could keep the corner where the 16:9 patch puts it for each
  shape, but it would move it toward the plane as the shape widens, for no visible gain.
- Investigation switches stayed local and uncommitted (the Android `dhd-test.env` reader, the
  Windows `DHD_DIAG_NO_NET` and `DHD_DIAG_SHADOW`): the fix needs none of them.

## Surprises

- The bug looked arm64-only because the Windows check ran in the harness's 960x720 window, where
  WIDESCREEN=1 changes nothing (a 4:3 window is 4:3). Checks of widescreen need the window's shape
  (`DHD_WINDOW_WIDTH`/`HEIGHT`) or a named ratio (`DHD_WIDESCREEN=20:9`).
- Any 2:1 or wider screen has it, PC too: 21:9 and 32:9 monitors with widescreen on (the line
  passes -111 at about 16:9 and the plane at about 1.9:1).
- A phone run of the state trace equals the Windows one field for field once both show the same
  menu (Windows offers ONLINE in the main menu, Android does not): a cross-platform check of the
  core that worked on the first try.

## Result

Where it lived: not arm64, not the renderers. The port's widescreen at screen
shapes wider than about 2:1 moved the bike shop's dark panel corner (0xA60038) past the camera's
plane, so the panel was drawn mirrored across the picture and over the rider. Every phone has
it (20:9 screens, widescreen on by default), and so would a PC at 21:9 or 32:9.

The fix: `host_widescreen_vsync` keeps the 16:9 patch's value for wider shapes. Checked:
- boot (600 fields) and race (5000) state traces equal to the base build's (02fb762), without
  widescreen as the recipe runs; the shop route with `DHD_WIDESCREEN=16:9` (1400 fields) equal too;
  with `DHD_WIDESCREEN=20:9` the one EE word that differs at fields 143 and 1300 is 0xA60038
  (base C2FC3A5D, -126.1; fix C2DE0000, -111), VU memory, GS registers and VRAM equal.
- Windows, hidden desktop, the shop at 20:9, 21:9 (1280x540) and 32:9 (1280x360): the rider is
  there, no wedge.
- The Mi 11i, release-signed APK of the fix, the player's own dhd.ini (native 2x, widescreen,
  `aspect 2.2222`): the rider beside the bike where the same screen had the goggles and the wedge.

At the merge: finding 056's "Bike shop words" (the line in 1/s for other shapes) is then true only
up to 16:9 (`partly superseded by 154`). Seen and left alone: the shop's condition (the low
halfword of 0xA095D0 reads 2) already holds at the title (field 143), so the three words are
written from there on, as PCSX2's conditional patch would.
