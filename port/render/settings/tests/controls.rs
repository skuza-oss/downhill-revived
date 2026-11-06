//! The bindings' rules (controls.rs, finding 157): the defaults are the game's old layouts, lines
//! read and written, a moved input, the rule that keeps the menus reachable.

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
    assert!(TARGETS.iter().filter(|t| t.essential).count() == 7);
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
    assert_eq!(l.input[t("cross")], [0, 0], "cross: its one input unknown, so nothing");
    assert_eq!(l.input[t("circle")], [vk("c"), 0]);
    assert_eq!(l.input[t("triangle")], [0, 0], "Esc is never bound");
    assert_eq!(l.input[t("l1")], [vk("x"), vk("a")], "two at most");
    assert_eq!(problems.len(), 6, "{problems:?}");
    assert!(problems.iter().all(|p| p.starts_with("keyboard: ")));
    // a controller has no stick directions; a hand-made duplicate: the first one counts
    let mut problems = Vec::new();
    let p = parse(PAD2, "lstick_up:south square:east circle:south", &mut problems);
    assert_eq!(p.input[t("square")], [id("east"), 0]);
    assert_eq!(p.input[t("circle")], [id("south"), 0]);
    assert_eq!(p.input[t("cross")], [0, 0], "south went to circle");
    assert_eq!(problems.len(), 1, "{problems:?}");
    // ...which leaves player 2 without Cross: Controller 2 goes back to its defaults
    let (c, problems) = layouts("", "", "square:east circle:south");
    assert_eq!(c.layouts[PAD2], default_layout(PAD2));
    assert!(problems.last().unwrap().contains("Controller 2 is back to its defaults"), "{problems:?}");
    // player 1: the keyboard and Controller 1 together
    let (c, problems) = layouts("cross:none", "", "");
    assert!(problems.is_empty());
    assert_eq!(c.layouts[KEYBOARD].input[t("cross")], [0, 0]);
    let (c, problems) = layouts("cross:none", "cross:none", "");
    assert_eq!(c.layouts[KEYBOARD], default_layout(KEYBOARD));
    assert_eq!(c.layouts[PAD1].input[t("cross")], [0, 0]);
    assert_eq!(problems.len(), 1, "{problems:?}");
}

#[test]
fn a_key_moves() {
    let mut c = Controls::default();
    let out = c.edit(KEYBOARD, Edit::Assign { target: t("square"), slot: 0, input: vk("x") }, 0);
    assert_eq!(out, Outcome::Changed("X moved from Cross to Square".into()));
    assert_eq!(c.layouts[KEYBOARD].input[t("cross")], [vk("space"), 0]);
    assert_eq!(c.layouts[KEYBOARD].input[t("square")], [vk("x"), 0], "Z is left without a button");
    assert_eq!(c.values()[KEYBOARD], "cross:space square:x");
    // the same place again: nothing to do; the other slot of the same button: they change places
    assert_eq!(c.edit(KEYBOARD, Edit::Assign { target: t("square"), slot: 0, input: vk("x") }, 0), Outcome::Unchanged);
    c.edit(KEYBOARD, Edit::Assign { target: t("square"), slot: 1, input: vk("z") }, 0);
    assert_eq!(c.layouts[KEYBOARD].input[t("square")], [vk("x"), vk("z")]);
    assert_eq!(c.edit(KEYBOARD, Edit::Assign { target: t("square"), slot: 1, input: vk("x") }, 0), Outcome::Changed(String::new()));
    assert_eq!(c.layouts[KEYBOARD].input[t("square")], [vk("z"), vk("x")]);
    // an alternate without a main is the main
    c.edit(KEYBOARD, Edit::Clear { target: t("square"), slot: 0 }, 0);
    assert_eq!(c.layouts[KEYBOARD].input[t("square")], [vk("x"), 0]);
    c.edit(KEYBOARD, Edit::Clear { target: t("l3"), slot: 0 }, 0);
    c.edit(KEYBOARD, Edit::Assign { target: t("l3"), slot: 1, input: vk("g") }, 0);
    assert_eq!(c.layouts[KEYBOARD].input[t("l3")], [vk("g"), 0]);
    // a stick direction on the keyboard
    let out = c.edit(KEYBOARD, Edit::Assign { target: t("lstick_left"), slot: 0, input: vk("left") }, 0);
    assert_eq!(out, Outcome::Changed("Left moved from Left to Left Stick Left".into()));
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
fn menus_stay_reachable() {
    let mut c = Controls::default();
    // player 1: the keyboard may lose Cross while Controller 1 has it, not both
    assert!(matches!(c.edit(KEYBOARD, Edit::Clear { target: t("cross"), slot: 0 }, 0), Outcome::Changed(_)));
    assert!(matches!(c.edit(KEYBOARD, Edit::Clear { target: t("cross"), slot: 0 }, 0), Outcome::Changed(_)));
    assert_eq!(c.edit(KEYBOARD, Edit::Clear { target: t("cross"), slot: 0 }, 0), Outcome::Unchanged);
    let refused = c.edit(PAD1, Edit::Assign { target: t("square"), slot: 0, input: id("south") }, 2);
    assert_eq!(refused, Outcome::Refused("Not changed: Cross would have no key and no button".into()));
    assert_eq!(c.layouts[PAD1], default_layout(PAD1), "a refused change changes nothing");
    // player 2's controller on its own; the message names the pad's own button
    let refused = c.edit(PAD2, Edit::Clear { target: t("up"), slot: 0 }, 1);
    assert_eq!(refused, Outcome::Refused("Not changed: Up would have no button on Controller 2".into()));
    let out = c.edit(PAD2, Edit::Assign { target: t("square"), slot: 1, input: id("lt") }, 1);
    assert_eq!(out, Outcome::Changed("L2 moved from L2 to Square".into()));
    // an essential button that keeps a second input may give one away
    let mut c = Controls::default();
    assert!(matches!(c.edit(PAD2, Edit::Assign { target: t("cross"), slot: 1, input: id("rb") }, 3), Outcome::Changed(_)));
    assert_eq!(c.edit(PAD2, Edit::Assign { target: t("circle"), slot: 0, input: id("south") }, 3),
               Outcome::Changed("B moved from Cross to Circle".into()));
    assert_eq!(c.layouts[PAD2].input[t("cross")], [id("rb"), 0]);
    // what can't be bound
    assert!(matches!(c.edit(PAD1, Edit::Assign { target: t("lstick_up"), slot: 0, input: id("south") }, 0), Outcome::Refused(_)));
    assert!(matches!(c.edit(KEYBOARD, Edit::Assign { target: t("cross"), slot: 0, input: 0x1B }, 0), Outcome::Refused(_)));
    assert!(matches!(c.edit(PAD1, Edit::Assign { target: t("cross"), slot: 0, input: 99 }, 0), Outcome::Refused(_)));
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
