# 102: dhd.ini in the browser through dhd_settings; the rider name's rule in the crate

Status: current
Checked: 2026-11-04

2026-10-28, branch `desk/settings-web` (from `desk/settings`). Finding 101 gave dhd.ini one
implementation, the Rust crate `dhd_settings` (`port/render/settings`), for the game and the
launcher; the browser kept its own copy of the rules in `port/web/www/settings.js`, and the rider
name's rule was written twice (`port/hle/online_menu.c`, `port/launcher/ui/app.js`). Both now come
from the crate.

## The browser's module

`dhd_render`'s web build can't carry the crate (finding 101: it lives in the graphics worker and
owns a window of the game's shared memory). So the crate is built on its own:

- `rustgs.py build --target wasm32-unknown-unknown --crate dhd_settings`: `cargo rustc -p
  dhd_settings --lib --crate-type cdylib`, `--locked --offline`, the same remapped paths, no
  wasm-bindgen and no link arguments (the renderer's memory layout is not its own). Gates: the crate
  has no dependency of any kind (`cargo tree -e all`), the include pre-check, the dep-info inputs
  (nothing outside the tree, CARGO_HOME, RUSTUP_HOME and the target dir) and `check_rust_lib.py` on
  the module. Its cargo target dir is `<target dir>/dhd_settings`: its flags differ from the
  renderer's wasm build, which would otherwise rebuild after it. Cached in the object cache like
  the others (a `crate` part in the key; the renderer's keys are unchanged).
- `port/web/build.py` runs it after the renderer and `wasm-opt -Oz` (except `--no-opt`):
  `build/web/dhd_settings.wasm`, about 60 KB, in the manifest. `serve.mjs` serves it;
  `check_web_layout.py` requires it and checks rule P1: it imports nothing, defines one memory,
  exported, neither shared nor memory64, no start function (a selftest fixture with the game's
  memory imported fails with P1).
- `src/web.rs` (wasm32 only): a C interface. The caller copies arguments into buffers from
  `dhd_settings_alloc`; each call leaves its result in the module's memory
  (`dhd_settings_out`, `_out_len`): `dhd_settings_disabled` (DHD_SETTINGS none/0),
  `dhd_settings_environment` (the game's start: `environment`, as JSON with the unknown keys),
  `dhd_settings_save` (`edited`: the new text, every other byte kept) and `dhd_settings_written`.
  The file functions (`read`, `save`, `create`, the C interface `c.rs`) are native only.
- `edited` (new in `lib.rs`) is the one place a save happens: the native `save` writes its result
  to the file, the browser stores it.

## settings.js now

It loads the module on the page (which shapes the canvas from widescreen before Start) and in the
game worker, and keeps:

- the storage: IndexedDB `downhill-revived-settings` / `ini`, one entry per DHD_SETTINGS name. The
  entry is now the bytes (a `Uint8Array`), so any text keeps every byte as on a PC; a text stored
  as a string before is read as UTF-8. The disc is never stored (a save of `iso` is answered 1 and
  dropped): it stays the player's own file, picked at each start;
- `BROWSER_PC_SETTINGS`, the rows the browser's PC SETTINGS shows (as before);
- `BROWSER_WITHOUT`, the variables neither a line nor a default sets in the browser because it has
  no such thing: the window, its size and the monitor (the canvas is the window, and widescreen.c's
  shape stays the page's), DHD_ASPECT (the canvas has one shape), V-Sync (the page's frames pace
  it), vibration, texture packs, Discord, the updater;
- `RENDERER_LIVE`, the variables the reactor follows while the game runs.

Gone: its key table (`PLAIN`), `BROWSER_DEFAULTS`, the parser, `lookup`, the line-keeping save and
its own header. The defaults, the variable names, the environment's precedence, `game_options` and
the locked rows (`DHD_ENV_OVERRIDES`, as `host.c` reads it) are the crate's.

What changes for players, now that the browser follows the crate:

- A setting's DHD_* spelling, the first line with a value, saves removing a setting's other lines,
  switch words (`on`, `off`, ...), the case of a choice: as on a PC (finding 101's disagreements 3,
  4, 5, 9 and 13, which the browser still had).
- A new store starts with the crate's header, not the browser's own two lines.
- With settings on, the player's defaults apply as on a PC whose launcher has made dhd.ini; the
  browser has no launcher, so it stands for that (no stored entry means a new dhd.ini, not the
  tools' run). The defaults it now sets beyond the old four (scale 2, widescreen on, 60, the sharp
  font): the race server, skip movies, the renderer, Smooth scaling and Unlock All off. The page
  already fixes DHD_SKIP_MOVIES and DHD_RENDER, so they stay, and the others are the core's own
  values; the browser has no online play.
- Locked rows: every setting's variable set before dhd.ini, as on a PC. Besides the page's
  `?env.NAME`, that is the worker's fixed DHD_ISO, DHD_SKIP_MOVIES and DHD_RENDER, none of them a
  browser row.

## The rider name

`src/name.rs`: `LETTERS` (A-Z, 0-9, space, dash), `MAX` (12), `RULE` (the hint's words), `UNNAMED`
(`RIDER`), `clean` (upper case, only those letters, at most 12) and `normalise` (`clean` without
spaces at either end; what is saved).

- The game: `table::c_table` adds `SETTING_NAME_LETTERS`, `SETTING_NAME_MAX` and
  `SETTING_NAME_UNNAMED` to `port/hle/settings_table.h`; `online_menu.c` takes its letters, length
  and unnamed name from them (no other change: the same values).
- The launcher: `settings_schema` gives the name its `rule` (letters, length, hint); `app.js` keeps
  what a field holds while the player types with it and shows the hint, and the new `rider_name`
  command gives the name saved. The two `maxlength="12"` are set from the rule.

## Checks (2026-10-28)

- `cargo test -p dhd_settings`: finding 101's corpus (8), the rider name (1), the generated header
  and `tests/web.rs`, which keeps `tests/web_corpus.json` equal to the crate's native results: 8
  cases (comments, a section, a key twice and CRLF; a saved key's other lines and its DHD_*
  spelling; a byte order mark, CP1252 and no final line ending; nothing stored yet, compared with
  the launcher's new dhd.ini; a value that needs quotes; values as the game reads them; unknown
  lines and DHD_* variables; variables already set), each with the start's variables, its unknown
  keys and the bytes after one save. `DHD_SETTINGS_BLESS=1` writes the file.
- `cargo clippy -p dhd_settings --all-targets` and `--target wasm32-unknown-unknown`: clean.
- `rustgs.py build --target wasm32-unknown-unknown --crate dhd_settings`: built (every gate), then
  a cache hit; `--target x86_64-pc-windows-msvc --crate dhd_settings` is refused.
- The launcher's `cargo test`: 8 pass, 1 ignored (as before).
- `build_wasm.py`: the core rebuilt from this branch (`online_menu.c` with the generated
  constants), 132 imports as before. `build.py`: the threaded browser build with the module
  (47,233 bytes after `-Oz`); `check_web_layout.py build/web`: OK (core, graphics, renderer,
  settings, vu; the settings module has its own 17-page memory and no import);
  `check_web_layout.py --selftest`: 7 fixtures, the new one failing with P1.
- `node port/web/check_settings.mjs --iso <the USA disc>`, headless Chrome, a fresh profile:
  - all 8 corpus cases give the native results through settings.js and IndexedDB (variables
    without `BROWSER_WITHOUT`, unknown keys, saved bytes); DHD_SETTINGS=none reads and stores
    nothing; `game_options` and the locked rows as natively;
  - the round trip: the first case's text (comments, `[video]`, `fps` twice, an unknown key, CRLF)
    stored, `scale = 3` saved: 10 of its 11 lines byte for byte, `scale = 2 ; two` became
    `scale = 3` (CRLF kept);
  - the game's own page: the same text stored, the game started with finding 086's DHD_NAV route
    to OPTIONS > PC SETTINGS, Right on Resolution and Cross. Its log shows `settings: unknown key
    foo` and `pc settings: Resolution: 3` (field 784, 14 s), and the stored bytes are the native
    save, the same single line changed.

## Open

- None for the settings. `BROWSER_WITHOUT` follows the browser's limits; a browser row that
  offers one of them later takes it out of the list.
