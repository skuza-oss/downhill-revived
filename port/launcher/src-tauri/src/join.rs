//! Join by link (docs/online-protocol.md join codes; finding 128).
//!
//! A room is shared as `https://<host>/join/<CODE>` — or the launcher's own `dhd://join/<CODE>`
//! scheme. The launcher is started with a link or a code (from the registered URL scheme on
//! Windows, or on its command line) and passes the code to the game in `DHD_NET_ROOM`, which the
//! ONLINE page joins with (port/hle/online_menu.c). The rule is the game's (port/net/join.c):
//! six characters from the server's alphabet, which leaves out `0 O 1 I L`.

/// The server's join alphabet (docs/online-protocol.md).
const ALPHABET: &[u8] = b"23456789ABCDEFGHJKMNPQRSTUVWXYZ";
pub const LENGTH: usize = 6;

/// The scheme a `dhd://join/<CODE>` link uses on Windows (registered for this user).
#[cfg_attr(not(windows), allow(dead_code))]
pub const SCHEME: &str = "dhd";

fn alphabet(c: u8) -> bool {
    ALPHABET.contains(&c)
}

fn upper(c: u8) -> u8 {
    if c.is_ascii_lowercase() {
        c - 32
    } else {
        c
    }
}

/// 1 when `code` is exactly a join code (the case does not matter).
pub fn code_valid(code: &str) -> bool {
    let b = code.as_bytes();
    b.len() == LENGTH && b.iter().all(|&c| alphabet(upper(c)))
}

/// Six alphabet bytes at the start of `b`, ended: a seventh alphabet byte means a longer word, not
/// a code (`K7QX2MZ` is no code, `K7QX2M/` is).
fn code_at(b: &[u8]) -> Option<String> {
    if b.len() < LENGTH || !b[..LENGTH].iter().all(|&c| alphabet(c)) {
        return None;
    }
    if b.len() > LENGTH && alphabet(b[LENGTH]) {
        return None;
    }
    Some(String::from_utf8_lossy(&b[..LENGTH]).into_owned())
}

/// The join code a code, a link, or any text naming one holds, upper case: the code in
/// `.../join/<CODE>`, after the last path separator or colon (`dhd://K7QX2M`, `dhd:K7QX2M`), or a
/// lone six-character token somewhere in the text. `None` when there is none.
pub fn code_from_text(text: &str) -> Option<String> {
    let up: String = text.chars().map(|c| c.to_ascii_uppercase()).collect();
    let t = up.trim();
    if t.is_empty() {
        return None;
    }
    if code_valid(t) {
        return Some(t.to_string());
    }
    let b = t.as_bytes();
    for needle in ["JOIN/", "JOIN:"] {
        if let Some(i) = t.find(needle) {
            if let Some(code) = code_at(&b[i + needle.len()..]) {
                return Some(code);
            }
        }
    }
    if let Some(i) = t.rfind('/') {
        if let Some(code) = code_at(&b[i + 1..]) {
            return Some(code);
        }
    }
    if let Some(i) = t.rfind(':') {
        if let Some(code) = code_at(&b[i + 1..]) {
            return Some(code);
        }
    }
    let mut i = 0;
    while i < b.len() {
        if !alphabet(b[i]) {
            i += 1;
            continue;
        }
        let mut len = 0;
        while i + len < b.len() && alphabet(b[i + len]) {
            len += 1;
        }
        if len == LENGTH {
            return Some(String::from_utf8_lossy(&b[i..i + LENGTH]).into_owned());
        }
        i += len;
    }
    None
}

/// The shareable link for a room, from a server base URL (`wss://` -> `https://`, `ws://` ->
/// `http://`; anything without a scheme gets `https://`). `None` when the code is not one or the
/// base names no host. (The game builds the link it shows and copies; this is here for the rule
/// to live in one place and for the launcher's own use/tests.)
#[allow(dead_code)]
pub fn link(base: &str, code: &str) -> Option<String> {
    if !code_valid(code) {
        return None;
    }
    let code = code.to_ascii_uppercase();
    let lower = base.to_ascii_lowercase();
    let (scheme, host) = if lower.starts_with("wss://") {
        ("https://", &base[6..])
    } else if lower.starts_with("https://") {
        ("https://", &base[8..])
    } else if lower.starts_with("ws://") {
        ("http://", &base[5..])
    } else if lower.starts_with("http://") {
        ("http://", &base[7..])
    } else {
        ("https://", base)
    };
    let host = host.trim_end_matches('/');
    if host.is_empty() {
        return None;
    }
    Some(format!("{scheme}{host}/join/{code}"))
}

/// The join code in a process's arguments: a link or a code, after the program's own path.
/// `--join CODE`, `--join=CODE` and a bare link all work. `None` when the arguments name none.
pub fn code_from_args<I: IntoIterator<Item = String>>(args: I) -> Option<String> {
    let mut it = args.into_iter();
    let _program = it.next();
    let rest: Vec<String> = it.collect();
    let mut i = 0;
    while i < rest.len() {
        let arg = &rest[i];
        if arg == "--join" {
            if let Some(next) = rest.get(i + 1) {
                if let Some(code) = code_from_text(next) {
                    return Some(code);
                }
            }
            i += 1;
        } else if let Some(value) = arg.strip_prefix("--join=") {
            if let Some(code) = code_from_text(value) {
                return Some(code);
            }
        } else if let Some(code) = code_from_text(arg) {
            return Some(code);
        }
        i += 1;
    }
    None
}

