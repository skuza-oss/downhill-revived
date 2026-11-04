# 101: dhd.ini, one implementation (dhd_settings)

Status: current
Supersedes: parts of 056
Checked: 2026-11-04

2026-10-27, branch `desk/settings` (from `desk/integrate`). dhd.ini was parsed, defaulted and saved
by three separate readers: C in the game (`port/hle/host.c`'s settings code, with the choices in
`pc_settings.c`), Rust in the launcher (`port/launcher/src-tauri/src/ini.rs`, its choices and
defaults typed again in `ui/app.js`) and JavaScript in the browser (`port/web/www/settings.js`).
They had drifted apart (below). Now one Rust crate holds the meaning, and the callers keep only
where and how they show it.

## The crate

`port/render/settings` (`dhd_settings`), a member of the renderer's workspace with no dependencies:

- `table.rs`, the settings: key, the DHD_* variable the game reads, kind (text, choice, switch),
  the player's default, the game's built-in value (what it does with no variable, as in the tools'
  runs), the choices with their labels, what any other value stands for, and whether the default
  applies without a dhd.ini (only `server`, the official race server). 29 keys, `discord` (on/off,
  default on) among them for the Discord Rich Presence work that comes next.
- `ini.rs`, the file: kept as the bytes it was read from; a save rewrites only the lines of the
  setting it writes.
- `lib.rs`: `read`, `save` (a `.new` file, then a rename), `create` (the header), and `environment`,
  the game's start as DHD_* variables.
- `c.rs`, the game's C interface (`port/render/include/dhd_settings.h`): `dhd_settings_load`,
  `dhd_settings_save`, `dhd_settings_read`. The environment stays the caller's (get and set
  functions), so the C runtime's copy is still the one the game reads.
- `table::c_table` writes `port/hle/settings_table.h` for the PC SETTINGS page, which runs inside the
  game core (wasm, no Rust). `tests/generated.rs` fails while the committed file differs
  (`DHD_SETTINGS_BLESS=1 cargo test -p dhd_settings` writes it).

### The rules

- Spaces and tabs around a line, key or value don't count, nor does a UTF-8 byte order mark.
- `;` or `#` starts a comment line, `[` a section (ignored: keys are global). A line without `=` is
  kept and ignored.
- Keys compare without case. A setting's variable name is another spelling of its key
  (`DHD_SCALE = 3` is `scale = 3`).
- A quoted value is taken as it is. Otherwise a `;` or `#` that starts the value or follows a space
  ends it.
- **An empty value is no value**: the default applies.
- **When a setting has several lines, the first with a value counts.**
- **A save writes the setting's first line** (any spelling), keeping that line's ending, and
  **removes its other lines**, so the saved value is the one that counts. Without a line it goes at
  the end, with the file's line ending. Every other line stays byte for byte: comments, unknown
  keys, blank lines, CRLF or LF, text in another encoding, the byte order mark.
- Values that need it are quoted (`;`, `#`, `"`, a space at either end). A key is letters, digits
  and `_`; a value is one line.
- Values as the game reads them: a choice's own spelling in any case. For a switch,
  0/off/false/no and 1/on/true/yes; another value is on (`atoi != 0`; textures and discord: not an
  off word). For `renderer`, anything but native is exact. The variable gets that spelling;
  a value outside the choices (`widescreen = 21:9`, `scale = 12`) is passed on as written.
- **Precedence**: a variable set in the environment wins over dhd.ini, which wins over the default.
  The player's defaults apply only when dhd.ini exists; the tools run without one and keep the
  built-in values. The window size's default (`monitor`) applies to width and height together,
  only when neither is named. `DHD_ENV_OVERRIDES` lists every setting's variable the environment
  had (PC SETTINGS shows those rows locked).
- `DHD_SETTINGS=none` (or `0`): nothing read or saved.
- `size` is width and height together: `1280x960` or `monitor`.

## Who uses it

