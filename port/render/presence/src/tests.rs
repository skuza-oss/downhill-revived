//! The state -> Discord payload mapping and the pacing, without Discord.

use super::*;
use std::time::{Duration, Instant};

fn state(kind: Kind, track: &str, place: u32, riders: u32) -> State {
    State { kind, track: track.into(), place, riders, ..State::default() }
}

fn room(code: &str, members: u32, max_members: u32) -> State {
    State { kind: Kind::Room, track: "ALPINE MX".into(), room: code.into(), members, max_members, ..State::default() }
}

fn lines(s: &State) -> Option<(String, Option<String>)> {
    payload(s, 7).map(|p| (p.details, p.state))
}

fn pair(details: &str, state: Option<&str>) -> Option<(String, Option<String>)> {
    Some((details.to_string(), state.map(str::to_string)))
}

fn json(p: &Payload) -> serde_json::Value {
    serde_json::to_value(worker::activity(p)).unwrap()
}

#[test]
fn the_lines_of_each_state() {
    assert_eq!(lines(&State::default()), None);
    assert_eq!(lines(&state(Kind::Menus, "ALPINE", 2, 6)), pair("In menus", None));
    assert_eq!(lines(&state(Kind::Race, "ALPINE MX", 2, 6)), pair("Racing: Alpine MX", Some("2nd of 6")));
    assert_eq!(lines(&state(Kind::Career, "PERU TDH", 1, 10)), pair("Career: Peru TDH", Some("1st of 10")));
    assert_eq!(lines(&state(Kind::OnlineRace, "BC TDX", 3, 4)), pair("Online race: BC TDX", Some("3rd of 4")));
    assert_eq!(lines(&state(Kind::Movie, "", 0, 0)), pair("Watching a movie", None));
}

#[test]
fn the_rider_beside_the_place() {
    // index + 1: 1 Cosmo, 7 Eric Carter, 16 Kineticlops; 0 and past the riders: none
    assert_eq!(rider_name(1), Some("Cosmo"));
    assert_eq!(rider_name(7), Some("Eric Carter"));
    assert_eq!(rider_name(16), Some("Kineticlops"));
    assert_eq!(rider_name(0), None);
    assert_eq!(rider_name(17), None);
    let race = State { rider: 1, ..state(Kind::Race, "ALPINE MX", 2, 6) };
    assert_eq!(lines(&race), pair("Racing: Alpine MX", Some("Cosmo \u{b7} 2nd of 6")));
    // the countdown (no place yet): the rider alone; no rider known: the place alone
    assert_eq!(lines(&State { place: 0, ..race.clone() }), pair("Racing: Alpine MX", Some("Cosmo")));
    assert_eq!(lines(&State { rider: 0, ..race.clone() }), pair("Racing: Alpine MX", Some("2nd of 6")));
    let online = State { kind: Kind::OnlineRace, rider: 12, ..race };
    assert_eq!(lines(&online), pair("Online race: Alpine MX", Some("Missy Giove \u{b7} 2nd of 6")));
}

#[test]
fn public_and_private_rooms() {
    // a public room: its track, its code, and the party's size, which Discord shows as "(3 of 6)"
    let public = room("K7M2QX", 3, 6);
    let p = payload(&public, 0).unwrap();
    assert_eq!((p.details.as_str(), p.state.as_deref()), ("Online room: Alpine MX", Some("Room K7M2QX")));
    assert_eq!(p.party, Some([3, 6]));
    assert_eq!(json(&p)["party"]["size"], serde_json::json!([3, 6]));
    // Discord refuses a party size without an id of 2 to 128 characters: this session's own token,
    // the same for every update and nothing of the room's (finding 136: it must not be the join
    // secret either, which Discord refuses -- code 5005).
    let id = json(&p)["party"]["id"].as_str().unwrap().to_string();
    assert!(id.starts_with("dhd-") && id.len() == 20, "{id}");
    assert!(!id.contains("K7M2QX"));
    assert_eq!(party_id(), id);
    assert_ne!(id, join_secret(&public).unwrap());
    // a private room comes without its code: the riders, and the code nowhere in the activity
    let private = State { room: String::new(), ..public.clone() };
    assert_eq!(lines(&private), pair("Online room: Alpine MX", Some("Private room")));
    let private_json = json(&payload(&private, 0).unwrap());
    let text = private_json.to_string();
    assert!(!text.contains("K7M2QX"), "{text}");
    assert!(text.contains("[3,6]"), "{text}");
    // even with Ask to Join on, a private room's code (empty here) never reaches its party id
    let private_id = private_json["party"]["id"].as_str().unwrap();
    assert!(private_id.starts_with("dhd-") && private_id.len() == 20, "{private_id}");
    // the room's track not known: the kind alone
    assert_eq!(lines(&State { track: String::new(), ..private.clone() }), pair("Online room", Some("Private room")));
    // the room's size not known yet: no party
    let unknown = payload(&State { max_members: 0, ..private.clone() }, 0).unwrap();
    assert_eq!(unknown.party, None);
    assert!(json(&unknown).get("party").is_none());
    // a count past the size, or none, stays inside it (Discord's size is current, then maximum)
    assert_eq!(payload(&room("", 9, 8), 0).unwrap().party, Some([8, 8]));
    assert_eq!(payload(&room("", 0, 4), 0).unwrap().party, Some([1, 4]));
    // no race kind has a party
    assert_eq!(payload(&state(Kind::OnlineRace, "BC", 2, 4), 0).unwrap().party, None);
}

