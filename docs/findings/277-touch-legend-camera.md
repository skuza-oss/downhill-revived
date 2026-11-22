# 277: touch: the menus' legend bar by taps (issue #120) and the camera button (issue #121)

Status: in progress

Branch `fix/touch-legend-camera` from `main` e272b2a2 (0.8.11 and finding 276), started 2026-11-22.
This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

- Issue #120: on a touch screen every word of the game's bottom legend that is shown ("▲ PREV ▼
  NEXT ✕ SELECT △ BACK" on the main menu, the rider screen's with □ INFO, the shop's, OPTIONS',
  the pause pages') works as a tap of its button, on every screen that draws it, through one
  mechanism where the game draws the legend; a desktop mouse gets the same. No new on-screen
  elements.
- Issue #121: the touch camera button (SELECT): find out why a press changes nothing and why the
  camera sometimes changes without one; fix a real cause, or say plainly what was tried.

Done when: a hidden check taps each legend word on the main menu, the rider screen and a pause
page and sees the game react; the existing touch checks (`touch_continue_check.py`,
`touch_left_right_check.py`, `touch_pause_check.py`) still pass; `check.py quick` and
`check.py docs`; boot and race state traces equal to the base's. Out of scope: gameplay.

## Progress

- [ ] 2026-11-22: causes, fixes, checks

## Decisions

## Surprises

## Result
