# 132: Track picker names and disc previews

Status: partly superseded by 166 (the 23-track list: 25 since 166)
Supersedes: parts of 115, 116
Checked: 2026-11-04

Branch `feat/track-picker`, based on unpublished `next/0.7.7`. No version change.

## Names

The room reads retail `D_0026EFE8` at runtime, as Single Event's list
(`func_001989C0`), its track list (`func_0015CAE0`) and records do. These are
loaded from the player's checked executable data image, including the exact
spelling `Midoule`. Country text after the comma is omitted consistently;
the final FR/MX/TD event suffix remains. Mosh Pit adds `(Arena)`.
The picker, track row, public list, results, presence, track-change notice and
capacity-switch notice share this name function. The rotation stays numeric.
MOSH remains host-picked only; its rotation flag and Left/Right skip are unchanged.

| Code | Room name |
|---|---|
| ALPINE | Mt. Midoule FR |
| MOAB | Red Pass FR |
| JUNGLE | Mt. Konawaki FR |
| PERU | Mt. Quintanaroo FR |
| PERU TDH | Mt. Quintanaroo TD |
| GLACIER | Mt. Zorkovaska FR |
| ALPINE MX | Mt. Midoule MX |
| MOAB MX | Red Pass MX |
| JUNGLE MX | Mt. Konawaki MX |
| PERU MX | Mt. Quintanaroo MX |
| JUNGLE TD | Mt. Konawaki TD |
| AUBER ST | Mt. Liddellroch FR |
| FUJI | Mt. Imasaki FR |
| MOAB TDH | Red Pass TD |
| GLACIER X | Mt. Zorkovaska MX |
| AUBER MX | Mt. Liddellroch MX |
| AUBER STD | Mt. Liddellroch TD |
| BC | Mt. McSchley FR |
| CITY | Salt City FR |
| GLACIER T | Mt. Zorkovaska TD |
| MOSH | Mosh Pit (Arena) |
| ALPINE T | Mt. Midoule TD |
| BC TDX | Mt. McSchley MX |

The web search for existing track-name information found Speedrun's level list
(https://www.speedrun.com/downhill_domination/levels). Retail source/data,
not that secondary list, determines spelling and track indices. No external
code was copied.

## Pictures

The retail chooser is a camera/map scene (the chooser's `0x2588` animation,
`func_001F8628`), rather than a convenient standalone preview bank. The disc
already has landscape pictures on its Single Event loading pages, including
Mosh Pit. These are used instead of generating substitute art.

Runtime reads use existing `cdvd_find` and `cdvd_read`, from the player's ISO.
All source artwork stays on that disc. Destinations use `LOADBAR/LALPSE`,
`LMOASE`, `LJUNSE`, `LPERSE`, `LGLASE`, `LAUBSE`, `LFUJSE`, `LBCSE`, `LCITSE`,
`LMOSHSE`, respectively. Variants share their destination's landscape; the event
name distinguishes them. No images, extracted text tables, or game data ship.
No generated thumbnails were needed.

The texture formats were traced through retail `func_0021AC08`,
`func_002195F0` and the loading-page loader `func_001829C0`.
PCSX2's [GS CLUT implementation](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GSClut.cpp)
and [GS register definitions](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GSRegs.h)
were consulted for CSM1/indexed palette and TEX0 behaviour; no code was adapted.
TEX's 512-square indexed image starts at byte `0x170`; RTX's CSM1 palette
starts at `0x60`, except the standalone arena palette at `0x10`.
The loading mesh uses inverted UVs. Rotate 180 degrees and crop its landscape
above the cutout (340 rows); the arena has no cutout and uses a central crop.
The loader checks expected sizes, makes a 256 by 160 PSMCT16 picture (each pixel the mean of
its 2 x 2-3 source pixels, 5 bits a channel with a 4 x 4 ordered dither) and caches the ten
pictures in the core's own memory (about 820 KiB, not the game's heap). Failure
leaves the selected name visible and logs unavailable. The current track's underline in
the list is as wide as its name (the capitals measurement) and sits below the letters.

