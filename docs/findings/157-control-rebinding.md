# 157: Keyboard and controller rebinding, in the launcher and in the game

Status: in progress

Branch `feat/rebinding` from `main` 113f76f, started 2026-11-05. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

Players choose which keys and controller buttons do what. The owner's choices (2026-11-05):

- **What is bound: the PS2's buttons.** Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3,
  START, SELECT, the d-pad and the sticks' directions. Not game actions: the game's own meaning of
  each button stays (Cross pedals in a race and confirms in menus). The pages may show what a
  button does in a race next to its name (docs/PLAYING.md's Controls table), as help only.
- **Where: the launcher and the game.** A Controls page in the launcher (beside Button Icons and
  Dead Zone, Settings > Controls) and a page in the game's PC SETTINGS where the player presses
  the key or button to assign. Both read and write the same `dhd.ini` keys through the one settings
  table (`port/render/settings`, AGENTS.md: one code path). The game applies a change made in the
  game at once; the launcher's apply at the next start, as its other settings do.
- **Two per PS2 button**: a main and an alternate, for keys and for pad buttons (today's
  "Space or X" and "Esc, Backspace" are such pairs). Assigning a key or button that another PS2
  button already uses moves it (no duplicates), and the page says so.
- **Controllers: a layout per player** (player 1 and player 2; every pad of a player uses that
  player's layout), with **swap sticks** and **invert X / invert Y per stick**. The keyboard is
  player 1's.
- **Defaults are today's mappings exactly**, so a player who changes nothing sees no difference:
  the keyboard table in docs/PLAYING.md (letters by place on the keyboard, scancodes, so AZERTY
  and QWERTZ keep working), gradual steering on the arrow keys, pads by position (bottom = Cross).
  A Reset (per page and per player) brings them back.
- Things that must keep working: the Esc pause and exit menu (finding on the exit menu), Alt+Enter
  or whatever the host reserves, the online menus' text entry, the touch controls (Android: out of
  scope, unchanged), SDL's `gamecontrollerdb.txt`, Button Icons (prompts follow the bound pad).
  Binding must never leave a player unable to reach a menu: Cross, Triangle, START and the d-pad
  cannot end up with nothing on both keyboard and pad (refuse the change and say why).

Out of scope: game-action bindings, mouse, touch, analog trigger behaviour, macros, Android UI
beyond what the shared settings give for free.

Acceptance:
- dhd.ini keys documented in docs/PLAYING.md (Controls) with the defaults; a missing or broken
  line falls back to the default with a dhd.log line.
- Unit tests for parsing, defaults, moving a key between buttons, the "never unreachable" rule.
- The launcher page and the in-game page both change the same dhd.ini lines; a binding changed in
  the game works without a restart.
- `python tools/check.py port` passes; boot and race state traces equal to the base build's with
  the defaults (no input changes).
- Checked by hand-equivalent harness runs on Windows (hidden desktop): a rebound key drives the
  game (DHD_TEST_INPUT or the harness's key route), and a pad layout per player.

## Progress

- [ ] 2026-11-05: finding reserved

## Decisions

## Surprises

## Result
