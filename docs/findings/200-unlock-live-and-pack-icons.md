# 200: Unlock All applies at once; button icons over a texture pack's font

Status: current
Supersedes: parts of 058, 095, 134
Checked: 2026-11-11

Branch `fix/unlock-live-and-pack-icons` from `main` f0971070 (v0.7.16), started 2026-11-11. This
file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Two player reports:

- **#83, Unlock All only after a restart** (0.7.15, Windows). Turning REVIVED SETTINGS > Game >
  Unlock All on or off must apply as soon as the player leaves the page, for everything finding
  095 covers (tracks, riders and pro riders, sponsor rows, shop items, preset bikes). The save's own
  unlocks are still never written. With the setting off and never touched, the game state is
  exactly as before (boot and race state traces equal to f0971070's).
- **#81, PS2 button icons with a texture pack on** (0.7.13, Windows, `PROMPTS=xbox TEXTURES=on`).
  The chosen icons (Xbox, Nintendo, Auto with such a controller, and the remapped places of finding
  157) must show whether or not a texture pack is on; the pack's other textures keep working. One
  code path.

Out of scope: state traces, the browser build, captures and benchmarks (the lead runs the traces
once on the merged branch, listed under Result).

## Progress

- [x] 2026-11-11: finding reserved (e81d560b)
- [x] 2026-11-11: #83 reproduced on f0971070 and fixed (5e314dac)
- [x] 2026-11-11: #81 reproduced on f0971070 and fixed, with tests (200dabca)
- [x] 2026-11-11: `python tools/check.py quick` OK on 200dabca; `port/tools/test_texpack.py` RESULT
  PASS (81 `image:` checks, 9 font-icon checks); `port/tools/test_gs_wgpu.py --skip-hardware`
  VERDICT PASS (its stubs changed)
- [x] 2026-11-11: merged into `main` (1a01efd7) and checked there once (below, Result)

## #83: the root cause

The value itself was live: `port_unlock_all()` reads `DHD_UNLOCK_ALL` through `hle_env`, and the
page applies the row at once (`commit`, `hle_env_changed`). Every reader finding 095 patched reads it
when its screen is built, so tracks, MOSHBOWL / SUPER JUMP, OPTIONS' PICKUP EDITOR, shop rows and the
41 preset bikes already followed a change made in the game (seen in the runs below). The one thing
the front end keeps is the rider select's statues: `func_001BB670` copies the 14 bonus and pro
riders' unlock bits (`func_00193FE8`) into `D_004B25A0` +0x3C (`state[15]`, which the lobby and
`func_001F7DE0`'s statue animations read), and it runs only at the game's start (`func_001B6708`,
called from `_start`), at a memory card load (OPTIONS' LOAD GAME DATA, `func_001F0160` case 6) and
in the retail cheat (`func_001F0780`); `func_001F66B0` (front end start `func_001F6AB8`, screen 18
`func_001F1500`) also refreshes it. A toggle in the page
calls none of these, so the BONUS RIDER / PRO RIDER statues stayed locked (or unlocked) until the
next start. (Finding 095's note that `func_001BB670` "runs on each rider-select entry" is wrong:
its callers are the game's start, the load and the cheat.)

Fix: `port_unlock_refresh()` (`port/patches_unlock.py`, added to `001EB1D8.cpp`) does what LOAD GAME
DATA does after a load: `func_001BB670(&D_004B25A0)`, `func_001F66B0()`, `func_001F7DE0()`.
`commit()` in `port/hle/pc_settings.c` calls it whenever the Unlock All row is applied (a change,
and the Undo that puts the opened value back). It writes only the lobby's copy and the statues'
animation state, never the profile `D_004225A0`; the cheat's writer `func_001F6618`
(`func_00194290`) is not called. Nothing runs unless the row changes.

## #81: the root cause

The icons are painted into the renderer's copy of the menus' 512x512 font atlas (`font_sharp.c`,
`button_icons.c`), which `native_replace` draws like a pack replacement. `native_replacement_view`
(`gpu_rust_native.inc`) returned the pack's texture whenever the pack had the atlas, so a pack that
replaces the font (common: PCSX2 dumps include it) hid the chosen icons. Still the case on
f0971070 (reproduced below).

Fix (one path, no switch):
- The pack lookup for the text engine's font moves into `native_decode` (it now takes the context),
  so the copy knows whether a pack replaces it.
- `font_sharp_view` takes the pack's replacement (`FontSharpPack`, a loader for its picture). With a
  pack the copy is made only for the 512x512 atlas with other icons than the game's in their places
  (a style key != 0): the pack's picture (`texpack_image`, read and decoded on the GS worker, once
  per style; the font copy cache keeps it) at its own scale, 1x-8x (`fit`: as it is, or bilinear to
  the nearest whole scale), with `button_icons_paint` over the eight icon cells. If the picture
  can't be read, the copy is made from the game's font as without a pack. Otherwise no copy: the
  pack's own atlas is drawn, as before.
- `native_replacement_view` prefers the copy, then the pack. The depth-buffer path in
  `native_texture` tries `native_replace` when the decoded font has a copy or a pack entry.
- The browser has no packs (stub `texpack_image`); its path is unchanged.

## Decisions

- **#81: keep the pack's letters, paint our icons over its cells** (not "prefer our copy"). The
  player chose the pack for its look, and chose the icons separately; this honours both. Our own
  copy would have swapped the pack's whole font for the game's as soon as Xbox icons were picked.
  With the PlayStation icons in their places the pack's atlas is drawn untouched; with them moved
  (finding 157) the pack's own icon cells are moved, as `move_game_icons` does for the game's.
- **#81: the pack's picture, not our sharpened font, as the base**: a pack is already high
  resolution; its scale is taken from its width (rounded, 1x-8x, at most the pack's 4096 limit).
- **#83: refresh in `commit`, not at the page's close**: the page is a game popup (menu 13) that
  returns to OPTIONS without a hook of ours; `commit` is where the value changes, and Undo goes
  through it too. The refresh copies what LOAD GAME DATA does in the same OPTIONS screen, so it
  runs in a context the game itself uses.

## Surprises

- In the hidden-desktop harness `DHD_NAV` steps don't drive the REVIVED SETTINGS page reliably
  (it shows as menu 13; presses got lost and later steps landed on other screens), and posted keys
  are ignored while a `DHD_NAV` script runs (it replaces the live pad). Driving everything with
  posted keys after `hidden_game.to_main` with `DHD_MEMCARD=1` and an empty `DHD_MEMCARD_DIR` works.
- Everything but the rider statues already followed a change on f0971070: only one cache to fix.

## How it was checked (hidden desktop, untimed, one game; nothing committed)

A local driver (`hidden_game.Game`, `to_main`, posted keys, a GS capture after each step; keyboard
defaults: X Cross, S Triangle, A L1, arrows, Enter Start), `DHD_MEMCARD=1` with an empty card
folder (a fresh save: bonus and pro riders, MOSHBOWL, SUPER JUMP and all but six tracks locked),
`DHD_SETTINGS=none` unless said.
- Toggle keys: main menu `down down x` (OPTIONS), `x` (REVIVED SETTINGS), `a` (L1: Game tab),
  `down right` (Unlock All), `x` (keep), `s` (back to the main menu). Route after it: `x` (rider
  select), `right` x14 with a capture each, `x x` (START NEW GAME), `x` (SINGLE EVENT), `x` (RACE),
  `x` (inventory BIKES), `down`, `s`, `enter` (track select), `right` x6.
- f0971070, toggled on: log `pc settings: Unlock All: 1`; the rider select still showed BONUS RIDER
  and PRO RIDER statues (the bug), while tracks (Mt. Konawaki FR listed), modes and 41 preset bikes
  were unlocked. Started with `DHD_UNLOCK_ALL=1`: every rider shown.
- 5e314dac, toggled on: Raquel, Drud, E.C. Carter, Tara Llanes, Richie Schley, Brian Lopes, Missy
  Giove, Kineticlops shown and selectable; modes, tracks and bikes unlocked.
- 5e314dac, toggled off (a dhd.ini with only the header and `unlock_all = 1`, `DHD_SETTINGS` naming
  it): log `Unlock All: 0`; statues locked again, MOSHBOWL / SUPER JUMP greyed, six tracks.
- #81: a local pack folder (`DHD_TEXTURE_PACK`) with a synthetic 1024x1024 PNG named as the atlas
  (`a85772cca2056b43-d789619edcafb368-00002664`, PSMT4HL 512x512, found with
  `DHD_TEXTURE_PACK_LOG`; a box per cell) and ten 256x256 magenta PNGs named as the main menu's
  backgrounds. f0971070 with `DHD_PROMPTS=xbox`: the legend showed the pack's boxes where A and Y
  belong (the bug). 200dabca: `xbox` shows A and Y, `nintendo` B and X, both over the pack's boxes
  for the letters; `playstation` the pack's own cells; `xbox` with `DHD_FONT=original` A and Y; the
  magenta backgrounds drawn in every run.
- Not checked in a game: a real controller's Auto and a remapped pad layout with a pack (the
  harness has no controller; `test_font_icons.c` covers the remapped places on the copy).

## Result

Merged into `main` as 1a01efd7 (with finding 201's lane) and checked there once, 2026-11-11:
- `python tools/check.py port -j 6`: every step OK (docs, tree, core, runtime tests, game build,
  retail scan, network tests, `test_gs_wgpu --skip-hardware`, wasm2c chunks). Also
  `rustgs.py test --target x86_64-pc-windows-msvc` (every crate test ok), `test_texpack.py` RESULT
  PASS (81 `image:` checks and the font-icon checks, among them an unreadable pack picture and
  another font texture with a pack), `test_gs_wgpu.py` with the hardware tests VERDICT PASS.
- The browser build (`port/web/build.py build/wasm/dhd_core.wasm`) compiles with the `texpack_image`
  stub and the new `native_decode`.
- Boot (600 fields) and race (5000 fields) state traces with Unlock All off and never touched,
  against f0971070's: `state_diff.py` PASS, every row equal.
- In game (hidden desktop, untimed, posted keys as above, a fresh card):
  - From a header-only dhd.ini: bonus and pro rider statues; Unlock All on in the page
    (`pc settings: Unlock All: 1`), back to the rider select: all 14 riders shown, no restart; off
    again (`Unlock All: 0`): the statues back, MOSHBOWL and SUPER JUMP greyed, the save's six
    tracks. The page remembers its tab: a second visit opens on Game, so L1 is not pressed again.
  - From `unlock_all = 1`: every rider shown; turned off in the page: the statues back.
  - The synthetic pack with `prompts = xbox`: A and Y in the legends (main menu, OPTIONS, REVIVED
    SETTINGS) over the pack's boxes; `nintendo`: B and X; `playstation`: the pack's own cells.
    With no pack and `xbox`: the legend strips pixel for pixel equal to f0971070's.
  - Unlock All on, the pack on, `xbox`: Kineticlops picked and raced on a Mountain Cross track
    the save had not unlocked, 41 s of race, no error lines in dhd.log.
- Not checked: a real controller on Auto, a remapped pad layout with a pack (covered by
  `test_font_icons.c` on the copy), a players' real texture pack, the race HUD's prompts with a
  pack (the race run showed no prompt on screen).
- Findings 095 (when `func_001BB670` runs), 058 ("a pack entry wins") and 134 (the icons with a
  pack) are partly superseded by this one. STATE.md's REVIVED SETTINGS line names this finding for
  Unlock All and the button icons.
