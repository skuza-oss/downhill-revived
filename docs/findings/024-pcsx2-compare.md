# 024 - Automatic PCSX2 rendering comparisons

Status: partly superseded by 088 (i686 build steps, build_compare_port.py and link.py)
Checked: 2026-11-04

The method (the race clock as the sync point, the pinned race seed, NAV, game-state dumps) is
current. Build steps are not: `build_compare_port.py --link` and the `link.py` relink are gone;
`race_compare.py` takes the 64-bit game with `--exe build/host64-exe/dhd64.exe`.

## Round 3: race comparison synchronised on the game's race clock (2026-10-14)

`port/tools/race_compare.py` compares retail PCSX2 and the port at five race moments:
countdown, start, +10 s, +30 s and +50 s. No GUI, no dialogs and no wall-clock timing.

```powershell
python port/tools/prepare_pcsx2_compare.py                      # patched PCSX2 adapter, once
python port/tools/build_host64.py                                # today; was build_compare_port.py --link
$env:DHD_ISO = '...\Downhill Domination (USA).iso'
python port/tools/race_compare.py LABEL --exe build/host64-exe/dhd64.exe   # about 40 minutes
```

(Then `--link` named the 32-bit link directory; that step is gone.)
Output: `build/port/compare/LABEL/index.html`, `contact-sheet.png`, one
`moment-*.png` triptych (PCSX2 | port | difference heatmap) per moment, `scores.json`,
and the raw captures under `retail/` and `port/`.

**Why the round-2 alignment failed.** Boot-field numbers can't line up: the port skips
movies and loads at different speeds, so it reached the race countdown at field 20,630
against 17,443 for PCSX2. Counting fields from GO doesn't work either. Under
`DHD_FRAME_CLOCK` the port spends more vsyncs per game frame than the PS2: the countdown
lasted 560 port fields against 260 retail, and at "GO + 599 fields" the HUD read 00:09
in PCSX2 but 00:02 in the port (run `race-r10-vsync-keyed`, kept for reference).

**How sync works now.** Both sides read three EE words on every vsync: the menu
(0x0029DED8, 9 in a race), the race phase (0x0043F348: 5 pre-race, 1 countdown, 2 GO,
3 later in the race) and the race clock (0x0043EEC8). The clock restarts at the countdown
and at GO, keeps running through phase 3, and advances 60 per second: in PCSX2 it read
exactly 300 at 300 fields after GO. A moment `c+N` / `g+N` is the first vsync after the
countdown/GO clock reaches N. Each side captures the vsync after that and the next
2 x `--window` (default 2) vsyncs. The port frame at delta 0 is paired with the most similar
PCSX2 frame among deltas 0..4 (64x56 thumbnail MAE), and the chosen delta is reported. Input is
synchronised the same way:

- before the countdown, both walk the menus by the screen shown (`NAV`, see "Event-driven
  navigation" below); `--timed-nav` keeps the old script keyed by field (PCSX2 through a p2m2
  movie, plus a Cross every 20 s up to 10 minutes for slow loads);
- from the countdown, both pads are neutral;
- after GO, both read the same race pad, keyed by the race clock (`--race-pad`, seconds; the
  default is r10's proof input: Cross 2 s every 20 s from 3 s, left/right taps every 12 s
  from 6 s).

The PCSX2 side is the round-2 adapter (v2.8.2 source, GPL) plus `DhdRaceSync` in its input
poll. It reads EE RAM directly, overrides both pads, and adds capture targets for the GS
thread. On the port side, `DHD_RACE_SYNC`, `DHD_RACE_PAD`, `DHD_RACE_CAPTURE` and
`DHD_RACE_CAPTURE_WINDOW` live in `port/hle/autopad.c`, with one call each in `host.c`
(pad) and `present.c` (capture). Other runs are unaffected unless `DHD_RACE_SYNC` is set.

**How exact it is.** Both sides capture at the same game-clock value, but the
simulation itself is not shared. The port's game frames span several clock ticks, so its
clock lands at or just past N (61 for 60, 16 for 15, 301 for 300), up to about 1/15 s late.
The HUD race timers agreed within one displayed second in the validation run. The riders'
own positions diverge as the race goes on: AI riders start differently, and the player's
speed drifts (41/39 mph at +5 s, 45/18 mph at +25 s after a port crash). Backgrounds and
the camera at the start gate match closely. Later moments compare the same place on the
track at the same race time, not identical frames.

