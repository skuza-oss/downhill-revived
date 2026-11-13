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
//! - The launcher's page (`launcher`, finding 225): the settings' schema and values as the JSON the
//!   page draws from, for the desktop launcher and, through the C interface, a phone's bridge.
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
pub mod launcher;
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

/// One save at a time in this process (finding 228): `save` reads, edits and replaces the whole
/// file, so two saves at once (the Android launcher's worker threads, the desktop launcher's
/// commands, the game's own pages) could both read the old text and one change would be lost.
/// Saves are rare and short: one lock for every path is enough.
#[cfg(not(target_arch = "wasm32"))]
static SAVING: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// A temporary file of its own for each save: `<file>.<process>-<n>.new`, so no other save (another
/// thread before this lock, or another process: the desktop launcher and its game) can write into
/// the file this one is about to rename.
#[cfg(not(target_arch = "wasm32"))]
fn temporary(path: &Path) -> PathBuf {
    use std::sync::atomic::{AtomicU64, Ordering};
    static NEXT: AtomicU64 = AtomicU64::new(0);
    let mut temp = path.as_os_str().to_owned();
    temp.push(format!(".{}-{}.new", std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
    PathBuf::from(temp)
}

/// How often and how long `replace` tries again: at most half a second in all.
#[cfg(not(target_arch = "wasm32"))]
const REPLACE_TRIES: u32 = 20;
#[cfg(not(target_arch = "wasm32"))]
const REPLACE_WAIT: std::time::Duration = std::time::Duration::from_millis(25);

/// Whether a failed replace may work a moment later: on Windows, another program holding the file
/// (an antivirus, an editor, our own launcher or game reading it) answers access denied (5), a
/// sharing violation (32) or a lock violation (33); Rust names only the first `PermissionDenied`,
/// the others come as raw OS errors. Elsewhere a refused rename doesn't change by waiting.
#[cfg(not(target_arch = "wasm32"))]
fn retryable(kind: io::ErrorKind, raw: Option<i32>, windows: bool) -> bool {
    windows && (kind == io::ErrorKind::PermissionDenied || matches!(raw, Some(5 | 32 | 33)))
}

/// `rename` until it works, a permanent error, or `REPLACE_TRIES` tries of a retryable one. `save`
/// reads the old file the same way (a program holding dhd.ini with no sharing at all makes the read
/// fail with a sharing violation too).
#[cfg(not(target_arch = "wasm32"))]
fn replace_with(mut rename: impl FnMut() -> io::Result<()>, windows: bool) -> io::Result<()> {
    let mut tries = 0;
    loop {
        match rename() {
            Err(e) if tries < REPLACE_TRIES && retryable(e.kind(), e.raw_os_error(), windows) => {
                tries += 1;
                std::thread::sleep(REPLACE_WAIT);
            }
            done => return done,
        }
    }
}

/// The new text takes the old file's place (`replace_with`'s tries on Windows).
#[cfg(not(target_arch = "wasm32"))]
fn replace(temp: &Path, path: &Path) -> io::Result<()> {
    replace_with(|| fs::rename(temp, path), cfg!(windows))
}

/// Saves `key = value` (`edited`) into the file: read, edit and replace as one step in this
/// process (`SAVING`). The new text goes to a temporary file of its own (`temporary`) and then
/// takes the old one's place, so a reader sees the old file or the new one, never a part.
/// Ok(false): settings are off.
#[cfg(not(target_arch = "wasm32"))]
pub fn save(path: &Path, key: &str, value: &str) -> io::Result<bool> {
    if disabled(path) {
        return Ok(false);
    }
    let _one_at_a_time = SAVING.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    let mut bytes = Vec::new();
    let read = replace_with(|| fs::read(path).map(|b| bytes = b), cfg!(windows));
    let old = match read {
        Ok(()) => Some(bytes),
        Err(e) if e.kind() == io::ErrorKind::NotFound => None,
        Err(e) => return Err(e),
    };
    let ini = edited(old.as_deref(), key, value).map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e))?;
    let temp = temporary(path);
    let written = (|| {
        let mut f = fs::File::create(&temp)?;
        f.write_all(ini.bytes())?;
        f.sync_all()?;
        drop(f);
        replace(&temp, path)
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
    let _one_at_a_time = SAVING.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
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

#[cfg(all(test, not(target_arch = "wasm32")))]
mod replace_tests {
    use super::*;

    #[test]
    fn what_is_tried_again() {
        use io::ErrorKind::*;
        // Windows: access denied, a sharing violation, a lock violation
        assert!(retryable(PermissionDenied, Some(5), true));
        assert!(retryable(Other, Some(32), true));
        assert!(retryable(Other, Some(33), true));
        // permanent ones fail at once
        assert!(!retryable(NotFound, Some(2), true));
        assert!(!retryable(Other, Some(112), true)); // the disk is full
        assert!(!retryable(Other, None, true));
        // elsewhere nothing waits
        assert!(!retryable(PermissionDenied, Some(13), false));
        assert!(!retryable(Other, Some(32), false));
    }

    /// `replace_with` over a list of answers: Ok(()) or an OS error code. Returns the result and
    /// how many renames were tried.
    fn run(answers: &[Option<i32>]) -> (io::Result<()>, usize) {
        let mut calls = 0;
        let result = replace_with(
            || {
                let answer = answers[calls.min(answers.len() - 1)];
                calls += 1;
                answer.map_or(Ok(()), |code| Err(io::Error::from_raw_os_error(code)))
            },
            true,
        );
        (result, calls)
    }

    #[test]
    fn injected_errors() {
        // a sharing violation twice, then the replace works
        let (r, calls) = run(&[Some(32), Some(32), None]);
        assert!(r.is_ok());
        assert_eq!(calls, 3);
        // access denied, then it works
        let (r, calls) = run(&[Some(5), None]);
        assert!(r.is_ok());
        assert_eq!(calls, 2);
        // a lock violation, then it works
        assert!(run(&[Some(33), None]).0.is_ok());
        // a permanent error: once, at once
        let (r, calls) = run(&[Some(2), None]);
        assert_eq!(r.unwrap_err().raw_os_error(), Some(2));
        assert_eq!(calls, 1);
        // a holder that never lets go: the bounded tries, then its error
        let t0 = std::time::Instant::now();
        let (r, calls) = run(&[Some(32)]);
        assert_eq!(r.unwrap_err().raw_os_error(), Some(32));
        assert_eq!(calls as u32, REPLACE_TRIES + 1);
        assert!(t0.elapsed() >= REPLACE_WAIT * REPLACE_TRIES);
    }

    /// Another thread holds `file` open with `share` (Windows' FILE_SHARE_* bits) for 200 ms, from
    /// before `work` starts; returns what `work` returned and how long it took.
    #[cfg(windows)]
    fn while_held<T>(file: &Path, share: u32, work: impl FnOnce() -> T) -> (T, std::time::Duration) {
        use std::os::windows::fs::OpenOptionsExt;
        use std::sync::{Arc, Barrier};
        let opened = Arc::new(Barrier::new(2));
        let holder = {
            let (file, opened) = (file.to_path_buf(), opened.clone());
            std::thread::spawn(move || {
                let f = fs::OpenOptions::new().read(true).share_mode(share).open(&file).unwrap();
                opened.wait();
                std::thread::sleep(std::time::Duration::from_millis(200));
                drop(f);
            })
        };
        opened.wait();
        let t0 = std::time::Instant::now();
        let out = work();
        let took = t0.elapsed();
        holder.join().unwrap();
        (out, took)
    }

    /// Real holders on Windows, each for 200 ms while a save or a replace runs: it waits and works.
    /// - dhd.ini open without delete sharing (an editor, a reader in another process): the rename
    ///   is refused with access denied (5);
    /// - the new text's temporary file open without delete sharing (an antivirus scanning the file
    ///   just written): the rename is refused with a sharing violation (32);
    /// - dhd.ini open with no sharing at all: the read before the edit fails with a sharing
    ///   violation (32).
    #[cfg(windows)]
    #[test]
    fn real_holders() {
        const READ: u32 = 1;
        const WRITE: u32 = 2;
        let dir = std::env::temp_dir().join(format!("dhd-settings-holder-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let p = dir.join("dhd.ini");
        assert!(save(&p, "fps", "60").unwrap());

        let (saved, took) = while_held(&p, READ, || save(&p, "fps", "120"));
        assert!(saved.unwrap(), "dhd.ini held without delete sharing: the save waits");
        assert!(took >= std::time::Duration::from_millis(100), "it did wait");
        assert_eq!(read(&p).unwrap().written("fps").as_deref(), Some("120"));

        let temp = dir.join("dhd.ini.test.new");
        fs::write(&temp, b"fps = 144\n").unwrap();
        let held_error = {
            use std::os::windows::fs::OpenOptionsExt;
            let f = fs::OpenOptions::new().read(true).share_mode(READ | WRITE).open(&temp).unwrap();
            let e = fs::rename(&temp, &p).unwrap_err().raw_os_error();
            drop(f);
            e
        };
        assert_eq!(held_error, Some(32), "a held source is a sharing violation");
        let (replaced, took) = while_held(&temp, READ | WRITE, || replace(&temp, &p));
        replaced.unwrap();
        assert!(took >= std::time::Duration::from_millis(100));
        assert_eq!(fs::read_to_string(&p).unwrap(), "fps = 144\n");

        let (saved, took) = while_held(&p, 0, || save(&p, "fps", "240"));
        assert!(saved.unwrap(), "dhd.ini held with no sharing: the read waits");
        assert!(took >= std::time::Duration::from_millis(100));
        assert_eq!(read(&p).unwrap().written("fps").as_deref(), Some("240"));
        fs::remove_dir_all(dir).unwrap();
    }
}
