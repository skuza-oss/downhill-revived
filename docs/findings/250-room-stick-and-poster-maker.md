# 250: the online room takes the stick; Poster Maker's picture

Status: current

Branch `fix/room-stick-poster-250` from `main` cae6f05c, started 2026-11-15. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

Two player reports:

- **#103:** the online Race Room and its "Leave the room?" question move the cursor only with the
  d-pad; the game's own menus take the left stick too. A generic USB pad in its digital mode (its
  d-pad comes as the stick) moves nothing there. Every screen of the online menu must take the
  stick as the game's menus do, through one shared input function, with a test that fails before
  and passes after.
- **#102:** Poster Maker (a race's pause menu) shows a black camera preview, and the saved poster
  (OPTIONS > POSTERS) is black too (0.8.2, Windows 11, Intel UHD on Dx12, native renderer, Native
  VU on, 2x, widescreen). The poster must show the scene in the preview, after saving and after a
  restart.

Done means `python tools/check.py quick`, `check.py port` and `check.py docs` green; boot and race
state traces equal to the base build's; the new tests green. Out of scope: the online race menu
over a running race (`online.c`, its own stick reading), other OPTIONS pages.

## Progress

- [x] 2026-11-15: #103 read and fixed: `port/hle/menu_pads.c`, test `port/tools/test_menu_pads.py`
- [x] 2026-11-15: #102 reproduced on the hidden desktop (below), cause found: the native front end
- [x] 2026-11-15: #102 fixed: `native_colour_read` (`port/gs/native_core.inc`) and
  `dhdr_native_surface_read_begin` / `_finish` (`port/render/src/native/surface_read.rs`); test in
  `port/gs/test_native_transfers.c`

## Decisions

- **#103, one reader for our screens that read the pads themselves.** The reporter's reading is
  right: `room_input` read only the button word (`func_00220230`). The game's popup menus take a
  direction when the button is down or the left stick is past 120 from the centre
  (`func_00220640` mode 0 calls `func_002209A8`; the popup manager's reader `func_001909A8`).
  `menu_pads_held()` (`port/hle/menu_pads.c`) is that word with `func_002209A8` itself asked for
  each direction, so the threshold is the game's own, and a pad in its digital mode (mode 4) has no
  stick, as in the game. The room, the leave question, the track list, ROOM RULES and the rider
  screen's Back read it (`online_menu.c`'s `pads_held` is it now). The online menu's other pages
  (HOME, the lists, QUICK, NAME and CODE entry) are popup-manager pages: the game's reader already
  moves their cursor with the stick, unchanged.
- **#103, repeat.** The room's own repeat (a direction held 24 updates, then every 6) now applies to
  the stick as to the d-pad: the stick is the d-pad there. The game's popup menus do not repeat a
  held direction at all (their latch `D_00660D80` holds until the pad is let go), so the room
  keeps its own rule for both.
- **#102, read the picture from the GPU, not draw it twice.** The native renderer draws only on the
  GPU; GS memory never holds its colour (by design: only the Z probe is answered, finding 097). A
  local-to-host read of colour pages a native colour surface owns now waits until the handed
  pictures are drawn (as `native_picture_read` does for screenshots), reads the read's rectangle
  of that surface (each GS pixel the mean of its scale x scale texels; the bytes the CT32 import
  puts in a surface, so a read gives back what an upload gave), and writes it into GS memory, which
  `gs_read_image` packs as before. One hook for every colour read (`Gs.read_image`, called by
  `gs_read_image` and `gs_defer_read_image`), not a Poster Maker case. Turning on exact colour
  drawing for the frame instead would cost the exact renderer a whole frame for every read and
  still miss the first one.
- **What is not read.** A read in another layout than the surface's (width, 16 against 32 bits),
  pages no colour surface owns, and runs with `DHD_EXACT_PROBE` (the deterministic checks: the exact
  device answers there, unchanged) keep GS memory as it was. Draws recorded in the same field
  before the read are not on the GPU yet, so not in it; dhd.log says so on its `gs native: colour
  read` line (the first eight reads are logged). Poster Maker reads while the paused scene stands
  still, so the field before is the same picture.
- **Game state.** Only a colour read changes anything, and the game reads colour only in Poster
  Maker (the reporter's log: `color_reads=0` over a session with a race). The state-trace recipes
  never read colour, and they run with `DHD_EXACT_PROBE=1`, where the hook does nothing.

## Surprises

- **#102 is every PC's, not the reporter's GPU.** Reproduced here first try on the hidden desktop
  (`DHD_UNLOCK_ALL=1`, Single Player, Single Event, Race, Mt. Midoule FR, pause, POSTER MAKER, Cross,
  Cross): the "Save Poster?" screen's preview is black with the native renderer, Native VU on and
  with `DHD_NATIVE_VU1=off`; with the exact front end (`DHD_RENDER=exact`, the PS2's pixels) the same
  route shows the scene. So no HLE piece is missing and the GS stream is right: the exact renderer,
  which keeps GS memory as the PS2 does, answers the read. PCSX2 was not run (it opens a window);
  the exact front end stands in for the referee here.
- The poster comes from `func_0019C358`: two `sceGsExecStoreImage` reads of 512 x 112 CT32 at block
  0x700 (`func_0019C220`), scaled 4 to 3 columns into a 384 x 224 RGB555 picture, which the preview
  draws and the card saves (`BASCUS-97177PS/poster0`). Each read is over the deferred reads' 4096
  pixels, so it runs as a plain `gs_read_image`, after the exact device's `sync_range` (which has
  nothing: with the default Z probe the exact device draws nothing).

## Result

Both fixed on Windows (dd6c7c8c #103, 4cc760ef #102).

- **Checked:** `check.py quick`, `check.py port` and `check.py docs` green. The boot (600 rows) and race
  (5000 rows) state traces equal the base build's (cae6f05c). `test_menu_pads.py` (in
  `test_runtime`): its source guard fails on the old `online_menu.c`, which reads `func_00220230`
  itself, and its guest test checks the d-pad, each stick edge at the game's 120 (7 and 249 count,
  8 and 248 do not), pad 1's stick, digital mode and a missing pad. `test_native_transfers.py`'s
  colour read (two 512 x 112 CT32 reads of a cleared surface) passes; with `DHD_EXACT_PROBE=1`,
  where the hook is off, it fails with every pixel 0, the bug's picture. On the hidden desktop,
  Poster Maker's "Save Poster?" preview shows the scene, the card gets `poster0`, and after a fresh
  start OPTIONS > POSTERS shows the thumbnail and the full poster.
- **Unproven on real hardware:** a real pad in the room (the reporter's generic USB pad in digital
  mode, an Xbox pad's stick); the reporter's Intel UHD on Dx12; the browser and phones (the code is
  shared, only Windows was run; the browser's read waits through `native_read_wait`, as
  screenshots do); PCSX2 as referee (not run, the exact front end stood in).
