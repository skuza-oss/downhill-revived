# 160: Garbage blocks on screen in the first seconds after start

Status: current
Checked: 2026-11-06

Branch `fix/boot-artifacts` from `main` 113f76f, started 2026-11-06. Merged into `main` at b8a63b5
on 2026-11-06; released in 0.7.12.

## Goal and acceptance

For the first 1-3 seconds after the game starts, players see rectangular blocks of other pictures:
pieces of the loading card and the logo over old picture data, then rock textures under the title's
PRESS X TO START (two players' screenshots, kept by the lead outside the repository; one
from an Android phone, the owner sees the same on Windows; issue #47's tester reports it too). The
blocks look like GS memory pages that hold other data (textures uploaded at boot) shown as
picture.

Goal: what a PS2 shows in those seconds is what the port shows (PCSX2 is the referee: replay or
record the same GS stream before blaming the renderer, finding 025), on every platform and both
renderers if both show it. Candidates to rule in or out with evidence: the display switched off or
faded on the PS2 (PMODE's circuit enables and ALP, DISPFB/DISPLAY, SMODE) and the port's scanout
showing the framebuffer anyway; the native renderer's surfaces or page tracking showing pages it
never drew; a frame presented before the game's first real one; the widescreen/4:3 card code
(finding 140) in those frames.

Acceptance: the cause named with evidence (frame dumps of the first seconds from the port and
PCSX2's picture or GS-stream replay for the same fields), the fix in the common code path, the
first seconds clean on Windows with RENDER=native and RENDER=exact (frame dumps or the harness's
captures on the hidden desktop), `test_gs_wgpu` unchanged, `check.py port` passing, and boot and
race state traces equal to the base build.

## Progress

- [x] 2026-11-06: finding reserved (7ae6e6e)
- [x] 2026-11-06: reproduced on Windows, both renderers, hidden desktop: every presented picture of
  a boot with movies (Cross skips the attract), `DHD_CAPTURE_PRESENTED` with a 0.1 ms interval,
  plus `DHD_FIELD_TRACE` for the display registers of each field. Both players' screenshots come
  back exactly (native, scale 2, widescreen: captures 754-757 and 804-805 of the run).
- [x] 2026-11-06: the PS2 side: a retail boot in the comparison PCSX2 (`pcsx2_retail.py`'s adapter,
  software renderer, same Cross presses by field) with each field's displayed buffer and, through
  PCSX2's `SaveInfo` dumps, each vsync's privileged registers. The PS2 has PMODE = 0 (no read
  circuit, a black screen) in exactly the fields where the port shows garbage.
- [x] 2026-11-06: cause: the GS reset in `sceGsResetGraph` (below)
- [x] 2026-11-06: fix: the reset clears the display registers; the native renderer shows black with
  no read circuit on; a movie's last picture stays until the game sets its display again
- [x] 2026-11-06: checks: after-captures (native and exact, movies on and skipped, the race load),
  the race load on the PS2, state traces, `check.py port`

## Cause

Two transitions in the boot, the same mechanism:

| | PS2 (PCSX2 frames) | port, base build (fields of one run) |
|---|---|---|
| attract skipped (Cross at 1500) | 1500-1513 the movie's last picture (DISPFB2 page 280, 640x368); 1513-1533 PMODE 0, black; 1534 the loading card, 512 wide at page 56 | 7 fields of the memory card check card (page 140), 4 of the loading card's texture pages over it (screenshot 1); field 789 the card |
| card to title | 1908-1912 PMODE 0, black; 1913 the title, 640 wide at page 70 | fields 833-839: DISPFB2 still the card's 512-wide buffer at page 112 while the title draws over its pages: the title's buffers and rock textures (screenshot 2) |

The same resets come in the race load: on the PS2 (the race route of `state_trace_run.py`, run in
PCSX2 by `DHD_COMPARE_NAV`) the screen is black at the race card's setup (frames 2667-2697) and for
11 frames between the card and the flyover (3187-3197). The base port showed the race card's text
and textures over the goggles at field 1352 and the card for those 11 fields.

`func_001B8CD8` (before the startup movies, the loading card, the title and other loads) calls the
SDK's `sceGsResetGraph(0, 1, 2, ff)` (`func_00243430`), which writes CSR = 0x200: a GS reset. On
the GS that clears the privileged registers too: PMODE, SMODE2, DISPFB, DISPLAY, BGCOLOR (PCSX2's
`gsCSRwrite`: "Privilage registers also reset", `memset(g_RealGSMem, 0)`). The screen stays black
until the game's next display setup (`func_0021F000`) writes PMODE again, after the load. The
port's reset (`gs_priv_write`) cleared only the drawing registers, and the game's register page
at 0x12000000, which the presenter reads each vsync, kept PMODE 0x8066 and the old DISPFB/DISPLAY:
so both renderers kept scanning out a framebuffer the game was loading textures into.

The grey card after the movie has a second cause: the PS2's movie player shows its pictures from
its own buffer (`func_0020BDA0(2)`: PMODE 0xFF02, DISPFB2 0x1518, page 280, after the first
picture), which stays on screen after the movie until the reset. The port's player (movie.c,
finding 055) is HLE: it never writes those registers, so after a movie the game's registers still
point at the memory card check card, shown for the fields between the movie's end and the reset,
and for one field between each two startup movies (captures 245, 442, 608 of the native run).

Ruled out: PMODE's ALP (0x80 throughout, and circuit 2 is not blended), the native renderer's
surfaces and page tracking (the exact renderer shows the same pictures, and they are what GS memory
holds), a picture before the game's first, and the widescreen card code (the same with the exact
renderer, which has none of it).

## Decisions

- The reset clears the display registers on both sides: the GS's copy (`gs_priv_write`, shared by
  every build and tool) and the game's register page (`dma_mmio_write`), as the hardware has one
  register file. CSR and IMR keep the port's handling (the FIELD bit the game reads is untouched).
