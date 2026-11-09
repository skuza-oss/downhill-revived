# 183: Performance settings: shadows and sun glow, lens flare, particles

Status: current
Checked: 2026-11-09

Branch `feat/performance-effects` from `main` 0574b05, started 2026-11-08. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The owner's decisions (2026-11-08): a **Performance** group in REVIVED SETTINGS > Graphics, with a
preset switch **Performance Mode** (turns all three off) and one toggle each:

1. **Shadows and sun glow**: the riders' shadows and the sun's glow chain; the sun disc stays.
2. **Lens flare**: the flare's ghost ring.
3. **Particles**: the particle system's effects (dust, dirt, spray).

Everything on by default (the original look). Not offered: fog, sharp textures, draw distance, a
lower output resolution. The settings are visual only: game state must not change, so they are
allowed online.

The Mali lane (branch `perf/mali-gpu`, its finding not merged yet) measured the effects with renderer heuristics (small surfaces, a blend key, VU program
addresses; a diagnostic patch, never committed). A setting needs an exact identification that works
on every renderer and platform: switch each effect off where the game decides to draw it.

Acceptance:

- each toggle off removes only its effect (PC pictures at a few race fields: on, each off, all off);
- boot and race state traces against the base build: identical with everything on; with
  everything off, identical in the game's state (only the effects' own draw data differs);