#[test]
fn the_join_secret() {
    // a public room with a free place: its code after the format's name
    assert_eq!(join_secret(&room("K7M2QX", 3, 6)).as_deref(), Some("dhd-room-1:K7M2QX"));
    // full, private, or not a room: none
    assert_eq!(join_secret(&room("K7M2QX", 6, 6)), None);
    assert_eq!(join_secret(&room("", 3, 6)), None);
    assert_eq!(join_secret(&State { room: "K7M2QX".into(), ..state(Kind::OnlineRace, "BC", 2, 4) }), None);
    // with Ask to Join on (finding 136), a joinable room carries the secret; a build with it off
    // sends neither the secret nor a party id
    let p = payload(&room("K7M2QX", 3, 6), 0).unwrap();
    assert_eq!(p.join.is_some(), ASK_TO_JOIN);
    if !ASK_TO_JOIN {
        assert!(json(&p).get("secrets").is_none());
        assert!(!json(&p)["party"]["id"].as_str().unwrap().contains("K7M2QX"));
    }
    // with a secret, the activity carries it (secrets.join) with the party's size and this session's
    // own id -- never the secret itself: Discord refuses an id matching the secret (code 5005)
    let joinable = Payload { join: Some("dhd-room-1:K7M2QX".into()), ..p.clone() };
    let j = json(&joinable);
    assert_eq!(j["secrets"], serde_json::json!({"join": "dhd-room-1:K7M2QX"}));
    assert_eq!(j["party"]["size"], serde_json::json!([3, 6]));
    assert_ne!(j["party"]["id"], "dhd-room-1:K7M2QX");
    assert_eq!(j["party"]["id"], json(&p)["party"]["id"]);
}

#[test]
fn the_presence_is_harmless_without_discord() {
    // The switch off, or unset (the tools' runs: the built-in value is off): the C entry point the
    // host calls starts nothing, so a PC with no Discord is unaffected however the game is built.
    assert_eq!(unsafe { c::dhd_presence_start(std::ptr::null(), None) }, 0);
    let off = std::ffi::CString::new("off").unwrap();
    assert_eq!(unsafe { c::dhd_presence_start(off.as_ptr(), None) }, 0);
    // The Social SDK is only ever a library beside the game (sdk.rs), loaded at run time: nothing
    // links against it, and one that isn't there is an Err (the worker keeps to the IPC), never a
    // build or start failure (finding 136).
    assert!(sdk::library_path().is_some(), "the SDK's path is named, not required");
    assert!(sdk::Client::load(std::path::Path::new("no-such-discord-sdk.dll"), false, None).is_err());
}

