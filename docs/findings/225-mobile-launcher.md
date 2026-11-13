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

Out of scope: translating the game's own pages, the iOS bridge itself (described for the iOS
lane's follow-up in `port/launcher/BRIDGE.md`), the touch layout editor's own changes (lane 226).

## Progress

- [x] 2026-11-13: finding reserved (167d0e4b).
- [x] 2026-11-13: the settings crate makes the page's JSON (`dhd_settings::launcher`,
  `dhd_settings_schema_json`, `dhd_settings_values_json`, `dhd_settings_rider_name`); settings
  `launcher` and `language` (600e2bf8). SETTINGS on a phone: Launcher At Start (ba395697).
- [x] 2026-11-13: the page: string table (`ui/i18n.js`, `ui/lang/en.json`, `tools/lang_check.py`
  in `check.py docs`), bridge (`ui/bridge.js`), capabilities, the phone rows and pages, the disc
  import's states (`ui/disc_flow.js`), the responsive layout; the desktop reads the crate's JSON and
  reports its caps (b130111b). The desktop launcher's Play page captured on a hidden desktop:
  pixel-equal to main's (0.8.0's launcher, the same scratch data folder); every page and settings
  tab of main's page and the lane's under the same stand-in Tauri API in headless Edge at 960x600:
  0 differing pixels in all seven views. The launcher crate's tests: 30 passed.
- [x] 2026-11-13: Android: `android_launcher.c` (the native calls in libmain), the game's SDL disc
  picker removed, the layout editor hook (3d86bcf8); `LauncherActivity`, `LauncherBridge`,
  `DiscImport`, `Native`, `SetupActivity` deleted, shortcuts, `build_apk.py` packs the page
  (6084577f); docs and `port/launcher/BRIDGE.md` (7f698afb).

## The bridge

`port/launcher/BRIDGE.md` has the calls, events and capabilities. In short: the page calls
`bridge.call(name, args)` and hears `bridge.on(event)`; on the desktop that is Tauri's invoke and
listen, on a phone a `MessagePort` the app posts to the page's origin after it loads (JSON
`{id, call, args}` answered `{id, ok, value | error}`, events `{event, payload}`). The page asks
which platform it is on only through the snapshot's `caps` (`window`, `pc-settings`, `bindings`,
`server`, `install-update`, `patch-notes`, `close-game` on the desktop; `touch-settings`,
`disc-import`, `direct-start`, `share-log`, `help`, `touch-editor` on a phone).

