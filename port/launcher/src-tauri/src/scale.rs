//! The resolution scale the game drew at in its last run, and how far this GPU goes (finding 136).
//!
//! The launcher can't make the game's GPU device, so it reads what the game found: the one line
//! the native renderer writes to dhd.log once it knows the scale it draws at
//! (port/gs/gpu_rust_shared.inc, native_scale_report):
//!
//! ```text
//! gs native: resolution scale 4x, capped: 8x asked, this GPU allows up to 4x (graphics memory: 8x needs 1024 MiB, ...)
//! gs native: resolution scale 3x: window asked (1080 lines, picture 448), this GPU allows up to 8x
//! ```
//!
//! A fresh install (no dhd.log yet) or a run without the native renderer has no such line: the
//! settings then say nothing about it.

use serde::Serialize;
use std::io::{BufRead, BufReader, Read};
use std::path::Path;

const PREFIX: &str = "gs native: resolution scale ";

/// The line's answer.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ScaleReport {
    /// The scale the pictures were drawn at.
    pub used: u32,
    /// What was asked for: a scale ("8"), or "window" for the window's match.
    pub asked: String,
    /// The largest scale this GPU takes.
    pub limit: u32,
    /// What holds the limit below 8: "memory" (graphics memory) or "texture" (the largest texture).
    pub reason: Option<&'static str>,
}

/// One dhd.log line, when it is the scale's.
pub fn parse(line: &str) -> Option<ScaleReport> {
    let rest = line.trim_end().strip_prefix(PREFIX)?;
    let (used, rest) = rest.split_once('x')?;
    let used = used.parse().ok()?;
    let rest = rest.strip_prefix(", capped").unwrap_or(rest).strip_prefix(": ")?;
    let (asked, rest) = rest.split_once(" asked")?;
    let asked = match asked.strip_suffix('x') {
        Some(n) => n.parse::<u32>().ok()?.to_string(),
        None if asked == "window" || asked == "canvas" => "window".to_string(),
        None => return None,
    };
    let (_, rest) = rest.split_once("this GPU allows up to ")?;
    let (limit, why) = rest.split_once('x')?;
    let limit = limit.parse().ok()?;
    let reason = if why.starts_with(" (graphics memory") {
        Some("memory")
    } else if why.starts_with(" (largest texture") {
        Some("texture")
    } else {
        None
    };
    Some(ScaleReport { used, asked, limit, reason })
}

/// The last run's report: the last such line in the first MAX_READ bytes of dhd.log (the line comes
/// with the first picture, near the start; a long run's log can be large).
pub fn last_run(log: &Path) -> Option<ScaleReport> {
    const MAX_READ: u64 = 4 << 20;
    let file = std::fs::File::open(log).ok()?;
    let mut found = None;
    let mut reader = BufReader::new(file.take(MAX_READ));
    let mut line = Vec::new();
    while reader.read_until(b'\n', &mut line).ok()? > 0 {
        if line.starts_with(PREFIX.as_bytes()) {
            found = parse(&String::from_utf8_lossy(&line)).or(found);
        }
        line.clear();
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    fn report(used: u32, asked: &str, limit: u32, reason: Option<&'static str>) -> Option<ScaleReport> {
        Some(ScaleReport { used, asked: asked.into(), limit, reason })
    }

    /// The lines as native_scale_report writes them.
    #[test]
    fn lines() {
        // from runs on a Ryzen 5 7600's integrated GPU with dhd.ini scale = 8, then window
        assert_eq!(parse("gs native: resolution scale 4x, capped: 8x asked, this GPU allows up to 4x (graphics memory: \
                          8x needs 1024 MiB, over the 242 MiB budget, half of the 485 MiB the GPU has)"),
                   report(4, "8", 4, Some("memory")));
        assert_eq!(parse("gs native: resolution scale 2x: window asked (720 lines, picture 448), this GPU allows up to 4x \
                          (graphics memory: 5x needs 400 MiB, over the 242 MiB budget, half of the 485 MiB the GPU has)\r\n"),
                   report(2, "window", 4, Some("memory")));
        assert_eq!(parse("gs native: resolution scale 2x: 2x asked, this GPU allows up to 4x (graphics memory: \
                          5x needs 400 MiB, over the 242 MiB budget, half of the 485 MiB the GPU has)"),
                   report(2, "2", 4, Some("memory")));
        assert_eq!(parse("gs native: resolution scale 2x: 2x asked, this GPU allows up to 8x"), report(2, "2", 8, None));
        assert_eq!(parse("gs native: resolution scale 3x: window asked (1080 lines, picture 448), this GPU allows up to 8x"),
                   report(3, "window", 8, None));
        assert_eq!(parse("gs native: resolution scale 4x, capped: window asked (2160 lines, picture 448), this GPU allows \
                          up to 4x (graphics memory: 5x needs 400 MiB, over the 256 MiB budget, half of the 512 MiB the GPU has)"),
                   report(4, "window", 4, Some("memory")));
        assert_eq!(parse("gs native: resolution scale 4x, capped: 8x asked, this GPU allows up to 4x (largest texture: 8192 pixels a side)"),
                   report(4, "8", 4, Some("texture")));
        // other lines
        assert_eq!(parse("gs native: resolution scale now 4 (DHD_SCALE)"), None);
        assert_eq!(parse("gs: resolution scale 9 is more than this renderer draws; 8 (DHD_SCALE)"), None);
        assert_eq!(parse("perf: render native scale=4 picture=2560x1792"), None);
    }

    /// The last line in the log counts; no log, or one without the line: nothing known.
    #[test]
    fn from_the_log() {
        let dir = std::env::temp_dir().join(format!("dhd-launcher-scale-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let log = dir.join("dhd.log");
        assert_eq!(last_run(&log), None);
        std::fs::write(&log, "dhd 0.7.7\ngs: GPUs AMD Radeon(TM) Graphics\n").unwrap();
        assert_eq!(last_run(&log), None);
        std::fs::write(&log, b"dhd 0.7.7\n\xff\xfe odd bytes\n\
            gs native: resolution scale 2x: 2x asked, this GPU allows up to 4x (graphics memory: 5x needs 400 MiB)\n\
            gs native: resolution scale 4x, capped: 8x asked, this GPU allows up to 4x (graphics memory: 8x needs 1024 MiB)\n\
            perf: render native scale=4\n").unwrap();
        assert_eq!(last_run(&log), report(4, "8", 4, Some("memory")));
        std::fs::remove_dir_all(dir).unwrap();
    }
}
