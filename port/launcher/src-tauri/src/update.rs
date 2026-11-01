//! The game's updater, on every platform (finding 093; the rules of finding 054): the update
//! source is dhd.ini's update_url or the GitHub releases API of skuza-oss/downhill-revived; its answer
//! is GitHub's release JSON (whose latest.json asset, or else its DownhillRevived-*-<PLATFORM>.zip
//! or -<PLATFORM>-experimental.zip asset and that asset's sha256 digest, names the zip) or a
//! latest.json itself. latest.json's top-level url, sha256 and size are the Windows zip's (what 0.6.x
//! launchers read); "platforms" holds each platform's own {url, sha256, size}, the url taken as
//! written (macOS and Linux name -experimental zips since 0.7.0). The zip is downloaded into dhd-update/ in the data
//! folder (game.rs) and its SHA-256 checked. Only the last step differs, by its target:
//! - a folder install (Windows, Linux): the files of the zip's one folder are copied over the
//!   game's (never dhd.ini, a log or a folder such as saves/). A file in use (this launcher) is
//!   renamed NAME.old first;
//! - a macOS app bundle: the zip's Downhill Revived.app replaces the running one whole (that one
//!   renamed NAME.app.old, the new one moved in), so no file inside a signed bundle is written;
//! - a Linux AppImage (game.rs Place::appimage): its update is the release's AppImage, not the zip
//!   (latest.json's "linux-appimage" entry, or the release's DownhillRevived-*-linux*.AppImage
//!   asset); the running image is renamed NAME.AppImage.old and the new file moved in.
//! A copy the Windows installer put in place (game.rs INSTALLED_MARKER) updates as a folder install,
//! then its Apps & features entry gets the new version. clean_up deletes the .old files and
//! dhd-update/ at the next start.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::time::Duration;

use crate::game::Place;

pub const OFFICIAL_SOURCE: &str = "https://api.github.com/repos/skuza-oss/downhill-revived/releases/latest";
pub const RELEASES_PAGE: &str = "https://github.com/skuza-oss/downhill-revived/releases/latest";
const WORK_DIR: &str = "dhd-update";
/// Never replaced by an update: the player's own files.
const KEEP: [&str; 4] = ["dhd.ini", "dhd.log", "iso-check.txt", "launcher.log"];
/// This build's name in release assets (DownhillRevived-<version>-<PLATFORM>.zip, or
/// -<PLATFORM>-experimental.zip while the platform is experimental) and in latest.json's platforms.
pub const PLATFORM: &str = if cfg!(windows) {
    "win"
} else if cfg!(target_os = "macos") {
    "mac"
} else {
    "linux"
};

/// latest.json's key for an AppImage's update (the AppImage file itself, not the zip).
pub const APPIMAGE: &str = "linux-appimage";

/// The update this copy takes: PLATFORM's zip, or APPIMAGE when it runs from an AppImage.
pub fn platform(place: &Place) -> &'static str {
    if place.appimage.is_some() {
        APPIMAGE
    } else {
        PLATFORM
    }
}

/// A release asset that is the platform's update: DownhillRevived-<version>-<PLATFORM>.zip or
/// DownhillRevived-<version>-<PLATFORM>-experimental.zip (macOS and Linux since 0.7.0); for
/// APPIMAGE DownhillRevived-<version>-linux.AppImage or -linux-experimental.AppImage.
fn platform_asset(name: &str, platform: &str) -> bool {
    let (key, ext) = if platform == APPIMAGE { ("linux", "AppImage") } else { (platform, "zip") };
    name.starts_with("DownhillRevived-")
        && [format!("-{key}.{ext}"), format!("-{key}-experimental.{ext}")].iter().any(|end| name.ends_with(end.as_str()))
}

#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Release {
    pub version: String,
    pub url: String,
    pub sha256: String,
    pub size: u64,
    pub page: String,
    /// The release's notes (GitHub's markdown body), when the source gives them.
    pub notes: String,
    /// When it was published (GitHub's published_at, e.g. 2026-10-26T20:28:33Z), when known.
    pub published: String,
}

