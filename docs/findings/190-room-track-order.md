# 190: the online room's track list in Super Career order

Status: current
Checked: 2026-11-10

Branch `feat/room-track-order` from `main` ac62c78d (0.7.14), started 2026-11-09. Merged into `main` at 34cf1293 on
2026-11-09; ships in 0.7.15.

## Goal and acceptance

Issue #66: the room's track list (finding 132's picker, 25 tracks since finding 166) is in the
game's track index order, which reads as random. It should follow the original Super Career
progression, first race to last, as the game itself orders it (checked against the game's data,
not copied from the issue). Every place that offers the list for choosing uses that order: the
picker (CHOOSE A TRACK), Left/Right on the room's TRACK row, and the host's rotation that NEXT,
RANDOM and the grid-lock capacity switch walk.

Display order only: track ids on the wire, rider limits, the pick-only arena, the server's
validation and protocol 4 stay as they are, so 0.7.13 and 0.7.14 games keep working in the same
rooms. The picker keeps its grid (two columns, 13 rows, same places and sizes).

Acceptance: `python tools/check.py port` and `check.py server` pass; boot state trace equal to the
base build; a hidden-desktop screenshot of the reordered picker (kept outside the repository).

Out of scope: the offline menus (Single Event, records), names, pictures, Dual Slalom (finding 166).

## Progress

- [x] 2026-11-09: finding reserved (80ede79a)
- [x] The game's career order read from its data (below)
- [x] Server: a test with the new order beside the old one (9e40014a)
- [x] Game: `port/hle/online_menu.c` list in that order, picker filled by columns (d9a18376)
- [x] Checks: `check.py port`, `check.py server`, boot trace, picker screenshots (Result)

## The game's career order