#[test]
fn the_join_reaches_only_the_sdk() {
    // a joinable public room: the SDK path (payload) carries the secret (secrets.join), with a party
    // id that is this session's own token, distinct from the secret (Discord refuses code 5005)
    let p = payload(&room("K7M2QX", 3, 6), 0).unwrap();
    assert!(p.join.is_some());
    assert_eq!(json(&p)["secrets"], serde_json::json!({"join": "dhd-room-1:K7M2QX"}));
    assert_ne!(json(&p)["party"]["id"], "dhd-room-1:K7M2QX");
    // the plain IPC drops it: no secret, this session's own party id, both buttons (finding 136)
    let ipc = worker::for_ipc(Some(p)).unwrap();
    assert_eq!(ipc.join, None);
    let ipc_json = json(&ipc);
    assert!(ipc_json.get("secrets").is_none(), "{ipc_json}");
    assert!(!ipc_json["party"]["id"].as_str().unwrap().contains("K7M2QX"), "{ipc_json}");
    assert_eq!(ipc_json["party"]["size"], serde_json::json!([3, 6]));
    assert_eq!(ipc_json["buttons"], json(&payload(&room("K7M2QX", 3, 6), 0).unwrap())["buttons"]);
    // clearing (None) and a race go through unchanged
    assert_eq!(worker::for_ipc(None), None);
}

#[test]
fn a_place_only_when_it_makes_sense() {
    // not known yet, alone on the track, or past the riders: no second line
    assert_eq!(lines(&state(Kind::Race, "MOAB", 0, 6)), pair("Racing: Moab", None));
    assert_eq!(lines(&state(Kind::Race, "MOAB", 1, 1)), pair("Racing: Moab", None));
    assert_eq!(lines(&state(Kind::Race, "MOAB", 7, 6)), pair("Racing: Moab", None));
    // no track name: the kind alone
    assert_eq!(lines(&state(Kind::Race, "", 2, 6)), pair("Racing", Some("2nd of 6")));
}

#[test]
fn names_and_places() {
    assert_eq!(track_name("ALPINE"), "Alpine");
    assert_eq!(track_name("GLACIER X"), "Glacier X");
    assert_eq!(track_name("AUBER STD"), "Auber STD");
    assert_eq!(track_name("CITY"), "City");
    assert_eq!(track_name("BC"), "BC");
    assert_eq!(track_name("  "), "");
    let words: Vec<_> = [1, 2, 3, 4, 10, 11, 12, 13, 21, 22, 23, 101, 111].iter().map(|&n| ordinal(n)).collect();
    assert_eq!(words, ["1st", "2nd", "3rd", "4th", "10th", "11th", "12th", "13th", "21st", "22nd", "23rd", "101st", "111th"]);
}

#[test]
fn the_elapsed_time() {
    let mut clock = Clock::new(1000);
    assert_eq!(clock.start(&state(Kind::Menus, "", 0, 0), 5000), 1000);
    // a race counts from its start; a new place keeps it, a new track or kind starts again
    assert_eq!(clock.start(&state(Kind::Race, "ALPINE", 0, 6), 6000), 6000);
    assert_eq!(clock.start(&state(Kind::Race, "ALPINE", 3, 6), 9000), 6000);
    assert_eq!(clock.start(&state(Kind::Race, "MOAB", 3, 6), 9500), 9500);
    assert_eq!(clock.start(&state(Kind::Career, "MOAB", 3, 6), 9700), 9700);
    // back in the menus: the session's time; the next race starts again
    assert_eq!(clock.start(&state(Kind::Menus, "", 0, 0), 12000), 1000);
    assert_eq!(clock.start(&state(Kind::Career, "MOAB", 0, 6), 13000), 13000);
    assert_eq!(payload(&state(Kind::Menus, "", 0, 0), 1000).unwrap().start, 1000);
}

#[test]
fn try_again_starts_the_time_again() {
    let mut clock = Clock::new(0);
    // the load, the flight and the countdown (no place), the race (a place): one start
    assert_eq!(clock.start(&state(Kind::Race, "ALPINE", 0, 6), 1000), 1000);
    assert_eq!(clock.start(&state(Kind::Race, "ALPINE", 6, 6), 9000), 1000);
    assert_eq!(clock.start(&state(Kind::Race, "ALPINE", 2, 6), 60000), 1000);
    // the results (no place) keep it
    assert_eq!(clock.start(&state(Kind::Race, "ALPINE", 0, 6), 90000), 1000);
    // TRY AGAIN: the countdown keeps it, the race's first place starts the new run
    assert_eq!(clock.start(&state(Kind::Race, "ALPINE", 0, 6), 95000), 1000);
    assert_eq!(clock.start(&state(Kind::Race, "ALPINE", 6, 6), 99000), 99000);
    assert_eq!(clock.start(&state(Kind::Race, "ALPINE", 4, 6), 120000), 99000);
}