fn agent() -> ureq::Agent {
    ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(6))
        .timeout_read(Duration::from_secs(20))
        .user_agent(concat!("DownhillRevived-launcher/", env!("DHD_VERSION")))
        .build()
}

pub fn get_json(url: &str) -> Result<serde_json::Value, String> {
    agent()
        .get(url)
        .set("Accept", "application/json")
        .call()
        .map_err(|e| format!("{url}: {e}"))?
        .into_json()
        .map_err(|e| format!("{url} didn't answer with JSON: {e}"))
}

/// url relative to the manifest at base.
fn resolve(base: &str, url: &str) -> String {
    if url.contains("://") || url.is_empty() {
        return url.to_string();
    }
    let base = base.split('?').next().unwrap_or(base);
    let after = base.find("://").map(|i| i + 3).unwrap_or(0);
    let root = base[after..].find('/').map(|i| i + after);
    match (url.starts_with('/'), root) {
        (true, Some(r)) => format!("{}{url}", &base[..r]),
        (false, Some(_)) => format!("{}{url}", &base[..=base.rfind('/').unwrap()]),
        _ => format!("{base}/{}", url.trim_start_matches('/')),
    }
}

fn text(v: &serde_json::Value, key: &str) -> String {
    v.get(key).and_then(|s| s.as_str()).unwrap_or_default().to_string()
}

/// latest.json: the platform's entry under "platforms", else (a manifest from before them, which
/// names the Windows zip) the top level on Windows. None: the release has no build for this
/// platform.
fn read_manifest(m: &serde_json::Value, base: &str, platform: &str) -> Result<Option<Release>, String> {
    let version = text(m, "version");
    let entry = match m.get("platforms") {
        Some(p) => p.get(platform),
        None => (platform == "win").then_some(m),
    };
    let Some(entry) = entry else { return Ok(None) };
    let (url, sha256) = (text(entry, "url"), text(entry, "sha256"));
    if version.is_empty() || url.is_empty() || sha256.is_empty() {
        return Err(format!("the update manifest {base} lacks version, url or sha256"));
    }
    Ok(Some(Release {
        version,
        url: resolve(base, &url),
        sha256: sha256.to_ascii_lowercase(),
        size: entry.get("size").and_then(|s| s.as_u64()).unwrap_or(0),
        page: text(m, "page"),
        ..Default::default()
    }))
}

pub fn parse_version(v: &str) -> Option<[u32; 3]> {
    let mut out = [0u32; 3];
    let mut parts = v.trim().trim_start_matches(['v', 'V']).split('.');
    for slot in &mut out {
        *slot = parts.next()?.parse().ok()?;
    }
    parts.next().is_none().then_some(out)
}

pub fn newer(candidate: &str, current: &str) -> bool {
    matches!((parse_version(candidate), parse_version(current)), (Some(a), Some(b)) if a > b)
}

/// The newest release the source names for platform (platform()); None when it has no build for
/// it, which is no update rather than a failed check (from n0buholic, PR #13). A source that can't
/// be reached or gives an unusable answer is an error.
pub fn latest(source: &str, platform: &str) -> Result<Option<Release>, String> {
    release_from(source, &get_json(source)?, platform)
}

