//! The dhd.ini corpus: every rule of the format through the crate (the game's start, the menus'
//! values, saves), on the cases the three old readers (host.c, the launcher's ini.rs, the
//! browser's settings.js) took differently.

use dhd_settings::{environment, Ini, OFFICIAL_SERVER};
use std::collections::BTreeMap;
use std::fs;

fn env_of(text: &str, set: &[&str]) -> BTreeMap<String, String> {
    let ini = Ini::parse(text.as_bytes());
    environment(Some(&ini), |n| set.iter().any(|s| s.eq_ignore_ascii_case(n))).vars.into_iter().collect()
}

fn saved(text: &[u8], key: &str, value: &str) -> Vec<u8> {
    let mut ini = Ini::parse(text);
    ini.set(key, value).unwrap();
    ini.bytes().to_vec()
}

#[test]
fn empty_and_duplicate_keys() {
    // The case the launcher took apart from the game: an empty first line, then a value.
    let text = "scale =\nscale = 4\n";
    let ini = Ini::parse(text.as_bytes());
    assert_eq!(ini.written("scale").as_deref(), Some("4"));
    assert_eq!(ini.value("scale"), "4");
    assert_eq!(env_of(text, &[])["DHD_SCALE"], "4");
    // The first value counts.
    let ini = Ini::parse(b"scale = 3\nscale = 4\n");
    assert_eq!(ini.value("scale"), "3");
    // Only empty lines: no value, the default.
    let ini = Ini::parse(b"scale =\nscale =   \n");
    assert_eq!(ini.written("scale"), None);
    assert_eq!(ini.value("scale"), "2");
    assert_eq!(env_of("scale =\n", &[])["DHD_SCALE"], "2");
    // A key's variable name is another spelling of it, in either order.
    assert_eq!(Ini::parse(b"DHD_SCALE = 5\nscale = 3\n").value("scale"), "5");
    assert_eq!(Ini::parse(b"Scale = 6\ndhd_scale = 3\n").value("SCALE"), "6");
    assert_eq!(env_of("dhd_scale = 5\nscale = 3\n", &[])["DHD_SCALE"], "5");
}

#[test]
fn comments_sections_and_quotes() {
    let text = "; a comment = no setting\n# another = none\n[display]\nscale = 3 ; three\nwindow = windowed# not a comment\n\
                server = ; none\nname = A#B\niso = \"D:\\Games\\a;b #1.iso\" ; the disc\ntexture_pack = \"unclosed\n  fps\t=\t120\t\n";
    let ini = Ini::parse(text.as_bytes());
    assert_eq!(ini.value("scale"), "3");
    assert_eq!(ini.written("window").as_deref(), Some("windowed# not a comment"));
    assert_eq!(ini.written("server"), None);
    assert_eq!(ini.value("server"), OFFICIAL_SERVER);
    assert_eq!(ini.written("name").as_deref(), Some("A#B"));
    assert_eq!(ini.written("iso").as_deref(), Some("D:\\Games\\a;b #1.iso"));
    assert_eq!(ini.written("texture_pack").as_deref(), Some("\"unclosed"));
    assert_eq!(ini.value("fps"), "120");
    assert_eq!(ini.written("a comment"), None);
    assert!(ini.unknown_keys().is_empty());
}

#[test]
fn values_as_the_game_reads_them() {
    let text = "vibration = off\ntextures = 0\nfont = OFF\nrenderer = whatever\nskip_movies = yes\nvsync = 2\n\
                widescreen = 21:9\nwindow = Borderless\nunlock_all = false\ndiscord = no\n";
    let ini = Ini::parse(text.as_bytes());
    for (key, want) in [("vibration", "0"), ("textures", "off"), ("font", "original"), ("renderer", "exact"), ("skip_movies", "1"),
                        ("vsync", "1"), ("widescreen", "21:9"), ("window", "borderless"), ("unlock_all", "0"), ("discord", "off")] {
        assert_eq!(ini.value(key), want, "{key}");
    }
    let env = env_of(text, &[]);
    for (name, want) in [("DHD_VIBRATION", "0"), ("DHD_TEXTURES", "off"), ("DHD_FONT", "original"), ("DHD_RENDER", "exact"),
                         ("DHD_SKIP_MOVIES", "1"), ("DHD_WIDESCREEN", "21:9"), ("DHD_WINDOW", "borderless"), ("DHD_DISCORD", "off")] {
        assert_eq!(env[name], want, "{name}");
    }
    assert_eq!(dhd_settings::find("font").unwrap().label("0"), Some("Original"));
    assert_eq!(dhd_settings::find("DHD_ASPECT").unwrap().label("WIDE"), Some("Wide"));
}

