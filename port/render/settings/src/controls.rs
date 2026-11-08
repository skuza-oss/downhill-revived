//! Which keys and controller buttons stand for the PS2 pad's buttons (finding 157): dhd.ini's
//! `keyboard`, `pad1` and `pad2` lines, their defaults (the layout the game always had), how a line
//! is read and written, and the rules of a change: an input another button had swaps with the
//! button's old one (no duplicates), and no device is left without a way through the menus (each
//! of the keyboard, Controller 1 and Controller 2 on its own keeps Cross, Triangle, START, R1 (the
//! settings tabs) and the d-pad).
//!
//! - What is bound are the DualShock 2's buttons (`TARGETS`): the game's own meaning of each stays
//!   (Cross pedals in a race and confirms in the menus). The keyboard can also hold the sticks'
//!   directions; a controller's sticks are its own, with Swap Sticks and an Invert per axis.
//! - Each button has two slots, a main and an alternate (`SLOTS`).
//! - Keys go by their place on a US keyboard (`KEYS`: scan codes, so AZERTY and QWERTZ keep the
//!   places), controller buttons by their place on the pad (`PAD_INPUTS`: the bottom face button
//!   is `south`, whatever its label).
//! - Esc is never bound: it is always Triangle in the menus and START (pause) while riding, so the
//!   pause and the exit menu always work (keyboard.c). Alt, Ctrl and the Windows keys stay the
//!   system's.
//!
//! The game reads a line through the C interface (`c.rs`, port/hle/host.c) and the generated
//! port/hle/controls_table.h (`c_table`, the tables for the game core's CONTROLS page and the
//! host's keyboard and controller code); the launcher uses this module as a crate.
//!
//! A line, `keyboard = cross:space,x square:z ...`: entries `button:input[,input]` (main, then
//! alternate; `none` for nothing), apart by spaces, only the buttons that differ from the defaults.
//! A button the line doesn't name keeps its defaults, less any the line gives another button. A
//! controller's line may add `sticks:swap` and `invert:lx,ly,rx,ry` (the controller's own sticks:
//! left X, left Y, right X, right Y). An empty line, or none, is the defaults.

use std::fmt::Write;

/// A button's slots: the main input and the alternate.
pub const SLOTS: usize = 2;

/// What a binding drives: one of the DualShock 2's buttons, or (the keyboard's) a stick direction.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Target {
    /// The dhd.ini name.
    pub name: &'static str,
    pub label: &'static str,
    /// What it does in a race (docs/PLAYING.md's Controls table): the pages' help, nothing more.
    pub race: &'static str,
    /// libpad's bit (port/hle/hle.h PAD_*); 0 for a stick direction.
    pub bit: u16,
    /// A stick direction: its axis (1 left X, 2 left Y, 3 right X, 4 right Y; 0 none) and its way
    /// (-1 left or up, 1 right or down).
    pub axis: u8,
    pub dir: i8,
    /// The menus need it (Cross, Triangle, START, the d-pad, and R1 for the settings tabs): each device keeps an input on it, on
    /// its own (the keyboard doesn't count for Controller 1, nor Controller 1 for the keyboard).
    pub essential: bool,
}

const fn button(name: &'static str, label: &'static str, race: &'static str, bit: u16, essential: bool) -> Target {
    Target { name, label, race, bit, axis: 0, dir: 0, essential }
}

const fn stick(name: &'static str, label: &'static str, race: &'static str, axis: u8, dir: i8) -> Target {
    Target { name, label, race, bit: 0, axis, dir, essential: false }
}

/// The DualShock 2's buttons, then the sticks' directions (the keyboard's only), in the pages' order.
pub const TARGETS: [Target; 24] = [
    button("cross", "Cross", "Pedal (twice: sprint), confirm", 0x4000, true),
    button("circle", "Circle", "Kick", 0x2000, false),
    button("square", "Square", "Punch", 0x8000, false),
    button("triangle", "Triangle", "Brake (in the air: tweak), back", 0x1000, true),
    button("l1", "L1", "Tricks (with R1: Superman)", 0x0400, false),
    button("r1", "R1", "Tricks (with L1: Superman), settings tabs", 0x0800, true),
    button("l2", "L2", "Tricks", 0x0100, false),
    button("r2", "R2", "Tricks, bunny hop", 0x0200, false),
    button("l3", "L3", "Powerslide", 0x0002, false),
    button("r3", "R3", "Sprint", 0x0004, false),
    button("start", "START", "Pause, start the race", 0x0008, true),
    button("select", "SELECT", "With up or down: camera, twice: bail out", 0x0001, false),
    button("up", "Up", "Lean, menus", 0x0010, true),
    button("down", "Down", "Lean, menus", 0x0040, true),
    button("left", "Left", "Steer, menus", 0x0080, true),
    button("right", "Right", "Steer, menus", 0x0020, true),
    stick("lstick_up", "Left Stick Up", "Lean", 2, -1),
    stick("lstick_down", "Left Stick Down", "Lean", 2, 1),
    stick("lstick_left", "Left Stick Left", "Steer", 1, -1),
    stick("lstick_right", "Left Stick Right", "Steer", 1, 1),
    stick("rstick_up", "Right Stick Up", "", 4, -1),
    stick("rstick_down", "Right Stick Down", "", 4, 1),
    stick("rstick_left", "Right Stick Left", "", 3, -1),
    stick("rstick_right", "Right Stick Right", "", 3, 1),
];
/// The buttons (TARGETS before the sticks' directions): what a controller can bind.
pub const BUTTONS: usize = 16;

