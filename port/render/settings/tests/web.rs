//! tests/web_corpus.json: dhd.ini cases with what the native game makes of them (its start's
//! variables and unknown keys, the bytes after one save), for the browser's check
//! (port/web/check_settings.mjs), which runs the same cases through settings.js and the crate's web
//! module and must give the same. Texts are bytes, one character a byte (U+0000-U+00FF). This test
//! fails while a stored result differs from the crate's; with DHD_SETTINGS_BLESS=1 it writes them.

use dhd_settings::{edited, environment, Ini};
use std::path::Path;

/// One case: dhd.ini's text (`stored` false: none yet, below), the variables already set, one save.
struct Case {
    name: &'static str,
    text: &'static [u8],
    stored: bool,
    set: &'static [&'static str],
    key: &'static str,
    value: &'static str,
}

const CASES: &[Case] = &[
    // the round trip: comments, a section, a key twice (not the saved one), an unknown key, CRLF
    Case {
        name: "comments and duplicates",
        text: b"; Downhill Revived settings\r\n# OPTIONS > REVIVED SETTINGS writes here\r\n[video]\r\nscale = 2 ; two\r\nfps = 120\r\n\
                fps = 144\r\nfoo = bar\r\nwidescreen = 0\r\n\r\ngame_options = 1 800\r\n",
        stored: true,
        set: &[],
        key: "scale",
        value: "3",
    },
    Case {
        name: "the saved key's other lines and spellings",
        text: b"a = 1\nDHD_SCALE = 5\nb = 2\nscale =\nscale = 3\nc = 3\n",
        stored: true,
        set: &[],
        key: "scale",
        value: "6",
    },
    Case {
        name: "byte order mark, CP1252, no final line ending",
        text: b"\xEF\xBB\xBF; Downhill Revived\r\n; caf\xE9 (CP1252)\r\n[video]\r\nScale = 2 ; two\r\n\r\nfoo = bar\r\nfont = sharp",
        stored: true,
        set: &[],
        key: "name",
        value: "SKUZA",
    },
    Case { name: "no settings stored yet", text: b"", stored: false, set: &[], key: "fps", value: "144" },
    Case { name: "a value that needs quotes", text: b"server = wss://a\n", stored: true, set: &[], key: "server", value: "wss://a;b #2" },
    Case {
        name: "values as the game reads them",
        text: b"vibration = off\nfont = OFF\nrenderer = whatever\nwidescreen = 21:9\nwindow = Borderless\nunlock_all = false\nfps = 240\n",
        stored: true,
        set: &[],
        key: "unlock_all",
        value: "1",
    },
    Case {
        name: "unknown lines and DHD_* variables",
        text: b"[x]\nno equals sign here\nfoo = bar\nDHD_SOMETHING = 7\nDHD_EMPTY =\n= no key\ngame_options = 1 2 3\n",
        stored: true,
        set: &[],
        key: "scale",
        value: "3",
    },
    Case {
        name: "the environment wins",
        text: b"fps = 144\nscale = 4\n",
        stored: true,
        set: &["DHD_FPS", "DHD_WIDESCREEN"],
        key: "fps",
        value: "60",
    },
];

fn bytes_json(b: &[u8]) -> String {
    let mut out = String::from("\"");
    for &c in b {
        match c {
            b'"' => out.push_str("\\\""),
            b'\\' => out.push_str("\\\\"),
            0x20..=0x7E => out.push(c as char),
            _ => out.push_str(&format!("\\u{c:04x}")),
        }
    }
    out.push('"');
    out
}

fn list_json<I: IntoIterator<Item = String>>(items: I) -> String {
    format!("[{}]", items.into_iter().collect::<Vec<_>>().join(", "))
}

fn corpus() -> String {
    let mut cases = Vec::new();
    for c in CASES {
        // Nothing stored yet: a player's first start, where the launcher has made dhd.ini (`create`:
        // HEADER) before the game reads it. The browser, which has no launcher, stands for that; a
        // start without dhd.ini (the tools') is DHD_SETTINGS=none there.
        let ini = Ini::parse(if c.stored { c.text } else { dhd_settings::HEADER.as_bytes() });
        let env = environment(Some(&ini), |n| c.set.contains(&n));
        let saved = edited(c.stored.then_some(c.text), c.key, c.value).unwrap();
        let vars = env.vars.iter().map(|(n, v)| format!("[{}, {}]", bytes_json(n.as_bytes()), bytes_json(v.as_bytes())));
        cases.push(format!(
            "  {{\n    \"name\": {},\n    \"text\": {},\n    \"stored\": {},\n    \"set\": {},\n    \"save\": [{}, {}],\n    \
             \"vars\": {},\n    \"unknown\": {},\n    \"saved\": {}\n  }}",
            bytes_json(c.name.as_bytes()),
            bytes_json(c.text),
            c.stored,
            list_json(c.set.iter().map(|s| bytes_json(s.as_bytes()))),
            bytes_json(c.key.as_bytes()),
            bytes_json(c.value.as_bytes()),
            list_json(vars),
            list_json(env.unknown.iter().map(|k| bytes_json(k.as_bytes()))),
            bytes_json(saved.bytes())
        ));
    }
    format!("[\n{}\n]\n", cases.join(",\n"))
}

#[test]
fn web_corpus_json_is_current() {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/web_corpus.json");
    let want = corpus();
    if std::env::var_os("DHD_SETTINGS_BLESS").is_some() {
        std::fs::write(&path, &want).unwrap();
    }
    let have = std::fs::read_to_string(&path).unwrap_or_default().replace("\r\n", "\n");
    assert!(have == want, "tests/web_corpus.json is not the crate's: DHD_SETTINGS_BLESS=1 cargo test -p dhd_settings");
}
