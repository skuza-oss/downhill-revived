# The launcher's bridge

One launcher page (`ui/`) runs on every platform (finding 225). Everything that touches files, the
network or the game goes through one interface, `ui/bridge.js`: `bridge.call(name, args)` returns a
promise, `bridge.on(event, handler)` hears events. The names, arguments and results are the same on
every platform; the page learns what a platform can do only from the snapshot's `caps`.

| Platform | Native side | Transport |
|---|---|---|
| Windows, macOS, Linux | `src-tauri/src/main.rs` (Tauri commands and events) | `window.__TAURI__` |
| Android | `port/android/.../LauncherActivity.java`, `LauncherBridge.java`, `port/host/android_launcher.c` | a `MessagePort` |
| iOS | `port/ios/app/launcher.m` (the web view, UIKit's parts), `port/ios/host/launcher_calls.c` (the calls) | a `MessagePort` that `port/ios/app/bridge_ios.js` posts |

## Transport on a phone

The page is the app's own files, served under one origin (Android:
`https://appassets.androidplatform.net/launcher/`, from the APK's `assets/launcher/`). After the page
has loaded, the app posts the text `"dhd-bridge"` with one `MessagePort` to that origin only. The page
sends JSON text `{"id": n, "call": "name", "args": {...}}`; the app answers
`{"id": n, "ok": true, "value": ...}` or `{"id": n, "ok": false, "error": ...}`, and sends events as
`{"event": "name", "payload": {...}}`. No other page can reach the bridge, the web view loads nothing
else (every other request is refused, navigation is blocked), and the page's content security policy
allows its own files only (`default-src 'self'`, no inline script, no network).

An error is either a string (shown as it is) or `{"code": "<string table key>", "params": {...}}`,
which the page words in the player's language (`ui/lang/en.json`).

**Order of the settings writes (finding 228).** `bridge.js`'s handoff gate: a setting change the
page makes is one operation from its first write to its last (`bridge.operation`: Performance mode's
three keys, a tab's reset, the rider name's check and save). `play` and `install_update` are sent
only when no operation is in progress and no `save_setting` / `controls_edit` is unanswered,
looked at again after every change; once a handoff has begun, a new operation is refused
(`footer.handoffBusy`), no write is sent after the handoff went out, and a second handoff (PLAY twice,
the editor row during the transition) is the same one. So a game never starts on a dhd.ini the
launcher is still writing, or with only part of a change applied. A handoff that can't go within 5 s
(a save that never answers: a stuck native call, a broken port) is cancelled, not performed: its
promise rejects with `{code: "footer.handoffStuck"}`, the page says so with a Try again, and the
outstanding save stays pending, so the next try waits for it again. A phone's native side runs `snapshot`
(which may save `iso`), `save_setting` and `play` one at a time in the order they arrived (Android:
`LauncherBridge.ordered`, one single-thread queue) and keeps long calls (`check_disc`'s hashing,
`check_update`, the picker) off that queue; the settings crate's `save` is itself one transaction at
a time per process, with a temporary file of its own. iOS (`launcher.m`) runs every call on a
concurrent queue: the crate's lock and the page's wait before `play` cover its saves; an ordered
queue there as on Android would also keep a `snapshot` from reading between two saves.

## Capabilities (`snapshot().caps`)

| Cap | The page then | Desktop | Phone |
|---|---|---|---|
| `window` | draws its own title bar (minimize, close) | yes | |
| `pc-settings` | shows the PC's settings rows (display, window size, V-Sync, texture pack, server, Discord) | yes | |
| `bindings` | shows the Controls tab (keys and controller buttons, the stick picture) | yes | |
| `server` | asks `server_status` and shows the race server's line | yes | |
| `install-update` | installs a newer release itself (`install_update`) | yes | |
| `patch-notes` | shows the Patch notes page and the release notes on Play | yes | |
| `close-game` | stays open while the game runs (`game-exited`, `stop_game`) | yes | |
| `touch-settings` | shows a touch device's settings (resolution up to `scale_max`, performance mode, widescreen, the Touch tab) | | yes |
| `disc-import` | imports the disc image into the app (`pick_disc` starts a copy; `disc-import` events) | | yes |
| `direct-start` | shows "Open the launcher at every start" (setting `launcher`) | | yes |
| `share-log` | offers Share log (Help page, the crash note) | | yes |
| `help` | shows the Help page (version, links, licences) and drops the footer's links | | yes |
| `touch-editor` | shows "Edit the touch layout" (`play` with `edit_touch_layout`; what each phone does: "`play` with `edit_touch_layout`" below) | | yes |

## Calls

| Call | Args | Value | Notes |
|---|---|---|---|
| `snapshot` | | `{version, caps, native_window_controls, settings, game_found, running, disc_problem, texture_pack, played, scale, scale_max?, disc_size?, free_space?, last_exit?, reopen?}` | `settings`: every setting's value (dhd.ini's, else its default), the settings crate's `values_json`. `disc_problem`: why the disc can't be used, else null. `scale`: the last run's resolution report or null. Phones add `scale_max`, `disc_size`, `free_space` (bytes), `last_exit` (`{why}` once after a run that closed unexpectedly, else null) and `reopen` (`"touch-editor"` once when the launcher shows again after the layout editor alone, else null: the page opens Settings at the Touch tab). |
| `settings_schema` | | `[{key, default, choices: [[value, label]], rule}]` | the settings crate's `schema_json`; `rule` is the rider name's `{letters, max, hint}` |
| `save_setting` | `{key, value}` | null | through the settings crate (`dhd_settings::save`): the other lines kept |
| `rider_name` | `{text}` | the name saved (`""` none) | `dhd_settings::name::normalise` |
| `check_update` | | `{release: {version, page, notes?, published?, size?} or null, newer}` | phones: the game's own check (`update_find`, `port/net/update_check.c`); an error `footer.checkFailed` when no source answers |
| `check_disc` | | `{path, usa}` | the online check (`dhd_disc`, with the game's `iso-check.txt`); desktop sends `disc-progress` while it hashes |
| `pick_disc` | | desktop: the path chosen, or null; phones: `{job}`, or null when cancelled | phones then send `disc-import` events for that job |
| `play` | phones: `{edit_touch_layout?}` | null | desktop: the launcher waits for `game-exited`; phones: the game takes the launcher's place. With `edit_touch_layout`: the touch layout editor, then the launcher again on Android, the game's menus on iOS ("`play` with `edit_touch_layout`" below) |
| `open_link` | `{url}` | null | `"discord"`, `"kofi"` or a URL under the project's pages only |
| `install_update` | `{release}` | | desktop (`install-update`) |
| `stop_game` | | | desktop (`close-game`) |
| `server_status` | | `{races, riders, min_version}` | desktop (`server`) |
| `open_folder` | | | desktop: the game's folder |
| `controls_schema`, `controls_state`, `controls_edit` | | | desktop (`bindings`), finding 157 |
| `retry_import` | `{job}` | `{job}` (a new job) | phones (`disc-import`): the same document again, after "not enough space" |
| `delete_original` | `{job}` | true when deleted | phones: the document the copy came from, when its provider allows |
| `share_log` | | null | phones (`share-log`): the system share sheet with the logs |
| `licences` | | text | phones (`help`): the licence texts the app carries |

## Events

| Event | Payload | From |
|---|---|---|
| `update-progress` | `{done, total}` | desktop, while `install_update` downloads |
| `disc-progress` | `{path, done, total}` | desktop, while `check_disc` hashes |
| `game-exited` | `{code, wants_update, played}` | desktop |
| `disc-import` | `{job, stage: "checking"}`, `{job, stage: "copying", done, total}`, `{job, stage: "done", deletable, size}`, `{job, stage: "problem", problem}` | phones (`ui/disc_flow.js` follows them) |
| `resumed` | | phones: the app came back to the front (the page reads the snapshot again) |

## iOS (finding 227)

The iOS app shows `ui/` in a `WKWebView` the same way: the files from the app bundle's `launcher/`
folder under one origin of its own, `dhd-launcher://app/` (a `WKURLSchemeHandler` that sends the
content security policy above as a response header and answers 403 to anything else), no other
loads, no navigation, a non-persistent data store. WKWebView has no call to post a port to a page,
so the app injects `bridge_ios.js` at the document's end in a content world of its own: it makes
the `MessageChannel`, posts one end to the page's window with `"dhd-bridge"` (the page's `bridge.js`
is the same as on Android) and carries the page's messages to the app's `dhd` message handler of
that world, and the app's answers and events back (`dhdDeliver`, called in that world). The page's
scripts see neither.

