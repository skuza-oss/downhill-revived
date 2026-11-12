# 218: READY on a phone in an online room, the attract demo's SKIP, the D-pad's place and taps on settings rows

Status: in progress

Branch `fix/phone-room-and-touch` from `main` c5f839a2, started 2026-11-12. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The phone check of main 1140b23f (Xiaomi Mi 11i, release-signed APK) found four things:

- A. In a P2P room joined from the phone, a tap on READY did nothing: phones could not race
  online. Find the cause (a merge since dbddf61e, or the session's state), fix it, and add a
  touch walk of the room page that readies a member.
- B. The title's attract demo showed no SKIP on the phone. Make it show and work, find why finding
  214's unit test passed, and add a run that checks it in the real game.
- C. The D-pad (finding 214) overlaps the game's own HUD in Classic, left hand; the TOUCH
  CONTROLS page's rows unavailable under D-pad do not look dimmed; rows on that page did not take
  taps.
- D. A double tap left or right on the D-pad showed no powerslide.

Done: each item's cause, fix and proof (Windows captures, phone screenshots), `check.py port` and
`check.py docs` green.

## Progress

- [ ] 2026-11-12: number reserved

## Decisions

## Surprises

## Result