- With no read circuit on, the native renderer now shows a black picture (`DISPLAY_OFF`, a clear
  of the display texture) instead of keeping the last one, as the exact renderer already did
  (`gs_display` fails, the presenter shows black). Other cases without a display surface keep the
  last picture.
- After a movie its last picture stays while the game's display registers are those it kept during
  the movie and no GS reset came (present.c `movie_shown`, `Gs.resets`): the PS2's player buffer
  staying on screen. A register change or a reset ends it. The registers alone don't do: with the
  fix they are 0 through the startup movies (the reset before them, and only the PS2's player sets
  them), so the reset before the loading card changes nothing there.
- The movie's pictures stay out of GS memory: copying them into page 280 and letting the game's
  `func_0020BDA0` set the registers would match the PS2 word for word, but costs an upload and a
  native import per picture and changes the game's flags (D_0029DA00) and so its state.
- `SetGsCrt` stays a no-op: the game writes SMODE2 with PMODE in every display setup, so the reset's
  SMODE2 0 is never scanned out.

## Surprises

- The comparison PCSX2's adapter skips a field whose display has no read circuit (no capture file):
  the missing captures were the first sign. Its `SaveInfo` register dumps (`SaveFrameStart`,
  `SaveFrameCount`, `DumpGSData`) give each vsync's registers without a rebuild.
- PCSX2's dump frame numbers are one lower than the adapter's capture numbers (frame 1513's
  registers belong to capture 1514).
- With `DHD_COMPARE_NAV` the adapter does not stop at `DHD_COMPARE_UNTIL`: the run had to be
  stopped after its last capture.
- A PNG of every presented picture slows the game to 4-25 fps (more in the 3D menus); for the race
  load, field-numbered GS captures (`DHD_SCREENSHOT` with `DHD_CAPTURE_FIELDS=1340-1480`) were
  enough and kept the game at speed outside the window.

## How to reproduce

