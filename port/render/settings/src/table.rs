//! Every dhd.ini setting: its key, the variable the game reads it from, the player's default, the
//! game's own value without one, and the choices the menus offer with their labels. The menus
//! (the game's PC SETTINGS page, the launcher's tabs) decide which rows they show and how; the
//! meaning is here.

/// One value a setting may take, and the word the menus show for it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Choice {
    pub value: &'static str,
    pub label: &'static str,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    /// Free text: a path, a name, an address, a number.
    Text,
    /// One of `choices`.
    Choice,
    /// Two choices, off then on: the words 0/off/false/no and 1/on/true/yes stand for them too.
    Switch,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Setting {
    /// The dhd.ini key (keys compare without case).
    pub key: &'static str,
    /// The DHD_* variable the game reads; its name is also a dhd.ini spelling of the key. Empty: the
    /// game takes no variable from it (the launcher's own, game_options, the window size's pair).
    pub env: &'static str,
    pub kind: Kind,
    /// The player's default, where dhd.ini has no value for the key ("" none). The game applies it
    /// only when dhd.ini exists (the tools run without one and keep the built-in values), unless
    /// `always`.
    pub default: &'static str,
    /// What the game does when nothing sets the variable (the tools' runs): the PC SETTINGS page
    /// shows it then.
    pub builtin: &'static str,
    pub choices: &'static [Choice],
    /// The choice any other value stands for (as the game reads it), or "" when another value is
    /// passed on as written (a shape for widescreen, a scale over 8).
    pub other: &'static str,
    /// The default applies without dhd.ini too.
    pub always: bool,
}

/// The official public race server (online/server): `server` when dhd.ini and DHD_NET_SERVER don't
/// name another.
pub const OFFICIAL_SERVER: &str = "wss://dhd.skuza.app";

pub const OFF_WORDS: [&str; 4] = ["0", "off", "false", "no"];
pub const ON_WORDS: [&str; 4] = ["1", "on", "true", "yes"];

const fn c(value: &'static str, label: &'static str) -> Choice {
    Choice { value, label }
}

const SWITCH: &[Choice] = &[c("0", "OFF"), c("1", "ON")];
const TOGGLE: &[Choice] = &[c("off", "OFF"), c("on", "ON")];

const fn text(key: &'static str, env: &'static str) -> Setting {
    Setting { key, env, kind: Kind::Text, default: "", builtin: "", choices: &[], other: "", always: false }
}

const fn switch(key: &'static str, env: &'static str, choices: &'static [Choice], default: &'static str, builtin: &'static str) -> Setting {
    // any other value is on, as the game reads it (atoi != 0; textures and discord: not an off word)
    Setting { key, env, kind: Kind::Switch, default, builtin, choices, other: choices[1].value, always: false }
}

const fn choice(key: &'static str, env: &'static str, choices: &'static [Choice], default: &'static str, builtin: &'static str) -> Setting {
    Setting { key, env, kind: Kind::Choice, default, builtin, choices, other: "", always: false }
}