#[test]
fn only_changes_and_at_most_one_update_per_gap() {
    let t0 = Instant::now();
    let at = |s: u64| t0 + Duration::from_secs(s);
    let p = |details: &str| Some(Payload { details: details.into(), ..Payload::default() });
    let mut pacer = Pacer::default();
    assert_eq!(pacer.due(at(0)), Due::Nothing);
    // the first update goes at once
    pacer.want(p("In menus"));
    assert_eq!(pacer.due(at(0)), Due::Send(p("In menus")));
    pacer.sent(p("In menus"), at(0));
    // nothing new: only the refresh, a minute after the update
    assert_eq!(pacer.due(at(1)), Due::Wait(Duration::from_secs(59)));
    // the same again: nothing
    pacer.want(p("In menus"));
    assert_eq!(pacer.due(at(2)), Due::Wait(Duration::from_secs(58)));
    // changes inside the gap wait for it, the last one wins
    pacer.want(p("Racing: Alpine"));
    assert_eq!(pacer.due(at(5)), Due::Wait(Duration::from_secs(10)));
    pacer.want(p("Career: Alpine"));
    assert_eq!(pacer.due(at(14)), Due::Wait(Duration::from_secs(1)));
    assert_eq!(pacer.due(at(15)), Due::Send(p("Career: Alpine")));
    pacer.sent(p("Career: Alpine"), at(15));
    // a change and back inside the gap: nothing to send
    pacer.want(p("In menus"));
    pacer.want(p("Career: Alpine"));
    assert_eq!(pacer.due(at(20)), Due::Wait(Duration::from_secs(55)));
    // unchanged for a minute: it goes again (a failed write tells a Discord that went away)
    assert_eq!(pacer.due(at(75)), Due::Send(p("Career: Alpine")));
    pacer.sent(p("Career: Alpine"), at(75));
    // clearing is a change too; nothing shown, nothing to refresh
    pacer.want(None);
    assert_eq!(pacer.due(at(95)), Due::Send(None));
    pacer.sent(None, at(95));
    assert_eq!(pacer.due(at(96)), Due::Nothing);
    assert_eq!(pacer.due(at(500)), Due::Nothing);
    // after a lost connection Discord shows nothing: the wanted state goes again
    pacer.want(p("In menus"));
    pacer.sent(p("In menus"), at(600));
    pacer.lost();
    assert_eq!(pacer.shown(), None);
    assert_eq!(pacer.due(at(620)), Due::Send(p("In menus")));
}

#[test]
fn the_setting() {
    // unset: the built-in value (the tools' runs, without dhd.ini) is off; dhd.ini's default is on
    assert!(!enabled(None));
    assert!(!enabled(Some("")));
    assert_eq!(dhd_settings::table::find("discord").map(|s| s.default), Some("on"));
    for on in ["on", "ON", "1", "yes", "true", "anything"] {
        assert!(enabled(Some(on)), "{on}");
    }
    for off in ["off", "OFF", "0", "no", "false"] {
        assert!(!enabled(Some(off)), "{off}");
    }
}

#[test]
fn the_activity_discord_gets() {
    let p = Payload {
        details: "Racing: Alpine".into(),
        state: Some("2nd of 6".into()),
        start: 1_700_000_000_000,
        ..Payload::default()
    };
    let json = json(&p);
    assert_eq!(json["details"], "Racing: Alpine");
    assert_eq!(json["state"], "2nd of 6");
    assert_eq!(json["timestamps"]["start"], 1_700_000_000_000i64);
    assert_eq!(json["assets"]["large_image"], "logo");
    assert_eq!(json["assets"]["large_text"], "Downhill Revived");
    assert_eq!(
        json["buttons"],
        serde_json::json!([
            {"label": "Get Downhill Revived", "url": "https://downhill-revived.pages.dev/"},
            {"label": "Join the Discord", "url": "https://discord.gg/XrSeTyzfr2"}
        ])
    );
    // a race has no party and no secrets
    assert!(json.get("party").is_none() && json.get("secrets").is_none());
    // no second line: no state key at all (Discord refuses an empty one)
    let menus = Payload { details: "In menus".into(), ..Payload::default() };
    assert!(serde_json::to_value(worker::activity(&menus)).unwrap().get("state").is_none());
}

