//! dhd.ini's text: one "key = value" a line. Kept as the bytes it was read from, so a save changes
//! only the lines of the setting it writes; every other line (comments, [sections], unknown keys,
//! blank lines, line endings, text in any encoding) stays byte for byte.
//!
//! The rules:
//! - spaces and tabs around a line, a key and a value don't count; a UTF-8 byte order mark before
//!   the first line neither;
//! - a line starting with ; or # is a comment, one starting with [ a section (ignored: every key
//!   is global), a line without = is kept and ignored;
//! - keys compare without case, and a setting's variable name is another spelling of its key
//!   (`DHD_SCALE = 3` is `scale = 3`);
//! - a value in double quotes is taken as it is (a path may hold ; or #); otherwise it ends at a ;
//!   or # that starts it or follows a space or tab (a comment);
//! - an empty value is no value: the default applies;
//! - when a setting has several lines, the first with a value counts.

use crate::table::{self, Setting};

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Ini {
    text: Vec<u8>,
}

/// A line: its content (no line ending) and where it starts and ends (with its line ending).
#[derive(Clone, Copy)]
struct Line {
    start: usize,
    content_end: usize,
    end: usize,
}

const BOM: &[u8] = b"\xEF\xBB\xBF";

fn trim(mut s: &[u8]) -> &[u8] {
    while let [b' ' | b'\t' | b'\r', rest @ ..] = s {
        s = rest;
    }
    while let [rest @ .., b' ' | b'\t' | b'\r'] = s {
        s = rest;
    }
    s
}

/// A line's key and its value as written (quotes and comment taken off), or None for a comment,
/// a section, a blank line or one without a key.
fn entry(line: &[u8]) -> Option<(&[u8], &[u8])> {
    let s = trim(line.strip_prefix(BOM).unwrap_or(line));
    if s.is_empty() || matches!(s[0], b';' | b'#' | b'[') {
        return None;
    }
    let eq = s.iter().position(|&b| b == b'=')?;
    let key = trim(&s[..eq]);
    if key.is_empty() {
        return None;
    }
    let v = trim(&s[eq + 1..]);
    if let Some(rest) = v.strip_prefix(b"\"") {
        if let Some(end) = rest.iter().position(|&b| b == b'"') {
            return Some((key, &rest[..end]));
        }
    }
    let cut = (0..v.len()).find(|&i| matches!(v[i], b';' | b'#') && (i == 0 || matches!(v[i - 1], b' ' | b'\t')));
    Some((key, trim(&v[..cut.unwrap_or(v.len())])))
}

fn text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

/// The spellings of `key`: a setting's key and variable name, else the key itself.
fn names(key: &str) -> (Option<&'static Setting>, [&str; 2]) {
    match table::find(key) {
        Some(s) => (Some(s), [s.key, if s.env.is_empty() { s.key } else { s.env }]),
        None => (None, [key, key]),
    }
}

/// A key or value that a line can hold: one line, and a key without = or a comment's start.
pub fn check(key: &str, value: &str) -> Result<(), String> {
    let key_ok = !key.is_empty() && key.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_');
    if !key_ok || value.contains(['\n', '\r']) {
        return Err(format!("not a setting: {key}"));
    }
    Ok(())
}

impl Ini {
    pub fn parse(bytes: &[u8]) -> Ini {
        Ini { text: bytes.to_vec() }
    }

    pub fn bytes(&self) -> &[u8] {
        &self.text
    }

    fn lines(&self) -> Vec<Line> {
        let mut out = Vec::new();
        let mut start = 0;
        while start < self.text.len() {
            let end = self.text[start..].iter().position(|&b| b == b'\n').map_or(self.text.len(), |i| start + i + 1);
            let mut content_end = end;
            if self.text[..content_end].ends_with(b"\n") {
                content_end -= 1;
            }
            if self.text[start..content_end].ends_with(b"\r") {
                content_end -= 1;
            }
            out.push(Line { start, content_end, end });
            start = end;
        }
        out
    }

    fn content(&self, l: Line) -> &[u8] {
        &self.text[l.start..l.content_end]
    }

    /// The lines that hold one of `names`, in order, with their values.
    fn lines_of(&self, names: &[&str]) -> Vec<(Line, &[u8])> {
        self.lines()
            .into_iter()
            .filter_map(|l| {
                let (k, v) = entry(self.content(l))?;
                names.iter().any(|n| n.as_bytes().eq_ignore_ascii_case(k)).then_some((l, v))
            })
            .collect()
    }

