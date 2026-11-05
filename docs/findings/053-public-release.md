# 053 — The first public release: reverb from the disc, a reproducible build, the zip (round 39)

Status: partly superseded by 088 (the 32-bit release build, its zip and checks)
Checked: 2026-11-04

Still true: reverb presets read from the player's disc (section 1), the game data image read
from the player's `SCUS_971.77` instead of shipped in the exe (section 4), the official server as
the default (section 5), and the rule behind them (no GPL-incompatible code, no game data, no
local inputs). Retired: the 32-bit build path of sections 2-3 and 6-13 (`build_clean.py`,
`link.py`, `--recomp-basis`, `dhd.exe` with D3D11/WinHTTP imports, the game's own updater). Today
`port/tools/package_release.py` builds and checks the 64-bit release itself (finding 088 CL3, the
release steps in `docs/STATE.md`). Sections 6-13 are the 0.2.x release log.

Goal: a release that anyone can rebuild from the public repository and their own disc, with no
third-party code under a GPL-incompatible licence and no local inputs.

## 1. Reverb presets from the player's disc

The native mixer's reverb (`port/hle/sound_reverb.h`, SetReverbType modes 0-9) needs ten preset
register sets. They were in `port/hle/sound_reverb_presets.h`, adapted from ps2sdk's
`iop/sound/libsd/src/effect.c` under the Academic Free License 2.0, which the FSF lists as
GPL-incompatible. The header and its licence file are gone; the presets are read at run time.

Where the game's own copy lives: the disc's `MOD/LIBSD.IRX` (28,557 bytes), Sony's libsd, which
989snd calls for the SPU2 effect. In its data:

- `uint32 size[10]`: the effect work area of each mode, in units of 4 sample words (mode 0: 2);
- directly after it, ten entries `{uint32 flags; uint16 reg[32]}` (68 bytes), the 32 SPU2 effect
  registers in hardware order (dAPF1, dAPF2, vIIR, vCOMB1-4, vWALL, vAPF1-2, mSAME, mCOMB1, ...,
  vIN L/R).

On this disc the table starts at file offset 0x4AE8. The values are the ps2sdk table exactly:
all ten modes, every register, and size = ps2sdk size × 2 (checked by script).

The mixer does not use the offset. `reverb_presets_load` (first SetReverbType) finds the module
with `cdvd_find("/MOD/LIBSD.IRX;1")`, reads it whole and scans it (2-byte steps) for the table's
shape:
- every flags word 0;
- every size non-zero and fitting `SndReverb.ram` (size × 4 ≤ 0x1804 × 8 sample words);
- mode 0 (OFF): every register 0 except mSAME and mDIFF (10, 11, 18, 19), which are 1;
- modes 1-9: vIN L and R (registers 30, 31) both 0x8000.

The shape matches once in the module. Without a match (no disc, another module), every mode plays
as OFF and the log says so once. `DHD_SND_TRACE=1` logs the registers each SetReverbType writes.

Checks:
- `test_sound` (test_runtime.py) serves an invented table of the same shape (made-up rooms,
  not the retail values) as the disc's LIBSD.IRX; the reverb tests pass on it.
- `replay_sound.c` (the audio replay) looks the module up on the ISO with its own ISO 9660 walk.
- At a race start (bench race, fields 3000-3100, `DHD_SND_TRACE=1`), the game sets one preset:
  core 1, mode 3, size 9,248 words. Its 32 registers equal what the old table gave for mode 3.

Sources: ps2sdk `effect.c` and `libsd-common.h` for the table layout and register order (no code
or values copied); the [no$psx SPU specification](https://psx-spx.consoledev.net/soundprocessingunitspu/)
for the register meanings.

## 2. The release build is the plain build (32-bit, retired)

Recent test executables were linked with
`build_clean.py --recomp-basis build/port/r29/native16/objects.rsp`, a local object set that is
not in the repository.

What the basis provides: it only swaps the recompiled asm-only functions (`build/port/recomp`;
721 objects of this link) for frozen copies compiled before commit 8815d61e. Speed does not come
from it:
- the VU1 AOT blocks and the native VU1 kernels come from `DHD_BUILD_VU_AOT=1`, which
  `build_clean.py` always sets;
- `vu1_aot.py` and `vu1_native_clip.py` generate them under `build/` from `assets/vutext.bin`,
  which `tools/split.py` writes from the player's executable.

The "third of the speed" (finding 047) is a bare `link.py` without that variable, not a plain
`build_clean.py`.

Proof from a fresh clone of main, in a temporary folder:

1. `git clone`, then `7z e <iso> SCUS_971.77 -oiso` (SHA-1 `4e25a222…`).
2. `python tools/split.py` (13 s).
3. `python port/tools/build_clean.py`: compile 81 s, link 273 s, no other input.

Results on b7904561:
- Speed. Bench race 6000-7000, 3 interleaved runs, all focused: plain 127.2 fields/s, a basis
  link of the same revision 123.5. An earlier pair on 2176dc56 (2 runs): plain 141.6 against the
  1499a7c4 basis build 140.9. The machine was busier for the second pair; the plain build is
  never slower.
- The exe's only absolute path was the shader's (`link.py` embedded it). It is now the
  repo-relative `port/gs/raster.hlsl`.
- `gate_native_renderer.py` against `r38-reviewed`: both arms match each other at all six moments
  and in every readback (1,153). The canonical reference differs in 54 words at each early
  moment, all in 0x8A0450-0x8A0780:
  - that is the bottom of the main thread's stack (`D_8A0400`, `port/hle/boot.c`): dead frames
    shifted by 16 bytes, holding host return addresses;
  - the cause is the recompiled SDK functions, compiled from the tree instead of the frozen basis;
  - the basis link of the same revision passes against r38-reviewed, which confirms it;
  - two plain builds give identical early dumps.
- So the reference is re-recorded from the plain build, `build/port/compare/r39-plain` (manifest
  lists the 54 words), and is the gate's default now. No check or exclusion changed. Gate on the
  plain exe, separate run: MATCH (all moments, 1,167 readbacks).

## 3. The release zip (the 32-bit zip; today finding 088 CL3)

`port/tools/package_release.py --exe build/port/clean/<rev>/dhd.exe --version X` writes
`build/release/DownhillRevived-X-win.zip`, one folder holding:
- `dhd.exe`;
- `dhd.ini`, from `port/release/dhd-default.ini`: comments and an empty `server =` line (no
  longer: section 4);
- `README.txt`, `THIRD-PARTY-NOTICES.txt` (port/release/), and `LICENSE.txt`.

`dhd.exe` imports only Windows DLLs (kernel32, user32, d3d11, dxgi, D3DCOMPILER_47, winhttp,
ole32, bcrypt); the C runtime is static. So nothing else is shipped.

The script refuses:
- an exe that build_clean.py didn't make (hash and manifest) or that was linked with
  `--recomp-basis`;
- any absolute path in the exe, ASCII or UTF-16;
- `\Users\`, the builder's account name, or the checkout's path or its parent's in the exe;
- a `workers.dev` host, or a server named by `DHD_NET_SERVER` or `--settings`;
- a server line in dhd.ini, or a `ws://` host other than the example in the text files;
- game-data file names.

An empty `server =` is ignored by `host.c` (empty values are skipped), so ONLINE says
"No server: set server = ... in dhd.ini" until the player adds one. `package_online.py` (the
friends' zip) is retired.

0.1.0 test (dhd.exe of b7904561, zip 1,988 KB), unzipped into a new folder and started without
`DHD_*` variables:
- the disc message and the file-open dialog appeared; the choice was saved into the shipped
  dhd.ini below `server =`, with `renderer = native` and `skip_movies = 1`;
- the title screen came up, then (driven by `DHD_NAV` from that point) rider select, track
  select and a race on Mt. Midoule at 60 fps;
- screenshots in `build/port/r39rel` (local).

## 4. Game data out of dhd.exe (round 39, second part)

Before: dhd.exe held the PS2 data image (`.ps2img`, 238,080 bytes: .vutext, .data, .rodata,
.gcc_except_table, .sdata, assembled from `asm/data`) and the VU microcode a second time, as the
compare words of the VU1/VU0 AOT blocks and the native VU1 kernels. A scan of the b7904561
exe (`port/tools/scan_retail.py`: every 64-byte window of the ELF with 8 or more distinct byte
values, against every exe offset) found 272 runs, 231,918 bytes.

Now:
- `port/hle/game_data.c` reads `SCUS_971.77` from the disc image's root at start (its own ISO
  9660 lookup), checks size and SHA-1 (`4e25a222…`, own SHA-1 code), and hands the bytes of its
  one PT_LOAD segment from 0x263BC0 to 0x29DCF0 to `ps2_image_load` (`port/runtime/memory.c`).
- `ps2_image_load` copies them to PS2 RAM, then writes the words the PC build changes: 4,419
  {PS2 address, value} pairs in `fixups.s` (pointers, which are host addresses here, code
  addresses of recompiled functions, `$LEH_` ranges). Switch-table entries equal to the retail
  word need none. Then the movie/heap words get PS2_RAM_BASE as before.
- `data.py` still writes `image.s` with every label in place; the link assembles it only to place
  the fixups and to check the result: outside the fixups the image must equal
  `iso/SCUS_971.77` byte for byte (it does, gaps included), and its end must be 0x29DCF0.
  `build_clean.py` copies the ELF into its worktree for that.
- The AOT blocks and kernels: `vu1_aot.py` and `vu1_native_clip.py` emit
  `AOT_WORDS(name, count, ...)` arrays that are empty (.bss) in dhd.exe, plus a table of where
  each array's words are in .vutext (`vudis.walk(..., sources=True)`; 781 + 12 entries, checked
  against the microcode when generated). `ps2_vu_aot_load` fills them from PS2 RAM right after
  the image load, before any VU code runs. The test and bench programs, which have no disc, define
  `PS2_VU_AOT_EMBED` and keep the constants. The comparison code is unchanged.
- A missing, changed or short SCUS_971.77: the first-start disc check (`host.c disc_problem`)
  now asks `game_data_read` too, so the prompt says why ("isn't the original one: the disc image
  is changed or damaged, or from another release") and offers the file dialog again. If the
  check fails later (DHD_ISO from the environment), a message box says so and the game ends.
  Tested on a copy of the first 7 MB of the disc with one ELF byte flipped, a file that is no
  disc, and the real disc.

Scan of the 90a288bb exe: 14 runs, 3,423 bytes, all compiled from `src/`: string literals of
seven units, a float table of `001EB1D8.cpp`, and the CRC-32 table clang computes at compile time
from `func_00234F38`'s loop (`scan_retail.SOURCE` lists them; `package_release.py` refuses any
other run in any packaged file). Against every other disc file up to 4 MB (2,246 of 2,338:
modules, banks, textures; the rest are movies and big audio banks), the exe's data sections
match only `MOD/LIBSD.IRX` in `snd_note_table`, the pitch table of `port/hle/sound_tables.h`:
it is floor(32768 · 2^(n/12)) and floor(32768 · 2^(n/1536)), which the formula reproduces
exactly (as `snd_pan_table` is floor(16383 · sin(πn/360))). Same math, so the same bytes; no
data from the disc.

Results on 90a288bb (plain `build_clean.py`, compile 73 s, link 215 s):
- Gate against `r39-plain`, unchanged: MATCH at all six moments, 1,153 readbacks. The data
  image built from the disc plus fixups is the image the exe used to hold.
- Bench race 6000-7000, native renderer, all runs focused, against the b7904561 release exe:
  two sessions, 3 runs in order before/disc then 5 in order disc/before. Medians 141.96 vs
  140.95, then 139.71 vs 141.32 fields/s: the first candidate of each pair is ahead; over all
  8 runs each, 140.87 (before) vs 141.13 (disc). No drop.
- 0.1.0 zip (1,916 KB; dhd.exe, README.txt, LICENSE.txt, THIRD-PARTY-NOTICES.txt) unzipped into
  a new folder and started with no `DHD_*` variables: the disc prompt; a copy of the disc's first
  sectors with one ELF byte flipped was refused with "isn't the original one" and Retry; the real
  disc was accepted, dhd.ini written from the template, the title screen came up. A second start
  (driven by `DHD_NAV`) reached a race at 60 fps. `DHD_ISO` pointing at the changed copy gives
  the start-time message box and exits. Unzipping the release again over the folder kept dhd.ini
  and a file in `saves\card1` byte for byte. The folder was deleted afterwards.

dhd.ini: the zip ships none (port/release/dhd-default.ini is gone). `settings_save` writes the
same template (comments and an empty `server =`) when it creates the file, which the first start
does when it saves the disc image. A new version unpacked over an old folder replaces only
dhd.exe and the text files.

## 5. The official server is the default (round 39, third part)

The project's public race server (`online/server/`, HTTP `GET /v1/lobbies`) was then deployed at
its Cloudflare workers address; today it is `wss://dhd.skuza.app`, the game's default. It is the one server address the repository and the
release may name. `host.c` `host_settings_load` sets `DHD_NET_SERVER` to it after reading dhd.ini
when nothing set it: no dhd.ini, an empty `server =`, no `DHD_NET_SERVER`. An explicit
`server = ...` or `DHD_NET_SERVER` still wins (self-hosting). The template's comment says so.
This replaces section 3's "No server: set server = ... in dhd.ini" for an empty line.

`package_release.py` allows that host and nothing else: any other `*.workers.dev` host in dhd.exe
or the text files, a `ws(s)://` address other than it and `races.example.org`, or the value of
`DHD_NET_SERVER` / `--settings` (unless it is the official one) refuses the zip.

Test on bb898f87 (plain `build_clean.py`, dhd.exe aade1819...), the 0.1.0 zip unzipped into a new
folder, no `DHD_NET_SERVER` and `server =` empty, three instances driven by `DHD_NAV` (no keys):
- HOST: ONLINE > Create A Public Race: room VVGW8W; `GET /v1/lobbies` listed it (1/4, ALPINE).
- A second instance: ONLINE > Public Races showed "HOST 1/4 ALPINE".
- A third: ONLINE > Join With A Code, VVGW8W typed with left/right and Cross, Join: in the room,
  both players READY (pings 42 and 47 ms); the server listed 2 players. The race was not started.
The zip is unchanged by the packaging fix (sha256 5434d5db...b033). The folder was deleted.
After the three were stopped the lobby stayed listed (1 player) for about two minutes, then the
server dropped it.

## 6. Release 0.2.0 (round 41): a new gate reference, the zip, smoke and updater checks

Build: `build_clean.py` on 22e70d54 (main after findings 054-057 and the icon commits),
compile 62 s, link 198 s; dhd.exe sha256 2253ddf9d124662a76c70bdfb391854a1e124e09a81e9d5537b384056d47f706.
The zip's README.txt is from 777fa75b (known gaps updated; the exe is unchanged by it).

Gate reference `build/port/compare/r40-plain`:
- On 22e70d54 against r39-plain the arms match each other at every moment through GO+4500 and in
  all 1,161 readbacks; against r39-plain both arms differ in exactly 2 words at GO+0/2/12/16,
  0x004F4324 (0x10 -> 0x24) and 0x004F4454 (0 -> 0x10). Those are GS packet bytes left in the
  font buffer by the main menu's EXIT GAME text (finding 056), not race state.
- So the reference is recorded from that build (`--record`, its exact arm; arms matched, 1,159
  readbacks), and `gate_native_renderer.py`'s default is now r40-plain: the release build, and
  the first build with every round-40 change in.
- The gate on HEAD (the same exe; 777fa75b changes only README.txt) against r40-plain:
  NATIVE RENDERER GAME-STATE GATE MATCH, EE at every moment, 1,151 readbacks. The readback count
  varies from run to run (1,161, 1,159, 1,151 on this exe); within a run the arms always agree.

Package: `package_release.py --exe build/port/clean/22e70d54/dhd.exe` wrote
`DownhillRevived-0.2.0-win.zip` (2,135 KB, sha256
a14684e0576fc8534afc0d09b5dd8045a3a97f20cedc72dd4ac2d0ed29756a0c) and `latest.json` (version 0.2.0,
the v0.2.0 release asset URL, that sha256, size 2,186,595). `scan_retail.py`: 14 runs of 64+
bytes, all from the decompiled source, none other. `llvm-readobj --coff-resources`: ICON ids 1-7
and GROUP_ICON 1 (port/res/dhd.rc); the window title bar shows it.

Smoke test, the zip unzipped into a new folder, no `DHD_*` variables except `DHD_WINDOW=windowed`
(so the borderless default didn't cover the desktop of the PC in use), menus by `DHD_NAV`,
dialogs by window messages:
- First start: the disc prompt; the real disc chosen; dhd.ini written with `renderer = native`,
  `scale = 2`, `window = borderless`, `widescreen = 1`, `DHD_ASPECT = wide`, `skip_movies = 0`,
  `auto_update = ask`, `textures = on` and the iso. The movies played (SCEA, copyright, the
  Incognito logo animation).
- EXIT GAME > Yes, Quit: "exit game: chosen in the main menu", exit code 0, no process left.
- A race (the gate's menu script): Alpine, the race clock running at 60 fps.
The folder was deleted.

Updater dry run (finding 054 tested the mechanics with a fake 0.2.1; this ran the real 0.2.0
zip): a copy of this dhd.exe with its three "0.2.0" strings changed to "0.1.9", in a folder with
`auto_update = on`, `update_url` = a latest.json on a local `http.server` naming the 0.2.0 zip and
its sha256, a README and a save file. Log: "0.2.0 is available (this is 0.1.9)", downloaded
2,186,595 bytes with the right sha256, "installed over 0.1.9; starting it", the new process
deleted dhd.exe.old and found "0.2.0 is the newest". After it: dhd.exe is the release's (sha256
2253ddf9...), README.txt replaced, LICENSE and notices added, dhd.ini and the save byte for byte
the same, no dhd-update folder. The folder was deleted.

## 7. Release 0.2.1 (round 42): the windows say Downhill Revived

c6a4cc4f: the game window (`present.c`), the disc and game-data message boxes (`host.c`,
`game_data.c`) and the online window titles and error box (`online.c`) say "Downhill Revived"
(the updater already did). The disc is still named "Downhill Domination (USA)" where the text
means the disc. `version.h` 0.2.1. The old developer launchers (`port/launcher`, `port/online`)
and the PCSX2 movie header in `compare_pcsx2.py` keep the game's name.

- Build: `build_clean.py` on c6a4cc4f, dhd.exe sha256
  3f7756f34a9498b8959dd0f094f42bb685f069bed57f1f21218742a6eec666b5.
- Gate against r40-plain: NATIVE RENDERER GAME-STATE GATE MATCH, 1,151 readbacks.
- `DownhillRevived-0.2.1-win.zip`, sha256
  c6840bba532f3bec69df87c8746b6fd7630b944e91bbb6c6cb1834c36e44d753 (2,186,596 bytes), and
  `latest.json` for v0.2.1.
- Smoke test (the zip unzipped, `DHD_WINDOW=windowed`, menus by `DHD_NAV`): the disc prompt and the
  game window are titled "Downhill Revived"; a race started (Alpine, clock running).
- Updater, for real: the 0.2.0 release (from its zip, dhd.exe 2253ddf9...) in a new folder with
  `auto_update = ask` and `update_url` = the 0.2.1 latest.json on a local `http.server` (its url
  pointed at the same server). The box "Downhill Revived 0.2.1 is available (this is 0.2.0)",
  Update now (BM_CLICK): downloaded 2,186,596 bytes with the right sha256, "installed over
  0.2.0; starting it", the new process deleted dhd.exe.old and found 0.2.1 newest. dhd.exe is
  then 0.2.1's (3f7756f3...); dhd.ini and the save are unchanged. The folder was deleted.

## 8. Release 0.2.2 (round 43): after-race fix, sharp font, keyboard and SDL3 pads

c4e2c707 (version.h 0.2.2) on top of finding 054 section 7, 058 and 059.

- Build: `build_clean.py` on c4e2c707, compile 77 s, link 365 s. The worktree had no SDL build;
  `link.py` ran `sdl3.py`, which unpacked the pinned SDL3 3.4.18 tarball (copied from
  build/downloads) and built `build/port/sdl3/3.4.18/build/SDL3-static.lib` inside the worktree
  with CMake. dhd.exe sha256 805d4e0913a56d0d995495bf75ffd1973b9a14611c69e652fdfcf9da8c22d781.
- Gate against r40-plain: NATIVE RENDERER GAME-STATE GATE MATCH, 1,156 readbacks.
- `DownhillRevived-0.2.2-win.zip` (2,808 KB, sha256
  4d400a85f3a8796f9a012f186203808c3036bfafe1dacd688c9d76b6c760178c) and `latest.json` for v0.2.2.
  `scan_retail.py`: the same 14 runs from the decompiled source, none other. Icon resource present.
- Smoke test (zip unzipped; dhd.ini with only the iso and `window = windowed`, so the player
  defaults applied; `DHD_NAV`): title to the main menu (five rows, EXIT GAME last); OPTIONS >
  PC SETTINGS shows the Font row, "Sharp" by default; a race started (Alpine).
- Updater: the 0.2.0 release with `auto_update = on` and a local latest.json for 0.2.2:
  "0.2.2 is available (this is 0.2.0)", 2,875,947 bytes downloaded and checked, installed,
  started; the new process deleted dhd.exe.old and found 0.2.2 newest; dhd.exe is 0.2.2's,
  README and notices replaced, dhd.ini and the save unchanged. The folder was deleted.

Online between 0.2.0 and 0.2.2: they can't share a room, so they can't desync. The lobby
version is `dhd-port <version> <first 8 bytes of the exe's SHA-256>` (`online.c`
`build_version`) and the server refuses a joiner whose string differs from the room's
(`room.ts`, `version_mismatch`); the public list marks such rooms "(other version)" and won't
join them. Apart from that, what goes over the wire is unchanged: `port/net` and the puppet
snapshot format (`hle_puppet_pack` / `hle_puppet_receive`) have no diff since 0.2.0, and no
`src/`, patch or data change touches the game's simulation. The 0.2.2 online changes are local:
after the results the PC leaves the room and plays offline (`race_over`), and the player's own
controller drives the results menu. Keyboard and SDL3 pads only feed the local pad. The sharp
font reads EE memory and writes only the native renderer's textures. Raising MIN_VERSION to
0.2.2 is still worth doing: 0.2.0 players would otherwise see 0.2.2 rooms as "other version"
with no hint, and 0.2.0 keeps the dead controller after an online race.

## 9. Release 0.2.3 (round 47)

Release revision **bde3cf923112bbbaf3c7ce2be0642c9c4a3b2b8d** (`bde3cf92`):
`Version 0.2.3`, changing only `port/include/version.h`. Includes the progressive
menu/race jitter correction (finding 060), pickup flare timing correction
(finding 061), and all additional numeric data corrections in ece43567
(finding 062). Later audit commits change tooling/documentation only; no
`data.py` correction landed after this release revision.

- Clean build: `python port/tools/build_clean.py --rev bde3cf923112bbbaf3c7ce2be0642c9c4a3b2b8d --tree build/port/clean/tree-release023 -j 4`.
  All 3,797 units compile, zero errors (73 s); link 473 s, 714 asm-only
  functions recompiled. SDL3 3.4.18 built inside the clean worktree by `sdl3.py`.
  No recompilation basis or uncommitted root-checkout source was used.
- Executable: `build/port/clean/bde3cf92/dhd.exe`, SHA-256
  `879ec2d7b81037eeae6ae5d914c56930c35e1c915bdd43a02a7bec13d9cc9b5b`.
- After finding 062 recorded and documented r46-plain, the default gate ran:
  `python port/tools/gate_native_renderer.py --exe build/port/clean/bde3cf92/dhd.exe --out build/port/release023/gate --env DHD_SCALE=2`.
  **NATIVE RENDERER GAME-STATE GATE MATCH**, result.json PASS, no failures.
  Both arms match r46-plain at GO+0/+2/+12/+16; exact/native match all six
  moments through GO+4500. **1,121 readback records / Z reads MATCH through
  virtual GS field 9042**. Native rendering was exercised.
- Word audit: every word in all eight frozen-reference and six paired
  comparisons was checked with the unchanged gate equivalence rules:
  **zero unexcluded differing words in all 14 comparisons**
  (`build/port/release023/gate-word-audit.json`). The superseded r40-plain
  29-word expectation was not applied: finding 062 audits the full 73-word
  correction and records r46-plain. This release task changed no reference,
  checker, or exclusions.
- Package: `python port/tools/package_release.py --exe build/port/clean/bde3cf92/dhd.exe`.
  `build/release/DownhillRevived-0.2.3-win.zip`, **2,876,789 bytes**, SHA-256
  `8ad7d7fee60592dd1039bfdddf26b8ad3a47811f27b807d3e84b1a51e862b752`.
  Packaging checks pass. `scan_retail.py`: 14 runs of 64+ bytes, all from
  decompiled source, zero disallowed runs. ICON resources 1-7 and GROUP_ICON 1
  are present (`llvm-readobj --coff-resources`).
- Smoke: the zip extracted into a fresh temporary folder, `DHD_WINDOW=windowed`,
  menus driven by `DHD_NAV`, movies skipped for this quick check. The window
  title is "Downhill Revived"; title and main menu are reached, then Alpine
  starts. The GO+30 capture logs clock 31, phase 2, field 3245; its race image
  was inspected. The test process and relaunch child were stopped by PID.
- Real updater: the original 0.2.2 release zip in another temporary folder,
  `auto_update = on`, local `latest.json` for the real 0.2.3 zip served by
  `python -m http.server`. Log: "0.2.3 is available (this is 0.2.2)", downloaded
  2,876,789 bytes with the package SHA-256, "installed over 0.2.2; starting it",
  then "0.2.3 is the newest". The installed exe matches the release SHA-256;
  `dhd.exe.old` and the update work folder are removed. `dhd.ini` and the
  synthetic `saves/card1/save.bin` remain byte-identical. Test/server processes
  were stopped by PID; both temporary test folders were deleted.

`build/release/latest.json`:

```json
{
 "name": "Downhill Revived",
 "version": "0.2.3",
 "url": "https://github.com/skuza-oss/downhill-revived/releases/download/v0.2.3/DownhillRevived-0.2.3-win.zip",
 "sha256": "8ad7d7fee60592dd1039bfdddf26b8ad3a47811f27b807d3e84b1a51e862b752",
 "size": 2876789,
 "page": "https://github.com/skuza-oss/downhill-revived/releases/tag/v0.2.3"
}
```

Local proof: `build/port/release023/` holds the build/link, gate, scan, icon,
packaging, smoke and updater logs and JSON results. These and all game-derived
artifacts stay ignored. The zip and manifest were published afterwards as release v0.2.3.

## 10. Release 0.2.4

Prepared 2026-10-18; **not published**. Release revision **R =
8a6f6e1818b4bb535f57496b9667b1f56e3b07bc** (`8a6f6e18`), `Version 0.2.4`,
changes only `port/include/version.h` on main `9177a74d`. Includes the independent
`dhd.ini` preferences (finding 064) and physical keyboard text entry / retail
online selection and race room (finding 065).

- Plain clean build: `python port/tools/build_clean.py --rev HEAD --tree
  build/port/clean/tree-release024 -j 4`, launched with HEAD exactly R. The
  resulting manifest pins the full R above. All 3,797 units compile, zero
  errors (69 s); link exit 0 (644 s), including SDL3 3.4.18 built in that tree.
  VU1 AOT enabled, no recompilation basis or uncommitted checkout source.
- Executable: `build/port/clean/8a6f6e18/dhd.exe`, SHA-256
  `123fb3aa283e6d35a963a89512daf5c70ae61976167075b7c75b7550d867e2e7`.
- Unchanged default gate: `python port/tools/gate_native_renderer.py --exe
  build/port/clean/8a6f6e18/dhd.exe --out build/port/release024/gate`.
  **NATIVE RENDERER GAME-STATE GATE MATCH**, result PASS, failures empty.
  All 14 EE comparisons match: both arms against r46-plain at GO+0/+2/+12/+16,
  and all six paired moments through GO+4,500. Both final capture fields 7,715;
  **1,087 readback records / Z reads MATCH through virtual GS field 9,008**.
  Native rendering exercised. No extra gate options, reference/checker/exclusion
  changes, or differing-word waiver.
- Package: `python port/tools/package_release.py --exe
  build/port/clean/8a6f6e18/dhd.exe`. `build/release/DownhillRevived-0.2.4-win.zip`,
  **2,879,370 bytes**, SHA-256
  `b8a23ced13566c2b3702ca318965642a8720710cb257854a8c855a9d2464fd0d`.
  Packaging checks pass; the four-file archive's executable equals R byte for
  byte. `scan_retail.py`: 14 runs of 64+ bytes, all SOURCE, zero disallowed
  runs. `llvm-readobj --coff-resources`: ICON 1-7 and GROUP_ICON 1 present.
- Player smoke: extracted the actual zip into a new temporary folder, with no
  ini initially. Cleared inherited `DHD_*`/`ROUND_*`; only `DHD_WINDOW=windowed`
  and `DHD_LOG=1` supplied. No scripted pad/navigation, seed, renderer, sound,
  memory-card, movie-skip or ISO variables. Selected the local disc through the
  actual first-start chooser, which wrote `dhd.ini`. Window title is Downhill
  Revived; title/main menu, original rider/shop screens, Alpine countdown and
  running race (00:19 HUD) reached with physical SendInput keys. AUDIO / OTHER
  music raw 512 -> 408, accepted, EXIT GAME, restart: raw 408 restored and the
  actual page shows 39. No manual memory-card save needed. ONLINE > Join With
  A Code accepts physically typed lowercase `xsaqz2`, displayed as `XSAQZ2`;
  Esc cancels. Read-only process memory confirms values; no game RAM writes.
- Online quick check: two instances of the same packaged R, local Wrangler
  4.151.0 dev server, `--local`, 127.0.0.1:30785, command-line
  `MIN_VERSION:0.2.4` override only. Host creates private VP2AM4; guest types
  lowercase `vp2am4` with real SendInput keys and Enter joins. Retail rider,
  bike and jersey screens exercised: Cosmo host, Mai guest, Entry Outfit 2
  visibly selected. Both READY, host Start The Race, both Alpine races running
  (phase 3, clocks 1,077/1,085 at read-only observation; screenshots show 00:17).
  Each sees the other rider; both log `online grid kept (no random start swap)`.
  This is a quick local check, not a multi-PC latency/performance test.
- Real updater: original `DownhillRevived-0.2.3-win.zip` extracted separately,
  `auto_update = on`, local latest.json and the real 0.2.4 zip served by
  `python -m http.server`. Logs: `0.2.4 is available (this is 0.2.3)`, downloaded
  2,879,370 bytes with the package SHA-256, `installed over 0.2.3; starting it`,
  then `0.2.4 is the newest`. Installed executable equals R; old executable and
  update work folder removed. `dhd.ini`, including the valid new `game_options`
  line plus font/textures preferences, and synthetic `saves/card1/save.bin`
  stay byte-identical. The updater test folder and server were removed/stopped.

`build/release/latest.json`:

```json
{
 "name": "Downhill Revived",
 "version": "0.2.4",
 "url": "https://github.com/skuza-oss/downhill-revived/releases/download/v0.2.4/DownhillRevived-0.2.4-win.zip",
 "sha256": "b8a23ced13566c2b3702ca318965642a8720710cb257854a8c855a9d2464fd0d",
 "size": 2879370,
 "page": "https://github.com/skuza-oss/downhill-revived/releases/tag/v0.2.4"
}
```

Evidence and harnesses: `build/port/release024/` (clean recipe and detailed
build/link logs, gate method/result, packaging, retail scan, icon resources,
player/online captures and logs, smoke/online/updater result JSON). All stay
ignored. The Computer Use native pipe returned unavailable twice, so the live
checks reused finding 065's local SendInput/PrintWindow approach. An early
input during a shop transition was ignored and repeated after observing the
settled screen. The offline route helper expected the tool harness's menu/phase
values; player mode kept menu 9 and race phase 3, so the live race screenshot and
read-only state established completion and only that helper PID was stopped.
No product changes were needed for these harness issues.

Stopped only recorded task-owned game/server/helper PIDs. Deleted all three smoke, online
guest and updater temporary folders. No third-party implementation copied.

## 11. Release 0.2.5

Prepared 2026-10-18; **not published**. Release revision **R =
18abe6f00f7243d0ebe67d188809b7a4c39fc580** (`18abe6f0`), `Version 0.2.5`. Includes the CPU speed
work (066), offline 120/144 FPS with input and adaptive clock fixes (067; online stays at 60), all
five performance optimizations (069), loading-card/Japan mosaic fixes (068), and the code cleanup
recorded in STATE.

- Plain clean build: `python port/tools/build_clean.py`, with HEAD exactly R.
  All 3,797 units compile, zero errors (62 s); link exit 0 (206 s), 23 existing
  SDK stubs, no remaining undefined symbols. Ordinary flags, VU1 AOT enabled,
  no recompilation basis or uncommitted checkout source.
- Executable: `build/port/clean/18abe6f0/dhd.exe`, SHA-256
  `d310080b2e437249ce85c8739493ca83d38f2376c34644b0c0b44be22aa2da39`.
- Unchanged default gate: `python port/tools/gate_native_renderer.py --exe
  build/port/clean/18abe6f0/dhd.exe --out build/port/release025/gate`, with
  `DHD_ISO` referencing the player's own disc. **NATIVE RENDERER GAME-STATE
  GATE MATCH**, result PASS, failures empty. All 14 EE comparisons match:
  both arms against r46-plain at GO+0/+2/+12/+16, and all six paired moments
  through GO+4,500. Both final capture fields 7,715; **1,151 readback records /
  Z reads MATCH through virtual GS field 9,072**. Native rendering exercised.
  No extra gate options, reference/checker/exclusion changes or word waivers.
- Package: `python port/tools/package_release.py --exe
  build/port/clean/18abe6f0/dhd.exe`. `build/release/DownhillRevived-0.2.5-win.zip`,
  **2,910,390 bytes**, SHA-256
  `26c93b4cc354ce1940ac9d080b3cff38edef1a1ff5251f3ec88a58d2ecb75683`.
  Packaging checks pass; the four-file archive's executable equals R byte for
  byte. `scan_retail.py`: 14 runs of 64+ bytes (3,423 bytes total), all SOURCE,
  zero disallowed runs. `llvm-readobj --coff-resources`: ICON 1-7 and
  GROUP_ICON 1 present.
- Player smoke: extracted the actual zip into a new temporary folder, no ini
  initially. Cleared inherited `DHD_*`/`ROUND_*`; supplied only
  `DHD_WINDOW=windowed` and `DHD_LOG=1`. Selected the local disc through the
  actual first-start message and file chooser; the game wrote `dhd.ini`.
  Startup movies, title/main menu, retail rider/shop/track screens, countdown
  and a running ten-rider Single Event race on Mt. Midoule Italy FR work with
  physical SendInput keys. AUDIO / OTHER music raw 512 -> 312, accepted,
  EXIT GAME/restart: raw 312 restored and the actual page shows 30. No manual
  career save. ONLINE > Join With A Code accepts physically typed lowercase
  `xsaqz2`, displayed as `XSAQZ2`; Esc cancels. Read-only process memory
  verifies menu/clock/preference/text values; no game RAM writes.
- High-rate smoke: before that restart, set `fps = 144` in the extracted
  copy's ini, without a `DHD_FPS` environment override. Startup logs requested
  144 real offline steps/s (online: 60). The retail selection flow reaches
  countdown and a running race, phase 3, clock 659, HUD 00:10. This is a quick
  launch/playability check, not a benchmark or high-rate equivalence proof.
  Finding 067's unmet high-rate AI nonregression and physics-equivalence
  requirements remain open; this check does not supersede them.
- Online quick check: two instances of the extracted packaged executable,
  `online_race.py --seconds 20 --capture g+20,g+600`, local Wrangler 4.151.0
  `dev --local` on loopback with command-line `MIN_VERSION:0.2.4` override.
  Health confirms that minimum; both Durable Objects are local. Scripted host
  Kalolo and guest Drud join the same room and run Alpine. Reciprocal GO+20
  captures show each other's rider beside the local player. Both exchange
  snapshots, log `online grid kept (no random start swap)`, and reach GO+600
  at field 2,327, clock 601, phase 3 (HUD 00:09), visibly riding the course.
  Both request 60 steps/s. Tool exits 0; the 20-second window intentionally
  does not wait for race results. This is a quick local check, not a multi-PC
  latency/performance or full-race test.
- Real updater: downloaded the original published
  [0.2.4 release zip](https://github.com/skuza-oss/downhill-revived/releases/tag/v0.2.4)
  anew, verified its SHA-256 against section 10, and extracted it separately.
  `auto_update = on`, local latest.json and the real new zip served by a
  loopback Python HTTP server. Logs: `0.2.5 is available (this is 0.2.4)`,
  downloaded 2,910,390 bytes with the package SHA-256, `installed over 0.2.4;
  starting it`, then `0.2.5 is the newest`. Installed executable equals R;
  old executable and update work folder removed. `dhd.ini`, including valid
  `game_options`, font/textures preferences, and synthetic
  `saves/card1/save.bin` remain byte-identical. Updater result PASS.

`build/release/latest.json`:

```json
{
 "name": "Downhill Revived",
 "version": "0.2.5",
 "url": "https://github.com/skuza-oss/downhill-revived/releases/download/v0.2.5/DownhillRevived-0.2.5-win.zip",
 "sha256": "26c93b4cc354ce1940ac9d080b3cff38edef1a1ff5251f3ec88a58d2ecb75683",
 "size": 2910390,
 "page": "https://github.com/skuza-oss/downhill-revived/releases/tag/v0.2.5"
}
```

Evidence and harnesses: `build/port/release025/` holds build/link logs, gate method/result,
packaging, retail scan, icon resources, player/online captures and logs, and smoke/online/updater
result JSON. All stay ignored. The Computer Use native pipe returned unavailable twice, so live
checks reused section 10's local SendInput/PrintWindow approach. Windows foreground restrictions
needed explicit focus recovery; inputs during shop/track transitions were retried on the observed
settled screen. These were harness issues, with no product change. No new release failure was found;
the existing high-rate limits above remain.

Stopped only task-owned game/server/helper processes by PID. No stash, FPS benchmark, push, GitHub
release, deployment, server/site change, game-data/capture commit or publication. No third-party
implementation copied.

## 12. Release 0.2.8: the AI's racing-line record

The fix (`port/patches.py`): `func_0010B8E8` returns an AI controller's next
racing-line record. Retail asm loads `x74` into `$v0` and returns it when it is non-zero
(`lw $v0, 0x74($a0)`, `bnez $v0` to `jr $ra`); only a zero `x74` calls `func_00117828`. The
matching C has no return on that path. On the EE that is harmless, but clang's build returned the
controller pointer, so every AI rider read its racing line from the wrong memory. The PC build
now returns `x74` there; the matching source is unchanged.

Build and gate:
- `build_clean.py` at the 0.2.8 version commit: `dhd.exe` SHA-256
  `b8cf7d2d467e6c3f363a9e67618aa6981c1c55e1b35cae1e05ed7c37e2f405f4`.
- Default gate against r46-plain: both arms match each other at all six moments through
  GO+4500, but both differ from r46-plain from GO+2 on (63 words at GO+2, 53 at GO+12/16; GO+0
  matches). Every word is accounted for:
  - one word in each AI rider 1-9, the same field in all nine: `0x000000C0` (read from the
    controller) becomes `0x40488DC8`, 3.13 (read from the record). The human rider 0 is
    unchanged;
  - nine words at 0x662FF0-0x6630A4 (zero, now saved return addresses) and the main thread's
    stack at 0x93C5B8-0x93C9EC: stack left behind by the different call path.
- Recorded `build/port/compare/r47-plain` with this executable (`--record`). Both arms matched
  each other, with 1,141 readbacks. The gate against r47-plain then passed: **NATIVE RENDERER
  GAME-STATE GATE MATCH**, all EE comparisons match, 1,144 readbacks and Z reads through virtual
  GS field 9065. r47-plain is now the gate's default; no exclusion or check changed.

Package: `package_release.py` first refused the executable. Its drive-path scan found `v:\@6;\@`
in `.rdata` at file offset 0x7C8D80, which is a float table (3.4421, 3.4422, ...; 3.4421 is the
bytes 76 3A 5C 40) that the new layout happened to put there. A drive-path match in the data
sections is now skipped only when it is shorter than 16 bytes and not followed by a NUL. Every C
string literal ends in a NUL, and a real path is long. Controls: a NUL-terminated path, a long
unterminated path and a short NUL-terminated one are still refused. 0.2.7 had no match at all.
`DownhillRevived-0.2.8-win.zip` is 2,933,382 bytes, SHA-256
`a5e8cbad458ffdb1662ce4125aaa847393743d9cd1543c1018d6871586159a20`.

Smoke: the zip was extracted into a new folder and run minimized with only the gate's menu and
race scripts. The window title is "Downhill Revived", the disc is read, title, main menu, Alpine.
The race runs with HUD and clock, and the AI riders pull ahead. The game was stopped by PID.

## 13. Release 0.2.9: high frame rates and fullscreen

Player reports on 0.2.8: at 120/144 fps everything animated ran far too fast (even the menu sky),
and Display: Fullscreen came up as a small window at every start. 0.2.9 is 0.2.8 with two fixes:
- The readable C of func_00204F38 (the animation blend), added before 0.2.8, put a sixth
  `* 16.66667f` in `unsorted/00202E00.cpp`; the FPS patch still expected five, so the transform
  rejected the unit and 0.2.8 ran its recompiled asm without any of its port patches (the
  animation clock among them). The patch now expects six.
- A fullscreen window starts borderless before exclusive mode; a renderer that can't start closes
  the start window with a message instead of running unseen; Optimus/PowerXpress exports; the
  adapter in the log.

Gate: the default gate against r47-plain: both arms match each other at all six moments, but
differ from r47-plain (63 words at GO+0/+2, 53 at GO+12/16). Against r46-plain (0.2.7) the same
run matches at GO+0/+2/+12/+16, and at GO+3600/4500 differs only in 5-7 words: AI riders'
racing-line record words (0.2.7 held the controller pointer, 0x0432E9xx; 0.2.9 the record,
0x10/0x4/0) and one stack word. So section 12's attribution was partly wrong: the nine rider
words at +0x1EC (0xC0 -> 3.13) and the stack words at 0x662FF0-0x6630A4 were 00202E00's asm,
not the AI fix; the AI fix only shows later in the race. Recorded `build/port/compare/r48-plain`
from 0.2.9 (`--record`), now the gate's default; the gate against it: **NATIVE RENDERER
GAME-STATE GATE MATCH**, 1,159 readbacks through GS field 9080.

## Open

- A save writes `saves\card1` on demand; the first run above made none.
