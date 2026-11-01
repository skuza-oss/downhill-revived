//! The thread that talks to Discord. Its calls block (the pipe or socket is read and written
//! plainly), so the game never waits on them: the game's calls only post a message.
//!
//! With the Social SDK's library beside the game (sdk.rs, finding 136) the thread goes through it
//! (`run_sdk`); without it, Discord's local IPC through discord-rich-presence (`run`). Both keep the
//! same state, lines, pacing and log. Ask to Join needs the SDK (only it can receive a join), so the
//! IPC drops the secret (`for_ipc`, finding 136).

use crate::{party_id, payload, sdk, Clock, Due, Kind, Pacer, Payload, State, APP_ID, ASK_TO_JOIN, BUTTONS, LOGO, LOGO_TEXT, RETRY};
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

/// How often the SDK's callbacks run while it is used.
const TICK: Duration = Duration::from_millis(100);

/// Starts the thread. Nothing is sent before the first `set`.
pub fn start(log: Log) -> Option<Worker> {
    let (to, from) = channel();
    std::thread::Builder::new()
        .name("discord".into())
        .stack_size(1024 * 1024)  // the SDK's calls run on it too
        .spawn(move || match sdk::library_path().filter(|p| p.is_file()) {
            None => run(from, log),
            Some(path) => {
                // the command Discord starts the game with for a join (only with ASK_TO_JOIN)
                let launch = std::env::current_exe().ok().map(|exe| format!("\"{}\"", exe.display()));
                match sdk::Client::load(&path, ASK_TO_JOIN, launch.as_deref()) {
                    Ok(client) => {
                        log("discord: through the Social SDK");
                        run_sdk(from, log, client)
                    }
                    Err(why) => {
                        log(&format!("discord: the Social SDK didn't load ({why}); through the IPC"));
                        run(from, log)
                    }
                }
            }
        })
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
/// room's party (its size and id, which Discord asks for with a size) and its join secret when set.
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
        // the id is this session's own token: Discord refuses it matching the join secret (finding 136)
        let mut party = Party::new().id(party_id());
        if let Some(size) = p.party {
            party = party.size(size);
        }
        if let Some(join) = &p.join {
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

/// What the plain IPC shows (worker's `run`): a join secret is dropped. Discord's local IPC hands
/// a join over as `ACTIVITY_JOIN`, which only arrives after the game subscribed, and this worker
/// reads the pipe only right after its own writes (finding 136, point 1). A Join button it could
/// not be told about would be worse than none, so the IPC keeps the state as it was (finding 136):
/// the party size with this session's own id, no secrets, both buttons.
pub fn for_ipc(mut p: Option<Payload>) -> Option<Payload> {
    if let Some(p) = p.as_mut() {
        p.join = None;
    }
    p
}

fn describe(p: &Payload) -> String {
    let party = p.party.map(|[n, max]| format!(", {n} of {max}")).unwrap_or_default();
    match &p.state {
        Some(state) => format!("{} ({}{party})", p.details, state),
        None => p.details.clone(),
    }
}

/// The presence through the Social SDK. Its updates are answered in callbacks (`run_callbacks`,
/// every TICK): a refused one counts as a lost connection, tried again after RETRY.
fn run_sdk(from: Receiver<Message>, log: Log, mut sdk: sdk::Client) {
    let mut clock = Clock::new(unix_ms());
    let mut pacer = Pacer::default();
    let mut next_try = Instant::now();
    // the update waiting for its answer: its kind and when it went (an answer that never comes
    // frees it after RETRY)
    let mut waiting: Option<(Kind, Instant)> = None;
    let mut kind = Kind::None;
    let mut logged_kind: Option<Kind> = None;
    let mut failing = false;
    loop {
        match from.recv_timeout(TICK) {
            Ok(Message::Set(s)) => {
                let start = clock.start(&s, unix_ms());
                kind = s.kind;
                pacer.want(payload(&s, start));
            }
            Ok(Message::Stop(done)) => {
                if pacer.shown().is_some() {
                    sdk.set(None);
                    // the clear has no answer: a few rounds of callbacks let it go out
                    for _ in 0..4 {
                        sdk.run_callbacks();
                        std::thread::sleep(Duration::from_millis(50));
                    }
                    log("discord: presence cleared");
                }
                drop(sdk);
                let _ = done.send(());
                return;
            }
            Err(RecvTimeoutError::Timeout) => {}
            Err(RecvTimeoutError::Disconnected) => return,
        }
        sdk.run_callbacks();
        let events = sdk.take_events();
        for line in &events.lines {
            log(&format!("discord: sdk: {line}"));
        }
        if let Some(secret) = events.join {
            crate::joined(&secret, &log);
        }
        let now = Instant::now();
        if let (Some(outcome), Some((k, _))) = (events.update, waiting) {
            waiting = None;
            match outcome {
                Ok(()) => {
                    failing = false;
                    if logged_kind != Some(k) {
                        if let Some(p) = pacer.shown() {
                            log(&format!("discord: presence set: {}", describe(p)));
                        }
                        logged_kind = Some(k);
                    }
                }
                Err(why) => {
                    pacer.lost();
                    next_try = now + RETRY;
                    logged_kind = None;
                    if !failing {
                        log(&format!("discord: the presence wasn't set ({why}); it waits for Discord (a try every 30 s)"));
                        failing = true;
                    }
                }
            }
        }
        if waiting.is_some_and(|(_, at)| now >= at + RETRY) {
            waiting = None;
        }
        if waiting.is_some() || now < next_try {
            continue;
        }
        if let Due::Send(p) = pacer.due(now) {
            sdk.set(p.as_ref());
            // a clear has no answer
            waiting = p.is_some().then_some((kind, now));
            pacer.sent(p, now);
        }
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
                pacer.want(for_ipc(payload(&s, start)));  // the IPC can't receive a join (finding 136)
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
