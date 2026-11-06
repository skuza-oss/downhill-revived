//! port/hle/settings_table.h is the crate's table for the game core's PC SETTINGS page, and
//! port/hle/controls_table.h its bindings' tables (finding 157). These tests fail while a file
//! differs; with DHD_SETTINGS_BLESS=1 they write it.

fn current(file: &str, want: String) {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../hle").join(file);
    if std::env::var_os("DHD_SETTINGS_BLESS").is_some() {
        std::fs::write(&path, &want).unwrap();
    }
    let have = std::fs::read_to_string(&path).unwrap_or_default().replace("\r\n", "\n");
    assert!(have == want, "port/hle/{file} is not the crate's: DHD_SETTINGS_BLESS=1 cargo test -p dhd_settings");
}

#[test]
fn settings_table_h_is_current() {
    current("settings_table.h", dhd_settings::table::c_table());
}

#[test]
fn controls_table_h_is_current() {
    current("controls_table.h", dhd_settings::controls::c_table());
}

#[test]
fn online_names_choices_and_defaults() {
    let setting = dhd_settings::table::find("names").unwrap();
    assert_eq!(setting.env, "DHD_NAMES");
    assert_eq!(setting.default, "small");
    assert_eq!(setting.builtin, "small");
    assert_eq!(setting.choices.iter().map(|c| c.value).collect::<Vec<_>>(), ["off", "small", "medium", "large"]);
    assert_eq!(setting.canonical("MEDIUM"), Some("medium"));
    assert_eq!(setting.canonical("broken"), None);
}