/// A key the keyboard may bind: its dhd.ini name, the pages' word for it, the Windows virtual-key
/// code of the US key at its place (the game's keyboard tables, port/hle/keyboard.c), SDL's scan
/// code (USB HID usage: the place) and the browser's KeyboardEvent.code (the launcher).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Key {
    pub name: &'static str,
    pub label: &'static str,
    pub vk: u8,
    pub scancode: u16,
    pub code: &'static str,
}

const fn key(name: &'static str, label: &'static str, vk: u8, scancode: u16, code: &'static str) -> Key {
    Key { name, label, vk, scancode, code }
}

/// Every key that can be bound. Not Esc (always back and pause), Alt, Ctrl, the Windows keys or Caps
/// Lock. The keypad's Enter is Enter (the game always took both).
pub const KEYS: &[Key] = &[
    key("a", "A", b'A', 4, "KeyA"), key("b", "B", b'B', 5, "KeyB"), key("c", "C", b'C', 6, "KeyC"),
    key("d", "D", b'D', 7, "KeyD"), key("e", "E", b'E', 8, "KeyE"), key("f", "F", b'F', 9, "KeyF"),
    key("g", "G", b'G', 10, "KeyG"), key("h", "H", b'H', 11, "KeyH"), key("i", "I", b'I', 12, "KeyI"),
    key("j", "J", b'J', 13, "KeyJ"), key("k", "K", b'K', 14, "KeyK"), key("l", "L", b'L', 15, "KeyL"),
    key("m", "M", b'M', 16, "KeyM"), key("n", "N", b'N', 17, "KeyN"), key("o", "O", b'O', 18, "KeyO"),
    key("p", "P", b'P', 19, "KeyP"), key("q", "Q", b'Q', 20, "KeyQ"), key("r", "R", b'R', 21, "KeyR"),
    key("s", "S", b'S', 22, "KeyS"), key("t", "T", b'T', 23, "KeyT"), key("u", "U", b'U', 24, "KeyU"),
    key("v", "V", b'V', 25, "KeyV"), key("w", "W", b'W', 26, "KeyW"), key("x", "X", b'X', 27, "KeyX"),
    key("y", "Y", b'Y', 28, "KeyY"), key("z", "Z", b'Z', 29, "KeyZ"),
    key("1", "1", b'1', 30, "Digit1"), key("2", "2", b'2', 31, "Digit2"), key("3", "3", b'3', 32, "Digit3"),
    key("4", "4", b'4', 33, "Digit4"), key("5", "5", b'5', 34, "Digit5"), key("6", "6", b'6', 35, "Digit6"),
    key("7", "7", b'7', 36, "Digit7"), key("8", "8", b'8', 37, "Digit8"), key("9", "9", b'9', 38, "Digit9"),
    key("0", "0", b'0', 39, "Digit0"),
    key("enter", "Enter", 0x0D, 40, "Enter"), key("backspace", "Backspace", 0x08, 42, "Backspace"),
    key("tab", "Tab", 0x09, 43, "Tab"), key("space", "Space", 0x20, 44, "Space"),
    key("minus", "Minus", 0xBD, 45, "Minus"), key("equals", "Equals", 0xBB, 46, "Equal"),
    key("lbracket", "Left Bracket", 0xDB, 47, "BracketLeft"), key("rbracket", "Right Bracket", 0xDD, 48, "BracketRight"),
    key("backslash", "Backslash", 0xDC, 49, "Backslash"), key("semicolon", "Semicolon", 0xBA, 51, "Semicolon"),
    key("quote", "Quote", 0xDE, 52, "Quote"), key("backquote", "Backquote", 0xC0, 53, "Backquote"),
    key("comma", "Comma", 0xBC, 54, "Comma"), key("period", "Period", 0xBE, 55, "Period"),
    key("slash", "Slash", 0xBF, 56, "Slash"), key("intlbackslash", "Key Beside Shift", 0xE2, 100, "IntlBackslash"),
    key("f1", "F1", 0x70, 58, "F1"), key("f2", "F2", 0x71, 59, "F2"), key("f3", "F3", 0x72, 60, "F3"),
    key("f4", "F4", 0x73, 61, "F4"), key("f5", "F5", 0x74, 62, "F5"), key("f6", "F6", 0x75, 63, "F6"),
    key("f7", "F7", 0x76, 64, "F7"), key("f8", "F8", 0x77, 65, "F8"), key("f9", "F9", 0x78, 66, "F9"),
    key("f10", "F10", 0x79, 67, "F10"), key("f11", "F11", 0x7A, 68, "F11"), key("f12", "F12", 0x7B, 69, "F12"),
    key("insert", "Insert", 0x2D, 73, "Insert"), key("home", "Home", 0x24, 74, "Home"),
    key("pageup", "Page Up", 0x21, 75, "PageUp"), key("delete", "Delete", 0x2E, 76, "Delete"),
    key("end", "End", 0x23, 77, "End"), key("pagedown", "Page Down", 0x22, 78, "PageDown"),
    key("right", "Right", 0x27, 79, "ArrowRight"), key("left", "Left", 0x25, 80, "ArrowLeft"),
    key("down", "Down", 0x28, 81, "ArrowDown"), key("up", "Up", 0x26, 82, "ArrowUp"),
    key("kp_divide", "Num Divide", 0x6F, 84, "NumpadDivide"), key("kp_multiply", "Num Multiply", 0x6A, 85, "NumpadMultiply"),
    key("kp_minus", "Num Minus", 0x6D, 86, "NumpadSubtract"), key("kp_plus", "Num Plus", 0x6B, 87, "NumpadAdd"),
    key("kp1", "Num 1", 0x61, 89, "Numpad1"), key("kp2", "Num 2", 0x62, 90, "Numpad2"), key("kp3", "Num 3", 0x63, 91, "Numpad3"),
    key("kp4", "Num 4", 0x64, 92, "Numpad4"), key("kp5", "Num 5", 0x65, 93, "Numpad5"), key("kp6", "Num 6", 0x66, 94, "Numpad6"),
    key("kp7", "Num 7", 0x67, 95, "Numpad7"), key("kp8", "Num 8", 0x68, 96, "Numpad8"), key("kp9", "Num 9", 0x69, 97, "Numpad9"),
    key("kp0", "Num 0", 0x60, 98, "Numpad0"), key("kp_period", "Num Period", 0x6E, 99, "NumpadDecimal"),
    key("lshift", "Left Shift", 0xA0, 225, "ShiftLeft"), key("rshift", "Right Shift", 0xA1, 229, "ShiftRight"),
];

