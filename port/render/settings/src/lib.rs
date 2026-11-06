//! dhd.ini, the player's settings (findings 064, 093, 101): the one implementation of its keys,
//! their defaults and choices, how the file is read and how a change is saved.
//!
//! - The game links it through dhd_render's static library (`native`) and calls the C interface
//!   (`c.rs`, port/render/include/dhd_settings.h) from port/hle/host.c: at the start each setting
//!   becomes the DHD_* variable the game reads (`environment`); REVIVED SETTINGS saves through it. The
//!   REVIVED SETTINGS page itself runs inside the game core, which has no Rust: it reads the choices from
//!   port/hle/settings_table.h, generated from this crate's table (`table::c_table`).
//! - The launcher (port/launcher) uses it as a Rust crate: its tabs show the table's choices and
//!   defaults, and it saves through `save`.
//! - The browser (port/web/www/settings.js) loads it as a small wasm module of its own on the page
//!   and in the game worker (`web`, built by rustgs.py with `--crate dhd_settings`); settings.js
//!   keeps only the text's storage (IndexedDB) and what the browser can't offer.
//! - The rider name's rule (`name`) is here too, for the launcher's rider card and, through
//!   settings_table.h, the game's online menu.
//! - The install id (`install`, finding 161): a random id kept in dhd.ini for the online server's
//!   bans; not a setting, never a variable or a log line.
//! - The keyboard and controller bindings (`controls`, finding 157): the `keyboard`, `pad1` and
//!   `pad2` lines' layouts and the rules of a change, for the game (through port/hle/
//!   controls_table.h and the C interface) and the launcher's Controls tab.
//!
//! Precedence: a DHD_* variable set in the environment overrides dhd.ini (the game's tools set
//! them); dhd.ini overrides the player's default; without dhd.ini the game keeps its built-in
//! values (the tools' runs), except for the settings marked `always`. DHD_SETTINGS names the file;
//! `none` or `0` there means no settings at all, read or saved.

#[cfg(not(target_arch = "wasm32"))]
pub mod c;
pub mod controls;
mod ini;
pub mod install;
pub mod name;
pub mod table;
#[cfg(target_arch = "wasm32")]
mod web;

pub use ini::{check, Ini};
pub use table::{find, Choice, Kind, Setting, OFFICIAL_SERVER, SETTINGS};

use std::path::Path;
// The files: native only (the browser's module keeps its text in IndexedDB, through settings.js).
#[cfg(not(target_arch = "wasm32"))]
use std::{
    fs,
    io::{self, Write},
    path::PathBuf,
};

/// The first lines of a new dhd.ini.
pub const HEADER: &str = "; Downhill Revived settings: one \"key = value\" per line. The launcher and\n\
; OPTIONS > REVIVED SETTINGS in the game write them here. DHD_* environment variables override them.\n";

/// DHD_SETTINGS=none (or 0): no settings file, the tools' rule.
pub fn disabled(path: &Path) -> bool {
    let name = path.as_os_str();
    name.eq_ignore_ascii_case("none") || name == "0"
}

/// dhd.ini's text, or None when there is no file (or settings are off).
#[cfg(not(target_arch = "wasm32"))]
pub fn read(path: &Path) -> Option<Ini> {
    if disabled(path) {
        return None;
    }
    fs::read(path).ok().map(|bytes| Ini::parse(&bytes))
}

/// The text after saving `key = value` (`Ini::set`) into `text` (None: no file yet, which starts as
/// HEADER). The file's `save` and the browser's store (`web`) both write this.
pub fn edited(text: Option<&[u8]>, key: &str, value: &str) -> Result<Ini, String> {
    let mut ini = Ini::parse(text.unwrap_or(HEADER.as_bytes()));
    ini.set(key, value)?;
    Ok(ini)
}

