//! launcher.log in the data folder (finding 138): what the launcher's network work did, with the
//! full technical errors the window's one-line messages leave out. Lines are appended with the UTC
//! time; past MAX bytes the oldest half is dropped. The player's file: updates never replace it
//! (update.rs KEEP) and the uninstaller keeps it.

use std::fs;
use std::io::Write;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

pub const FILE: &str = "launcher.log";
const MAX: u64 = 256 * 1024;

/// Seconds since the Unix epoch as YYYY-MM-DD HH:MM:SS (UTC).
fn utc(secs: u64) -> String {
    let (days, rest) = ((secs / 86_400) as i64, secs % 86_400);
    // civil_from_days (Howard Hinnant's algorithm)
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!("{year:04}-{month:02}-{day:02} {:02}:{:02}:{:02}", rest / 3600, rest / 60 % 60, rest % 60)
}

/// One line (newlines in text become spaces) appended to launcher.log in data. Best effort.
pub fn line(data: &Path, text: &str) {
    let path = data.join(FILE);
    if fs::metadata(&path).is_ok_and(|m| m.len() > MAX) {
        if let Ok(old) = fs::read(&path) {
            let keep = &old[old.len() / 2..];
            let start = keep.iter().position(|&b| b == b'\n').map_or(0, |i| i + 1);
            let _ = fs::write(&path, &keep[start..]);
        }
    }
    let now = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs());
    let text = text.replace(['\r', '\n'], " ");
    if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(&path) {
        let _ = writeln!(f, "{} {} {text}", utc(now), env!("DHD_VERSION"));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utc_dates() {
        assert_eq!(utc(0), "1970-01-01 00:00:00");
        assert_eq!(utc(1_793_621_045), "2026-11-02 12:04:05");
        assert_eq!(utc(951_782_400), "2000-02-29 00:00:00");
    }

    #[test]
    fn appends_and_trims() {
        let dir = std::env::temp_dir().join(format!("dhd-launcher-log-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        line(&dir, "first\nline");
        line(&dir, "second");
        let text = fs::read_to_string(dir.join(FILE)).unwrap();
        assert_eq!(text.lines().count(), 2);
        assert!(text.lines().next().unwrap().ends_with("first line"));
        fs::write(dir.join(FILE), "x\n".repeat(MAX as usize / 2 + 10)).unwrap();
        line(&dir, "after");
        let len = fs::metadata(dir.join(FILE)).unwrap().len();
        assert!(len < MAX, "{len}");
        assert!(fs::read_to_string(dir.join(FILE)).unwrap().ends_with("after\n"));
        fs::remove_dir_all(&dir).unwrap();
    }
}
