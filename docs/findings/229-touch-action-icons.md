# 229: Touch buttons that show what they do

Status: in progress

Branch `feat/touch-action-icons` from `main` 6e47e82a, started 2026-11-13. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The owner's decision (2026-11-13): the shared in-game touch controls (`port/hle/touch*.c`, Android
and iOS) get a second look in which each button shows what it does (a pedal on ✕, fists on ◻ and
○, a brake on △, trick marks on the shoulders) with a small badge of the pad's own name, as a
community iOS fork showed players in the project's Discord. Our own drawings; the idea is credited
to the fork (`bagasromadon/downhill-revived-ios-newbutton`). A setting, Button Style, chooses
Actions or Symbols (today's look). Actions is the default only if it reads at a phone's default
button size; otherwise Symbols stays the default.

Done when: the action table (what each button does on the ground and in the air) is written from
the game; the pictures switch with the rider's ground/air state without flicker and never under a
held finger (unit tests); the setting exists in the game's TOUCH CONTROLS page and the launcher
(eight languages, `tools/lang_check.py` clean); captures of both styles at five screen sizes judged
at real physical size; `check.py port` and `check.py docs` pass; boot and race state traces equal
the reference; the phone and the iOS Simulator build checked.

Out of scope: new gameplay, a per-slot action swap (finding 226), portrait layouts.

## Progress

- [ ] 2026-11-13: finding reserved

## Decisions

## Surprises

## Result