- `python tools/check.py port`; the settings tests (`settings/tests/generated.rs`);
- the Mi 11i (throttled `t612`, 4x, uncapped, the Mali lane's route), Performance Mode off against on,
  two interleaved runs each; the whole-race gain compared with the Mali lane's +40 % (all off, 4x).

## Progress

- [x] 2026-11-08: finding reserved
- [x] 2026-11-08: the effects found in the decompiled code (below) and switched off there
  (`port/patches_effects.py`), the settings and the REVIVED SETTINGS rows: `cfda8fb`
- [x] 2026-11-08: PC pictures (2x, the Mali lane's route, fields 6800, 7400, 8640 and 9500): on,
  each switch off, all off ("Checks")
- [x] 2026-11-08: boot and race state traces against the base 0574b05 ("Checks")
- [x] 2026-11-09: the Mi 11i, throttled, 4x: Performance Mode off against on ("Speed")
- [x] 2026-11-09: `python tools/check.py port` OK; the settings crate's tests OK

## Checks

**Pictures** (PC, Vulkan's default adapter, native renderer, 2x, GS captures; kept by the lane, not
committed). Pixels differing by more than 8 from the all-on picture:

| field | shadows and glow off | lens flare off | particles off | all off |
|---|---:|---:|---:|---:|
| 6800 (race start, the sun in view) | 47,600: the large glow round the sun | 65,762: the ghost ring and ghosts | 10,322: the heat haze over the trail | 105,878 |
| 7400 | 3,656: the rider's shadow | 0 | 310 | 3,966 |
| 8640 | 14,950: the rider's shadow | 0 | 426 | 15,390 |
| 9500 | 19,914: the rider's shadow | 0 | 740 | 20,654 |

All off is the union of the three; the sun's disc and halo stay in every picture; no other part of
the picture changes. No "microprogram failed" line in any run.

**The page** (hidden desktop, keys): Graphics shows Resolution, Scaling, Texture Pack, Font, the
"Performance" heading, Performance Mode and the three switches. Performance Mode ON set all three
OFF; Particles back ON showed Performance Mode as Custom; dhd.ini then held `shadows = 0`,
`lens_flare = 0`, `particles = 1`.

**State traces** (`state_trace_run.py`, the effects build against the base build):

- Everything on: boot (600 fields) and race (5000 fields) identical to the base, every section.
- Everything off: menu, phase, race clock, sound and code identical on all 5000 race fields; ram,
  hw, VU, the GS sections and (on some fields) cpu and scratch differ, as they must: the frame's
  draw data is different. The EE RAM dumps at fields 4000 and 4900 differ in 1719 and 1739 words, all
  draw data: the shadow packets and chain (`D_0062F980`, `D_0064F980`, `D_00660080`, `D_00660190`,
  `D_0029E278`, `D_0029E288`), the particle chains (`D_00679280`), the haze packets' four words, the
  flare object's pass count (0x00723A94), the glow chain's sprites (`D_00759790`) and its builder's
  cursors (`D_0029DC38`, `D_0029DC50`), the shadow light point in the view's data (0x00528740), the
  texture slots table (`D_0067DF90`) and, in the level's mesh packets, TEX0 base pointers (fewer
  uploads, so textures land at other places in GS memory), and one bit (0x1000) of a scene node's
  word at 0x0078CA84 that the shadows' second scene walk leaves. No rider, camera or emitter state
  differs, and the race's ids match field by field.

## Speed

The rooted Mi 11i throttled with `port/tools/phone_throttle.py --apply t612`, 4x, uncapped
(`DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1`), the Mali lane's route (`DHD_BENCH=300-14000`), a
debuggable APK with the release's native code and the shipped PGO profile (sha256 `681f7a4b…`),
the keys in the app's dhd.ini, 90 s cool-downs with the screen on and other apps closed; Performance
Mode off, on, off, on. Fields/s (GPU ms a timed frame), means of the two runs per state:

| fields | off | on | gain | the Mali lane's widest all-off set |
|---|---:|---:|---:|---:|
| menus and loading, 300-6060 | 62.6 (11.8) | 69.0 (10.9) | +10 % | +18 % |
| race start, 6321-7500 | 47.2 (17.4) | 75.1 (10.7) | +59 % | +57 % |
| 7500-9000 | 60.9 (14.2) | 74.2 (11.7) | +22 % | +28 % |
| 9000-10500 | 63.6 (13.6) | 75.5 (11.4) | +19 % | +30 % |
| 10500-12000 | 62.9 (13.8) | 72.6 (12.0) | +15 % | +48 % |
| 12000-14000 | 66.7 (12.9) | 78.6 (11.0) | +18 % | +42 % |
| **the race, 6321-14000** | **60.4** | **75.4** | **+24.7 %** | **+40.3 %** |
| the route, 300-14000 | 61.2 | 72.4 | +18.3 % | +29.7 % |

The two runs of each state agree within 3 % on every range. Busy ms a field, off -> on: render
11.2 -> 8.7, VU 12.5 -> 11.4, GS 15.8 -> 13.0; the 30 s windows' limits move from cpu:render and
the GPU to cpu:vu and the GPU.

The Mali lane's widest set also took fog off, sampled every texture with points and drew the window
at 896 lines (none of them offered here) and lost the sun with its `small` rule; those account for
the rest of its gain (its own split: fog 4-5 % of the scene's fragment work; render thread 11.3 ->
4.5 ms). The race start, where the shadows and the glow carry the cost, gains the same. By that
lane's verdict (the Mi at 4x matches the Redmi 9 at 2x on whole-race gains within about 5 points),
the Redmi 9 would go from 47.2 to about 59 fields/s in the race: an estimate, not measured.

## Where the game draws each effect

All three are found by the code that makes their draw data, not by what the renderer sees, so the
rule is the same for both renderers and every platform (the patches are in the core).

**The riders' shadows** (unit `0021D250`, overlay B programs 0x2DC8 and 0x30A0). Four shadow slots,
`D_0062F880[4]` (`func_0021D638` hands one to a rider). Each view, `func_0021D950` (from
`func_00215B68`) checks the game's own switch `D_0029E268` (set by `func_0021E428`: 1 in a race, 0
in the menus), places each slot's light point and, for a slot whose rider is drawn in this view's
buffer, sets the slot's bit in scratchpad 0x70000208 and starts its packet (FRAME to the 64 x 64
target at page 508, a clear). Everything after it reads that bit: `func_0021D250` (the scene walk's
callback) adds the rider's meshes as a silhouette (program 0x2DC8), `func_00217C00` walks the scene
a second time for each marked slot (`func_0021E3D8` returns its owner only with the bit) to draw
the ground again darkened by the blurred silhouette (`func_0021DE28`, program 0x30A0;
`func_0021DF38` adds the two blur passes `D_0065F980`/`D_0065FD80`, overlay B 0x0050), and
`func_0021E0B8`/`func_0021E130` link the shadow chain into the frame only when a silhouette was
added (`D_0029E288`). **Off**: the bit is never set (`port_fx_shadows()` beside the buffer test in
`func_0021D950`); the light points are still placed as retail does. No shadow chain, no blur, no
second scene walk.

**The sun's glow chain** (`func_00237D20`, made once a race by `func_00228BE8`; per view by
`func_002388F0`). The sun is a flare source with flag 0x80 (`func_00228328`); each field it is in
front of the camera, `func_00228328` calls `func_00238740` with its screen place, which places the
chain's sprites and sets the pending flag `D_0029E3A0[view]`, the only place that sets it.
`func_002388F0` (from the particle chain, `func_0022E9A0`) adds the chain only while the flag is
set: the picture around the sun copied into a 64 x 64 target (pages 508/510), blurred outwards
with sprites scaled by 1.07, and added back over the sun (the large soft glow). **Off**:
`func_00228328` does not make the call. The sun's disc and halo are the flare's passes 0 and 1
(next) and stay.

