//! A P2P room's part of the send thread (finding 186): the star's peer connections (peer.rs) and
//! the shared router (dhd_star), beside the room's SFU association (lib.rs `Assoc`). The room is a
//! room like any other: its messages go to the game as they come; the routes (`p2p`) and the star's
//! signaling (`p2p_signal`) come here. Every race frame goes through the router, which says where
//! it goes: a peer, this game's SFU publication, the game.
use crate::{lock, peer, peer::Peer, WsConn};
use dhd_star::{Action, Star};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::net::IpAddr;
use std::time::Instant;

/// The longest wait for work while peers exist: they need polling even when nothing was queued
/// (ICE and SCTP timers), and the 60 Hz race wants its frames out within a couple of milliseconds.
pub const POLL: std::time::Duration = std::time::Duration::from_millis(2);

struct Link {
    peer: Peer,
    up: bool,
    logged_path: bool,
}

/// Signals held for a peer the router asked for but this thread hasn't made yet (the other end's
/// offer can come in the same read as the routes that ask for the peer); a new hub's epoch drops
/// them.
const HELD_SIGNALS: usize = 32;

pub struct P2p {
    pub star: Star,
    peers: HashMap<u64, Link>,
    held: HashMap<u64, Vec<Value>>,
    /// The hub's epoch the held signals and this game's own are for.
    epoch: u64,
    wake: peer::Wake,
    /// The address the peers bind to (peer::reachable_address), found with the first peer.
    local: Option<IpAddr>,
    hub: Option<u64>,
    /// The members the routes put on the SFU, logged once each (per hub).
    on_sfu: Vec<u64>,
    started: Instant,
}

impl P2p {
    pub fn new(wake: peer::Wake) -> Self {
        // DHD_NET_P2P_UDP=0 (a test switch): no direct links from this game, so it races over the
        // room's SFU (the fallback, docs/TESTING.md).
        let direct = std::env::var("DHD_NET_P2P_UDP").map_or(true, |v| v.trim() != "0");
        if !direct {
            eprintln!("net: P2P: DHD_NET_P2P_UDP=0, no direct links from this game (test switch): it races over the SFU");
        }
        Self { star: Star::new(direct), peers: HashMap::new(), held: HashMap::new(), epoch: 0, wake, local: None, hub: None, on_sfu: vec![], started: Instant::now() }
    }

    /// The router's clock: milliseconds since the room began here.
    pub fn now(&self) -> u64 {
        self.started.elapsed().as_millis() as u64
    }

    /// A room message for the star: the welcome and the routes. Logs a new hub and each member the
    /// room puts on its SFU (dhd.log: who raced how, without an address).
    pub fn room(&mut self, m: &Value) {
        self.star.room(m);
        if m["type"] != "p2p" {
            return;
        }
        if m["epoch"].as_u64() != Some(self.epoch) {
            self.epoch = m["epoch"].as_u64().unwrap_or(0);
            self.held.clear();
        }
        let hub = m["hub"].as_u64();
        if hub != self.hub {
            self.hub = hub;
            self.on_sfu.clear();
            match hub {
                Some(id) if self.star.is_hub() => eprintln!("net: P2P: this game is the room's hub (member {id})"),
                Some(id) => eprintln!("net: P2P: member {id} is the room's hub"),
                None => {}
            }
        }
        let sfu: Vec<u64> = m["members"].as_array().map_or(vec![], |a| {
            a.iter().filter(|x| x["link"] == "sfu").filter_map(|x| x["id"].as_u64()).collect()
        });
        for id in sfu.iter().filter(|id| !self.on_sfu.contains(id)) {
            eprintln!("net: P2P: member {id} races over the room's SFU (no direct link to the hub)");
        }
        self.on_sfu = sfu;
    }

    /// The other end's signaling, as the room passed it on; held when its peer is still to be made.
    /// One for another hub's epoch is dropped (its peer is gone, or not yet this one).
    pub fn signal(&mut self, from: u64, epoch: u64, signal: &Value) {
        if epoch != self.star.epoch() {
            return;
        }
        match self.peers.get_mut(&from) {
            Some(link) => {
                if let Err(e) = link.peer.signal(signal) {
                    eprintln!("net: P2P: signaling with member {from} failed ({e})");
                }
            }
            None => {
                let held = self.held.entry(from).or_default();
                if held.len() < HELD_SIGNALS {
                    held.push(signal.clone());
                }
            }
        }
    }

    /// The star has a peer to poll (the send thread then waits at most POLL).
    pub fn polling(&self) -> bool {
        !self.peers.is_empty()
    }

