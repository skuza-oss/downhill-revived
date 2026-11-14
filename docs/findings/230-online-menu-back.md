# 230: Back through the Online menus

Status: in progress

Branch `fix/online-menu-back` from `main` a99bf961, started 2026-11-14.

## Goal and acceptance

Issue #96: Back returns one step from rider choice to ONLINE, and from the public
room list through rider choice to ONLINE. Reproduce all five entries with keyboard
and Triangle on hidden desktops, then check the shared path and regression test.
Run quick, port if needed, docs, and boot/race state comparisons against the base.

## Progress

- [x] Read issue #96 and findings 211, 215, 216; base build underway.
- [ ] Reproduce, fix, and check.

## Decisions

Use the existing hidden-game and local-server harnesses. No protocol changes.

## Surprises

Pending reproduction.

## Result

The lead closes this finding at merge.
