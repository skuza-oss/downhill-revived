# 230: Back through the Online menus

Status: current

Branch `fix/online-menu-back` from `main` a99bf961, started 2026-11-14.

## Goal and acceptance

[Issue #96](https://github.com/skuza-oss/downhill-revived/issues/96): Back must undo
a menu step, with Esc, Backspace and Triangle. Hidden games, a fresh local server
per test, no visible windows. Gates: quick, port, docs, boot/race traces equal to base.

## Progress

- [x] Read the report and findings 211, 215, 216; reserve finding (bf0424e4).
- [x] Reproduce on 0.8.1: Quick Race, Public Races, Create Public, Create Private,
  Create P2P. The shop cancels and immediately opens again on the same rider.
- [x] Fix the shared shell transition; add `port/tools/online_back_check.py`.
- [x] Hidden fixed build: all five entries with Esc, Backspace and scripted Triangle
  (15/15); START still commits to the room with keyboard and Triangle (2/2).
- [x] Final boot (600 fields) and race (5,000 fields) equal the base, every row.
- [x] `python tools/check.py quick -j 6`, `check.py port -j 6`, `check.py docs`.

## Decisions

`port/patches.py` auto-accepts Single Race (menu 11) while choosing online.
The shop's Back returns to that page (previous menu 18), so the shortcut confirms
the same rider and opens the shop again. `port_online_shell` now intercepts this
backward arrival before the shortcut, restores the carousel at its current rider,
and waits for Back to be released. An uncommitted rider's Back uses the existing
room-leave cleanup to return to ONLINE; changing a committed selection still
returns to its room.
No transport, protocol, retail source or gameplay change.

## Surprises

- On this base the public-room browser precedes rider choice. Its Back already
  returns to ONLINE. The list seen after selecting a rider is the bike shop;
  its Back is the loop above. The existing public-browser order is retained.
- Bots must advertise the game's full room version, including its core ID.
  Early public-browser runs with a mismatched bot are excluded from evidence.
- An initial condition on menu 16 missed the shop return to menu 11; the new
  regression caught it before commit.
- The regression fails on the base (no return to rider choice after shop Back).
  Evidence stays under `build/menu-back/`; no game data is tracked.

## Result

The shared shell restores rider choice on shop Back, then ONLINE on rider Back.
Each fixed run opens the shop once, reaches ONLINE after the second Back, and has
no guest memory fault. Evidence: `build/menu-back/final-*`; regression driver
`port/tools/online_back_check.py` (`--entry`, `--back`, `--commit`; quick/list use
`--version` with the tested game's full room version). Core ID of the checked
build: `b05627efa2d996e3`. The base is a99bf961.

Quick, port and docs gates pass. Both state traces match the base. No open
implementation work; the lead closes this finding at merge.