/// The pad's word for each input, by the controller's kind (port/hle/hle.h PAD_KIND_*: the
/// labels a pad of that kind prints); a pad of no known kind gets Xbox's.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PadLabels {
    pub xbox: &'static str,
    pub playstation: &'static str,
    pub nintendo: &'static str,
}

/// A controller input that may be bound, by its place on the pad: its id in a layout, its dhd.ini
/// name, its labels, SDL's SDL_GamepadButton (-1 for a trigger), the trigger (1 left, 2 right, 0
/// none) and the browser's standard-mapping button index (the launcher; -1 none).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PadInput {
    pub id: u8,
    pub name: &'static str,
    pub labels: PadLabels,
    pub sdl: i8,
    pub trigger: u8,
    pub web: i8,
}

const fn pad(id: u8, name: &'static str, labels: [&'static str; 3], sdl: i8, web: i8) -> PadInput {
    PadInput { id, name, labels: PadLabels { xbox: labels[0], playstation: labels[1], nintendo: labels[2] }, sdl, trigger: 0, web }
}

const fn trigger(id: u8, name: &'static str, labels: [&'static str; 3], which: u8, web: i8) -> PadInput {
    PadInput { trigger: which, ..pad(id, name, labels, -1, web) }
}

/// Every controller input that can be bound (not the Guide / PS / Home button: the system's).
pub const PAD_INPUTS: &[PadInput] = &[
    pad(1, "south", ["A", "Cross", "B"], 0, 0),
    pad(2, "east", ["B", "Circle", "A"], 1, 1),
    pad(3, "west", ["X", "Square", "Y"], 2, 2),
    pad(4, "north", ["Y", "Triangle", "X"], 3, 3),
    pad(5, "back", ["View", "Share", "Minus"], 4, 8),
    pad(6, "start", ["Menu", "Options", "Plus"], 6, 9),
    pad(7, "ls", ["Left Stick", "L3", "Left Stick"], 7, 10),
    pad(8, "rs", ["Right Stick", "R3", "Right Stick"], 8, 11),
    pad(9, "lb", ["LB", "L1", "L"], 9, 4),
    pad(10, "rb", ["RB", "R1", "R"], 10, 5),
    pad(11, "dpad_up", ["D-pad Up", "D-pad Up", "D-pad Up"], 11, 12),
    pad(12, "dpad_down", ["D-pad Down", "D-pad Down", "D-pad Down"], 12, 13),
    pad(13, "dpad_left", ["D-pad Left", "D-pad Left", "D-pad Left"], 13, 14),
    pad(14, "dpad_right", ["D-pad Right", "D-pad Right", "D-pad Right"], 14, 15),
    pad(15, "misc", ["Share", "Mic", "Capture"], 15, -1),
    pad(16, "paddle_r1", ["P1", "Right Paddle", "Right Paddle"], 16, -1),
    pad(17, "paddle_l1", ["P3", "Left Paddle", "Left Paddle"], 17, -1),
    pad(18, "paddle_r2", ["P2", "Right Paddle 2", "Right Paddle 2"], 18, -1),
    pad(19, "paddle_l2", ["P4", "Left Paddle 2", "Left Paddle 2"], 19, -1),
    pad(20, "touchpad", ["Touchpad", "Touchpad", "Touchpad"], 20, 17),
    trigger(21, "lt", ["LT", "L2", "ZL"], 1, 6),
    trigger(22, "rt", ["RT", "R2", "ZR"], 2, 7),
];

