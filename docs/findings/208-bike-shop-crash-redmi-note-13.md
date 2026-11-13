# 208: the Bike Shop closes the game on a Redmi Note 13 4G (issue #78)

Status: current
Checked: 2026-11-13

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

Out of scope: the Options > Posters crash (finding 207), touch skipping of the flyover (209), the online
rider code.

## Progress

- [x] 2026-11-12: finding reserved (54b76ebb).
- [x] 2026-11-12: the crash named from the player's log and the releases' unstripped libraries
  (The crash, below).
- [x] 2026-11-12: every rider and bike texture file the shop can load checked against the USA
  disc, statically, and the shop's arena placed in a Windows run (What the USA disc allows).
- [x] 2026-11-12: the crash reproduced on Windows with a damaged copy of the disc image; the
  Posters crash's class (a stack overrun into a guard page, finding 207) ruled out.
- [x] 2026-11-12: diagnostics (What changed): read or write in the POSIX guest fault line
  (5ffc0c17), the last disc files in the crash report (665c58e9), the disc files check at every
  start (2ef4d480). `check.py port` OK; the Android debug APK builds (`build_apk.py`, PASS).
- [x] 2026-11-12: the owner's decision: a changed image's notice in the main menu, once per
  image; the hash made gentle (4def7e9a), the notice (ae8bcfa5), PLAYING.md and STATE.md.
- [x] 2026-11-12: merged into `main`; the lead's checks on the merged main (Result).
- [ ] the reporter's answer (What to ask).

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

The Options > Posters crash (finding 207) is a write off
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

- **The disc files check at every start** (`disc_check_start`, online_menu.c, called by the main
  menu's first frame; its answer polled each field by `disc_check_field` from the kernel's VBLANK,
  on any screen): the Online menu's check runs once (iso-check.txt when the launcher or an
  earlier start wrote it for this image; else the host hashes the image's files once) and dhd.log
  says `disc check: the disc image has the USA disc's game files` or which file differs first.
  The disc lines now start `disc check:` and no longer print the image's path (the settings line
  hides it as `(a path)`).
- **The disc notice** (below): a changed image is told once in the main menu.
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

## The disc notice (the owner's decision, 2026-11-12)

When the start-of-game check finds that the image's game files are not the USA disc's, the game
logs it and shows one dismissible notice, once per image.

- **What it says** (`disc_notice.h` `disc_notice_lines`, the game's own popup, `menu_page.c`, as
  EXIT GAME and PC SETTINGS use; title DISC IMAGE, lines wrapped at 38 characters, one OK row):
  "This disc image's game files are not the USA disc's (SCUS-97177). First file that differs:
  R/CNOS.RTX. / The game may close unexpectedly. A new copy of your own disc fixes it. Online
  needs the USA disc's files." Without a named file (a cut dump, no file system) the check's own
  reason stands in its place. OK or Triangle dismisses it.
- **When:** only in the main menu (patched `func_001ED5B8` in `port/patches_exit.py`: if
  `port_disc_notice_due()`, the menu's layer off, `port_disc_notice()` modally, the layer on,
  and the menu's frame ends there, as EXIT GAME's popup does), once the check has answered. An
  answer that comes during a race or anywhere else waits for the next main menu. Never over a
  race or a movie. It blocks nothing: OK goes back to the main menu as it was.
- **Where "seen" is stored:** `disc-notice.txt` in the player's data folder, next to
  `iso-check.txt` and `dhd.ini` (`DHD_DATA_DIR`, else the game's folder, on Android the app's
  files folder): the game files hashes of the images whose notice was dismissed, one per line,
  newest first, at most 16. An image is its game files hash (`dhd_disc`), so another image, or
  the same image changed, has another hash and shows the notice again; a matching image never
  shows anything. Nothing is sent anywhere. A data folder that can't be written logs it and
  shows the notice again at the next start.
- **The browser:** its worker sets `DHD_FRAME_CLOCK=1` and has no disc check, so no check and no
  notice there (one implementation; it simply never answers).

### The background hash on a first start