/// The settings, in the order a new dhd.ini would list them.
pub const SETTINGS: &[Setting] = &[
    text("iso", "DHD_ISO"),
    // The 3D picture's resolution: 1-8 times the PS2's, or the window's (DHD_SCALE_MAX may cap it).
    choice("scale", "DHD_SCALE", &[c("1", "1x"), c("2", "2x"), c("3", "3x"), c("4", "4x"), c("5", "5x"), c("6", "6x"),
                                   c("7", "7x"), c("8", "8x"), c("window", "Match window")], "2", "2"),
    choice("window", "DHD_WINDOW", &[c("windowed", "Window"), c("borderless", "Borderless"), c("fullscreen", "Fullscreen")],
           "borderless", "windowed"),
    // The windowed display's size: width and height together (monitor: the desktop's resolution,
    // which present.c puts in their place). Saving "size" writes both lines.
    choice("size", "", &[c("monitor", "Monitor size"), c("896x672", "896x672"), c("1280x960", "1280x960"),
                         c("1440x1080", "1440x1080"), c("1280x720", "1280x720"), c("1920x1080", "1920x1080"),
                         c("2560x1440", "2560x1440")], "monitor", "896x672"),
    Setting { default: "monitor", builtin: "896", ..text("width", "DHD_WINDOW_WIDTH") },
    Setting { default: "monitor", builtin: "672", ..text("height", "DHD_WINDOW_HEIGHT") },
    text("monitor", "DHD_MONITOR"),
    // 1: the monitor's shape (a wider view, the HUD and menus kept in shape); also a shape (21:9).
    Setting { other: "", ..switch("widescreen", "DHD_WIDESCREEN", SWITCH, "1", "0") },
    // The picture's shape: widescreen's (wide), 4:3, 16:9, stretched or whole pixels.
    choice("DHD_ASPECT", "DHD_ASPECT", &[c("4:3", "4:3"), c("wide", "Wide"), c("16:9", "16:9"), c("stretch", "Stretch"),
                                         c("pixels", "Pixels")], "wide", "4:3"),
    choice("DHD_PRESENT_FILTER", "DHD_PRESENT_FILTER", &[c("linear", "Smooth"), c("point", "Sharp")], "linear", "linear"),
    Setting { other: "exact", ..choice("renderer", "DHD_RENDER", &[c("native", "Native"), c("exact", "Exact")], "native", "exact") },
    switch("skip_movies", "DHD_SKIP_MOVIES", SWITCH, "1", "0"),
    switch("vibration", "DHD_VIBRATION", SWITCH, "1", "1"),
    // The sticks' dead zone (finding 134, port/hle/stick.h): game = the game's own (about 36%, as the
    // PS2 always had it); a percentage moves where steering, leaning and the right stick start, with
    // the whole travel past it used. Another number (dhd.ini) is a percentage too.
    choice("stick_dead_zone", "DHD_STICK_DEAD_ZONE", &[c("game", "Game (36%)"), c("5", "5%"), c("10", "10%"), c("15", "15%"),
                                                       c("20", "20%"), c("25", "25%"), c("30", "30%"), c("40", "40%"),
                                                       c("50", "50%")], "game", "game"),
    // The button symbols the game shows (finding 134, port/hle/prompts.c): auto = the connected
    // controller's kind (player 1's; the PS2's own without one). The tools' runs keep the PS2's.
    choice("prompts", "DHD_PROMPTS", &[c("auto", "Auto"), c("playstation", "PlayStation"), c("xbox", "Xbox"),
                                       c("nintendo", "Nintendo")], "auto", "playstation"),
    // The keys and controller buttons for the PS2 pad's buttons (finding 157, controls.rs): the
    // keyboard's layout and each player's controller's; empty, the game's own layouts.
    text("keyboard", "DHD_KEYBOARD"),
    text("pad1", "DHD_PAD1"),
    text("pad2", "DHD_PAD2"),
    // A PCSX2 texture pack: texture_pack its folder (empty: textures/SCUS-97177/replacements in the
    // folder of dhd.ini and the saves, else beside the game), textures = off the game's own textures.
    switch("textures", "DHD_TEXTURES", TOGGLE, "on", "on"),
    text("texture_pack", "DHD_TEXTURE_PACK"),
    text("texture_cache_mb", "DHD_TEXTURE_CACHE_MB"),
    // sharp: the game's font with clean edges at any resolution; original: as the PS2 draws it.
    switch("font", "DHD_FONT", &[c("original", "Original"), c("sharp", "Sharp")], "sharp", "sharp"),
    // Simulation steps a second (findings 067, 114), offline and online.
    choice("fps", "DHD_FPS", &[c("60", "60 fps"), c("120", "120 fps"), c("144", "144 fps"), c("165", "165 fps"),
                               c("240", "240 fps"), c("monitor", "Monitor")], "60", "60"),
    switch("vsync", "DHD_VSYNC", SWITCH, "1", "0"),
    switch("perf_counter", "DHD_PERF_COUNTER", SWITCH, "0", "0"),
    switch("unlock_all", "DHD_UNLOCK_ALL", SWITCH, "0", "0"),
    // The touch controls on phones (finding 141, port/hle/touch.c; the TOUCH CONTROLS pages): the
    // preset, the hand (left mirrors the controls and the HUD), stick or tilt steering, the size (auto:
    // the screen height's rule, 100% at most) and the opacity, the steering stick (floating or fixed)
    // and its range, the trick stick's size, the trick labels, the attacks (flick off PEDAL or
    // buttons), vibration, whether a controller hides the overlay, tilt's strength and centre (degrees),
    // the hints already shown (a number) and the layout editor's changes.
    choice("touch_preset", "DHD_TOUCH_PRESET", &[c("thumb", "Thumb zones"), c("classic", "Classic pad")], "classic", "classic"),  // the full pad by default
    choice("touch_hand", "DHD_TOUCH_HAND", &[c("right", "Right"), c("left", "Left")], "right", "right"),
    choice("touch_steer", "DHD_TOUCH_STEER", &[c("stick", "Stick"), c("tilt", "Tilt")], "stick", "stick"),
    choice("touch_size", "DHD_TOUCH_SIZE", &[c("auto", "Auto"), c("70", "70%"), c("75", "75%"), c("80", "80%"), c("85", "85%"),
                                             c("90", "90%"), c("95", "95%"), c("100", "100%"), c("105", "105%")], "auto", "auto"),
    choice("touch_opacity", "DHD_TOUCH_OPACITY", &[c("25", "25%"), c("30", "30%"), c("35", "35%"), c("40", "40%"), c("45", "45%"),
                                                   c("50", "50%"), c("55", "55%"), c("60", "60%"), c("65", "65%"), c("70", "70%"),
                                                   c("75", "75%"), c("80", "80%"), c("85", "85%"), c("90", "90%"), c("95", "95%"),
                                                   c("100", "100%")], "60", "60"),
    choice("touch_stick", "DHD_TOUCH_STICK", &[c("floating", "Floating"), c("fixed", "Fixed")], "floating", "floating"),
    choice("touch_stick_range", "DHD_TOUCH_STICK_RANGE", &[c("8", "8 mm"), c("10", "10 mm"), c("12", "12 mm"), c("14", "14 mm"),
                                                           c("16", "16 mm"), c("18", "18 mm"), c("20", "20 mm")], "12", "12"),
    choice("touch_trick_stick", "DHD_TOUCH_TRICK_STICK", &[c("25", "25 mm"), c("28", "28 mm"), c("31", "31 mm"), c("34", "34 mm"),
                                                           c("37", "37 mm")], "31", "31"),
    choice("touch_labels", "DHD_TOUCH_LABELS", &[c("auto", "Auto"), c("always", "Always"), c("off", "Off")], "auto", "auto"),
    choice("touch_attacks", "DHD_TOUCH_ATTACKS", &[c("flick", "Flick"), c("buttons", "Buttons")], "flick", "flick"),
    switch("touch_vibration", "DHD_TOUCH_VIBRATION", &[c("0", "Off"), c("1", "On")], "1", "1"),
    choice("touch_with_pad", "DHD_TOUCH_WITH_PAD", &[c("hide", "Hide"), c("show", "Show")], "hide", "hide"),
    choice("touch_tilt_strength", "DHD_TOUCH_TILT_STRENGTH", &[c("0", "0%"), c("10", "10%"), c("20", "20%"), c("30", "30%"),
                                                               c("40", "40%"), c("50", "50%"), c("60", "60%"), c("70", "70%"),
                                                               c("80", "80%"), c("90", "90%"), c("100", "100%")], "50", "50"),
    text("touch_tilt_centre", "DHD_TOUCH_TILT_CENTRE"),
    text("touch_hints", "DHD_TOUCH_HINTS"),
    text("touch_edits", "DHD_TOUCH_EDITS"),
    text("memory_card", "DHD_MEMCARD"),
    text("sound_latency_ms", "DHD_SND_LATENCY_MS"),
    Setting { default: OFFICIAL_SERVER, builtin: OFFICIAL_SERVER, always: true, ..text("server", "DHD_NET_SERVER") },
    text("name", "DHD_NET_NAME"),
    choice("names", "DHD_NAMES", &[c("off", "Off"), c("small", "Small"), c("medium", "Medium"), c("large", "Large")], "small", "small"),
    text("rider", "DHD_NET_RIDER"),
    // The launcher's: whether it looks for a newer release, and where (empty: the official releases).
    choice("auto_update", "", &[c("ask", "Ask"), c("on", "Automatic"), c("off", "Off")], "ask", "ask"),
    text("update_url", "DHD_UPDATE_URL"),
    // Discord Rich Presence (port/render/presence): the game shows what is played on the player's
    // Discord profile. Off without dhd.ini, so the tools' runs never reach the PC's Discord.
    switch("discord", "DHD_DISCORD", TOGGLE, "on", "off"),
    // The game's own options (finding 064), read after its boot and each game-data load.
    text("game_options", ""),
];

