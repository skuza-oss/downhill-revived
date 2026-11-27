//! Where the game and the player's files are, and starting the game and waiting for it.
//!
//! The game is beside the launcher: dhd.exe in the Windows folder, dhd in the Linux folder (or the
//! AppImage's usr/bin) and in a macOS app bundle's Contents/MacOS. The player's files (dhd.ini,
//! saves/, dhd.log, the update's work folder) go in one data folder, decided here and nowhere else:
//! - the system's folder for app data when the game is installed: the Windows installer's folder
//!   (it writes INSTALLED_MARKER beside the launcher), a macOS app bundle (never written to) or a
//!   Linux AppImage (a read-only image, run from $APPIMAGE);
//! - else the game's folder when it is a plain folder the player can write to (the zips, portable
//!   as always);
//! - else (a read-only folder such as Program Files) the system's folder for app data too.
//! On Linux, RecompLauncher's data folder (RECOMPLAUNCHER_DATA, `{data}` in recomplauncher.json)
//! comes first: the launcher backs up, moves and uninstalls the game with it. The first start with
//! it copies the player's files in from the folder the rule above picks, or else from the system's
//! folder for app data (where an AppImage RecompLauncher still ran as an image kept them;
//! copy_old_data), and the folder the rule picks stays the data folder until the copy has finished.
//! The game is started with that folder in DHD_SETTINGS, DHD_MEMCARD_DIR and DHD_DATA_DIR (dhd.log,
//! iso-check.txt), and with DHD_LAUNCHER=1, so it doesn't open the launcher again
//! (port/hle/launcher_link.c). The online menu's Update Now ends it with UPDATE_EXIT_CODE, and the
//! launcher installs the update.

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

/// Beside the launcher in a copy the Windows installer put there (port/release/installer/windows.iss
/// writes it, its uninstaller deletes it; updates never touch it): the player's files go to the
/// system's app data, not the install folder.
pub const INSTALLED_MARKER: &str = "installed.txt";

/// The Windows installer's entry under HKCU\...\Uninstall (Inno Setup's AppId + "_is1",
/// windows.iss): the launcher keeps its DisplayVersion current after an update.
pub const UNINSTALL_KEY: &str = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\{930C9A04-B028-4A3A-8027-E06E5D50C889}_is1";

#[derive(Clone)]
pub struct Place {
    /// The launcher's and the game's folder: DHD_GAME_DIR (development), else the launcher's own.
    pub game: PathBuf,
    /// The player's files.
    pub data: PathBuf,
    /// The macOS app bundle the launcher runs from (Downhill Revived.app): an update replaces it
    /// whole, never a file inside it.
    pub bundle: Option<PathBuf>,
    /// The Linux AppImage the launcher runs from ($APPIMAGE): an update replaces that one file.
    pub appimage: Option<PathBuf>,
    /// Put there by the Windows installer (INSTALLED_MARKER).
    pub installed: bool,
}

impl Place {
    pub fn find() -> Place {
        let game = std::env::var_os("DHD_GAME_DIR").map(PathBuf::from).unwrap_or_else(|| {
            std::env::current_exe()
                .ok()
                .and_then(|p| p.parent().map(Path::to_path_buf))
                .unwrap_or_else(|| PathBuf::from("."))
        });
        let appimage = appimage(&game, std::env::var_os("APPIMAGE"), std::env::var_os("APPDIR"));
        let place = Place::at(game, app_data(), appimage);
        match recomplauncher_data() {
            Some(data) => place.with_data(data, app_data()),
            None => place,
        }
    }

    /// RecompLauncher's data folder in place of this one's, once the player's files are in it
    /// (copy_old_data, from this one's, else from app_data); until then this place as it is.
    pub fn with_data(self, data: PathBuf, app_data: Option<PathBuf>) -> Place {
        let mut olds = vec![self.data.as_path()];
        olds.extend(app_data.as_deref().filter(|a| *a != self.data && *a != data));
        if data == self.data || fs::create_dir_all(&data).is_err() || !copy_old_data(&olds, &data) {
            return self;
        }
        Place { data, ..self }
    }

