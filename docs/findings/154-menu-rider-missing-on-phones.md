# 154: The rider missing from the inventory screen on phones

Status: in progress

Branch `fix/menu-rider` from `main` 738ad0b, started 2026-11-05. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

On Android (the Mi 11i, Adreno 660, and players on other phones, it seems all of them), the
career's INVENTORY screen (Bikes / Jerseys / Bike Color; the "Bike / Jersey" board, BANK at the
bottom) shows the bike but not the rider standing beside it: of the first (default) character only
the goggles are visible, a small purple patch left of the bike. The owner's screenshot from the
Mi 11i is kept by the lead (`F:\Temp\dhd-test\menu-rider-missing-mi11i.png`). Whether a PC or Mac
shows the same is not known yet.

Goal: the rider is drawn on that screen on phones, as on the PS2 (PCSX2 is the referee: replay the
same GS stream before blaming the renderer, finding 025), with one code path for every platform
and no change to game state (unless the cause is a game-state divergence, in which case the
divergence is the bug).

Acceptance:
- Where the bug lives, with evidence: the core (EE/VU results differ on arm64, e.g. floating point
  contraction), the native renderer (state the phone path takes), the exact renderer, a setting
  the phone uses by default (RENDER=native, SCALE=2, WIDESCREEN=1, TEXTURES=on, ...), or a
  phone-only code path (findings 146-149).
- The fix, and a check that shows the rider on that screen where it was missing (the lead
  confirms on the Mi 11i).
- `python tools/check.py port` passes; boot and race state traces equal to the base build's when
  the change can touch game state.

## Progress

- [ ] 2026-11-05: finding reserved

## Decisions

## Surprises

## Result
