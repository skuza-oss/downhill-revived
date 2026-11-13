# 227: iOS: the launcher's bridge and the first on-screen run's fixes

Status: current
Checked: 2026-11-13

Branch `feat/ios-launcher-and-fixes` from `main` 0b686bf0, started 2026-11-13 (main 0b68cbf8 and
6e47e82a, lane 226, merged in on the way). This file is the lane's plan while it runs and its
finding when it merges.

## Goal and acceptance

The first on-screen run of the iOS app (finding 224) in the iOS Simulator, and later a tap run
with mobile-mcp, found problems; this lane fixes them and builds the launcher's iOS bridge that
`port/launcher/BRIDGE.md` described (finding 225).

Part 1, the runs' findings:
1. The iPad stood in portrait with the 4:3 picture in a band: the game must present in landscape,
   or at least lay itself out for the window it gets.
2. Sent to the background in a race the game froze ("freeze: no field for 5 s"), dropped the held
   fingers and carried on with nobody steering on return.
3. The next start said nothing about how the last run ended (Android's `previous run:` lines).
4. Gamepad prompts on a touch screen ("PRESS A TO START", "LB / RB Tabs").
5. Desktop rows in SETTINGS on iOS.
6. Smaller notes: SKIP over the attract demo's text, Thumb zones' SLIDE and HOP, SELECT/START over
   PUNCH (lane 226's files), the flight over the track not skipped by a pad press.
7. (the tap run) SETTINGS can't be left by touch; Join With A Code can't be cancelled with the
   keyboard up; "QUIT COWNHILL REVIVED?" in the quit dialog.

Part 2: the launcher page in a `WKWebView` with the phone calls and events; it replaces lane 224's
first-start disc screen; PLAY starts the game, EXIT GAME ends cleanly.

Done when: unit tests for the bridge's message handling, the previous-run record, the background
clock rule and the settings rows per platform; on Windows `check.py port` and `check.py docs`
green, the seven state traces equal the reference build `main-524948ba-ref-v3`, the Android app
builds; on the Mac the Simulator app and the unsigned IPA build, and the Simulator runs below.

Out of scope: the touch overlay's own drawing and editor (lane 226), a real iPhone or iPad (no
Apple ID here), translations.

## Progress

- [x] 2026-11-13: finding reserved (08e881d8)
- [x] phones share SETTINGS' rows, the previous run's quote and the update check: `port/host/phone.c`
  (5a5252c1)
- [x] the app away from the screen (Android and iOS) and prompts by the device in use (306dc7db)
- [x] iOS's run record (7616d69e); the launcher's calls in shared C, the bridge's page side, the
  disc copy's rules (860cca5c); the app opens into the launcher, the iPad's game turned (4f244b64,
  d161aed6)
- [x] docs (c4a3765b); main with lane 226 merged in (41b620fd)
- [x] the tap run's items: our pages' legends answer a tap (ba1687e8); iOS Back by an edge swipe,
  Cancel and Done over the keyboard (e5300b78)
- [x] found on the way: dhd.ini's absolute disc path after an update's new container (8286f422);
  the checks' direct start (2eeed2db); no `UIRequiresFullScreen` (a45e2dd1)
- [x] the outside review (`review-090`): one disc check with the whole volume (c83d7d4d), one audit
  of every release IPA (45b8b9cd), the background waits for the game and renderer (2a7e59f1)

## Part 1, item by item

**1. The iPad in portrait.** Cause: the app (no scene manifest, `UIRequiresFullScreen`,
landscape-only Info.plist) got a landscape window, 2752 x 2064, and iPadOS 26 showed it scaled
into the portrait screen with black bands ("compatibility" letterboxing): the game believed the
screen was landscape because its window was. iPadOS 26 does not reliably honour an orientation
lock (Apple's TN3192 deprecates `UIRequiresFullScreen`; developer forum threads 811216 and 812253
report landscape-only iPad apps shown in portrait with the rotation lock off). With `UIRequiresFullScreen` the scene even
kept the screen's landscape size while the device stood upright. Fix: no `UIRequiresFullScreen`
(deprecated, TN3192) and the iPad declares every orientation, so the app gets the real window; the game asks for landscape
(`requestGeometryUpdateWithPreferences`, `supportedInterfaceOrientationsForWindow` landscape while
the game runs); where the window stays portrait and fills the screen, the game's view is turned a
quarter at the screen's whole size (`metal_bridge.m`), so the player turns the iPad and the game is
upright with its controls at the screen's edges, the touch screen's size, safe area and touches
being the turned view's own (UIKit converts them); any other window (Stage Manager, Split View)
the game takes as it is. The launcher turns with the device.

**2. The background in a race.** Cause: iOS waited in `gs_pump_messages` (Android in
`app_background_wait`) without telling anyone: the freeze watch saw no field for 17 s, held
fingers were dropped by the cancel only, the sound device ran dry, and an online clock would owe
the time away (fps.c keeps what the two-field cap leaves over online and gives it back at two
fields a step). Android did the same apart from the pause (finding 091: it waits; it never
opened the pause either). Fix, one path for both phones (`present.c`'s `app_away_wait`):
`host_app_away(1)` lets go of every finger, pauses the sound device (`host_sound_away`), quiets
the freeze watch; at the return the core rebases its clock right after that present
(`host_service_host_came_back` in `kernel.c`: the time away is no game time, offline and online)
and the touch overlay does what it does when a controller goes away mid-race
(`touch_pad_lost`): offline it opens the game's own pause, online nothing (the race goes on,
finding 215). The pure rule's test: `test_fps away` (17 s away online is owed without the rebase,
gone with it).
Online and iOS's suspension: the app is suspended a few seconds after it leaves the screen; its
sockets then stop. A short absence (seconds) keeps the room's WebSocket and the P2P links; a long
one (beyond the room's and WebRTC's timeouts, about 30 s) loses them, and the room treats the
player as gone, as when a phone loses its network. Not run here (no second player in the lane).

