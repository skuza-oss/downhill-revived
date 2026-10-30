//! Discord Rich Presence (finding 106): what the player is doing, on their Discord profile.
//!
//! The game core works out the state (port/hle/presence.c) and sends it through
//! `host_service_presence` when it changes; port/hle/host.c hands it here (`c.rs`). A thread of its
//! own (`worker.rs`) talks to Discord's local IPC through the discord-rich-presence crate: a named
//! pipe on Windows, a Unix socket on macOS and Linux. No Discord SDK. Discord running or not, the
//! player sees nothing of it but their profile: no error box, one log line at most while Discord
//! isn't there, a quiet retry every 30 s.
//!
//! This file is the part without Discord: the state, the two lines, the party's size and the
//! elapsed time Discord shows for it (`payload`), and the pacing (`Pacer`: only a change is sent, at
//! most one update per 15 s, which is about what Discord allows; what is shown goes again every
//! minute, which is how a Discord restarted since is noticed) (finding 121).

use std::sync::Mutex;
use std::time::{Duration, Instant};

mod c;
pub mod sdk;
mod worker;

pub use worker::{start, Worker};

/// The Discord application (public: every Rich Presence client names its own).
pub const APP_ID: &str = "1564742442857799934";
/// The large image (the art asset uploaded to the application) and its hover text.
pub const LOGO: &str = "logo";
pub const LOGO_TEXT: &str = "Downhill Revived";
/// The two buttons other people see on the profile.
pub const BUTTONS: [(&str, &str); 2] = [
    ("Get Downhill Revived", "https://downhill-revived.pages.dev/"),
    ("Join the Discord", "https://discord.gg/XrSeTyzfr2"),
];
/// The shortest time between two updates, and between two tries to reach Discord.
pub const MIN_GAP: Duration = Duration::from_secs(15);
pub const RETRY: Duration = Duration::from_secs(30);
/// How often an unchanged presence goes again: a Discord that quit or restarted since shows up as a
/// failed write, and the presence comes back once it is there (RETRY).
pub const REFRESH: Duration = Duration::from_secs(60);

/// Ask to Join (finding 121): a public room's join secret, which puts a Join button on the profile.
/// Off: the game can't take a join from Discord yet (nothing reads Discord's ACTIVITY_JOIN), and a
/// button that does nothing is worse than none.
pub const ASK_TO_JOIN: bool = false;
/// What a join secret starts with: its format, for the game that takes it.
pub const JOIN_PREFIX: &str = "dhd-room-1:";
/// A join code's letters and length (the server's, port/hle/online_menu.c's code_letters).
pub const CODE_LETTERS: &str = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
pub const CODE_LENGTH: usize = 6;

/// The game's riders by index (port/hle/online.c's rider_name).
pub const RIDERS: [&str; 16] = [
    "Cosmo", "Mai", "Ajax", "Jonnie", "Kalolo", "T-bag", "Eric Carter", "Tara Llanes", "Drud", "Brian Lopes",
    "Rakel", "Missy Giove", "Richie Schley", "Jen", "Woody", "Kineticlops",
];

/// What the player is doing: port/host/host_api.h's PRESENCE_*, in that order.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Kind {
    /// Nothing to show (the presence is cleared).
    #[default]
    None,
    Menus,
    /// A race outside the career (single event and the other modes).
    Race,
    Career,
    /// In an online room, before its race.
    Room,
    OnlineRace,
    Movie,
}

impl Kind {
    pub fn from_c(kind: i32) -> Kind {
        match kind {
            1 => Kind::Menus,
            2 => Kind::Race,
            3 => Kind::Career,
            4 => Kind::Room,
            5 => Kind::OnlineRace,
            6 => Kind::Movie,
            _ => Kind::None,
        }
    }

    fn racing(self) -> bool {
        matches!(self, Kind::Race | Kind::Career | Kind::OnlineRace)
    }
}

/// The state as the core sends it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct State {
    pub kind: Kind,
    /// The track as the game names it ("ALPINE MX"), of the race or the room; empty when not known.
    pub track: String,
    /// A race: the player's place (1 first; 0 not known), the riders in it, and the player's rider
    /// (the game's index + 1; 0 not known).
    pub place: u32,
    pub riders: u32,
    pub rider: u32,
    /// An online room: its join code (empty for a private room), the players in it and how many it
    /// takes.
    pub room: String,
    pub members: u32,
    pub max_members: u32,
}