The picker is two columns of names (23 tracks, 12 rows); the selected track's
picture, its name and "UP TO N RIDERS" (its rider limit, finding 123) sit in a
panel on the right. A first three-column layout with "NAME - N" labels overlapped
its neighbours and the picture: retail's width routine (`func_0020F860`, used by
`menu_text_width`) knows only capitals, and mixed-case names measured short, so
`fit()` never shrank them. `menu_text_width` now measures an uppercased copy (lowercase
draws as small capitals; all-capital callers measure as before). It still leaves out
some letter spacing on long strings, so the room's track line aims 14 units under
its panel. The room's compact track/rules panel has no spare image area, so the
picture appears only in the picker.

### The picture's texture (lead review, 2026-11-02)

The first versions drew 128 x 80 flat sprites, one per sample, from a 245 KB packet in the
retail heap: noisy, blocky at 2x-4x, and the heap was never given back. Now
`menu_draw_picture` draws one bilinear sprite of a VRAM texture, and nothing comes from the
game's heap.

**Where in VRAM.** One hidden-desktop run (approved for this) logged every distinct TEX0
(texture and CLUT), MIPTBP, FRAME, ZBUF and upload / local-copy address the GS was given,
every 2 s, with a temporary hook in `gs_write` (not committed), and a local script
(`build/checks/census_report.py`) turned them into used block ranges per phase: the rider
screen, the room, the picker (16 cursor moves), the room again and the rider screen again
(CHANGE RIDER). The shell's two 640 x 224 PSMCT24 framebuffers and Z use blocks 0-0x1A40,
CLUTs sit at 0x1A95-0x1D9E, the texture bins at 0x1DF0-0x38E0 and small render targets and
textures from 0x3F80. **0x38E0-0x3F80 was never touched in any phase, nor anywhere in the
whole run** (boot, main and online menus, shop). The bins may reach 0x3B9F (their end
pointer `D_0029E22C` = word 0xEE7F0, `func_00218D98`), so the picture takes **blocks
0x3C00-0x3E00** (pages 0x1E0-0x1EF): 3 pages of margin above the bins' limit, 12 below the
render targets. The texture is PSMCT16, TBW 4, TW = TH = 256: its 256-line footprint is 16
pages; a PSMCT32 one would be 32 pages and reach 0x3F80. Only 160 lines are uploaded; the
UVs stay half a texel inside (0.5-255.5, 0.5-159.5), so bilinear never reads the rest.

**Upload.** The pixels sit in the core's memory with their GIF packet in front
(BITBLTBUF/TRXPOS/TRXREG/TRXDIR, then IMAGE: 81,920 bytes) and go straight to PATH3
(`host_service_graphics_feed(2, ...)`) when the cursor reaches a picture other than the one
in VRAM, or after the picture wasn't drawn for 8 fields (the picker closed). The GS gets it
before the frame's display list, which holds the sprite. dhd.log, once: `menu: track
picture: 81920 bytes to GS blocks 3C00-3E00 from the core's memory, per new picture`.

**Draw.** Two of the popups' existing rectangle packets: A+D TEXFLUSH, TEX0 (DECAL, alpha
from the vertex), TEX1 0x60 (bilinear magnify and minify), then one packed sprite (PRIM
0x116) with RGBAQ, UV, XYZ2 x2 through the game's own XYZ2 writer (widescreen mapping kept).
The text packets set their own TEX0 and TEX1 (`func_00210068`), so the bilinear TEX1 does
not reach the text drawn after it. The 245 KB heap packet, its allocation and its log lines
are gone.

## Verification

Plain `build.py -j 6` with `DHD_BUILD_SLOTS=2` (after the lead's build commits
b6ef0b0 and e6e4d0e): core and Windows game build without any manual step. After the texture change
the plain build ran through in one go: core `9a58b7b798b0528a` (strict link, 138 host
imports) and `dhd64.exe`, no warnings in the changed files. An earlier attempt's silent exit during the ABI adapter
stage did not come back.

