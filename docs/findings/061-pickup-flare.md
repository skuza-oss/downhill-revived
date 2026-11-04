# Pickup flare investigation (round 45)

Status: current
Checked: 2026-11-04

## Reproduction, before the fix

Baseline: clean build `00c53232`, current main's round-44 renderer. Local evidence is
under `build/port/r45pickup/`; no game data is tracked. All runs use the standard
`race_compare.NAV`, seed 81257, virtual field clock, no settings file, no movies,
no memory card, and small windows. No FPS benchmark was run.

- Standard `race_compare.RACE_PAD`: the port collects a yellow combat-upgrade pickup
  between GO+1400 and +1420 ticks. Native and exact both show an oversized yellow/orange
  flare in addition to yellow fragments. Native: `before-native/port/race-g+1440+0.bmp`;
  exact: `before-exact/port/race-g+1420+0.bmp`.
- PCSX2 software replay of the port's exact GS dump also shows the glow:
  `pcsx2-pickup/00155_f00001_fr1_00e00_C_24.png`. This isolates the symptom to the
  packets/data supplied to the GS, rather than native-only presentation. The replay
  framebuffer needs scanout correction and is not an independent retail pickup.
- The first independent retail run missed the pickup because the trajectories diverged.
  That run is **not** evidence of correct pickup rendering.
- Corrected independent retail input:
  `3:2:cross,23:2:cross,6:0.3:left,18:0.45:right,21.5:0.083333:right`.
  PCSX2 hits the same yellow pickup around GO+1350, displays `Combat Upgrade Kick`,
  and the sparse captures show yellow fragments without the glow (the brief initial
  flash is resolved by the consecutive-field check below). Captures:
  `retail-hit5/retail/field-006310.png` (+1360), `field-006320.png` (+1370),
  `field-006330.png` (+1380); contact sheet `retail-hit5-contact.png`.

The following trace identifies the game-side inputs that generate the flare.

## Root cause and correction

A hardware write breakpoint on the first source flare record's size word (host
0x087202A0) fires at race clock 1415, in `func_00228090 + 0x323` (diagnostic
`watch-pickup/port.log`, resolved against `probe.map`). The caller is recompiled
`func_0022E9A0`, retail PC 0x0022ECD4: particle flag +0x282C bit 24 requests a
flare, using position +0x2600, strength +0x27FC and size +0x27DC. This is a
particle flare, not an incorrect native-only depth-visibility answer.

The pickup emitter uses particle template index 16. `func_0022C468` copies its
0x50-byte scale record from `D_0027D300 + 16 * 0x50`. At +0x18 the two timing
halfwords are 0 and 120. Splat labeled the packed word at **0x0027D818** as
`D_00780000`. `port/tools/data.py` treated it as a RAM pointer and relocated it
to **0x08780000**, changing the duration halfword to **2168**. The live particle's
copy is at +0x27D8. Its normalized age increment (+0x2764) becomes
`1/(1 + 2168/2) = 0.000921659`, instead of retail's `1/(1 + 120/2) = 0.016393442`.

Actual pickup records (different hit times/slots, same template):

- Retail, GO+1360, slot 19: age 0.081967205, strength -3.6885185, flare flags 0.
- Port before fix, GO+1420, slot 20: age 0.0055299532, strength 24.156679,
  flare flags 0x01207080. Projected flare strength is 1.3078846, rising to
  2.0613267 at +1440. Thus the port keeps emitting a very bright flare after
  retail has cleared it. `func_0022C080` evaluates this envelope and clears
  +0x282C when strength <= 0.

The correction marks **+0x18 of all 77 scale records** as a numeric halfword
pair in the existing data converter's NUMBERS schema. It changes 25 wrongly
symbolized words in this table, not pointer handling elsewhere. Synthetic
regressions cover every timing slot and confirm identical bits in neighboring
fields still relocate as pointers (`python port/tools/test_particle_timing.py`: PASS).
No `src/`, GS rendering, readback, or matching-check code changes.

Clean-build visual verification and the unchanged r40-plain gate follow below.

## Draw trace and research

CPU GIF replay of the before exact capture (`trace_gs.py`, `pickup-draws.txt`, ignored)
identifies the large flare sprites in the second dump field. Example draw 16399:
PATH1, PRIM **0x156** (textured, alpha-blended, UV sprite),
TEX0 **0x2002DB8599306530**, ALPHA **0x2000000048**, TEST **0x7000D**,
FRAME **0x01080038**, ZBUF **0x101000000**, XYOFFSET **0x790800007000**.
Corners are (52533,45063) and (13112,22067), Z **0x0023768A**,
UV (9,9)/(1015,1015), RGBA **0x801F6F7F**. The companion sprite uses
Z 0x00FFFFFF. ALPHA is additive `Cs * As/128 + Cd` (the stored FIX 32 is unused); TEST enables
GREATER depth testing and alpha GREATER 0; Z24 writes are masked. These huge
sprite extents originate in the flare record's strength, rather than a missing
native depth test or a replacement texture. `func_002286F8` prepares the flare
packet through `func_00226588`, whose MSCAL is overlay A byte entry **0x26D8**
(finding 019). The record originates at `func_0022E9A0 -> func_00228090` and is
projected/read back by `func_00228328` before that packet is built.

