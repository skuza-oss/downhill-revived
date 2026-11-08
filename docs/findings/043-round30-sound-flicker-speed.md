# Round 30: sound stutter, native flicker, exact-frame cost, VU1 load

Status: partly superseded by 088 (gate/build tools, the "still to do" list)
Checked: 2026-11-04

Since then: resolution scale is done (findings 045, 086: 1x-8x), the unstretched 16:9 HUD too
(finding 056), vibration through SDL3 (finding 059), a stick dead zone (finding 134). MSAA/FXAA,
texture filtering and button remap options were not built (no such key in
`port/render/settings/src/table.rs`). The
launcher here is gone (the player launcher is `port/launcher`, finding 093), and so are
`gate_native_renderer.py` and `build_clean.py` (finding 088); state traces are the check now.

Done in this round: sound time-stretch, the native renderer's alpha and texture fixes, the shorter
exact-frame hold, cheaper work-queue signalling, a bulk VIF unpack path, the dhd.ini settings
file, window modes, scaled presentation, widescreen 16:9 and a launcher are committed;
`port/tools/gate_native_renderer.py` passes on each batch (game state, every readback record and
the early reference moments). Not done yet: resolution scale, an unstretched 16:9 HUD, MSAA/FXAA, texture
filtering, remap and vibration (see "PC options").

## Step 0: the plain build

`port/tools/build_clean.py` builds a revision from a clean worktree (`build/port/clean/tree`,
asm and assets copied in), relinked on an accepted recompiled-object basis
(`--recomp-basis build/port/r29/native16/objects.rsp`), and `--install` copies the result to
`build/port/fast-preview/native/dhd.exe` (the old one kept as `dhd-previous.exe`, the manifest as
`build.json`). `--tree` builds another revision in its own worktree.

## Step A: sound stutter

Gaps came from the output thread finding its ring empty whenever a field ran late. The thread now
keeps a cushion (`DHD_SND_LATENCY_MS`, 70 ms), paces against the device clock, and when the game
runs slower than real time plays the ring at the measured game rate by WSOLA time-stretch
(waveform-similarity overlap-add: fixed nominal hop times tempo, best match within a seek window,
pitch unchanged; standard method, no code copied). At full speed it copies, joining and leaving
the stretcher at window boundaries where both give the same signal. `DHD_SND_STRETCH=0` and
`DHD_SND_LEGACY=1` keep the old paths for comparison.

Real-time paced race, 3 interleaved runs, audio on: legacy 521 gaps (median 901 ms of silence);
paced without stretch 15 gaps (70 ms); paced with stretch 1 gap (0 ms).