/// One of the three layouts: the keyboard (player 1's) and the two players' controllers.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Device {
    /// The dhd.ini key, and the DHD_* variable the game reads (table.rs).
    pub key: &'static str,
    pub env: &'static str,
    pub label: &'static str,
    pub pad: bool,
    /// 1 or 2.
    pub player: u8,
}

pub const KEYBOARD: usize = 0;
pub const PAD1: usize = 1;
pub const PAD2: usize = 2;
pub const DEVICES: [Device; 3] = [
    Device { key: "keyboard", env: "DHD_KEYBOARD", label: "Keyboard", pad: false, player: 1 },
    Device { key: "pad1", env: "DHD_PAD1", label: "Controller 1", pad: true, player: 1 },
    Device { key: "pad2", env: "DHD_PAD2", label: "Controller 2", pad: true, player: 2 },
];

/// A controller's own sticks, inverted: these bits of `Layout::invert`.
pub const INVERT_LX: u8 = 1;
pub const INVERT_LY: u8 = 2;
pub const INVERT_RX: u8 = 4;
pub const INVERT_RY: u8 = 8;
const INVERT_NAMES: [(u8, &str); 4] = [(INVERT_LX, "lx"), (INVERT_LY, "ly"), (INVERT_RX, "rx"), (INVERT_RY, "ry")];

/// A layout, as the game keeps it (port/hle/controls_table.h's ControlsLayout, 52 bytes): each
/// target's main and alternate input (the keyboard: the key's `vk`; a controller: the input's
/// `id`; 0 nothing; an alternate only with a main), and a controller's stick options.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Layout {
    pub input: [[u8; SLOTS]; 24],
    pub swap_sticks: u8,
    /// INVERT_* bits.
    pub invert: u8,
    pub unused: [u8; 2],
}

/// The game's layouts before rebinding: the keyboard of docs/PLAYING.md (finding 059) and the pads
/// by position (finding 059: bottom = Cross).
const KEYBOARD_DEFAULTS: [(&str, [&str; SLOTS]); 16] = [
    ("cross", ["space", "x"]), ("circle", ["c", ""]), ("square", ["z", ""]), ("triangle", ["s", "backspace"]),
    ("l1", ["a", ""]), ("r1", ["d", ""]), ("l2", ["q", ""]), ("r2", ["e", ""]), ("l3", ["f", ""]), ("r3", ["w", ""]),
    ("start", ["enter", ""]), ("select", ["tab", ""]),
    ("up", ["up", ""]), ("down", ["down", ""]), ("left", ["left", ""]), ("right", ["right", ""]),
];
const PAD_DEFAULTS: [(&str, &str); 16] = [
    ("cross", "south"), ("circle", "east"), ("square", "west"), ("triangle", "north"), ("l1", "lb"), ("r1", "rb"),
    ("l2", "lt"), ("r2", "rt"), ("l3", "ls"), ("r3", "rs"), ("start", "start"), ("select", "back"),
    ("up", "dpad_up"), ("down", "dpad_down"), ("left", "dpad_left"), ("right", "dpad_right"),
];