/// The setting a dhd.ini key names: its key or its variable's name, without case.
pub fn find(key: &str) -> Option<&'static Setting> {
    SETTINGS
        .iter()
        .find(|s| s.key.eq_ignore_ascii_case(key) || (!s.env.is_empty() && s.env.eq_ignore_ascii_case(key)))
}

impl Setting {
    /// The choice `value` is, as the game reads it: a choice's own value, an off or on word for a
    /// switch, else `other`. None: a value outside the choices, passed on as written.
    pub fn canonical(&self, value: &str) -> Option<&'static str> {
        let v = value.trim();
        if let Some(ch) = self.choices.iter().find(|ch| ch.value.eq_ignore_ascii_case(v)) {
            return Some(ch.value);
        }
        if self.kind == Kind::Switch {
            if OFF_WORDS.iter().any(|w| w.eq_ignore_ascii_case(v)) {
                return Some(self.choices[0].value);
            }
            if ON_WORDS.iter().any(|w| w.eq_ignore_ascii_case(v)) {
                return Some(self.choices[1].value);
            }
        }
        (!self.other.is_empty() && !v.is_empty()).then_some(self.other)
    }

    /// The label the menus show for `value`, if it is one of the choices.
    pub fn label(&self, value: &str) -> Option<&'static str> {
        let v = self.canonical(value)?;
        self.choices.iter().find(|ch| ch.value == v).map(|ch| ch.label)
    }
}