/// Register the `dhd` URL scheme for this user, pointing at this launcher, so a `dhd://join/<CODE>`
/// link opens it (Windows only; macOS and Linux need the packaging to declare the scheme — an
/// `Info.plist` CFBundleURLTypes or a `.desktop` `x-scheme-handler`). Best effort: a failure leaves
/// the registry as it was and returns false. `DHD_NO_SCHEME=1` skips it.
#[cfg(windows)]
pub fn register_scheme(exe: &std::path::Path) -> bool {
    fn reg_add(path: &str, extra: &[&str]) -> bool {
        let mut cmd = std::process::Command::new("reg");
        cmd.args(["add", path, "/f"]).args(extra);
        cmd.stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null());
        cmd.status().map(|s| s.success()).unwrap_or(false)
    }
    if std::env::var_os("DHD_NO_SCHEME").is_some() {
        return false;
    }
    let exe = exe.display().to_string();
    let icon = format!("\"{exe}\",0");
    let command = format!("\"{exe}\" \"%1\"");
    let key = format!(r"HKCU\Software\Classes\{SCHEME}");
    reg_add(&key, &[])
        && reg_add(&key, &["/v", "URL Protocol", "/t", "REG_SZ", "/d", ""])
        && reg_add(&format!(r"{key}\DefaultIcon"), &["/ve", "/t", "REG_SZ", "/d", &icon])
        && reg_add(&format!(r"{key}\shell\open\command"), &["/ve", "/t", "REG_SZ", "/d", &command])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn valid_codes() {
        assert!(code_valid("K7QX2M"));
        assert!(code_valid("k7qx2m"));
        assert!(!code_valid("K7QX2"));
        assert!(!code_valid("K7QX2MZ"));
        assert!(!code_valid("K7QX2O")); // O is not in the alphabet
        assert!(!code_valid(""));
    }

    #[test]
    fn code_from_a_code() {
        assert_eq!(code_from_text("K7QX2M").as_deref(), Some("K7QX2M"));
        assert_eq!(code_from_text("k7qx2m").as_deref(), Some("K7QX2M"));
        assert_eq!(code_from_text("  K7QX2M\n").as_deref(), Some("K7QX2M"));
        assert_eq!(code_from_text("K7QX2M?s=1").as_deref(), Some("K7QX2M"));
        assert_eq!(code_from_text("Join code K7QX2M").as_deref(), Some("K7QX2M"));
    }

    #[test]
    fn code_from_a_link() {
        for text in [
            "https://dhd.skuza.app/join/K7QX2M",
            "https://dhd.skuza.app/join/k7qx2m",
            "https://dhd.skuza.app/join/k7qx2m?x=1#top",
            "https://dhd.skuza.app/K7QX2M",
            "http://127.0.0.1:8787/join/K7QX2M",
            "dhd://join/K7QX2M",
            "dhd:join/K7QX2M",
            "dhd://K7QX2M",
            "dhd:K7QX2M",
        ] {
            assert_eq!(code_from_text(text).as_deref(), Some("K7QX2M"), "{text}");
        }
    }

    #[test]
    fn rejects_rubbish() {
        for text in [
            "",
            "https://dhd.skuza.app/join/",
            "https://dhd.skuza.app/",
            "K7QX2",
            "K7QX2MZ",
            "0O1IL2",
            "hello world",
        ] {
            assert_eq!(code_from_text(text), None, "{text}");
        }
    }

    #[test]
    fn builds_links() {
        assert_eq!(link("wss://dhd.skuza.app", "K7QX2M").as_deref(), Some("https://dhd.skuza.app/join/K7QX2M"));
        assert_eq!(link("WSS://dhd.skuza.app", "k7qx2m").as_deref(), Some("https://dhd.skuza.app/join/K7QX2M"));
        assert_eq!(link("ws://127.0.0.1:8787", "K7QX2M").as_deref(), Some("http://127.0.0.1:8787/join/K7QX2M"));
        assert_eq!(link("wss://host/", "K7QX2M").as_deref(), Some("https://host/join/K7QX2M"));
        assert_eq!(link("", "K7QX2M"), None);
        assert_eq!(link("wss://host", "bad"), None);
        // a link it builds, it parses
        let l = link("wss://dhd.skuza.app", "K7QX2M").unwrap();
        assert_eq!(code_from_text(&l).as_deref(), Some("K7QX2M"));
    }

    #[test]
    fn code_from_command_line() {
        let args = |v: &[&str]| v.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(code_from_args(args(&["DownhillRevived.exe", "dhd://join/K7QX2M"])).as_deref(), Some("K7QX2M"));
        assert_eq!(code_from_args(args(&["DownhillRevived.exe", "--join", "K7QX2M"])).as_deref(), Some("K7QX2M"));
        assert_eq!(code_from_args(args(&["DownhillRevived.exe", "--join=K7QX2M"])).as_deref(), Some("K7QX2M"));
        assert_eq!(
            code_from_args(args(&["DownhillRevived.exe", "https://dhd.skuza.app/join/k7qx2m"])).as_deref(),
            Some("K7QX2M")
        );
        assert_eq!(code_from_args(args(&["DownhillRevived.exe"])), None);
        assert_eq!(code_from_args(args(&["DownhillRevived.exe", "--verbose"])), None);
    }
}
