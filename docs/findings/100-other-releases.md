# 100 — Other releases: what supporting the European disc (and others) would take

Status: current
Checked: 2026-11-04

## In short (for players)

- **Only the USA disc works:** *Downhill Domination*, SCUS-97177. Dump your own disc to an ISO
  with your usual PS2 dumping tool.
- **Why not the European disc (SLES-52202)?** It is not just a translation. It is a different
  build of the game: made for 50 Hz PAL TVs, with five languages and changed tracks. Supporting
  it means building and maintaining a second game core, about 200 hours of work.
- **The Japanese release** (*MT Bikers*, SCPS-15062) is another different build again.
- **Not planned for now.** Please don't open new issues asking for it: this page is the answer.

## Technical details

Everything below is the research behind that answer: measurements and estimates, not a plan.

Date: 2026-10-27. Research only: no port code changed.

Question: Downhill Revived runs the USA release (SCUS-97177, `SCUS_971.77`). How hard is it to
support other releases, starting with the European one (SLES-52202, `SLES_522.02`)?

Short answer: the European ELF is the same source code built again six months later, for PAL (50 Hz,
640x512) and five languages. Its track and asset files were revised too. 84% of the USA functions
reappear in it unchanged once addresses are masked. The recommended route is a second game core for
the European ELF. It reuses the decomp's C for every unchanged function and recompiles the rest from
the European asm with `recomp.py`, as the port already does for functions that are still asm. The 50
Hz constants go back to their 60 Hz values. Estimate: **180-290 hours**. Section 8 has the
approaches and section 9 the images worth asking for.

## Tool

`port/tools/release_compare.py USA.iso OTHER.iso [files|elf|code|data] [--asm asm/nonmatchings]`
reads both images in place (its own ISO 9660 walk; nothing is extracted) and prints only statistics:
file lists and content hashes per folder, ELF headers, the function comparison and the data
comparison below. `code` needs the function list splat generates from your own `SCUS_971.77`
(`asm/nonmatchings`). Runs: `files` 7 s, `code` 2 s, `data` 60 s.

## 1. The disc

| | USA | Europe |
|---|---|---|
| SYSTEM.CNF | BOOT2 `SCUS_971.77`, VER 1.00, VMODE NTSC | BOOT2 `SLES_522.02`, VER 1.01, VMODE PAL |
| boot ELF | 1,691,684 bytes, file date 2003-06-13 | 1,864,236 bytes, file date 2003-12-16 |
| files | 2,338 (2,522,611,561 bytes) | 2,385 (2,762,705,805 bytes) |
| image SHA-256 | `0fe2b777…a7eb86` (redump 30393) | `abb5f5d9…e36a38` |

The European image's MD5 (`ef00557f…`) and CRC-32 (`712a1e09`) are the ones commonly listed for the
redump-named "Downhill Domination (Europe) (En,Fr,De,Es,It)". Redump's own page (disc 4510:
SLES-52202 / SLES-52202-GER, EXE date 2003-12-16, version 1.01) refused connections during this
research, so its hashes were not compared directly.

Files present on both discs (2,336 names), compared by content: the game's `IE` container is a zip
local header with `IE` for `PK`, holding dates and a CRC. Where only that wrapper differs, the
content is the same:

| folder | same | same content, new wrapper | content changed, same size | content changed | only USA | only EU |
|---|---|---|---|---|---|---|
| (root) | 1 | | | 1 (SYSTEM.CNF) | 1 (ELF) | 1 (ELF) |
| BIKE | 5 | 198 | 70 | 8 | | 4 (`MANDORE.*`) |
| LOADBAR | 126 | | 111 | 75 | | |
| LVL | | 3 | 3 | 210 | | |
| MEMCRD, SKAT, RST | 75 | | | | | |
| MOD | 10 | | | | 1 (`SNPROFIL.IRX`) | |
| MOV | 4 | | | 22 | | 12 |
| PTS | 413 | | 2 | | | |
| R (riders) | 68 | 690 | 22 | | | |
| REP | 30 | | 1 (`GLACIERX.ORB`) | | | |
| SHELL | 2 | | 2 | 6 | | 32 |
| SND, VAG | 178 | | | | | |
| total | 912 | 891 | 211 | 322 | 2 | 49 |

- **Sound is identical.** Every `SND/` bank, `VAG/` stream, `SKAT/` file and the IOP modules
  (`MOD/`, including `LIBSD.IRX`, which the reverb presets come from: finding 053) are byte for
  byte the same. There is no dubbing: the speech is English in every language.
