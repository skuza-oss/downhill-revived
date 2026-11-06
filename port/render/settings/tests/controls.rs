//! The bindings' rules (controls.rs, finding 157): the defaults are the game's old layouts, lines
//! read and written, a swapped or moved input, the rule that keeps the menus reachable on each
//! device on its own.

use dhd_settings::controls::*;

fn vk(name: &str) -> u8 {
    key_named(name).unwrap().vk
}

fn id(name: &str) -> u8 {
    pad_named(name).unwrap().id
}

fn t(name: &str) -> usize {
    target(name).unwrap()
}

fn layouts(keyboard: &str, pad1: &str, pad2: &str) -> (Controls, Vec<String>) {
    let mut problems = Vec::new();
    let c = Controls::from_values([keyboard, pad1, pad2], &mut problems);
    (c, problems)
}

#[test]
fn defaults_are_the_old_layouts() {
    // keyboard.c's table before rebinding (finding 059), by virtual-key code
    let k = default_layout(KEYBOARD);
    let want: [(&str, [u8; 2]); 16] = [
        ("cross", [0x20, b'X']), ("circle", [b'C', 0]), ("square", [b'Z', 0]), ("triangle", [b'S', 0x08]),
        ("l1", [b'A', 0]), ("r1", [b'D', 0]), ("l2", [b'Q', 0]), ("r2", [b'E', 0]), ("l3", [b'F', 0]), ("r3", [b'W', 0]),
        ("start", [0x0D, 0]), ("select", [0x09, 0]), ("up", [0x26, 0]), ("down", [0x28, 0]), ("left", [0x25, 0]),
        ("right", [0x27, 0]),
    ];
    for (name, keys) in want {
        assert_eq!(k.input[t(name)], keys, "{name}");
    }
    for s in BUTTONS..TARGETS.len() {
        assert_eq!(k.input[s], [0, 0], "the keyboard had no stick keys");
    }
    // gamepad.c's: by position, the triggers L2 and R2
    let p = default_layout(PAD1);
    for (name, input) in [("cross", "south"), ("circle", "east"), ("square", "west"), ("triangle", "north"), ("select", "back"),
                          ("start", "start"), ("l3", "ls"), ("r3", "rs"), ("l1", "lb"), ("r1", "rb"), ("l2", "lt"), ("r2", "rt"),
                          ("up", "dpad_up"), ("down", "dpad_down"), ("left", "dpad_left"), ("right", "dpad_right")] {
        assert_eq!(p.input[t(name)], [id(input), 0], "{name}");
    }
    assert_eq!(p, default_layout(PAD2));
    assert_eq!((p.swap_sticks, p.invert), (0, 0));
    for d in 0..3 {
        assert_eq!(format(d, &default_layout(d)), "");
        assert_eq!(parse(d, "", &mut Vec::new()), default_layout(d));
    }
    assert_eq!(Controls::default().unreachable(), None);
}