pub fn target(name: &str) -> Option<usize> {
    TARGETS.iter().position(|t| t.name == name)
}

pub fn key_named(name: &str) -> Option<&'static Key> {
    KEYS.iter().find(|k| k.name == name)
}

pub fn key_of(vk: u8) -> Option<&'static Key> {
    KEYS.iter().find(|k| k.vk == vk)
}

pub fn pad_named(name: &str) -> Option<&'static PadInput> {
    PAD_INPUTS.iter().find(|p| p.name == name)
}

pub fn pad_of(id: u8) -> Option<&'static PadInput> {
    PAD_INPUTS.iter().find(|p| p.id == id)
}

/// The input code a layout of `device` keeps for the dhd.ini name (vk or id).
fn input_named(device: usize, name: &str) -> Option<u8> {
    if DEVICES[device].pad {
        pad_named(name).map(|p| p.id)
    } else {
        key_named(name).map(|k| k.vk)
    }
}

/// The dhd.ini name of a code of `device`'s layouts.
fn input_name(device: usize, code: u8) -> &'static str {
    if DEVICES[device].pad {
        pad_of(code).map_or("?", |p| p.name)
    } else {
        key_of(code).map_or("?", |k| k.name)
    }
}

/// The pages' word for a code of `device`'s layouts; a controller's by its kind (PAD_KIND_*: 1
/// PlayStation, 3 Nintendo, else Xbox's words).
pub fn input_label(device: usize, code: u8, kind: i32) -> &'static str {
    if DEVICES[device].pad {
        pad_of(code).map_or("?", |p| match kind {
            1 => p.labels.playstation,
            3 => p.labels.nintendo,
            _ => p.labels.xbox,
        })
    } else {
        key_of(code).map_or("?", |k| k.label)
    }
}

/// Whether `target` can be bound on `device` (a controller's sticks are its own).
pub fn bindable(device: usize, target: usize) -> bool {
    target < TARGETS.len() && (!DEVICES[device].pad || target < BUTTONS)
}

pub fn default_layout(device: usize) -> Layout {
    let mut l = Layout::default();
    if DEVICES[device].pad {
        for (name, input) in PAD_DEFAULTS {
            l.input[target(name).unwrap()][0] = pad_named(input).unwrap().id;
        }
    } else {
        for (name, keys) in KEYBOARD_DEFAULTS {
            for (slot, k) in keys.iter().enumerate().filter(|(_, k)| !k.is_empty()) {
                l.input[target(name).unwrap()][slot] = key_named(k).unwrap().vk;
            }
        }
    }
    l
}

/// An alternate without a main becomes the main.
fn tidy(slots: &mut [u8; SLOTS]) {
    if slots[0] == 0 {
        slots.swap(0, 1);
    }
}

/// The target and slot that hold `code`, if one does.
fn holder(l: &Layout, code: u8) -> Option<(usize, usize)> {
    (0..TARGETS.len()).find_map(|t| l.input[t].iter().position(|&c| c == code).map(|s| (t, s)))
}

/// The layout a line of `device` gives (see the top). Each entry that can't be read is skipped,
/// with a line in `problems` (the game logs them): its button keeps its default.
pub fn parse(device: usize, text: &str, problems: &mut Vec<String>) -> Layout {
    let dev = &DEVICES[device];
    let mut l = default_layout(device);
    let mut named = [false; 24];
    let mut problem = |what: String| problems.push(format!("{}: {what}", dev.key));
    for entry in text.split_whitespace() {
        let Some((name, values)) = entry.split_once(':') else {
            problem(format!("\"{entry}\" is not button:input"));
            continue;
        };
        match (name, dev.pad) {
            ("sticks", true) => {
                match values {
                    "swap" => l.swap_sticks = 1,
                    "normal" => l.swap_sticks = 0,
                    _ => problem(format!("sticks:{values} is not sticks:swap")),
                }
                continue;
            }
            ("invert", true) => {
                l.invert = 0;
                for v in values.split(',').filter(|v| !v.is_empty() && *v != "none") {
                    match INVERT_NAMES.iter().find(|(_, n)| *n == v) {
                        Some((bit, _)) => l.invert |= bit,
                        None => problem(format!("invert:{v} is not lx, ly, rx or ry")),
                    }
                }
                continue;
            }
            _ => {}
        }
        let Some(t) = target(name).filter(|&t| bindable(device, t)) else {
            problem(format!("{name} is not a button {}", if dev.pad { "of a controller" } else { "of the keyboard" }));
            continue;
        };
        if named[t] {
            problem(format!("{name} is named twice: the first one counts"));
            continue;
        }
        let mut slots = [0u8; SLOTS];
        let mut n = 0;
        for v in values.split(',').filter(|v| !v.is_empty() && *v != "none" && *v != "-") {
            let Some(code) = input_named(device, v) else {
                problem(format!("{name}: \"{v}\" is not {}", if dev.pad { "a controller's button" } else { "a key the game can use" }));
                continue;
            };
            if slots.contains(&code) {
                continue;
            }
            if let Some(other) = (0..24).find(|&u| named[u] && l.input[u].contains(&code)) {
                problem(format!("{name}: {v} is {}'s already", TARGETS[other].label));
                continue;
            }
            if n == SLOTS {
                problem(format!("{name}: two inputs at most, {v} left out"));
                continue;
            }
            slots[n] = code;
            n += 1;
        }
        // `none` (or nothing) unbinds the button; a value the game can't read is skipped, so a
        // button with none it can read keeps its default (PLAYING.md, "Controls").
        if n == 0 && values.split(',').any(|v| !v.is_empty() && v != "none" && v != "-") {
            continue;
        }
        l.input[t] = slots;
        named[t] = true;
    }
    // a default the line gave another button leaves the button the line doesn't name
    for t in (0..24).filter(|&t| !named[t]) {
        for s in 0..SLOTS {
            let code = l.input[t][s];
            if code != 0 && (0..24).any(|u| named[u] && l.input[u].contains(&code)) {
                l.input[t][s] = 0;
            }
        }
        tidy(&mut l.input[t]);
    }
    l
}

