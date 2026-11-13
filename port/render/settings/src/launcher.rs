//! The launcher's view of the settings (finding 225): the JSON its page draws from, made here once
//! for every platform. The desktop launcher (port/launcher, Tauri) returns it from its
//! `settings_schema` and `snapshot` commands; a phone's bridge (Android: port/host/android_launcher.c
//! through the C interface in c.rs) returns the same text, so the page sees one shape everywhere
//! (port/launcher/BRIDGE.md).
//!
//! - `schema_json`: every setting as `{"key", "default", "choices": [[value, label]...], "rule"}`,
//!   `rule` the rider name's (`{"letters", "max", "hint"}`) for `name`, else null.
//! - `values_json`: every setting's value as dhd.ini gives it, else its default (`Ini::value`), as
//!   one object `{key: value}`.

use crate::{name, Ini, SETTINGS};
use std::fmt::Write;

/// `s` as a JSON string.
pub fn json_string(out: &mut String, s: &str) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

/// Every setting's default, choices and (the rider name's) rule.
pub fn schema_json() -> String {
    let mut out = String::from("[");
    for (i, s) in SETTINGS.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str("{\"key\":");
        json_string(&mut out, s.key);
        out.push_str(",\"default\":");
        json_string(&mut out, s.default);
        out.push_str(",\"choices\":[");
        for (j, c) in s.choices.iter().enumerate() {
            if j > 0 {
                out.push(',');
            }
            out.push('[');
            json_string(&mut out, c.value);
            out.push(',');
            json_string(&mut out, c.label);
            out.push(']');
        }
        out.push_str("],\"rule\":");
        if s.key == "name" {
            out.push_str("{\"letters\":");
            json_string(&mut out, name::LETTERS);
            let _ = write!(out, ",\"max\":{},\"hint\":", name::MAX);
            json_string(&mut out, name::RULE);
            out.push('}');
        } else {
            out.push_str("null");
        }
        out.push('}');
    }
    out.push(']');
    out
}

/// Every setting's value: dhd.ini's, else its default (an empty `Ini` when there is no file).
pub fn values_json(ini: &Ini) -> String {
    let mut out = String::from("{");
    for (i, s) in SETTINGS.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        json_string(&mut out, s.key);
        out.push(':');
        json_string(&mut out, &ini.value(s.key));
    }
    out.push('}');
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn schema_lists_every_setting_with_the_name_rule() {
        let s = schema_json();
        assert!(s.starts_with("[{\"key\":\"iso\",\"default\":\"\",\"choices\":[],\"rule\":null}"));
        assert!(s.contains("{\"key\":\"scale\",\"default\":\"2\",\"choices\":[[\"1\",\"1x\"],"));
        assert!(s.contains(&format!("\"rule\":{{\"letters\":\"{}\",\"max\":{},", name::LETTERS, name::MAX)));
        assert_eq!(s.matches("{\"key\":").count(), SETTINGS.len());
    }

    #[test]
    fn values_follow_the_file_then_the_default() {
        let ini = Ini::parse(b"scale = 4\nname = A \"B\"\\\n");
        let v = values_json(&ini);
        assert!(v.contains("\"scale\":\"4\""));
        assert!(v.contains("\"fps\":\"60\""), "a default");
        assert!(v.contains("\"name\":\"A \\\"B\\\"\\\\\""), "escaped: {v}");
        assert!(v.contains("\"launcher\":\"1\""), "the launcher opens at start by default");
    }

    #[test]
    fn control_characters_are_escaped() {
        let mut out = String::new();
        json_string(&mut out, "a\u{1}\n");
        assert_eq!(out, "\"a\\u0001\\n\"");
    }
}