    /// Runs every peer: its signals go to the room, its messages and its state to the router. A peer
    /// that failed or never connected is dropped; the router tells the room.
    pub fn poll(&mut self, c: &WsConn) {
        let now = self.now();
        let mut failed = vec![];
        for (&id, link) in &mut self.peers {
            match link.peer.poll() {
                Ok((signals, messages)) => {
                    for signal in signals {
                        c.write_now(false, json!({"type": "p2p_signal", "to": id, "epoch": self.star.epoch(), "signal": signal}).to_string().into_bytes());
                    }
                    if link.peer.connected() && !link.up {
                        link.up = true;
                        self.star.link(id, true, now);
                    }
                    if let (false, Some(path)) = (link.logged_path, link.peer.path()) {
                        link.logged_path = true;
                        eprintln!("net: P2P: member {id} connected: {}", path.describe());
                    }
                    for (reliable, bytes) in messages {
                        self.star.peer_message(id, reliable, &bytes, now);
                    }
                    if link.peer.timed_out() {
                        eprintln!("net: P2P: member {id} could not be reached directly");
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
            self.peers.remove(&id);
            self.star.link(id, false, now);
        }
        self.star.tick(now);
    }

    /// Carries out the router's actions on the peers and the room; returns those for the SFU and
    /// the game, which the send thread owns.
    pub fn run(&mut self, c: &WsConn) -> Vec<Action> {
        let mut rest = vec![];
        for action in self.star.take() {
            match action {
                Action::Connect { member, offerer } => self.connect(c, member, offerer),
                Action::Close(member) => {
                    self.held.remove(&member);
                    if self.peers.remove(&member).is_some_and(|l| l.up) {
                        eprintln!("net: P2P: the link to member {member} closed");
                    }
                }
                Action::Peer(member, reliable, bytes) => {
                    if let Some(link) = self.peers.get_mut(&member) {
                        if let Err(e) = link.peer.send(reliable, bytes) {
                            eprintln!("net: P2P: a message to member {member} was dropped ({e})");
                        }
                    }
                }
                Action::Room(m) => c.write_now(false, m.to_string().into_bytes()),
                other => rest.push(other),
            }
        }
        rest
    }

    fn connect(&mut self, c: &WsConn, member: u64, offerer: bool) {
        if self.local.is_none() {
            let room = lock(&c.live).as_ref().and_then(|l| l.tcp.local_addr().ok()).map(|a| a.ip());
            self.local = peer::reachable_address(room);
        }
        let now = self.now();
        let made = match self.local {
            Some(local) => Peer::new(local, offerer),
            None => Err("no local address".into()),
        };
        match made {
            Ok(peer) => {
                // The hub's offer goes out with the peer's first poll.
                self.peers.insert(member, Link { peer, up: false, logged_path: false });
                let epoch = self.star.epoch();
                for signal in self.held.remove(&member).unwrap_or_default() {
                    self.signal(member, epoch, &signal);
                }
                (self.wake)();
            }
            Err(e) => {
                eprintln!("net: P2P: no connection to member {member} ({e})");
                self.star.link(member, false, now);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use crate::{ws_send, ws_start, ws_stop, WsConn, WsHandler};
    use serde_json::{json, Value};
    use std::ffi::{c_char, c_void, CString};
    use std::sync::Mutex;
    use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

    #[derive(Default)]
    struct Inbox {
        messages: Vec<Value>,
        frames: Vec<Vec<u8>>,
    }

    unsafe extern "C" fn on_open(_: *mut c_void, ws: *mut WsConn) {
        let hello = json!({"type": "hello", "protocol": 4, "version": "p2p-smoke", "clientVersion": "0.7.13",
            "isoSha256": "a".repeat(64), "name": "TEST", "features": ["session", "countdown", "race_rejoin", "p2p"]})
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
        conn: Option<*mut WsConn>,
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
            Self { conn: Some(conn), inbox }
        }
        fn send(&self, m: Value) {
            let text = m.to_string();
            unsafe { ws_send(self.conn.unwrap(), 0, text.as_ptr().cast(), text.len()) };
        }
        fn frame(&self, bytes: &[u8]) {
            unsafe { ws_send(self.conn.unwrap(), 1, bytes.as_ptr().cast(), bytes.len()) };
        }
        fn last(&self, kind: &str) -> Option<Value> {
            self.inbox.lock().unwrap().messages.iter().rev().find(|m| m["type"] == kind).cloned()
        }
        /// The slots whose frames arrived since `since` frames.
        fn senders(&self, since: usize) -> std::collections::HashSet<u8> {
            self.inbox.lock().unwrap().frames.iter().skip(since).map(|f| f[1]).collect()
        }
        fn frames(&self) -> usize {
            self.inbox.lock().unwrap().frames.len()
        }
        /// The game leaves: its connection and its links end.
        fn stop(&mut self) {
            if let Some(conn) = self.conn.take() {
                self.send_leave(conn);
                unsafe { ws_stop(conn) };
            }
        }
        fn send_leave(&self, conn: *mut WsConn) {
            let text = json!({"type": "leave"}).to_string();
            unsafe { ws_send(conn, 0, text.as_ptr().cast(), text.len()) };
        }
    }

    impl Drop for Client {
        fn drop(&mut self) {
            if let Some(conn) = self.conn.take() {
                unsafe { ws_stop(conn) };
            }
        }
    }

    fn wait(what: &str, mut done: impl FnMut() -> bool) {
        let end = Instant::now() + Duration::from_secs(30);
        while Instant::now() < end {
            if done() {
                return;
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        panic!("P2P room test: timed out waiting for {what}");
    }

    fn epoch_ms() -> f64 {
        SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis() as f64
    }

    /// A race frame as the game sends it: an event, or a minimal state bundle (version 6).
    fn race_frame(state: bool) -> Vec<u8> {
        let mut frame = vec![0u8; 24];
        frame[0] = if state { 5 } else { 6 };
        frame[2] = 6;
        frame[5..13].copy_from_slice(&epoch_ms().to_le_bytes());
        frame
    }

    /// `n` games in one P2P room, all ready, racing: each has heard every other racer's frames.
    fn racing(n: usize) -> Vec<Client> {
        let base = std::env::var("DHD_NET_SERVER").expect("local_server.py supplies DHD_NET_SERVER");
        let host = Client::new(&format!("{base}/v1/rooms/new?transport=p2p"));
        wait("the welcome", || host.last("welcome").is_some());
        let welcome = host.last("welcome").unwrap();
        assert_eq!(welcome["transport"], "p2p");
        let code = welcome["code"].as_str().unwrap().to_owned();
        assert_eq!(code.len(), 7);
        let mut clients = vec![host];
        for _ in 1..n {
            clients.push(Client::new(&format!("{base}/v1/rooms/{code}")));
        }
        wait("every member in every room", || {
            clients.iter().all(|c| c.last("room").is_some_and(|m| m["members"].as_array().is_some_and(|a| a.len() == n)))
        });
        for c in &clients {
            c.send(json!({"type": "ready", "ready": true}));
        }
        wait("the start", || clients.iter().all(|c| c.last("start").is_some()));
        hear_each_other(&clients, n);
        clients
    }

    /// Every game sends frames until each has heard every other racer since now.
    fn hear_each_other(clients: &[Client], racers: usize) {
        let marks: Vec<usize> = clients.iter().map(Client::frames).collect();
        wait("every rider's frames", || {
            for c in clients {
                c.frame(&race_frame(false));
                c.frame(&race_frame(true));
            }
            clients.iter().zip(&marks).all(|(c, m)| c.senders(*m).len() == racers - 1)
        });
    }

    fn finish(clients: &[Client]) {
        for (i, c) in clients.iter().enumerate() {
            c.send(json!({"type": "finish", "ticks": 100 + i, "place": i + 1}));
        }
        wait("the results", || clients.iter().all(|c| c.last("ended").is_some() &&
            c.last("room").is_some_and(|r| !r["lastRace"].is_null())));
    }

    fn room_race(n: usize) {
        let clients = racing(n);
        finish(&clients);
        eprintln!("P2P ROOM MATCH: {n} games joined, readied, raced (each heard the other {}) and got the results", n - 1);
    }

    /// A signal for another hub's epoch is never applied: dropped as it comes, and those held for a
    /// peer still to be made go with a new hub (review 5).
    #[test]
    fn signals_for_another_hubs_epoch_are_dropped() {
        let mut p = super::P2p::new(std::sync::Arc::new(|| {}));
        p.room(&json!({"type": "welcome", "memberId": 2}));
        p.room(&json!({"type": "p2p", "hub": 1, "epoch": 1, "members": [{"id": 1}, {"id": 2}], "race": null}));
        p.signal(1, 1, &json!({"type": "offer", "sdp": "v=0"}));  // before its peer is made: held
        p.signal(1, 0, &json!({"type": "candidate", "candidate": "old"}));  // an earlier hub's: dropped
        assert_eq!(p.held[&1].len(), 1);
        p.room(&json!({"type": "p2p", "hub": 3, "epoch": 2, "members": [{"id": 1}, {"id": 2}, {"id": 3}], "race": null}));
        assert!(p.held.is_empty());
        p.signal(1, 1, &json!({"type": "offer", "sdp": "v=0"}));
        assert!(p.held.is_empty());
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

    /// The hub's game leaves mid-race: the room picks the next hub, the others link to it, their
    /// riders' frames flow again and every game left gets the results.
    #[test]
    #[ignore = "run through local_server.py"]
    fn p2p_room_hub_leaves_mid_race() {
        let mut clients = racing(3);
        clients[0].stop();
        let rest = &clients[1..];
        wait("the hub's departure", || rest.iter().all(|c| c.last("left").is_some_and(|l| l["slot"] == 0)));
        hear_each_other(rest, 2);
        finish(rest);
        eprintln!("P2P ROOM MATCH: the hub left mid-race; the two others raced on over the next hub and got the results");
    }
}