**The lens flare** (VU1 overlay A program 0x26D8, `func_00226588`; the object `D_00723A80[view]`,
made by `func_00225DB0`). Each visible flare source (the sun, and the particle system's flares, as
a pickup's: `func_00228090`) is three quadwords; the object's header gives the program the
sources' count and the pass count, 18, one per row of five tables of 18 (`D_00298EB0` colours,
`D_00298EE8` alpha, `D_00298F30` texture, `D_00298F48` place along the line from the source
through the screen's centre, `D_00298F90` size). Pass 0 draws each source's disc (place 1.0, size
150), pass 1 its halo (the same, only when the Z probe found it unhidden), passes 2-17 the 16 ghosts
(places 0.15 ... -0.775, sizes 6-40). **Off**: `func_00226588`, which writes the header's count of
sources each field, also writes the pass count: 18, or 2. The Z probe (`func_00228328`,
`sceGsExecStoreImage` of a 4 x 4 patch; findings 028, 097) and everything the CPU does with its
answer stay as they are.

**The particles** (emitters `D_0067E180[64]`, updated by the unit `00228BE8`, drawn by overlay A).
Each update ends with the emitter's own packet made by its render type's builder (`func_00224F58`
0x0050, `00225458` 0x02D0, `002256F0` 0x1010, `00225A40` 0x1240, `00225CE8`, `002268E0` 0x0E10,
`00226B90` 0x17C8, `00226D08` 0x1F50, `00227310` 0x22C8, `00227518` 0x2E48, `00227840` 0x30F8,
`00227C60` 0x0850). Per view, the asm `func_0022E9A0` culls and sorts the visible ones and calls
`func_0022E560` for each, which links the emitter's packet into the view's chain. Render type 0xB
(program 0x0E10) is a heat haze: it samples two 64 x 64 textures that VU1 program 0x29C0 draws each
field (`D_00678880`, `D_00678D80`, made once a race by `func_00237918`; linked by `func_0022E9A0`
whenever any emitter is drawn). **Off**: `func_0022E560` links nothing, and the two texture
packets' MSCAL word (recorded when `func_00237918` writes it) is a VIF NOP; with Particles on it is
written back to the game's own word. The emitters are still updated (the particle generator
`D_0029DAF8` and every emitter's state go on as in retail); only the drawing stops. A pickup's flare
is a flare source (above), not a particle, so it stays with Particles off.

## Decisions

- **One switch for shadows and the sun's glow**, as the owner asked: both use the 64 x 64 targets
  at pages 508/510 and both are cut by the same `port_fx_shadows()`.
- **Performance Mode is no setting of its own**: it shows ON when the three are off, OFF when they
  are on, Custom otherwise, and changing it writes the three. No dhd.ini key can disagree with the
  three rows.
- **The rows apply at once** (live): nothing the game logic reads changes, so a change mid-race is
  safe; the core reads the variables through `hle_env` (pc_settings.c `port_fx_*`).
- **Shown on every platform**: Android's and the browser's row lists (`DHD_PC_SETTINGS_ROWS`) name
  the three variables; the preset row follows them. The page gets a "Performance" heading row
  (`menu_page_text`), so menu rows and settings rows are mapped both ways (`row_of_menu`,
  `menu_of_row`).
- **Particles means the whole particle system's drawing** (overlay A except the flare's 0x26D8):
  every emitter, the heat haze included. The Mali lane's widest set measured the same set
  (`DHD_DIAG_SKIP_A`).

## Surprises

- The Mali lane's labels for overlay A's 0x0E10 and 0x29C0 ("the riders' ground shadows") were the
  heat haze: the pictures without them differ from the full ones only in the warped band of
  scenery behind the deck. The riders' shadows are the overlay B programs 0x2DC8 and 0x30A0.
- **The haze program runs in two parts.** The first build made only the MSCAL of 0x29C0 a NOP:
  the MSCNT after it (continue where the program stopped) then continued the program before it,
  overlay A's at 0x0050, on stale data, which looped until the VU guard stopped it ("vu1:
  microprogram at 0x0050 didn't end", thousands a minute) and made the VU thread the limit: the Mi
  11i went from 61.3 to 43.2 fields/s with Performance Mode on. Both words are NOPs now (the
  MSCNT at `p6->x4` of `func_00237918`); the runs above have no such line.
- Retail has its own shadow switch (`D_0029E268`, `func_0021E428`), but it is game RAM the race
  code sets; the setting gates the per-view bit instead, so RAM stays as retail writes it.

## Result

Merged into `main` 2026-11-09.

REVIVED SETTINGS > Graphics has a Performance group: Performance Mode and Shadows & Sun Glow, Lens
Flare and Particles (dhd.ini `shadows`, `lens_flare`, `particles`; `DHD_SHADOWS`,
`DHD_LENS_FLARE`, `DHD_PARTICLES`), all on by default, live, on PC, Android and the browser. Each is
cut in the game code that makes the effect's draw data (`port/patches_effects.py`); the game's
state is the same with any of them off. On the throttled Mi 11i at 4x, Performance Mode gives
+24.7 % over the race (60.4 -> 75.4 fields/s) and +59 % at the race start. Left: the launcher's
settings tabs don't show the group (the game's page does).