It reports `["touch-settings", "disc-import", "direct-start", "share-log", "help", "touch-editor"]`
and implements the phone calls in `launcher_calls.c`, over the code Android's launcher calls:

- `snapshot`, `settings_schema`, `save_setting`, `rider_name`: `dhd_settings_values_json`,
  `dhd_settings_schema_json`, `dhd_settings_save`, `dhd_settings_rider_name` on the app's
  Documents/dhd.ini; `scale_max` 6; `last_exit` once after a run that closed unexpectedly (the
  app's own run record, `port/ios/host/run_record.c`: iOS keeps no exit record an app can read).
- `pick_disc`, `retry_import`, `delete_original` and the `disc-import` events: the document picker
  for any file (opened in place), the boot check before and after (`host_disc_check`), the room,
  the copy with progress, `DiscImage`'s completeness rule (`port/ios/host/disc_copy.c`), the rename
  over the old copy; a file already in the app's Documents is moved into place instead of copied.
- `check_disc`: `dhd_disc_check` with `iso-check.txt` beside dhd.ini.
- `check_update`: `phone_update_find` (`port/host/phone.c`, the same as Android's) with latest.json's
  `ios` key.
- `play`: the game in the launcher's place (`DHD_LAUNCHER_CHECKED=1` when the page showed the update
  line). With `edit_touch_layout` iOS sets `DHD_TOUCH_EDIT_AT_START=1`: the editor opens over the
  game's menus and the game goes on after Done, because the editor alone would end the process,
  which closes an iOS app (it can't start itself again). This is where the phones differ: see
  "`play` with `edit_touch_layout`" below. `open_link` (the same list as the desktop's and Android's),
  `share_log` (the share sheet with one text file: the device, dhd-exit.txt, dhd.log,
  dhd-previous.log), `licences` (the app's LICENSE.txt and THIRD-PARTY-NOTICES.txt).
- `resumed` when the app becomes active again with the page on screen.

## `play` with `edit_touch_layout` (findings 226, 227, 228)

One call, one row on both phones; what follows Done differs, because only Android can bring a
launcher back after the game's process has ended:

- **Android**: the game starts as the layout editor alone (`--touch-editor` among the activity's
  arguments, `DhdActivity.getArguments`; finding 226: muted, no movies, the editor over a plain
  background). Its Done or Back saves `touch_edits` and ends the game as EXIT GAME does
  (`host_end`). `LauncherActivity` stays below the game for this start only; when the game's
  process ends, Android starts it again from its saved state, and the next snapshot's `reopen` is
  `"touch-editor"` once, so the page opens Settings at the Touch tab.
- **iOS**: the game starts with `DHD_TOUCH_EDIT_AT_START=1`: the editor opens over the game's menus
  once the start's movies are over (`touch_host.c`), and the game goes on after Done. An iOS app
  can't start itself again, so the editor alone's end would close the app; `reopen` is never set.
- The desktop has no `touch-editor` cap; `--touch-editor` / `DHD_TOUCH_EDITOR=1` also serve the
  tests (`docs/TESTING.md`).
