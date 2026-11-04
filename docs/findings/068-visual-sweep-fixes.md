# Visual sweep corrections (round 51)

Status: current
Checked: 2026-11-04

Implementation branch `fix/sweep-visuals`, based on release 0.2.4 (`bc68ef08`). The scope is finding
063's loading card, Fog Editor slider, and Japan loading mosaic, in that order.

## Reproduction setup

Diagnostic logs stay under ignored `build/port/r68/`; game-derived captures stay outside the
repository in the artifact directory. The release clean executable `8a6f6e18` is the initial picture
baseline. An early-history boot capture starts at field 60, before the previously observed damage,
rather than interpreting the already-damaged field-125 freeze as evidence of its producer.

This task explicitly requires one game at a time, so that exemption is removed. Capture waits and
retries while any unowned game/emulator runs; only the launched process family may be stopped. No
simultaneous game runs are used to accelerate this task. Settings, widescreen, font and timing
choices remain recorded by the existing capture tools.

The implementation makes reasonable diagnostic choices without waiting for user answers. A candidate
will only be corrected after packet/state evidence and independent retail pictures establish the
difference.

## Transfer ordering regression

A recorder-only synthetic probe submits a black clear followed by a full-page red host upload. The
old native recorder emits IMPORT(red), DRAW(black), leaving black. The upload has incorrectly become
the initial content of the earlier draw. This establishes an actual ordering bug independently of
the still-pending game capture; it does not yet prove the loading card's cause.

The native recorder now closes pending draws before host upload data is written, and before a local
copy selects its source owner. The latter also prevents a newly drawn source, not yet claimed by the
recorder, from falling back to stale CPU pixels. An optional backend callback leaves the exact
renderer unchanged. Two synthetic offscreen D3D11 cases check the resulting pixels, with no game
assets: `python port/tools/test_native_transfers.py`. Both pass after the fix; the later
identical-stream replay establishes that this separate ordering bug does not cause the loading-card
defect.

