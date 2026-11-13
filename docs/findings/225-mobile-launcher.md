# 225: one launcher for desktop, Android and iOS, translatable

Status: in progress

Branch `feat/mobile-launcher` from `main` b9da043f, started 2026-11-13. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The owner's decision (2026-11-13): our own launcher, in the desktop launcher's design, on Android
now and on iOS once that platform is in main (the iOS lane); people change there the settings that
are hard to change in the game. So:

- One launcher UI for every platform: the desktop's `port/launcher/ui`, made responsive for a
  phone and a tablet in portrait and landscape, shown on a phone in the system web view through a
  small native bridge with the same calls as the desktop's Tauri commands.
- On Android the launcher replaces `SetupActivity` (the disc import), the in-game update notice
  and the first-start "closed unexpectedly" dialog: one path for each.
- Every launcher string comes from one English string table with stable keys; a language is one
  more file of the same shape; a check lists missing and stale keys and fails on keys the source
  lacks. Only English ships in this lane.

Done when: the desktop launcher looks as before (captures against main), `check.py port` and
`check.py docs` are green, a boot state trace equals the reference build's, and on the Mi 11i the
launcher shows each page in portrait and landscape, PLAY starts a race, a setting changed in the
launcher shows in the game's SETTINGS and the reverse, EXIT GAME three times ends normally.

Out of scope: translating the game's own pages, the iOS bridge itself (described here for the iOS
lane's follow-up), the touch layout editor's own changes (lane 226).

## Progress

- [x] 2026-11-13: finding reserved.

## Decisions

## Surprises

## Result