/// latest() for the source's answer.
fn release_from(source: &str, body: &serde_json::Value, platform: &str) -> Result<Option<Release>, String> {
    let r = match body.get("assets").and_then(|a| a.as_array()) {
        None => read_manifest(body, source, platform)?,
        Some(assets) => {
            let mut manifest = None;
            let mut zip = None;
            for a in assets {
                let name = text(a, "name");
                if name == "latest.json" {
                    manifest = Some(text(a, "browser_download_url"));
                } else if platform_asset(&name, platform) {
                    zip = Some(a.clone());
                }
            }
            let r = if let Some(m) = manifest {
                read_manifest(&get_json(&m)?, &m, platform)?
            } else if let Some(z) = zip {
                if !text(&z, "digest").starts_with("sha256:") {
                    return Err(format!("the newest release at {source} has a {platform} build without a digest"));
                }
                Some(Release {
                    version: text(body, "tag_name").trim_start_matches(['v', 'V']).to_string(),
                    url: text(&z, "browser_download_url"),
                    sha256: text(&z, "digest")[7..].to_ascii_lowercase(),
                    size: z.get("size").and_then(|s| s.as_u64()).unwrap_or(0),
                    ..Default::default()
                })
            } else {
                None
            };
            r.map(|mut r| {
                r.page = text(body, "html_url");
                r.notes = text(body, "body");
                r.published = text(body, "published_at");
                r
            })
        }
    };
    let Some(mut r) = r else { return Ok(None) };
    let hex = r.sha256.len() == 64 && r.sha256.bytes().all(|c| c.is_ascii_hexdigit());
    if parse_version(&r.version).is_none() || !hex || !(r.url.starts_with("https://") || r.url.starts_with("http://")) {
        return Err(format!("the update source {source} gave an unusable answer (version \"{}\")", r.version));
    }
    if r.page.is_empty() {
        r.page = if source == OFFICIAL_SOURCE { RELEASES_PAGE.into() } else { source.into() };
    }
    Ok(Some(r))
}

/// Downloads r's update to dhd-update/ in the data folder, calling progress(done, total) as it
/// goes, checks it and puts the release in place; an error names what went wrong or what wasn't
/// replaced.
pub fn install(r: &Release, place: &Place, progress: impl FnMut(u64, u64)) -> Result<(), String> {
    let work = place.data.join(WORK_DIR);
    let _ = fs::remove_dir_all(&work);
    let made = fs::create_dir_all(&work).map_err(|e| format!("can't make {}: {e} (is the folder read-only?)", work.display()));
    let done = match &place.appimage {
        Some(image) => made
            .and_then(|_| download(r, &work.join(format!("DownhillRevived-{}.AppImage", r.version)), progress))
            .and_then(|new| replace_file(&new, image)),
        None => made
            .and_then(|_| download(r, &work.join(format!("DownhillRevived-{}.zip", r.version)), progress))
            .and_then(|zip| unpack(&zip, &work))
            .and_then(|unpacked| match &place.bundle {
                Some(app) => replace_bundle(&unpacked, app),
                None => copy_release(&unpacked, &place.game),
            }),
    };
    let _ = fs::remove_dir_all(&work);
    if done.is_ok() && place.installed {
        note_installed_version(&r.version);
    }
    done
}

/// r's file downloaded to path and its SHA-256 checked; path.
fn download(r: &Release, path: &Path, mut progress: impl FnMut(u64, u64)) -> Result<PathBuf, String> {
    let zip_path = path.to_path_buf();

    let resp = agent().get(&r.url).call().map_err(|e| format!("download failed: {e}"))?;
    let total = resp.header("Content-Length").and_then(|s| s.parse().ok()).unwrap_or(r.size);
    let mut reader = resp.into_reader();
    let mut file = fs::File::create(&zip_path).map_err(|e| format!("{}: {e}", zip_path.display()))?;
    let mut hash = Sha256::new();
    let mut buf = vec![0u8; 256 * 1024];
    let mut done = 0u64;
    loop {
        let n = reader.read(&mut buf).map_err(|e| format!("download failed: {e}"))?;
        if n == 0 {
            break;
        }
        hash.update(&buf[..n]);
        file.write_all(&buf[..n]).map_err(|e| format!("can't write the download: {e}"))?;
        done += n as u64;
        progress(done, total);
    }
    drop(file);
    let got: String = hash.finalize().iter().map(|b| format!("{b:02x}")).collect();
    if got != r.sha256 {
        return Err(format!("the download is damaged (SHA-256 {got}, expected {})", r.sha256));
    }
    Ok(zip_path)
}