- **Languages are separate files, picked by name.** `SHELL/UI{F,G,I,S}.*` (menu ring) and
  `SHELL/BIKESHP{F,G,I,S}.*` (bike shop) exist next to the English `UI.*`/`BIKESHOP.*`. The
  European ELF names the `BIKESHP?` variants. Loading screens keep their names, but their `.RTX`
  doubles (133,488 to 265,584 bytes of content).
- **Movies are re-encoded at 25 pictures/s** (all 640x368, as on the USA disc). There are new
  per-language legal movies (`{EN,FR,GE,IT,SP}COPYRT.PSS`, `{..}SCEAPR.PSS`) and
  `CODEMAST.PSS` and `REVIEW.PSS`. `BIO.PSS`, `DHCOPYRT.PSS`, `DHSCEAPR.PSS` and `LOADING.PSS`
  are unchanged.
- **The tracks were revised.** All 54 levels differ in content. The USA level-format reader parses
  all 54 European levels with no failure, so the formats are the same. Their counts differ, though:
  - collision triangles differ in 50 levels (all but the podiums and the trainer), by up to 1.1%
    (-340 to +26 triangles);
  - render triangles differ in 51, instances in 49;
  - every level has 1-9 more textures (the finish banner and other text in the four new
    languages);
  - each of the five levels checked grows its `.RTX` by the same 134,272 bytes and its `.TEX` by
    91,904 (the same additions in each);
  - `.PTR` files share only 6-10% of their 32-byte chunks with the USA ones (rebuilt offsets).
  - Bikes: every bike `.TEX` has the same 3,078 changed bytes (one shared texture), `MAN100`
    shrank and `MANDORE` is new.

## 2. Code: same source, built again for PAL

ELF layout (one PT_LOAD at 0x10A000 in both):

| section | USA | Europe |
|---|---|---|
| .text | 0x10A000-0x263BC0 (1,416,128) | 0x10A000-0x269D88 (1,441,160, +1.8%) |
| .vutext | 0x263BC0, 41,280 | 0x269D90, 41,280 |
| .data | 0x26DD00, 109,424 | 0x273F00, 133,256 |
| .rodata | 0x288880, 79,576 | 0x294800, 198,648 (2.5x) |
| .gcc_except_table | 328 | 328 |
| .sdata | 0x29C100, 7,152 | 0x2C5180, 11,632 |
| .sbss + .bss | to 0x8A0400 | to 0x8B2740 |

Each of the decomp's 5,689 functions was looked up in the European `.text`. The USA function's
instructions were compared with address fields masked: j/jal targets, `lui` values in the address
range 0x0010-0x0090, the `%lo` half of those pairs, and `$gp` offsets. The neighbours' shift was
preferred, so function order is kept:

| region | functions | identical bytes | same after masking | only immediates differ | changed | not found |
|---|---|---|---|---|---|---|
| game | 3,534 | 648 | 2,153 | 451 | 231 | 51 |
| engine | 1,202 | 322 | 748 | 84 | 34 | 14 |
| sdk | 801 | 394 | 364 | 37 | 6 | 0 |
| libgcc + crt0 | 152 | 25 | 125 | 2 | 0 | 0 |
| **all** | **5,689** | **1,389** (95 KB) | **3,390** (836 KB) | **574** (294 KB) | **271** (153 KB) | **65** (27 KB) |

- 4,779 functions (84%, 931 KB of 1,405 KB) are the same code at a new address. 4,149 of them sit
  in 55 long runs with one shift each: the link order is unchanged (+0x0 at the start, growing
  to +0x6180 at the end of .text). The SDK libraries sit in the same place and are the same
  version (a 2.4 library scan finds the same 53 functions in both).
- **"Only immediates differ" (574):** the same instructions and registers. By what changed:
  - Struct offsets in 110 functions (269 immediates: +4, +8 and +0x140 the most common). A few
    structs gained members. One of them is a size used as 72 in the USA build and 76 in the EU
    build, in 20 functions. The rider struct is still 0x7BD0 bytes (finding 040;
    `port/hle/puppet.c`).
  - Screen layout for 512 lines: 448→512 (`func_0020B938`), 224→256 and 512→640
    (`func_00228BE8`), and menu coordinates scaled 1.14-1.25 (`func_001F5828`, `func_001A2EF8`,
    `func_001A46A0`). The European build draws 640x512.
  - About 240 functions differ only in `%lo` halves the simple `lui` tracker misses: these are
    really the same as the masked group, which makes about 88% in all.
