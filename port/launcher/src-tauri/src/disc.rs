//! The disc image, checked twice:
//! - to boot, as the game checks it before it starts (port/hle/host.c disc_problem), without its
//!   last step (the game's program and data hashed, game_data.c): an ISO 9660 volume whose
//!   SYSTEM.CNF boots SCUS_971.77, with the whole declared volume (shared disc_check.c).
//!   The launcher only refuses what the game would refuse; the game
//!   still checks the data itself. The game's other releases are named (finding 100).
//! - for online races, which need the USA disc's game files (finding 098): the game's own check,
//!   dhd_disc (port/render/disc), with the game's iso-check.txt, so the game doesn't hash the
//!   files again.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

use std::ffi::{c_char, c_int, c_void, CStr};

// disc_check.h's answers
const DISC_GAME: c_int = 0;
const DISC_NOT_ISO: c_int = 1;
const DISC_NO_CNF: c_int = 2;
const DISC_CUT: c_int = 4;

extern "C" {
    fn disc_check_image(read: unsafe extern "C" fn(*mut c_void, u64, *mut c_void, usize) -> c_int,
        context: *mut c_void, size: u64, id: *mut c_char, id_size: usize, volume: *mut u64) -> c_int;
}

unsafe extern "C" fn read_at(context: *mut c_void, offset: u64, buffer: *mut c_void, n: usize) -> c_int {
    let file = &mut *(context as *mut File);
    let bytes = std::slice::from_raw_parts_mut(buffer as *mut u8, n);
    (file.seek(SeekFrom::Start(offset)).is_ok() && file.read_exact(bytes).is_ok()) as c_int
}

/// The game's other releases (finding 100): the table the game reads too (host.c).
const OTHER_RELEASES: &str = include_str!("../../../hle/other_releases.inc");

/// The quoted arguments of each of the table's name(...) lines.
fn table(name: &'static str) -> impl Iterator<Item = Vec<&'static str>> {
    OTHER_RELEASES
        .lines()
        .filter_map(move |l| l.trim().strip_prefix(name)?.strip_prefix('('))
        .map(|args| args.split('"').skip(1).step_by(2).collect())
}

/// A boot file's letters and digits, upper case: SCPS_150.62 and SCPS-15062 alike.
fn serial_key(boot: &str) -> String {
    boot.chars().filter(char::is_ascii_alphanumeric).map(|c| c.to_ascii_uppercase()).collect()
}

/// What the release booting boot is, when it is one of the game's other releases.
fn other_release(boot: &str) -> Option<&'static str> {
    let key = serial_key(boot);
    if key.is_empty() {
        return None;
    }
    table("OTHER_RELEASE").find(|a| a.len() == 2 && serial_key(a[0]) == key).map(|a| a[1])
}

/// None when the file is Downhill Domination (USA); else why not, in the game's words.
pub fn problem(path: &Path) -> Option<String> {
    let Ok(mut f) = File::open(path) else {
        return Some(if path.exists() { "The file can't be opened." } else { "The file isn't there any more." }.into());
    };
    let size = match f.metadata() {
        Ok(m) => m.len(),
        Err(_) => return Some("The file can't be read.".into()),
    };
    let mut id = [0 as c_char; 32];
    let code = unsafe { disc_check_image(read_at, &mut f as *mut File as *mut c_void,
        size, id.as_mut_ptr(), id.len(), std::ptr::null_mut()) };
    match code {
        DISC_GAME => return None,
        DISC_NOT_ISO => return Some("It isn't a disc image (no ISO 9660 volume in it).".into()),
        DISC_NO_CNF => return Some("It has no SYSTEM.CNF, so it isn't a PlayStation 2 game disc.".into()),
        DISC_CUT => return Some("The disc image is incomplete: it does not hold its whole declared volume.".into()),
        _ => {}
    }
    let boot = unsafe { CStr::from_ptr(id.as_ptr()) }.to_string_lossy().into_owned();
    let id = if boot.is_empty() { None } else { Some(boot) };
    if let Some(what) = id.as_deref().and_then(other_release) {
        let more = table("OTHER_RELEASES_MORE").next().and_then(|a| a.first().copied()).unwrap_or_default();
        return Some(format!("This is {what}. Downhill Revived needs the USA disc (SCUS-97177) for now. More: {more}"));
    }
    Some(match id {
        Some(id) => format!("It is another PlayStation 2 disc ({id}), not Downhill Domination (USA), SCUS-97177."),
        None => "It is another PlayStation 2 disc, not Downhill Domination (USA), SCUS-97177.".into(),
    })
}

