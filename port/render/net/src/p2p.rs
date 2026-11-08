//! The send thread of a P2P room's connection (finding 180), in place of the SFU's (lib.rs
//! send_main). The room connection carries membership and signaling only (online/server/src/
//! p2p.ts); race frames and room commands travel over peer connections (peer.rs): the host has one
//! per guest, a guest one to the host. The room's rules run in the host's coordinator (dhd_session),
//! which speaks the SFU room's messages to the game, so the game's online code is the same for
//! both kinds of room.
use crate::{lock, peer::Peer, WsConn};
use dhd_session::{Action, Session};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::net::IpAddr;
use std::sync::{atomic::Ordering, Arc};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// The longest wait for work: peers need polling even when nothing was queued (ICE and SCTP
/// timers), and the 60 Hz race wants its frames out within a couple of milliseconds.
const POLL: Duration = Duration::from_millis(2);
/// The room messages that go to the server; every other text message is a room command for the
/// host's coordinator.
const SERVER_MESSAGES: [&str; 4] = ["hello", "ping", "pong", "leave"];

struct Link {
    peer: Peer,
    up: bool,
    logged_path: bool,
}

struct P2pRoom<'a> {
    c: &'a WsConn,
    session: Option<Session>,
    links: HashMap<u64, Link>,
    /// This game's TURN credentials (p2p_turn), for every peer made from now on.
    credentials: Option<Value>,
    /// DHD_NET_P2P_UDP=0: no direct UDP, the relay carries everything (tests of the fallback).
    direct: bool,
    /// The server's clock minus this machine's (ms), from the welcome: the coordinator's times
    /// (countdowns, start, finish deadlines) are on the server's clock, as an SFU room's are.
    clock_offset: i64,
    wake: crate::peer::Wake,
}

pub fn run(c: &Arc<WsConn>) {
    let weak = Arc::downgrade(c);
    let mut room = P2pRoom {
        c,
        session: None,
        links: HashMap::new(),
        credentials: None,
        direct: std::env::var("DHD_NET_P2P_UDP").map_or(true, |v| v != "0"),
        clock_offset: 0,
        wake: Arc::new(move || {
            if let Some(c) = weak.upgrade() {
                c.wake.notify_all();
            }
        }),
    };
    while !c.stopping.load(Ordering::Acquire) {
        room.from_server();
        room.from_game();
        room.poll_peers();
        room.run_session();
        if let Some(f) = c.h.on_tick {
            if unsafe { f(c.h.ctx) } != 0 {
                c.force_close();
            }
        }
        let q = lock(&c.queue);
        let _ = c.wake.wait_timeout(q, POLL).unwrap_or_else(|e| e.into_inner());
    }
}

fn epoch_ms() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis() as i64
}

