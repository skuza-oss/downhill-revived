# 056: Widescreen HUD at any shape, player defaults, EXIT GAME

Status: partly superseded by 101 (player_defaults in host.c, the disc prompt, 4:3 cards; since 154 the bike shop corner past 16:9)
Checked: 2026-11-04

The HUD anchoring and EXIT GAME are current. The player defaults now live in one place, the
`dhd_settings` crate (`port/render/settings/src/table.rs`, finding 101): native, 2x, borderless,
widescreen on and `DHD_ASPECT` wide as below, but `skip_movies` is 1 there (movies skipped); `host.c`'s `player_defaults` and the disc prompt are gone
(findings 093, 101). The full-screen 4:3 pictures of the "Full-screen 2D pictures" section fill
widescreen since finding 140.

Round 40. Three parts:
- in widescreen the HUD, menus and 2D keep their shape at any aspect;
- a new install starts at 2x, borderless, in the monitor's shape with widescreen on;
- the main menu has an EXIT GAME row.

Code:
- `port/hle/widescreen.c` (moved out of host.c);
- `port/patches_widescreen.py`;
- `port/hle/exit_menu.c` and `port/patches_exit.py`;
- the native renderer's presentation (`port/gs/gpu_native.inc`);
- the defaults in `port/hle/host.c`.

No `src/` change. With widescreen off every new helper returns its input.

## How the game draws its HUD

Widescreen was already Hor+ (finding 043, from Aced14's PCSX2 pnach): the 22 field-of-view words
are X times s and Y divided by s, with s = (4/3) / aspect. Before this round s was fixed at 0.75
(16:9). The picture is still 640 (or 512) pixels wide and is shown wider, so anything the game
draws in 2D is stretched by 1/s. A capture of the old build at 16:9 showed two kinds of HUD:

1. **3D pieces in front of the camera**: the speedometer, the track bar and the place.
   `func_00176930(owner, obj, offset, ...)` places a scene object at a camera-space offset (x
   right, y forward, z up) of view `owner->x1280`. VU1 projects them with the Hor+ view, so they
   keep their shape, but they stay at their 4:3 angles and are pulled in from the edges.
2. **2D through the font engine**: the HUD text (speed digits, PUNCH, times, place numbers,
   CONGRATULATIONS) and every menu and popup string. These are stretched.
   - `func_0020F9F0` (centred), `func_0020F9A8` (left) and `func_0020FA98` (right) set the line;
   - `func_0020FBB0(font, view, s)` walks the string;
   - `func_0020FDB0` emits one sprite per glyph: pen `font->x10` and width `font->x8`, in 1/16
     pixel from the view's left side;
   - the HUD's own code (units 0x1751C8-0x17C2A0) is what calls the first two for the race HUD.

Other 2D, each patched separately:
- popup and message boxes (00190E48, 001E4950, 00151AD0, 00169BD0);
- the engine's 2D vertex `func_00210578`, used by the popup bars and the front end's fades;
- a HUD diamond (0017B0D8) and the markers over the other riders (0017B848, only their width).

The full-screen fades and the lens flare were left alone: a fade covers the whole view, and the
flare is projected.

## Method: the game's own 2D code, through port/patches.py

The fix maps X in the game's 2D code (PATCHES entries in `port/patches_widescreen.py`, added to
`port/patches.py`). The renderer was not asked to guess which draws are 2D. Two rules, each in
GS pixels of the view:

- **Centred 4:3** (menus, popups, the pause menu, results, fades): x' = cx + (x - cx) * s about
  the view's centre. A point at or past the view's side stays where it is, so full-width bars
  and fades still reach the edges. The composition is the 4:3 one, in the middle. Hor+ widens
  the 3D background (the main menu's rock, the pause and results backgrounds), which fills the
  sides.
- **Anchored HUD**: the HUD's text calls go through `port_hud_text_c` / `port_hud_text_l`, which
  mark the string as HUD. The string's reference x picks its anchor by thirds of the view: left
  side, centre or right side. Then x' = A + (x - A) * s, so an element keeps its distance to its
  side and its shape. The HUD's 3D pieces get the same treatment in camera space
  (`port_ws_hud_offset`):
  - u = x * (focal / s) / (depth * half width) gives where the piece is across the 4:3 view;
  - if |u| > 1/3, x moves out by (1 - s) * depth * half width / focal, the width the view
    gained at that depth.

Both rules depend only on s, so 16:9, 16:10, 21:9 and anything up to 4:1 work the same way.
`DHD_WIDESCREEN`:
- `1`: the display's shape (the monitor's; the window size's when windowed; a ratio in
  `DHD_ASPECT` if one is given);
