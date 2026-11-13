# 228: The launcher's layout editor alone on Android, and the merged main proved

Status: in progress
Supersedes: 226 in part (the launcher's entry point on Android); finding 225's bullet on "Edit the
touch layout" is corrected in place (its launcher decision stands)

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

- [x] 2026-11-13: finding reserved (6d7e8a81)
- [x] the launcher's row starts the editor alone and comes back; `DHD_TOUCH_EDIT_AT_START` removed;
  BRIDGE.md, PLAYING.md, STATE.md, the Android notes (4153b2e1)
- [x] Windows: `check.py port`, the seven traces, the launcher captures, the packaged game, the P2P
  race
- [x] the Mi 11i (release-signed APK of 4153b2e1), then the published 0.8.0 APK back
- [x] the Mac desktop build of 4153b2e1 builds, packages and starts
- [x] the outside review of the merged main (review 090), its findings 1 and 3, given to this lane by
  the lead: settings saves one at a time and PLAY after them (c9fb17e3); a refused delete of the
  original disc no longer reported as done (f24502e5)
- [ ] `check.py port` on the fixes, the Mi 11i again for the parts that touch settings and the disc
  page

## Decisions

- **Back to the launcher by Android's own task rule, not a second process.** The game ends its
  process with `host_end` (findings 210, 217) and SDL_main runs once per process, so the editor
  alone cannot hand the screen back to a launcher in the same process, and the launcher cannot
  start a second game there. For this one start `LauncherActivity` does not finish: it stays below
  the game, stopped, with its state saved. When the game's process ends, Android removes the dead
  game's activity ("app died, no saved state") and starts the activity under it again in a new
  process from its saved state. `LauncherActivity` then shows its page (never a direct start, even
  with `launcher = 0`) and the snapshot's `reopen` (`"touch-editor"`, once) opens Settings at the
  Touch tab, where the row is. Dropped: the launcher in its own process (`android:process`: two
  processes, `DhdActivity.started` and the exit records per process all change for one row), a
  launcher start sent from the dying process or by an alarm (races the process's end; background
  starts are restricted). PLAY is unchanged: the launcher finishes, EXIT GAME leaves the app.
- **The argument, not a variable.** `DhdActivity.getArguments()` returns `--touch-editor` for the
  launcher's extra; `android_main.c` already passed only that argument on to `launcher.c`'s main
  (`touch_host_args`), as finding 226 built it. One switch, the same on every platform.
- **`DHD_TOUCH_EDIT_AT_START` deleted**, with its block in `touch_host.c`: nothing else used it and
  `docs/TESTING.md` never listed it as a test switch (one code path; `DHD_TOUCH_EDITOR` is the
  test switch and is listed).
- **No unit test for the return.** The logic is Android's task rule plus two lines (`play` keeps the
  activity, `onCreate` reads the saved state); it was proved on the phone below. The launcher's JS
  tests and `check.py port` still pass.

**Review 090, finding 1: two settings saves at once.** `dhd_settings::save` read, edited and
replaced dhd.ini with no lock and one fixed temporary name (`<file>.new`). The Android launcher
runs the page's calls on a cached thread pool, so two saves (two settings changed quickly, or a
setting and PLAY, or the snapshot's own `iso` save) could overlap: one change lost, or one rename
putting a half-written `.new` over dhd.ini. Fixed at three levels:
- the crate (every caller, every platform): the whole read-modify-write is one transaction per
  process (`SAVING`, a lock; `create` takes it too), each save writes a temporary file of its own
  (`<file>.<process>-<n>.new`), and the rename is retried for half a second when Windows refuses it
  (another program holding the file without delete sharing);
- Android: `snapshot` (with its `iso` save), `save_setting` and `play` run on one single-thread
  queue in the page's order (`LauncherBridge.ordered`), so PLAY (and "Edit the touch layout")
  starts the game only after every earlier save has finished; the long calls (the online check's
  hashing, the update check, the picker) stay on the pool; the queue finishes a queued save when
  the launcher ends (`shutdown`, not `shutdownNow`);
- the page (`bridge.js` `orderedCalls`, desktop and phones): `play` and `install_update` are sent
  only after every `save_setting` / `controls_edit` sent before them has answered.
Tests: `saves_at_once` in the crate's corpus (12 threads x 25 keys into one file, a reader checking
that every read is a whole file and never shows fewer keys; with the lock and the unique names
taken out, 11 of 12 writers fail with `NotFound`), and the page's ordering test.