### Round-3 proof: `build/port/compare/race-r10/index.html`

The port is the r10 build (r5 snapshot objects) plus the three sync modules. Its SHA-256
is in `scores.json`. Both sides reached the race clock values exactly (port: 121 for the
countdown's 120). The countdown lasted 260 fields in PCSX2 and 747 in the port. An earlier
port run took 560, so the port's vsyncs per game frame also vary between runs.

| Moment | Clock | HUD retail / port | PCSX2 delta | MAE | Over 16 | Block SSIM |
|---|---|---|---|---|---|---|
| countdown ("2" shown) | c+120 | 00:00 / 00:00 | +2 | 34.25 | 73.9% | 0.563 |
| start | g+30 | 00:00 / 00:00 | +3 | 16.68 | 38.6% | 0.571 |
| +10 s | g+600 | 00:09 / 00:09 | 0 | 21.53 | 57.1% | 0.420 |
| +30 s | g+1800 | 00:29 / 00:29 | 0 | 36.62 | 78.7% | 0.350 |
| +50 s | g+3000 | 00:49 / 00:48 | 0 | 39.81 | 85.8% | 0.346 |

The thumbnail MAE across the five PCSX2 candidates varies by under 1.3 at each moment,
so the window choice barely matters. The scores are dominated by scene differences, listed below.

Visible differences, largest first:

- **Countdown.** (1) The port's riders are see-through and look inside-out: their goggles
  and chests show through their backs, with parts missing. PCSX2 draws them solid, seen
  from behind. A depth-test or face-culling error on the rider meshes is the likely cause.
  (2) In this frame the port's sky is flat purple, with no cloud layer and no sun halo.
  The port worker's host-clock r10 frame has them, so this may be intermittent. (3) The
  start-gate floor has holes and scattered fragments along the bottom edge. The small blue
  railing feet are also in PCSX2, so they are not a bug. (4) A blue vertical strip runs
  along the right edge in the port only.
- **Start.** (1) The simulation has already diverged: the port's riders are scattered,
  one is mid-air at cable-car height, and the player is placed 4/10. In PCSX2 all ten
  riders are still at the gate. (2) The riders are see-through as above. (3) Part of the
  floor is missing at the bottom left (the background shows through), with blue specks there.
- **+10 s.** (1) The port's player rider isn't visible and the camera is elsewhere
  (33 against 39 mph). (2) The rock face on the right is drawn as large smeared slabs
  instead of PCSX2's detailed rock. (3) PCSX2 has a blue sponsor banner on the left; the
  port has only a small blue dot.
- **+30 s.** (1) The port is in a different place: a rock gully at 13 against 29 mph, with
  no rider visible. (2) A long grey translucent ribbon arcs across the port's sky. PCSX2
  has nothing like it; this is the "grey shape floating mid-air". (3) A small blue dot
  on the slope appears in both, so blue dots alone are not port errors.
- **+50 s.** (1) The port's rider is drawn as a large grey translucent jagged shape above its
  own shadow, at 19 against 46 mph. PCSX2 shows the rider solid on grass, plus a trick
  popup ("BLACK DIAMOND 389") and 371 points against 24. (2) The scene differs (open grass
  in PCSX2, snowy gully in the port), so most of the score here is position, not rendering.

Limits. The countdown and start are trustworthy rendering comparisons: same camera, same
background, same moment. From +10 s the riders' positions diverge, so later scores
mix physics divergence with rendering. The rendering-only observations there (rider not
solid, grey translucent shapes, smeared rock) hold regardless. Exact field lockstep is not
reachable, because the port and the PS2 spend different numbers of vsyncs per game frame.
The race clock makes the moments agree to within one game frame. Runs take about
40 minutes (PCSX2 software renderer and the port's virtual clock both run below real time).
Earlier runs are kept: `race-r10/vsync-keyed/` (the failed field-offset sync) and
`race-r10/validation-30hz-labels/` (clock sync with moments mislabelled at half the times).

## Round 2: independent retail boot and virtual clock (2026-10-13)

The default mode now boots the retail ISO independently in PCSX2, without manual
clicks. It references an existing BIOS in place, uses the same frame-quantized
digital/pressure input as the port, and captures numbered reference fields.
`--mode gs-replay` keeps the round-1 packet replay backend separately available.

Setup, after compiling the native game units:

```powershell
python port/tools/prepare_pcsx2_compare.py
python port/tools/link.py
python port/tools/compare_pcsx2.py LABEL --pad build/port/pad-script.txt --at 300 --interval 10 --until 310
```

The comparison builder downloads only public GPL PCSX2 sources/dependencies into
`build/port/compare-pcsx2-src/` and `build/port/compare-research/`. Source is pinned
to v2.8.2 commit `fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3`; the dependency archive
is SHA-256 pinned. It uses the installed Visual Studio/MSBuild and 7-Zip. The current
Windows dependencies need an FFmpeg format-query API compatibility adaptation;
that patch also stays in the downloaded build tree. Adapter wrapper code is GPL-3.0+.

Set `DHD_ISO` to the existing USA ISO. BIOS discovery checks the user's existing
Documents/PCSX2/bios directory for one ROM0, or accepts `DHD_PCSX2_BIOS` / `--bios`.
Generated per-run INIs reference that file's directory/name; no BIOS or ISO is
copied. No INI containing its path is tracked. The supplied USA v2.30 BIOS was
successfully loaded. The logs report that NVRAM was unchanged at shutdown.

`--exe` selects a port build. It is copied/hash-checked before launch. PCSX2's
public runtime is also copied into a private per-run directory so another adapter
rebuild cannot replace an active executable. Neither runtime snapshot includes
BIOS/ISO/ELF data. `build_compare_port.py` can privately snapshot an existing native
link's COFF inputs and rebuild the small clock/capture modules, avoiding shared
recomp/data regeneration. This does not repair already inconsistent worker objects.

`DHD_FRAME_CLOCK=1` is opt-in. The kernel, bus counters, alarms and SIF deadlines
share virtual bus cycles instead of QPC time. Active kernel polls advance a fixed
quarter-millisecond; the idle scheduler jumps to the next virtual interrupt/RPC
deadline. Vsyncs and pad input use the same clock. `timer.c` retains its existing
BUS/BUS16/BUS256 conversion and remainder logic. Fractional RPC deadlines round
up one bus cycle in virtual mode, preventing an idle loop stranded before a reply.
Diagnostics/profile rates use `host_wall_seconds()`; capture logs include both
wall and game time. Normal runs retain host-time behavior.

This is deterministic **HLE scheduling**, not EE instruction cycle accounting.
No assertion is made that its syscall/poll execution budget equals retail timing.
Movies are bypassed only in the port; retail input at the same numbered fields
can encounter different loading/menu states. Removing QPC dependence therefore
does not establish matching simulation state or a fully deterministic whole game.

PCSX2's adapter uses the unmodified `InputRecordingFile`/`PadData` reader for the
generated `.p2m2`, indexed by zero-based `g_FrameCount`. It overrides input after
source polling. The CPU carries that count in unused padding in the existing MTGS
VSync packet; the GS thread tags exports as `completed = g_FrameCount + 1`, the
same counting convention as the port. It never reads a racing live CPU counter
to label a screenshot. Captures are synchronous at that GS queue boundary.

The default software backend exports the completed PCSX2 CPU raster buffer,
cropped using its current DISPLAY/DISPFB registers, with current-field bob for
INT+FFMD. This avoids a stalled driver readback path encountered during bring-up.
The active circuit is selected as in the native scanout; general two-circuit
PMODE compositing is not validated in this software export. `--retail-renderer 3`
uses D3D11 and exports the completed merged display texture. Its export was also
validated. Both are independent retail runs, not replay of native packets.

Captures have exact boot-field identities; cross-implementation **game moments
are not proved equal**. Default image matching searches only ±2 reference fields
around each actual native capture count, using 64x56 RGB thumbnail MAE. JSON/HTML
retain the requested/selected fields, displacement, same-field MAE, raw/corrected
scores and original image sizes. `--match-window 0` enforces same-field selection;
`--reference-offset` declares an explicit boot-field offset. Searches never expand
silently. A size difference is explicitly recorded if nearest-neighbor resolution
normalization is needed. The final proof required no resolution normalization.

### Round-2 local proof

The fresh mixed worker cache hit a pre-existing event-queue callback failure at
boot. To isolate this round, a coherent baseline at `c7ae867b` was compiled under
`build/port/compare-baseline-stable/`, with only the new clock/capture modules and
the existing VU1 AOT option. All 3,797 units compiled; link retained 23 existing
SDK stubs and no unresolved symbols. Extracted asm/VU data used for that local
build remains ignored; the ISO, ELF and BIOS were never copied or moved.

Completed command (standard pad script is the default):

```powershell
python port/tools/compare_pcsx2.py round2-race-start --exe build/port/compare-baseline-stable/build/port/link/dhd-clock-final.exe --at 300 --interval 10 --until 310 --timeout 1800 --match-window 2
```

Output: `build/port/compare/round2-race-start/index.html` (triptychs, contact sheet
and full scores/provenance). Native fields 17,983 and 18,583 were captured at game
times 300.016549 and 310.026549 s, wall times 556.202571 and 563.517775 s. Selected
retail fields were 17,985 and 18,585 (both +2, within N=2; explicit boot offset 0).
Scores, respectively:

- MAE **39.331451**, **59.452183** / 255; raw MAE **39.369713**, **59.531075**.
- Pixels over threshold 16: **85.212615%**, **95.043345%**.
- Block SSIM **0.257401**, **0.240474**; vertical corrections **+1**, **-1**.

**The phase difference is visible and remains unresolved:** retail is racing
(HUD timers approximately 4 and 14 seconds), while native is still in the track
intro with the Mt. Midoule overlay. These are real independent race-period
comparisons, but not equivalent race-start states. N=2 cannot correct that large
phase difference. Scores mix simulation/camera timing with rendering differences
and must not be treated as a renderer-only fidelity gate. Exact game-state/race
start synchronization has **not** been achieved; this round exposes it rather
than hiding it through unrestricted image matching.

Earlier independent 250/260 s output is in `round2-race-quarter/index.html`: MAE
38.821922/45.644928, block SSIM 0.407141/0.287408; this pair likewise shows different
progress through selection/intro. Round 1's much lower packet-replay scores below
are a different test and do not contradict these independent-game differences.

Validation: eight synthetic Python tests pass, including movie layout/intervals,
bounded temporal selection and BIOS reference-only config generation. Native timer
and field-capture fixtures pass; the clock-disabled path and monotonic/reset behavior
are covered. The adapter builds successfully and automated software/hardware retail
boot/capture checks pass. No full new whole-game determinism claim is made.

## Round 1 implemented scope (historical)

`port/tools/compare_pcsx2.py` runs an isolated copy of the native executable,
records GS packet sequences and their native framebuffer captures, replays each
sequence in stock PCSX2 2.8.2's software renderer, and pairs the **same dump-local
field**. It writes triptych PNGs, a contact sheet, `index.html` and `scores.json`
under `build/port/compare/LABEL/`. Everything containing game content stays there.
The PCSX2 settings/data directory is private to each replay; the installed
settings, other workers' executables and other PCSX2 processes are untouched.

**This is a GS replay fallback, not the requested independent retail-game
lockstep.** PCSX2 receives the native run's GS packets rather than running the
pad script against its own EE/IOP. Camera, simulation, VU output, disc timing and
startup movies therefore cannot be assessed with these scores. The initial GS
freeze contains native-rendered VRAM; the first field is excluded and the last
field of a sequence is scored, but unchanged pixels can still inherit that VRAM.
A low score is bounded renderer evidence, never proof of full game fidelity.

The configured install's BIOS directory was empty and its BIOS filename unset
at inspection. A retail boot needs an existing legally supplied BIOS referenced
in place. No BIOS, ISO or ELF was copied, moved or downloaded for this task.

## Usage

Install the small Python dependencies if absent: `python -m pip install Pillow numpy`.
Build the game first (today `python port/tools/build_host64.py`; then `link.py`). Set `DHD_ISO` to the existing
USA disc image. The tool rejects an unset or nonexistent path; it never copies it.

```powershell
python port/tools/compare_pcsx2.py LABEL --mode gs-replay --pad build/port/pad-script.txt --at 250 --interval 3 --until 340
```

`--pad` accepts the existing `seconds:duration:button[+button],...` string or a
UTF-8 text file. Its default is the standard `protect-round.ps1` pad script.
Labels cannot contain path separators and existing output directories are never
overwritten. `--exe` selects a port build; its executable is copied and hash-checked
before launch so another worker's relink cannot replace a running image.
Frame mode also checks that this copied image contains the scheduling hooks and
fails if a concurrent relink supplied a legacy binary. Host mode accepts legacy
images; missing per-field metadata is represented as null, not estimated silently.

The default `--clock fields` converts seconds to NTSC fields at 60000/1001 Hz.
Interval boundaries use ceiling: a button is pressed on all zero-based fields
whose timestamp falls in the interval, including its start and excluding its end.
The same input is also serialized independently as a power-on `input.p2m2` file
for future retail replay integration. **Generating that file does not mean it
was played in PCSX2 by this runner.**

Small native additions:

- `kernel_vsync_count` counts delivered VBLANKs from boot and resets in `kernel_init`.
- `DHD_AUTOPAD_FRAMES` uses zero-based field indices instead of host seconds;
  the first delivered vsync samples input index 0. Existing `DHD_AUTOPAD` keeps
  its behavior unless the frame script is explicitly set.
- `DHD_CAPTURE_FRAME` / `DHD_CAPTURE_FRAME_INTERVAL` schedule presentation BMPs
  by the delivered-field count; the count is logged with each capture.
- `DHD_GS_CAPTURE_FRAME` / `DHD_GS_CAPTURE_FRAME_INTERVAL` do the same for dumps.
  `compare: dump` log records identify every local field, port count and host time.

The kernel still runs its timers from the host clock and discards overdue vsyncs.
Counting delivered fields removes host-speed differences from *input placement*;
it does **not** make the entire native simulation deterministic. A capture can
start late if a GIF packet is incomplete. The actual field/timestamp in the report
takes precedence over the requested schedule. The default 120-field sequence
compares its last field, about two NTSC seconds after its start when full speed
is sustained. Intervals are rounded up to an integer field count; long schedules
can accumulate sub-field rounding drift relative to literal second boundaries.

`--clock host` reproduces the old run-round input and capture timing. This is
useful for existing race-navigation scripts while slow builds drop many vsyncs.
Sequences may finish after their nominal interval, so later starts can be delayed;
the logged host time makes that explicit. `--until` is the last requested *start*,
not an execution deadline. `--timeout` is a separate wall-clock deadline, default
3600 seconds, and failed runs retain logs and a report with failed status.

To compare existing native sequences without another game run:

```powershell
python port/tools/compare_pcsx2.py REPLAY_LABEL --from-dumps build/port/race-fog2-gs
```

The directory must contain uncompressed native version-8 `.gs` dumps and their
`frame-NNN-native-NNN.bmp` captures. This is not a general PCSX2 dump reader.
`--fields 120` controls new sequences (2..600); its length must be shorter than
the requested interval. The final field is deliberately used instead of the
initial shared framebuffer. `--pcsx2` selects another executable, but the isolated
settings layout and raw-frame filename reader were validated only with 2.8.2.

## Interlace and scores

The reader uses each VSync's actual PMODE/DISPFB/DISPLAY/SMODE2 registers to choose
the active PCSX2 software framebuffer and its displayed crop. INT+FFMD samples
the current half-height field, then repeats lines with nearest-neighbor sampling,
matching the native current-field bob scanout in finding 021. It does not weave
different game moments or resize two unrelated screenshots until they fit.
Unexpected dimensions or framebuffer bases fail explicitly.

RGB MAE is in 0..255 units. A pixel exceeds `--threshold 16` when **any** channel
differs by more than 16. SSIM is a cheap mean of non-overlapping 8x8 luminance
windows, with population variances and the usual constants for a 255 range; it
is named `block_ssim`, not represented as standard Gaussian-window SSIM.
The five worst 64x64 regions have coordinates and individual MAEs in JSON;
three are marked on the heatmap. Alpha is excluded.

By default, scores also search vertical shifts -1, 0, +1, omitting the unmatched
edge rows. Both raw and corrected MAE/threshold percentages, chosen shift and
scored pixel count are retained. `--line-shift 0` disables this correction.
There is no temporal nearest-image matching: field identities come directly
from the packet sequence. A one-line minimum cannot conceal temporal mismatch
in an independent retail run, because there is no independent retail run here.

## PCSX2 research and remaining retail integration

Sources examined at the pinned public `v2.8.2` tag (PCSX2 Dev Team, GPL-3.0+):

- [InputRecordingFile.h](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/Recording/InputRecordingFile.h)
  and [InputRecordingFile.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/Recording/InputRecordingFile.cpp):
  version-1 movie header is 570 bytes; each field stores two 18-byte pads, active-low
  button flags, centered analogs and pressure bytes. The Python serializer is
  independently written from the wire layout; no upstream implementation copied.
- [InputRecording.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/Recording/InputRecording.cpp):
  power-on playback resets the VM, supplies controller values from its current
  movie frame, and pauses at the recording end. Save-state movies use an adjacent
  `*_SaveState.p2s` and store the initial emulator frame count.
- [Hotkeys.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/Hotkeys.cpp):
  `FrameAdvance` requests one VM frame. Finding 021's installed bindings expose
  F8 screenshot, Shift+F8 single GS dump and Ctrl+Shift+F8 multi-frame dump.
- [QtHost.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2-qt/QtHost.cpp):
  `-batch`, `-nogui`, `-fastboot`, `-statefile`, `-datapath` and `-logfile` exist;
  there is no CLI `.p2m2` playback or capture-at-frame option. The local help text
  file was empty, so the actual source's option parser was checked.
- [PINE.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/PINE.cpp):
  stock IPC supports memory access, metadata/status and state slots, but no
  pause, frame advance, controller injection or frame-exact screenshot command.
- [MainWindow.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2-qt/MainWindow.cpp):
  input playback is started through the Tools/Input Recording/Play file dialog.
  GUI hotkeys would need acknowledged pause/step control; wall-clock injection
  would not supply the requested guarantee.
- [GSRenderer.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/Common/GSRenderer.cpp)
  and [GSRendererSW.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/SW/GSRendererSW.cpp):
  F8 queues a later GS snapshot, whereas software `SaveFrame` output carries the
  actual GS field number and circuit. The latter is used for packet replay.

Downloaded public sources and all executable/output artifacts are confined to
`build/port/compare-research/` and `build/port/compare/`. No GPL emulator code is
linked into the native port by this change.

An independent retail runner remains open. A small GPL PCSX2 patch could expose
power-on movie playback plus a CPU/MTGS-fenced capture schedule keyed to the
recording frame counter. It would need a supplied BIOS, a fixed emulator build,
a common startup/movie policy and investigation of native loader/timer divergence.
Frame numbers alone would still not prove matching game moments. No patched
emulator was built here, and exact retail/native lockstep has **not been achieved**.

## Validation and local proof

Six synthetic Python tests cover movie layout/pressures/boundaries, malformed
scripts, GS field/register parsing and truncation, half-field bob crops, known
MAE/threshold/region scores, and correction that preserves raw scores.
`python port/tools/test_compare_pcsx2.py` passes. The native capture fixture passes
in legacy four/six-field mode and with field scheduling: field 9 does not capture
even though host time is past the old deadline; field 10 starts the sequence.
The port link succeeded with the existing 23 SDK stubs and no unresolved symbols.
An end-to-end frame-clock smoke run (`frame-schedule-smoke-2`) started at exactly
port field 300 and paired dump-local field 3 with port field 303. Its host time
was 34.603473 seconds, demonstrating that the schedule followed delivered fields
rather than five host seconds. MAE 0.091878, threshold percentage 0%, block SSIM
0.999156, shift 0. This small boot-screen check does not establish race determinism.

The broader runtime suite passed its native asm/VU/GS/VIF/controller/timer/SIF/
sound/WARP checks, then failed in the concurrent rider-time fixture compilation:
generated `001825B0.c` could not find `animation.h` through that test's include
paths. That unrelated shared-header fixture was left to its owner; the whole
runtime command is therefore **not** claimed to pass.

Automatic reuse check: `build/port/compare/replay-verify-2/index.html`, Mt. Midoule
intro waterfall, dump-local field 119: MAE **0.440627**, threshold percentage
**0.007847%**, block SSIM **0.999040**, chosen vertical shift **0**. The score and
the visible purple geometry agree on both renderers for these native-generated
packets; that does not establish that retail PCSX2 would generate those packets.

Fresh standard-script run:

```powershell
python port/tools/compare_pcsx2.py race-start-host --clock host --at 250 --interval 3 --until 259
```

Output: `build/port/compare/race-start-host/index.html`, four paired 120-field
sequences. Field 119 of the first shows SELECT LEVEL; the remaining three show
GET READY TO RACE with the current incorrect rider close-up. Their MAEs are
**0.642713, 0.368731, 0.368436, 0.368043**; all have **0%** of pixels exceeding
threshold 16 and vertical shift **0**. Final block SSIM: **0.998593**.
The final triptych is `moment-003-field-119.png`; contact sheet and full region
scores are beside it. The isolated executable's SHA-256 is in `scores.json`.

This fresh run used a concurrent worker's legacy relink: its input/capture
schedule was the explicitly selected host clock and it lacked the field metadata
hooks. Thus its port boot-vsync count and exact host capture time are null in
the report; dump-local field identity is still exact. The renderer is visibly
replaying the same incorrect upstream camera packets. This establishes automatic
race-start **GS replay** comparison, not independent retail race-start agreement.

## Round 4: game-state dumps, race seed and the depth row (2026-10-14)

`race_compare.py --dump` writes EE RAM (32 MB) then the scratchpad (16 KB) at every
moment: `retail-ee/ee-c+120.bin` from the patched PCSX2 (`DHD_COMPARE_RACE_DUMP`) and
`port-ee/ee-c+120.bin` from the port (`DHD_RACE_DUMP`). Pointers differ by
0x08000000 (the port stores host addresses); the port's stack lives on the host, so
EE stack regions don't compare. `DHD_COMPARE_RACE_GSDUMP=dir` makes the adapter
write an uncompressed PCSX2 GS dump of the two fields after each moment
(`gs-c+120.gs`); OpenCFile opens `\\?\` paths, so the adapter passes backslashes.
`build_compare_port.py --rebuild runtime/X.c --unit unsorted/X.c` rebuilds more host
sources and patched game units into the comparison image.

**Race seed.** func_0016B8C8 seeds srand and the Mersenne Twister at 0x00663140
with func_00238F10 = D_0029E3E0 + T0_COUNT, the EE time since a timer reset, and
stores it at 0x0043FA10. (func_00222E10 would use the RTC only for a zero seed.)
PCSX2 is deterministic and seeds 81257 with the standard navigation; the port seeded
12816 on every run. Brute-forcing the MT state in the dumps confirmed both values,
each two twists and 141 draws in. The different seed put riders 0, 1 and 3 in each
other's gate slots. The port now routes that seed through `hle_race_seed`
(port/patches.py, port/hle/autopad.c); `DHD_RACE_SEED` pins it and race_compare.py
passes 81257 by default. With the pin, all ten riders match retail to 0.01 at
countdown +120.

**Frame pacing.** The game steps physics with D_0029E3D0 = vsyncs since the last
frame, clamped to 1..2 (func_00238D98; dt = D_0029E400). PCSX2 runs the race at
delta 1 (60 fps, dt 1/60) at every moment; the port under `DHD_FRAME_CLOCK=1` runs
every frame at delta 2 (30 fps): each kernel boundary charges a synthetic 0.25 ms
and a race frame crosses enough of them to overrun a field. The animation clock
D_0029E160 still advances at the same rate on both (171 ticks from countdown +120 to
GO +30 in PCSX2, 172 in the port); it is only offset by 137 (retail's longer load
before rider setup), which shifts absolute animation start times (rider +0x3B38:
19 in the port, 157 in PCSX2) without changing relative times.

**Depth row.** In the countdown frame every port vertex had Z in 0xFFFBD7..0xFFFC00,
against 0x8F3..0x1EF2D9 in PCSX2's GS dump. Replaying the port's own GS capture in
PCSX2 (`compare_pcsx2.py --mode gs-replay --from-dumps`) gave MAE 0.0000, so the
port's rasteriser draws the stream exactly as PCSX2's GS does: the vertex data was
wrong. The projection matrices D_0075AA70/ABF0/AD70 match retail exactly; their
products with the view rotation (D_0075B1F0/AEF0/B070, func_0023CB00) lack element
[2][3] (16778240, the depth offset) because the port's view rotation has
[3][3] = 0. It is copied from the camera orientation (camera +0x40, EE 0x007A4FBC),
whose [3][3] is 0 in the port and 1 in PCSX2.

**Where the depth row breaks.** It is not only the camera: in the countdown dump, about 220
rotation matrices that are 4x4 with an identity last row in PCSX2 have [3][3] = 0 in the port
(an array at 0x00769830 with stride 0x40, which func_0023AE78 fills by copying, the camera
at 0x007A4F80 + 0x40, 0x0079AAC0, 0x0078CDC0, 0x0071A6A0 ...). Their 3x3 parts agree with
PCSX2. A hardware write watch on the camera's [3][3] (host 0x087A4FFC) during the race hits
only inside `run` in port/runtime/ps2asm.c: an `sq` of a GPR, so an inline asm block stores
row 3 as (0,0,0,0). The libvu0 unit-matrix block (func_00221F20: `qmfc2.i $2, $vf0` ...
`prot3w`) gives the right identity when run alone through the interpreter, so the zero
comes from another block, probably a copy (func_00209248 copies a matrix into camera +0x40)
of a matrix already wrong upstream. The next step is to log the template and caller at the
watch hit; the probe was stopped before the race when the round changed direction.

**Consequence for the renderer.** The riders' wrong depth is in the GS stream itself, and
PCSX2's GS draws the port's stream as the port's renderer does (MAE 0.0000 above), so a
different GS renderer cannot fix it.

**Running the comparison next to other load.** A game using 7 of 12 cores and most of the
GPU held the port to 2.5 fields/s, and three runs looked stuck on the memory-card screen
for that reason only: the virtual frame clock makes the boot deterministic (countdown at
field 20630 and GO at 21493 in every run with the default charge), so field-keyed presses
can't miss a screen. With above-normal priority (now the default, the relaunched child
inherits it) the same run does 16 fields/s. A heartbeat every 30 s of wall time reports
field, menu, race phase and clock; race_compare.py stops with "port stuck at <screen>"
after three unchanged beats. The menu global (0x0029DED8) is 0 on every boot screen up to
the main menu (title, memory card), then 15, 33, 1, 11, 18 through the front end and 9 in
the race, so state-keyed navigation can only start after the main menu. (Superseded: event-driven
navigation below tells the two menu-0 screens apart by retrying with another button.)

**Frame pacing (open).** Lowering the per-poll charge is the candidate fix for the 30 fps
race step; it also moves the boot screens to other fields, so it needs the navigation
re-checked. Experiments with 4608 cycles were stopped before the race.

**Event-driven navigation (2026-10-14).** The timed script pressed at fixed fields from 45 s to
355 s plus a Cross every 20 s, so both sides sat on menus for minutes (the port also reached
attract mode, which menu 0 enters after 3600 ticks without input). `port/hle/menu_nav.h` now
presses the next button as soon as the game shows the expected screen. The port (`DHD_NAV`,
autopad.c) and the PCSX2 adapter (`DHD_COMPARE_NAV`, the header pasted into its pad hook) run the
same file. A step is `menu[/phase][@address=value][+settle]:chord[|chord]`: wait for the menu
(0x0029DED8), race phase (0x0043F348) and EE word, settle 10 fields, hold 4 fields. The step is
done when any of the three changes. Otherwise it presses again after 30 fields, cycling through
the chords. race_compare.py's `NAV`:

    0:triangle|cross,15:cross,3:cross,16:cross,1:cross,11:cross,
    18@4C7F38=0:start,18@4C7F38=1:cross,9/5:cross

- Menu 0 is first the memory-card dialog (Cross = Retry, Triangle = Cancel), then the title, so
  its step alternates Triangle and Cross.
- Menu 18, the bike/jersey screen, ignores Cross until Start has set its confirmation: the word
  `D_004C7A30`+0x508 = 0x004C7F38 goes from 0 to 1. This was found with `DHD_NAV_DUMP=dir`, which
  writes EE RAM as each press starts and 29 fields later. An unconditional `18:cross` stayed there
  forever.
- 9/5 is the race load, which ends on a press.

Boot to GO, before → after:

| Side | Before | After |
|---|---|---|
| Port wall time | 657 s | 263 s |
| Port game time | 288 s | 53.6 s |
| PCSX2 wall time | about 105 s | 48 s |

The port's wall time is still bound by its speed; the user's game was running alongside.

The race seed (func_00238F10, above) follows menu timing, so the new path seeds 80728 in PCSX2.
To keep the reference dumps, the adapter now pins it the way the port does. With
`DHD_COMPARE_RACE_SEED`, it rewrites the call at 0x0016B910 (`jal func_00238F10; nop`) into
`lui/ori` of the value once the game is loaded, then clears the recompiler there.
race_compare.py passes the same seed to both sides.

Against the old timed reference (`retail-all`, seed 81257):
- PCSX2 with NAV is byte-identical in all ten rider structs, cameras included, at GO+0 and GO+2.
- Every rider position is exactly equal (error 0.0) at GO+12, 30, 60, 120, 300 and 600.
- The only remaining differences are a few words in the rider struct's tail: +0x77F8, +0x7808,
  +0x7A4C–7A58, +0x7B2C and +0x7B3C, probably sound handles and levels. They are 0 in every
  timed run and set after the NAV path, and they never reach positions.
- The port's GO+0/GO+2 dumps under NAV match its timed-navigation dumps except +0x535C in two
  riders. That word is 0 in retail and stale memory in both port runs, so it is already part of
  the GO+0 base the race comparisons subtract.