- `W:H`: that shape;
- `0`: off.

The process is now DPI aware (`host_display_aware`), so borderless runs at the monitor's real
resolution and the monitor's shape is measured in pixels.

### Full-screen 2D pictures

Since finding 140 these screens and the movies fill the window: the picture's sides are filled with
the picture itself, blurred and darkened, and the game draws a card's text unmapped (card mode),
so it keeps its shape. Finding 140 also shows the goggles before a level wide again, and has the
current rule. What follows is this round's version.

Some screens are full-screen 2D pictures made for 4:3:
- the memory card warning;
- the publisher cards;
- the startup loading card (sunset rock and logo);
- the race loading card ("Single Event / Mt. Middle").

The native renderer shows these at 4:3, pillarboxed. A field counts as one when either:
- VU1 drew nothing in it; or
- the game may be showing a loading card and VU1 drew under 1000 primitives. The cards are about
  150 triangles of tiles, drawn through VU1. `host_widescreen_loading` says when a card may be up:
  - screen `D_0029DED8` = 0 (the title, before its 3D has loaded);
  - screen 9 (the race) with phase `D_0043F348` = 0 or 5 (loading, or loaded and waiting for X).

It takes 8 such fields in a row to switch, so one field can't flip it. During a race, its
countdown and its results, the primitive count is never consulted.

A trace of a race from the boot to the results logged the state and VU1 count at each change:
- the cards: 144-170 primitives;
- the title's 3D, the menus and the flyover: thousands;
- the race and the results: never under 1000.

The first version used the count alone (17ab51cf); review asked for the game's own state, as
above.

Movies are shown 4:3 by the movie player (finding 055).

### Other changes

- `DHD_ASPECT=wide` is the picture in widescreen's shape. It is what widescreen sets, and the PC
  SETTINGS "Picture" row offers it as "Wide".
- `DHD_SCREENSHOT_WINDOW=WxH` writes screenshots as a W x H window would show the picture, with
  bars and the 4:3 switch. The checks below use it.

### Bike shop words

The bike shop words (0xA095D0, 0xA09610, 0xA60038) are written each field while the condition
holds, as on PCSX2.
- The retail values logged this round: bars 1.0 (3F800002), shadow -95.886 (C2BFC5EA).
- The pnach's 16:9 shadow is -111. Other shapes use the line through (1, -95.886) and
  (4/3, -111) in 1/s.

## Player defaults (then `port/hle/host.c` `player_defaults`; now `dhd_settings`, finding 101)

The defaults apply only with a dhd.ini, and only for keys it doesn't name:

| key | default |
|---|---|
| renderer | native |
| scale | 2 |
| window | borderless (the monitor's resolution) |
| widescreen | 1 (the monitor's shape) |
| DHD_ASPECT | wide |
| skip_movies | 0 (movies play, finding 055) |

Where they come from:
- **New install.** The release zip ships no dhd.ini. The first start writes one from the
  template, and the template now lists these lines. Keys the template lacks are added by the
  disc prompt (`host_disc_ready`).
- **Existing dhd.ini.** Every line it has wins, and so does a `DHD_*` variable. A missing key
  takes the default.
- **No dhd.ini (the tools).** Nothing changes: `race_compare.py` (so the gate), `bench_race.py`
  and `online_race.py` set `DHD_SETTINGS=none` and strip `DHD_*`, so a dhd.ini beside the exe
  can't change their runs.

Checked: a dhd.ini with only `iso`, `window = windowed` and 1280x720 started with the native
renderer, scale 2 and widescreen 16:9, and kept windowed. `ws43.ini` (`widescreen = 0`) stayed 4:3.

## EXIT GAME

The main menu (`func_001ED5B8`) gets a fifth row under ONLINE (`port/patches_exit.py`, applied
after the ONLINE patch). The labels come from `port_exit_label`.

Choosing it opens a menu_page popup in the game's style: "QUIT DOWNHILL REVIVED?" with "No,
Return" (the cursor starts there) and "Yes, Quit", the words of the game's own confirm popup.
- No or Triangle returns to the main menu with the cursor on EXIT GAME.
- Yes calls `gs_window_close()`, which posts WM_CLOSE as the close button does. The game ends at
  the next field with exit code 0. The relaunched child ends with it (job object), and nothing
  is saved.
- Tested with DHD_NAV: open, No, open again, Yes. The log ends with "exit game: chosen in the
  main menu", the exit code is 0, and no dhd.exe of the run remains.

## Gate

`gate_native_renderer.py` on 30222647 (the 5b00e37f code plus the gdi32 link fix):
- the two arms match each other at every moment (EE through g+4500, 1155 readbacks);
- both differ from r39-plain in two words at g+0/2/12/16: 0x4F4324 (0x10 -> 0x24) and
  0x4F4454 (0 -> 0x10).

Those words are in the font engine's view-0 text packet buffer `D_004F3AD0` (+0x854, +0x984). An
EE dump at the main menu (DHD_NAV_DUMP, widescreen off, as in the gate) already shows them,
bb898f87 vs this build, among 53 differing words of that buffer. With widescreen off every 2D
helper is the identity. So in the front end the only change is the fifth row's text, which
leaves more glyphs in the buffer, and these leftovers carry into the race. This is by reasoning
and the dump, not an A/B build of c61937ef. The reference is re-recorded
once at integration; until then "arms match, only these words differ" is the pass.

The later commits of this round were not run through the gate; the user asked for short checks:
- 17ab51cf, 536c6329, ee6943f0: the 2D-field test of the presentation and the bike shop words,
  all behind widescreen, which the gate runs without;
- a7f9b7a3: the tools' `DHD_SETTINGS=none`.

## Screenshots

Under `build/port/r40ui/shots/` (local, game-derived), from clean builds with
`DHD_SCREENSHOT_WINDOW`:
- before/after at 16:9: race HUD, main menu, PC SETTINGS;
- before/after at 21:9: race HUD;
- after only at 21:9: main menu, PC SETTINGS;
- the pause menu, at 16:9 and 21:9;
- the loading cards;
- the EXIT GAME popup, at 16:9 and 21:9;
- race results (16:9).

The results shot comes from a local test build that finishes the race early. Its 2D code is
this round's.

## Research

- PCSX2 widescreen patches mostly change only the 3D field of view, and 2D is left stretched
  ([PCSX2 forum](https://forums.pcsx2.net/Thread-Stretched-3D-graphics-with-normal-2D-HUD),
  [widescreen patch thread](https://forums.pcsx2.net/Thread-PCSX2-Widescreen-Game-Patches?page=723)).
  The patches that fix a HUD write per-game scale words, as this disc's pnach does for the bike
  shop. No generic HUD fix exists.
- PC widescreen fixers (ThirteenAG's Widescreen Fixes Pack and similar) anchor HUD elements to
  the screen edges and keep menus 4:3 in the middle. The method here is the same, applied at the
  game's own 2D entry points.
- The pnach map is Aced14's SCUS-97177_5AE01D98.pnach (github.com/PCSX2/pcsx2_patches), as in
  finding 043. No code was copied.

## Left

- `func_0020FA98` (right-aligned text) is not wrapped for HUD use. The HUD's right-side text
  comes through the centred and left calls; nothing seen was wrong.
- The 2-4 player split screens were not checked. Anchoring uses each view's own centre and size.
- The menu_page popups (EXIT GAME, PC SETTINGS) sit left of centre, as at 4:3: that is the
  game's popup layout.
