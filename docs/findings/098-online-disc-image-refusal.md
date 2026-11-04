# 098: a player couldn't create a public room (the disc image), and the log said nothing

Status: current
Checked: 2026-11-04

A player on 0.6.4 or older reported that they couldn't create a public room or play online. They
fixed it themselves by choosing another disc image. Their dhd.log (1281 bytes) ended with `options:
restored ...` and had no `online menu:` line at all.

## Cause

The game and online check the disc in two different ways:
- To boot, the game checks only its program and data (`game_data.c`: SCUS_971.77 by SHA-1). Any
  image with the right SCUS_971.77 boots and plays offline.
- Online needs the exact USA disc: the whole file's SHA-256 must be redump disc 30393's
  (`online_menu.c us_iso_sha256`, finding 041). A dump that differs anywhere else is turned away.
  That can be a bad dump, a modified or rebuilt image, or other padding.

Such an image boots. On ONLINE, though, Create, Join and the public races did nothing. The footer
said "This ISO isn't the USA disc (SCUS-97177)", and that reads as wrong: the player's disc *is* the
USA disc. `not_ready()` wrote nothing to the log. When `iso-check.txt` already held the result from
an earlier start, the log didn't even have `checking the ISO's SHA-256`. That matches the player's
log.

Reproduced: a copy of the redump image with one byte flipped in sector 0 (the system area, which
nothing reads). It boots, then ONLINE > Create A Public Race gives the footer above and no log line.

## Fixes

- `online_menu.c`: as soon as the check's result is known, ONLINE shows lines under its rows.
  The footer holds one line of about 44 characters at 640 pixels.
  - Checking: "Checking the disc image for online..." (footer "Checking the disc image, a
    moment...").
  - Unreadable (gone, or a read error): "The disc image couldn't be read for the online check.
    Choose it again in the launcher." (footer "Can't read the disc image: choose it again").
  - Not an exact copy: "Online needs an exact copy of the Downhill Domination (USA) disc. Your
    disc image is different (a bad or modified dump). Make a new dump or choose another image in
    the launcher." (footer "This disc image can't be used online").
- The log:
  - the result, once, with the path, the image's SHA-256, the one online needs and where the
    result came from (checked now, from iso-check.txt, or DHD_NET_ISO_SHA256);
  - a missing image, a check that can't start and a failed read;
  - `online menu: can't create a room: <why>` (or `can't join`) for each Create or Join that
    is turned away;
  - a public list without an answer.
- `host.c`: the log's first line names the release and the build: `Downhill Revived 0.6.4
  (dhd-port 0.6.4 9768683142730c02)`. The build part is the start of the exe's SHA-256, the
  same name the lobby uses (`online.c build_version`). It is printed wherever the log goes,
  tools' files included.

A wrong region can't reach the online menu: such a disc has no SCUS_971.77, so it doesn't boot.

## What else was checked

- **Log tail (0.6.4's pipe, finding 097).** I started the game the way the launcher does (no
  console, no std handles, so dhd.log goes through the pipe), minimized and driven by DHD_NAV:
  - the online menu's lines reach the file while it runs;
  - closing the window (WM_CLOSE, the same path as EXIT GAME, `present.c`) keeps the exit-time
    `profile native:` line;
  - TerminateProcess at 200 offline steps/s: the last line in the file was 0.1 s or less before
    the kill.

  No tail loss, so the missing lines are explained by the refusal above, not by the log.
- `rpc loadfile: function 255 not written (send 0 bytes:, receive 4)` is printed on every boot,
  here too: it is normal.

## Checks

The x64 build of this branch: the core with `build_wasm.py` and the exe with `build_host64.py`,
- modified image, new iso-check.txt: the check row, then the 5 lines and the footer (screenshots).
  Log: `checking`, `can't create a room: Checking the disc image, a moment...` while hashing,
  then the result with both hashes and `can't create a room: This disc image can't be used
  online`;
- modified image, second start (cache hit): the result line (`from iso-check.txt`) and the
  refusal;