| Caller | Now | Deleted |
|---|---|---|
| The game's host (`host.c`) | `dhd_settings_load` at the start, `_save` for every setting the game writes, `_read` for `game_options`; dhd.ini's path and the locked check stay | the parser, the variable table, the player defaults, the override list, the 44-line template, the line-keeping save (238 lines out, 31 in) |
| PC SETTINGS (`pc_settings.c`, in the core) | each row's variable, built-in value and choices from `settings_table.h`; the page keeps its rows, labels, live flags and the lists that depend on the PC (scales up to DHD_SCALE_MAX, rates up to port_fps_max, the monitor's own size); the window size saves as `size` | its own choice tables, fallbacks and value spellings |
| The Android start (`android_main.c`) | `dhd_settings_read` / `_save` for the disc line | its own reader and template |
| The launcher (`main.rs`, `game.rs`) | `read`, `save`, `create`; `snapshot` sends every setting's value (dhd.ini's, else the default); the new `settings_schema` command sends every default and choice; the race server and update address follow the game's precedence (`Ini::effective`). The data folder and `DHD_SETTINGS` / `DHD_DATA_DIR` are unchanged (`Place::settings` is the old `ini::path`) | `ini.rs` (130 lines) and `OFFICIAL_SERVER` |
| The launcher's page (`app.js`) | the tabs keep their rows, labels, hints and controls; options, a switch's on and off values and Reset tab's defaults come from `settings_schema` | `DEFAULTS` and every `options` / `on` / `off` list |
| The browser (`settings.js`) | the crate's own wasm module since finding 102 (below) | its parser, tables, defaults and save (finding 102) |

The game links the crate in its one Rust static library, as it does `dhd_net`: `dhd_render`'s
`native` feature depends on it and re-exports it. `rustgs.py` keys its cache on the crate's sources
and applies its include gate to them. The launcher takes it as a path dependency, outside its own
workspace, so cargo names those sources by their full path: `build_launcher.py` now remaps the
repository to `dhd`, as it already did for the cargo and rustup homes.

## The disagreements fixed

1. `scale =` then `scale = 4`: the game and the browser used 4; the launcher kept the empty first
   value and showed its default, 2. Now 4 everywhere (the first line with a value).
2. `scale =` alone: the game took the line as present and skipped the player's default (DHD_SCALE
   unset, the built-in value), while the launcher showed the default. Now the default applies
   everywhere.
3. `DHD_SCALE = 3` with `scale = 2`: the game used 3; the launcher read only `scale` and showed 2.
   Now 3 everywhere, and saving `scale` replaces the `DHD_SCALE` line.
4. A save over several lines of a key replaced only the first. Saving an empty `server` over
   `server = a` / `server = b` left `b` in force. Now the other lines go.
5. Words for switches: `vibration = on` was off in the game (`atoi`) and off in the launcher (not
   `1`); `font = off` was the original font in the game but no choice in the launcher. Now one
   reading for both (on; original).
6. The locked rows: the game listed only 12 variables as environment overrides, so DHD_FPS,
   DHD_VSYNC and DHD_UNLOCK_ALL set outside showed as changeable. Now every setting's variable.
7. Precedence in the launcher: it took `update_url` before DHD_UPDATE_URL, and never looked at
   DHD_NET_SERVER. The game takes the environment first. The launcher does too now.
8. `DHD_SETTINGS=none`: the browser read and saved nothing, but the game read nothing and saved to
   a file named `none` in its folder. Now nothing is read or saved anywhere.
9. Line endings and the byte order mark: the game wrote CRLF (C text mode), the launcher LF, and
   each rewrote every line's ending on a save. A UTF-8 byte order mark made the first key
   unreadable to all three. Now each line keeps its bytes, and the mark is skipped and kept.
10. A new dhd.ini had three texts: host.c's 44-line template with values, the launcher's two
    comment lines, and Android's own with `auto_update = off` (which the game ignores and Android
    has no launcher for). Now the crate's header everywhere.