Consulted upstream PCSX2
[GSRegs.h at 091ab68708a555198d80a7216965481c7ddd0d98](https://github.com/PCSX2/pcsx2/blob/091ab68708a555198d80a7216965481c7ddd0d98/pcsx2/GS/GSRegs.h)
for PRIM, ALPHA, TEST and ZBUF field definitions. The installed PCSX2 software
renderer was used both for independent retail play and GS-stream replay via the
existing comparison tools. PCSX2 is GPL-3.0-or-later; no implementation was copied.
The timing-field diagnosis comes from local retail data, retail RAM
captures, and the matching `func_0022C468`/`func_0022C080` sources and assembly.


A denser retail pass corrects the initial sparse-capture interpretation:
`retail-hit-dense/retail/field-006308.png` and `field-006309.png` **do** show a
bright orange flash. Fields 006307 and 006310 bracket it without that glow;
yellow fragments continue afterward (`retail-dense-contact.png`). This is a
**two-field retail flash**, not an effect that should be suppressed entirely.
The bug is its prolonged duration in the port. Sparse +10-tick captures missed
the legitimate brief flash; the consecutive-field reference is authoritative.


## Corrected image words

Every changed static word is a packed timing pair at scale-record +0x18. The
low halfword is unchanged; the upper halfword loses the erroneous 2048 added
by RAM-pointer rebasing. All other record fields retain their existing policy.

| EE address | Template | Before | Correct retail word |
| --- | ---: | --- | --- |
| 0x0027D818 | 16 | 0x08780000 | 0x00780000 |
| 0x0027D8B8 | 18 | 0x083C0000 | 0x003C0000 |
| 0x0027D9A8 | 21 | 0x083C0014 | 0x003C0014 |
| 0x0027DB38 | 26 | 0x085A0014 | 0x005A0014 |
| 0x0027DB88 | 27 | 0x085A0000 | 0x005A0000 |
| 0x0027DBD8 | 28 | 0x08320000 | 0x00320000 |
| 0x0027DC28 | 29 | 0x083C0000 | 0x003C0000 |
| 0x0027DDB8 | 34 | 0x08780014 | 0x00780014 |
| 0x0027DE08 | 35 | 0x086E0000 | 0x006E0000 |
| 0x0027DEA8 | 37 | 0x08280000 | 0x00280000 |
| 0x0027DF98 | 40 | 0x083C0014 | 0x003C0014 |
| 0x0027DFE8 | 41 | 0x085A0014 | 0x005A0014 |
| 0x0027E088 | 43 | 0x085A0014 | 0x005A0014 |
| 0x0027E0D8 | 44 | 0x086E0000 | 0x006E0000 |
| 0x0027E128 | 45 | 0x083C0000 | 0x003C0000 |
| 0x0027E268 | 49 | 0x083C0000 | 0x003C0000 |
| 0x0027E2B8 | 50 | 0x08280000 | 0x00280000 |
| 0x0027E448 | 55 | 0x083C0000 | 0x003C0000 |
| 0x0027E498 | 56 | 0x083C0000 | 0x003C0000 |
| 0x0027E538 | 58 | 0x08780000 | 0x00780000 |
| 0x0027E5D8 | 60 | 0x08780000 | 0x00780000 |
| 0x0027E678 | 62 | 0x083C003C | 0x003C003C |
| 0x0027E6C8 | 63 | 0x083C003C | 0x003C003C |
| 0x0027E8A8 | 69 | 0x08780014 | 0x00780014 |
| 0x0027EA38 | 74 | 0x08780000 | 0x00780000 |

## Clean-build visual check

`python port/tools/build_clean.py --tree build/port/clean/tree-r45pickup -j 6`
built revision **830c9150**, all 3,797 source units and 714 recompiled assembly-only
functions. Executable: `build/port/clean/830c9150/dhd.exe`, SHA-256 prefix
`a66cb7aec17004c5` (full hash in its manifest). This is a plain clean build,
without a frozen recompilation basis or diagnostic code.

Native 2x, original race pad/seed/NAV, same GO-relative captures:

- `before-after-g1440.png`: the sustained orange wash is gone; fragments and
  the race scene remain. Separate BMPs are in `before-native/port/` and
  `after-native/port/` with name `race-g+1440+0.bmp`.
- `after-native-contact.png`: a brief flash remains at +1416, with fragments
  and no large glow at +1418/+1420 onward, consistent with the dense retail pass.
- Real sun at +240: **entire 1024x896 RGB image identical** before/after,
  including the sun and its halo (`before-after-g240.png`).
- Non-pickup +960: 44 changed pixels out of 917,504, RGB MAE 0.000420707/255;
  the road, rider, HUD and scene remain visually unchanged
  (`before-after-g960.png`). These are picture comparisons, not FPS tests.
- Post-fix +1420 EE snapshot: pickup template 16 now has duration 120,
  normalized age step 0.0163934417, strength -3.6885185, flare flags 0;
  there is no pickup flare record. Before, the same moment had duration 2168,
  step 0.000921659, strength 24.156679 and active flare flags 0x01207080.

Exact-renderer and gate results follow below.


## Frozen-reference word audit

The four early exact-arm snapshots (+0/+2/+12/+16) have the **same 29 differences**
from r40-plain: the 25 timing words above and these four scale-table +0x48 slopes.
`func_00228BE8` initializes each slope as `(x28 - x2C) / x1A` for type 0x8003;
correcting x1A necessarily changes the result. Two timing words are also adjusted
by this initialization: 0x27E498 becomes 0x00440000 (duration 68), and 0x27E5D8
becomes 0x00800000 (duration 128), rather than their raw-image durations 60/120.
All 29 corrected snapshot words match the independent retail pickup RAM capture
at +1360. Full address/before/after records for each moment are retained in
`build/port/r45pickup/gate-reference-differences.json`. No checker exclusions change.

| EE address | Template | r40-plain slope bits | Corrected slope bits |
| --- | ---: | --- | --- |
| 0x0027E158 | 45 | 0x3DB99046 | 0x404BBBBC |
| 0x0027E478 | 55 | 0x3DADE7B6 | 0x403EEEEF |
| 0x0027E4C8 | 56 | 0x3DF7BDEF | 0x40880000 |
| 0x0027E608 | 60 | 0x3D5399FA | 0x3F6EEEEF |

The schema covers 77 slots. Of 28 symbolized timing words, three were already
preserved by the existing unaligned-value rule (0x27DD18, 0x27DD68, 0x27E7B8);
they do not change. Thus the fix changes 25 image words, not 28.


## Final gate result

Command (existing local ISO supplied in the environment):

```text
python port/tools/gate_native_renderer.py --exe build/port/clean/830c9150/dhd.exe --out build/port/r45pickup/gate --env DHD_SCALE=2
```

**Overall result: FAIL against the unchanged default r40-plain reference.**
Each arm differs from that reference in exactly the 29 audited words at each
of +0/+2/+12/+16; there are no additional early state differences. Every one
of those corrected words matches independent retail RAM. This is the necessary
game-visible data correction described above, not a renderer-only approximation.

**Exact versus native: MATCH** at all six checkpoints (+0/+2/+12/+16/+3600/+4500).
**READBACK MATCH: 1,128 records, 1,128 Z reads, through virtual GS field 9049.**
Native rendering was used (8,379 native presentations). The unchanged gate writes
`gate/result.json` with FAIL and the eight frozen-reference comparison failures;
`gate.log` preserves all six paired EE MATCH lines and the readback MATCH line.
No reference was re-recorded, and no comparison exclusions or matching checks
were modified. The baseline refresh is in finding 062.

The independent post-fix exact-renderer run also removes the prolonged flare:
`after-exact/port/race-g+1420+0.bmp`, `race-g+1440+0.bmp`; a brief initial flash
remains at +1418. Exact before/after: `before-after-exact-g1420.png`.
The exact sun crop (x=360..511, y=0..159) is RGB-identical; the whole exact +960
image differs in 454 pixels with RGB MAE 0.00794038/255. Native controls above
remain the shipped-path regression checks.

All captures, RAM/GS dumps, diagnostic binaries and scripts stay in ignored
`build/port/r45pickup/`. No FPS benchmark, publication, reference recording,
source matching change, or proprietary-data commit was performed.

Capture labels are requested race ticks, not a promise of identical scanout phase:
for example the native +1416 capture logged clock 1418 and the exact +1418
capture logged clock 1419. The sustained-glow before/after pair at +1440 logs
clock 1441 in both native runs. Adjacent requested tags can be omitted by the
existing one-capture-per-field helper; actual BMPs and capture clocks are in the
logs. The dense retail pass uses actual consecutive fields 6300-6315.