`func_001821E8(type, index)` returns a career's event list by career type, the setup block's
`+0x10C` (`D_004B25A0.x10C`); 0x21 ends a list. The type is named by the results screen
(`func_0017A678`): 0 MX, 1 TD, 2 FR, 3 SUPER, 4 ARCADE, 5 FREESTYLE, 6-9 CUSTOM ("... CAREER
COMPLETED"). Type 3, the Super Career, is `D_0026F150`:

| # | Ids | Tracks |
|---|---|---|
| 1-3 | 0, 6, 24 | Mt. Midoule FR, MX, TD |
| 4-6 | 1, 7, 13 | Red Pass FR, MX, TD |
| 7-9 | 2, 8, 10 | Mt. Konawaki FR, MX, TD |
| 10-12 | 5, 14, 22 | Mt. Zorkovaska FR, MX, TD |
| 13-15 | 11, 15, 16 | Mt. Liddellroch FR, MX, TD |
| 16 | 12 | Mt. Imasaki FR |
| 17-18 | 19, 25 | Mt. McSchley FR, MX |
| 19-21 | 3, 9, 4 | Mt. Quintanaroo FR, MX, TD |
| 22-24 | 20, 17, 18 | Salt City FR, MX, TD |

The issue's list is exactly this order, with Mosh Pit last: no difference. Mosh Pit (23) and Dual
Slalom (21) are in no career list; the room puts Mosh Pit last, where the game's own track list
(`D_00296F60`, read by `func_001F8440` for Single Event and the records) has it too.

## Decisions

- **The order is written in the source with its citation**, not read from `D_0026F150` at run
  time: the table is constant retail data, the list must be a compile-time array (`COUNT`, the hit
  boxes), and the names, the only game text, are still read from the player's data image.
- **The rotation follows the list.** The host sends its rotation in the list's order (as before),
  so NEXT goes to the next career race, the grid-lock switch picks the next fitting career track,
  and Left/Right on TRACK steps in career order. The ids, limits and pick-only set are the same 25,
  and the server takes any order (`room.ts` checks only bytes, matching limits and a ten-rider
  entry), so nothing changes on the wire or the server. A 0.7.13/0.7.14 host still sends index
  order; its room behaves as before. NEXT after Salt City TD skips Mosh Pit and wraps to Mt. Midoule
  FR.
- **The picker fills its columns top to bottom** (`TRACK_ROWS` = 13): the grid is unchanged (two
  columns of 13 rows of 10.5 units, same x, y, widths, underline and hit boxes), only the place of
  each track in it. Row-major filling would have read Midoule FR | Midoule MX across, splitting
  each mountain over rows; the issue asks for a left column 1-13 and a right column 14-25. So
  Up/Down step through the list (±1, continuing into the other column, as the mouse wheel already
  did) and Left/Right jump a column (±13); a move past either end does nothing, as before.
- **Not the Single Event order (`D_00296F60`)**: it differs (Mt. Quintanaroo after Mt. Konawaki,
  Salt City before Mt. McSchley); the issue and the lead asked for the career's progression.
- No launcher or server track list exists: the public list, results, presence and notices show
  one track's name (`track_name`), and the server keeps only the host's rotation.

## Surprises

- `D_0026F210`, a second 24-entry list (Red Pass first: 1, 7, 13, 0, 6, 24, ...), sits right after
  the career tables; `func_001B8C48` reads it by `x2A4`, not `func_001821E8`. Not a career type;
  left alone.
- Hidden-desktop runs: posted keys do not reach the room while a `DHD_AUTOPAD_FRAMES` script is
  set (the script supplies the pad every field), and posted Esc does not close the shop. Driving
  the whole route by the field script works: `...,2530:8:cross,2950:8:start` (the shop), then
  `3300-3360` five `up` (TRACK), `3420:8:cross` (the picker), `down`/`right` presses.

## Result

The room's track list follows the Super Career (above), Mosh Pit last, everywhere it is chosen
from: the picker (filled by columns), Left/Right on TRACK, and the host's rotation. Changed:
`port/hle/online_menu.c` (the list, `TRACK_ROWS`, the picker's placement and Up/Down/Left/Right);
`online/server/test/session.test.ts` (a test with the career-order list; the index-order test
stays, for older hosts). The list is bytes, not ints (1708297a): as 32-bit words it was a copy of
retail's .data that `package_release.py`'s game-data check refused in dhd.exe. Server code untouched: no deploy, `MIN_VERSION` and protocol 4 stay.

Checks (2026-11-09, `DHD_BUILD_SLOTS=2`, `-j 6`):

- `python tools/check.py port -j 6`: OK (check_docs, check_tree, build_wasm, test_runtime,
  build_game, net_tests, test_gs_wgpu, test_wasm2c_chunks). Lane core `1b807c80a952d701`; the base
  (ac62c78d + the plan commit) core came from the module cache.
- Boot state trace, base build against the lane (`state_trace_run.py boot`, `state_diff.py`):
  all 600 rows match.
- `python tools/check.py server`: OK (npm test 239 passed in 18 files, among them "takes the
  game's list in Super Career order": eight ready on Salt City MX switch to Salt City TD at grid
  lock, `fittingRotation(8)` = 18, 0, 24, 1, 13, 2, 10, 5, 22, 11, 16, 12, 19, 3, 4, 20, NEXT then
  Mt. Midoule FR; typecheck).
- Hidden desktop, a local server (`local_server.py` + `live_game.py`, PrintWindow captures kept
  outside the repository), this game hosting a public room built from the menus: the log has
  `the room's track list: 25 tracks, MOSH PIT pick-only`; the picker shows Mt. Midoule FR to
  Mt. Liddellroch FR down the left column and Mt. Liddellroch MX to Mosh Pit (Arena) down the
  right, nothing overlapping, BACK clear; Down x3 reaches Red Pass FR (its picture), Right Mt.
  McSchley FR, Down x9 stops on Mosh Pit (the ninth press past the end does nothing), Cross makes
  it the room's track (`< Mosh Pit (Arena) >`). No race was run: the race path is unchanged.

Left for the merge: Status `current`, the STATE-LOG entry; finding 132's and 166's list order
notes (index order) are then history for the order only.