/// The zip unpacked into work/unpacked; that folder.
fn unpack(zip_path: &Path, work: &Path) -> Result<PathBuf, String> {
    let unpacked = work.join("unpacked");
    let f = fs::File::open(zip_path).map_err(|e| e.to_string())?;
    zip::ZipArchive::new(f)
        .and_then(|mut z| z.extract(&unpacked))
        .map_err(|e| format!("can't unpack the update: {e}"))?;
    Ok(unpacked)
}

/// The unpacked release's one folder: the one whose inner/GAME_EXE is the game.
fn release_folder(unpacked: &Path, inner: &Path) -> Result<PathBuf, String> {
    fs::read_dir(unpacked)
        .map_err(|e| e.to_string())?
        .flatten()
        .map(|e| e.path())
        .find(|p| p.join(inner).join(crate::game::GAME_EXE).is_file())
        .ok_or_else(|| "the update holds no game".to_string())
}

/// A folder install: the release's plain files over the game's.
fn copy_release(unpacked: &Path, game_dir: &Path) -> Result<(), String> {
    let folder = release_folder(unpacked, Path::new(""))?;
    let mut failed = Vec::new();
    for entry in fs::read_dir(&folder).map_err(|e| e.to_string())?.flatten() {
        let src = entry.path();
        let name = entry.file_name();
        if !src.is_file() || KEEP.iter().any(|k| name.eq_ignore_ascii_case(k)) {
            continue;
        }
        let dst = game_dir.join(&name);
        if fs::copy(&src, &dst).is_ok() {
            continue;
        }
        // in use (this launcher): it can be renamed but not overwritten
        let old = old_name(&dst);
        let _ = fs::remove_file(&old);
        if fs::rename(&dst, &old).and_then(|_| fs::copy(&src, &dst)).is_err() {
            failed.push(name.to_string_lossy().into_owned());
        }
    }
    if failed.is_empty() {
        Ok(())
    } else {
        Err(format!("couldn't replace {}", failed.join(", ")))
    }
}

/// A macOS app bundle: the release's Downhill Revived.app in place of app, the old one kept as
/// NAME.app.old until the next start (or put back when the new one can't be moved in).
fn replace_bundle(unpacked: &Path, app: &Path) -> Result<(), String> {
    let new = release_folder(unpacked, &Path::new("Contents").join("MacOS"))?;
    if app.to_string_lossy().contains("/AppTranslocation/") {
        return Err("macOS runs Downhill Revived from a read-only copy: move it to Applications, start it again, \
                    then update"
            .into());
    }
    let old = old_name(app);
    let _ = fs::remove_dir_all(&old);
    fs::rename(app, &old).map_err(|e| format!("can't replace {}: {e}", app.display()))?;
    move_dir(&new, app).map_err(|e| {
        let _ = fs::remove_dir_all(app);
        let _ = fs::rename(&old, app);
        format!("can't put the new {} in place: {e}", app.display())
    })
}

/// An AppImage: the downloaded image (new) in place of the running one (image), which is kept as
/// NAME.old until the next start (or put back when the new one can't be moved in). A running
/// AppImage can be renamed: its mount holds the file open, not its name.
fn replace_file(new: &Path, image: &Path) -> Result<(), String> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(new, fs::Permissions::from_mode(0o755)).map_err(|e| format!("{}: {e}", new.display()))?;
    }
    let old = old_name(image);
    let _ = fs::remove_file(&old);
    fs::rename(image, &old).map_err(|e| format!("can't replace {}: {e} (is its folder read-only?)", image.display()))?;
    let moved = fs::rename(new, image).or_else(|_| fs::copy(new, image).map(|_| ()));
    moved.map_err(|e| {
        let _ = fs::remove_file(image);
        let _ = fs::rename(&old, image);
        format!("can't put the new {} in place: {e}", image.display())
    })
}

