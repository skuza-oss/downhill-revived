//! Where the game and the player's files are, and starting the game and waiting for it.
//!
//! The game is beside the launcher: dhd.exe in the Windows folder, dhd in the Linux folder and in
//! a macOS app bundle's Contents/MacOS. The player's files (dhd.ini, saves/, dhd.log, the update's
//! work folder) go in one data folder, decided here and nowhere else: the game's folder when it is
//! a plain folder the player can write to (the Windows and Linux archives, portable as always),
//! else the system's folder for app data (an app bundle is never written to; a read-only folder
//! such as Program Files). The game is started with that folder in DHD_SETTINGS, DHD_MEMCARD_DIR
//! and DHD_DATA_DIR (dhd.log, iso-check.txt), and with DHD_LAUNCHER=1, so it doesn't open the
//! launcher again (port/hle/launcher_link.c). The online menu's Update Now ends it with
//! UPDATE_EXIT_CODE, and the launcher installs the update.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};

#[cfg(windows)]
pub const GAME_EXE: &str = "dhd.exe";
#[cfg(not(windows))]
pub const GAME_EXE: &str = "dhd";

/// 'U': the game asks for an update (port/hle/launcher_link.c); under 256 for POSIX exit statuses.
pub const UPDATE_EXIT_CODE: u32 = 85;

/// The data folder's name under the system's app data (Windows, macOS; Linux: a lower-case one).
const APP_DATA_NAME: &str = "Downhill Revived";

pub struct Place {
    /// The launcher's and the game's folder: DHD_GAME_DIR (development), else the launcher's own.
    pub game: PathBuf,
    /// The player's files.
    pub data: PathBuf,
    /// The macOS app bundle the launcher runs from (Downhill Revived.app): an update replaces it
    /// whole, never a file inside it.
    pub bundle: Option<PathBuf>,
}

impl Place {
    pub fn find() -> Place {
        let game = std::env::var_os("DHD_GAME_DIR").map(PathBuf::from).unwrap_or_else(|| {
            std::env::current_exe()
                .ok()
                .and_then(|p| p.parent().map(Path::to_path_buf))
                .unwrap_or_else(|| PathBuf::from("."))
        });
        Place::at(game, app_data())
    }

    /// The rule above, for the launcher in folder game; app_data is the system's folder for it.
    pub fn at(game: PathBuf, app_data: Option<PathBuf>) -> Place {
        let bundle = app_bundle(&game);
        let data = match app_data {
            Some(d) if bundle.is_some() || !writable(&game) => d,
            _ => game.clone(),
        };
        let _ = fs::create_dir_all(&data);
        Place { game, data, bundle }
    }

    pub fn exe(&self) -> PathBuf {
        self.game.join(GAME_EXE)
    }

    /// dhd.ini's path: DHD_SETTINGS, else in the data folder.
    pub fn settings(&self) -> PathBuf {
        match std::env::var_os("DHD_SETTINGS") {
            Some(p) => PathBuf::from(p),
            None => self.data.join("dhd.ini"),
        }
    }

    pub fn start(&self) -> std::io::Result<Child> {
        self.start_with_join(None)
    }

    /// The same, with the join-by-link code in `DHD_NET_ROOM` (finding 125) when the launcher was
    /// started from a link: the game opens ONLINE by itself and joins that room (online_menu.c's
    /// join_read), as a browser's `?room=` does.
    pub fn start_with_join(&self, join: Option<&str>) -> std::io::Result<Child> {
        let mut cmd = Command::new(self.exe());
        cmd.current_dir(&self.data)
            .env("DHD_LAUNCHER", "1")
            .env("DHD_SETTINGS", self.settings())
            .env("DHD_DATA_DIR", &self.data);
        if let Some(code) = join {
            cmd.env("DHD_NET_ROOM", code);
        }
        if std::env::var_os("DHD_MEMCARD_DIR").is_none() {
            cmd.env("DHD_MEMCARD_DIR", self.data.join("saves"));
        }
        cmd.spawn()
    }
}

