# 268: touch players cannot change what the game's menus change with left/right (issue #108)

Status: in progress

Branch `fix/touch-left-right-268` from `main` 8d956bff, started 2026-11-17. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

Issue #108: on a phone without a controller, Career's shop (the Bike / Jersey screen) shows the
heading `◄ INVENTORY ►` over BIKES, JERSEYS, BIKE COLOR. Left/right changes the heading (to the shop
pages, where songs are bought) and nothing on the screen takes a tap for it: the player stays in
INVENTORY.

Done when: every menu of the game where left/right does something can be done by touch, through
one rule in the shared touch/menu code where the game's code allows it; pad and keyboard play and
game state are unchanged (boot and race state traces equal to the base); one tap is one step; a
hidden touch run moves the shop's heading INVENTORY → the next page → back, reaches an item's buy
prompt by taps only, and works two other menus of the list. Out of scope: gameplay, the race HUD's
touch controls, our own pages (their value rows take taps already, finding 092).

## Progress

- [ ] the cause, on the base build in a hidden run (`DHD_MOUSE_TRACE=1`)
- [ ] the fix, its unit test, the hidden check tool, the checks

## Decisions

## Surprises

## Result
