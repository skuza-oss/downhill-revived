# 228: The launcher's layout editor alone on Android, and the merged main proved

Status: in progress

Branch `fix/android-editor-alone` from `main` 6e47e82a (lanes 224, 225, the launcher's languages
and 226 merged that day, not yet built or run as a whole), started 2026-11-13. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

1. Joining work: the launcher's "Edit the touch layout" on Android starts the layout editor alone
   (finding 226: `--touch-editor`), and Done brings the player back to the launcher, with the exit
   path of findings 210 and 217 intact (`host_end`, no abort, the next start says the previous run
   ended normally). The older `DHD_TOUCH_EDIT_AT_START` (the editor over the game's menus) goes if
   nothing else uses it. `port/launcher/BRIDGE.md` says what `play` with `edit_touch_layout` does,
   so iOS (lane 227) can do the same.
2. Proof of the merged main plus this lane: `check.py port` and `check.py docs`; the seven state
   traces equal the reference build of 524948ba; the desktop launcher in English and German; the
   packaged-style Windows game to the title and out; a two-game P2P race on a local server; on the
   Mi 11i a release-signed APK: the launcher, a race with the new default places, the editor alone
   and back, the in-game editor, three clean exits, a direct start, one P2P room against a PC; the
   Mac desktop build builds and starts.

Out of scope: `port/ios` and the iOS side of the bridge (lane 227).

## Progress

- [ ] 2026-11-13: finding reserved

## Decisions

## Surprises

## Result