- image renamed after boot: `can't be found for the online check`, the unreadable lines;
- the redump image: `the disc image is the USA disc (SHA-256 0fe2b777..., checked now)`, then
  `creating a room (public)` (no server was running, so the connection was refused as expected).

## The launcher says it before the game starts (done 2026-10-27, finding 093)

`port/launcher/src-tauri/src/disc.rs` made the boot check only: an ISO 9660 volume whose
SYSTEM.CNF boots SCUS_971.77. Its last step, hashing, was skipped. The proposal was:
- After a disc is chosen (and at start, for a disc already set), hash the file on a thread with
  the `sha2` crate it already has (`update.rs`). That takes a few seconds on an SSD and about
  half a minute on a hard disk.
- Write the game's cache, `iso-check.txt` beside the game, in the game's format:
  `size:ticks:sha256`, ticks being the file's last write time in .NET ticks (FILETIME +
  504911232000000000). The game then reads the result at once. Or pass the hash as
  `DHD_NET_ISO_SHA256`.
- On a mismatch, show "Offline only: this disc image isn't an exact copy of the USA disc, so
  online races won't take it. A new dump or another image fixes it." on the Play page and on the
  Online tab. Offline play is unchanged.

Done as proposed, with iso-check.txt (not `DHD_NET_ISO_SHA256`), so a game started without the
launcher gets the result too. The note reads "Offline only: this disc image isn't an exact copy of
the USA disc (SCUS-97177). Online needs an exact dump." Details: finding 093, "Online disc check".

## The game files check (2026-10-28, public issue #9)

The whole-file SHA-256 above still turned away players with a real USA disc: dumping tools differ in
sectors the game never reads (the system area before sector 16, the padding between files, the end
of the disc), so a good dump of their own disc rarely equals redump's image byte for byte.

### What counts now

Only what the game can read: the files of the ISO 9660 file system. `dhd_disc::hash`
(port/render/disc) reads the primary volume descriptor (sector 16), walks the directory tree from
its root and, for each file, makes a record: its path from the root (`BIKE/BANIM.NGA`, without the
`;1` version; `FILE.` is `FILE`), a 0 byte, its size as 8 bytes little-endian, and its contents'
SHA-256. The game files hash is the SHA-256 of all the records sorted (by path, as they start with
it), one after the other.

- Doesn't count: sectors 0-15, the volume descriptors, the directories' own bytes (dates,
  extents), the padding after each file and anything after the last one. The same files at
  other places on the disc give the same hash.
- Counts: every file's name, size and every byte of it. The USA disc's 2338 files hold
  2,522,611,561 of the image's 2,530,869,248 bytes, so the check reads about what it read
  before, once, in disc order. It runs on a thread as before (the game's host, the launcher).
- No ISO 9660 volume, a damaged directory or a file that lies past the image's end (a cut
  dump) has no hash: such an image is "different", like a modified one.

The USA disc (redump 30393, whole file `0fe2b777...` as above): game files hash
`c8c17bcae383daf9f2feadc7f6d7476c43d659ab8d66127a299b01311f2a3476`, 2338 files (`online_menu.c
us_files_sha256`, `dhd_disc::USA_FILES_SHA256`). It was computed from the image twice: by the crate
and by an independent Python script written from the rule above (not kept); both gave this value.

The server is unchanged: it still compares each joiner's `hello.isoSha256` with the host's, which is
now the files hash. Rooms already need the same build version, so a 0.6.x client and a new one never
meet in a room.

### One implementation, in Rust

The launcher (Rust) and the game (C, when started without the launcher) both hashed, each with its
own iso-check.txt reader and writer. Now one crate, `dhd_disc` in the renderer's workspace like
`dhd_settings` (finding 101), does the hash and the cache:

- the game links it in its one Rust static library (dhd_render's `native` feature, as `dhd_net`
  and `dhd_settings`); `host_sdl.c` calls `dhd_disc_check` (port/render/include/dhd_disc.h) on
  its host thread, so Windows, macOS, Linux and Android run the same code;
- the launcher takes it as a path dependency;
- the online menu runs in the game core (wasm, no Rust): it keeps only a copy of the USA value,
  and `tests/constant.rs` fails while the two differ;