impl P2pRoom<'_> {
    fn now(&self) -> u64 {
        (epoch_ms() + self.clock_offset).max(0) as u64
    }

    /// What the server sent: membership, signaling and credentials are for this thread; the rest
    /// (pongs, errors) goes to the game as it came.
    fn from_server(&mut self) {
        let incoming: Vec<_> = lock(&self.c.queue).incoming.drain(..).collect();
        for (binary, data) in incoming {
            let m = if binary { None } else { serde_json::from_slice::<Value>(&data).ok() };
            match m.as_ref().and_then(|m| m["type"].as_str()) {
                Some("p2p_welcome") => self.welcome(m.as_ref().unwrap()),
                Some("p2p_members") => {
                    let now = self.now();
                    if let Some(s) = self.session.as_mut() {
                        s.members(&m.as_ref().unwrap()["members"], now);
                    }
                    self.connect_members(&m.unwrap()["members"]);
                }
                Some("p2p_signal") => {
                    let m = m.unwrap();
                    let from = m["from"].as_u64().unwrap_or(0);
                    if let Some(link) = self.links.get_mut(&from) {
                        if let Err(e) = link.peer.signal(&m["signal"]) {
                            eprintln!("net: P2P: signaling with member {from} failed ({e})");
                        }
                    }
                }
                Some("p2p_turn") => {
                    let m = m.unwrap();
                    for (id, link) in &mut self.links {
                        if let Err(e) = link.peer.turn(&m, self.wake.clone()) {
                            eprintln!("net: P2P: no relay for member {id} ({e})");
                        }
                    }
                    self.credentials = Some(m);
                }
                _ => self.c.deliver(binary, &data),
            }
            lock(&self.c.queue).handled += 1;
            self.c.wake.notify_all();
        }
    }

    fn welcome(&mut self, m: &Value) {
        self.clock_offset = m["serverTime"].as_i64().map_or(0, |t| t - epoch_ms());
        eprintln!("net: P2P room {}, member {}, host {}", m["code"], m["memberId"], m["hostId"]);
        self.session = Some(Session::new(m, self.now()));
    }

    /// A peer for each member this game links to (the host: every guest; a guest: the host);
    /// those of members gone are dropped.
    fn connect_members(&mut self, members: &Value) {
        let Some(s) = self.session.as_ref() else { return };
        let ids: Vec<u64> = members.as_array().map_or(vec![], |a| a.iter().filter_map(|m| m["id"].as_u64()).collect());
        self.links.retain(|id, _| ids.contains(id));
        let wanted: Vec<u64> =
            ids.into_iter().filter(|&id| id != s.me && (s.is_host() || id == s.host) && !self.links.contains_key(&id)).collect();
        let host = s.is_host();
        // The address this game reaches the server from: the network the other games can reach.
        let Some(local) = lock(&self.c.live).as_ref().and_then(|l| l.tcp.local_addr().ok()).map(|a| a.ip()) else { return };
        for id in wanted {
            match self.make_peer(local, host) {
                Ok(peer) => {
                    self.links.insert(id, Link { peer, up: false, logged_path: false });
                }
                Err(e) => eprintln!("net: P2P: no connection to member {id} ({e})"),
            }
        }
    }

    fn make_peer(&self, local: IpAddr, offerer: bool) -> Result<Peer, Box<dyn std::error::Error>> {
        let mut peer = Peer::new(local, offerer, self.direct)?;
        if let Some(credentials) = &self.credentials {
            peer.turn(credentials, self.wake.clone())?;
        }
        Ok(peer)
    }

    /// What the game sent: race frames and room commands go to the coordinator, the server's
    /// own messages (hello, pings, leave) to the server.
    fn from_game(&mut self) {
        let outgoing: Vec<_> = lock(&self.c.queue).out.drain(..).collect();
        let now = self.now();
        for (binary, data) in outgoing {
            if binary && matches!(data.first(), Some(5 | 6)) {
                if let Some(s) = self.session.as_mut() {
                    s.frame(&data);
                }
                continue;
            }
            if binary {
                self.c.write_now(true, data); // the clock's PING
                continue;
            }
            let Ok(m) = serde_json::from_slice::<Value>(&data) else { continue };
            if SERVER_MESSAGES.contains(&m["type"].as_str().unwrap_or("")) {
                self.c.write_now(false, data);
            } else if let Some(s) = self.session.as_mut() {
                s.local(m, now);
            }
        }
    }

    /// Runs every peer: its signals to the room, its messages to the coordinator. A peer that
    /// failed or never connected is dropped: the coordinator turns that member's rider into the
    /// host's AI (a guest whose host is gone leaves the room).
    fn poll_peers(&mut self) {
        let now = self.now();
        let mut failed = vec![];
        for (&id, link) in &mut self.links {
            match link.peer.poll() {
                Ok((signals, messages)) => {
                    for signal in signals {
                        self.c.write_now(false, json!({"type": "p2p_signal", "to": id, "signal": signal}).to_string().into_bytes());
                    }
                    if link.peer.connected() && !link.up {
                        link.up = true;
                        if let Some(s) = self.session.as_mut() {
                            s.link(id, true, now);
                        }
                    }
                    if let (false, Some(path)) = (link.logged_path, link.peer.path()) {
                        link.logged_path = true;
                        eprintln!("net: P2P: member {id} connected: {}", path.describe());
                    }
                    if let Some(s) = self.session.as_mut() {
                        for (reliable, bytes) in messages {
                            s.receive(id, reliable, bytes, now);
                        }
                    }
                    if link.peer.timed_out() {
                        eprintln!("net: P2P: member {id} could not be reached (no direct path, no relay)");
                        failed.push(id);
                    }
                }
                Err(e) => {
                    eprintln!("net: P2P: the connection to member {id} failed ({e})");
                    failed.push(id);
                }
            }
        }
        for id in failed {
            self.links.remove(&id);
            if let Some(s) = self.session.as_mut() {
                s.link(id, false, now);
            }
        }
    }

    /// The coordinator's time-driven work, then what it asked for.
    fn run_session(&mut self) {
        let now = self.now();
        let Some(s) = self.session.as_mut() else { return };
        s.tick(now);
        for action in s.take() {
            match action {
                Action::Game(m) => self.c.deliver(false, m.to_string().as_bytes()),
                Action::Frame(bytes) => self.c.deliver(true, &bytes),
                Action::Server(m) => self.c.write_now(false, m.to_string().into_bytes()),
                Action::Peer(id, reliable, bytes) => {
                    if let Some(link) = self.links.get_mut(&id) {
                        if let Err(e) = link.peer.send(reliable, bytes) {
                            eprintln!("net: P2P: a message to member {id} was dropped ({e})");
                        }
                    }
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ws_send, ws_start, ws_stop, WsHandler};
    use std::ffi::{c_char, c_void, CString};
    use std::sync::Mutex;
    use std::time::Instant;

    #[derive(Default)]
    struct Inbox {
        messages: Vec<Value>,
        frames: Vec<Vec<u8>>,
    }

    unsafe extern "C" fn on_open(_: *mut c_void, ws: *mut WsConn) {
        let hello = json!({"type": "hello", "protocol": 4, "version": "p2p-smoke", "clientVersion": "0.7.13",
            "isoSha256": "a".repeat(64), "name": "TEST", "features": ["session", "countdown"]})
        .to_string();
        unsafe { ws_send(ws, 0, hello.as_ptr().cast(), hello.len()) };
    }

    unsafe extern "C" fn on_message(ctx: *mut c_void, binary: i32, data: *const u8, n: usize) {
        let inbox = unsafe { &*(ctx as *const Mutex<Inbox>) };
        let bytes = unsafe { std::slice::from_raw_parts(data, n) };
        if binary != 0 {
            inbox.lock().unwrap().frames.push(bytes.to_vec());
        } else if let Ok(m) = serde_json::from_slice::<Value>(bytes) {
            inbox.lock().unwrap().messages.push(m);
        }
    }

    unsafe extern "C" fn on_close(_: *mut c_void, _: i32, _: *const c_char, _: i32) -> *const c_char {
        std::ptr::null()
    }

    /// A game's network side as the C client drives it: a room connection and what it received.
    struct Client {
        conn: *mut WsConn,
        inbox: Box<Mutex<Inbox>>,
    }

    impl Client {
        fn new(url: &str) -> Self {
            let mut inbox = Box::<Mutex<Inbox>>::default();
            let h = WsHandler {
                ctx: (&mut *inbox as *mut Mutex<Inbox>).cast(),
                on_open: Some(on_open),
                on_message: Some(on_message),
                on_close: Some(on_close),
                on_tick: None,
            };
            let url = CString::new(url).unwrap();
            let conn = unsafe { ws_start(url.as_ptr(), &h) };
            assert!(!conn.is_null());
            Self { conn, inbox }
        }
        fn send(&self, m: Value) {
            let text = m.to_string();
            unsafe { ws_send(self.conn, 0, text.as_ptr().cast(), text.len()) };
        }
        fn frame(&self, bytes: &[u8]) {
            unsafe { ws_send(self.conn, 1, bytes.as_ptr().cast(), bytes.len()) };
        }
        fn last(&self, kind: &str) -> Option<Value> {
            self.inbox.lock().unwrap().messages.iter().rev().find(|m| m["type"] == kind).cloned()
        }
        fn senders(&self) -> std::collections::HashSet<u8> {
            self.inbox.lock().unwrap().frames.iter().map(|f| f[1]).collect()
        }
    }

    impl Drop for Client {
        fn drop(&mut self) {
            unsafe { ws_stop(self.conn) };
        }
    }

    fn wait(what: &str, mut done: impl FnMut() -> bool) {
        let end = Instant::now() + Duration::from_secs(25);
        while Instant::now() < end {
            if done() {
                return;
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        panic!("P2P room test: timed out waiting for {what}");
    }

    /// A race frame as the game sends it: an event, or a minimal state bundle (version 6).
    fn race_frame(state: bool) -> Vec<u8> {
        let mut frame = vec![0u8; 24];
        frame[0] = if state { 5 } else { 6 };
        frame[2] = 6;
        frame[5..13].copy_from_slice(&(epoch_ms() as f64).to_le_bytes());
        frame
    }

    /// `n` games in one room: they join, ready, start, exchange race frames (each hears every
    /// other) and get the results.
    fn room_race(n: usize) {
        let base = std::env::var("DHD_NET_SERVER").expect("local_server.py supplies DHD_NET_SERVER");
        let host = Client::new(&format!("{base}/v1/p2p/rooms/new"));
        wait("the welcome", || host.last("welcome").is_some());
        let code = host.last("welcome").unwrap()["code"].as_str().unwrap().to_owned();
        assert_eq!(code.len(), 7);
        let mut clients = vec![host];
        for _ in 1..n {
            clients.push(Client::new(&format!("{base}/v1/p2p/rooms/{code}")));
        }
        wait("every member in every room", || {
            clients.iter().all(|c| c.last("room").is_some_and(|m| m["members"].as_array().is_some_and(|a| a.len() == n)))
        });
        for c in &clients {
            c.send(json!({"type": "ready", "ready": true}));
        }
        wait("the start", || clients.iter().all(|c| c.last("start").is_some()));
        wait("every rider's frames", || {
            for c in &clients {
                c.frame(&race_frame(false));
                c.frame(&race_frame(true));
            }
            clients.iter().all(|c| c.senders().len() == n - 1)
        });
        for (i, c) in clients.iter().enumerate() {
            c.send(json!({"type": "finish", "ticks": 100 + i, "place": i + 1}));
        }
        wait("the results", || clients.iter().all(|c| c.last("ended").is_some()));
        eprintln!("P2P ROOM MATCH: {n} games joined, readied, raced (each heard the other {}) and got the results", n - 1);
    }

    #[test]
    #[ignore = "run through local_server.py"]
    fn p2p_room_two_games() {
        room_race(2);
    }

    #[test]
    #[ignore = "run through local_server.py"]
    fn p2p_room_ten_games() {
        room_race(10);
    }
}