#[test]
fn environment_overrides_and_defaults() {
    // With a dhd.ini: its values, else the player's defaults; a variable already set wins and is
    // listed as an override.
    let env = env_of("scale = 4\nfps = 144\n", &["DHD_FPS", "DHD_WINDOW"]);
    assert_eq!(env["DHD_SCALE"], "4");
    assert!(!env.contains_key("DHD_FPS") && !env.contains_key("DHD_WINDOW"));
    assert_eq!(env["DHD_ENV_OVERRIDES"], ",DHD_WINDOW,DHD_FPS,");
    for (name, want) in [("DHD_RENDER", "native"), ("DHD_WIDESCREEN", "1"), ("DHD_ASPECT", "wide"), ("DHD_SKIP_MOVIES", "1"),
                         ("DHD_VSYNC", "1"), ("DHD_WINDOW_WIDTH", "monitor"), ("DHD_WINDOW_HEIGHT", "monitor"),
                         ("DHD_NET_SERVER", OFFICIAL_SERVER), ("DHD_DISCORD", "on")] {
        assert_eq!(env[name], want, "{name}");
    }
    assert!(!env.contains_key("DHD_ISO") && !env.contains_key("DHD_NET_NAME"));
    // Without dhd.ini (the tools): nothing but the official server and the override list.
    let env: BTreeMap<_, _> = environment(None, |_| false).vars.into_iter().collect();
    assert_eq!(env.len(), 2);
    assert_eq!(env["DHD_NET_SERVER"], OFFICIAL_SERVER);
    assert_eq!(env["DHD_ENV_OVERRIDES"], ",");
    // The window size's default is the pair's: one side named keeps the other unset.
    let env = env_of("width = 1280\n", &[]);
    assert_eq!(env["DHD_WINDOW_WIDTH"], "1280");
    assert!(!env.contains_key("DHD_WINDOW_HEIGHT"));
    let env = env_of("", &["DHD_WINDOW_HEIGHT"]);
    assert!(!env.contains_key("DHD_WINDOW_WIDTH"));
    // A server line names another server; an empty one is the official.
    assert_eq!(env_of("server = wss://races.example.org\n", &[])["DHD_NET_SERVER"], "wss://races.example.org");
    assert_eq!(env_of("server =\n", &[])["DHD_NET_SERVER"], OFFICIAL_SERVER);
    // effective: the environment over the file.
    let ini = Ini::parse(b"update_url = https://a.example/latest.json\nscale = 3\n");
    let var = |n: &str| (n == "DHD_UPDATE_URL").then(|| "https://b.example/latest.json".to_string());
    assert_eq!(ini.effective("update_url", var), "https://b.example/latest.json");
    assert_eq!(ini.effective("scale", var), "3");
}

#[test]
fn unknown_lines() {
    let text = "[x]\nno equals sign here\nfoo = bar\nDHD_SOMETHING = 7\nDHD_EMPTY =\n= no key\ngame_options = 1 2 3\n";
    let ini = Ini::parse(text.as_bytes());
    assert_eq!(ini.unknown_keys(), vec!["foo".to_string()]);
    let env = environment(Some(&ini), |_| false);
    assert_eq!(env.unknown, vec!["foo".to_string()]);
    let vars: BTreeMap<_, _> = env.vars.into_iter().collect();
    assert_eq!(vars["DHD_SOMETHING"], "7");
    assert!(!vars.contains_key("DHD_EMPTY") && !vars.contains_key("foo"));
    assert_eq!(ini.written("game_options").as_deref(), Some("1 2 3"));
    // A save keeps all of them.
    let after = saved(text.as_bytes(), "scale", "3");
    assert_eq!(after, format!("{text}scale = 3\n").as_bytes());
}