#[test]
fn tables() {
    for (i, k) in KEYS.iter().enumerate() {
        assert!(KEYS[i + 1..].iter().all(|o| o.name != k.name && o.vk != k.vk && o.scancode != k.scancode && o.code != k.code), "{}", k.name);
        assert!(k.vk != 0x1B && k.vk != 0x10 && k.vk != 0x11 && k.vk != 0x12, "{}: Esc, Shift, Ctrl and Alt are the game's or the system's", k.name);
        assert!(k.name.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_'));
        assert!(k.label.bytes().all(|b| b.is_ascii_alphanumeric() || b == b' '), "{}: the game's font draws letters, digits and spaces", k.label);
    }
    for (i, p) in PAD_INPUTS.iter().enumerate() {
        assert_eq!(p.id as usize, i + 1);
        assert!(PAD_INPUTS[i + 1..].iter().all(|o| o.name != p.name));
        assert_eq!(p.sdl < 0, p.trigger != 0);
    }
    for d in DEVICES {
        let s = dhd_settings::find(d.key).expect("a settings row");
        assert_eq!(s.env, d.env);
        assert_eq!(s.default, "", "an empty line is the defaults");
    }
    assert!(TARGETS.iter().filter(|t| t.essential).count() == 8);
}

#[test]
fn lines_read_and_written() {
    let mut problems = Vec::new();
    // a button the line names gets just that; a default the line gives it leaves its old button
    let l = parse(KEYBOARD, "square:x", &mut problems);
    assert!(problems.is_empty(), "{problems:?}");
    assert_eq!(l.input[t("square")], [vk("x"), 0]);
    assert_eq!(l.input[t("cross")], [vk("space"), 0]);
    assert_eq!(format(KEYBOARD, &l), "cross:space square:x");
    assert_eq!(parse(KEYBOARD, &format(KEYBOARD, &l), &mut problems), l);

    let l = parse(KEYBOARD, "  cross:j,k   lstick_left:kp4 start:none ", &mut problems);
    assert!(problems.is_empty(), "{problems:?}");
    assert_eq!(l.input[t("cross")], [vk("j"), vk("k")]);
    assert_eq!(l.input[t("lstick_left")], [vk("kp4"), 0]);
    assert_eq!(l.input[t("start")], [0, 0]);
    assert_eq!(format(KEYBOARD, &l), "cross:j,k start:none lstick_left:kp4");

    let p = parse(PAD1, "cross:east circle:south sticks:swap invert:ry,lx", &mut problems);
    assert!(problems.is_empty(), "{problems:?}");
    assert_eq!((p.input[t("cross")], p.input[t("circle")]), ([id("east"), 0], [id("south"), 0]));
    assert_eq!((p.swap_sticks, p.invert), (1, INVERT_LX | INVERT_RY));
    assert_eq!(format(PAD1, &p), "cross:east circle:south sticks:swap invert:lx,ry");
}

#[test]
fn broken_lines_fall_back() {
    let mut problems = Vec::new();
    let l = parse(KEYBOARD, "cross:nokey square circle:c,c,c triangle:esc sticks:swap cross:j l1:x,a,b", &mut problems);
    assert_eq!(l.input[t("cross")], [vk("j"), 0], "cross:nokey skipped, so the later cross:j counts");
    assert_eq!(l.input[t("circle")], [vk("c"), 0]);
    assert_eq!(l.input[t("triangle")], default_layout(KEYBOARD).input[t("triangle")], "Esc is never bound: the default stays");
    assert_eq!(l.input[t("l1")], [vk("x"), vk("a")], "two at most");
    assert_eq!(problems.len(), 5, "{problems:?}");
    // a mistyped value keeps that button's default; `none` still unbinds it
    let mut problems = Vec::new();
    let l = parse(KEYBOARD, "cross:j square:typo circle:none", &mut problems);
    assert_eq!(l.input[t("cross")], [vk("j"), 0]);
    assert_eq!(l.input[t("square")], default_layout(KEYBOARD).input[t("square")]);
    assert_eq!(l.input[t("circle")], [0, 0]);
    assert_eq!(problems.len(), 1, "{problems:?}");
    assert!(problems.iter().all(|p| p.starts_with("keyboard: ")));
    // a controller has no stick directions; a hand-made duplicate: the first one counts
    let mut problems = Vec::new();
    let p = parse(PAD2, "lstick_up:south square:east circle:south", &mut problems);
    assert_eq!(p.input[t("square")], [id("east"), 0]);
    assert_eq!(p.input[t("circle")], [id("south"), 0]);
    assert_eq!(p.input[t("cross")], [0, 0], "south went to circle");
    assert_eq!(problems.len(), 1, "{problems:?}");
    // ...which leaves Controller 2 without Cross: it goes back to its defaults
    let (c, problems) = layouts("", "", "square:east circle:south");
    assert_eq!(c.layouts[PAD2], default_layout(PAD2));
    assert!(problems.last().unwrap().contains("Controller 2 is back to its defaults"), "{problems:?}");
    // each device on its own: the keyboard's Cross doesn't count for Controller 1, nor its for the keyboard
    let (c, problems) = layouts("cross:none", "", "");
    assert_eq!(c, Controls::default());
    assert_eq!(problems, ["keyboard: Cross has no key: Keyboard is back to its defaults"]);
    let (c, problems) = layouts("square:x", "cross:none square:none", "up:none");
    assert_eq!(c.values(), ["cross:space square:x".to_string(), String::new(), String::new()], "only the broken devices");
    assert_eq!(problems, ["pad1: Cross has no button: Controller 1 is back to its defaults",
                          "pad2: Up has no button: Controller 2 is back to its defaults"]);
}

#[test]
fn a_key_swaps() {
    let mut c = Controls::default();
    // X was Cross's alternate: Square takes it, Cross gets Square's Z in that slot
    let out = c.edit(KEYBOARD, Edit::Assign { target: t("square"), slot: 0, input: vk("x") }, 0);
    assert_eq!(out, Outcome::Changed("Swapped Square and Cross".into()));
    assert_eq!(c.layouts[KEYBOARD].input[t("cross")], [vk("space"), vk("z")]);
    assert_eq!(c.layouts[KEYBOARD].input[t("square")], [vk("x"), 0]);
    assert_eq!(c.values()[KEYBOARD], "cross:space,z square:x");
    // the same place again: nothing to do
    assert_eq!(c.edit(KEYBOARD, Edit::Assign { target: t("square"), slot: 0, input: vk("x") }, 0), Outcome::Unchanged);
    // to an empty slot: nothing to swap, so it moves (Cross keeps Space)
    let out = c.edit(KEYBOARD, Edit::Assign { target: t("square"), slot: 1, input: vk("z") }, 0);
    assert_eq!(out, Outcome::Changed("Z moved from Cross to Square".into()));
    assert_eq!(c.layouts[KEYBOARD].input[t("cross")], [vk("space"), 0]);
    assert_eq!(c.layouts[KEYBOARD].input[t("square")], [vk("x"), vk("z")]);
    // the other slot of the same button: they change places
    assert_eq!(c.edit(KEYBOARD, Edit::Assign { target: t("square"), slot: 1, input: vk("x") }, 0), Outcome::Changed(String::new()));
    assert_eq!(c.layouts[KEYBOARD].input[t("square")], [vk("z"), vk("x")]);
    // an alternate without a main is the main
    c.edit(KEYBOARD, Edit::Clear { target: t("square"), slot: 0 }, 0);
    assert_eq!(c.layouts[KEYBOARD].input[t("square")], [vk("x"), 0]);
    c.edit(KEYBOARD, Edit::Clear { target: t("l3"), slot: 0 }, 0);
    c.edit(KEYBOARD, Edit::Assign { target: t("l3"), slot: 1, input: vk("g") }, 0);
    assert_eq!(c.layouts[KEYBOARD].input[t("l3")], [vk("g"), 0]);
    // a stick direction: taking the d-pad's only Left key would leave the keyboard without Left
    let refused = c.edit(KEYBOARD, Edit::Assign { target: t("lstick_left"), slot: 0, input: vk("left") }, 0);
    assert_eq!(refused, Outcome::Refused("Left needs a key".into()));
    assert_eq!(c.edit(KEYBOARD, Edit::Assign { target: t("lstick_left"), slot: 0, input: vk("kp4") }, 0), Outcome::Changed(String::new()));
    let out = c.edit(KEYBOARD, Edit::Assign { target: t("left"), slot: 0, input: vk("kp4") }, 0);
    assert_eq!(out, Outcome::Changed("Swapped Left and Left Stick Left".into()));
    assert_eq!((c.layouts[KEYBOARD].input[t("left")], c.layouts[KEYBOARD].input[t("lstick_left")]),
               ([vk("kp4"), 0], [vk("left"), 0]));
    // the line read back is the same layout
    let mut problems = Vec::new();
    let again = Controls::from_values(c.values().each_ref().map(|s| s.as_str()), &mut problems);
    assert!(problems.is_empty(), "{problems:?}");
    assert_eq!(again, c);
    // Reset
    assert_eq!(c.edit(KEYBOARD, Edit::Reset, 0), Outcome::Changed(String::new()));
    assert_eq!(c, Controls::default());
}

#[test]
fn each_device_keeps_the_menus() {
    let mut c = Controls::default();
    // the keyboard on its own: Cross may lose Space (X is left), not its last key
    assert_eq!(c.edit(KEYBOARD, Edit::Clear { target: t("cross"), slot: 0 }, 0), Outcome::Changed(String::new()));
    assert_eq!(c.layouts[KEYBOARD].input[t("cross")], [vk("x"), 0]);
    assert_eq!(c.edit(KEYBOARD, Edit::Clear { target: t("cross"), slot: 0 }, 0), Outcome::Refused("Cross needs a key".into()));
    assert_eq!(c.layouts[KEYBOARD].input[t("cross")], [vk("x"), 0], "a refused change changes nothing");
    // Controller 1 on its own, though the keyboard has Cross; Controller 2 the same
    let refused = c.edit(PAD1, Edit::Clear { target: t("cross"), slot: 0 }, 1);
    assert_eq!(refused, Outcome::Refused("Cross needs a button".into()));
    assert_eq!(c.layouts[PAD1], default_layout(PAD1));
    assert_eq!(c.edit(PAD2, Edit::Clear { target: t("up"), slot: 0 }, 1), Outcome::Refused("Up needs a button".into()));
    for name in ["triangle", "start", "down", "left", "right"] {
        assert!(matches!(c.edit(PAD2, Edit::Clear { target: t(name), slot: 0 }, 1), Outcome::Refused(_)), "{name}");
    }
    // other buttons may be left with nothing
    assert_eq!(c.edit(PAD2, Edit::Clear { target: t("select"), slot: 0 }, 1), Outcome::Changed(String::new()));
    let out = c.edit(PAD2, Edit::Assign { target: t("square"), slot: 1, input: id("lt") }, 1);
    assert_eq!(out, Outcome::Changed("L2 moved from L2 to Square".into()));
    assert_eq!(c.layouts[PAD2].input[t("l2")], [0, 0]);
    // a move (to an empty slot) that would take an essential button's last input: refused
    let before = c;
    let refused = c.edit(PAD2, Edit::Assign { target: t("circle"), slot: 1, input: id("south") }, 1);
    assert_eq!(refused, Outcome::Refused("Cross needs a button".into()));
    assert_eq!(c, before);
    // an essential button that keeps a second input may give one away; a swap gives it the other's
    let mut c = Controls::default();
    assert_eq!(c.edit(PAD2, Edit::Assign { target: t("cross"), slot: 1, input: id("lb") }, 3),
               Outcome::Changed("L moved from L1 to Cross".into()));
    // R1 switches the settings tabs: its last input stays (finding 157, release review)
    assert_eq!(c.edit(PAD2, Edit::Assign { target: t("square"), slot: 1, input: id("rb") }, 3),
               Outcome::Refused("R1 needs a button".into()));
    assert_eq!(c.edit(PAD2, Edit::Assign { target: t("circle"), slot: 0, input: id("south") }, 3),
               Outcome::Changed("Swapped Circle and Cross".into()));
    assert_eq!((c.layouts[PAD2].input[t("cross")], c.layouts[PAD2].input[t("circle")]), ([id("east"), id("lb")], [id("south"), 0]));
    // what can't be bound
    assert!(matches!(c.edit(PAD1, Edit::Assign { target: t("lstick_up"), slot: 0, input: id("south") }, 0), Outcome::Refused(_)));
    assert!(matches!(c.edit(KEYBOARD, Edit::Assign { target: t("cross"), slot: 0, input: 0x1B }, 0), Outcome::Refused(_)));
    assert!(matches!(c.edit(PAD1, Edit::Assign { target: t("cross"), slot: 0, input: 99 }, 0), Outcome::Refused(_)));
}

/// The owner's edits on the in-game Controls page (a DualSense as Controller 1; finding 157,
/// 2026-11-06), as their dhd.log shows them: under the old rule they left `pad1 = cross:none
/// square:none` and no way to confirm on the controller.
#[test]
fn the_owners_sequence() {
    let mut c = Controls::default();
    let ps = 1; // PAD_KIND_PLAYSTATION
    let assign = |name, input| Edit::Assign { target: t(name), slot: 0, input: id(input) };
    let clear = |name| Edit::Clear { target: t(name), slot: 0 };
    // Square's button onto Cross: they swap
    assert_eq!(c.edit(PAD1, assign("cross", "west"), ps), Outcome::Changed("Swapped Cross and Square".into()));
    assert_eq!(c.values()[PAD1], "cross:west square:south");
    assert_eq!(c.edit(PAD1, assign("square", "south"), ps), Outcome::Unchanged);
    assert_eq!(c.edit(PAD1, clear("square"), ps), Outcome::Changed(String::new()));
    assert_eq!(c.values()[PAD1], "cross:west square:none");
    assert_eq!(c.edit(PAD1, assign("square", "south"), ps), Outcome::Changed(String::new()));
    assert_eq!(c.values()[PAD1], "cross:west square:south");
    // clearing Cross, again and again: refused, though the keyboard has Cross
    for _ in 0..11 {
        assert_eq!(c.edit(PAD1, clear("cross"), ps), Outcome::Refused("Cross needs a button".into()));
    }
    assert_eq!(c.edit(PAD1, clear("square"), ps), Outcome::Changed(String::new()));
    assert_eq!(c.values()[PAD1], "cross:west square:none", "Cross keeps a button");
    assert_eq!(c.layouts[KEYBOARD], default_layout(KEYBOARD));
    // the same on the keyboard: Square's Z onto Cross, then Cross cleared twice
    let mut c = Controls::default();
    let out = c.edit(KEYBOARD, Edit::Assign { target: t("cross"), slot: 0, input: vk("z") }, 0);
    assert_eq!(out, Outcome::Changed("Swapped Cross and Square".into()));
    assert_eq!(c.values()[KEYBOARD], "cross:z,x square:space");
    assert_eq!(c.edit(KEYBOARD, Edit::Clear { target: t("cross"), slot: 0 }, 0), Outcome::Changed(String::new()));
    assert_eq!(c.edit(KEYBOARD, Edit::Clear { target: t("cross"), slot: 0 }, 0), Outcome::Refused("Cross needs a key".into()));
    assert_eq!(c.values()[KEYBOARD], "cross:x square:space");
    // the line the old rule wrote: Controller 1 alone goes back to its defaults at the next start
    let (c, problems) = layouts("", "cross:none square:none", "");
    assert_eq!(c, Controls::default());
    assert_eq!(problems, ["pad1: Cross has no button: Controller 1 is back to its defaults"]);
}

#[test]
fn stick_options() {
    let mut c = Controls::default();
    assert_eq!(c.edit(KEYBOARD, Edit::SwapSticks(true), 0), Outcome::Unchanged, "the keyboard's sticks are keys");
    assert_eq!(c.edit(PAD2, Edit::SwapSticks(true), 0), Outcome::Changed(String::new()));
    assert_eq!(c.edit(PAD2, Edit::Invert(INVERT_LY, true), 0), Outcome::Changed(String::new()));
    assert_eq!(c.edit(PAD2, Edit::Invert(INVERT_RX, true), 0), Outcome::Changed(String::new()));
    assert_eq!(c.edit(PAD2, Edit::Invert(INVERT_RX, false), 0), Outcome::Changed(String::new()));
    assert_eq!(c.edit(PAD2, Edit::Invert(3, true), 0), Outcome::Unchanged, "one axis at a time");
    assert_eq!(c.values(), [String::new(), String::new(), "sticks:swap invert:ly".to_string()]);
    assert_eq!(c.edit(PAD2, Edit::Reset, 0), Outcome::Changed(String::new()));
    assert_eq!(c.values()[PAD2], "");
}