/// The game's online cache, in the data folder (DHD_DATA_DIR: online_menu.c iso_check_path).
pub fn cache_path(data: &Path) -> PathBuf {
    data.join("iso-check.txt")
}

/// The online check of the disc image at path (dhd_disc::check, with the game's iso-check.txt in
/// the data folder): true for the USA disc's game files, which online races take. progress as in
/// dhd_disc::hash; Err(None) when it stopped it.
pub fn online_check(data: &Path, path: &Path, progress: impl FnMut(u64, u64) -> bool) -> Result<bool, Option<String>> {
    match dhd_disc::check(Some(&cache_path(data)), path, progress) {
        Ok((files, _)) => Ok(files.is_usa()),
        // no file system to hash, or a cut dump: not the USA disc's files
        Err(dhd_disc::Error::Format(_)) => Ok(false),
        Err(dhd_disc::Error::Stopped) => Err(None),
        Err(dhd_disc::Error::Read(e)) => Err(Some(e.to_string())),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn temp(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!("dhd-launcher-disc-{}-{:?}-{name}", std::process::id(), std::thread::current().id()))
    }

    fn fixture(name: &str) -> PathBuf {
        let dir = temp("fixtures");
        // the one synthetic set the C and Java tests use too (python3 on macOS and Linux)
        let made = ["python3", "python"].iter().any(|python| {
            std::process::Command::new(python)
                .arg(concat!(env!("CARGO_MANIFEST_DIR"), "/../../host/disc_fixtures.py"))
                .arg(&dir).status().is_ok_and(|s| s.success())
        });
        assert!(made, "port/host/disc_fixtures.py needs python");
        dir.join(format!("{name}.iso"))
    }

    #[test]
    fn checks_the_boot_file() {
        let good = fixture("good");
        for (name, message) in [("no-volume", "no ISO"), ("no-cnf", "no SYSTEM.CNF"),
            ("cut", "incomplete"), ("tiny", "no ISO")] {
            assert!(problem(&fixture(name)).unwrap().contains(message));
        }
        assert_eq!(problem(&good), None);
        let other = fixture("other");
        assert!(problem(&other).unwrap().contains("(SLUS_203.12)"));
        assert!(problem(Path::new("no-such.iso")).unwrap().contains("isn't there"));
        let _ = fs::remove_file(good);
        let _ = fs::remove_file(other);
    }

    #[test]
    fn names_the_other_releases() {
        let more = "More: https://github.com/skuza-oss/downhill-revived/blob/main/docs/findings/100-other-releases.md";
        let eu = fixture("europe");
        assert_eq!(
            problem(&eu).unwrap(),
            format!("This is the European disc (SLES-52202). Downhill Revived needs the USA disc (SCUS-97177) for now. {more}")
        );
        let jp = fixture("japan");
        assert_eq!(
            problem(&jp).unwrap(),
            format!(
                "This is the Japanese release, MT Bikers (SCPS-15062). Downhill Revived needs the USA disc (SCUS-97177) \
                 for now. {more}"
            )
        );
        // matched by letters and digits only, in any case
        assert_eq!(other_release("scps-15062"), Some("the Japanese release, MT Bikers (SCPS-15062)"));
        assert_eq!(other_release("SLES_522.03"), None);
        assert_eq!(other_release(""), None);
        assert_eq!(table("OTHER_RELEASE").count(), 2);
        let _ = fs::remove_file(eu);
        let _ = fs::remove_file(jp);
    }

    #[test]
    fn checks_the_game_files_for_online() {
        let data = temp("data");
        std::fs::create_dir_all(&data).unwrap();
        // an image that boots, but whose files aren't the USA disc's
        let img = fixture("good");
        assert_eq!(online_check(&data, &img, |_, _| true), Ok(false));
        // the answer is in the game's iso-check.txt now: not hashed again
        assert!(std::fs::read_to_string(cache_path(&data)).unwrap().starts_with("files:"));
        assert_eq!(online_check(&data, &img, |_, _| panic!("hashed again")), Ok(false));
        let _ = std::fs::remove_file(cache_path(&data));
        // stopped when another disc is chosen
        assert_eq!(online_check(&data, &img, |_, _| false), Err(None));
        // not an ISO 9660 image: not the USA disc's files either
        let text = temp("text.iso");
        std::fs::write(&text, b"abc").unwrap();
        assert_eq!(online_check(&data, &text, |_, _| true), Ok(false));
        assert!(matches!(online_check(&data, Path::new("no-such.iso"), |_, _| true), Err(Some(_))));
        let _ = std::fs::remove_file(img);
        let _ = std::fs::remove_file(text);
        let _ = std::fs::remove_dir_all(data);
    }
}