/// The line for a layout of `device`: the buttons that differ from the defaults, then a
/// controller's stick options; "" for the defaults.
pub fn format(device: usize, l: &Layout) -> String {
    let defaults = default_layout(device);
    let mut out = String::new();
    let mut add = |s: &str| {
        if !out.is_empty() {
            out.push(' ');
        }
        out.push_str(s);
    };
    for t in 0..24 {
        if l.input[t] == defaults.input[t] {
            continue;
        }
        let codes: Vec<&str> = l.input[t].iter().filter(|&&c| c != 0).map(|&c| input_name(device, c)).collect();
        add(&format!("{}:{}", TARGETS[t].name, if codes.is_empty() { "none".to_string() } else { codes.join(",") }));
    }
    if DEVICES[device].pad {
        if l.swap_sticks != 0 {
            add("sticks:swap");
        }
        if l.invert != 0 {
            let axes: Vec<&str> = INVERT_NAMES.iter().filter(|(b, _)| l.invert & b != 0).map(|(_, n)| *n).collect();
            add(&format!("invert:{}", axes.join(",")));
        }
    }
    out
}

/// A change to one layout, on a page.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Edit {
    /// `input` (a key's vk, a controller input's id) to the target's slot; a button that had it gets the
    /// target's old one there (a swap).
    Assign { target: usize, slot: usize, input: u8 },
    Clear { target: usize, slot: usize },
    /// The device's defaults.
    Reset,
    SwapSticks(bool),
    /// An INVERT_* bit on or off.
    Invert(u8, bool),
}

/// What an edit did: the new layout is in place (`Changed`, with what the page says, "" nothing
/// more), nothing to do (`Unchanged`), or it is refused, with why.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Outcome {
    Changed(String),
    Unchanged,
    Refused(String),
}

/// The three layouts together (the dhd.ini lines are read and written as one).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Controls {
    pub layouts: [Layout; 3],
}

impl Default for Controls {
    fn default() -> Self {
        Controls { layouts: [default_layout(KEYBOARD), default_layout(PAD1), default_layout(PAD2)] }
    }
}

/// What a device's essential button needs, in the pages' words: "a key" or "a button".
fn an_input(device: usize) -> &'static str {
    if DEVICES[device].pad { "a button" } else { "a key" }
}

impl Controls {
    /// The layouts the three lines give (`parse`). A hand-made line that leaves an essential button
    /// with nothing on its device puts that device alone back to its defaults, with a line in
    /// `problems`.
    pub fn from_values(values: [&str; 3], problems: &mut Vec<String>) -> Controls {
        let mut c = Controls { layouts: [0, 1, 2].map(|d| parse(d, values[d], problems)) };
        for device in 0..DEVICES.len() {
            if let Some(t) = c.missing(device) {
                problems.push(format!(
                    "{}: {} has no {}: {} is back to its defaults",
                    DEVICES[device].key,
                    TARGETS[t].label,
                    if DEVICES[device].pad { "button" } else { "key" },
                    DEVICES[device].label
                ));
                c.layouts[device] = default_layout(device);
            }
        }
        c
    }