/// The installed copy's Apps & features entry (Windows, game.rs UNINSTALL_KEY) names the version
/// now in its folder. Best effort: the entry is only what the list shows.
fn note_installed_version(version: &str) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        let key = format!(r"HKCU\{}", crate::game::UNINSTALL_KEY);
        let _ = std::process::Command::new("reg")
            .args(["add", &key, "/v", "DisplayVersion", "/t", "REG_SZ", "/d", version, "/f"])
            .creation_flags(CREATE_NO_WINDOW)
            .status();
    }
    #[cfg(not(windows))]
    let _ = version;
}

/// Renamed, or (on another volume) copied and the source removed.
fn move_dir(from: &Path, to: &Path) -> std::io::Result<()> {
    if fs::rename(from, to).is_ok() {
        return Ok(());
    }
    copy_dir(from, to)?;
    fs::remove_dir_all(from)
}

fn copy_dir(from: &Path, to: &Path) -> std::io::Result<()> {
    fs::create_dir_all(to)?;
    for e in fs::read_dir(from)? {
        let e = e?;
        let dst = to.join(e.file_name());
        if e.file_type()?.is_dir() {
            copy_dir(&e.path(), &dst)?;
        } else {
            fs::copy(e.path(), dst)?; // with its permissions
        }
    }
    Ok(())
}

fn old_name(p: &Path) -> PathBuf {
    let mut s = p.as_os_str().to_owned();
    s.push(".old");
    PathBuf::from(s)
}