11. Choices and labels: the window sizes were five in the game and seven in the launcher (with
    2560x1440 and Monitor size). The labels differed too: `896 × 672` against `896x672`,
    `1x (PS2)` against `1x`, `60 fps` against `60`, Match window against Window. Now one list.
    The game shows the monitor's own size in place of Monitor size, as before. Font is listed
    Original then Sharp in both.
12. The window size with only `width` named: the game used width x 672; the launcher showed
    Monitor size. Now width x 672 in both.
13. Case in values: `window = Borderless` reached present.c as written, whose `strcmp` made it a
    window; the launcher showed no choice. Now the variable gets `borderless`, and both show it.
14. Reset tab wrote the launcher's own copy of each default. Now it writes the crate's, and empties
    the race server's line, so the official address stays the game's to name.

## The browser: what it needs

Done in finding 102 (the plan below, with a JSON result in place of a C list).

The browser still runs `settings.js`'s own copy of the rules. Its tables point at the crate now, and
its parse already followed rule 1. Wiring it in through `dhd_render`'s web build is out of
proportion here:

- in the default threaded build that module lives in the GS worker, not where the settings are
  (the game worker, and the page, which reads widescreen to shape the canvas before the worker
  starts);
- it imports the core's shared memory and owns the window 0x60000000-0x69000000 in it, so a
  second instance in the game worker would share its heap.

What it needs: `dhd_settings` built on its own as a small wasm32 cdylib with its own memory and
C-ABI exports (parse to a variable list, save one line, the table as JSON), no wasm-bindgen.
`settings.js` then keeps IndexedDB, `readIni` and `enabled` and calls it for everything else. That
means:

- a second wasm artifact in `rustgs.py` with its gates (`check_rust_lib.py` on the module);
- `build.py` copying it;
- `serve.mjs`'s allowlist and `check_web_layout.py`'s module rules for it.

Until then the browser keeps rules 3, 4, 5 and 9 its old way. Its own defaults (`BROWSER_DEFAULTS`)
are a subset by design: the canvas has one shape, no movies and no rumble.

## Checks (2026-10-27)

- `cargo test -p dhd_settings`: the corpus (8 tests: empty and duplicate keys, spellings, comments,
  sections and quotes, values as the game reads them, environment overrides and defaults, unknown
  lines, byte-identical saves with CRLF, a byte order mark, non-UTF-8 text and no final line
  ending, files and `none`, the C interface) and the generated header.
- The launcher's `cargo test`: 8 pass (its dhd.ini test kept, now through the crate), 1 ignored
  (the live GitHub source).
- `cargo clippy -p dhd_settings --all-targets`: clean.
- `rustgs.py build` for x86_64-pc-windows-msvc, aarch64-linux-android and x86_64-unknown-linux-gnu:
  built, every gate passed (versions, includes, check_rust_lib).
- `build_wasm.py`: the core rebuilt with the new `pc_settings.c` (one unit recompiled). Its 132
  imports are the same as the main checkout's core.
- `build_host64.py`: links (132 imports, 37 host objects).
- `build_launcher.py`: the release `DownhillRevived.exe` builds and passes its own check (no folder
  of this machine, the version), with the repository remapped.
- A Windows run, headless (no window), DHD_* cleared but for the run's own variables:
  - `DHD_SETTINGS` named an existing CRLF dhd.ini with a comment, a section, an unknown key, an
    empty then a set `fps`, and the disc in quotes;
  - the game took the disc, `skip_movies`, `memory_card` and the default renderer from it, and
    logged `settings: unknown key foo`;
  - a DHD_NAV route opened OPTIONS > PC SETTINGS (the cursor row by its word at 0x4C6DA8,
    D_004C6D98.x10, since rows the game disables are skipped), pressed Right on Resolution and
    Cross: `pc settings: Resolution: 3`;
  - after: the file differs from before in that one line only (`scale = 2 ; two` became
    `scale = 3`), with every CRLF kept and no `dhd.ini.new` left.

## Open

- The browser (above): done, finding 102.
- The rider name's rule (12 of A-Z, 0-9, space, dash): in the crate since finding 102.