Later (2026-10-27): the same output runs on every platform through SDL3's audio stream
(`port/host/sound_out_sdl.c`, WASAPI under it on Windows); the WASAPI file and the legacy loop
(`DHD_SND_LEGACY`) are gone. The stream's get-callback does the thread's work once per device
period, which is asked for at 10 ms (`SDL_AUDIO_DEVICE_SAMPLE_FRAMES` 480, WASAPI's event period);
the queue it measures is the stream plus one period, WASAPI's padding. A 20-s minimized title run
on Windows (bench window fields 600-1500) gave the same numbers as the WASAPI build: no underruns,
queue p1/p50/p99 62/71/79 ms (WASAPI 62/70/78), game tick max 17 ms. The boot's load stall gaps
in both.

## Exact-frame hold

When the Z probe comes within 32 rows of the exact band the native renderer draws whole fields
exactly. The 32-row lead already makes the first such field exact; the old 120-field tail only
repeated it. `DHD_NATIVE_FULL_HOLD` (default 20) sets the tail: full exact fields in the gate race
went from 257 to 57, gate still matching.

## Ground and loading-screen flicker (native only)

Counting per field (`DHD_NATIVE_TEXLOG=path.csv`: unsupported surface textures with the reason,
decode failures, pages) showed no untextured draws: the 16-slot decode probe never failed in a
race. Two real faults were found instead.

1. **Rider shadow at half, varying strength.** The shadow is built in a CT32 buffer: a white
   silhouette with alpha 0xFF, two bilinear blur passes, then the ground is darkened by
   `Cd*(1-As)` with As = texture alpha x vertex alpha / 128. The native renderer stored alpha as
   a/128 clamped to 1, so 0xFF became 0x80 and the blur and darkening ran on a clipped value: the
   shadow under the rider came out weaker and changed from field to field, which on the ground
   reads as flicker. Render targets now hold alpha as a/255 (full 8 bits) and the pixel shader
   writes the GS blend factor a/128 to a second output; `BLEND_SA`/`BLEND_ISA` use dual-source
   blending (`SRC1_ALPHA`, `INV_SRC1_ALPHA`). No blend in a race reads destination alpha (counted:
   `blend_dest_alpha=0`). Field 4000 mean difference to exact 1.93 -> 1.80, field 400 2.14 -> 1.94;
   shadows match exact by eye.
2. **Stale textures over GPU-drawn memory.** A texture whose pages a native surface had drawn, in
   a layout the surface could not be sampled as (other PSM, other width), was decoded from the CPU
   copy of GS memory, which is stale for those pages. Such textures are now rebuilt on the GPU by
   the `native_convert` compute shader: each page takes its words from its source (the CPU
   decode, or one of up to four colour/depth surfaces), using the GS address swizzle and an
   inverse page table for the CT32 and Z32 layouts; CT16/CT24/indexed formats are expanded with
   TEXA and the palette. Z24 top-byte textures (8H/4HL/4HH) keep the CPU copy unless a colour
   surface drew the page after its last upload. About 18-38 thousand conversions per race run.
   Depth surfaces became `R32_TYPELESS` (D32_FLOAT for depth, R32_FLOAT to sample).

The loading-screen flicker did not reproduce after these fixes: scripted captures of fields
1470-1476 (track preview), 2190-2200 and 2360-3260 (fly-over to GO), with and without
interpolation, match the exact renderer frame by frame (`DHD_CAPTURE_FIELDS=a-b,...` saves every
field in the windows). It is most likely the same two faults; it needs confirming in real play.

## Speed: where the heavy-scene time goes

Profile of a heavy race stretch (fields around 4700-6000, 950 frames, native renderer): the
VIF/VU worker thread is busy 85% of the wall time at 47.5 fields/s, about 18 ms per field: VU1
programs 11.4 ms, GIF hand-off 2.2 ms, VIF unpack and the rest about 4.4 ms. The GS/D3D worker
and the EE thread have slack. The top VU1 entries: program 0078 at MSCAL 0x1040, 27% of VU time
(585 calls a field, 5.2 us each), then 0020/0020 (63 us a call), 0078/1CB0, 0040/1040,
1638/1040, 1430/1040.

**Multi-core VU1.** `DHD_VU_SEQ` captures 8,000 consecutive VU1 runs with their full state;
`port/tools/vu1_seq_dep.py` replays each run from the state a parallel dispatcher would have 1-3
runs earlier. Only 22.9% of runs give the same output at distance 1 (25.9% at 2, 32.8% at 3):
consecutive runs share registers (above all the VI2 output cursor) and data memory. Per run
parallelism is therefore out. Per object (one MSCAL chain per object setup) the chains depend on
each other mainly through VI2 and the constants uploaded before them, so a design that is
possible but large is: a liveness pass that knows which registers and memory each program
reads, per-worker VU memory mirrors fed by the VIF stream, and in-order stitching of the GIF
output. With three VU workers it could bring the VU share from 11.4 ms to about 4-5 ms a field,
at the cost of copying the VIF stream to every mirror (about 1-2 ms). Not built.

**GPU compute VU1.** The programs branch per vertex (clipping), kick GIF packets mid-program and
use VU timing quirks the exact referee depends on; a GPU port would need the AOT translator to
emit HLSL and a readback of every GIF packet the same field. Estimate: weeks of work, a large
win only for the plain transform loops (already native SSE kernels for five of them), and a
GPU-to-CPU round trip per field. Not worth it before the multi-core design.

## Speed: the work queues

Both graphics queues (EE -> VIF/VU worker, VU worker -> GS/D3D worker, `port/hle/graphics_queue.c`,
`gs_queue.inc`) signalled a Windows event for every packet and every retirement: thousands of
kernel calls a field, each also waking a sleeping thread, on the busiest thread. Each side now
announces when it sleeps (an interlocked counter it raises before its last recheck), the other
side signals only then, and a waiting side spins about 4,000 pauses before it sleeps. Waits for
retirement keep a 1 ms timeout as a guard. Packet order and contents are unchanged, so game
state is too (gate passes; queue and split-queue stress tests pass).

`unpack_run` (`port/hle/vif.c`) gained a bulk path for the common UNPACK: no mask, no row
addition (STMOD 0), no filling cycle, nothing buffered and whole words; it writes each vector
straight from the stream (V4-32 by one 16-byte copy) instead of the per-field mask switch and
three modulo operations per vector. Results are the same words the generic path writes (VIF
tests pass, gate passes).

Bench (`port/tools/bench_race.py`, fields 6000-7000 unpaced, native renderer, 3 interleaved runs
each, game window focused, `build/port/BENCH.lock` held and no other dhd.exe seen during the
run): base 71.31 fields/s median (runs 73.2, 55.4, 71.3), queue + VIF 83.96 (64.8, 84.0, 85.0):
+17.7% at the median, +16% best against best. Spreads are large (17-20 fields/s) because the
other worker's builds still load the CPU; a first set taken while that worker's games also ran is
discarded. The queue change alone (first set) reached 84.9 in one run, so most of the gain is the
signalling.

## PC options

Settings live in `dhd.ini` next to dhd.exe (or the file `DHD_SETTINGS` names), loaded first
thing in `main` (`host_settings_load`, `port/hle/host.c`): `key = value` lines, `[sections]` and
comments ignored. Each key sets the DHD_* variable it stands for unless the environment already
has it, so environment variables override the file; a line may also name a DHD_* variable
directly. Keys: `iso`, `skip_movies`, `renderer` (native, exact), `fps` (60, 120, 144, monitor,
or any rate: interpolated pictures), `window` (windowed, borderless, fullscreen), `width`,
`height`, `widescreen`, `sound_latency_ms`; reserved for the options still to come: `scale`,
`msaa`, `fxaa`, `filter`, `vibration`, `remap`.

Window (`port/gs/present.c`): `DHD_WINDOW=windowed` (client `DHD_WINDOW_WIDTH` x
`DHD_WINDOW_HEIGHT`, 896x672 by default, 4:3), `borderless` (a popup over the primary monitor)
or `fullscreen` (exclusive: `ResizeTarget` to the monitor's size, `SetFullscreenState`, left again
at exit). The native renderer now draws the scanned-out picture into a back buffer the size of
the window instead of copying it 1:1 and letting DXGI stretch: one filtered triangle into a
viewport that keeps the picture's shape, black bars around it. `DHD_ASPECT` = 4:3 (default, a
television's picture), 16:9 or any W:H, `stretch`, or `pixels`; `DHD_PRESENT_FILTER=point` for
sharp pixels (linear by default). The exact renderer keeps its 1:1 copy (DXGI stretches it).
Captures and the gate read the scanned-out picture, not the back buffer, so they are unaffected.
Checked by eye: a 1280x720 window shows the 4:3 picture at 960x720 with bars at the sides, at
120 pictures/s, with the settings taken from a test dhd.ini.

`port/launcher/Launcher.ps1` (WinForms; `build_clean.py --install` copies it to
`build/port/fast-preview` with `Launcher.cmd`) edits the same dhd.ini: disc image, renderer,
pictures per second, display mode, window size, picture shape and scaling, intro movies; Play
saves and starts the game with no other DHD_* variables.

### Widescreen 16:9

Source map: the PCSX2 widescreen patch for this disc, `patches/SCUS-97177_5AE01D98.pnach` in
github.com/PCSX2/pcsx2_patches, "Widescreen Hacks (by Aced14)" (forum thread: PCSX2 Widescreen
Game Patches, page 304), and the same values in a PS4 Lua port of it (mycompiler.io/view/GoxnAkKt8Dw,
"no interlacing by someother1ne"). The pcsx2_patches repository has no licence file (README and
patches only), so nothing is copied or shipped from it: it was read as a map of which game values
change, and the values are applied from our side with credit here and in `port/hle/host.c`. A
Scribd copy (document 836572594) could not be read beyond its page wrapper; its summary lists
only optional tweaks (fog, post-processing, LOD distance), none needed for 16:9.

What the patch changes, read against our code: every view goes through one projection setter,
`func_0023D8D8(view, x, y)`: x is the focal length in pixels (view->x0, also used for the culling
planes), y the vertical factor relative to x. The patch is Hor+: every X it touches times 3/4,
every Y times 4/3, so the vertical scale stays (0.58333 x 277 = 0.77778 x 207.75) and the sides
widen. Sites: the 1P race (`func_0016B8C8`, 277), replay (`func_001A8A48`, 277), bike shop
(`func_001F84A8`, 688 + up to 2312 close-up), title and shell menus (688; the title's call is in
`func_001B6708`, recompiled from asm), the view table `D_0027ECC0` (0xC0 bytes a view, X at +0 and
Y at +0x20: paused 2-4P, 2P both ways, 4P, the menu's Y; the loading-screen view, item 11, is left
alone as the patch leaves it commented out; item 5's X 348 and item 10's 13/0.98 are untouched),
three tree sprite widths (0x26C5C0, 0x26C700, 0x26C758: 277 -> 207.75, or trees look squeezed)
and, in the bike shop's run-time block, the HUD bar widths and shadow width (when the halfword at
0xA095D0 is 2).