Port, on a hidden desktop (`hidden_game.Game`, which takes the game lock): `DHD_SCREENSHOT=<unset>`,
`DHD_CAPTURE_PRESENTED=OUT/p`, `DHD_CAPTURE_AT=0`, `DHD_CAPTURE_INTERVAL=0.0001`, movies on
(`DHD_SKIP_MOVIES=<unset>`), `DHD_MEMCARD=1` with an empty `DHD_MEMCARD_DIR`, `DHD_SCALE=2`,
`DHD_WIDESCREEN=1`, a 1280x720 window, `DHD_FIELD_TRACE=OUT/trace` with
`DHD_FIELD_TRACE_FIELDS=0-6000` (each field's display registers), the X key 30 s after the window
shows (skips the attract). PS2: `pcsx2_retail.py`'s config and adapter on a hidden desktop, a p2m2
pad movie with Cross at frames 1500 and 1530, `DHD_COMPARE_FIELDS` for the frames to keep, and in
`[EmuCore/GS]` `DumpGSData`, `SaveInfo`, `SaveFrameStart`, `SaveFrameCount`, `SaveDrawCount=-1`
with `SWDumpDirectory` for `*_vsync_gs_reg.txt` (PMODE, DISPFB, DISPLAY, SMODE of each vsync).

## Result

Code: `port/gs/gs.c` (`gs_priv_write`: the reset clears PMODE to BGCOLOR, counts `Gs.resets`),
`port/hle/dma.c` (the game's register page follows), `port/gs/native_core.inc` and
`port/render/src/native/{render,scanout}.rs` (`NATIVE_DISPLAY_OFF` / `DISPLAY_OFF`: a black
picture), `port/hle/movie.c` and `port/gs/present.c` (`movie_shown`: the last picture held). No
`src/` change, no ABI layout change (`display_surface` gains the value -2).

After the fix, on Windows (RTX 3070, Direct3D 12), hidden desktop, every presented picture:

| run | after the attract | card to title | between movies |
|---|---|---|---|
| native, scale 2, widescreen, movies | the movie's last picture 4 pictures, black 9, the card | black 5, the title | no grey card |
| exact, movies | the last picture 4, black 9, the card | black 5, the title | no grey card |
| native, movies skipped (players' default) | (no movie) black 9, the card | black 5, the title | - |
| native, race load (fields 1340-1480) | black 1349-1354, the race card | black 1452-1462, the flyover | - |

Before, the same runs showed the garbage of both screenshots (native and exact), the grey card
between movies, and with movies skipped garbage at the card (4 pictures) and the title (2). The
captures are game-derived and stay outside the repository.

Checks:
- state traces against the base build (113f76f, `state_trace_run.py boot` and `race`, headless,
  virtual clock): the only differing section is `hw`, in the fields between a reset and the game's
  next display setup (boot 14 of 600 rows: 95-103, 149-153; race 31 of 5000: those and 1350-1355,
  1453-1463). Word dumps at fields 95, 150, 1352 and 1456: PMODE, SMODE2, DISPFB2 and DISPLAY2 at
  0x12000000, 0x20, 0x90, 0xA0, zero after the fix as on the PS2. EE RAM, CPU, scratchpad, VU, GS
  registers, VRAM, the GS stream and the sound are equal in every row. Two base runs of the race
  trace are equal to each other (the base is deterministic).
- `python tools/check.py port`: OK (docs, tree, core, runtime tests, game, net tests,
  `test_gs_wgpu`, wasm2c chunks); `test_gs_wgpu` itself is unchanged.
- `rustgs.py test --target x86_64-pc-windows-msvc`: every test of the Rust library passes (unit,
  `abi_layout`, `device_smoke`, `native_spirv`, the native ones).
- `test_dhdr_abi.py`: all 293 rows of the header and `abi.rs` agree on x86_64 and wasm32; its two
  function-table complaints (`dhdr_surface_counter`, `dhdr_surface_touch`) predate this lane.
- The core changed (`dma.c`, core id 2bc710619147444e): online rooms need both players on this
  build.

Left: the PS2 shows black for longer in the race card's load (31 frames against the port's 6):
load time, not a difference in what is shown.