The other writers of the same file, and whether they can overlap now:

| Writer | Where it runs | Can it overlap? |
|---|---|---|
| The game: REVIVED SETTINGS (`pc_settings.c`), game options, the online name, TOUCH CONTROLS, the touch editor's Done, hints and tilt centre (`touch_host.c`), the bindings (`host.c`), the install id at the start | the game's process, all through `host_setting_save` -> `dhd_settings_save` | inside the process: no, the crate's lock makes each one transaction |
| Android's first start (`android_main.c` `settings_start`, `iso`) | the game's process, before the game | no: the same lock; and the launcher's saves have finished before PLAY |
| The Android launcher | the same process as the game | no: the queue, the lock, PLAY after the saves. Opened from the icon while the editor alone runs, its saves and the game's go through the same lock |
| The desktop launcher (Tauri) and the desktop game | two processes; the launcher's rows stay usable while the game runs | the file is always whole now (unique temporary names, an atomic rename on Windows, macOS and Linux, the retry on Windows). Still open: a lost update when both save within the same few milliseconds (each reads the old text, the last rename wins). A cross-process fix needs a lock between processes (`File::lock` on a sidecar file such as `dhd.ini.lock`): small, but it puts a new file in every player's folder and needs a fallback where a file system has no locks, so it is left for a decision |
| The browser | IndexedDB through `web.rs` | not this file |

**Review 090, finding 3: a refused delete reported as done.** The page flashed "The original file
is deleted" whatever `delete_original` answered (false when the document provider refuses or
throws). Now the question waits in a `deleting` stage (busy: no new pick, no PLAY), `true` ends it,
`false` or a failed call asks again with `disc.deleteFailed` (English and the seven languages,
`lang_check.py` clean). Tested in `test_launcher_ui.js` (both outcomes and the page's check that
only `true` counts).

## Surprises

- The task rule held even though `DhdActivity` is `singleInstance` (its own task): logcat at Done
  shows `Force removing ActivityRecord{... DhdActivity}: app died, no saved state`, then the
  launcher's `launcher: back from the layout editor alone` in a new process 0.3 s later.
- `online_race.py`'s plain scripted line does not reliably finish Mt. Midoule: in the first run
  the guest crashed often and was still riding at the 420 s limit; in a 900 s run neither rider
  finished (the script pedals 400 s). With `DHD_NET_TEST_FINISH=70/72` (as the 0.8.0 gate ran it)
  both finish. Not a regression: the two scripted riders get stuck on the track.
- A PC game that joins a room by `DHD_NET_JOIN` (not from the menus) never reports its finish to the
  room server (`host_service_net_finish` only when `from_menu`), so the room's finish window does
  not start and the phone, the host, raced on after the P2P results until it left the race. A
  test-harness limit, not the game's.
- The release APK's first log line says `dhd-port 0.8.0 unknown` for the core id; the published
  0.8.0 APK says the same (seen when it was put back). Not new.

## Checked on the merged main (2026-11-13)

Branch head 4153b2e1 (main 6e47e82a plus Part 1). The two hand-merged places of the merge
(`touch_host.c` after `TOUCH_COMMAND_EDIT`, `android_main.c`'s `DHD_LAUNCHER_CHECKED` rule and
`--touch-editor`) compiled and worked as written; nothing from the merge needed a fix.

Windows:
- `python tools/check.py port -j 6` (`DHD_BUILD_SLOTS=2`): OK, all 18 steps; `check.py docs` OK.
- State traces of the branch's `dhd64.exe` against `main-524948ba-ref-v3`: boot, race, race2,
  race3, race4, race5, race6: 7 of 7 `state_diff.py` PASS.
- The desktop launcher of the branch's package on a hidden desktop, a scratch data folder with the
  test disc: English and `language = de` both draw their Play page right (captures kept outside the
  repository). The Play page's release notes show only a cut Android "What you need" line and an
  empty "Install": the same in the published 0.8.0 launcher, so from the published notes' text.
