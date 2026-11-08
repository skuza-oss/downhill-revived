//! The Controls tab's bindings (finding 157): the tables, the three layouts dhd.ini holds
//! (keyboard, pad1, pad2) and each change, made by dhd_settings::controls (the game's own rules)
//! and saved at once. The game takes them at its next start, as the launcher's other settings.

use dhd_settings::controls::{self, Controls, Edit, Outcome};
use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Serialize)]
pub struct Target {
    label: &'static str,
    race: &'static str,
}

#[derive(Serialize)]
pub struct Key {
    vk: u8,
    label: &'static str,
    /// The browser's KeyboardEvent.code for the key at that place.
    code: &'static str,
}

#[derive(Serialize)]
pub struct PadInput {
    id: u8,
    /// Xbox's, PlayStation's and Nintendo's words.
    labels: [&'static str; 3],
    /// The Gamepad API's standard-mapping button, -1 none.
    web: i8,
}

#[derive(Serialize)]
pub struct Device {
    label: &'static str,
}

#[derive(Serialize)]
pub struct Schema {
    targets: Vec<Target>,
    /// How many of the targets a controller binds (the rest are the keyboard's stick directions).
    buttons: usize,
    keys: Vec<Key>,
    pads: Vec<PadInput>,
    devices: Vec<Device>,
}

pub fn schema() -> Schema {
    Schema {
        targets: controls::TARGETS.iter().map(|t| Target { label: t.label, race: t.race }).collect(),
        buttons: controls::BUTTONS,
        keys: controls::KEYS.iter().map(|k| Key { vk: k.vk, label: k.label, code: k.code }).collect(),
        pads: controls::PAD_INPUTS
            .iter()
            .map(|p| PadInput { id: p.id, labels: [p.labels.xbox, p.labels.playstation, p.labels.nintendo], web: p.web })
            .collect(),
        devices: controls::DEVICES.iter().map(|d| Device { label: d.label }).collect(),
    }
}

#[derive(Serialize)]
pub struct Layout {
    inputs: Vec<[u8; controls::SLOTS]>,
    swap_sticks: bool,
    invert: u8,
}

#[derive(Serialize)]
pub struct State {
    layouts: Vec<Layout>,
}

/// The three layouts dhd.ini gives (the game's reading of its lines: a broken one falls back).
fn read(settings: &Path) -> Controls {
    let ini = dhd_settings::read(settings).unwrap_or_default();
    let values = controls::DEVICES.map(|d| ini.value(d.key));
    Controls::from_values(values.each_ref().map(|v| v.as_str()), &mut Vec::new())
}

fn state_of(c: &Controls) -> State {
    State {
        layouts: c
            .layouts
            .iter()
            .map(|l| Layout { inputs: l.input.to_vec(), swap_sticks: l.swap_sticks != 0, invert: l.invert })
            .collect(),
    }
}

pub fn state(settings: &Path) -> State {
    state_of(&read(settings))
}

/// A change as the page asks for it: `edit` is assign, clear, reset, swap or invert (`input` the
/// INVERT_* bit).
#[derive(Deserialize)]
pub struct Request {
    edit: String,
    target: usize,
    slot: usize,
    input: u8,
    on: bool,
}

#[derive(Serialize)]
pub struct Answer {
    /// changed, unchanged or refused
    outcome: &'static str,
    message: String,
    layouts: Vec<Layout>,
}

/// Makes the change (the game's rules) and saves the device's dhd.ini line when it changed.
/// `kind`: the connected controller's PAD_KIND_*, for the message's words.
pub fn edit(settings: &Path, device: usize, r: &Request, kind: i32, save: impl Fn(&str, &str) -> Result<(), String>) -> Result<Answer, String> {
    if device >= controls::DEVICES.len() {
        return Err(format!("no such layout: {device}"));
    }
    let edit = match r.edit.as_str() {
        "assign" => Edit::Assign { target: r.target, slot: r.slot, input: r.input },
        "clear" => Edit::Clear { target: r.target, slot: r.slot },
        "reset" => Edit::Reset,
        "swap" => Edit::SwapSticks(r.on),
        "invert" => Edit::Invert(r.input, r.on),
        other => return Err(format!("not an edit: {other}")),
    };
    let mut c = read(settings);
    let (outcome, message) = match c.edit(device, edit, kind) {
        Outcome::Changed(m) => {
            save(controls::DEVICES[device].key, &c.values()[device])?;
            ("changed", m)
        }
        Outcome::Unchanged => ("unchanged", String::new()),
        Outcome::Refused(m) => ("refused", m),
    };
    Ok(Answer { outcome, message, layouts: state_of(&c).layouts })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The schema the page gets: every key has its browser code, a controller has the buttons only.
    /// With DHD_CONTROLS_SCHEMA=FILE it is written there, with the default layouts, for
    /// port/launcher/test_controls.js ({"schema": ..., "state": ...}).
    #[test]
    fn schema_for_the_page() {
        let s = schema();
        assert_eq!((s.targets.len(), s.buttons, s.devices.len()), (24, 16, 3));
        assert!(s.keys.iter().all(|k| !k.code.is_empty() && k.vk != 0));
        assert!(s.pads.iter().any(|p| p.web == 0 && p.labels == ["A", "Cross", "B"]));
        if let Some(path) = std::env::var_os("DHD_CONTROLS_SCHEMA") {
            let both = serde_json::json!({ "schema": s, "state": state_of(&Controls::default()) });
            std::fs::write(path, both.to_string()).unwrap();
        }
    }

    #[test]
    fn edits_saved_and_read_back() {
        let dir = std::env::temp_dir().join(format!("dhd-launcher-controls-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let ini = dir.join("dhd.ini");
        let _ = std::fs::remove_file(&ini);
        let save = |k: &str, v: &str| dhd_settings::save(&ini, k, v).map(|_| ()).map_err(|e| e.to_string());
        let x = controls::key_named("x").unwrap().vk;
        let square = controls::target("square").unwrap();
        let ask = |edit: &str, target, input| Request { edit: edit.into(), target, slot: 0, input, on: true };
        let a = edit(&ini, 0, &ask("assign", square, x), 2, save).unwrap();
        assert_eq!((a.outcome, a.message.as_str()), ("changed", "Swapped Square and Cross"));
        assert_eq!(dhd_settings::read(&ini).unwrap().written("keyboard").as_deref(), Some("cross:space,z square:x"));
        // Cross's last key can't be cleared (finding 157: each device on its own)
        let cross = controls::target("cross").unwrap();
        assert_eq!(edit(&ini, 0, &ask("clear", cross, 0), 2, save).unwrap().outcome, "changed");
        let a = edit(&ini, 0, &ask("clear", cross, 0), 2, save).unwrap();
        assert_eq!((a.outcome, a.message.as_str()), ("refused", "Cross needs a key"));
        assert_eq!(dhd_settings::read(&ini).unwrap().written("keyboard").as_deref(), Some("cross:z square:x"));
        assert_eq!(state(&ini).layouts[0].inputs[square], [x, 0]);
        let a = edit(&ini, 2, &ask("swap", 0, 0), 2, save).unwrap();
        assert_eq!(a.outcome, "changed");
        assert!(a.layouts[2].swap_sticks);
        assert_eq!(dhd_settings::read(&ini).unwrap().written("pad2").as_deref(), Some("sticks:swap"));
        let a = edit(&ini, 0, &ask("reset", 0, 0), 2, save).unwrap();
        assert_eq!(a.outcome, "changed");
        assert_eq!(dhd_settings::read(&ini).unwrap().written("keyboard"), None, "an empty line: the defaults");
        assert!(edit(&ini, 3, &ask("reset", 0, 0), 2, save).is_err());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
