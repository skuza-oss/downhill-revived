# 227: iOS: the launcher's bridge and the first on-screen run's fixes

Status: in progress

Branch `feat/ios-launcher-and-fixes` from `main` 0b686bf0, started 2026-11-13. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

The first on-screen run of the iOS app (finding 224) in the iOS Simulator found problems; this lane
fixes them and builds the launcher's iOS bridge that `port/launcher/BRIDGE.md` describes
(finding 225).

Part 1, the run's findings:
1. The iPad stood in portrait with the 4:3 picture in a band: the game must present in landscape,
   or at least lay itself out for the window it gets (size and physical size from the host, the
   touch controls in the real window).
2. Sent to the background in a race the game froze ("freeze: no field for 5 s"), dropped the held
   fingers and carried on with nobody steering on return: pause as Android does, release pads and
   fingers, stop the sound, resume without a time jump, no freeze line for a backgrounded app.
3. The next start said nothing about how the last run ended: `previous run:` lines as on Android.
4. Gamepad prompts on a touch screen ("PRESS A TO START", "LB / RB Tabs"): touch prompts until a
   real controller sends input, back to touch prompts on the next touch.
5. Desktop rows in SETTINGS on iOS: exactly Android's rows, by the same platform test.
6. Smaller notes (SKIP over the attract text, Thumb zones' circles, SELECT/START over PUNCH: lane
   226's files, written down for it; SKIP and a pad in the flight).

Part 2: the launcher page in a `WKWebView` with the phone calls and events; it replaces lane 224's
first-start disc screen; PLAY starts the game, EXIT GAME ends cleanly.

Done when: unit tests for the bridge's message handling, the previous-run record, the background
clock rule and the settings rows per platform; on Windows `check.py port` and `check.py docs` green,
the seven state traces equal the reference build `main-524948ba-ref-v3`, the desktop launcher's
tests pass, the Android app builds; on the Mac the Simulator app and the unsigned IPA build, and the
Simulator runs listed in "What was run" show each item.

Out of scope: the touch overlay's own drawing and editor (`port/hle/touch*.c`, lane 226), tilt on
iOS, a real iPhone or iPad (no Apple ID here), translations.

## Progress

- [x] 2026-11-13: finding reserved

## Decisions

## Surprises

## Result