#[test]
fn saves_keep_every_other_line() {
    // CRLF, a byte order mark, text that isn't UTF-8, no final line ending: all kept.
    let mut text = b"\xEF\xBB\xBF; Downhill Revived\r\n; caf\xE9 (CP1252)\r\n[video]\r\nScale = 2 ; two\r\n\r\n".to_vec();
    text.extend_from_slice(b"iso = \"D:\\a;b.iso\"\r\nfoo = bar\r\nfont = sharp");
    let after = saved(&text, "scale", "4");
    let mut want = text.clone();
    let at = want.windows(15).position(|w| w == b"Scale = 2 ; two").unwrap();
    want.splice(at..at + 15, b"scale = 4".iter().copied());
    assert_eq!(after, want);
    // a new key at the end, with the file's line ending; the missing last one is added first
    let after = saved(&text, "name", "SKUZA");
    let mut want = text.clone();
    want.extend_from_slice(b"\r\nname = SKUZA\r\n");
    assert_eq!(after, want);
    // the last line replaced: it gets the file's line ending
    let after = saved(&text, "font", "original");
    assert!(after.ends_with(b"foo = bar\r\nfont = original\r\n"));
    // the first line, after the byte order mark
    let after = saved(b"\xEF\xBB\xBFscale = 2\nfps = 60\n", "scale", "3");
    assert_eq!(after, b"\xEF\xBB\xBFscale = 3\nfps = 60\n");
    // the setting's other lines (duplicates, its DHD_* spelling) go; the first keeps its place
    let after = saved(b"a = 1\nDHD_SCALE = 5\nb = 2\nscale =\nscale = 3\nc = 3\n", "scale", "6");
    assert_eq!(after, b"a = 1\nscale = 6\nb = 2\nc = 3\n");
    let ini = Ini::parse(&after);
    assert_eq!(ini.value("scale"), "6");
    // quotes where the value needs them; an empty value
    let after = saved(b"", "iso", "D:\\x;y.iso");
    assert_eq!(after, b"iso = \"D:\\x;y.iso\"\n");
    assert_eq!(Ini::parse(&after).written("iso").as_deref(), Some("D:\\x;y.iso"));
    assert_eq!(saved(b"server = wss://a\n", "server", ""), b"server =\n");
    // the window size: both lines
    let after = saved(b"width = 896\nheight = 672\n", "size", "1920x1080");
    assert_eq!(after, b"width = 1920\nheight = 1080\n");
    assert_eq!(Ini::parse(&after).value("size"), "1920x1080");
    assert_eq!(Ini::parse(b"").value("size"), "monitor");
    assert_eq!(Ini::parse(b"width = monitor\nheight = monitor\n").value("size"), "monitor");
    assert_eq!(Ini::parse(b"width = 1280\n").value("size"), "1280x672");
    // what no line can hold
    let mut ini = Ini::parse(b"");
    assert!(ini.set("a b", "1").is_err() && ini.set("key", "two\nlines").is_err() && ini.set("size", "big").is_err());
    // a round trip of the whole corpus: saving a value again changes nothing
    for text in [&b"scale = 4\r\n; x\r\n"[..], b"; only a comment", b"", b"[s]\nfps = 120\n"] {
        let once = saved(text, "fps", "144");
        assert_eq!(saved(&once, "fps", "144"), once);
    }
}