#[test]
fn a_join_from_discord() {
    // only this game's secrets, with a code the server could have made
    assert_eq!(join_code("dhd-room-1:K7M2QX"), Some("K7M2QX"));
    assert_eq!(join_code("dhd-room-1:K7M2Q"), None);
    assert_eq!(join_code("dhd-room-1:K7M2QXX"), None);
    assert_eq!(join_code("dhd-room-1:K7M2Q1"), None, "1 is not a code letter");
    assert_eq!(join_code("dhd-room-1:k7m2qx"), None);
    assert_eq!(join_code("dhd-room-2:K7M2QX"), None);
    assert_eq!(join_code("K7M2QX"), None);
    assert_eq!(join_code(&join_secret(&room("K7M2QX", 3, 6)).unwrap()), Some("K7M2QX"));
    // the code waits for the game, which takes it once
    let lines = std::cell::RefCell::new(Vec::new());
    let log = |line: &str| lines.borrow_mut().push(line.to_string());
    joined("something else", &log);
    assert_eq!(take_join(), None);
    joined("dhd-room-1:K7M2QX", &log);
    assert_eq!(take_join().as_deref(), Some("K7M2QX"));
    assert_eq!(take_join(), None);
    assert_eq!(lines.borrow().len(), 2);
    // the C side: too small a buffer takes nothing
    joined("dhd-room-1:ABC234", &log);
    let mut out = [0 as std::ffi::c_char; 8];
    assert_eq!(unsafe { c::dhd_presence_take_join(out.as_mut_ptr(), 6) }, 0);
    assert_eq!(unsafe { c::dhd_presence_take_join(out.as_mut_ptr(), 8) }, 1);
    assert_eq!(unsafe { std::ffi::CStr::from_ptr(out.as_ptr()) }.to_str(), Ok("ABC234"));
    assert_eq!(unsafe { c::dhd_presence_take_join(out.as_mut_ptr(), 8) }, 0);
}

/// Against the real SDK and a running Discord, by hand (finding 136):
/// DHD_DISCORD_SDK_TEST=<path of discord_partner_sdk.dll> cargo test -p dhd_presence -- --ignored --nocapture
/// Shows a room for a few seconds on this PC's Discord profile, then clears it.
#[test]
#[ignore]
fn the_sdk_against_discord() {
    let path = std::env::var("DHD_DISCORD_SDK_TEST").expect("DHD_DISCORD_SDK_TEST: the SDK library's path");
    let mut sdk = sdk::Client::load(std::path::Path::new(&path), false, None).expect("the SDK loads");
    let p = payload(&State { rider: 1, ..room("K7M2QX", 3, 6) }, 1_700_000_000_000).unwrap();
    sdk.set(Some(&p));
    let mut outcome = None;
    for _ in 0..100 {
        sdk.run_callbacks();
        let e = sdk.take_events();
        for line in e.lines {
            println!("sdk: {line}");
        }
        if e.update.is_some() {
            outcome = e.update;
            break;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    println!("update: {outcome:?}");
    std::thread::sleep(Duration::from_secs(5));
    sdk.set(None);
    for _ in 0..10 {
        sdk.run_callbacks();
        std::thread::sleep(Duration::from_millis(100));
    }
    assert_eq!(outcome, Some(Ok(())));
}

/// The same through the plain IPC (no SDK), by hand: cargo test -p dhd_presence -- --ignored --nocapture
#[test]
#[ignore]
fn the_ipc_against_discord() {
    use discord_rich_presence::{DiscordIpc, DiscordIpcClient};
    let mut client = DiscordIpcClient::new(APP_ID);
    client.connect().expect("Discord is running");
    let p = payload(&room("K7M2QX", 3, 6), 1_700_000_000_000).unwrap();
    client.set_activity(worker::activity(&p)).unwrap();
    let (_, answer) = client.recv().unwrap();
    println!("answer: {answer}");
    std::thread::sleep(Duration::from_secs(5));
    client.clear_activity().unwrap();
    let _ = client.recv();
    let _ = client.close();
    assert_ne!(answer["evt"], "ERROR", "{answer}");
}
