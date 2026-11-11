# 200: Unlock All applies at once; button icons over a texture pack's font

Status: in progress

Branch `fix/unlock-live-and-pack-icons` from `main` f0971070 (v0.7.16), started 2026-11-11. This
file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Two player reports:

- **#83, Unlock All only after a restart** (0.7.15, Windows). Turning REVIVED SETTINGS > Game >
  Unlock All on or off must apply as soon as the player leaves the page, for everything finding
  095 covers (tracks, riders and pro riders, sponsor rows, shop items, preset bikes). The save's own
  unlocks are still never written. With the setting off and never touched, the game state is
  exactly as before (boot and race state traces equal to f0971070's).
- **#81, PS2 button icons with a texture pack on** (0.7.13, Windows, `PROMPTS=xbox TEXTURES=on`).
  The chosen icons (Xbox, Nintendo, Auto with such a controller, and the remapped places of finding
  157) must show whether or not a texture pack is on; the pack's other textures keep working. One
  code path.

Out of scope: state traces, the browser build, captures and benchmarks (the lead runs the traces
once on the merged branch, listed under Result).

## Progress

- [ ] 2026-11-11: finding reserved

## Decisions

## Surprises

## Result