Hidden desktop, before the lead's 03:40 process change (`build/checks/run-picker2.py`,
a local server, one game creating a public room, a scripted WebSocket member;
PrintWindow captures under `build/checks/picker2/`, not committed), on the build with
the two-column picker but before the track line's last width change: all ten disc
pictures logged "loaded from disc" and the packet "allocated, sampling step 1"; all 23
picker views showed the right name, picture and rider limit, nothing cut off or
overlapping; a race on Mt. Quintanaroo MX after the picker had allocated its packet
loaded and ran (the retail heap still loads a race). Not seen: TRACK CHANGED after a
race (the captures ended before the game was back in the room), the "TAKES 6" line,
and the room's track line after its width change (before it, "< Mt. Quintanaroo MX >"
touched the panel's right edge). No unit tests cover this menu code; the server is
untouched. No browser build was run.
Those runs predate the texture: the textured picture has not been seen on screen yet
(the census run used the flat-sprite build), so items 2 and 2b below are its first look.

### Checks for the lead (merged branch)

Windows game, `DHD_ONLINE_MENU_TRACE=1`, a local server:

1. ONLINE > CREATE A PUBLIC RACE, pick a rider, into the room. dhd.log: none of the
   `online menu: preview ... unavailable` lines.
2. Room: Up x5 to TRACK, X opens CHOOSE A TRACK. Step Right through all 23. Look for:
   two columns of names, none overlapping; the picture on the right matches the place
   (Mt. Midoule green alpine valley under a pink sky, Mt. Quintanaroo jungle, Mosh Pit
   the lit arena, ...; variants share their mountain's picture); the name under it and
   "UP TO N RIDERS" with that track's limit from finding 123 (Mt. Quintanaroo MX: 6). The
   picture: smooth at 1x and at 4x (bilinear, no squares, no speckle), the right picture
   for each place, the right way up, filling its frame. Text drawn after it (the name, the
   limit, BACK) as sharp as before. The current track's yellow underline (Mt. Midoule FR
   at first) is as wide as its name and below the letters. dhd.log: ten `online menu:
   preview L...SE loaded from disc` lines and one `menu: track picture: 81920 bytes to GS
   blocks 3C00-3E00 from the core's memory, per new picture`; no `menu: preview packet`
   lines any more.
2b. Triangle closes the picker; look at the room and its 3D rider, then CHANGE RIDER (the
   rider screen) and back: no texture glitches (a wrong patch on the rider, bike or
   background would mean the area isn't free after all). Open the picker again: the
   picture is right at once. With PC SETTINGS at 4x: the same.
3. Pick Mt. Quintanaroo MX (list place 9, the longest name) with X: the room's TRACK
   line `< Mt. Quintanaroo MX >` sits inside the left panel. Left/Right on TRACK steps
   names and skips Mosh Pit.
4. Race it (Square = ready; a second player or the rules' AUTO start), let it finish:
   the race loads after the picker (retail heap), and back in the room with THEN = NEXT
   TRACK the top-right line reads `TRACK CHANGED: Mt. Konawaki TD` (the next in the
   rotation), inside the screen. RANDOM TRACK: same line with another real name.
5. With 8 ready players on a 6-rider track (e.g. Mt. Quintanaroo MX), at grid lock the
   line `Mt. Quintanaroo MX TAKES 6: RACING <name>` fits on screen.
6. Public races list (ONLINE > PUBLIC RACES from a second game): the room's row names
   the track with the same name.
7. Browser build: compiles (the room code is shared; no browser-only change).

Research captures inspected earlier: `build/research/crop.png`,
`build/research/all-disc-previews.png` and `build/research/LMOSHSE.png` (ignored
local files). The montage exposed the arena's standalone palette; its loader was
corrected.

The hidden-game harness now has `Game.print_window`, also available as
`printwindow LABEL` in live_game.py, capturing only its own hidden client's HWND.
`build.py -j N` forwards the limit to core and native compilation.