The logic a phone needs is the desktop's own code, not a Java copy: the settings schema and values
come from the settings crate on both (the desktop's `settings_schema` and `snapshot` now return the
crate's JSON), saves go through `dhd_settings_save`, the rider name through `normalise`, the disc's
boot check is host.c's `host_disc_check`, the online check `dhd_disc_check`, the update check the
game's `update_find` (its two sources now named once in `update_check.h`). `Native.java` loads
them from libmain.so (`port/host/android_launcher.c`, exported through the version script next to
`SDL_main`); loading the library starts no game (its two constructors only print a line and set a
pointer).

## Android

- `LauncherActivity` is the entry (LAUNCHER intent, `screenOrientation="fullUser"`: portrait or
  landscape). It reads how the run before ended first (LastExit, before native code, as
  SetupActivity did); after a crash the page shows the note with Share log once (the dialog of
  finding 152 is gone). With the setting `launcher` off and a disc in place it starts the game at
  once (not after a crash: the note shows instead); the Launcher shortcut opens the page anyway.
- The WebView loads `https://appassets.androidplatform.net/launcher/index.html` (Android reserves
  that host for an app's own files), served by `shouldInterceptRequest` from `assets/launcher`
  with a CSP header (`default-src 'self'`, no inline script, `connect-src 'self'`); any other
  request gets 403, every navigation is blocked, file and content access are off, no
  `addJavascriptInterface`. The bridge's port is posted with that origin as the target, so only
  this page receives it.
- PLAY starts `DhdActivity` and finishes the launcher. EXIT GAME still ends the process through
  `host_end` (findings 210, 217): the app leaves, and the next start opens the launcher. The
  launcher opened from the icon while the game runs goes straight back to the game
  (`DhdActivity.started`: SDL_main runs once per process).
- `DiscImport` is SetupActivity's copy job (check before, room, copy with progress, check the copy,
  rename over the old copy in one step) sending `disc-import` events instead of drawing screens; a
  replaced disc drops `iso-check.txt`. `DhdActivity` checks the copy (DiscImage) before SDL
  starts and opens the launcher without it (a start that skips the launcher: a USB controller).
- The update line: the launcher asks `update_find` at every open (not once a day: the server's
  copy of latest.json answers first) unless `auto_update` is off; the game skips its own daily
  notice when the launcher showed the line (`DHD_LAUNCHER_CHECKED`). Nothing is downloaded.
- "Edit the touch layout" starts the game with `DHD_TOUCH_EDIT_AT_START=1`; `touch_host.c` opens
  the layout editor once, as TOUCH CONTROLS > Edit layout does, at the first field with no movie,
  attract demo, race HUD or replay, two seconds into the run at least. Lane 226 may move that
  moment (the main menu) or the editor's own exit; the hook is the variable.
- No new permission: the WebView needs none for local files, INTERNET was there for the update
  check.

## Strings

`ui/lang/en.json`: 235 strings with stable dotted keys (`row.scale.hint`, `footer.upToDate`),
whole sentences with named placeholders (`{version}`), `**bold**` spans, counts as `.one` /
`.other` (Intl.PluralRules picks the form). `i18n.js` loads English and the chosen language
(dhd.ini's `language`, else the system's first language with a file: exact, a code it starts
with, the base, another file of the same language except across Chinese scripts), falls back to
English per key, and fills `data-i18n` attributes in index.html. `tools/lang_check.py` (in
`check.py docs`) fails on a language key the source lacks, a renamed placeholder, a lost bold span,
LANGUAGES not matching the files, or a key the page (or Android's `coded(...)` errors) names that
`en.json` lacks; `--list` lists each language's missing keys. Translators: `ui/lang/README.md`.
The Language row appears once a second language ships.

Not translated: the settings' choice labels (`4:3`, `Borderless`, `60 fps`) come from the settings
crate's table, the words the game's own SETTINGS shows; native messages in English (the disc
check's reasons from host.c, DiscImage and the desktop's disc.rs, the desktop's update errors).
Translating those needs codes from the native side, as the new Android errors already have.

The game's own pages are not translated: their font (the game's own and the sharp one) has Latin
letters only; translating them would need a font with the other scripts in the native renderer's
text path, the menus' fixed-width layouts reworked for longer words, and the
game's own menu strings (in the disc's data) replaced at run time.

## Decisions

- Same process for launcher and game, the launcher finishing on PLAY: EXIT GAME's `host_end` ends
  the process, so returning to the launcher would need a second process; leaving the app is the
  clean end on Android, and the next start shows the launcher.
- A MessagePort with a target origin instead of `addJavascriptInterface`: the framework's way to
  scope a bridge to one origin without androidx (the app has no androidx).
- Separate tab lists by capability (`needs` per row) rather than per platform: the desktop's tabs
  come out exactly as before (no Touch tab, no rows lost), a phone gets Graphics, Touch, Game,
  Online.
- Performance mode on a phone is one switch over the three effects (as the game's Performance
  Mode row): mixed values say so in the hint.
- The Language row is hidden until a second language exists, so the desktop launcher looks as it
  did.
- The update check at every launcher open on a phone, as the desktop does; a direct start keeps the
  game's once-a-day notice.
- The game's native disc picker went (one place to choose the disc); a disc missing in the game is
  only logged, since a launcher started from the game's thread would end with the game's process.

## Surprises

- Classic scripts share one scope: `app.js`'s `words` helper clashed with `i18n.js`'s and the
  page didn't start (the desktop capture showed the static HTML). `test_launcher_ui.js` now
  parses all the page's scripts as one script.
- WebView2 keeps its profile in the launcher's app data folder: two launchers at once (main's and
  the lane's on the hidden desktop) fail with "resource in use"; the captures set
  `WEBVIEW2_USER_DATA_FOLDER` per run.
- Headless Edge doesn't make windows narrower than about 500 px: the portrait previews were made
  in a phone-sized iframe.

## Result

Filled in at the merge.
