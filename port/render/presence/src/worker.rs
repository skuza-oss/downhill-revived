//! The thread that talks to Discord. Its calls block (the pipe or socket is read and written
//! plainly), so the game never waits on them: the game's calls only post a message.

use crate::{payload, Clock, Due, Kind, Pacer, Payload, State, APP_ID, BUTTONS, LOGO, LOGO_TEXT, RETRY};
use discord_rich_presence::activity::{Activity, Assets, Button, Party, Secrets, Timestamps};
use discord_rich_presence::{DiscordIpc, DiscordIpcClient};
use std::sync::mpsc::{channel, Receiver, RecvTimeoutError, Sender};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// A line for the game's log (dhd.log).
pub type Log = Box<dyn Fn(&str) + Send>;

enum Message {
    Set(State),
    Stop(Sender<()>),
}

/// The running presence: the game's side of the thread.
pub struct Worker {
    to: Sender<Message>,
}

/// How long `stop` waits for the thread to clear the presence; past it the process ends anyway and
/// Discord clears it when the connection closes.
const STOP_WAIT: Duration = Duration::from_millis(500);

/// Starts the thread. Nothing is sent before the first `set`.
pub fn start(log: Log) -> Option<Worker> {
    let (to, from) = channel();
    std::thread::Builder::new()
        .name("discord".into())
        .stack_size(256 * 1024)
        .spawn(move || run(from, log))
        .ok()
        .map(|_| Worker { to })
}

impl Worker {
    /// The player's state now; sent if it changed, once the gap since the last update allows.
    pub fn set(&self, s: State) {
        let _ = self.to.send(Message::Set(s));
    }

    /// Clears the presence and closes the connection (at exit), waiting at most STOP_WAIT.
    pub fn stop(&self) {
        let (done, wait) = channel();
        if self.to.send(Message::Stop(done)).is_ok() {
            let _ = wait.recv_timeout(STOP_WAIT);
        }
    }
}

fn unix_ms() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0)
}

/// The activity Discord gets for `p`: its two lines, the elapsed time, the logo and the buttons; a
/// room's party size, and its join secret (with the party's id, which Ask to Join needs) when set.
pub fn activity(p: &Payload) -> Activity<'_> {
    let mut a = Activity::new()
        .details(p.details.as_str())
        .timestamps(Timestamps::new().start(p.start))
        .assets(Assets::new().large_image(LOGO).large_text(LOGO_TEXT))
        .buttons(BUTTONS.iter().map(|(label, url)| Button::new(*label, *url)).collect());
    if let Some(state) = &p.state {
        a = a.state(state.as_str());
    }
    if p.party.is_some() || p.join.is_some() {
        let mut party = Party::new();
        if let Some(size) = p.party {
            party = party.size(size);
        }
        if let Some(join) = &p.join {
            party = party.id(join.as_str());
            a = a.secrets(Secrets::new().join(join.as_str()));
        }
        a = a.party(party);
    }
    a
}

/// Sends `p` (None: clears) and reads Discord's answer, which keeps the pipe drained. Err: the
/// connection is gone. Ok(Some(message)): Discord refused it.
fn send(client: &mut DiscordIpcClient, p: Option<&Payload>) -> Result<Option<String>, ()> {
    match p {
        Some(p) => client.set_activity(activity(p)),
        None => client.clear_activity(),
    }
    .map_err(|_| ())?;
    let (_, answer) = client.recv().map_err(|_| ())?;
    Ok((answer["evt"] == "ERROR").then(|| answer["data"]["message"].as_str().unwrap_or("?").to_string()))
}

fn connect() -> Option<DiscordIpcClient> {
    let mut client = DiscordIpcClient::new(APP_ID);
    client.connect().ok().map(|_| client)
}

fn describe(p: &Payload) -> String {
    let party = p.party.map(|[n, max]| format!(", {n} of {max}")).unwrap_or_default();
    match &p.state {
        Some(state) => format!("{} ({}{party})", p.details, state),
        None => p.details.clone(),
    }
}

fn run(from: Receiver<Message>, log: Log) {
    let mut clock = Clock::new(unix_ms());
    let mut pacer = Pacer::default();
    let mut client: Option<DiscordIpcClient> = None;
    let mut next_try = Instant::now();
    // the log's lines: one while Discord can't be reached (again after it was), the first update
    // after each connection and each change of kind, the clear at exit
    let mut told_absent = false;
    let mut logged_kind: Option<Kind> = None;
    let mut kind = Kind::None;
    let mut refused = false;
    loop {
        let now = Instant::now();
        let wait = match pacer.due(now) {
            Due::Nothing => None,
            Due::Wait(d) => Some(d),
            Due::Send(_) if client.is_none() => Some(next_try.saturating_duration_since(now)),
            Due::Send(_) => Some(Duration::ZERO),
        };
        let message = match wait {
            None => from.recv().ok(),
            Some(d) if d.is_zero() => from.try_recv().ok(),
            Some(d) => match from.recv_timeout(d) {
                Ok(m) => Some(m),
                Err(RecvTimeoutError::Timeout) => None,
                Err(RecvTimeoutError::Disconnected) => return,
            },
        };
        match message {
            Some(Message::Set(s)) => {
                let start = clock.start(&s, unix_ms());
                kind = s.kind;
                pacer.want(payload(&s, start));
            }
            Some(Message::Stop(done)) => {
                if let Some(c) = client.as_mut() {
                    if pacer.shown().is_some() && send(c, None).is_ok() {
                        log("discord: presence cleared");
                    }
                    let _ = c.close();
                }
                let _ = done.send(());
                return;
            }
            None if wait.is_none() => return,  // the game's side is gone
            None => {}
        }
        let now = Instant::now();
        let Due::Send(p) = pacer.due(now) else { continue };
        if client.is_none() {
            if now < next_try {
                continue;
            }
            client = connect();
            if client.is_none() {
                next_try = now + RETRY;
                if !told_absent {
                    log("discord: not running; the presence waits for it (a try every 30 s)");
                    told_absent = true;
                }
                continue;
            }
            told_absent = false;
            logged_kind = None;
        }
        let c = client.as_mut().expect("connected");
        match send(c, p.as_ref()) {
            Ok(answer) => {
                pacer.sent(p.clone(), now);
                if let Some(message) = answer {
                    if !refused {
                        log(&format!("discord: the presence was refused: {message}"));
                        refused = true;
                    }
                } else if logged_kind != Some(kind) {
                    match &p {
                        Some(p) => log(&format!("discord: presence set: {}", describe(p))),
                        None => log("discord: presence cleared"),
                    }
                    logged_kind = Some(kind);
                }
            }
            Err(()) => {
                client = None;
                pacer.lost();
                next_try = now + RETRY;
                if !told_absent {
                    log("discord: the connection closed; the presence waits for Discord (a try every 30 s)");
                    told_absent = true;
                }
            }
        }
    }
}
