//! Native P2P adapter: one peer per guest at the host, one host peer at a guest.
use crate::{
    lock,
    peer::Peer,
    session::{Action, Session},
    WsConn,
};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::net::{SocketAddr, ToSocketAddrs};
use std::sync::{atomic::Ordering, Arc};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tungstenite::{Bytes, Message, Utf8Bytes};

fn epoch() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}
fn deliver(c: &WsConn, binary: bool, bytes: &[u8]) {
    if let Some(f) = c.h.on_message {
        let mut data = bytes.to_vec();
        data.push(0);
        unsafe { f(c.h.ctx, binary as i32, data.as_ptr(), bytes.len()) };
    }
}
fn service(c: &WsConn, binary: bool, data: Vec<u8>) {
    if let Some(live) = lock(&c.live).as_mut() {
        let msg = if binary {
            Message::Binary(Bytes::from(data))
        } else {
            Message::Text(Utf8Bytes::from(String::from_utf8_lossy(&data).into_owned()))
        };
        if live.ws.send(msg).is_err() {
            let _ = live.tcp.shutdown(std::net::Shutdown::Both);
        }
    }
}

pub fn run(c: Arc<WsConn>) {
    let mut session: Option<Session> = None;
    let mut peers: HashMap<u64, Peer> = HashMap::new();
    let mut up = HashMap::new();
    let mut credentials: Option<Value> = None;
    let direct = std::env::var("DHD_NET_P2P_UDP").map_or(true, |s| s != "0");
    let mut clock_offset = 0i64;
    let mut stun: Option<SocketAddr> = None;
    let weak = Arc::downgrade(&c);
    let wake: crate::peer::Wake = Arc::new(move || {
        if let Some(c) = weak.upgrade() {
            c.wake.notify_all();
        }
    });
    while !c.stopping.load(Ordering::Acquire) {
        let now = (epoch() as i64 + clock_offset).max(0) as u64;
        let incoming: Vec<_> = lock(&c.queue).incoming.drain(..).collect();
        for (binary, data) in incoming {
            if binary {
                deliver(&c, true, &data);
            } else if let Ok(m) = serde_json::from_slice::<Value>(&data) {
                match m["type"].as_str() {
                    Some("p2p_welcome") => {
                        clock_offset =
                            m["serverTime"].as_u64().unwrap_or(epoch()) as i64 - epoch() as i64;
                        session = Some(Session::new(&m, now));
                        stun = ("stun.cloudflare.com", 3478)
                            .to_socket_addrs()
                            .ok()
                            .and_then(|a| a.filter(|a| a.is_ipv4()).next());
                        eprintln!(
                            "net: P2P room {}, member {}, host {}",
                            m["code"], m["memberId"], m["hostId"]
                        );
                    }
                    Some("p2p_members") => {
                        if let Some(s) = session.as_mut() {
                            s.members(&m["members"], now);
                            let members = m["members"].as_array().cloned().unwrap_or_default();
                            let ids: Vec<_> =
                                members.iter().filter_map(|m| m["id"].as_u64()).collect();
                            peers.retain(|id, _| ids.contains(id));
                            let local = lock(&c.live)
                                .as_ref()
                                .and_then(|l| l.tcp.local_addr().ok())
                                .map(|a| a.ip());
                            for id in ids {
                                if id == s.me
                                    || (!s.is_host() && id != s.host)
                                    || peers.contains_key(&id)
                                {
                                    continue;
                                }
                                if let Some(local) = local {
                                    match Peer::new(local, s.is_host(), direct) {
                                        Ok(mut p) => {
                                            if let Some(stun) = stun {
                                                let _ = p.discover(stun);
                                            }
                                            if let Some(creds) = &credentials {
                                                let _ = p.turn(creds, wake.clone());
                                            }
                                            peers.insert(id, p);
                                            up.insert(id, false);
                                        }
                                        Err(e) => eprintln!("net: P2P peer setup failed: {e}"),
                                    }
                                }
                            }
                        }
                    }
                    Some("p2p_signal") => {
                        if let Some(id) = m["from"].as_u64() {
                            if let Some(p) = peers.get_mut(&id) {
                                if let Err(e) = p.signal(&m["signal"]) {
                                    eprintln!("net: P2P signaling failed for {id}: {e}");
                                }
                            }
                        }
                    }
                    Some("p2p_turn") => {
                        for p in peers.values_mut() {
                            if let Err(e) = p.turn(&m, wake.clone()) {
                                eprintln!("net: P2P relay setup failed: {e}");
                            }
                        }
                        credentials = Some(m);
                    }
                    _ => deliver(&c, false, &data),
                }
            }
            lock(&c.queue).handled += 1;
            c.wake.notify_all();
        }
        let outgoing: Vec<_> = lock(&c.queue).out.drain(..).collect();
        for (binary, data) in outgoing {
            if binary && matches!(data.first(), Some(5 | 6)) {
                if let Some(s) = session.as_mut() {
                    s.frame(&data);
                }
            } else if !binary {
                if let Ok(m) = serde_json::from_slice::<Value>(&data) {
                    if matches!(
                        m["type"].as_str(),
                        Some("hello" | "ping" | "pong" | "leave")
                    ) {
                        service(&c, false, data);
                    } else if let Some(s) = session.as_mut() {
                        s.local(m, now);
                    }
                }
            } else {
                service(&c, true, data);
            }
        }
        let mut failed = vec![];
        for (&id, p) in &mut peers {
            match p.poll() {
                Ok((signals, frames)) => {
                    for signal in signals {
                        service(
                            &c,
                            false,
                            json!({"type":"p2p_signal","to":id,"signal":signal})
                                .to_string()
                                .into_bytes(),
                        );
                    }
                    if p.connected() && !up[&id] {
                        up.insert(id, true);
                        eprintln!("net: P2P peer {id} connected");
                        if let Some(s) = session.as_mut() {
                            s.link(id, true, now);
                        }
                    }
                    if let Some(s) = session.as_mut() {
                        for (reliable, bytes) in frames {
                            s.receive(id, reliable, bytes, now);
                        }
                    }
                    if p.timed_out() {
                        failed.push(id);
                    }
                }
                Err(e) => {
                    eprintln!("net: P2P peer {id} failed: {e}");
                    failed.push(id);
                }
            }
        }
        for id in failed {
            peers.remove(&id);
            up.remove(&id);
            if let Some(s) = session.as_mut() {
                s.link(id, false, now);
            }
        }
        if let Some(s) = session.as_mut() {
            s.tick(now);
            for action in s.take() {
                match action {
                    Action::Client(m) => deliver(&c, false, m.to_string().as_bytes()),
                    Action::Frame(bytes) => deliver(&c, true, &bytes),
                    Action::Service(m) => service(&c, false, m.to_string().into_bytes()),
                    Action::Peer(id, reliable, bytes) => {
                        if let Some(p) = peers.get_mut(&id) {
                            if let Err(e) = p.send(reliable, bytes) {
                                eprintln!("net: P2P peer {id} queue failed: {e}");
                            }
                        }
                    }
                }
            }
        }
        if let Some(f) = c.h.on_tick {
            if unsafe { f(c.h.ctx) } != 0 {
                c.force_close();
            }
        }
        let q = lock(&c.queue);
        let _wait = c
            .wake
            .wait_timeout(q, Duration::from_millis(2))
            .unwrap_or_else(|e| e.into_inner());
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
    struct Rx {
        messages: Vec<Value>,
        frames: Vec<Vec<u8>>,
    }
    unsafe extern "C" fn open(ctx: *mut c_void, ws: *mut WsConn) {
        let _ = ctx;
        let hello=json!({"type":"hello","protocol":4,"version":"p2p-smoke","clientVersion":"0.7.13","isoSha256":"a".repeat(64),"name":"TEST","features":["session","countdown"]}).to_string();
        unsafe { ws_send(ws, 0, hello.as_ptr().cast(), hello.len()) };
    }
    unsafe extern "C" fn message(ctx: *mut c_void, binary: i32, data: *const u8, n: usize) {
        let rx = unsafe { &*(ctx as *const Mutex<Rx>) };
        let bytes = unsafe { std::slice::from_raw_parts(data, n) };
        if binary != 0 {
            rx.lock().unwrap().frames.push(bytes.to_vec());
        } else if let Ok(m) = serde_json::from_slice::<Value>(bytes) {
            rx.lock().unwrap().messages.push(m);
        }
    }
    unsafe extern "C" fn closed(_: *mut c_void, _: i32, _: *const c_char, _: i32) -> *const c_char {
        std::ptr::null()
    }
    struct Client {
        conn: *mut WsConn,
        rx: Box<Mutex<Rx>>,
    }
    impl Client {
        fn new(url: &str) -> Self {
            let mut rx = Box::<Mutex<Rx>>::default();
            let h = WsHandler {
                ctx: (&mut *rx as *mut Mutex<Rx>).cast(),
                on_open: Some(open),
                on_message: Some(message),
                on_close: Some(closed),
                on_tick: None,
            };
            let url = CString::new(url).unwrap();
            let conn = unsafe { ws_start(url.as_ptr(), &h) };
            assert!(!conn.is_null());
            Self { conn, rx }
        }
        fn send(&self, m: Value) {
            let bytes = m.to_string();
            unsafe { ws_send(self.conn, 0, bytes.as_ptr().cast(), bytes.len()) };
        }
        fn find(&self, kind: &str) -> Option<Value> {
            self.rx
                .lock()
                .unwrap()
                .messages
                .iter()
                .rev()
                .find(|m| m["type"] == kind)
                .cloned()
        }
    }
    impl Drop for Client {
        fn drop(&mut self) {
            unsafe { ws_stop(self.conn) };
        }
    }
    fn wait(mut f: impl FnMut() -> bool) {
        let end = Instant::now() + Duration::from_secs(25);
        while Instant::now() < end {
            if f() {
                return;
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        panic!("P2P room test timed out");
    }
    #[test]
    #[ignore = "run through local_server.py"]
    fn p2p_room_ten_clients() {
        let base = std::env::var("DHD_NET_SERVER").unwrap();
        let host = Client::new(&format!("{base}/v1/p2p/rooms/new"));
        wait(|| host.find("welcome").is_some());
        let code = host.find("welcome").unwrap()["code"].as_str().unwrap().to_owned();
        let mut clients = vec![host];
        for _ in 1..10 { clients.push(Client::new(&format!("{base}/v1/p2p/rooms/{code}"))); }
        wait(|| clients.iter().all(|c| c.find("room").is_some_and(|m| m["members"].as_array().is_some_and(|a| a.len()==10))));
        for c in &clients { c.send(json!({"type":"ready","ready":true})); }
        wait(|| clients.iter().all(|c| c.find("start").is_some()));
        eprintln!("P2P TEN: all clients started");
        let mut frame = vec![0u8;24];frame[0]=5;frame[2]=6;
        wait(|| {
            for c in &clients { unsafe {ws_send(c.conn,1,frame.as_ptr().cast(),frame.len())}; }
            clients.iter().all(|c| {
                let rx=c.rx.lock().unwrap();
                let senders: std::collections::HashSet<_>=rx.frames.iter().map(|f|f[1]).collect();
                senders.len()==9
            })
        });
        for (i,c) in clients.iter().enumerate() { c.send(json!({"type":"finish","ticks":100+i,"place":i+1})); }
        wait(|| clients.iter().all(|c|c.find("ended").is_some()));
        eprintln!("P2P TEN MATCH: all ten clients received the other nine riders and the race results");
    }
    #[test]
    #[ignore = "run through local_server.py"]
    fn p2p_room_smoke() {
        let base =
            std::env::var("DHD_NET_SERVER").expect("local_server.py supplies DHD_NET_SERVER");
        let host = Client::new(&format!("{base}/v1/p2p/rooms/new"));
        wait(|| host.find("welcome").is_some());
        let code = host.find("welcome").unwrap()["code"]
            .as_str()
            .unwrap()
            .to_owned();
        let guest = Client::new(&format!("{base}/v1/p2p/rooms/{code}"));
        wait(|| {
            guest
                .find("room")
                .is_some_and(|m| m["members"].as_array().is_some_and(|m| m.len() == 2))
        });
        host.send(json!({"type":"ready","ready":true}));
        guest.send(json!({"type":"ready","ready":true}));
        wait(|| host.find("start").is_some() && guest.find("start").is_some());
        let mut frame = vec![0u8; 24];
        frame[0] = 5;
        frame[2] = 6;
        frame[4] = 1;
        frame[5..13].copy_from_slice(&(epoch() as f64).to_le_bytes());
        wait(|| {
            unsafe { ws_send(guest.conn, 1, frame.as_ptr().cast(), frame.len()) };
            !host.rx.lock().unwrap().frames.is_empty()
        });
        assert_eq!(host.rx.lock().unwrap().frames[0][1], 1);
        host.send(json!({"type":"finish","ticks":100,"place":1}));
        guest.send(json!({"type":"finish","ticks":110,"place":2}));
        wait(|| host.find("ended").is_some() && guest.find("ended").is_some());
        eprintln!("P2P ROOM MATCH: two native clients joined, readied, started, exchanged snapshots and collected results");
    }
}