- **Changed (271, 153 KB):** instructions inserted or removed, mostly in game code. The menus,
  text and options code at 0x1E0000-0x1F8000 and 0x18-0x19xxxx changed the most:
  `func_001F24B8`, `func_001EB4D0`, `func_001F0A28`, `func_001F0160` and `func_001EE8A8` each gained
  a lot of code. The five-language text handling and the language option are here (the
  European ELF holds that option's name in English and French; the USA ELF has neither).
- **Not found (65, 27 KB):** rewritten beyond recognition or removed.
- **New in Europe:** 65,772 bytes of `.text` (4.6%) match no USA function, in 499 runs, the
  largest 3.3 KB.
- `.vutext`: 99.8% of its words in order. One VU1 microprogram is 8 bytes longer (the overlay table
  shows the size change), which shifts what follows: 1,326 words differ at the same address.

Conclusion: not a patched USA binary and not a separate code base. It is the same tree with a
PAL/frame-rate configuration, a five-language text system and fixes, compiled again with the same
toolchain.

## 3. Data: what a European player's ELF provides

The port reads the USA data image (0x263BC0-0x29DCF0: .vutext, .data, .rodata, .gcc_except_table,
.sdata) from the player's `SCUS_971.77`. It checks size and SHA-1 and writes 4,348 words over it
(`fixups`: 2,437 code pointers that become wasm table indices, 1,911 other values; 4,159 of them in
.data). A European player has no `SCUS_971.77`.

USA sections against the European ones, in order (pointer-like words compared as one token):

| section | USA → EU bytes | USA words found in order | in runs of 16+ words |
|---|---|---|---|
| .vutext | 41,280 → 41,280 | 99.8% | 99.5% |
| .data | 109,424 → 133,256 | 92.5% | 89.5% |
| .rodata | 79,576 → 198,648 | 87.4% | 83.3% |
| .gcc_except_table | 328 → 328 | 100% | 100% |
| .sdata | 7,152 → 11,632 | 86.4% | 74.4% |

- Strings of 4+ characters in .rodata: 1,998 (USA) against 7,834 (EU). The European text is
  there in five languages.
- What the USA image has that the European ELF does not: 4,797 words (19 KB) are not found in
  order. Of these, 2,140 are inside strings, 1,136 are zero fill and 102 are pointers. Only 149
  USA strings (2.6 KB) are absent from the whole European ELF; the rest moved.
- So the USA image can't be rebuilt from a European ELF alone. About 92% of it is there,
  rearranged, but the rest would have to be shipped (retail bytes in the exe: not allowed,
  finding 053) or substituted object by object.
- A European core uses the European image as is: same mechanism, its own SHA-1, its own address
  range (0x269D90-0x2C7EF0, 385,376 bytes) and its own fixups, generated by `data.py` from a
  European data label map.

## 4. Video and timing

| | USA (what the port assumes) | European build |
|---|---|---|
| field rate | 59.94 Hz: `kernel.c` VSYNC_CYCLES `BUS_HZ * 1001 / 60000`, `host.c` pacing `1001/60000`, `fps.c` | 50 Hz, with game constants rescaled (section 2) |
| sound per field | 4004/5 samples at 48 kHz (`sound.c`, `kernel.c`) | 960 |
| frame buffer | 640x448 (present.c fallback 448, HUD anchors of finding 056, movie centring DY 90 in 448) | 640x512 |
| movies | frame period from the stream (`movie.c`; 29.97/30 so far) | 25 pictures/s: handled as is |

- The port's 60/120/144/240 modes (findings 049, 067) step the USA logic at its 60 Hz units.
- A European core can do the same once its rescaled constants have their USA values back.
  `release_compare.py code` lists every one with both values; the values are plain numbers, not
  game data. That core then runs the European game at 60 Hz with the USA physics timing.
- The alternative is an authentic 50 Hz mode: pacing, the mixer's samples per field, the online
  clock (`online.c` uses 59.94) and the rate scaling of `fps.c` all made per release. Possible,
  but every timing path gets a second value.
- 512 lines: `gpu_rust_native.inc` and `present.c` size the picture from the GS DISPLAY
  registers (448 only as a fallback). The HUD anchoring and widescreen code (finding 056,
  `patches_widescreen.py`, 17 units), the movie centring and the port's own menu layer assume
  448 and need a 512 case. The other choice, patching the layout constants back to 448, would
  touch the scaled menu coordinates in several functions. Supporting 512 is the smaller and
  cleaner change.

## 5. Languages

- The European build has a language option in its own menus and five text sets in .rodata. The
  shell loads `UI*`/`BIKESHP*` by language letter; loading screens, level textures and legal
  movies come in per-language variants.
- Kernel `GetOsdConfigParam` (where libscf reads the console's language) has a stub in both ELFs,
  reached only from SDK code. Whether the first boot defaults to the console's language could
  not be settled statically. Checking it takes one boot with the language changed in the PCSX2
  BIOS settings. For the port that means `GetOsdConfigParam`'s language field in the HLE kernel,
  fed from the launcher's settings.
- Fonts: the in-level `.RTX` grows by the same 134,272 bytes in each level checked, and the UI's `.RTX` by
  132,096. That fits extra glyph pages for accented letters. The sharp font (finding 058) replaces
  the game's glyphs from its own table and would need those letters.

## 6. What changes around the core

| | today | with a European core |
|---|---|---|
| launcher `disc.rs` and `host.c disc_problem` | SYSTEM.CNF must name `SCUS_971.77`; the European (`SLES_522.02`) and Japanese (`SCPS_150.62`) discs are refused by name, from one table both read (`port/hle/other_releases.inc`, finding 093), with a link here | the same table with ELF size and SHA-1 per release, and the matching release accepted |
| `game_data.c` | one name, size, SHA-1, range | the same per release; the core build says which one it needs |
| online, client (`online_menu.c`) | only the USA image's SHA-256 | a table of known images; the server already refuses a guest whose `isoSha256` differs from the host's (`iso_mismatch`), so rooms stay per release |
| cross-region races | — | possible later at 60 Hz (same rider size, state sync), but 50 of 54 tracks have different collision, so a US and an EU rider would not race on identical courses. Keep rooms per release first |
| texture packs (`texpack.c`) | folder `textures\SCUS-97177\replacements` | also `SLES-52202`. PCSX2 names files by content hash: textures that didn't change (most level textures: 99.7% of a level's `.TEX` content found) keep working; the changed bike texture, loading screens and UI need their own entries |
| memory card (`mc.c`) | `saves\card1\BASCUS-97177DHD\…` | the European code writes `BESLES-52202…` by itself. Saves are separate; a converter would need the save layouts compared first |
| widescreen | patches on USA units | shared where the unit is unchanged; the menu units are among the changed ones |
| players | English, 60 Hz | expect their language; 60 Hz is what PC players want (an optional 50 Hz mode for purists) |

The port's code refers to 562 USA symbols (403 functions, 159 data) in `port/hle`, `port/gs`,
`port/host`, `port/runtime`, `port/net` and the patch files. Of those 403 functions, 313 are the
same in Europe, 48 differ only in immediates and 42 changed or weren't found. The 42 are mostly the
menus, options and text, where the PC SETTINGS, unlock and exit patches sit.

## 7. Other releases

| release | serial | date | notes |
|---|---|---|---|
| USA | SCUS-97177, v1.00 | 2003-07 | supported (redump 30393) |
| Europe / Australasia | SLES-52202 (and SLES-52202-GER cover), v1.01 | 2004-02 | Codemasters; this finding (redump 4510) |
| Japan: MT Bikers: Bakusou Mountain Bikers | SCPS-15062 | 2004-05-20 | SCEI; a later build; Japanese text; one rider replaced (fandom wiki); PCSX2 wiki also lists SCAJ-20083 for Asia |
| USA demo | SCUS-97329 | 2003 | here (`iso/demo_us`, `SCUS_973.29`, NTSC) |
| Europe demo | SLED-52325 | 2003-12 | here (`iso/demo_eu`, `SLED_523.25`, PAL); a layout close to the retail EU ELF (unverified: the note this cited, number 011, is not in this repository) |
| prototype, 2003-05-23 | (SCUS_971.77 loader + overlays) | 2003-05 | here (`iso/proto0523`) |

No Greatest Hits or Platinum reissue and no second USA revision were found (redump, PSXDataCenter,
GameFAQs, PCSX2 wiki). A Korean fan translation exists as a patch for the USA disc; patched images
fail the SHA-1 check, as they should.

## 8. Approaches

**A. Run the USA core with the European disc (one core).** The core needs the USA data image at
USA addresses, which a European player doesn't have.
- About 92% of it can be rebuilt from `SLES_522.02` object by object, using a data map.
- The rest (19 KB: 149 USA-only strings, about 1,400 changed words) has no legal source. Each
  object would need a European stand-in: the English text from the European tables, the numbers
  from the changed structs.
- The USA code would then load the European level files: same formats, revised content.
- Result: English only, without the European fixes, an untested combination of code and
  assets, and 19 KB of hand-made substitutes.
- Effort 40-70 hours (map tool 10-15, residual objects 20-40, disc check 3-5, level
  tests 6-10). Not recommended: it gives European players nothing they'd ask for.

**B. A second matching decompilation of `SLES_522.02`.**
- Splat config and symbols from the matcher: 10-15 h.
- Shared sources with a region macro for the 574 immediate-only functions (frame counts, struct
  offsets, screen sizes): 60-100 h.
- Re-match the 336 changed or missing functions from the USA C: about 1-2 h each, 340-670 h.
- About 270 new functions (65.8 KB at the USA average of 247 bytes): about 2-3 h each, 540-810 h.
- European TU/rodata/data matching: 40-80 h.
- Total about **1,000-1,700 hours**, plus the port work of C. These per-function hours are
  assumptions: the project has no measured verified-functions-per-hour figure (finding 007 lists
  it as open).
- Worth it only if a matching European decomp is a goal in itself.

**C. A European core: shared C, recompiled deltas, 60 Hz (recommended).**
1. European address map: every USA function and data object to its European address, from the
   matcher. Splat config and symbol file for `SLES_522.02`, asm generated locally from the
   player's own ELF. 10-15 h.
2. `build_wasm.py --release eu`:
   - the 4,779 unchanged functions (plus about 240 that differ only in missed `%lo` halves) take
     the decomp's C, compiled with European symbol addresses;
   - the 336 changed or missing ones and the 65.8 KB of new code go through `recomp.py` from
     the European asm, the way the port already runs functions that are still asm (finding 018).
     Port code, not decomp claims: nothing is counted as decompiled.
   - Units that mix the two get split.
   - 20-30 h.
3. Data: a release table in `game_data.c` and the launcher, the European image range, fixups from
   `data.py` on the European data labels, code pointers re-targeted through the map. 20-30 h.
4. 60 Hz: a generated table puts the USA values back into the 386 rescaled immediates of the
   shared functions. The same check is needed in the recompiled European-only code. 10-20 h. A
   50 Hz option can come later.
5. 640x512: the 448 fallbacks, HUD/widescreen anchors, movie centring and the port's menu layer.
   15-25 h.
6. Port patches: the 42 changed or missing functions the port patches (menus, options, text)
   need C for the European version. Non-matching C is enough for patching. Plus the 48
   immediate-only ones. 60-100 h.
7. The HLE's 562 USA symbol references through the map, with the 90 non-identical functions
   reviewed by hand: 15-20 h.
8. Launcher, disc check, online release table, texture-pack folder, memory card name: 10-15 h.
9. Tests: boot in each language, menus, a race per mode, movies, saves, an online race between
   two European clients: 20-30 h.

Total **180-290 hours**. Result: the European game in its five languages, on its revised tracks, at
60 Hz, with the port's features. The one-code rule still holds: one source tree, two symbol maps.
Later the European matching decomp (B) can grow from it function by function if wanted.

**Recommendation: C.** Start with steps 1-3 (about 50-75 h). That alone shows whether the European
core boots to the title screen on shared code, before the menu and patch work.

## 9. Images still wanted

1. **MT Bikers: Bakusou Mountain Bikers (Japan, SCPS-15062)**, redump-verified if possible. It is
   the last build (May 2004), with Japanese text (a double-byte font and text path) and a
   changed rider. Running this finding's tool on it shows whether it descends from the European
   1.01 tree (then a third core is cheap after C) or is a separate branch.
2. **SCAJ-20083 (Asia)**, only if it really exists: one database lists it and nothing confirms it.
   It would show whether Asia got an English NTSC build distinct from the USA one.

## Sources

- [redump disc 30393 (USA)](http://redump.org/disc/30393/), [redump disc 4510 (Europe)](http://redump.org/disc/4510/)
- [PSXDataCenter SCUS-97177](https://psxdatacenter.com/psx2/games2/SCUS-97177.html),
  [SLES-52202](https://psxdatacenter.com/psx2/games2/SLES-52202.html),
  [SCPS-15062](https://psxdatacenter.com/psx2/games2/SCPS-15062.html)
- [PCSX2 wiki: Downhill Domination](https://wiki.pcsx2.net/index.php/Downhill_Domination),
  [GameFAQs release data](https://gamefaqs.gamespot.com/ps2/914603-downhill-domination/data),
  [Downhill Domination wiki](https://downhill-domination.fandom.com/wiki/Downhill_Domination)
- ps2sdk / Sony libscf for `GetOsdConfigParam` (kernel syscall 0x4B) as the console's language source.