    /// The value dhd.ini gives `key` (the first of its lines with one), as written. "size" is
    /// width x height (a missing one the game's own), or monitor when width is.
    pub fn written(&self, key: &str) -> Option<String> {
        if key.eq_ignore_ascii_case("size") {
            let side = |k: &str| self.written(k).or_else(|| table::find(k).map(|s| s.builtin.to_string()));
            if !self.has("width") && !self.has("height") {
                return None;
            }
            let (w, h) = (side("width")?, side("height")?);
            return Some(if w.eq_ignore_ascii_case("monitor") { "monitor".into() } else { format!("{w}x{h}") });
        }
        let (_, names) = names(key);
        self.lines_of(&names).into_iter().map(|(_, v)| v).find(|v| !v.is_empty()).map(text)
    }

    /// Whether dhd.ini has a value for `key`.
    pub fn has(&self, key: &str) -> bool {
        self.written(key).is_some()
    }

    /// The player's value of `key`: dhd.ini's, as the game reads it (a choice's own spelling), else
    /// the default. A width and height of their own make "size" the monitor default only when
    /// neither is there.
    pub fn value(&self, key: &str) -> String {
        let setting = table::find(key);
        match (self.written(key), setting) {
            (Some(v), Some(s)) => s.canonical(&v).map_or(v, String::from),
            (Some(v), None) => v,
            (None, Some(s)) => s.default.into(),
            (None, None) => String::new(),
        }
    }

    /// `key`'s value as the environment would make it: the variable when it is set (the
    /// environment overrides dhd.ini), else `value`.
    pub fn effective(&self, key: &str, env: impl Fn(&str) -> Option<String>) -> String {
        match table::find(key).filter(|s| !s.env.is_empty()).and_then(|s| env(s.env)) {
            Some(v) => v,
            None => self.value(key),
        }
    }

    /// Keys of lines that aren't settings, the install id or DHD_* variables (the game logs them).
    pub fn unknown_keys(&self) -> Vec<String> {
        self.lines()
            .into_iter()
            .filter_map(|l| entry(self.content(l)).map(|(k, _)| text(k)))
            .filter(|k| table::find(k).is_none() && !k.eq_ignore_ascii_case(crate::install::KEY))
            .filter(|k| !k.get(..4).is_some_and(|p| p.eq_ignore_ascii_case("DHD_")))
            .collect()
    }

    /// Every line's key and value, in order (values as written, empty ones included).
    pub fn entries(&self) -> Vec<(String, String)> {
        self.lines().into_iter().filter_map(|l| entry(self.content(l)).map(|(k, v)| (text(k), text(v)))).collect()
    }

    /// Writes `key = value`: in place of the setting's first line (any spelling), whose line ending
    /// it keeps; the setting's other lines go (one line holds it after a save); without a line, at
    /// the end. Every other line stays as it was. "size" writes width and height (monitor: both).
    pub fn set(&mut self, key: &str, value: &str) -> Result<(), String> {
        check(key, value)?;
        if key.eq_ignore_ascii_case("size") {
            let (w, h) = if value.eq_ignore_ascii_case("monitor") {
                ("monitor", "monitor")
            } else {
                value.split_once(['x', 'X']).ok_or_else(|| format!("not a window size: {value}"))?
            };
            self.set("width", w.trim())?;
            return self.set("height", h.trim());
        }
        let (setting, names) = names(key);
        let key = setting.map_or(key, |s| s.key);
        let quote = if value.contains([';', '#', '"']) || value.starts_with([' ', '\t']) || value.ends_with([' ', '\t']) {
            "\""
        } else {
            ""
        };
        let line = if value.is_empty() { format!("{key} =") } else { format!("{key} = {quote}{value}{quote}") };
        let lines = self.lines();
        let ending: &[u8] = match lines.first() {
            Some(l) if self.text[l.content_end..l.end].starts_with(b"\r") => b"\r\n",
            _ => b"\n",
        };
        let ours: Vec<Line> = self.lines_of(&names).into_iter().map(|(l, _)| l).collect();
        let mut out = Vec::with_capacity(self.text.len() + line.len() + 2);
        let mut at = 0;
        for (i, l) in ours.iter().enumerate() {
            out.extend_from_slice(&self.text[at..l.start]);
            if i == 0 {
                if l.start == 0 && self.text.starts_with(BOM) {
                    out.extend_from_slice(BOM);
                }
                out.extend_from_slice(line.as_bytes());
                let own = &self.text[l.content_end..l.end];
                out.extend_from_slice(if own.is_empty() { ending } else { own });
            }
            at = l.end;
        }
        out.extend_from_slice(&self.text[at..]);
        if ours.is_empty() {
            if !out.is_empty() && !out.ends_with(b"\n") {
                out.extend_from_slice(ending);
            }
            out.extend_from_slice(line.as_bytes());
            out.extend_from_slice(ending);
        }
        self.text = out;
        Ok(())
    }
}