/// The C table the game's PC SETTINGS page (port/hle/pc_settings.c, inside the game core, which has
/// no Rust) reads: the settings with choices. tests/generated.rs keeps port/hle/settings_table.h
/// equal to it.
pub fn c_table() -> String {
    let mut out = String::from(
        "/* The settings' choices for the game's PC SETTINGS page (pc_settings.c): generated from the one\n   \
         table of dhd.ini's settings, port/render/settings/src/table.rs (dhd_settings), whose test\n   \
         keeps this file equal to it (DHD_SETTINGS_BLESS=1 cargo test -p dhd_settings writes it).\n   \
         Don't edit it here. */\n\
         #ifndef DHD_SETTINGS_TABLE_H\n#define DHD_SETTINGS_TABLE_H\n\n\
         typedef struct { const char *value, *label; } SettingChoice;\n\
         /* key: the dhd.ini key; env: the variable (\"\" none); builtin: the value without one. */\n\
         typedef struct { const char *key, *env, *builtin; const SettingChoice *choices; int count; } SettingInfo;\n\n",
    );
    let with_choices = || SETTINGS.iter().filter(|s| !s.choices.is_empty());
    for s in with_choices() {
        let list: Vec<String> = s.choices.iter().map(|ch| format!("{{\"{}\", \"{}\"}}", ch.value, ch.label)).collect();
        out += &format!("static const SettingChoice setting_choices_{}[] = {{{}}};\n", s.key.to_ascii_lowercase(), list.join(", "));
    }
    out += "\nstatic const SettingInfo setting_table[] = {\n";
    for s in with_choices() {
        out += &format!(
            "    {{\"{}\", \"{}\", \"{}\", setting_choices_{}, {}}},\n",
            s.key,
            s.env,
            s.builtin,
            s.key.to_ascii_lowercase(),
            s.choices.len()
        );
    }
    out += "};\n\n";
    // the rider name's rule (crate::name), for the online menu (port/hle/online_menu.c)
    out += &format!(
        "/* The rider name (\"name\", DHD_NET_NAME): up to SETTING_NAME_MAX of SETTING_NAME_LETTERS, upper\n   \
         case; SETTING_NAME_UNNAMED while none is set. {} */\n\
         #define SETTING_NAME_LETTERS \"{}\"\n#define SETTING_NAME_MAX {}\n#define SETTING_NAME_UNNAMED \"{}\"\n\n#endif\n",
        crate::name::RULE,
        crate::name::LETTERS,
        crate::name::MAX,
        crate::name::UNNAMED
    );
    out
}