- the browser build has no online races (no network services in `worker.js`; its disc isn't a
  file the host can open), so it has nothing to check.

C in the core was the other option, but the launcher would then need a C build of its own and the
core can't read files on a thread. A Rust crate costs nothing new: SHA-256 comes from `ring`, which
both Cargo.locks already had (rustls), so no new crate entered either lock.

Removed: the launcher's whole-file hashing, its iso-check.txt key and reader (and their tests),
`online_menu.c`'s cache reader and writer, and `host_sdl.c`'s SHA-256 job; the service
`host_service_file_sha256_*` is now `host_service_disc_check_start(iso, cache)` / `_poll(digest,
note, size)`.

### iso-check.txt

The line is now `files:<size>:<write time, ns since 1970>:<hash>:<file count>:<first other file>`.
The `files:` mark keeps the two formats apart both ways: 0.6.x's `size:ticks:sha256` line never
starts with it, so its whole-file hash is never taken as a files hash, and 0.6.x's reader never
matches the new line (it would hash again and write its own). Only the crate reads and writes the
file now, so the key no longer has to agree across two languages.

### What the player and the log see

- ONLINE's lines: "The disc image's game files are different from the USA disc's (a bad or
  modified dump). Make a new dump or choose another image in the launcher." Footer (44
  characters): "This disc's game files aren't the USA disc's".
- The launcher: "Offline only: this disc image's game files are different from the USA disc's
  (SCUS-97177): a bad or modified dump. Make a new dump or choose another image."
- dhd.log: `checking the disc image's game files`, then either `the disc image has the USA disc's
  game files (files SHA-256 c8c17bca..., checked now)` or `the disc image <path> has game files
  different from the USA disc's (redump 30393): <first file> isn't the USA disc's: files SHA-256
  <hash>, online needs c8c17bca... (<how>)`, how being checked now, from iso-check.txt, or given
  by DHD_NET_ISO_SHA256. When the image has another number of files, the note says so too
  (`2337 files, the USA disc has 2338`). An image without a usable file system: `can't have the
  USA disc's game files: <why>`.
- The first other file is cheap to name: the crate holds the first 4 bytes of the SHA-256 of
  each of the USA disc's 2338 records (`src/usa_files.bin`, 9352 bytes, sorted; no game data,
  `DHD_DISC_BLESS=1` with `DHD_ISO` writes it), and the first record of the image whose tag isn't
  among them is the one named.

### Checks

- `cargo test -p dhd_disc` (synthetic ISO 9660 images built in the test): the hash is the rule
  written out; a flipped byte in sector 0, in the system area, in the padding after a file and
  after the last file, extra sectors at the end and the same files in another order on the disc
  all give the same hash; a flipped byte inside a file, another name, another size or a missing
  file give another; no volume, a damaged directory, a cut dump (`A.BIN lies past the image's
  end`), a missing file and stopping; progress over the files' bytes; iso-check.txt written,
  read back without hashing, a 0.6.x line for the same file not taken, stale after a write-time
  change; `usa_files.bin` sorted, 2338 tags.
- `tests/constant.rs`: online_menu.c's value is the crate's.
- The launcher's `cargo test` (10 pass, the network one ignored as before), including the online
  check through the crate on a booting image whose files aren't the USA disc's.
- `port/tools/test_async_services.py`: the host service on a 40 MiB synthetic image (missing
  file, busy, start refused while busy, the files hash and note, then the answer from
  iso-check.txt) and the lobby list: RESULT PASS.
- `build_host64.py --fake --run`: the fixture's disc check answers -2 with its reason for a text
  file; boundary negatives 9 and 10 (the new service with bad guest addresses) trap.
- Built: the core (`build_wasm.py`, strict link), the x64 game (`build_host64.py --target win-x64`,
  131 imports) and the launcher (`build_launcher.py`). `port/net/build_tests.py`: test_hash,
  test_json and test_loopback ok. The server (room.ts) is unchanged and wasn't rerun.
- Not run: a game start on the ONLINE page with a real or modified image (the log lines above
  come from the code, not from a run).