/// What Discord shows: the first line, the second, and the start of the elapsed time (Unix ms); a
/// room's party size (players, room size: Discord adds "(3 of 6)" to the second line) and, with
/// ASK_TO_JOIN, its join secret.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Payload {
    pub details: String,
    pub state: Option<String>,
    pub start: i64,
    pub party: Option<[i32; 2]>,
    pub join: Option<String>,
}

/// "ALPINE MX" as "Alpine MX": a word of four letters or more in title case, a shorter one (MX,
/// TDH, BC) as it is.
pub fn track_name(name: &str) -> String {
    name.split_whitespace()
        .map(|w| {
            if w.chars().count() < 4 {
                return w.to_string();
            }
            let mut c = w.chars();
            let first = c.next().map(|f| f.to_ascii_uppercase()).into_iter();
            first.chain(c.map(|x| x.to_ascii_lowercase())).collect()
        })
        .collect::<Vec<_>>()
        .join(" ")
}

/// 1st, 2nd, 3rd, 4th ... 11th, 12th, 13th ... 21st.
pub fn ordinal(n: u32) -> String {
    let suffix = match (n % 10, n % 100) {
        (_, 11..=13) => "th",
        (1, _) => "st",
        (2, _) => "nd",
        (3, _) => "rd",
        _ => "th",
    };
    format!("{n}{suffix}")
}

/// The rider of the game's index + 1 (State::rider); None for 0 or out of range.
pub fn rider_name(rider: u32) -> Option<&'static str> {
    RIDERS.get(rider.checked_sub(1)? as usize).copied()
}

/// The join secret of a public room with a free place: its code after JOIN_PREFIX. None otherwise
/// (a private room comes without its code, a full one can't be joined).
pub fn join_secret(s: &State) -> Option<String> {
    (s.kind == Kind::Room && !s.room.is_empty() && s.members < s.max_members).then(|| format!("{JOIN_PREFIX}{}", s.room))
}

/// The party's id Discord asks for with a party size (2 to 128 characters, else it refuses the
/// update): the join secret when there is one, otherwise this session's own random token, which
/// says nothing of the room (a private room's code must not leave the core).
pub fn party_id(join: Option<&str>) -> String {
    use std::hash::{BuildHasher, Hasher};
    static TOKEN: std::sync::OnceLock<String> = std::sync::OnceLock::new();
    if let Some(join) = join {
        return join.to_string();
    }
    TOKEN
        .get_or_init(|| {
            // RandomState's keys are random per process
            let mut h = std::collections::hash_map::RandomState::new().build_hasher();
            h.write_u32(std::process::id());
            format!("dhd-{:016x}", h.finish())
        })
        .clone()
}

/// The room's code in a join secret Discord handed over; None for anything this game didn't make.
pub fn join_code(secret: &str) -> Option<&str> {
    let code = secret.strip_prefix(JOIN_PREFIX)?;
    (code.len() == CODE_LENGTH && code.chars().all(|c| CODE_LETTERS.contains(c))).then_some(code)
}

/// A friend's join from Discord, until the game takes it (`take_join`).
static JOIN: Mutex<Option<String>> = Mutex::new(None);

/// Discord handed over `secret` (the SDK's join callback): its code waits for the game.
pub fn joined(secret: &str, log: &dyn Fn(&str)) {
    match join_code(secret) {
        Some(code) => {
            log("discord: a join from Discord; it waits for the ONLINE page");
            *JOIN.lock().unwrap_or_else(|e| e.into_inner()) = Some(code.to_string());
        }
        None => log("discord: a join from Discord with a secret this game didn't make; ignored"),
    }
}

/// The join code waiting from Discord, taken (at most once).
pub fn take_join() -> Option<String> {
    JOIN.lock().unwrap_or_else(|e| e.into_inner()).take()
}

