# 240: iOS: the disc reachable without the picker's hand-over

Status: in progress

Branch `fix/ios-disc-pick` from `main` b1d5e515 (v0.8.3), started 2026-11-14. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

A player with 0.8.x's unsigned IPA, sideloaded on a real iPhone (signed by a third-party tool),
taps the `.iso` in the launcher's Files picker and nothing happens: the picker stays, the launcher
gets nothing, nothing is logged. Nobody had tapped this picker before (every check used the test
switches).

Done when: the picker is tapped through in the Simulator and what happens is written here; the disc
also reaches the one import (`launcher.m` `startImport:` / `job_run`, the disc check, the in-place
move) without the picker: found by itself in the app's folder under any name, and by "Open in
Downhill Revived"; every picker and import step is in a launcher log that Share log includes; the
first-start page and the iOS install notes say so; the unsigned IPA builds and passes its audit.

Out of scope: a real iPhone (none here).

## Progress

- [x] 2026-11-14: the folder look, Open in, the launcher log, the page line, the notes
- [ ] the Simulator: the picker by taps, the folder look, Open in, a wrong file, the shared log
- [ ] `check.py quick`, the unsigned IPA and its audit

## Decisions

- **The folder look** (`folder_disc`, `lookForDisc:`): when no accepted `SCUS-97177.iso` is in
  Documents, the newest `.iso` (any case, any name) in Documents or Documents/Inbox that passes
  `host_disc_check` goes through the import, which moves it into place (a rename: the same volume).
  It runs once the page listens (the page's first `check_update`: its event handlers are set by
  then, so the import's events reach it) and at each return to the front. A file that failed is
  not checked again while its size and time are unchanged; when nothing passes, the newest file that
  failed for the first time goes through the import once, so the page shows its normal problem text
  instead of nothing.
- **Open in**: Info.plist declares `public.iso-image` (`CFBundleDocumentTypes`, rank Alternate, plus
  an imported declaration for devices without the system's), and `application:openURL:options:`
  hands the file to the same import: in place (outside the app: copied, as a picked file), or a
  copy in Inbox (moved, not copied again). A start that opens a document opens the launcher even
  with direct start on; a document opened while the game runs is left to the next folder look.
- **The picker**: unchanged apart from showing file extensions (`asCopy:NO` kept: `asCopy:YES` would
  make a 2.5 GB second copy). The cause on the device is not visible from here; see Surprises.
- **launcher.log** in Documents (started again past 256 KB), listed in Share log after
  `dhd-exit.txt`: picker shown, cancelled, chosen (name), each import's name, size and whether
  security-scoped access was granted, each problem, done (moved or copied), the folder look's
  refusals. Names only: a file outside the app's folder by its last component.
- **The page**: an iOS-only capability `files-folder` adds one line under the first-start step
  (`home.first.filesFolder`, 8 languages).

## Surprises

- LiveContainer, a common way to run unsigned IPAs without signing them, breaks guest apps' file
  pickers and offers a per-app "Fix File Picker" switch (its README; Locus's README describes the
  symptom: the picker doesn't import). The player's tool is not known; the folder look and Open in
  need no hand-over from the picker either way.

## Result

(at the merge)
