# 208: the Bike Shop closes the game on a Redmi Note 13 4G (issue #78)

Status: in progress

Branch `fix/bike-shop-android` from `main` ad0135bd, started 2026-11-12.

## Goal and acceptance

Issue #78: on a Redmi Note 13 4G (Xiaomi 23129RA5FL, Snapdragon 685, Adreno 610, Android 15) the
game closes every time the player enters the Bike Shop, on 0.7.14 and 0.7.16. On the Mi 11i (the
same 2400x1080 screen at 120 Hz, Android 13) the Bike Shop works on 0.7.16. There is no such phone
here.

Done when: the crash's place is named (our function, what it reads, the guest address) with the
most likely cause and how sure that is; a fix if the evidence supports one, else diagnostics
that pin it from the next shared log, and the exact question for the reporter. Offline behaviour
unchanged; `check.py port` and `check.py docs` green; the Android debug APK compiles.

Out of scope: the Options > Posters crash (207), touch skipping of the flyover (209), the online
rider code.

## Progress

- [x] 2026-11-12: finding reserved (54b76ebb).
- [x] 2026-11-12: the crash named from the player's log and the releases' unstripped libraries
  (The crash, below).
- [x] 2026-11-12: every rider and bike texture file the shop can load checked against the USA
  disc, statically, and the shop's arena placed in a Windows run (What the USA disc allows).
- [x] 2026-11-12: the crash reproduced on Windows with a damaged copy of the disc image; the
  Posters crash's class (a stack overrun into a guard page, lane 207) ruled out.
- [x] 2026-11-12: diagnostics (What changed): read or write in the POSIX guest fault line
  (5ffc0c17), the last disc files in the crash report (665c58e9), the disc files check at every
  start (2ef4d480). `check.py port` OK; the Android debug APK builds (`build_apk.py`, PASS).
- [ ] the reporter's answer (What to ask); the lead's checks (Left for the lead).

## The crash

The player's log (two runs, 0.7.16 then 0.7.14) has the game's own report and Android's
tombstone. The tombstone's BuildId (9bf143d8...) is the 0.7.16 release's `libmain.so`; its kept
unstripped library names the frames (`llvm-symbolizer` of the NDK, offsets from the tombstone):

| frame | function |
|---|---|
| #00 | `w2c_core_func_002195F0` (crash) |
| #01 | `func_001414A0` |
| #02 | `func_00139BF8` |
| #03 | `func_001F1500` |
| #04 | `func_001EB4D0` |
| #05-#06 | `func_001B6708` (through its indirect-call wrapper) |
| #07-#10 | `w2c_core_root`, `thread_main`, `fiber_entry`, `fiber_start` |

0.7.14's own library gives the same chain at its own offsets. Not the graphics driver, not our
host: the game's code, translated.

- `func_00139BF8` sets up the Bike Shop and calls `func_001414A0(&D_0036B730, &D_0036B6A0,
  &D_0036B6B0)`: the shop's preview object, the rider (rider, outfit, a flag) and the bike (frame,
  fork, wheels). `func_001414A0` loads three models' files from the disc into a 0x9B000-byte arena
  (`obj->x94`): `R/<rider>.NGP`, `BIKE/<frame>.NGP`, `BIKE/<fork>.NGP`, then the three `.RTX`
  (textures), then `.TEX`, and for each RTX calls `func_002195F0` to upload its textures.
- `func_002195F0` walks the RTX file's records: the first at file + 16 * (word 1 of the file), each
  next one at record + 16 * (record word 0 >> 2), until a record whose word 0 is 0.
- The faulting instruction (0.7.16, `libmain.so+0xc325c0`) is `ldr x27, [x20, x21]` with x20 =
  guest base + 8: the load of a record's format word (record + 8). The fault address minus the
  guest base is EE **0x06531648**, so the record was at **0x06531640**: above the 32 MB of RAM
  (no view maps [0x02000000, 0x08000000)), an access error on reserved guest space, as the game's
  own line says (`host64 memory: access of uncommitted guest address 0x006531648`). Both versions
  give the same guest address: the walk is the same, deterministic, from the same data.
