# Data fixup audit and reference refresh (round 46)

Status: history

A round-46 audit on the 32-bit build. Its corrections are in the data conversion
(`port/tools/data.py`, which `build_wasm.py` runs); `audit_data_fixups.py` and the renderer gate
are gone (finding 088).

## Scope and reproducible inventory

`port/tools/audit_data_fixups.py` assembles the converter's image, checks every
label offset and the unchanged retail-image comparison, resolves every fixup
using the executable's map and absolute symbols, and emits a CSV containing the
EE address, ELF word, resolved host word, operand label, owning splat object,
triage signals, decision, and both independent RAM words. Generated assembly,
CSVs, ELF bytes and RAM stay in ignored `build/port/r46fixups/`.

The historical 4,419 entries are reconstructed with `--historical`, temporarily
removing only round 45/46 numeric schemas in this process. Round 41's viewport
schema remains enabled. Current main entering this task had **4,394** entries
(round 45 removed 25); this change leaves **4,348**. Every historical entry is
accounted for: 4,193 pointers, 98 unchanged-value entries, 59 exception markers,
and **69 incorrect relocations** (25 already fixed in round 45, **44 new**).
Two additional instruction entries are redundant identity fixups, removed by
the instruction schema without changing their bytes.

Inventory command (use the new clean map/RAM for the final inventory):

```text
python port/tools/audit_data_fixups.py --historical --out build/port/r46fixups/audit-original --map build/port/clean/830c9150/dhd.map --port-ram build/port/r45pickup/gate/exact/port-ee/ee-g+0.bin --retail-ram build/port/r45pickup/retail/retail-ee/ee-g+240.bin
```

The RAM captures are from independently booted retail PCSX2 and the round-45
clean port. Their race moments differ, so runtime equality alone cannot prove
pointer meaning. Initially 4,390 current entries still held their initialized
relocation in the port and their ELF value in retail. Four were mutated:
0x281930, 0x281940, 0x281944 are SDK allocator linked-list pointers (readers
00255A08/00255C70/002563B8); the two allocators differ by 16 bytes between
captures. 0x28214C is the heap cursor, with the expected RAM-base translation.
These are retained, not guessed to be numeric because of runtime differences.

## Classification evidence and corrections

All 44 newly changed words still equal their ELF values in independent retail
RAM. Their old host values are not valid translations of their *meaning*:

- **24 SDK instruction words:** the copied kernel patch images at
  [0x282230,0x2829D8), [0x2829F8,0x283160), [0x2831A0,0x283518).
  Adjacent words are branches, loads, COP0 operations and return delay slots.
  `func_0025F1B0` passes D_00282230 to the kernel-image copying helper.
  Aligned AND/MULT/etc. encodings had acquired RAM-base addition. The complete
  instruction regions now have a numeric schema; syscall descriptor tables
  between them retain native callbacks. The 26 removed entries include two
  harmless identity entries at 0x282484/0x28248C.
- **13 TLB record words:** D_00283560/D_00283630/D_00283750 are 16-byte
  `{PageMask, EntryHi, EntryLo0, EntryLo1}` records. `func_0025F6E0` loads all
  four words as syscall 0x56 arguments via 0025F690, never dereferencing them.
  The record arrays [0x283560,0x2837D0) are numeric; the three pointers in
  D_002837D0 remain relocated. In particular 0x283654's EntryHi was falsely
  resolved to **func_00180000**: checking only D_ labels would miss it.