**3. The previous run.** iOS keeps no exit record an app can read, so the app keeps its own marker
(`Documents/dhd-run.txt`: launcher, game, inactive, background, ended with host_end's code) and the
game's crash report at the end of dhd.log is the sign of a crash, as on Android 10.
`run_record.c`'s `run_judge` words them as Android does ("how it ended: normally (...)", "closed
unexpectedly, it crashed (...)", "system record: none (iOS keeps none an app can read); the app's
own: ..."), into `dhd-exit.txt`, which the game quotes as `previous run:` lines
(`phone_quote_last_exit`, Android's quote moved to `phone.c`); after a crash or a stop on the
screen the launcher shows its note with Share log once. MetricKit was left out: its reports come a
day late or at the next launch, after the game's first lines, and not on a sideloaded build in the
Simulator; the marker covers the cases a player needs.

**4. Gamepad prompts on a touch screen.** Cause: the iOS Simulator offers a virtual controller
(SDL names it "Gamepad", standard) whether or not one is attached (the lane's Mac has no controller
connected: `ioreg`, Bluetooth); a real iPhone lists a controller only when one is paired, but a
paired pad left in a bag would do the same. Fix (shared, `touch_host_pad_prompts`): on a touch
screen a controller's button names (the icons, SETTINGS' Tabs line) apply once it has sent input
since the last touch, and touch's words come back at the next touch. Desktops are unchanged.

**5. SETTINGS' rows.** iOS set no `DHD_PC_SETTINGS_ROWS`, so it showed every desktop row. Android's
list moved to `phone.c` (`PHONE_SETTINGS_ROWS`, `phone_settings_rows`); both phones call it.
`test_launcher_ios.py` checks the list has no desktop row and nobody else names a phone's rows.

**6. Smaller notes.** The flight over the track: the scripted run's press during the flight was
✕ (autopad `4000` at field 2702, 6.6 s into the flight, `log-B1.log`), and the flight is skipped by
START (finding 209's SKIP sends START): the script's button, not iOS. The three drawing notes were
lane 226's and it fixed them before this lane merged it in (its finding: the top buttons above
the action cluster, `cutout_shift`, SKIP under the action thumb).

**7. The tap run.** SETTINGS: its legend is the page's own text (`p->footer`), and only the game's
PREV / NEXT / BACK / SELECT legend answered a tap; now the words of our pages' own legends do too
(Back, Undo, Keep, Clear; `menu_page.c`), on every platform. iOS also gets a Back: a swipe in from
the left edge outside a race is Android's Back (`touch_back`). The code field: the keyboard's bar
has Cancel (the field's Esc) and Done. The quit dialog: (see "What was run").

## Part 2: the launcher's iOS bridge

`port/ios/app/launcher.m` (the web view and what only UIKit does) and
`port/ios/host/launcher_calls.c` (the calls, plain C over the shared code; `BRIDGE.md`, "iOS").
The page is the desktop's and Android's `port/launcher/ui`, packed by `build_ios.py` as `launcher/`.

- One origin, `dhd-launcher://app/`, served by a `WKURLSchemeHandler` from the bundle with
  Android's content security policy; anything else 403, no navigation, a non-persistent store.
- The transport: WKWebView can't post a `MessagePort` to a page, so `bridge_ios.js`, injected at the
  document's end in a content world of its own, makes the channel, posts one end to the page with
  "dhd-bridge" (the page's `bridge.js` unchanged) and relays to the `dhd` message handler of that
  world; answers and events come back through `dhdDeliver` in that world.
- Calls: all of BRIDGE.md's phone calls (snapshot, settings_schema, save_setting, rider_name,
  check_update with latest.json's `ios` key, check_disc, pick_disc / retry_import /
  delete_original with the disc-import events, play, open_link, share_log, licences) and the
  `resumed` event. Nothing of BRIDGE.md is left out.
- Deleted: lane 224's first-start disc screen, its own ISO check (a second implementation of
  host.c's), the Documents scan and the import (`ios_disc_*`); `host_disc_check` now builds on iOS.
- "Edit the touch layout": `DHD_TOUCH_EDIT_AT_START=1`, as Android's launcher (lane 225's way);
  lane 226's `DHD_TOUCH_EDITOR=1` (the editor alone, the game ends after Done) is not used: on iOS
  that end closes the app, as EXIT GAME does, and Android does not use it either (one path; see
  Decisions).
- EXIT GAME: `host_end`, the app closes (lane 224's choice kept): a second core can't start in one
  process (the guest memory is reserved once), and a relaunch from inside the app is not possible
  on iOS; the next start opens the launcher.

## The review (`review-090`, the lead's outside review of the merged iOS platform)

1. and 2. (high, medium): lane 224's `ios_disc_validate` used a closed `FILE` on a SYSTEM.CNF
   whose BOOT2 named nothing and accepted any file over 100 MB naming the serial. It was already
   deleted with the disc screen (4f244b64); the disc check every platform makes moved into
   `port/host/disc_check.c` and now requires the whole declared volume (c83d7d4d): iOS's picker
   import (the source before copying, the copy after), a file put in Documents through Files (moved
   into place by the same import), the launcher's snapshot and every start go through
   `host_disc_check` with it; Android too. `test_disc_check`: a cut image that kept its header, a
   SYSTEM.CNF with `BOOT2 =` and nothing after, an over-long boot name, another game, no SYSTEM.CNF,
   no volume; each a clean answer. Not done: a file dropped into Documents under another name is not
   found by itself any more (lane 224 scanned Documents): the launcher's picker lists it (On My
   iPhone > Downhill Revived) and moves it into place.
3. (medium): `port/ios/ipa_audit.py`, the one audit of every release IPA (45b8b9cd); negative tests
   through `ios_release` itself (`test_ipa_audit.py`).
Risk: the background waited for the game thread to stop (2a7e59f1); replaced in round 2 (below) by a
hold of the render thread set by the callback itself (ea901d3f).

## What was run

On the lane's Mac (M1, 8 GB, Xcode 26.5, the iOS 26.5 Simulator), with the Windows build's core
parts (wasm2c, 8 parts) and the branch; the Simulator app built without warnings (the last build:
2a7e59f1). Sound: `SDL_AUDIO_DRIVER=dummy` in each device's launchd environment for every launch,
unset after. Evidence is named S (a `simctl io` screenshot), T (driven by real taps, mobile-mcp,
its screenshots) or L (dhd.log or the app's log only).

"DHD Lane 224" (iPhone 17 Pro Max):
- The launcher, every page and Settings tab, portrait then landscape (`turn landscape` in the test
  file), through the page's own bridge calls (`DHD_LAUNCHER_TEST`): S (A01-A13). The calls' answers:
  snapshot (version 0.8.0, the six caps, `disc_problem` null, the disc's size, `scale_max` 6),
  `settings_schema` (59 settings), `rider_name` ("COSMO THE GR"), `check_disc` (the copy, usa),
  `licences` (the GPL text), `check_update` (up to date, the releases page), `open_link` to another
  site refused: L. The Game tab shows the existing disc as "copied into the app: 2.4 GB": S.
- Settings round trip: `save_setting perf_counter 1` through the bridge, the snapshot read it back,
  PLAY by `bridge.call('play')`: the game's settings line says `PERF_COUNTER=1` and the title shows
  the FPS counter: L, S (B01, B02). Restored after (the device's dhd.ini put back).
- PLAY into the game in landscape (2868 x 1320); the title says PRESS ✕ TO START with the
  Simulator's "Gamepad (standard)" as player 1: S (B02).
- A kill on the screen (`simctl launch --terminate-running-process` over the running game), the
  next start: the launcher's note "It stopped while on the screen without a crash report ..." with
  Share log: S (C01); dhd-exit.txt "how it ended: closed unexpectedly, it stopped while on the
  screen ...": L. After EXIT GAME: "how it ended: normally (...)", the marker `ended`, code 0: L;
  the next game's `previous run:` lines: L.
- By taps (T, `t01`-`t26`): the memory card notice left by the edge swipe (`touch log: back`); the
  main menu's legend ✕ SELECT △ BACK (touch words); SINGLE PLAYER, the rider, START NEW GAME, SINGLE
  EVENT, RACE, the bike shop's START, the track, SKIP, a race; the app sent to the background
  (Settings app to the front) for 12.8 s and brought back: the game's PAUSED panel on screen (S
  G01), log "gs: in the background; the game waits", "back in the foreground after 12.8 s",
  "touch: back from the background in a race: the game's pause", no freeze line; QUIT RACE, YES;
  OPTIONS > SETTINGS: "Tap a tab", the Display tab without Borderless or Window Size (Picture,
  Widescreen, Frame Rate, V-Sync, FPS & Ping); the legend's "Back" tapped: out of SETTINGS;
  ONLINE > Join With A Code, "ABC" typed, the keyboard's Cancel: back on the ONLINE page; EXIT
  GAME, Yes, Quit: the app closed to the home screen. The OPTIONS menu's own "△ BACK" did not
  answer a tap (the game's own list menus have no legend taps); the edge swipe did.
- The quit dialog at full size (E01-exit-dialog-full.png): the title's first D really is drawn
  like a C ("QUIT COWNHILL REVIVED?"); the D of REVIVED is right. Not a misreading; cause not
  found in this lane (see Open).
- A race by the nav route (`DHD_NAV`, autopad), backgrounded and back: L, "the time away is no game
  time" (game 13.8 s against wall 29.9 s across 15.8 s away; clock lines with no step over two
  fields, no fast-forward); the pause cannot show there: a scripted pad (`DHD_NAV`) replaces the
  live pad and touch entirely (`pad.c` live), which is why the pause was proven by taps.

"DHD Lane 224 iPad" (iPad Pro 13-inch, iPadOS 26.5):
- The launcher upright and in landscape: S (P01-P04). With `UIRequiresFullScreen` the game's
  window in an upright device was the screen's landscape size shown scaled (P05, the first run's
  band) and the request to turn was refused ("the current windowing mode does not allow
  programmatic changes"); without it and the device turned to landscape (mobile-mcp `orient`), the
  game fills the screen (P07, 2752 x 2064) and in a race the Classic pad sits at the screen's edges
  with SELECT/START clear of the speedometer (lane 226's tablet places): S (ipad-C01-race,
  ipad-C03-back). The quarter turn in an upright full-screen window logged once
  ("turned a quarter in a portrait window") before the system's window became landscape: L; not
  seen on a screenshot in an upright full-screen window.
- Background in a race and back: L, in order: "gs: in the background; the game waits (its drawing
  done, no surface)", "ios: off the screen: the game and its renderer stopped in 41 ms", "ios: in
  the background: ... in 0 ms", "back in the foreground after 18.7 s", the pause asked.

Windows: `check.py port` OK on 2a7e59f1 (18 steps); the seven state traces against
`main-524948ba-ref-v3` on e5300b78's build (the commits after it up to a45e2dd1 touch iOS files only): 7 of 7 PASS (600, 5000, 5 x 14000 rows), and boot and
race again on 2a7e59f1 (the shared disc check and the background wait): PASS. The Android debug APK:
see Result.

## Decisions

- The bridge's message handling in C, not Objective-C: the page's calls are the shared code's,
  and a C file runs in `test_runtime.py` with a fake page on any machine.
- A content world for `bridge_ios.js`, not the page's: the page's scripts see no message handler.
- iPad: every orientation declared and the game turned in a portrait window, rather than a lock
  iPadOS 26 may ignore; the launcher turns with the device.
- The Back on iOS is an edge swipe outside races plus tappable legends, not a new overlay button:
  no drawing change in lane 226's overlay, and the same legends answer taps on Android.
- The editor entry stays `DHD_TOUCH_EDIT_AT_START` on both phones: with `DHD_TOUCH_EDITOR` the game
  ends after Done, which on a phone closes the app instead of showing the launcher again.
- EXIT GAME closes the app (`host_end`), as lane 224 did: a core can't start twice in one process,
  and iOS gives an app no way to relaunch itself; the next start opens the launcher.
- The whole-volume rule went into the shared check, not an iOS one: Android and the desktop's
  start get it too (a cut image fails at the start with its own message instead of in a load).
- No `UIRequiresFullScreen`: on iPadOS 26 it gave the app a scene of the screen's landscape size
  shown scaled into an upright screen; without it the window is real.

## Surprises

- Python `\\n` in a heredoc reached the files as real line breaks through the tool's quoting; C
  edits with escapes went through the editor instead.
- `fps.c` is in the core, not the host: present.c could not rebase the clock directly (its link
  failed on the core's symbols); the core asks the host once a field instead
  (`host_service_host_came_back`).
- WKWebView's `callAsyncJavaScript` in the page's world is not held by the page's content security
  policy, so the test switch can run expressions without `unsafe-eval`.

## A person's checklist (the taps nobody could script; Simulator or a real iPhone)

1. Start the app: the launcher opens (portrait or landscape, whichever the phone is held), the
   footer says "Up to date" or names a newer release.
2. Settings > Game > Disc image > Replace: the Files picker opens; pick the `.iso`: checking,
   copying with a bar, then the offer to delete the original (Delete it only if you want to).
3. Settings > Touch > Edit layout: the game starts and opens the layout editor over its menus;
   move a control, Done: the game goes on to its menus; the next Edit layout shows the move kept.
4. Back in the launcher (start the app again), switch "Open the launcher at every start" off,
   close the app from the app switcher, start it: the game starts directly.
5. In the game: OPTIONS > SETTINGS > Game > Launcher At Start ON; tap Back in the line at the
   bottom: SETTINGS closes. Next start: the launcher.
6. In any game menu swipe in from the left edge: the menu goes back (the memory card notice
   cancels, OPTIONS returns to the main menu).
7. Start a race, then go to the home screen and come back: the game's PAUSED panel is up; RESUME
   goes on from where you were (the race clock didn't run on).
8. In a race tap the touch Pause: the pause rows (RESUME, RESTART RACE, QUIT RACE, CONTROLS, CODES,
   BAIL OUT) each answer one tap.
9. ONLINE > Join With A Code: type a few letters, tap Cancel above the keyboard: back to ONLINE,
   nothing sent. Again with a real code from a friend's P2P race and Done: the room opens.
10. In a P2P room tap READY: the line says ready; leave with the room's own Leave.
11. With a Bluetooth controller: press a button on it: the prompts become its buttons (A / B or
    ✕ / ○); touch the screen: they turn back into touch's words.
12. Launcher > Help > Share log file: the share sheet with one text file; cancel.
13. Force a stop (the app switcher while the game is on screen is "normal"; a crash is not
    reproducible on purpose): after any unexpected end the next start's launcher shows "closed
    unexpectedly" with Share log once; Dismiss hides it.
14. iPad: hold it in landscape: the game fills the screen with the pad at the edges; upright: the
    game turns a quarter at the whole screen (turn the iPad); in a Stage Manager window it fills
    the window.

## Open

- The game's own list menus (OPTIONS, the main menu) don't take a tap on their legend's △ BACK;
  on iOS the edge swipe goes back there. A tappable legend there would be the same on Android.
- "QUIT COWNHILL REVIVED?": the first D of the quit dialog's title is drawn like a C (the iOS
  Simulator, full size); the cause (a glyph pair in the game's font, the sharp font's copy, or the
  title's style) was not found here.
- Not seen here: a real iPhone or iPad, a P2P room with a second player, the online race after a
  long background (the room's and WebRTC's timeouts), the file picker's import by tap, the layout
  editor by drag (the tap tool has no held drag), the quarter turn on a screenshot.
- Strings: none were added to `en.json` (the iOS bridge reuses Android's codes and words).

## Round 2 (the review `sol-lane227`)

1. **Settings saved at once, PLAY before a save ended** (d162f052). Every page message ran on a
   concurrent queue, and `dhd_settings_save` reads, changes and writes the whole file with one fixed
   temporary name, so two saves could lose one and PLAY could start the game mid-write. The calls
   that touch dhd.ini (snapshot with its disc path correction, save_setting, play) take a ticket where
   the messages arrive one by one (the main thread) and run in that order on any thread; the game's
   own disc path correction takes one too (`launcher_settings_run`); after PLAY a settings call is
   refused. Long calls stay off it. `test_launcher_order` (24 saves and PLAY on 26 threads with a
   read-modify-write writer: no key lost, PLAY after every earlier save, a later save refused; it
   fails without the order), `test_bridge_ios` (unawaited saves then PLAY reach the app in order).
   For lane 228's crate fix: on iOS dhd.ini lives in Documents, visible in the Files app, so its
   temporary file shows there while it exists; the game's own saves (in-game SETTINGS) and the
   launcher never write at once on iOS now (the launcher refuses after PLAY), but the game writes
   from its own thread and the crate should still lock per path.
2. **The edge Back cancelled a steering finger** (9db4c6ce). The recognizer had no delegate, so
   `gestureRecognizerShouldBegin` was never asked. MetalGameView is the delegate now; touches are
   not delayed (begin or end); a recognized Back cancels only its own finger; a refused swipe is
   logged. `test_launcher_ios` fails without the delegate. By tap (mobile-mcp, round 2's build): the
   swipe's touches reached the game (`touch log: down ... up`, no cancel), but the swipe was not
   recognized as Back on the memory card notice either, unlike round 1's build (t04); the tool was
   failing under the Mac's memory pressure (`context deadline exceeded`) and its device
   orientation had flipped once. Not settled: whether the delegate stops a real edge swipe from
   being recognized in menus. The legend taps (Back) remain the menus' way back either way.
3. **The audit ignored names** (125ee253): member and folder names through the same patterns, in
   `ios_release` and the staged-app audit; the licence entries `ios_release` writes are audited too.
   New negative cases: a UDID file name, a private literal as a folder name (both routes), a
   symbolic link, a fat Mach-O with one signed slice; the name cases fail without the name scan.
4. **The background's acknowledgement** (ea901d3f). The platform's callback (iOS's resign-active and
   background, Android's lifecycle watch) holds the native render thread between pictures itself
   (`gs_render_hold`): it finishes the picture under way, takes no new one, and the fields stay
   queued with their uploads; iOS waits until it has none under way (`gs_render_quiet`, under the
   render queue's lock, each absence afresh: no stale flag across a flip) and does not wait for the
   game thread. The return lets it go (`app_away_wait` once the new surface is made, or
   `gs_app_back` when the game never got to its wait). Guaranteed: no new GPU work or present from
   the render thread after the callback returns. Not claimed: that GPU work submitted before has
   finished (it may still run; iOS lets it), nor anything about the GS worker's own exact Z-probe
   submits on iOS (not routed through the render thread; it blocks on the held queue within two
   fields). Forced slow case, `DHD_TEST_SLOW_AWAY=4000` in a race (the iPhone Simulator): "ios: off the
   screen: the renderer is held, no picture under way (0 ms)" came before "gs: test: the game thread
   reaches the background's wait 4000 ms late", then the wait, the return, the pause.
5. **A superseded import landing over a newer one** (73cab42e): each pick or retry begins a
   generation; an import renames (or moves a file from Documents) only while its generation is the
   newest, checked and done under one lock (`disc_publish`). `test_disc_copy`.
6. **Tests that drive the real return** (857d6775): the came-back flag is `app_away.c` and the
   kernel's reaction is fps.c's `port_clock_after_away`, which `test_fps away` now drives;
   `test_app_away`: one came-back per return, an offline race's pause and none online at the
   return (`touch_pad_lost`), finding 220's `sync_clock_stood` counting a 17 s absence once with
   stamps that neither jump back nor stall.
7. **"QUIT COWNHILL REVIVED?"**: not fixed. Ruled out: the text (exit_menu.c says DOWNHILL); the D
   glyph or its atlas cell (the same title draws REVIVED's D right in the same draw, and the JOIN WITH
   A CODE title draws CODE's D right); a cell clobbered by the button icons (they paint 0xA0-0xBA,
   not letters). What remains: the D is drawn wrong only where an italic glyph follows it closely
   (D then O), so the next glyph's quad over-drawing the D's right side (a blend or alpha-test
   difference of the sharp font's copy against the game's font) is the likely cause; checking it
   needs the same dialog with `font = original` and on Windows, not done in this round.

## Result

The iOS app opens into the shared launcher (portrait and landscape, every page, the phone calls
through the shared code), PLAY starts the game in landscape, EXIT GAME closes the app and the next
start says how the run ended; leaving the app in an offline race pauses it at the return with no
time caught up, and the background holds the render thread before the callback returns (round 2); an iPad's game
fills its real window; touch prompts until a controller is used; SETTINGS shows Android's rows and
can be left by touch; the code field can be cancelled. Shared changes: `phone.c` (Android's rows,
quote and update check), `app_away_wait` (Android gets the same pause and clock rule), our pages'
tappable legends, one disc check with the whole volume for every platform, one release IPA audit.

Checks: Windows `check.py port` OK on 2a7e59f1; the seven state traces equal the reference (see
What was run); the Android debug APK builds (`build_apk.py`, 33.7 MB, on 2a7e59f1; not installed:
no phone in this lane). The Mac: the Simulator app (no warnings), the device app and its unsigned
IPA (`package_unsigned.py`'s audit passed), `package_release.py --platform ios --ipa` passed every
check and wrote `DownhillRevived-0.8.0-ios-experimental.ipa` (14.5 MB). The release IPA, looked at:
Info.plist names the app only (`app.skuza.DownhillRevived`, the SDK), the executable is not signed
at all and has no entitlements (`codesign -d`), no `/Users/` path in the executable or any member,
no device id; source paths in it are relative (`port/...`, Rust's `cargo/registry/...` and
`/rustc/<hash>/...`).

What Android's phone gate should look at (shared code this lane changed): the background (now the
game's pause at the return in an offline race, the sound paused, no freeze line, the clock rebased),
the button names with a controller connected but unused (touch's words until it is used),
SETTINGS' legend words by tap, the disc import and start refusing a cut image, the launcher's update
line (`phone_update_find`), SETTINGS' rows (`phone_settings_rows`, the same list).