/// The two lines for `s`, its elapsed time counted from `start`. None: nothing to show.
pub fn payload(s: &State, start: i64) -> Option<Payload> {
    let track = track_name(&s.track);
    let on = |what: &str| if track.is_empty() { what.to_string() } else { format!("{what}: {track}") };
    let place = (s.place > 0 && s.riders > 1 && s.place <= s.riders).then(|| format!("{} of {}", ordinal(s.place), s.riders));
    // a race's second line: the rider and the place joined by a middle dot ("Cosmo . 2nd of 6"),
    // or either alone
    let riding: Vec<String> = [rider_name(s.rider).map(str::to_string), place].into_iter().flatten().collect();
    let riding = (!riding.is_empty()).then(|| riding.join(" \u{b7} "));
    let mut p = Payload { start, ..Payload::default() };
    (p.details, p.state) = match s.kind {
        Kind::None => return None,
        Kind::Menus => ("In menus".to_string(), None),
        Kind::Race => (on("Racing"), riding),
        Kind::Career => (on("Career"), riding),
        Kind::OnlineRace => (on("Online race"), riding),
        // the room's track, then the room: a public one by its code, a private one comes without
        // it (port/hle/presence.c keeps it); Discord adds the party's size, "Room K7M2QX (3 of 6)"
        Kind::Room => {
            if s.max_members > 0 {
                p.party = Some([s.members.clamp(1, s.max_members) as i32, s.max_members as i32]);
            }
            p.join = join_secret(s).filter(|_| ASK_TO_JOIN);
            let room = if s.room.is_empty() { "Private room".to_string() } else { format!("Room {}", s.room) };
            (on("Online room"), Some(room))
        }
        Kind::Movie => ("Watching a movie".to_string(), None),
    };
    Some(p)
}

/// Where the elapsed time starts: the session's start, and for a race the moment it began (a new
/// race kind or track; a place changing keeps it), or the moment its next run began (TRY AGAIN).
#[derive(Debug)]
pub struct Clock {
    session: i64,
    race: Option<Run>,
}

/// The race the clock counts: its kind and track, its start, and whether the place was seen in this
/// run (`placed`) or seen and gone since (`over`: the results, or the countdown of TRY AGAIN).
#[derive(Debug)]
struct Run {
    kind: Kind,
    track: String,
    start: i64,
    placed: bool,
    over: bool,
}

impl Clock {
    pub fn new(session: i64) -> Clock {
        Clock { session, race: None }
    }

    pub fn start(&mut self, s: &State, now: i64) -> i64 {
        if !s.kind.racing() {
            self.race = None;
            return self.session;
        }
        if !matches!(&self.race, Some(r) if r.kind == s.kind && r.track == s.track) {
            let track = s.track.clone();
            self.race = Some(Run { kind: s.kind, track, start: now, placed: false, over: false });
        }
        let run = self.race.as_mut().expect("set above");
        // the place shows only while the riders race (presence.c): back after it went, the same
        // track is ridden again, and that run counts from its start
        if s.place > 0 {
            if run.over {
                run.start = now;
                run.over = false;
            }
            run.placed = true;
        } else if run.placed {
            run.placed = false;
            run.over = true;
        }
        run.start
    }
}

/// What the worker does next.
#[derive(Debug, PartialEq, Eq)]
pub enum Due {
    /// Discord shows what it should.
    Nothing,
    /// Send this (None: clear).
    Send(Option<Payload>),
    /// Nothing before this long: a change waits for the gap since the last update, what is shown
    /// for its refresh.
    Wait(Duration),
}

/// Only a change is sent, at most one per MIN_GAP; changes in between are merged (the last wins).
/// What is shown goes again REFRESH after the last update, so a lost connection is noticed.
#[derive(Debug, Default)]
pub struct Pacer {
    wanted: Option<Payload>,
    shown: Option<Payload>,
    last: Option<Instant>,
}

impl Pacer {
    pub fn want(&mut self, p: Option<Payload>) {
        self.wanted = p;
    }

    pub fn due(&self, now: Instant) -> Due {
        let gap = match (&self.shown, self.wanted == self.shown) {
            (None, true) => return Due::Nothing,
            (Some(_), true) => REFRESH,
            (_, false) => MIN_GAP,
        };
        match self.last.map(|t| (t + gap).saturating_duration_since(now)) {
            Some(wait) if !wait.is_zero() => Due::Wait(wait),
            _ => Due::Send(self.wanted.clone()),
        }
    }

    pub fn sent(&mut self, p: Option<Payload>, now: Instant) {
        self.shown = p;
        self.last = Some(now);
    }

    /// What Discord shows (None: nothing).
    pub fn shown(&self) -> Option<&Payload> {
        self.shown.as_ref()
    }

    /// The connection is gone: Discord shows nothing from this game any more.
    pub fn lost(&mut self) {
        self.shown = None;
    }
}

/// Whether DHD_DISCORD's value (None: not set) turns the presence on, as the game reads the
/// switch (dhd_settings: an off word is off, anything else on; unset, the built-in value).
pub fn enabled(setting: Option<&str>) -> bool {
    let Some(s) = dhd_settings::table::find("discord") else { return false };
    let value = setting.filter(|v| !v.trim().is_empty()).unwrap_or(s.builtin);
    s.canonical(value) == Some("on")
}

#[cfg(test)]
mod tests;
