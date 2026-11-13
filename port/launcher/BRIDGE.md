# The launcher's bridge

One launcher page (`ui/`) runs on every platform (finding 225). Everything that touches files, the
network or the game goes through one interface, `ui/bridge.js`: `bridge.call(name, args)` returns a
promise, `bridge.on(event, handler)` hears events. The names, arguments and results are the same on
every platform; the page learns what a platform can do only from the snapshot's `caps`.

| Platform | Native side | Transport |
|---|---|---|
| Windows, macOS, Linux | `src-tauri/src/main.rs` (Tauri commands and events) | `window.__TAURI__` |
| Android | `port/android/.../LauncherActivity.java`, `LauncherBridge.java`, `port/host/android_launcher.c` | a `MessagePort` |
| iOS (to do) | a `WKWebView` host with the calls below | a `WKScriptMessageHandlerWithReply`, or a `MessagePort` |

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
| `touch-editor` | shows "Edit the touch layout" (`play` with `edit_touch_layout`: the layout editor alone, then the launcher again) | | yes |

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
| `play` | phones: `{edit_touch_layout?}` | null | desktop: the launcher waits for `game-exited`; phones: the game takes the launcher's place. With `edit_touch_layout` the game starts as the layout editor alone (the argument `--touch-editor`, finding 226: muted, no movies, the editor over a plain background); its Done or Back saves `touch_edits` and ends the game as EXIT GAME does (`host_end`), and the launcher shows again with `reopen` set (below) |
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

## What the iOS bridge must implement

The iOS app (lane 224's platform) shows `ui/` in a `WKWebView` the same way: the files from the app
bundle under one origin of its own (a `WKURLSchemeHandler` for a custom scheme, with the content
security policy above as a response header), no other loads, no navigation. It reports
`["touch-settings", "disc-import", "direct-start", "share-log", "help", "touch-editor"]` (dropping a
cap it can't do yet) and implements the phone calls:

- `snapshot`, `settings_schema`, `save_setting`, `rider_name`: the C interface the Android side uses
  (`dhd_settings_values_json`, `dhd_settings_schema_json`, `dhd_settings_save`,
  `dhd_settings_rider_name` in `port/render/include/dhd_settings.h`) on the app's dhd.ini, so no
  Swift copy of the settings exists. `scale_max` is the phones' renderer limit (6).
- `pick_disc`, `retry_import`, `delete_original` and the `disc-import` events: a document picker for
  any file, the copy into the app's own folder with progress, the boot check before and after
  (`host_disc_check`, host.c) and the completeness rule of `DiscImage.java` (the copy holds the
  whole ISO 9660 volume); this replaces the iOS lane's own first-start import.
- `check_disc`: `dhd_disc_check` (`dhd_disc.h`) with `iso-check.txt` beside dhd.ini.
- `check_update`: `update_find` (`port/net/update_check.h`, the sources `UPDATE_SOURCE_SERVER` and
  `UPDATE_SOURCE_GITHUB`) with the platform's key in `latest.json` once releases have an iOS build;
  until then `{release: null, newer: false}` (the page says no package is published).
- `play`: start the game view; `DHD_LAUNCHER_CHECKED=1` when the page showed the update line. With
  `edit_touch_layout`: `--touch-editor` in the game's argv (or `DHD_TOUCH_EDITOR=1`), and when the game
  would end after the editor's Done, the launcher's page again with the next snapshot's `reopen` set to
  `"touch-editor"` (once). Android does it this way (finding 228): `LauncherActivity` stays below the
  game for this start only, the game's `host_end` ends the process, and Android starts the launcher
  again from its saved state, which sets `reopen`.
- `open_link`, `share_log` (a share sheet with dhd.log and dhd-previous.log), `licences`.
- After a run that closed unexpectedly, `last_exit` once (iOS has no exit record; the game's own crash
  report at the end of dhd.log is the sign, as Android 10 reads it in `CrashReport.logCrash`).