#[test]
fn files() {
    let dir = std::env::temp_dir().join(format!("dhd-settings-test-{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    let p = dir.join("dhd.ini");
    assert!(dhd_settings::read(&p).is_none());
    assert!(dhd_settings::save(&p, "scale", "3").unwrap());
    assert_eq!(fs::read_to_string(&p).unwrap(), format!("{}scale = 3\n", dhd_settings::HEADER));
    assert!(!dir.join("dhd.ini.new").exists());
    dhd_settings::create(&p).unwrap();
    assert_eq!(dhd_settings::read(&p).unwrap().value("scale"), "3");
    let fresh = dir.join("fresh.ini");
    dhd_settings::create(&fresh).unwrap();
    assert_eq!(fs::read_to_string(&fresh).unwrap(), dhd_settings::HEADER);
    // DHD_SETTINGS=none: nothing read or written
    for off in ["none", "NONE", "0"] {
        assert!(dhd_settings::read(std::path::Path::new(off)).is_none());
        assert!(!dhd_settings::save(std::path::Path::new(off), "scale", "3").unwrap());
    }
    fs::remove_dir_all(dir).unwrap();
}

#[test]
fn c_interface() {
    use std::ffi::{c_char, CStr, CString};
    use std::sync::Mutex;
    static SET: Mutex<Vec<(String, String)>> = Mutex::new(Vec::new());
    unsafe extern "C" fn get(name: *const c_char) -> *const c_char {
        let name = unsafe { CStr::from_ptr(name) }.to_str().unwrap();
        if name == "DHD_FPS" { c"240".as_ptr() } else { std::ptr::null() }
    }
    unsafe extern "C" fn set(name: *const c_char, value: *const c_char) {
        let (n, v) = unsafe { (CStr::from_ptr(name), CStr::from_ptr(value)) };
        SET.lock().unwrap().push((n.to_str().unwrap().into(), v.to_str().unwrap().into()));
    }
    let dir = std::env::temp_dir().join(format!("dhd-settings-c-{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    let p = dir.join("dhd.ini");
    fs::write(&p, "fps = 120\ngame_options = 1 800\n").unwrap();
    let path = CString::new(p.to_str().unwrap()).unwrap();
    unsafe {
        assert_eq!(dhd_settings::c::dhd_settings_load(path.as_ptr(), get, set, None), 1);
        assert_eq!(dhd_settings::c::dhd_settings_save(path.as_ptr(), c"scale".as_ptr(), c"5".as_ptr()), 1);
        let mut out = [0 as c_char; 8];
        assert_eq!(dhd_settings::c::dhd_settings_read(path.as_ptr(), c"game_options".as_ptr(), out.as_mut_ptr(), out.len()), 1);
        assert_eq!(CStr::from_ptr(out.as_ptr()).to_str().unwrap(), "1 800");
        let mut short = [0 as c_char; 3];
        dhd_settings::c::dhd_settings_read(path.as_ptr(), c"game_options".as_ptr(), short.as_mut_ptr(), short.len());
        assert_eq!(CStr::from_ptr(short.as_ptr()).to_str().unwrap(), "1 ");
        assert_eq!(dhd_settings::c::dhd_settings_read(path.as_ptr(), c"iso".as_ptr(), out.as_mut_ptr(), out.len()), 0);
    }
    let set: BTreeMap<_, _> = SET.lock().unwrap().iter().cloned().collect();
    assert!(!set.contains_key("DHD_FPS"));
    assert_eq!(set["DHD_ENV_OVERRIDES"], ",DHD_FPS,");
    assert_eq!(set["DHD_SCALE"], "2");
    assert_eq!(fs::read_to_string(&p).unwrap(), "fps = 120\ngame_options = 1 800\nscale = 5\n");
    fs::remove_dir_all(dir).unwrap();
}

#[test]
fn install_id() {
    use dhd_settings::install;
    let a = install::fresh();
    let b = install::fresh();
    assert!(install::valid(&a) && install::valid(&b), "{a} {b}");
    assert_ne!(a, b);
    assert!(!install::valid("0123456789ABCDEF0123456789abcdef") && !install::valid("abc"));
    let dir = std::env::temp_dir().join(format!("dhd-settings-install-{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    let p = dir.join("dhd.ini");
    // no file: an id for this run, and no dhd.ini made for it
    let (once, kept) = install::get(&p);
    assert!(install::valid(&once) && !kept && !p.exists());
    // a file: made once, kept after
    fs::write(&p, "fps = 120\ninstall_id = not-an-id\n").unwrap();
    let (id, kept) = install::get(&p);
    assert!(install::valid(&id) && kept);
    assert_eq!(fs::read_to_string(&p).unwrap(), format!("fps = 120\ninstall_id = {id}\n"));
    assert_eq!(install::get(&p), (id.clone(), true));
    // not a setting: no variable, not an unknown line, not in the launcher's table
    let ini = dhd_settings::Ini::parse(&fs::read(&p).unwrap());
    assert!(ini.unknown_keys().is_empty());
    let env = dhd_settings::environment(Some(&ini), |_| false);
    assert!(env.vars.iter().all(|(_, v)| v != &id));
    assert!(dhd_settings::find("install_id").is_none());
    // the C interface
    let path = std::ffi::CString::new(p.to_str().unwrap()).unwrap();
    let mut out = [0 as std::ffi::c_char; 33];
    unsafe {
        assert_eq!(dhd_settings::c::dhd_settings_install_id(path.as_ptr(), out.as_mut_ptr(), out.len()), 1);
        assert_eq!(std::ffi::CStr::from_ptr(out.as_ptr()).to_str().unwrap(), id);
        assert_eq!(dhd_settings::c::dhd_settings_install_id(path.as_ptr(), out.as_mut_ptr(), 32), 0);
    }
    fs::remove_dir_all(dir).unwrap();
}