Ours (`DHD_WIDESCREEN=1`, dhd.ini `widescreen = 1`, launcher checkbox): `port_fov_x` (3/4, else 1)
multiplies the code constants through `port/patches.py` (the source stays matching; the title and
shell 688 are caught in the setter), `host_widescreen_init` rewrites the 22 data words after
checking each still holds its retail value, `host_widescreen_vsync` does the bike shop's live
words, and the native picture defaults to 16:9 (`DHD_ASPECT`). With the option off, x * 1.0f is
exact, so nothing changes. Checked by eye at field 4000 of the recorded race: the same moment with
a wider view, the rider's proportions and the vertical framing unchanged. The HUD is stretched
4/3, as in PCSX2; keeping it unstretched would need the native renderer to tell HUD draws from
the scene and is not done.

This writes EE memory and changes what the game culls, so game state differs with the option
on: it does (a dust cloud present at field 4000 without it is absent with it). Online players
need the same setting; the exact-state gates run with it off (gate matches with it off).

### No interlacing

The patch's other section changes `andi v0, v0, 1` at 0x00243D34 to `andi v0, v0, 0`: inside
`func_00243CE8`, which returns the CSR FIELD bit (bit 13 of 0x12001000) in interlaced mode, so the
game would always see the even field. The game reads it per field (`func_001B55F8`:
`D_0029E110 = !field`, which picks a half-line draw offset and a register value 2 or 3 passed on
by `func_001B4DD0`). Our GS toggles FIELD every vblank as the hardware does (`gs_vblank`), so the
game draws alternate fields offset by half a line, exactly as on a PS2; both renderers see the
same game state, so this cannot explain a native-only flicker. Forcing even fields is a change of
game state (and of a function recompiled from asm, which takes no source patch), so it is not
implemented; it would be an option of the same kind as widescreen.

Still to do: resolution scale (draw native surfaces at 2-4x, the exact band and Z probe staying
at 1x), an unstretched HUD in 16:9, MSAA/FXAA (FXAA as a pass in the present shader is the cheap
one), texture filtering, remap, DualSense rumble (output report 0x02 over USB, 0x31 over
Bluetooth).