/// What the last update left behind: *.old files, the old app bundle or AppImage and dhd-update/.
pub fn clean_up(place: &Place) {
    let _ = fs::remove_dir_all(place.data.join(WORK_DIR));
    if let Some(app) = &place.bundle {
        let _ = fs::remove_dir_all(old_name(app));
    }
    if let Some(image) = &place.appimage {
        let _ = fs::remove_file(old_name(image));
    }
    if let Ok(dir) = fs::read_dir(&place.game) {
        for e in dir.flatten() {
            if e.file_name().to_string_lossy().ends_with(".old") && e.path().is_file() {
                let _ = fs::remove_file(e.path());
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions() {
        assert!(newer("0.5.1", "0.5.0"));
        assert!(newer("v1.0.0", "0.9.9"));
        assert!(!newer("0.5.0", "0.5.0"));
        assert!(!newer("junk", "0.5.0"));
    }

    #[test]
    fn relative_urls() {
        assert_eq!(resolve("https://a.b/x/latest.json", "z.zip"), "https://a.b/x/z.zip");
        assert_eq!(resolve("https://a.b/x/latest.json?q", "/z.zip"), "https://a.b/z.zip");
        assert_eq!(resolve("https://a.b/x/latest.json", "https://c/z.zip"), "https://c/z.zip");
    }

    /// The official source answers (network: cargo test -- --ignored).
    #[test]
    #[ignore]
    fn official_source() {
        match latest(OFFICIAL_SOURCE, PLATFORM) {
            Ok(Some(r)) => {
                println!("{} {} {} bytes, {} chars of notes", r.version, r.url, r.size, r.notes.len());
                assert!(parse_version(&r.version).is_some());
            }
            // it answered (over HTTPS), but its newest release has no build for this platform yet
            Ok(None) => println!("the newest release has no {PLATFORM} build"),
            Err(e) => panic!("{e}"),
        }
    }

    /// latest.json from before "platforms" names the Windows zip; with them, each its own.
    #[test]
    fn manifest_platforms() {
        let sha = "ab".repeat(32);
        let old = serde_json::json!({"version": "0.6.4", "url": "w.zip", "sha256": sha});
        assert_eq!(read_manifest(&old, "https://a.b/latest.json", PLATFORM).unwrap().map(|r| r.url),
                   (PLATFORM == "win").then(|| "https://a.b/w.zip".to_string()));
        let new = serde_json::json!({"version": "0.7.0", "url": "w.zip", "sha256": sha, "platforms": {
            PLATFORM: {"url": "mine.zip", "sha256": sha, "size": 5}}});
        let r = read_manifest(&new, "https://a.b/latest.json", PLATFORM).unwrap().unwrap();
        assert_eq!((r.url.as_str(), r.size), ("https://a.b/mine.zip", 5));
        let other = serde_json::json!({"version": "0.7.0", "url": "w.zip", "sha256": sha, "platforms": {"none": {}}});
        assert!(read_manifest(&other, "x", PLATFORM).unwrap().is_none());
        let bad = serde_json::json!({"version": "0.7.0", "platforms": {PLATFORM: {"url": "mine.zip"}}});
        assert!(read_manifest(&bad, "x", PLATFORM).is_err());
    }

    /// 0.7.0's latest.json: the Windows zip at the top level and under win, macOS and Linux as
    /// -experimental zips. Each launcher takes its own entry's url as written (Windows never the
    /// Linux or macOS zip); a release with only experimental zips is no update for Windows.
    #[test]
    fn experimental_platforms() {
        let (w, m, l) = ("a".repeat(64), "b".repeat(64), "c".repeat(64));
        let base = "https://github.com/skuza-oss/downhill-revived/releases/download/v0.7.0/";
        let zip = |p: &str| format!("{base}DownhillRevived-0.7.0-{p}.zip");
        let manifest = serde_json::json!({"version": "0.7.0", "url": zip("win"), "sha256": w, "size": 1, "platforms": {
            "linux": {"url": zip("linux-experimental"), "sha256": l, "size": 3},
            "mac": {"url": zip("mac-experimental"), "sha256": m, "size": 2},
            "win": {"url": zip("win"), "sha256": w, "size": 1}}});
        let r = release_from("https://a.b/latest.json", &manifest, PLATFORM).unwrap().unwrap();
        let want = match PLATFORM {
            "win" => (zip("win"), w.clone()),
            "mac" => (zip("mac-experimental"), m.clone()),
            _ => (zip("linux-experimental"), l.clone()),
        };
        assert_eq!((r.url, r.sha256), want);

        // GitHub's release JSON without a latest.json asset: the zips by name and digest
        let asset = |p: &str, sha: &str| serde_json::json!({"name": format!("DownhillRevived-0.7.0-{p}.zip"),
            "browser_download_url": zip(p), "digest": format!("sha256:{sha}"), "size": 9});
        let all = serde_json::json!({"tag_name": "v0.7.0", "assets": [
            asset("linux-experimental", &l), asset("mac-experimental", &m), asset("win", &w)]});
        assert_eq!(release_from(OFFICIAL_SOURCE, &all, PLATFORM).unwrap().unwrap().url, want.0);
        let experimental = serde_json::json!({"tag_name": "v0.7.0", "assets": [
            asset("linux-experimental", &l), asset("mac-experimental", &m)]});
        let r = release_from(OFFICIAL_SOURCE, &experimental, PLATFORM).unwrap();
        assert_eq!(r.map(|r| r.url), (PLATFORM != "win").then(|| want.0.clone()));
    }

    /// A release with no build for this platform is no update, not a failed check (from
    /// n0buholic, PR #13); a zip without its digest, or an answer that names no release, still is.
    #[test]
    fn missing_platform_is_not_a_failed_check() {
        let other = if PLATFORM == "win" { "linux" } else { "win" };
        let sha = format!("sha256:{}", "ab".repeat(32));
        let body = serde_json::json!({"tag_name": "v0.7.0", "assets": [
            {"name": format!("DownhillRevived-0.7.0-{other}.zip"), "digest": sha}]});
        assert!(release_from(OFFICIAL_SOURCE, &body, PLATFORM).unwrap().is_none());
        let no_digest = serde_json::json!({"tag_name": "v0.7.0", "assets": [
            {"name": format!("DownhillRevived-0.7.0-{PLATFORM}.zip"), "browser_download_url": "https://a.b/z.zip"}]});
        assert!(release_from(OFFICIAL_SOURCE, &no_digest, PLATFORM).is_err());
        let unversioned = serde_json::json!({"platforms": {PLATFORM: {"url": "z.zip", "sha256": "ab".repeat(32)}}});
        assert!(release_from(OFFICIAL_SOURCE, &unversioned, PLATFORM).is_err());
    }

    fn zip_of(files: &[(String, &str)]) -> Vec<u8> {
        let mut out = Vec::new();
        let mut z = zip::ZipWriter::new(std::io::Cursor::new(&mut out));
        for (name, body) in files {
            z.start_file(name.as_str(), zip::write::SimpleFileOptions::default()).unwrap();
            z.write_all(body.as_bytes()).unwrap();
        }
        z.finish().unwrap();
        out
    }

    /// A latest.json naming zip (this platform's entry; the top level is another zip) and the zip,
    /// from a local HTTP server; its base address.
    fn serve(zip_bytes: Vec<u8>) -> String {
        serve_for(zip_bytes, PLATFORM, "zip")
    }

    /// serve() for platform's entry, a file with extension ext.
    fn serve_for(zip_bytes: Vec<u8>, platform: &str, ext: &str) -> String {
        use std::io::{BufRead, BufReader};
        use std::net::TcpListener;

        let sha: String = Sha256::digest(&zip_bytes).iter().map(|b| format!("{b:02x}")).collect();
        let manifest = format!(
            r#"{{"version":"9.9.9","url":"other.zip","sha256":"{}","platforms":{{"{platform}":{{"url":"DownhillRevived-9.9.9-{platform}.{ext}","sha256":"{sha}","size":{}}}}}}}"#,
            "0".repeat(64),
            zip_bytes.len()
        );
        let server = TcpListener::bind("127.0.0.1:0").unwrap();
        let base = format!("http://{}", server.local_addr().unwrap());
        std::thread::spawn(move || {
            for stream in server.incoming().take(2).flatten() {
                let mut reader = BufReader::new(stream.try_clone().unwrap());
                let mut line = String::new();
                reader.read_line(&mut line).unwrap();
                loop {
                    let mut h = String::new();
                    if reader.read_line(&mut h).unwrap() <= 2 {
                        break;
                    }
                }
                let body: &[u8] = if line.contains("latest.json") { manifest.as_bytes() } else { &zip_bytes };
                let mut s = stream;
                write!(s, "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len()).unwrap();
                s.write_all(body).unwrap();
            }
        });
        base
    }

    /// A folder install (Windows, Linux): the release over the game folder, whose dhd.ini survives.
    #[test]
    fn installs_a_release() {
        use crate::game::GAME_EXE;
        let base = serve(zip_of(&[
            (format!("DownhillRevived-9.9.9/{GAME_EXE}"), "new game"),
            ("DownhillRevived-9.9.9/README.txt".into(), "new readme"),
            ("DownhillRevived-9.9.9/dhd.ini".into(), "name = WRONG\n"),
        ]));
        let game = std::env::temp_dir().join(format!("dhd-launcher-update-{}", std::process::id()));
        let _ = fs::remove_dir_all(&game);
        fs::create_dir_all(&game).unwrap();
        fs::write(game.join(GAME_EXE), "old game").unwrap();
        fs::write(game.join("dhd.ini"), "name = SKUZA\n").unwrap();
        let place = Place { game: game.clone(), data: game.clone(), bundle: None, appimage: None, installed: false };

        let r = latest(&format!("{base}/latest.json"), PLATFORM).unwrap().unwrap();
        assert_eq!(r.version, "9.9.9");
        assert!(newer(&r.version, "0.5.0"));
        let mut last = 0;
        install(&r, &place, |done, _| last = done).unwrap();
        assert_eq!(last, r.size);
        assert_eq!(fs::read_to_string(game.join(GAME_EXE)).unwrap(), "new game");
        assert_eq!(fs::read_to_string(game.join("README.txt")).unwrap(), "new readme");
        assert_eq!(fs::read_to_string(game.join("dhd.ini")).unwrap(), "name = SKUZA\n");
        assert!(!game.join(WORK_DIR).exists());
        fs::remove_dir_all(&game).unwrap();
    }

    /// An app bundle (macOS): the new Downhill Revived.app in place of the old, kept as .app.old
    /// until clean_up; the work folder is in the data folder, outside the bundle.
    #[test]
    fn replaces_a_bundle() {
        use crate::game::GAME_EXE;
        let base = serve(zip_of(&[
            (format!("Downhill Revived.app/Contents/MacOS/{GAME_EXE}"), "new game"),
            ("Downhill Revived.app/Contents/Info.plist".into(), "new plist"),
        ]));
        let root = std::env::temp_dir().join(format!("dhd-launcher-bundle-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let app = root.join("Applications").join("Downhill Revived.app");
        let macos = app.join("Contents").join("MacOS");
        fs::create_dir_all(&macos).unwrap();
        fs::write(macos.join(GAME_EXE), "old game").unwrap();
        fs::write(macos.join("stale"), "only in the old release").unwrap();
        let data = root.join("Application Support");
        fs::create_dir_all(&data).unwrap();
        let place = Place { game: macos.clone(), data: data.clone(), bundle: Some(app.clone()), appimage: None, installed: false };

        let r = latest(&format!("{base}/latest.json"), PLATFORM).unwrap().unwrap();
        install(&r, &place, |_, _| ()).unwrap();
        assert_eq!(fs::read_to_string(macos.join(GAME_EXE)).unwrap(), "new game");
        assert_eq!(fs::read_to_string(app.join("Contents").join("Info.plist")).unwrap(), "new plist");
        assert!(!macos.join("stale").exists());
        assert!(old_name(&app).join("Contents").join("MacOS").join("stale").exists());
        assert!(!data.join(WORK_DIR).exists());
        clean_up(&place);
        assert!(!old_name(&app).exists());
        fs::remove_dir_all(&root).unwrap();
    }

    /// An AppImage (Linux): its own entry and asset, never the zip's; the new image in place of the
    /// running one, which stays as .old until clean_up.
    #[test]
    fn replaces_an_appimage() {
        let base = serve_for(b"new image".to_vec(), APPIMAGE, "AppImage");
        let root = std::env::temp_dir().join(format!("dhd-launcher-appimage-update-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let data = root.join("share");
        fs::create_dir_all(&data).unwrap();
        let image = root.join("DownhillRevived.AppImage");
        fs::write(&image, "old image").unwrap();
        let place = Place { game: root.join("mount"), data: data.clone(), bundle: None, appimage: Some(image.clone()), installed: false };
        assert_eq!(platform(&place), APPIMAGE);

        let r = latest(&format!("{base}/latest.json"), platform(&place)).unwrap().unwrap();
        assert!(r.url.ends_with(".AppImage"));
        install(&r, &place, |_, _| ()).unwrap();
        assert_eq!(fs::read_to_string(&image).unwrap(), "new image");
        assert_eq!(fs::read_to_string(old_name(&image)).unwrap(), "old image");
        assert!(!data.join(WORK_DIR).exists());
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(fs::metadata(&image).unwrap().permissions().mode() & 0o777, 0o755);
        }
        clean_up(&place);
        assert!(!old_name(&image).exists());
        fs::remove_dir_all(&root).unwrap();
    }

    /// Release assets by name: each platform's zip, the AppImage only for an AppImage.
    #[test]
    fn asset_names() {
        assert!(platform_asset("DownhillRevived-0.8.0-win.zip", "win"));
        assert!(platform_asset("DownhillRevived-0.8.0-linux-experimental.zip", "linux"));
        assert!(!platform_asset("DownhillRevived-0.8.0-linux-experimental.AppImage", "linux"));
        assert!(platform_asset("DownhillRevived-0.8.0-linux-experimental.AppImage", APPIMAGE));
        assert!(!platform_asset("DownhillRevived-0.8.0-linux-experimental.zip", APPIMAGE));
        assert!(!platform_asset("DownhillRevived-0.8.0-win-setup.exe", "win"));
        assert!(!platform_asset("DownhillRevived-0.8.0-mac-experimental.dmg", "mac"));
    }
}