    /// The three dhd.ini lines.
    pub fn values(&self) -> [String; 3] {
        [0, 1, 2].map(|d| format(d, &self.layouts[d]))
    }

    /// The first essential target `device`'s layout has nothing on.
    pub fn missing(&self, device: usize) -> Option<usize> {
        (0..TARGETS.len()).find(|&t| TARGETS[t].essential && self.layouts[device].input[t][0] == 0)
    }

    /// The first device with an essential target it has nothing on: (device, target).
    pub fn unreachable(&self) -> Option<(usize, usize)> {
        (0..DEVICES.len()).find_map(|d| self.missing(d).map(|t| (d, t)))
    }

    /// Makes the change to `device`'s layout, unless it would leave that device without an
    /// essential button ("Cross needs a button"). An input another button has swaps: the target
    /// takes it, the other button the target's old input in that slot (none there: the other button
    /// keeps what else it has). `kind`: the controller's PAD_KIND_*, for the words of the message.
    pub fn edit(&mut self, device: usize, edit: Edit, kind: i32) -> Outcome {
        let dev = &DEVICES[device];
        let mut next = *self;
        let l = &mut next.layouts[device];
        let mut message = String::new();
        match edit {
            Edit::Assign { target, slot, input } => {
                if !bindable(device, target) || slot >= SLOTS {
                    return Outcome::Refused(format!("{} can't be set on the {}", label_of(target), dev.label.to_lowercase()));
                }
                if input_label(device, input, kind) == "?" || input == 0 {
                    return Outcome::Refused(if dev.pad { "That button can't be used".into() } else { "That key can't be used".into() });
                }
                let word = input_label(device, input, kind);
                // an alternate without a main is the main
                let slot = if l.input[target][0] == 0 { 0 } else { slot };
                match holder(l, input) {
                    Some((t, s)) if t == target && s == slot => return Outcome::Unchanged,
                    Some((t, _)) if t == target => {
                        // the button's other slot: the two change places
                        l.input[t].swap(0, 1);
                        tidy(&mut l.input[t]);
                    }
                    Some((t, s)) => {
                        let old = l.input[target][slot];
                        l.input[target][slot] = input;
                        l.input[t][s] = old;
                        tidy(&mut l.input[t]);
                        message = if old != 0 {
                            format!("Swapped {} and {}", TARGETS[target].label, TARGETS[t].label)
                        } else {
                            format!("{word} moved from {} to {}", TARGETS[t].label, TARGETS[target].label)
                        };
                    }
                    None => l.input[target][slot] = input,
                }
            }
            Edit::Clear { target, slot } => {
                if !bindable(device, target) || slot >= SLOTS || l.input[target][slot] == 0 {
                    return Outcome::Unchanged;
                }
                l.input[target][slot] = 0;
                tidy(&mut l.input[target]);
            }
            Edit::Reset => *l = default_layout(device),
            Edit::SwapSticks(on) if dev.pad => l.swap_sticks = on as u8,
            Edit::Invert(bit, on) if dev.pad && INVERT_NAMES.iter().any(|(b, _)| *b == bit) => {
                l.invert = if on { l.invert | bit } else { l.invert & !bit };
            }
            Edit::SwapSticks(_) | Edit::Invert(..) => return Outcome::Unchanged,
        }
        if next == *self {
            return Outcome::Unchanged;
        }
        if let Some(t) = next.missing(device) {
            return Outcome::Refused(format!("{} needs {}", TARGETS[t].label, an_input(device)));
        }
        *self = next;
        Outcome::Changed(message)
    }
}

fn label_of(target: usize) -> &'static str {
    TARGETS.get(target).map_or("?", |t| t.label)
}