    /// The rule above, for the launcher in folder game; app_data is the system's folder for it,
    /// appimage the AppImage the launcher runs from.
    pub fn at(game: PathBuf, app_data: Option<PathBuf>, appimage: Option<PathBuf>) -> Place {
        let bundle = app_bundle(&game);
        let installed = game.join(INSTALLED_MARKER).is_file();
        let data = match app_data {
            Some(d) if bundle.is_some() || appimage.is_some() || installed || !writable(&game) => d,
            _ => game.clone(),
        };
        let _ = fs::create_dir_all(&data);
        Place { game, data, bundle, appimage, installed }
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

    /// What starts the launcher again after an update: the AppImage file (the image this one runs
    /// from is gone once it exits), else this executable.
    pub fn launcher(&self) -> std::io::Result<PathBuf> {
        match &self.appimage {
            Some(a) => Ok(a.clone()),
            None => std::env::current_exe(),
        }
    }

    /// Starts the launcher again after an update (launcher()). From an AppImage, without the variables
    /// the AppImage runtime and its GTK hook set for this image's mount: the new image's AppRun keeps
    /// an APPDIR it inherits, so it would run the old launcher from the old mount (and keep that
    /// mounted) instead of the new one.
    pub fn relaunch(&self) -> std::io::Result<Child> {
        let exe = self.launcher()?;
        let mut cmd = Command::new(&exe);
        if self.appimage.is_some() {
            let appdir = std::env::var("APPDIR").unwrap_or_default();
            for (key, value) in appimage_env(std::env::vars_os(), &appdir) {
                match value {
                    Some(v) => cmd.env(key, v),
                    None => cmd.env_remove(key),
                };
            }
            if let Some(dir) = exe.parent() {
                cmd.current_dir(dir);
            }
            #[cfg(target_os = "linux")]
            close_on_exec_all();
        }
        cmd.spawn()
    }

    pub fn start(&self) -> std::io::Result<Child> {
        let mut cmd = Command::new(self.exe());
        cmd.current_dir(&self.data)
            .env("DHD_LAUNCHER", "1")
            .env("DHD_SETTINGS", self.settings())
            .env("DHD_DATA_DIR", &self.data);
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

/// The AppImage the launcher in dir runs from: the AppImage runtime names the image in APPIMAGE and
/// its mounted folder in APPDIR (Linux only; an AppImage's variables left in the environment of a
/// launcher started from elsewhere don't count: dir must be inside APPDIR).
fn appimage(dir: &Path, image: Option<std::ffi::OsString>, appdir: Option<std::ffi::OsString>) -> Option<PathBuf> {
    if !cfg!(target_os = "linux") && !cfg!(test) {
        return None;
    }
    let (image, appdir) = (PathBuf::from(image?), PathBuf::from(appdir?));
    (image.is_absolute() && appdir.is_absolute() && dir.starts_with(&appdir)).then_some(image)
}

/// The changes to the environment that leave none of an AppImage's own variables for a new image:
/// (name, None) removes it, (name, Some(v)) sets v. The runtime's APPDIR, APPIMAGE, ARGV0 and OWD
/// and the GTK hook's GTK_THEME go; so does every variable naming the mount (appdir). A list of
/// folders (PATH, which AppRun extends with the mount's usr/bin, or XDG_DATA_DIRS) loses only the
/// mount's entries: without PATH the new image's runtime can't find fusermount.
fn appimage_env(vars: impl IntoIterator<Item = (std::ffi::OsString, std::ffi::OsString)>, appdir: &str)
    -> Vec<(std::ffi::OsString, Option<std::ffi::OsString>)> {
    const OWN: [&str; 5] = ["APPDIR", "APPIMAGE", "ARGV0", "OWD", "GTK_THEME"];
    let mut out = Vec::new();
    for (key, value) in vars {
        let text = value.to_string_lossy().into_owned();
        let name = key.to_string_lossy().into_owned();
        if OWN.contains(&name.as_str()) {
            out.push((key, None));
        } else if appdir.is_empty() || !text.contains(appdir) {
            continue;
        } else if text.contains(':') {
            let kept: Vec<&str> = text.split(':').filter(|d| !d.is_empty() && !d.starts_with(appdir)).collect();
            out.push((key, (!kept.is_empty()).then(|| kept.join(":").into())));
        } else {
            out.push((key, None));
        }
    }
    out
}

/// Marks every open file of this process but stdin, stdout and stderr close-on-exec, before an
/// AppImage relaunch (this launcher exits right after). The AppImage runtime hands the app fd 1023,
/// open on its mount, and the end of a pipe that keeps the mount alive until every copy is closed: a
/// new image started with them found the old mount through fd 1023 and ran the old launcher from it,
/// and the old mount stayed until the new launcher closed.
#[cfg(target_os = "linux")]
fn close_on_exec_all() {
    let Ok(entries) = fs::read_dir("/proc/self/fd") else { return };
    for e in entries.flatten() {
        let Ok(fd) = e.file_name().to_string_lossy().parse::<i32>() else { continue };
        if fd > 2 {
            // SAFETY: fcntl on a descriptor number this process holds; it only changes its flags
            unsafe {
                let flags = libc::fcntl(fd, libc::F_GETFD);
                if flags >= 0 {
                    libc::fcntl(fd, libc::F_SETFD, flags | libc::FD_CLOEXEC);
                }
            }
        }
    }
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

/// RecompLauncher's data folder for the game (RECOMPLAUNCHER_DATA), on Linux; an absolute path only.
fn recomplauncher_data() -> Option<PathBuf> {
    if !cfg!(target_os = "linux") {
        return None;
    }
    std::env::var_os("RECOMPLAUNCHER_DATA").map(PathBuf::from).filter(|p| p.is_absolute())
}

/// Started by RecompLauncher (RECOMPLAUNCHER=1), on any system: it installs the game's releases
/// itself, so this launcher never looks for, offers or installs one (RecompLauncher's guide for
/// developers). Only the updates: where the player's files go is recomplauncher_data's.
pub fn updated_by_recomplauncher() -> bool {
    std::env::var_os("RECOMPLAUNCHER").is_some_and(|v| v == "1")
}

/// The player's files copy_old_data takes from the old data folder: dhd.ini first (the only one
/// that replaces what's there), then the rest, never over one already there. Not the logs, the
/// update's work folder or its file list.
const OLD_DATA: [&str; 5] = ["dhd.ini", "saves", "textures", "iso-check.txt", "disc-notice.txt"];
/// In the new data folder once every file of OLD_DATA was copied (or wasn't there): the copy's only
/// "done".
const COPIED: &str = ".copied-from-old-folder";
/// In the new data folder while a copy runs: the names of OLD_DATA it has put in place, each written
/// just before its rename, so a copy that failed or was killed takes back exactly those.
const COPYING: &str = ".copying-from-old-folder";

/// Copies the player's files (OLD_DATA) from the old data folders into data, the first time; whether
/// data now has them (COPIED is there). Each one comes from the first of olds that has it (the
/// folder the game used, then the system's app data folder, where an AppImage that RecompLauncher
/// still ran as an image kept them). Each one is copied as NAME.part and renamed into place only
/// once all of it was copied (the texture pack's files hard-linked where they can be); the originals
/// are never moved or changed. Until it returns true the old folder stays the data folder: a copy
/// that failed is taken back (COPYING) and tried again at the next start, which brings saves made in
/// the old folder meanwhile.
pub fn copy_old_data(olds: &[&Path], data: &Path) -> bool {
    copy_old_data_with(olds, data, |_| Ok(()))
}

/// copy_old_data, with before_rename run before each file goes into place (tests: a failure there).
fn copy_old_data_with(olds: &[&Path], data: &Path, before_rename: impl FnMut(&str) -> std::io::Result<()>) -> bool {
    if data.join(COPIED).is_file() {
        let _ = fs::remove_file(data.join(COPYING));
        return true;
    }
    if take_back(data).is_err() {
        return false;
    }
    match copy_items(olds, data, before_rename) {
        Ok(()) => {
            let _ = fs::remove_file(data.join(COPYING));
            true
        }
        Err(_) => {
            let _ = take_back(data);
            false
        }
    }
}

fn copy_items(olds: &[&Path], data: &Path, mut before_rename: impl FnMut(&str) -> std::io::Result<()>) -> std::io::Result<()> {
    for (i, name) in OLD_DATA.iter().enumerate() {
        let mut from = None;
        for old in olds {
            if old.join(name).try_exists()? {
                from = Some(old.join(name));
                break;
            }
        }
        let to = data.join(name);
        let Some(from) = from.filter(|_| i == 0 || to.symlink_metadata().is_err()) else { continue };
        let part = data.join(format!("{name}.part"));
        remove_any(&part)?;
        copy_tree(&from, &part, *name == "textures")?;
        let mut journal = fs::OpenOptions::new().create(true).append(true).open(data.join(COPYING))?;
        std::io::Write::write_all(&mut journal, format!("{name}\n").as_bytes())?;
        journal.sync_all()?;
        before_rename(name)?;
        fs::rename(&part, &to)?;
    }
    sync_dir(data);
    let mut marker = fs::File::create(data.join(COPIED))?;
    for old in olds {
        std::io::Write::write_all(&mut marker, format!("{}\n", old.display()).as_bytes())?;
    }
    marker.sync_all()?;
    sync_dir(data);
    Ok(())
}

/// Removes what a copy that didn't finish put in data: the files COPYING names (only names of
/// OLD_DATA), the NAME.part copies, then COPYING itself.
fn take_back(data: &Path) -> std::io::Result<()> {
    let journal = data.join(COPYING);
    match fs::read_to_string(&journal) {
        Ok(text) => {
            for name in text.lines().filter(|n| OLD_DATA.contains(n)) {
                remove_any(&data.join(name))?;
            }
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => return Err(e),
    }
    for name in OLD_DATA {
        remove_any(&data.join(format!("{name}.part")))?;
    }
    remove_any(&journal)
}

fn remove_any(path: &Path) -> std::io::Result<()> {
    match fs::symlink_metadata(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(e),
        Ok(m) if m.is_dir() => fs::remove_dir_all(path),
        Ok(_) => fs::remove_file(path),
    }
}

/// A file, or a folder and everything in it, copied to `to` (which isn't there), each file synced;
/// with link, each file hard-linked instead where it can be (a texture pack the game only reads:
/// not twice on the drive), else copied.
fn copy_tree(from: &Path, to: &Path, link: bool) -> std::io::Result<()> {
    if fs::metadata(from)?.is_dir() {
        fs::create_dir(to)?;
        for entry in fs::read_dir(from)? {
            let entry = entry?;
            copy_tree(&entry.path(), &to.join(entry.file_name()), link)?;
        }
        Ok(())
    } else if link && fs::hard_link(from, to).is_ok() {
        Ok(())
    } else {
        fs::copy(from, to)?;
        // fsync takes any descriptor; Windows' flush wants a writable handle, which a read-only
        // file refuses (the copy only runs on Linux)
        if cfg!(unix) {
            fs::File::open(to)?.sync_all()?;
        }
        Ok(())
    }
}

/// The renames in dir made durable before what follows them (Linux; a folder can't be opened so on
/// Windows).
fn sync_dir(dir: &Path) {
    if let Ok(d) = fs::File::open(dir) {
        let _ = d.sync_all();
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
        let p = Place::at(folder.clone(), Some(app_data.clone()), None);
        assert_eq!((p.data, p.bundle, p.installed), (folder.clone(), None, false));
        // the same folder put there by the installer: app data
        fs::write(folder.join(INSTALLED_MARKER), "installed").unwrap();
        let p = Place::at(folder.clone(), Some(app_data.clone()), None);
        assert_eq!((p.data, p.installed), (app_data.clone(), true));
        // an app bundle: never written to
        let macos = root.join("Downhill Revived.app").join("Contents").join("MacOS");
        fs::create_dir_all(&macos).unwrap();
        let p = Place::at(macos.clone(), Some(app_data.clone()), None);
        assert_eq!(p.data, app_data);
        assert_eq!(p.bundle, Some(root.join("Downhill Revived.app")));
        assert_eq!(p.exe(), macos.join(GAME_EXE));
        assert!(app_bundle(&root.join("x").join("Contents").join("MacOS")).is_none());
        fs::remove_dir_all(root).unwrap();
    }

    /// An AppImage: its usr/bin inside APPDIR, the image named by APPIMAGE; app data, and the
    /// image is what an update replaces and what starts again.
    #[test]
    fn appimage_place() {
        let root = std::env::temp_dir().join(format!("dhd-launcher-appimage-{}", std::process::id()));
        let mount = root.join(".mount_DownhiXYZ");
        let bin = mount.join("usr").join("bin");
        fs::create_dir_all(&bin).unwrap();
        let image = root.join("DownhillRevived.AppImage");
        let found = appimage(&bin, Some(image.clone().into()), Some(mount.clone().into()));
        assert_eq!(found, Some(image.clone()));
        // variables inherited by a launcher that isn't inside the image
        assert_eq!(appimage(&root.join("elsewhere"), Some(image.clone().into()), Some(mount.clone().into())), None);
        assert_eq!(appimage(&bin, None, Some(mount.clone().into())), None);
        let app_data = root.join("share");
        let p = Place::at(bin.clone(), Some(app_data.clone()), found);
        assert_eq!((p.data.clone(), p.appimage.clone()), (app_data, Some(image.clone())));
        assert_eq!(p.launcher().unwrap(), image);
        fs::remove_dir_all(root).unwrap();
    }

    /// A relaunched AppImage gets none of the old mount's variables: they would make the new image
    /// run the old launcher from the old mount.
    #[test]
    fn appimage_relaunch_env() {
        let m = "/tmp/.mount_DownhiAB12";
        let vars = [
            ("APPDIR", m.to_string()),
            ("APPIMAGE", "/home/deck/DownhillRevived.AppImage".into()),
            ("OWD", "/home/deck".into()),
            ("GTK_THEME", "Adwaita:light".into()),
            ("GTK_PATH", format!("{m}//usr/lib/gtk-3.0")),
            ("GIO_MODULE_DIR", format!("{m}//usr/lib/gio/modules")),
            ("XDG_DATA_DIRS", format!("{m}/usr/share:/usr/share:/home/deck/.local/share/flatpak/exports/share")),
            ("PATH", format!("{m}/usr/bin:/usr/local/bin:/usr/bin:/bin")),
            ("GI_TYPELIB_PATH", format!("{m}//usr/lib/girepository-1.0")),
            ("HOME", "/home/deck".into()),
            ("DHD_SETTINGS", "/home/deck/my.ini".into()),
        ];
        let got = appimage_env(vars.iter().map(|(k, v)| (k.into(), v.into())), m);
        let get = |k: &str| got.iter().find(|(n, _)| n == k).map(|(_, v)| v.clone().map(|v| v.into_string().unwrap()));
        assert_eq!(get("PATH"), Some(Some("/usr/local/bin:/usr/bin:/bin".into())));  // fusermount stays found
        for gone in ["APPDIR", "APPIMAGE", "OWD", "GTK_THEME", "GTK_PATH", "GIO_MODULE_DIR", "GI_TYPELIB_PATH"] {
            assert_eq!(get(gone), Some(None), "{gone}");
        }
        assert_eq!(get("XDG_DATA_DIRS"), Some(Some("/usr/share:/home/deck/.local/share/flatpak/exports/share".into())));
        assert_eq!((get("HOME"), get("DHD_SETTINGS")), (None, None));  // untouched
    }

    /// An old data folder (dhd.ini, saves/) and RecompLauncher's new one, under a fresh root.
    fn old_and_new(test: &str) -> (PathBuf, PathBuf, PathBuf) {
        let root = std::env::temp_dir().join(format!("dhd-launcher-{test}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let (old, data) = (root.join("usr").join("bin"), root.join("recompdata"));
        fs::create_dir_all(old.join("saves")).unwrap();
        fs::create_dir_all(&data).unwrap();
        fs::write(old.join("dhd.ini"), "iso = old.iso\n").unwrap();
        fs::write(old.join("saves").join("card0.ps2"), "old save").unwrap();
        (root, old, data)
    }

    fn read(p: PathBuf) -> String {
        fs::read_to_string(p).unwrap_or_default()
    }

    /// The first start with RecompLauncher's data folder: the player's files copied in, the
    /// originals left, the old dhd.ini over the one RecompLauncher wrote, nothing else in the way.
    #[test]
    fn copies_old_data() {
        let (root, old, data) = old_and_new("copy");
        fs::write(data.join("dhd.ini"), "iso = launcher.iso\n").unwrap();
        fs::write(old.join("dhd.log"), "a log").unwrap();
        assert!(copy_old_data(&[&old], &data));
        assert_eq!(read(data.join("dhd.ini")), "iso = old.iso\n");
        assert_eq!(read(data.join("saves").join("card0.ps2")), "old save");
        assert!(data.join(COPIED).is_file() && !data.join(COPYING).exists() && !data.join("dhd.log").exists());
        assert_eq!((read(old.join("dhd.ini")), read(old.join("saves").join("card0.ps2"))), ("iso = old.iso\n".into(), "old save".into()));
        // done once: a later save in the old folder and RecompLauncher's dhd.ini stay as they are
        fs::write(old.join("saves").join("card1.ps2"), "later").unwrap();
        fs::write(data.join("dhd.ini"), "iso = launcher.iso\n").unwrap();
        assert!(copy_old_data(&[&old], &data));
        assert!(!data.join("saves").join("card1.ps2").exists());
        assert_eq!(read(data.join("dhd.ini")), "iso = launcher.iso\n");
        fs::remove_dir_all(root).unwrap();
    }

    /// A fresh install: nothing to copy, RecompLauncher's dhd.ini kept, done at once.
    #[test]
    fn fresh_data() {
        let (root, old, data) = old_and_new("fresh");
        fs::remove_dir_all(&old).unwrap();
        fs::write(data.join("dhd.ini"), "iso = launcher.iso\n").unwrap();
        assert!(copy_old_data(&[&old], &data));
        assert_eq!(read(data.join("dhd.ini")), "iso = launcher.iso\n");
        assert!(data.join(COPIED).is_file() && !data.join("saves").exists());
        fs::remove_dir_all(root).unwrap();
    }

    /// A copy that fails takes back what it put in place and leaves the old folder in use; the next
    /// start copies again and brings the save made in the old folder meanwhile.
    #[test]
    fn failed_copy_is_taken_back() {
        let (root, old, data) = old_and_new("failed");
        fs::create_dir_all(old.join("textures")).unwrap();
        fs::write(old.join("textures").join("a.png"), "png").unwrap();
        let fail = |name: &str| if name == "textures" { Err(std::io::Error::other("disk full")) } else { Ok(()) };
        assert!(!copy_old_data_with(&[&old], &data, fail));
        for gone in ["dhd.ini", "saves", "textures", "textures.part", COPYING, COPIED] {
            assert!(!data.join(gone).exists(), "{gone}");
        }
        let place = Place { game: old.clone(), data: old.clone(), bundle: None, appimage: None, installed: false };
        fs::write(old.join("saves").join("card0.ps2"), "newer save").unwrap();
        let place = place.with_data(data.clone(), None);
        assert_eq!(place.data, data);
        assert_eq!(read(data.join("saves").join("card0.ps2")), "newer save");
        assert_eq!(read(data.join("textures").join("a.png")), "png");
        fs::remove_dir_all(root).unwrap();
    }

    /// A copy killed after some files went into place: that start played on the old folder, and the
    /// next one takes them back and copies everything again.
    #[test]
    fn interrupted_copy_is_finished() {
        let (root, old, data) = old_and_new("killed");
        fs::create_dir_all(old.join("textures")).unwrap();
        let killed = copy_items(&[&old], &data, |name| if name == "textures" { Err(std::io::Error::other("killed")) } else { Ok(()) });
        assert!(killed.is_err());
        assert!(data.join("saves").is_dir() && data.join("textures.part").is_dir() && !data.join(COPIED).exists());
        fs::write(old.join("saves").join("card0.ps2"), "played meanwhile").unwrap();
        fs::write(data.join("dhd.ini"), "iso = launcher.iso\n").unwrap();
        assert!(copy_old_data(&[&old], &data));
        assert_eq!(read(data.join("saves").join("card0.ps2")), "played meanwhile");
        assert_eq!(read(data.join("dhd.ini")), "iso = old.iso\n");
        assert!(data.join("textures").is_dir() && !data.join("textures.part").exists() && !data.join(COPYING).exists());
        fs::remove_dir_all(root).unwrap();
    }

    /// Files only in the system's app data folder (an AppImage RecompLauncher ran as an image before
    /// it unpacked them): copied from there, one the game's folder has from the game's folder; the
    /// texture pack's files linked, not copied.
    #[test]
    fn copies_from_app_data() {
        let (root, old, data) = old_and_new("appdata");
        fs::remove_dir_all(old.join("saves")).unwrap();
        let share = root.join("share").join("downhill-revived");
        fs::create_dir_all(share.join("saves")).unwrap();
        fs::create_dir_all(share.join("textures")).unwrap();
        fs::write(share.join("dhd.ini"), "iso = image.iso\n").unwrap();
        fs::write(share.join("saves").join("card0.ps2"), "image save").unwrap();
        fs::write(share.join("textures").join("a.png"), "png").unwrap();
        let place = Place { game: old.clone(), data: old.clone(), bundle: None, appimage: None, installed: false };
        let place = place.with_data(data.clone(), Some(share.clone()));
        assert_eq!(place.data, data);
        assert_eq!(read(data.join("dhd.ini")), "iso = old.iso\n");
        assert_eq!(read(data.join("saves").join("card0.ps2")), "image save");
        assert_eq!(read(data.join("textures").join("a.png")), "png");
        assert_eq!(read(share.join("saves").join("card0.ps2")), "image save");
        #[cfg(unix)]
        {
            use std::os::unix::fs::MetadataExt;
            let (a, b) = (fs::metadata(share.join("textures").join("a.png")).unwrap(), fs::metadata(data.join("textures").join("a.png")).unwrap());
            assert_eq!((a.dev(), a.ino()), (b.dev(), b.ino()));
        }
        fs::remove_dir_all(root).unwrap();
    }

    /// A data folder that can't be made: the old folder stays the data folder.
    #[test]
    fn unusable_data_keeps_old_folder() {
        let (root, old, _) = old_and_new("unusable");
        fs::write(root.join("file"), "").unwrap();
        let place = Place { game: old.clone(), data: old.clone(), bundle: None, appimage: None, installed: false };
        assert_eq!(place.with_data(root.join("file").join("recompdata"), None).data, old);
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