/// Saves `key = value` (`edited`) into the file. The new text goes to `<file>.new` first and then
/// takes the old one's place. Ok(false): settings are off.
#[cfg(not(target_arch = "wasm32"))]
pub fn save(path: &Path, key: &str, value: &str) -> io::Result<bool> {
    if disabled(path) {
        return Ok(false);
    }
    let old = match fs::read(path) {
        Ok(bytes) => Some(bytes),
        Err(e) if e.kind() == io::ErrorKind::NotFound => None,
        Err(e) => return Err(e),
    };
    let ini = edited(old.as_deref(), key, value).map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e))?;
    let mut temp = path.as_os_str().to_owned();
    temp.push(".new");
    let temp = PathBuf::from(temp);
    let written = (|| {
        let mut f = fs::File::create(&temp)?;
        f.write_all(ini.bytes())?;
        f.sync_all()?;
        drop(f);
        fs::rename(&temp, path)
    })();
    if written.is_err() {
        let _ = fs::remove_file(&temp);
    }
    written.map(|()| true)
}

/// A new dhd.ini (HEADER) when there is none: the game then starts on the player's defaults.
#[cfg(not(target_arch = "wasm32"))]
pub fn create(path: &Path) -> io::Result<()> {
    if disabled(path) {
        return Ok(());
    }
    match fs::OpenOptions::new().write(true).create_new(true).open(path) {
        Ok(mut f) => f.write_all(HEADER.as_bytes()),
        Err(e) if e.kind() == io::ErrorKind::AlreadyExists => Ok(()),
        Err(e) => Err(e),
    }
}

/// What the game sets at its start.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct Environment {
    /// DHD_* variables and their values, each once, none the environment had already.
    pub vars: Vec<(String, String)>,
    /// Keys of dhd.ini lines that aren't settings (the game logs them).
    pub unknown: Vec<String>,
}

/// The game's start: dhd.ini (None: no file) as DHD_* variables. `is_set` says whether the
/// environment has a variable already; it wins. DHD_ENV_OVERRIDES lists the settings' variables
/// the environment set (",DHD_SCALE,DHD_FPS,"), which REVIVED SETTINGS shows locked.
pub fn environment(ini: Option<&Ini>, is_set: impl Fn(&str) -> bool) -> Environment {
    let mut out = Environment::default();
    let mut overrides = String::from(",");
    for s in SETTINGS.iter().filter(|s| !s.env.is_empty() && is_set(s.env)) {
        overrides += s.env;
        overrides += ",";
    }
    out.vars.push(("DHD_ENV_OVERRIDES".into(), overrides));
    let put = |vars: &mut Vec<(String, String)>, name: &str, value: &str| {
        if !is_set(name) && !vars.iter().any(|(n, _)| n.eq_ignore_ascii_case(name)) {
            vars.push((name.into(), value.into()));
        }
    };
    // the window size's default is the pair's: only when neither side has a value anywhere
    let size_named = |k: &str| ini.is_some_and(|i| i.has(k)) || table::find(k).is_some_and(|s| is_set(s.env));
    let size_free = !size_named("width") && !size_named("height");
    for s in SETTINGS.iter().filter(|s| !s.env.is_empty()) {
        match ini.and_then(|i| i.written(s.key)) {
            Some(v) => put(&mut out.vars, s.env, s.canonical(&v).unwrap_or(&v)),
            None if (ini.is_some() || s.always)
                && !s.default.is_empty()
                && (!matches!(s.key, "width" | "height") || size_free) =>
            {
                put(&mut out.vars, s.env, s.default)
            }
            None => {}
        }
    }
    if let Some(ini) = ini {
        for (k, v) in ini.entries() {
            let dhd = k.get(..4).is_some_and(|p| p.eq_ignore_ascii_case("DHD_"));
            if dhd && !v.is_empty() && table::find(&k).is_none() {
                put(&mut out.vars, &k, &v);
            }
        }
        out.unknown = ini.unknown_keys();
    }
    out
}