- The registers give the step before it: the out-of-line address adapter was last called with
  x1 = 0x0185FDA0 and x2 = 4 (`host64_memory_address_slow`, the 4-byte read of the previous
  record's word 0 in the low RAM view), and x0 = its host address. So the record at EE 0x0185FDA0
  held a word 0 of about 0x01334628 (a step of 0x04CD18A0 bytes): not a texture record. The
  guest base itself (fault address - guest address = x29, 0x6FDAA19000) is an ordinary mmap
  address; nothing in the guest layout depends on a fixed host address or a 4 KB page
  (`HOST64_PAGE` is 16 KB on every host).

## What the USA disc allows

`func_001FA3E8` and `func_001FA4B0` build the names from tables in SCUS_971.77: 16 riders
(C M X J K T R L D B E S H I G P), 18 outfits (NO, with W or S, then AM EB MO SU SW SO FO OA KI CA
HY PI SP SH MT PO SN), 30 frames, 22 forks. The loader (`func_0020D958`) finds the files in the
game's own table of the disc (`func_0020E520`), not through `sceCdSearchFile`, so a missing file
is not logged (the game's own message is a no-op debug print): the player's log cannot say which
files were read. A missing file loads 0 bytes and leaves the arena's old bytes; with the USA
disc's names no selection reaches a missing RTX (the combinations below).

Checked on the USA disc image (redump 30393's files), unpacked as the game does (a zip local
header with "IE\3\4", raw deflate after the name), with scratch scripts (not committed):

- Every `.RTX` under `R/` and `BIKE/` that a name can reach walks cleanly to its end record
  inside its file. The only `R/` files that do not are the 16 `R/?DI.RTX` and `R/?IO.RTX`
  (riders C D E J K M T X): another format (sizes not a multiple of 16), and no outfit code is DI
  or IO, so the shop never loads them.
- Every combination (116,820 riders, outfits, frames and forks that exist) fits the arena: the
  largest, HNOS + MONFAR4 + FKPAT, needs 0x41E70 of 0x9B000 bytes with 128-byte alignment.
- A Windows run of 0.7.16 (hidden desktop, `DHD_NAV` race_compare route, `DHD_NAV_DUMP`): in the
  shop the arena is at canonical 0x087A9E80 (EE 0x007A9E80) for rider [0,0,0] (CNOS) and bike
  [8,3,0]. The shop opened, the walk ran, no fault. The phone's walk was at EE 0x0185FDA0, not in
  that arena: either its arena lies elsewhere (another path through the menus) or the walk had
  already left it.

So with the USA disc's files `func_002195F0` cannot leave a file from any selection the shop can
make, and the Mi 11i and Windows agree. What the player's game read at 0x0185FDA0 was not a
texture record of the USA disc.

## Not the Posters crash's class (a stack local overrun into a guard page)

The Options > Posters crash (lane 207, merged into main after this branch's base) is a write off
the top of an EE stack slot into the next slot's guard page (EE stack slot k: 256 KiB at 0x06000000 + (k - 1) * 0x40000, its lowest 16 KiB
page the guard; `port/host64/fiber.c`), and its fault address lies in the same
[0x02000000, 0x08000000) range as this one. Ruled out here:

- **The access is a read** (`ldr x27, [x20, x21]`), the load of a texture record's format word;
  the Posters crash's is a write.
- **The address is no guard page**: 0x06531648 is in slot 21 (0x06500000-0x0653FFFF) at offset
  0x31648, deep inside the slot and 0xE9B8 below its top. Slots are committed when a fiber is first
  made and stay committed, so the fault says slot 21 never existed in that run: the pointer only
  happened to land in the stack range. The Windows reproduction below faulted at 0x0D2803618,
  outside it.
- **The pointer comes from data in RAM, not from the stack**: x21 is the walk's record pointer,
  built from the file's start word and each record's size word, all in the arena in guest RAM
  (the previous record, read just before, was at EE 0x0185FDA0). A shadow stack overrun cannot
  change RAM bytes.
- **The function's only stack local cannot overrun**: `image[0x60]`, filled by `func_00243D80`
  (its eight arguments, an image then dbp, dbw, psm, x, y, w, h, match libgraph's
  `sceGsSetDefLoadImage`, which fills a `sceGsLoadImage` of 6 quadwords, 0x60 bytes) and read by
  `func_002440A8`. The wasm
  frame is exactly 0x60 bytes (`sub w23, w20, #0x60`).

And a layout overrun would crash the Mi 11i too: it runs the same APK into the same shop.

## Reproduced on Windows with a damaged disc image

A scratch copy of the USA image with one shop file changed (the route's rider, `R/CNOS.RTX`,
3798 bytes), this branch's `dhd64.exe` on the hidden desktop, the same route into the shop:

| change to `R/CNOS.RTX` | what zlib says | the shop |
|---|---|---|
| 64 bytes flipped early in the deflate stream | "invalid distance too far back" | opens (the unpacker stops; the walk reads what was in the arena and ends) |
| the whole file zeroed (a bad rip's filled sectors) | not a packed file | opens (size 0; the walk ends at once on zeroed bytes) |
| 32 bytes flipped at 70 % of the stream | no error; unpacked bytes right only up to 0xC45, the records end at 0xD20 | **the game ends**: `host64 memory: read of uncommitted guest address 0x0d2803618`, frames `func_002195F0` < `func_00139BF8` (`func_001414A0` inlined) < `func_001EB4D0` < `func_001B6708`, as on the phone |

So a disc image whose shop file has wrong bytes that still unpack gives exactly the phone's
crash: same function, a read of guest memory outside RAM, every visit. Whether a given damage
crashes depends on the bytes the walk meets, so the guest address is the image's own (the phone's
0x06531648, here 0x0D2803618). The start-of-game check named the file in each of these runs
(`disc check: ... R/CNOS.RTX isn't the USA disc's`).

## The cause, and how sure

Most likely (about 75 %): **the player's disc image differs from the USA disc in a file the shop
loads** (a damaged or modified dump: a bad rip, a repack, a fan patch). The
game checks only SCUS_971.77 itself (SHA-1, `game_data.c`), which matched ("game data:
SCUS_971.77 from the disc image"); the other 2337 files are checked only when the Online menu
opens (`dhd_disc`, finding 098), and the player's log never shows that check. A changed RTX or a
damaged deflate stream gives exactly this: the unpacker stops or writes other bytes, the record
walk reads a word that is not a size, and the same image crashes at the same address every time,
on every version. The Android copy checks that the image is complete (`DiscImage.completeness`),
not its contents.

Less likely (about 20 %): the player's state (memory card: career progress, a bike or part
selection) leads the game somewhere the route here does not: the shop's arena or tables in
another place and RAM overwritten before the walk. Against it: every selection the shop can make
loads and walks cleanly, and nothing device-specific in our code touches this path. Not tested
here: no career save from that phone.

Ruled out or very unlikely: the screen (the Mi 11i has the same one), the GPU driver (the faulting
code is the game's own, before any draw), Android 15's page size (guest commits and guards are
16 KB on every host, `HOST64_PAGE`, so a 16 KB kernel commits the same ranges; and the access
is a read outside RAM, which no page size makes legal), memory tagging (the Cortex-A73/A53 cores
of the Snapdragon 685 have no MTE), RAM size (the guest space is fixed), a stack overrun (the
section above).

## What changed

No game code: nothing in the evidence is a bug in our translation, and a guard in
`func_002195F0` would only move a damaged image's crash to the race, which reads the same files.
Diagnostics, always on, the same on every platform:

- **The disc files check at every start** (`disc_check_field`, online_menu.c, called once per
  field from the kernel's VBLANK): about 10 s in, the Online menu's check runs once (iso-check.txt
  when the launcher or an earlier start wrote it for this image; else the host hashes the image's
  files once on its thread) and dhd.log says `disc check: the disc image has the USA disc's game
  files` or which file differs first. Not with `DHD_BENCH` or a tool's `DHD_NET_ISO_SHA256`; the
  browser has no check and its start fails quietly. The disc lines now start `disc check:` and no
  longer print the image's path (the settings line hides it as `(a path)`).
- **The disc files the game looked up last** (cdvd.c `hle_disc_lookup`): the last 8 lookups
  (path, ` (not found)` when missing) of `sceCdSearchFile`, fileio's `cdrom0:` and the game's own
  loader (`func_0020D958`, a port patch in `port/patches.py`: the first 128 KB chunk of each
  file) in the HLE window at `PS2_DISC_RECENT` (0x70004800, memory.h),
  printed as `disc files looked up last (N in all): ...` (`port/runtime/disc_recent.h`) by the
  crash report (host.c; on Android and the other POSIX hosts it runs before guest memory's own
  report) and by Windows' guest memory fault report (which ends the process before host.c's).
  A Bike Shop crash then names the rider and bike files. The window is not part of the state
  trace and the game never reads it.
- **Read or write** on the POSIX guest fault line, as Windows has: ` (a read)` / ` (a write)`
  after the address, from the CPU's fault record (arm64 ESR WnR, x86-64 error code). The line's
  start is unchanged (CrashReport.java matches it on Android 10).

## What to ask the reporter

1. Open ONLINE once on the current version (0.7.16) and stay on that screen until it shows the
   menu (it checks the disc image first: on a phone up to a minute the first time), then close
   the game and share the log again (long-press the icon, Share log). dhd.log then has the disc
   check's answer and the first file that differs. With a build of this branch the check runs by
   itself at every start and a crash lists the files read last.
2. Did the Bike Shop ever work for you, and does it close the game with a new career too, or only
   with your current save?
3. Where does the disc image come from (your own disc, which tool, or a download), and is it
   patched (a translation, a mod)?

## Decisions

- Diagnostics rather than a game patch: the evidence points to the data, not to our code; a
  bounds guard on the walk would hide a damaged image until the next screen that reads it.
- The check reuses finding 098's check and cache, not a second one: one implementation (root
  AGENTS.md). Started 600 fields in, after the boot's loads, on the host's thread; a phone hashes
  its 2.5 GB image once (then iso-check.txt answers).
- The recent lookups live at a fixed guest address in the HLE window, so the native crash report
  can read them without asking the core; the crash report already reads guest memory (`DHD_PEEK`).

## Surprises

- `R/?DI.RTX` and `R/?IO.RTX` (16 files) are not texture-record files although they share the
  extension; nothing reaches them through the shop's names.
- The first version of the recent-files line recorded only `sceCdSearchFile` and listed the
  sound and IRX files at the damaged-image crash, not the shop's: the game's loader has its own
  table of the disc. The loader is patched to note its files too.
- The arena search by file contents in RAM dumps found only shared headers (the shop's RTX space
  is reused for the TEX files right after the upload); reading the shop object
  (`D_0036B730`: x94, x130, count) and the selections (`D_0036B6A0`, `D_0036B6B0`) from the dumps
  answered it directly.

## Left for the lead

- State traces, all seven recipes against the format-2 references (`docs/TESTING.md`): `boot`,
  `race`, `race2` to `race6`. Expected equal: the change writes only the HLE window, which the
  trace does not hash, and logs.
- On the Mi 11i, a build of this branch: the Bike Shop opens, and dhd.log has a `disc check:`
  line about 10 s after start (the first start hashes the 2.5 GB image once on a host thread:
  see that the menus stay smooth meanwhile; later starts answer from iso-check.txt).
- A test APK is not needed to decide the cause: the release's Online menu already gives the
  decisive line (question 1). Worth making only if the reporter's disc check says the USA disc's
  files: then this branch's crash line names the files the shop read.
- If the disc differs: whether to tell players at start (a notice that the disc image is damaged
  or changed and the game may stop) rather than only in dhd.log is a product decision, not taken
  here.