/// The C tables (port/hle/controls_table.h, generated: tests/generated.rs keeps it equal) for the
/// game core's CONTROLS page (port/hle/controls_settings.c) and the host's keyboard and controller
/// code (keyboard.c, sdl_host.c, gamepad.c).
pub fn c_table() -> String {
    let mut out = String::from(
        "/* Keyboard and controller bindings' tables (finding 157): generated from\n   \
         port/render/settings/src/controls.rs (dhd_settings), whose test keeps this file equal to it\n   \
         (DHD_SETTINGS_BLESS=1 cargo test -p dhd_settings writes it). Don't edit it here.\n   \
         A layout's inputs: the keyboard's are the keys' vk (the Windows virtual-key code of the US key\n   \
         at the place), a controller's the inputs' id; 0 nothing. */\n\
         #ifndef DHD_CONTROLS_TABLE_H\n#define DHD_CONTROLS_TABLE_H\n\n#include <stdint.h>\n\n",
    );
    let _ = writeln!(
        out,
        "enum {{ CONTROL_TARGETS = {}, CONTROL_BUTTONS = {BUTTONS}, CONTROL_SLOTS = {SLOTS}, CONTROL_DEVICES = 3, \
         CONTROL_KEYS = {}, CONTROL_PAD_INPUTS = {} }};",
        TARGETS.len(),
        KEYS.len(),
        PAD_INPUTS.len()
    );
    out += "enum { CONTROL_KEYBOARD, CONTROL_PAD1, CONTROL_PAD2 };\n";
    let _ = writeln!(
        out,
        "enum {{ CONTROL_INVERT_LX = {INVERT_LX}, CONTROL_INVERT_LY = {INVERT_LY}, CONTROL_INVERT_RX = {INVERT_RX}, \
         CONTROL_INVERT_RY = {INVERT_RY} }};"
    );
    out += "/* dhd_controls_edit's edits and answers (dhd_settings.h) */\n\
            enum { CONTROL_ASSIGN, CONTROL_CLEAR, CONTROL_RESET, CONTROL_SWAP_STICKS, CONTROL_INVERT };\n\
            enum { CONTROL_REFUSED = -1, CONTROL_UNCHANGED = 0, CONTROL_CHANGED = 1 };\n\n";
    out += "/* A layout: each target's main and alternate input, a controller's stick options (52 bytes). */\n\
            typedef struct ControlsLayout { uint8_t input[CONTROL_TARGETS][CONTROL_SLOTS]; uint8_t swap_sticks, invert, unused[2]; } ControlsLayout;\n\n";
    out += "/* The PS2 pad's buttons, then the keyboard's stick directions: dhd.ini name, the pages' word, what it\n   \
            does in a race, libpad's bit (0: a stick direction: axis 1 LX 2 LY 3 RX 4 RY, dir -1 / 1), and whether\n   \
            the menus need it. */\n\
            typedef struct { const char *name, *label, *race; uint16_t bit; uint8_t axis; int8_t dir; uint8_t essential; } ControlTarget;\n\
            static const ControlTarget control_targets[CONTROL_TARGETS] = {\n";
    for t in &TARGETS {
        let _ = writeln!(
            out,
            "    {{\"{}\", \"{}\", \"{}\", 0x{:04X}, {}, {}, {}}},",
            t.name, t.label, t.race, t.bit, t.axis, t.dir, t.essential as u8
        );
    }
    out += "};\n\n/* The keys that can be bound: dhd.ini name, the pages' word, vk, SDL's scan code (the place). */\n\
            typedef struct { const char *name, *label; uint8_t vk; uint16_t scancode; } ControlKey;\n\
            static const ControlKey control_keys[CONTROL_KEYS] = {\n";
    for k in KEYS {
        let _ = writeln!(out, "    {{\"{}\", \"{}\", 0x{:02X}, {}}},", k.name, k.label, k.vk, k.scancode);
    }
    out += "};\n\n/* The controller inputs that can be bound: id, dhd.ini name, the words of an Xbox, a PlayStation and a\n   \
            Nintendo pad, SDL_GamepadButton (-1: a trigger: 1 left, 2 right). */\n\
            typedef struct { uint8_t id; const char *name, *label[3]; int8_t sdl; uint8_t trigger; } ControlPadInput;\n\
            static const ControlPadInput control_pad_inputs[CONTROL_PAD_INPUTS] = {\n";
    for p in PAD_INPUTS {
        let _ = writeln!(
            out,
            "    {{{}, \"{}\", {{\"{}\", \"{}\", \"{}\"}}, {}, {}}},",
            p.id, p.name, p.labels.xbox, p.labels.playstation, p.labels.nintendo, p.sdl, p.trigger
        );
    }
    out += "};\n\n/* The defaults: the keyboard, Controller 1, Controller 2 (the game's layouts before rebinding). */\n\
            static const ControlsLayout control_defaults[CONTROL_DEVICES] = {\n";
    for d in 0..3 {
        let l = default_layout(d);
        let pairs: Vec<String> = l.input.iter().map(|s| format!("{{{}, {}}}", s[0], s[1])).collect();
        let _ = writeln!(out, "    {{{{{}}}, {}, {}, {{0, 0}}}},", pairs.join(", "), l.swap_sticks, l.invert);
    }
    out += "};\n\n/* The settings: dhd.ini key and variable of the keyboard, Controller 1 and Controller 2. */\n\
            static const struct { const char *key, *env, *label; } control_devices[CONTROL_DEVICES] = {\n";
    for d in &DEVICES {
        let _ = writeln!(out, "    {{\"{}\", \"{}\", \"{}\"}},", d.key, d.env, d.label);
    }
    out += "};\n\n#endif\n";
    out
}
