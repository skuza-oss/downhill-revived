# 240: iOS: the disc reachable without the picker's hand-over

Status: current
Checked: 2026-11-14

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

- [x] 2026-11-14: the folder look, Open in, the launcher log (7148fe4b, 086ef09e), the page line and
  the notes (42592e71)
- [x] the Simulator: the picker by taps, the folder look, Open in, a wrong file, the shared log ("What
  was run")
- [x] `check.py quick` (Windows) OK; the unsigned IPA from 086ef09e with 0.8.3's release core: `package_release.py
  --platform ios` passed every check (unsigned, the audit, the version), 14.2 MB

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

## What was run

The Mac's "DHD Lane 224" Simulator (iPhone 17 Pro Max, iOS 26.5), the app built from 42592e71 with
0.8.3's core parts, taps by mobile-mcp, the disc fixtures of `port/host/disc_fixtures.py` (`good`,
`other`), the device's own disc set aside under a name that is not `.iso` and put back after.
Screenshots t01-t15 stayed outside the repository.

- **The picker by taps (the player's steps)**: Choose disc, On My iPhone > Downloads, tap
  `Downhill Domination (USA).iso`: the picker closed, `documentPicker:didPickDocumentsAtURLs:` was
  called, the import ran (security-scoped access granted, copied, checked) and the page said Ready
  to ride with the delete question. **Our path works in the Simulator; the device's failure is on
  the device's side** (the picker never handed the file over: the picker stayed open, so the
  delegate was not called). With 0.8.x nothing said so; now launcher.log shows "picker: shown" and
  no "chosen" line in that case.
- **A file that is not the disc** by the picker (`Not The Disc.iso`): the page's normal problem
  ("It is another PlayStation 2 disc (SLUS_203.12) ...") and "Choose another file".
- **The folder look**: with the app in the background, `random dump.ISO` (not the disc) put in
  Documents; back in front: the same problem on the page; back in front again: nothing new (not
  checked again). Then `My Dump (USA).iso` (the disc fixture): back in front, moved into place as
  SCUS-97177.iso, Ready to ride. At the launcher's start, `Inbox/Downhill Domination (USA)-1.iso`
  was moved into place (Inbox left empty).
- **Open in** from the Files app, the app not running: long press on the file, Open With >
  Downhill Revived (offered by the new document type): the app started into the launcher, the
  file came in place (outside the app: copied and checked), Ready to ride with the delete question.
  An Inbox copy did not occur in the Simulator (Files opens in place); the folder look covers it.
- **Share log** (the page's `share_log` through the test switch): the shared file lists
  `dhd-exit.txt`, `launcher.log`, `dhd.log`, `dhd-previous.log`; launcher.log has every step above.

Windows: `check.py quick` OK (docs, the core, `test_runtime`, the iOS tests). The device IPA: Info.plist carries the
document type and the imported `public.iso-image`, the executable is not signed, the page has the
new line and the snapshot's caps end with `files-folder`.

## Surprises

- The first core parts used on the Mac (a gate copy from before 0.8.3's version commit) stamped
  `dhd-port 0.8.2`; `package_release.py` refused the IPA for it, rightly. The release's own core
  parts fixed it. The Simulator runs above used the first parts: no difference for the launcher.
- In the Simulator the Files app's "On My iPhone" lists only what its file provider knows: a file
  copied into the wrong app group (the Photos provider's "File Provider Storage") never shows; the
  right one is the `group.com.apple.FileProvider.LocalStorage` container.
- LiveContainer, a common way to run unsigned IPAs without signing them, breaks guest apps' file
  pickers and offers a per-app "Fix File Picker" switch (its README; Locus's README describes the
  symptom: the picker doesn't import). The player's tool is not known; the folder look and Open in
  need no hand-over from the picker either way.

## Result

Our picker works in the Simulator by real taps; the player's failure is the device's picker not
handing the file over (the tool that signed or runs the app; LiveContainer is a known case). The
disc no longer depends on it: an `.iso` under any name moved into Files > On My iPhone > Downhill
Revived (or its Inbox) is found when the launcher opens and at each return to the front, and Files'
Open With > Downhill Revived hands it over directly; both go through the one import. Every step is
in launcher.log, which Share log carries, so the next report says where it stopped. Not proven: a
real iPhone, a sideloaded install, an Inbox copy made by iOS itself (the Simulator's Files opened in
place). Left for the lead: a test IPA for the player (same version 0.8.3, so name it apart when
handing it over).

Checked:
- 0.8.4 gate on ad4b92f9: `check.py port`, `check.py server` and `check.py docs` OK; boot and race state
  traces equal the reference build.
- The Mac's "DHD Lane 224" Simulator, the app of ad4b92f9: the disc renamed `Gate Dump (USA).iso` in Documents
  was found when the launcher opened (`folder (launcher opened): importing ...`, then `done: moved into
  place`), the launcher said Ready to ride; the iPad Simulator opened into the launcher.
- The unsigned IPA through `package_release.py --platform ios`: the audit passed; Info.plist carries the
  `public.iso-image` document type; no signing files, no home-folder paths and no team id of the Mac's
  identities in it. No real iPhone or iPad.