/// Downhill Revived.app when dir is its Contents/MacOS.
fn app_bundle(dir: &Path) -> Option<PathBuf> {
    let contents = dir.parent().filter(|_| dir.file_name() == Some("MacOS".as_ref()))?;
    let app = contents.parent().filter(|_| contents.file_name() == Some("Contents".as_ref()))?;
    app.extension().is_some_and(|e| e.eq_ignore_ascii_case("app")).then(|| app.to_path_buf())
}

/// A file can be made in dir (not a read-only folder such as Program Files).
fn writable(dir: &Path) -> bool {
    let probe = dir.join(".dhd-write-check");
    let ok = fs::write(&probe, b"").is_ok();
    let _ = fs::remove_file(&probe);
    ok
}

/// The system's folder for an app's own files.
fn app_data() -> Option<PathBuf> {
    let home = || std::env::var_os("HOME").map(PathBuf::from);
    if cfg!(windows) {
        std::env::var_os("APPDATA").map(|d| PathBuf::from(d).join(APP_DATA_NAME))
    } else if cfg!(target_os = "macos") {
        home().map(|h| h.join("Library").join("Application Support").join(APP_DATA_NAME))
    } else {
        let xdg = std::env::var_os("XDG_DATA_HOME").map(PathBuf::from).filter(|p| p.is_absolute());
        xdg.or_else(|| home().map(|h| h.join(".local").join("share"))).map(|d| d.join("downhill-revived"))
    }
}

/// The exit code as the game set it.
pub fn exit_code(status: std::process::ExitStatus) -> Option<u32> {
    status.code().map(|c| c as u32)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn data_folder() {
        let root = std::env::temp_dir().join(format!("dhd-launcher-place-{}", std::process::id()));
        let app_data = root.join("app data");
        // a plain folder: the game's own
        let folder = root.join("DownhillRevived-9.9.9");
        fs::create_dir_all(&folder).unwrap();
        let p = Place::at(folder.clone(), Some(app_data.clone()));
        assert_eq!((p.data, p.bundle), (folder, None));
        // an app bundle: never written to
        let macos = root.join("Downhill Revived.app").join("Contents").join("MacOS");
        fs::create_dir_all(&macos).unwrap();
        let p = Place::at(macos.clone(), Some(app_data.clone()));
        assert_eq!(p.data, app_data);
        assert_eq!(p.bundle, Some(root.join("Downhill Revived.app")));
        assert_eq!(p.exe(), macos.join(GAME_EXE));
        assert!(app_bundle(&root.join("x").join("Contents").join("MacOS")).is_none());
        fs::remove_dir_all(root).unwrap();
    }

    /// dhd.ini as the launcher reads and saves it (the crate's rules, port/render/settings).
    #[test]
    fn keeps_other_lines() {
        let dir = std::env::temp_dir().join(format!("dhd-ini-test-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let p = dir.join("dhd.ini");
        fs::write(&p, "; comment\n[x]\nScale = 2 ; two\niso = \"D:\\a;b.iso\"\n").unwrap();
        dhd_settings::save(&p, "scale", "4").unwrap();
        dhd_settings::save(&p, "name", "SKUZA").unwrap();
        let text = fs::read_to_string(&p).unwrap();
        assert_eq!(text, "; comment\n[x]\nscale = 4\niso = \"D:\\a;b.iso\"\nname = SKUZA\n");
        let ini = dhd_settings::read(&p).unwrap();
        assert_eq!(ini.value("iso"), "D:\\a;b.iso");
        assert_eq!(ini.value("scale"), "4");
        dhd_settings::create(&p).unwrap();
        assert_eq!(fs::read_to_string(&p).unwrap(), text);
        let new = dir.join("new.ini");
        dhd_settings::create(&new).unwrap();
        assert_eq!(fs::read_to_string(&new).unwrap(), dhd_settings::HEADER);
        fs::remove_dir_all(dir).unwrap();
    }
}