- Started only from the main menu's first frame (the menus are up), once per run.
- Never in timed, deterministic or traced runs: `DHD_BENCH`, `DHD_FRAME_CLOCK`,
  `DHD_STATE_TRACE`, or a tool's `DHD_NET_ISO_SHA256` (then the menu reads the given hash).
- On the host's job thread at `SDL_THREAD_PRIORITY_LOW`, in 1 MiB pieces (the check's own
  buffer), resting 1 ms after each (`dhd_disc_check`'s new `pause_us`).
- Stopped at its next piece when the game exits (an `atexit` sets `dhd_disc_check`'s new `stop`
  flag); a stopped check writes no cache and answers -3, which the poll treats as no answer.
- Once per image: iso-check.txt answers later starts. The cache's name was wrong on Android, Mac
  and Linux without `DHD_DATA_DIR` (the path was cut at `\` only, so the file was
  `dhd.exeiso-check.txt`); fixed (`disc_data_path`), so a phone hashes once more after this
  update.
- It can still run into a race if the player starts one within the first 15-20 s; it keeps its
  low priority and rests there.

What it costs on Windows (this PC, Ryzen 5 7600 and RTX 3070; hidden desktop, untimed, scale 4,
1920x1080, GS captures on, other lanes' games running, so the numbers are noisy). The USA image
with an empty data folder (the hash runs: the game started 02:39:27, iso-check.txt was written
02:40:00, so about 15-20 s of hashing after the main menu came up), then the same with the
cache. The `perf:` menu lines as written:

    hash:   perf: menus 30 s frames=1640 fps=54.1/60 low1%=13.3 over=5.7%(>20.0ms) max=152.9ms | ms/field cpu/busy game=4.78/3.90 vu=5.17/5.32 gs=2.24/1.22 render=2.37/0.98 present_wait=0.59 | gpu_ms/frame=1.48 | limit=cpu:vu
            perf: menus 30 s frames=1697 fps=56.1/60 low1%=18.2 over=5.0%(>20.0ms) max=80.1ms | ...
    cached: perf: menus 30 s frames=1698 fps=56.2/60 low1%=17.1 over=4.7%(>20.0ms) max=134.3ms | ms/field cpu/busy game=4.79/2.87 vu=4.80/4.75 gs=2.13/1.13 render=2.03/0.86 present_wait=0.47 | gpu_ms/frame=2.44 | limit=cpu:vu
            perf: menus 30 s frames=1712 fps=56.7/60 low1%=20.3 over=4.6%(>20.0ms) max=65.9ms | ...

The first window holds the hash: 54.1 fps and low 1 % 13.3 against 56.2 and 17.1 cached; the
second windows (no hash in either) differ about as much (56.1/18.2 against 56.7/20.3). Within
these runs' noise; a phone was not measured here (Left for the lead).

### Tests

- `port/hle/test_disc_notice.c` (`test_runtime.py`, `disc-notice`): the USA disc's hash, no
  notice; no answer yet, no notice; a different hash, due, and the lines name the file and fit;
  dismissed, not again for that image; another image, again, the first still seen; one line per
  image, 16 kept, newest first; a reason without a file; a word too long for a line.
- Hidden-desktop runs of this branch's `dhd64.exe` (untimed, each in its own `DHD_DATA_DIR`,
  widescreen on): a scratch copy of the USA image with `R/CNOS.RTX` changed (image A), then with
  `BIKE/MAN80.RTX` changed too (image B):

| image, data folder | window | result |
|---|---|---|
| A, empty | 1920x1080, scale 4 | `disc notice: shown` naming R/CNOS.RTX; one Cross: `dismissed`, the main menu as before |
| B, A seen | 2400x1080 (20:9), scale 4 | shown again, naming BIKE/MAN80.RTX (first in path order) |
| B, A and B seen | 2400x1080 | no notice; the answer `from iso-check.txt` |
| USA image, empty | 1920x1080 | `the disc image has the USA disc's game files (checked now)`, no notice |
| USA image, cached | 1920x1080 | `(from iso-check.txt)`, no notice |

  Pictures (PrintWindow of the hidden window) were kept outside the repository; the scratch image
  was deleted after.

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
  AGENTS.md). Started from the main menu (the first version started it 600 fields in, which a
  deterministic run or a race could reach first); a phone hashes its 2.5 GB image once (then
  iso-check.txt answers).
- The notice in the game's own popup (`menu_page`), opened from the main menu like EXIT GAME, not
  a host window or Android dialog: one path on every platform, drawn in the game's style, and it
  can only appear where the player is choosing what to do next.
- "Seen" keyed by the game files hash, not the path or the file's size and time: a copy of the
  same files elsewhere is the same image, a changed file is another.
- The notice's rules in a header (`disc_notice.h`) apart from the page, so a plain host test runs
  them without the core.
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

## Result

Checked on the merged main (lane merged as 0bee06dc):

- `check.py port` OK.
- All seven state traces (`boot`, `race`, `race2` to `race6`) equal the format-2 references of
  0.7.16's game code (the code-pointer and mask lists they were made with); no trace log has a
  `disc check:` line.
- Windows, hidden desktop, untimed, each run with an empty data folder (`DHD_DATA_DIR`):
  - The USA image, first start: `disc check: ... (checked now)`, no notice, `iso-check.txt`
    written; the Bike Shop opens. The next start: `(from iso-check.txt)`, no notice, and the game
    read about 58 MiB in all (about 2.4 GiB with the hash).
  - The race route (`DHD_NAV`) pressed on 0.2 s after the main menu came up: the race ran with no
    fault and its `perf: race` lines (60.0 and 59.9 fps). On this PC the hash ended about 14 s
    after the main menu, in the menus before the race loaded (17.6 s), so it never ran beside
    the race itself here.
  - EXIT GAME 6 s into the hash: the game ended within half a second, no `disc check:` line, no
    `iso-check.txt`; the next start hashed again (`checked now`).
  - A scratch copy of the image with 32 bytes of `R/CNOS.RTX` changed (`DHD_SCALE=4`): the DISC
    IMAGE popup names `R/CNOS.RTX`, OK closes it to the main menu, the next start shows nothing;
    the same copy with `BIKE/MAN80.RTX` changed too shows it again, naming `BIKE/MAN80.RTX`;
    `disc-notice.txt` lists both images. The copy was deleted.
  - With `DHD_BENCH` or `DHD_FRAME_CLOCK=1`: no `disc check:` line, nothing written, about 58 MiB
    read in 75 s (no hash).
- A release-signed APK of the merged main on the Mi 11i (installed over 0.7.16, dhd.ini and the
  memory card files unchanged before and after):
  - First start (no `iso-check.txt` under the corrected name; the old `dhd64iso-check.txt` stays
    beside it, unused): the main menu at 04:14:02, `iso-check.txt` written 04:14:19, so about 16 s
    of hashing (the game had run from this image just before, so part of it may have been in the
    phone's file cache). `disc check: ... (checked now)`, no notice. `perf: menus` as written: the
    window with the boot and the hash's start 57.5/60 fps, over 0.4 %, max 433 ms (the last
    check's cached start: 57.9, 0.4 %, 384 ms); the window in which the hash ended 59.9/60, over
    0.0 %, max 17.8 ms; the windows after it the same. The menus answered every tap at once.
    The Bike Shop opened and a bike was switched (GT to Specialized).
  - Second start: `(from iso-check.txt)` within the first log read after the main menu, no
    notice. A single race (Red Pass TD) from that start: `perf: session race` 59.8/60 fps, over
    0.1 %, as the last check.
  - No crash or fault lines; no `disc-notice.txt`.

## Still open

- The reporter's answer (What to ask the reporter).
- The notice on a phone (needs a changed image on the device; never the owner's own copy): the
  popup at 20:9 and OK by touch.
- A slow phone's hash time: the Mi 11i's 16 s may have been helped by the file cache.
- A test APK only if the reporter's disc check says the USA disc's files: then the crash line
  names the files the shop read.
