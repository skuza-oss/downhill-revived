# 129: names beside the track bar's dots

Work in progress on `trackbar-names`. The race-progress "track bar" is a 3D HUD object placed in
front of the camera (finding 056), with one small dot per rider that `func_00179338`
(`src/unsorted/00179338.c`) moves along it, storing each rider's 0..1 progress at `rider+0x7A40`.
For every rider it feeds a 16-byte "handle" held in the HUD object to `func_00203B68`, which
lerps the dot's world point from that handle's owner node and writes it to the marker object's
`+0x18`. The handle lives at a fixed offset for this PC's rider and for "type 1" riders
(`0x10E4`/`0x1124` and `0x10F4`/`0x1134`), and in a per-rider array indexed past the split-screen
views for the rest (`0xFC4 + (i - players)*0x10`, or `0x1054 + ...` in the other HUD mode);
`handle[0]` is the owner, `handle[1]` the marker whose `+0x10` is the dot's world point.

Code: `port/hle/trackbar.c` (`hle_trackbar`, called from `func_00176ED8` right after
`func_00179338` by `port/patches_trackbar.py`), `hle_online_slot_name` in `port/hle/online.c`
(the online name of a race slot), and the `hle/trackbar.c` line in
`port/tools/build_wasm.py`'s `MODULE_UNITS`. It projects each dot's world point with
`func_00222D48` and draws the rider's name just to its right with the HUD font, in the same
style as `hle_online_names` (pen `0x4F3570` saved and restored, `func_0023D618`/`func_0023D650`
for the view centre/size, `func_0020FB80` glyph scale, `func_0020F860` width, `func_0020F9F0`
centred text with a black edge). This PC's name is red, the other riders' green; offline the
view's own rider reads "YOU", the other split-screen players (race slots below the number of
views, one per view, as `func_00179338`'s handles have them) "P1".."P4", and the computer riders
"CPU".

## Where this stopped / what is left

The name drawing and the world-to-screen projection are written, syntax-checked with the build's
clang flags, and committed. **The dots themselves are not yet recoloured**: I could not identify
the marker's colour/material field from the disassembly alone (`func_002063F0` only copies the
owner's position to the visual node), so the first build carries a guarded `DHD_TB_DUMP` dump of
each rider's handle, owner and marker words to find it; the recolour should be a small write to
whichever of those words is the dot's colour, added in `hle_trackbar`'s per-slot loop.

**Verification is unfinished.** Every race screenshot so far (my own run folder
`build/port/tb129/`, one offline race navigated by `race.NAV` with `DHD_RACE_SYNC`) shows the
game's own MUSIC/audio overlay over the race — the track bar HUD is hidden behind it, so the
names could not be seen. That overlay's text ("MUSIC", "MUSIC VOLUME", "GAME SOUNDS", ...) is in
neither `dhd_core.wasm` nor `dhd64.exe`, so it comes from the disc, not the port. Next: get a
clean race frame (try dismissing the overlay with `CIRCLE`/`START` from `DHD_RACE_PAD`, or a
different route to the race), confirm the dot offsets from the `DHD_TB_DUMP` dump printed while
the names draw, then verify the names sit to the right of the dots and add the dot recolour.

The `D:\DevCaches\build.lock` was held by `custom-music` throughout, so the build/run that would
confirm the offsets was not done in this session.