- The packaged `dhd.exe` (hidden desktop, `DHD_LAUNCHER=1`, header-only dhd.ini, Native VU on by
  default): to the main menu, EXIT GAME by `DHD_NAV`, `exit game: chosen in the main menu`, exit
  code 0, `end: handlers done in 33 ms`, no problem lines. (The 0.8.0 smoke script's "front end
  runs" check fails on the published 0.8.0 the same way: that line appears only after a race.)
- Two hidden games in a P2P room on a local server (`online_race.py --hidden`, finish hooks 70 and
  72 s): both finished, both show `1. Host 1:10.03  2. Guest 1:12.03`; no `refused`, `freeze:` or
  `crash:` line in either log.

The Mi 11i (Android 13), release-signed APK of 4153b2e1 built as 0.8.0's (`package_release.py
--platform android`, PGO dhd-197, release certificate), `install -r` over the owner's 0.8.0:
- The launcher opened at the start in English (the phone's language): Play, Settings (Graphics,
  Touch, Game, Online), Help. Its Language row: Deutsch (the page in German at once, dhd.ini
  `language = de`), then System again.
- PLAY into the game, SINGLE EVENT race at Mt. Midoule with the Classic pad: SELECT and START in a
  row above R1/R2, clear of the PUNCH meter (top left); ✕ held 20 s (25 mph, clock running, the HUD
  readable); the stick turned the rider; a new race's flight: SKIP under the right thumb ended it
  straight into the countdown. Thumb zones: camera and pause in a row above the right cluster,
  SKIP ended the flight, PEDAL held (28 mph). Classic pad set back in the game's own TOUCH CONTROLS.
- TOUCH CONTROLS > Edit Layout in the game: the editor over the menus, ✕ − to 95 %, UNDO to 100 %,
  DONE; `touch_edits` unchanged.
- "Edit the touch layout" in the launcher: `touch: the layout editor alone (DHD_TOUCH_EDITOR):
  muted, no movies`, then `... is open` over a plain background with centre lines; ✕ − twice
  (90 %), UNDO (95 %), RESET (100 %), DONE: `touch: saved (hints 39, layout "")`, `end: closing
  (code 0)`, `end: handlers done in 59 ms`, and the launcher back at Settings > Touch within 1.5 s.
- EXIT GAME after a race (67 ms), after the editor alone (59 ms), from the main menu (69 ms), and
  three more (before the direct start, after turning the launcher back on, after the online room:
  37 ms): each `end: handlers done`,
  Android's record `EXIT_SELF` status 0, the next start's `previous run: how it ended: normally`, no
  FORTIFY or fatal signal, the newest tombstone still the old `tombstone_09`.
- "Open the launcher at every start" off: a cold start went straight to the title; the game's
  SETTINGS > Game > Launcher At Start on; the next start opened the launcher.
- One P2P room on the official server (CREATE A P2P RACE on the phone, the hub; a hidden PC game of
  this build joining by `DHD_NET_JOIN`, direct inside the local network): READY by tap, the host's
  START, a race (the online flight's SKIP, 46 mph), the PC's finish hook at 70 s, the P2P results
  on both (`1. L228PC 1:10.03` ... `10. RIDER (did not finish)`), LEAVE RACE (tapped twice, by
  design), LEAVE the room (`net: left room ... after 8:29`). The phone's log: no `refused` line;
  one `freeze:` episode, 25 s at the race's load (`waiting for 1 player's riders` until the PC's
  rider choice arrived), see the Result.
- After: the four save files' md5s equal to before; dhd.ini's lines equal to before plus
  `language =` (the System default, written by the Language row). The published 0.8.0 APK
  reinstalled with `install -r`, started once, EXIT GAME (`previous run: ... normally`), the phone
  at its home screen.

The Mac (M1, 8 GB, `-j4`, the PC's wasm2c parts of the same core): `build_host64.py --release
--target macos-arm64` of 4153b2e1 PASS (148 generated imports, 41 host objects; 15.5 min with a
cold Rust library), `package_release.py --platform mac` made the zip with the launcher (signed ad
hoc); the packaged game, started with SDL's dummy audio driver and its own data folder, reached the
main menu and ended by EXIT GAME (`DHD_NAV`) after 10 s: `exit game: chosen in the main menu`, exit
code 0, `end: handlers done`; nothing left running.

## Result

The launcher's "Edit the touch layout" on Android opens the layout editor alone and, after DONE or
Back, shows the launcher again at its Touch tab; `DHD_TOUCH_EDIT_AT_START` is gone. iOS does the
same through `play`'s `edit_touch_layout` as `port/launcher/BRIDGE.md` now describes (`reopen` in the
snapshot). The merged main of 2026-11-13 builds and works on Windows and on the Mi 11i.

Open: the race-load wait on a phone room's host while a joiner is still on its way to its rider
(25 s of no fields with a hidden PC joiner; the 0.8.0 gate's room had 2.8 s): the online code did
not change in today's lanes, so this is an observation for the online area, not a regression of
the merge; the room went on normally after it.
