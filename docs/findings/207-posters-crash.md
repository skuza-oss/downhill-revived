# 207: Posters in OPTIONS closes the game (issue 87)

Status: in progress

Branch `fix/posters-crash` from `main` ad0135bd, started 2026-11-12. This file is the lane's plan
while it runs and its finding when it merges. Someone who has only this file and the git log must
be able to pick the lane up.

## Goal and acceptance

GitHub issue #87: on 0.7.16 (Windows) choosing Posters in the game's OPTIONS menu closes the game
at once, every time. The reporter's log has `host64 memory: write of uncommitted guest address
0x006040478` and runs with Unlock All on (finding 095), widescreen, the native renderer at scale 2,
`PROMPTS=auto`, `TEXTURES=on`, an Intel UHD GPU.

Done means:

- the crash reproduced on current main (hidden desktop, untimed) with the exact commands, and gone
  after the fix with the same commands;
- the real cause fixed in `port/` (a patch in `port/patches*.py`, `src/` keeps matching), not the
  row hidden and not the fault caught;
- the neighbouring OPTIONS rows and galleries that Unlock All opens, or that load pictures the same
  way, opened and closed with Unlock All on and off without a fault;
- Posters captures at `DHD_SCALE=4`;
- a cheap test (a headless walk of the OPTIONS rows);
- `python tools/check.py port` and `check.py docs` green on the last commit;
- with Unlock All off and never touched the game state is unchanged: the state-trace recipes
  against the format-2 references are left for the lead (Result).

Out of scope: state traces (the lead runs them), benchmarks, the online rider code
(`port/hle/puppet.c`, `puppet_sync.h`, `online.c`), the Bike Shop crash on Android (208) and the
flyover skip with touch controls (209).

## Progress

- [x] 2026-11-12: finding reserved (0aca5b1a)
- [x] 2026-11-12: frames of the reporter's log named in the 0.7.16 linker map (below)
- [x] 2026-11-12: reproduced on `main` ad0135bd, with Unlock All on and (the surprise) with it off
- [x] 2026-11-12: cause found and fixed in the port's copy of `func_0019E550` (c3e2f0b5)
- [x] 2026-11-12: `port/tools/options_walk.py`: every OPTIONS row, four scenarios
- [x] 2026-11-12: `check.py port` and `check.py docs` (Result)

## The crash, named

The 0.7.16 map (`DownhillRevived-0.7.16-win.map`, kept outside the repository) names the reporter's
return addresses, top first:

| rva | function | what it is |
|---|---|---|
| 0x4c9040 (+0x60) | `func_0018D9D0` | the shared menu's constructor: the fault, a store into the menu |
| 0x4928d2 | `func_00187378` | the menu class's constructor (calls it, then sets the class table) |
| 0x4883b3 | `func_00184940` | the 16-byte header, then `func_00187378(obj + 0x10)` |
| 0x516d70 | `func_0019E550` | the Posters screen's first pass: lists the poster files on the card |
| 0x51738e | `func_0019E530` (indirect call) | the Posters menu's update, a virtual call |
| 0x4d9369 | `func_001901C0` | the nested menu loop |
| 0x4d97ab | `func_00190188` | runs that loop for a popup |
| 0x68edb0 | `func_001F0160` | OPTIONS, case 4 (row 5 in the port) after `func_00187F48` |
| 0x68414c, 0xacf7a1, 0x6134ae, 0x9f0743, 0x9eb23d, 0x19cbc4e, 0xd269b9 | front end, `func_001B6708`, the thread's start, the fiber | |

My own build (`dhd64.map`) gives the same chain with different addresses and `func_00184940`
inlined: `w2c_core_func_0018D9D0 +0x60`, `func_00187378`, `func_0019E550 +0x2fb`, ...

## Cause

`func_0019E550` builds a menu on its stack. The matching source (`src/unsorted/0019E550.c`) has

    int sp60[4];            /* `empty`  */
    struct Small sp70;      /* `player`, 0x610 bytes */
    ...
    func_00184940(empty);   /* header (x0, x4), then the 0x4FC-byte menu at empty + 0x10 */

`func_00184940` writes a 0x10-byte header and then constructs the whole menu at `empty + 0x10`. On
the EE the two locals sit back to back (`sp60 + 0x10 == sp70`), so the menu is built in `player`
itself, and `empty` is the header of the very object `player` is the menu of. The decompilation
declared them as two separate things, `empty` only 16 bytes. The two other callers of
`func_00184940` on the stack declare the right thing (`PlayerHolder`, `func_0018A868`; `PH_895A0`,
`func_00187438`).

In the wasm core clang lays the locals out on the shadow stack as it likes, and `empty` came out
topmost. The 0x50C-byte store ran off the top of the main EE fiber's 256 KB slot into the next
slot's no-access guard page (0x06040000 + 0x478): `host64 memory: write of uncommitted guest
address 0x006040478`. It is the class of finding 094 (`buf[0x20]` too small in `func_0017A678`),
and the frames above keep no address-taken locals, so the overrun always lands there: every time.

It never depended on Unlock All. The row is greyed on a save that does not own the shop's Poster
Maker flag (`+0x1219F` of the profile); Unlock All opens the row on any save (finding 095), but a
save that earned the flag reaches the same code and crashes the same. Nobody had opened the row
since the port started: finding 063 lists "unlocked Poster Maker" among its open items.

## Fix

`port/patches.py`: the port's copy of `func_0019E550` declares the one object they are,

    struct Framed {int x0; void *x4; char pad8[8]; struct Small player;};
    struct Framed frame;   empty = &frame;  player = &frame.player;

so the header is followed by the menu in any layout. The retail source and its bytes are not
touched (`src/` unchanged); the EE's own layout is exactly what the declaration says, so the
function does what it did there.

## How it was reproduced and checked

All runs: hidden desktop (`port/tools/hidden_game.py`, no visible window, no focus), untimed game
lock, posted keys, `DHD_MEMCARD=1` with a card folder of its own, `DHD_SETTINGS=none`, windowed 960x720,
sound off. Main menu: `down down x` (OPTIONS); in OPTIONS `down` x5 then `x` reaches POSTERS (with
Unlock All off and the save not owning it the row is greyed and the cursor skips it).

- Before: the base build (`main` ad0135bd, built as is) with `DHD_UNLOCK_ALL=1` and a fresh card:
  the game closes, `host64 memory: write of uncommitted guest address 0x006040478`, the same
  address as the report. With Unlock All off and a card whose save has `+0x1219F` set: the same
  fault at the same address.
  `python port/tools/options_walk.py --exe BASE.exe --scenario on-fresh --rows 4,5` prints
  `FAIL POSTERS: closed the game: host64 memory: write of uncommitted guest address 0x006040478`.
- After: the same two runs reach "POSTER DATA NOT FOUND ... RETRY / CANCEL" (the retail screen for a
  card without poster files); Triangle returns to OPTIONS with the cursor on POSTERS.
- A card with a save that owns the flags is made by `options_walk.py` itself: SAVE GAME DATA once
  on an empty card, then the profile's bytes at `+0x1219F` (posters) and `+0x12198` (pickup editor)
  set to 1 in the saved file (the save file is the profile, `0x12240` bytes, flags at their offsets).

## The OPTIONS rows (`port/tools/options_walk.py`)

Rows: 0 REVIVED SETTINGS, 1 CONTROLS, 2 DIFFICULTY, 3 AUDIO / OTHER, 4 VIDEO CLIPS, 5 POSTERS,
6 PICKUP EDITOR, 7 LOAD GAME DATA, 8 SAVE GAME DATA, 9 LOAD REPLAY. For each row the game offers, the
tool takes a picture of the list, presses Cross, takes the row's picture, requires the game alive with no
fault in its log, presses Triangle when the row opened a screen of its own, and requires the OPTIONS
list again with the cursor on the row (mean grey difference of the list's crop under 6; the screens
differ by 19 or more, the same screen by about 1).

| Scenario | Rows offered | Result on the fix (c3e2f0b5) |
|---|---|---|
| off-fresh (retail rules: POSTERS, PICKUP EDITOR greyed) | 0 1 2 3 4 7 8 9 | all open and close, no fault |
| on-fresh (Unlock All) | 0-9 | all open and close, no fault |
| off-earned (flags earned in the save) | 0-9 | all open and close, no fault |
| on-earned | 0-9 | all open and close, no fault |

On the base build the walk of rows 4 and 5 (on-fresh) fails at POSTERS and nowhere else; the other
rows were not walked on the base build (the game closes at row 5, the rows after it are not reached
in that run), but the fix touches one function only, so they do not depend on it.

Deeper than the list, with Unlock All on and a fresh card (a scratch driver, not committed):
VIDEO CLIPS plays and skips (Cross) each of its eight entries, INTRO, TECHNICAL DH, MOUNTAIN CROSS,
FREERIDE, CRASHES, TRICKS, CREDITS and RIDER PROFILE (`movie: mov/attract.pss`, `tdh`, `mx`, `fr`,
`crashes`, `tricks`, `credits`, `biocosmo`, each then `skipped`), the list returns each time and
the game stays alive. PICKUP EDITOR opens its USER PICKUPS page (OFF / EDITOR) and Triangle returns;
LOAD GAME DATA and SAVE GAME DATA show their confirmation ("Load Game Data? ... YES / NO", "Overwrite
Game Data?"); LOAD REPLAY shows TWO / THREE / FOUR PLAYERS and returns.

Pictures: `DHD_SCALE=4` (`perf: render native scale=4 picture=2560x1792`; the capture window stays
960x720) of the Posters screen with Unlock All on and a fresh card, and with it off and an earned
card, and of the OPTIONS list before and after, were taken in `walk4x` of this lane's scratch folder
(`options_walk.py --scenario on-fresh --scenario off-earned --rows 5 --scale 4`).

## Decisions

- **Fix the port's copy, not `src/`:** the decompilation matches retail's bytes with two separate
  locals; the EE layout makes them one object. Declaring the one object in the port keeps the
  behaviour of the EE frame (the header and the menu contiguous) whatever layout clang picks.
- **No change to the Posters row, to Unlock All or to the flags:** the row's rules are retail's
  (finding 095 and 200 untouched); only the crash is fixed.
- **The walk watches pictures, not game screens:** the harness has no way to ask which screen is
  shown, and a row's own way back differs (SAVE GAME DATA returns by itself). A crop of the list
  before and after is enough and needs no row knowledge.

## Surprises

- The crash does not need Unlock All: a save that owns the shop's flag crashes too. Unlock All only
  made the row reachable on the reporter's save.
- Poster data on the card was not made: a folder `BASCUS-97177PS` with `poster0..2` files of 384x224
  pixels (the size `func_0019CB08` draws) was not listed (the screen still said "POSTER DATA NOT
  FOUND"); the real name and format come from the poster maker (not in OPTIONS) and the card
  layer. Dropped after one try. The listing loop of `func_0019E550` that runs when poster files exist
  (`sp680.xC94 != 0`) was therefore not run in a game; its locals (`large`, `sp1320`, `name`, `sp50`)
  have the retail frame's sizes (0xCA0, 0x20, 0x50, 0x10), so the same overrun cannot be in them.
- `hidden_game.Game.shot` can hand back a capture the game is still writing (a truncated image):
  the tool takes it again.

## Result

(Filled in at the merge.) Fixed: OPTIONS > POSTERS opens (the retail "POSTER DATA NOT FOUND" screen
on a card without poster files) with Unlock All on and off, on a fresh card and a card whose save
owns the flag.

Checks left for the lead, with why the game state cannot change with Unlock All off and never
touched. The change is one patch, to the port's copy of `func_0019E550`, which only the Posters
screen's update (`func_0019E3A0` through the virtual `func_0019E530`) calls; none of the recipes
opens OPTIONS, let alone POSTERS, and the function writes only its own frame and the menu's
objects on it. A core id changes, as with any core change; the trace compares game state, not the
core's layout (finding 205). Run, against the format-2 references:

    python port/tools/state_trace_run.py boot  --name new-boot
    python port/tools/state_trace_run.py race  --name new-race
    python port/tools/state_trace_run.py race2 ... race6   # each: --name new-raceN
    python port/tools/state_diff.py REF.tsv build/state-trace/new-NAME.tsv   # PASS for each

and, once on the merged branch, `python port/tools/options_walk.py` (about six minutes, four
scenarios) and `check.py port`. At the merge: Status `current`, a `docs/STATE-LOG.md` entry, and the
Posters row noted in `docs/STATE.md` if the lead wants it there.
