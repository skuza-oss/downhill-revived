# 190: the online room's track list in Super Career order

Status: in progress

Branch `feat/room-track-order` from `main` ac62c78d (0.7.14), started 2026-11-09. This file is
the lane's plan while it runs and its finding when it merges.

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

- [ ] 2026-11-09: finding reserved
- [ ] The game's career order read from its data
- [ ] Game: `port/hle/online_menu.c` list in that order, picker filled by columns
- [ ] Server: a test with the new order beside the old one
- [ ] Checks: `check.py port`, `check.py server`, boot trace, picker screenshot

## Decisions

## Surprises

## Result