- **Six text words:** 0x26FA6C (Japan within D_0026F9D0's rider records),
  0x270130 (the '?' rider field), 0x28C82C (retry popup tail),
  0x2935D0/0x2935D4 (NUL-separated names in D_002935C0), and 0x29460C
  (trick-help glyph string). Splitting text across words, embedded NULs and
  non-ASCII button glyphs defeat the old printable-previous-word heuristic.
  These specific proven text slots now preserve their bytes.
  `001DDE70.c` defines the sound-prefix block as `NoteList.names[10][3]`;
  retail 001DDFBC copies its bytes to a local before building VAG filenames.
- **One inflate extra-bit word:** 0x299530, in D_002994F8 (`cplext`), contains
  unsigned-short extra-bit counts / invalid-code sentinels. Its neighbors
  contain repeated small halfwords. `func_00240CB8` takes `unsigned short *e`
  and reads `e[*p-s]` as a number. The full 32-halfword table is numeric.

Only the pickup duration has a demonstrated visible symptom (finding 061).
Text corrections restore original bytes, but no claim is made of a verified
screen change in the affected rider/help/popup cases. The SDK images/TLB data
are preserved for correctness; the host uses HLE for those platform services.
The inflate sentinel correction has no established visual symptom.

The remaining data-address entries were grouped by owner and target, including
menu/string-pointer tables, the particle vector/matrix fields, BSS object
tables, SDK allocator lists, and TLB-table descriptors. Code-address entries
are callback, constructor/destructor, vtable and exception tables; the lone
numeric function-shaped entry is corrected above. Low values that resolve to
the identical word do not corrupt anything. A D_ label alone was never used
as proof: the false targets have labels in BSS too. Real BSS pointers retained
include the SDK buffers 0x281DAC/0x281DB4 (readers 00257358/00258258) and arena
pointer 0x29D99C (`00202E00.cpp` dereferences its +4 cursor).

`test_particle_timing.py` covers every new schema slot with synthetic
address-shaped words, the function-shaped TLB value, and adjacent real SDK,
table and arena pointers. Five tests pass. No matching source or checker
exclusion changes.

Research: the local retail instruction readers provide the primary evidence.
The [ps2sdk kernel declarations](https://github.com/ps2dev/ps2sdk/blob/master/ee/kernel/include/kernel.h)
were consulted for TLB/kernel interfaces, and the
[R5900 register definitions](https://github.com/ps2homebrew/sioshell/blob/master/r5900_regs.h)
for COP0 PageMask/EntryLo register identities. Initial GS-table search was
discarded after the reader established these were TLB records. No outside code
was copied or adapted.

## Clean build and frozen-reference audit

The per-word audit includes these new instruction addresses (hex EE addresses):
282288, 2822C8, 2822E8, 282308, 28231C, 282330, 282344, 282380,
282398, 2823B8, 2823CC, 2823E0, 2823F4, 2824A8, 2824C4, 2824C8,
282BB8, 282C18, 282D50, 282DC0, 282F78, 282FDC, 2830C0, 283418.

The TLB addresses are 283654, 283674, 283684, 283690, 283694, 2836A0,
2836B0, 283720, 283730, 283740, 2837A0, 2837B0, 2837C0.
The six text addresses and one inflate address are enumerated above.

Round 45's 25 timing addresses are 27D818, 27D8B8, 27D9A8, 27DB38,
27DB88, 27DBD8, 27DC28, 27DDB8, 27DE08, 27DEA8, 27DF98, 27DFE8, 27E088,
27E0D8, 27E128, 27E268, 27E2B8, 27E448, 27E498, 27E538, 27E5D8,
27E678, 27E6C8, 27E8A8, 27EA38. The four derived slope addresses
are 27E158, 27E478, 27E4C8, 27E608; initialization of the two timing pairs
27E498/27E5D8 is explained in finding 061. These, together with the 44 new
corrections, define the expected **73** differences. Full before/after values
are kept locally rather than duplicating instruction or game-data blocks here.

`port/tools/audit_fixup_gate.py` requires the exact set of differences in all
eight arm/checkpoint comparisons, using the existing gate equivalence rules,
and independently asserts that every corrected word equals retail RAM.

Clean build of final implementation revision **f0308534** (includes the concurrent
0.2.3 version bump): all 3,797 units compile, no errors; link succeeds with no
undefined symbols. `build/port/clean/f0308534/dhd.exe`, SHA-256
`de9a4f8a05dbaade357f42aed3f0ceb67546c9f9573c6a0b45dd17fc28317959`.
The earlier ece43567 clean build also passed, before the version bump.

The final executable's `final-gate-r40` run **FAILS only against the old reference**:
exactly 73 intended words in each of eight arm/checkpoint comparisons. The
auditor passes and confirms **every corrected word equals independent retail
RAM** at every checkpoint. Values/reasons are in
`build/port/r46fixups/final-gate-reference-differences.json`. No other early
differences exist under the unchanged gate's rules.

**Exact/native MATCH at all six moments**, including GO+60/+75 seconds.
**READBACK MATCH: 1,096 records, 1,096 Z reads, through virtual GS field 9017.**
The final inventory (`audit-final/fixups.csv`) resolves all 4,348 retained
entries using this executable's own map and early EE RAM: 4,344 unchanged
initialized relocations and the same four runtime-mutated allocator words
discussed above. The historical CSV remains `audit-original/fixups.csv`.

Recorded **build/port/compare/r46-plain** with the final clean executable:

```text
python port/tools/gate_native_renderer.py --exe build/port/clean/f0308534/dhd.exe --out build/port/r46fixups/record --reference build/port/compare/r46-plain --record --env DHD_SCALE=2
```

The recording run passes all six paired EE comparisons and **1,107 readbacks /
Z reads through GS field 9028**. Its manifest records the same executable hash
as above. A second retail audit of the recording's snapshots again proves
exactly the same 73 corrected words at all eight early comparisons, all equal
to independent retail RAM (`record-reference-differences.json`). No reference
files are tracked.

Final default-reference command (no `--record` or `--reference`):

```text
python port/tools/gate_native_renderer.py --exe build/port/clean/f0308534/dhd.exe --out build/port/r46fixups/gate --env DHD_SCALE=2
```

**NATIVE RENDERER GAME-STATE GATE MATCH** (exit 0, `gate/result.json` PASS).
All eight frozen-reference comparisons and all six paired EE comparisons
MATCH. **1,096 readback records / Z reads MATCH through virtual GS field
9017.** `r46-plain` is now the default. No exclusions or checks have changed;
the gate's only functional edit is its default reference path.

All generated inventories, fixup assembly, snapshots, readbacks and executables
stay under ignored `build/`. No proprietary data was staged. No FPS benchmarks, publication, release upload, matching
source changes or stash operations were performed. The subsequent commits
only document these results; f0308534 is the final implementation build.