Research: upstream [PCSX2
GSState.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GSState.cpp), `CheckWriteOverlap`,
`Write`, and `Move`, explicitly checks transfer/draw overlap and orders transfers with earlier
draws. Also consulted [ps2tek GS transfer documentation](https://psi-rockin.github.io/ps2tek/).
PCSX2 is GPL-3.0-or-later; no upstream implementation was copied or adapted. The probe and
correction were written from this port's recorder behavior.

The unchanged matching-source rebuild passes all eight retail sections:
`build/port/r68/retail-recheck2.log`, **MATCH**.

## Loading-card producer isolated

Fresh baseline captures completed for exact and native fields 130-170 and independent retail fields
930-970. The exact GS history begins at field 1; both port arms captured 156 fields, rather than
inheriting damaged native VRAM from the old field-125 freeze.

The CPU packet trace of that **exact** stream identifies high-byte font uploads: PSMT4HL, 512x512,
DBW 8, first DBP 0 and later DBP **128 blocks**. The latter covers physical pages **4-131**. Loading
scanout alternates page 56 and page 112, PSMCT24, 512x224 (56 pages per buffer). Thus it covers all
of buffer 56, and 20 pages of buffer 112: its first two 32-line page rows, followed by the left four
pages of the third row. After line doubling and scale 2 this predicts a 256-pixel-tall black strip,
then a 512x128 black block on the left. This is the stepped missing corner in the native captures.
The upload writes alpha bits 24-27 only; the 24 RGB bits must survive. The port's `page_writes`
invalidated the entire native colour owner and imported stale CPU RGB on the next claim.

The correction records per-pixel alpha values and bit masks on affected native colour pages, and
advances their owner version with the CPU write. A GPU pass merges those bits into a snapshot of the
scaled page, preserving every RGB sample, the other nibble and untouched pixels. CPU VRAM, its write
counters, the exact renderer and EE readbacks retain their original operations. Depth and 16-bit
owners retain their previous path; their physical layout is different. Draw/copy/field boundaries
end upload coalescing, preserving packet order.

All nine synthetic GPU cases pass, including sampling the stored alpha through a PSMT8H texture and
a synthetic CLUT: expected indices A3, 45, 7E, A0 and A5 are displayed as red values. This checks
both nibble formats, byte replacement, zero writes and untouched pixels, in addition to complete RGB
preservation. The original baseline fails the first two ordering cases in every output pixel; the
ordering correction passes both. Logs: `build/port/r68/test-transfers-*.log` and
`test-alpha-after.log`. Stream replay and fresh after-game pictures follow.

Additional research: [PCSX2
GSLocalMemory.h](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GSLocalMemory.h),
`WritePixel8H`, `WritePixel4HL`, `WritePixel4HH`, confirms masked high-byte writes preserve the
other bits of the 32-bit word. No upstream code was copied.

## Loading visual acceptance

The same field-1 exact stream was replayed through baseline native, ordering-only native, and the
alpha correction. All consume the same 156 fields. Baseline and ordering-only pictures are
RGB-identical throughout fields 130-149: ordering was a real separate bug, but **does not fix this
loading card**. The alpha correction restores the entire card. Odd fields repair exactly **327,680
pixels**, the 20 pages predicted above, with zero changes outside that stepped region. Even fields
restore the previously black card (711,494 changed nonblack pixels).
`evidence/loading-replay-metrics.json` records every field in this window.

PCSX2 software replay independently displays the complete card from those same packets:
`evidence/pcsx2-boot-stream/reference-field-130.png` and `...131.png`. Independent retail also
retains it throughout fields 930-970. A fresh untraced native boot of clean **b34e28c0** retains it
through fields 130-149, with the same later title transition as exact. All 41 fields per arm were
reviewed in `{native-before,native-after,exact,retail}-boot-review.jpg`, then the fixed card at
larger size in `loading-fixed-review.jpg`. A review PNG made while a replay file was still being
written was incomplete; only completed originals and the subsequent full-image review are acceptance
evidence.

Before/after originals: `evidence/before/native/shot-f000130.bmp`, `...131.bmp`;
`evidence/after-boot/shot-f000130.bmp`, `...131.bmp`. The offline harnesses and wire dumps also
remain there, untracked.

Clean build b34e28c0: **3,797/3,797 units, zero errors**, successful link, no undefined symbols.
`build/port/clean/b34e28c0/dhd.exe`, SHA-256
`abc9f726e2c65a4976aaf21aad69385fdab1e45307e9e31082c8a0f390c17afd`. Final unchanged r46-plain gate
and delivery results are recorded below. The additional game-shaped two-buffer regression also
passes: a 512x512 PSMT4HL upload at block 128 preserves all of page-56's buffer and page-112's
corner. All **13** synthetic pixel cases pass (`test-alpha-pages.log`).

## Fog Editor: expected retail grey, no correction

Fresh sequential retail/native/exact runs enter the editor through the built-in unlock input, then
visit RANGE, VISIBILITY, RED, edit RED with Right, and visit GREEN and BLUE. All six rows in all
three arms were reviewed at panel size in `evidence/fog-probe/rows-review.jpg`; the RANGE bar is
enlarged separately in `evidence/fog-range-comparison.jpg`. Every bar remains grey in retail too.
The red cursor changes position with the selected value. Different aspect/font and background poses
are the existing intended presentation/timing differences.

The source is now resolved: matching `src/unsorted/00167370.c` constructs the Fog Editor and calls
`func_00191078(..., 190, 142, 144, 2)`. That matching helper, `src/unsorted/00191078.c`, explicitly
writes RGB **(90,90,90)** for the top two vertices and **(150,150,150)** for the bottom two. It also
draws the tick marks. This is a two-pixel Gouraud grey bar, not a missing colour texture or fog
tint.

The exact arm's field-5700 two-field GS stream proves the packet values. The second field's draws
38145/38146 are PATH3 untextured Gouraud triangles, PRIM **0xC**, RGBA **0x805A5A5A** /
**0x80969696**. Widescreen maps its original 144-pixel width to 108 GS pixels (raw x 31984..33712
with OFX 0x7000). `build/port/r68/fog-slider-draws.txt` retains the trace. PCSX2 software replay
shows the same grey bar (`evidence/pcsx2-fog-stream/fog-reference-1.png`).

Representative originals: retail `fog-probe/retail/retail/field-007350.png`, native
`fog-probe/native/shot-190.bmp`, exact `fog-probe/exact/gs/frame-000-native-001.bmp`, all beneath
`evidence/`. The old baseline has the same appearance; there is no justified before/after colour
change. Finding 063's unresolved grey-fill candidate is **not a bug**.

## Japan NEW LEVEL: same high-byte ownership bug, corrected

Fresh baseline exact/native and corrected-native runs capture **every field 6300-6600**, 301
pictures per arm. Read-only end-of-window RAM proves selected track **12** in each
(`japan/<arm>/state-before-stop.json`). Exact retains the Japan loading card. Baseline native starts
showing Italy remnants at **6408** and continues alternating tiled/partly missing old imagery until
the flyby. Corrected native retains the Japan card through that boundary.

The exact arm records a **220-field GS stream starting at 6300**, before the
damage, with its complete exact VRAM freeze. Packet trace
`build/port/r68/japan-high-byte-transfers.json` identifies PSMT4HL 512x512 uploads
at **6354 (DBP 0)** and **6408 (DBP 128)**, plus two 64x64 PSMT8H uploads at
6408. The second font upload coincides exactly with the mosaic onset. Native's
whole-page invalidation replaces newly drawn Japan RGB with old CPU-visible
Italy RGB. This is the boot-card defect's same storage/ownership error, not a
Japan asset, VU program, level-selection or simulation correction.

Replaying this exact stream with baseline native loses the card in favor of old Italy imagery;
replaying it after the fix retains the card. The initial exact freeze contains complete Italy RGB,
so the baseline replay's stale-image pattern need not equal the live native mosaic pixel for pixel.
PCSX2 software replay retains the Japan card too. Referee:
`evidence/japan/pcsx2-stream/reference-6420.png`; replay before/after:
`evidence/japan/replay-{before,after}/field-120.ppm` (global field 6420).

Independent retail supplies **617** pictures from the initial request 7900-8550. It emits none for
**7978-8011** around the display/loading reinitialization. The existing strict capture helper
correctly does **not** report that broader request complete. After preserving it, only owned
emulator PID 56812 was stopped. Every PNG and completion record in the relevant **8012-8550** window
is checked: **539 consecutive pictures, no missing images**, plus read-only track ID 12.
`evidence/japan/retail-window-result.json` records both the incomplete initial request and this
complete window. No missing fields are fabricated, no helper or comparison check is relaxed, and no
parity claim covers those 34 fields.

Reviewed every field of the candidate window 6400-6475 for all three port arms, and retail
8012-8090, in `japan/{exact,native,after,retail}-dense-review.jpg`. The larger
`transition-fixed-review.jpg` compares retail/exact/before/after. Retail, exact and corrected native
show the same complete Japan artwork and labels; the baseline native alone shows Italy's rider/level
text in tiles. Before/after originals at the defect: `evidence/japan/native/shot-f006420.bmp` and
`evidence/japan/after/shot-f006420.bmp`. No additional runtime change was needed after b34e28c0.

## Final gate and delivery

The unchanged `port/tools/gate_native_renderer.py`, default **r46-plain** reference, on clean
runtime b34e28c0 is **MATCH**. Eight early EE comparisons against the frozen reference and six
exact/native EE comparisons all match. **1,114 readbacks, all Z reads, match through GS field
9,035**, beyond both final race captures at field 7,715. `failures` is empty. Native presents 8,714
frames in the final reported profile, proving the requested backend ran. Native uses scale 2;
neither gate arm enables widescreen. All six local reference dumps hash-identically to the original
r46-plain files. No reference was recorded, and no checker, exclusion or tolerance was changed.

Run data: `build/port/r68/gate/{method,result}.json`, the two complete arm directories, and
`build/port/r68/gate.log`. The final attempt completes both arms and the original comparator. The
unchanged matching-source check still reports all eight ELF sections **MATCH**, and all 13 synthetic
GPU pixel cases **PASS**.

Later commits extend tests and document evidence only; they do not change runtime source. The
launcher disables updates and settings-file loading for this development build. `README.txt`,
licence/notices, `gate-result.json`, `gate.log`, and the build/matching/synthetic validation logs
accompany it. No ISO or extracted game assets are included in the launcher package; local captures
remain in its separate `evidence/` directory.

Working commits: `631c820e` exclusive capture guard, `d36e1f2a` draw/transfer ordering, `b34e28c0`
high-byte RGB preservation, `d17d3aa4` loading acceptance and two-buffer regression, `46b3990f` Fog
Editor retail proof, `a76e8df4` Japan acceptance.