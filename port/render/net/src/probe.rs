//! A bare transport probe (finding 158, round 3): two clients through the real Cloudflare Realtime
//! SFU with no game. Each client is the game's own stack (`ws_start`, its send thread, `sfu::Sfu`);
//! this test plays the room's Durable Object on a local WebSocket and calls the Realtime API
//! itself. Both clients live in this process, so one clock times the one-way delay.
//!
//! Test-only and ignored: it needs the network and the SFU app's credentials, read at run time
//! from REALTIME_APP_ID / REALTIME_APP_TOKEN or the server's gitignored `online/server/.dev.vars`.
//! Nothing is printed of them. Run:
//!
//! `python port/tools/rustgs.py test -- -p dhd_net --lib probe -- --ignored --nocapture`
//!
//! DHD_PROBE_SECONDS (default 30), DHD_PROBE_RIDERS (snapshots per 30 Hz tick, default 1; 10 is a
//! host with nine AI riders), DHD_PROBE_RIDERS_B (the second client's, default the same),
//! DHD_PROBE_BYTES (snapshot size, default 150). Runs longer than 30 s also print each 30 s window.
//!
//! The TURN fallback (round 4): with DHD_NET_SFU_UDP=0 both clients drop every UDP datagram to and
//! from the SFU, ask this stand-in room for TURN credentials after the direct timeout, and reach
//! the SFU through Cloudflare's TURN service over TLS on 443. The stand-in mints them with the TURN
//! key (TURN_KEY_ID / TURN_KEY_API_TOKEN, same sources) and revokes them at the end.
use super::Sfu;
use crate::{ws_send, ws_start, ws_stop, WsConn, WsHandler};
use serde_json::{json, Value};
use std::ffi::{c_char, c_int, c_void, CString};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{mpsc, Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};
use tungstenite::Message;

const RACE: u64 = 77_000_001;

fn epoch() -> Instant {
    static E: OnceLock<Instant> = OnceLock::new();
    *E.get_or_init(Instant::now)
}

/// A secret from the environment, else from the server's gitignored `.dev.vars`; never printed.
fn secret(k: &str) -> String {
    if let Ok(v) = std::env::var(k) { return v; }
    let dir = std::env::var("CARGO_MANIFEST_DIR").expect("run through cargo");
    let file = std::path::Path::new(&dir).join("../../../online/server/.dev.vars");
    let text = std::fs::read_to_string(file).expect("online/server/.dev.vars");
    text.lines()
        .find_map(|l| l.trim().strip_prefix(k).and_then(|r| r.trim_start().strip_prefix('=')))
        .map(|v| v.trim().trim_matches('"').to_string())
        .expect("credential missing")
}

/// One Realtime API call (HTTP/1.1, Connection: close).
fn api(method: &str, path: &str, body: &Value) -> Value {
    let (app, token) = (secret("REALTIME_APP_ID"), secret("REALTIME_APP_TOKEN"));
    call(method, &format!("/v1/apps/{app}/{path}"), &token, body)
}

/// TURN credentials for one client, as the room mints them (online/server/src/sfu.ts Turn).
pub(crate) fn turn_credentials(tag: &str) -> (String, Value) {
    turn_credentials_lasting(tag, 3600)
}

/// Credentials valid for `ttl` seconds (peer.rs's renewal test: one that expires during the test).
pub(crate) fn turn_credentials_lasting(tag: &str, ttl: u32) -> (String, Value) {
    let (key, token) = (secret("TURN_KEY_ID"), secret("TURN_KEY_API_TOKEN"));
    let r = call("POST", &format!("/v1/turn/keys/{key}/credentials/generate-ice-servers"), &token, &json!({"ttl": ttl, "customIdentifier": tag}));
    let server = r["iceServers"].as_array().unwrap().iter().find(|s| s["username"].is_string()).unwrap().clone();
    let urls: Vec<Value> = server["urls"].as_array().unwrap().iter().filter(|u| u.as_str().is_some_and(|u| u.starts_with("turns:"))).cloned().collect();
    let user = server["username"].as_str().unwrap().to_string();
    (user.clone(), json!({"type":"sfu_turn","urls":urls,"username":user,"credential":server["credential"]}))
}

/// Best effort: an expired credential is no longer found (a test's cleanup must not panic).
pub(crate) fn revoke_turn(user: &str) {
    let (key, token) = (secret("TURN_KEY_ID"), secret("TURN_KEY_API_TOKEN"));
    if let Err(e) = try_call("POST", &format!("/v1/turn/keys/{key}/credentials/{user}/revoke"), &token, &Value::Null) {
        eprintln!("TURN revoke: {e}");
    }
}

/// One HTTPS call to rtc.live.cloudflare.com; panics on a status outside 2xx. A 204 is Null.
fn call(method: &str, path: &str, token: &str, body: &Value) -> Value {
    match try_call(method, path, token, body) {
        Ok(v) => v,
        Err(e) => panic!("{e}"),
    }
}

/// The same call, its failure returned: a status outside 2xx, an errorCode, no answer.
fn try_call(method: &str, path: &str, token: &str, body: &Value) -> std::result::Result<Value, String> {
    let uri: tungstenite::http::Uri = "https://rtc.live.cloudflare.com/".parse().unwrap();
    let (mut s, tcp) = crate::stream::open(&uri, true).map_err(|_| "Realtime API connection".to_string())?;
    let _ = tcp.set_read_timeout(Some(Duration::from_secs(15)));
    let body = if body.is_null() { String::new() } else { body.to_string() };
    let request = format!(
        "{method} {path} HTTP/1.1\r\nHost: rtc.live.cloudflare.com\r\nAuthorization: Bearer {token}\r\n\
         Content-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    );
    s.write_all(request.as_bytes()).and_then(|_| s.flush()).map_err(|_| "Realtime API write".to_string())?;
    let mut data = Vec::new();
    let mut buf = [0u8; 16384];
    while let Ok(n) = s.read(&mut buf) {
        if n == 0 { break; }
        data.extend_from_slice(&buf[..n]);
    }
    let mut headers = [httparse::EMPTY_HEADER; 64];
    let mut r = httparse::Response::new(&mut headers);
    let start = match r.parse(&data) { Ok(httparse::Status::Complete(n)) => n, _ => return Err("no HTTP response".into()) };
    let chunked = r.headers.iter().any(|h| h.name.eq_ignore_ascii_case("transfer-encoding")
        && String::from_utf8_lossy(h.value).to_ascii_lowercase().contains("chunked"));
    let what = path.rsplit('/').next().unwrap_or("");
    if !r.code.is_some_and(|c| (200..300).contains(&c)) { return Err(format!("Realtime API {what}: {:?}", r.code)); }
    if r.code == Some(204) { return Ok(Value::Null); }
    let body = if chunked { crate::http::dechunk(&data[start..]).map_err(|_| "bad chunks".to_string())? } else { data[start..].to_vec() };
    let v: Value = serde_json::from_slice(&body).map_err(|_| "Realtime JSON".to_string())?;
    if let Some(code) = v.get("errorCode") { return Err(format!("Realtime error on {what}: {code}")); }
    Ok(v)
}

fn allocate(session: &str, member: u64, publisher: Option<&str>) -> Vec<Value> {
    let policies = [json!({"dataChannelName":"state","ordered":false,"maxRetransmits":0}),
                    json!({"dataChannelName":"events","ordered":true})];
    let channels: Vec<Value> = policies.iter().map(|p| {
        let mut p = p.clone();
        p["location"] = json!(if publisher.is_some() { "remote" } else { "local" });
        if let Some(s) = publisher { p["sessionId"] = json!(s); }
        p
    }).collect();
    let r = api("POST", &format!("sessions/{session}/datachannels/new"), &json!({"dataChannels": channels}));
    r["dataChannels"].as_array().unwrap().iter().map(|c| json!({
        "channelId": c["id"], "name": c["dataChannelName"], "memberId": member, "local": publisher.is_none(),
    })).collect()
}

/// What one client received, filled by its on_message callback.
#[derive(Default)]
struct Rx {
    open: bool,
    ready: bool,
    /// (send tick, one-way ms) of every snapshot
    delays: Vec<(u32, f64)>,
    last_arrival: Option<Instant>,
    /// (send tick, arrival interval ms) of rider 0
    gaps: Vec<(u32, f64)>,
    seqs: std::collections::HashSet<u32>,
}

unsafe extern "C" fn on_open(ctx: *mut c_void, _ws: *mut WsConn) {
    let rx = unsafe { &*(ctx as *const Mutex<Rx>) };
    rx.lock().unwrap().open = true;
}
unsafe extern "C" fn on_message(ctx: *mut c_void, binary: c_int, data: *const u8, n: usize) {
    let now = Instant::now();
    let rx = unsafe { &*(ctx as *const Mutex<Rx>) };
    let bytes = unsafe { std::slice::from_raw_parts(data, n) };
    let mut rx = rx.lock().unwrap();
    if binary == 0 {
        if String::from_utf8_lossy(bytes).contains("\"sfu_ready\"") { rx.ready = true; }
        return;
    }
    rx.record(bytes, now);
}

impl Rx {
    /// A snapshot `[5, slot, 0, rider, seq u32, sent nanos u64, ...]` arriving at `now`.
    fn record(&mut self, bytes: &[u8], now: Instant) {
        if bytes.len() < 16 || bytes[0] != 5 { return; }
        let seq = u32::from_le_bytes(bytes[4..8].try_into().unwrap());
        let sent = u64::from_le_bytes(bytes[8..16].try_into().unwrap());
        let at = now.duration_since(epoch()).as_nanos() as u64;
        self.delays.push((seq, at.saturating_sub(sent) as f64 / 1e6));
        if bytes[3] == 0 {
            if let Some(last) = self.last_arrival { self.gaps.push((seq, now.duration_since(last).as_secs_f64() * 1e3)); }
            self.last_arrival = Some(now);
            self.seqs.insert(seq);
        }
    }
    fn clear(&mut self) { self.delays.clear(); self.gaps.clear(); self.last_arrival = None; self.seqs.clear(); }
    /// The whole run, then each 30 s window (as the game's net: lines), when longer than one.
    fn report(&self, name: &str, ticks: u64) {
        let line = |label: String, from: u32, to: u32| {
            let mut d: Vec<f64> = self.delays.iter().filter(|(t, _)| (from..to).contains(t)).map(|x| x.1).collect();
            d.sort_by(|a, b| a.partial_cmp(b).unwrap());
            let gaps: Vec<f64> = self.gaps.iter().filter(|(t, _)| (from..to).contains(t)).map(|x| x.1).collect();
            let over100 = gaps.iter().filter(|g| **g > 100.0).count();
            let max_gap = gaps.iter().cloned().fold(0.0, f64::max);
            let got = self.seqs.iter().filter(|t| (from..to).contains(*t)).count();
            eprintln!(
                "probe: {label} received {got} of {} rider-0 snapshots ({} frames); one-way ms p50 {:.1} p95 {:.1} p99 {:.1} max {:.1}; gaps >100 ms {over100}, longest gap {max_gap:.1} ms",
                to - from, d.len(), percentile(&d, 0.5), percentile(&d, 0.95), percentile(&d, 0.99), d.last().copied().unwrap_or(f64::NAN)
            );
        };
        line(name.to_string(), 0, ticks as u32);
        if ticks > 900 {
            for w in 0..ticks.div_ceil(900) as u32 {
                line(format!("{name} window {}", w + 1), w * 900, ((w + 1) * 900).min(ticks as u32));
            }
        }
    }
}

/// The snapshot one client sends for `rider` on `tick`.
fn snapshot(me: usize, rider: u8, tick: u64, size: usize) -> Vec<u8> {
    let mut b = vec![0u8; size];
    b[0] = 5; b[1] = me as u8; b[3] = rider;
    b[4..8].copy_from_slice(&(tick as u32).to_le_bytes());
    b[8..16].copy_from_slice(&(Instant::now().duration_since(epoch()).as_nanos() as u64).to_le_bytes());
    b
}

/// Waits for a 30 Hz tick without the OS timer's granularity.
fn wait_until(due: Instant) {
    while Instant::now() < due {
        let left = due - Instant::now();
        if left > Duration::from_millis(2) { std::thread::sleep(left - Duration::from_millis(2)); } else { std::hint::spin_loop(); }
    }
}
unsafe extern "C" fn on_close(_: *mut c_void, _: c_int, _: *const c_char, _: c_int) -> *const c_char {
    std::ptr::null()
}

/// The fake Durable Object: one thread per client connection, the Realtime calls inline.
#[derive(Default)]
struct Room {
    sessions: [Option<String>; 2],
    published: [bool; 2],
    ready: [bool; 2],
    out: Vec<mpsc::Sender<String>>,
    turn_users: Vec<String>,
}

fn serve(room: Arc<Mutex<Room>>, me: usize, mut ws: tungstenite::WebSocket<TcpStream>, out: mpsc::Receiver<String>) {
    ws.get_mut().set_read_timeout(Some(Duration::from_millis(5))).unwrap();
    loop {
        while let Ok(text) = out.try_recv() {
            if ws.send(Message::text(text)).is_err() { return; }
        }
        let message = match ws.read() {
            Ok(Message::Text(t)) => t.to_string(),
            Ok(Message::Close(_)) => return,
            Ok(_) => continue,
            Err(tungstenite::Error::Io(e)) if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => continue,
            Err(_) => return,
        };
        let m: Value = serde_json::from_str(&message).unwrap();
        let member = me as u64 + 1;
        let send = |room: &Room, who: usize, v: Value| { let _ = room.out[who].send(v.to_string()); };
        match m["type"].as_str() {
            Some("sfu_offer") => {
                let r = api("POST", "sessions/new", &json!({"sessionDescription":{"type":"offer","sdp":m["sdp"]}}));
                let mut room = room.lock().unwrap();
                room.sessions[me] = Some(r["sessionId"].as_str().unwrap().to_string());
                send(&room, me, json!({"type":"sfu_answer","sdp":r["sessionDescription"]["sdp"]}));
            }
            Some("sfu_connected") => {
                let session = room.lock().unwrap().sessions[me].clone().unwrap();
                let mine = allocate(&session, member, None);
                let mut r = room.lock().unwrap();
                send(&r, me, json!({"type":"sfu_channels","channels":mine}));
                r.published[me] = true;
                let other = 1 - me;
                if r.published[other] {
                    let theirs_session = r.sessions[other].clone().unwrap();
                    drop(r);
                    let sub_me = allocate(&session, other as u64 + 1, Some(&theirs_session));
                    let sub_other = allocate(&theirs_session, member, Some(&session));
                    let r = room.lock().unwrap();
                    send(&r, me, json!({"type":"sfu_channels","channels":sub_me}));
                    send(&r, other, json!({"type":"sfu_channels","channels":sub_other}));
                }
            }
            Some("sfu_turn") => {
                let (user, message) = turn_credentials(&format!("probe-m{member}"));
                let mut r = room.lock().unwrap();
                r.turn_users.push(user);
                eprintln!("probe: client {me} asked for TURN credentials");
                send(&r, me, message);
            }
            Some("sfu_ready") => {
                let mut r = room.lock().unwrap();
                r.ready[me] = true;
                if r.ready == [true, true] {
                    for who in 0..2 { send(&r, who, json!({"type":"sfu_ready"})); }
                }
            }
            _ => {}
        }
    }
}

fn percentile(sorted: &[f64], p: f64) -> f64 {
    if sorted.is_empty() { return f64::NAN; }
    sorted[((sorted.len() as f64 - 1.0) * p).round() as usize]
}

fn env_or(name: &str, default: u64) -> u64 {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

#[test]
#[ignore = "needs the network and Realtime credentials"]
fn probe_sfu_one_way_delay() {
    epoch();
    let seconds = env_or("DHD_PROBE_SECONDS", 30);
    let riders = env_or("DHD_PROBE_RIDERS", 1).clamp(1, 10) as u8;
    let riders = [riders, env_or("DHD_PROBE_RIDERS_B", riders as u64).clamp(1, 10) as u8];
    let size = env_or("DHD_PROBE_BYTES", 150).clamp(16, 834) as usize;
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = CString::new(format!("ws://{}/", listener.local_addr().unwrap())).unwrap();
    let room = Arc::new(Mutex::new(Room::default()));
    let rx: Vec<Arc<Mutex<Rx>>> = (0..2).map(|_| Arc::new(Mutex::new(Rx::default()))).collect();
    let mut clients = vec![];
    let mut servers = vec![];
    for (me, rx) in rx.iter().enumerate() {
        let h = WsHandler { ctx: Arc::as_ptr(rx) as *mut c_void, on_open: Some(on_open), on_message: Some(on_message), on_close: Some(on_close), on_tick: None };
        clients.push(unsafe { ws_start(url.as_ptr(), &h) } as usize);
        let (stream, _) = listener.accept().unwrap();
        let ws = tungstenite::accept(stream).unwrap();
        let (tx, out) = mpsc::channel();
        room.lock().unwrap().out.push(tx);
        let r = room.clone();
        servers.push(std::thread::spawn(move || serve(r, me, ws, out)));
    }
    let until = Instant::now() + Duration::from_secs(10);
    while !rx.iter().all(|r| r.lock().unwrap().open) { assert!(Instant::now() < until, "no WebSocket"); std::thread::sleep(Duration::from_millis(5)); }
    let players = json!([{"memberId":1,"slot":0},{"memberId":2,"slot":1}]);
    {
        let r = room.lock().unwrap();
        for who in 0..2 { r.out[who].send(json!({"type":"start","startAt":RACE,"players":players}).to_string()).unwrap(); }
    }
    let setup = Instant::now();
    let until = setup + Duration::from_secs(40);
    while !rx.iter().all(|r| r.lock().unwrap().ready) { assert!(Instant::now() < until, "SFU setup timed out"); std::thread::sleep(Duration::from_millis(5)); }
    eprintln!("probe: SFU ready {} ms after start; {seconds} s at 30 Hz, {riders:?} snapshot(s) of {size} bytes per tick per client",
        setup.elapsed().as_millis());
    std::thread::sleep(Duration::from_millis(500));
    for r in &rx { r.lock().unwrap().clear(); }
    let start = Instant::now();
    let ticks = seconds * 30;
    for tick in 0..ticks {
        wait_until(start + Duration::from_micros(tick * 33_333));
        for (me, c) in clients.iter().enumerate() {
            for rider in 0..riders[me] {
                let b = snapshot(me, rider, tick, size);
                unsafe { ws_send(*c as *mut WsConn, 1, b.as_ptr() as *const c_void, b.len()) };
            }
        }
    }
    std::thread::sleep(Duration::from_millis(1500));
    for (me, r) in rx.iter().enumerate() { r.lock().unwrap().report(&format!("client {me}"), ticks); }
    for c in clients { unsafe { ws_stop(c as *mut WsConn) }; }
    for s in servers { let _ = s.join(); }
    let users = std::mem::take(&mut room.lock().unwrap().turn_users);
    for user in &users { revoke_turn(user); }
    if !users.is_empty() { eprintln!("probe: revoked {} TURN credential(s)", users.len()); }
}

/// The same two sessions with no thread loop: one thread sends on each tick and busy-polls both
/// `Sfu`s (DHD_PROBE_SLEEP_US between rounds, default 0). The floor our own loop is compared with.
#[test]
#[ignore = "needs the network and Realtime credentials"]
fn probe_sfu_direct() {
    epoch();
    let seconds = env_or("DHD_PROBE_SECONDS", 30);
    let riders = env_or("DHD_PROBE_RIDERS", 1).clamp(1, 10) as u8;
    let riders = [riders, env_or("DHD_PROBE_RIDERS_B", riders as u64).clamp(1, 10) as u8];
    let size = env_or("DHD_PROBE_BYTES", 150).clamp(16, 834) as usize;
    let nap = Duration::from_micros(env_or("DHD_PROBE_SLEEP_US", 0));
    let mut c: Vec<Sfu> = vec![];
    let mut sessions = vec![];
    for _ in 0..2 {
        let (mut sfu, offer) = Sfu::start(Arc::new(|| {}), false).unwrap();
        let r = api("POST", "sessions/new", &json!({"sessionDescription":{"type":"offer","sdp":offer["sdp"]}}));
        sessions.push(r["sessionId"].as_str().unwrap().to_string());
        sfu.signal(&json!({"type":"sfu_answer","sdp":r["sessionDescription"]["sdp"]})).unwrap();
        sfu.signal(&json!({"type":"start","startAt":RACE,"players":[{"memberId":1,"slot":0},{"memberId":2,"slot":1}]})).unwrap();
        c.push(sfu);
    }
    let mut rx = [Rx::default(), Rx::default()];
    let poll = |c: &mut Vec<Sfu>, rx: &mut [Rx; 2]| -> [bool; 2] {
        let mut text = [false; 2];
        for who in 0..2 {
            let (t, frames) = c[who].poll().unwrap();
            text[who] = t.iter().any(|m| m["type"] == "sfu_connected");
            let now = Instant::now();
            for f in frames { rx[who].record(&f, now); }
        }
        text
    };
    let until = Instant::now() + Duration::from_secs(15);
    let mut connected = [false; 2];
    while connected != [true, true] {
        assert!(Instant::now() < until, "SFU connect timed out");
        let t = poll(&mut c, &mut rx);
        for who in 0..2 { connected[who] |= t[who]; }
        std::thread::sleep(Duration::from_millis(1));
    }
    for who in 0..2 {
        let channels = allocate(&sessions[who], who as u64 + 1, None);
        c[who].signal(&json!({"type":"sfu_channels","channels":channels})).unwrap();
    }
    for who in 0..2 {
        let other = 1 - who;
        let channels = allocate(&sessions[who], other as u64 + 1, Some(&sessions[other]));
        c[who].signal(&json!({"type":"sfu_channels","channels":channels})).unwrap();
    }
    let until = Instant::now() + Duration::from_secs(15);
    while !(c[0].usable() && c[1].usable()) {
        assert!(Instant::now() < until, "channels did not open");
        poll(&mut c, &mut rx);
        std::thread::sleep(Duration::from_millis(1));
    }
    for s in c.iter_mut() { s.released = true; }
    eprintln!("probe: direct, SFU ready; {seconds} s at 30 Hz, {riders:?} snapshot(s) of {size} bytes per tick per client, {} us between polls", nap.as_micros());
    let settle = Instant::now() + Duration::from_millis(500);
    while Instant::now() < settle { poll(&mut c, &mut rx); }
    for r in rx.iter_mut() { r.clear(); }
    let start = Instant::now();
    let ticks = seconds * 30;
    let mut tick = 0;
    let end = start + Duration::from_micros(ticks * 33_333 + 1_500_000);
    let mut last = Instant::now();
    let mut loop_max = Duration::ZERO;
    let mut loops_over_5ms = 0;
    while Instant::now() < end {
        let now = Instant::now();
        loop_max = loop_max.max(now - last);
        if now - last > Duration::from_millis(5) { loops_over_5ms += 1; }
        last = now;
        if tick < ticks && Instant::now() >= start + Duration::from_micros(tick * 33_333) {
            for who in 0..2 {
                for rider in 0..riders[who] { c[who].send(&snapshot(who, rider, tick, size)).unwrap(); }
            }
            tick += 1;
        }
        poll(&mut c, &mut rx);
        if !nap.is_zero() { std::thread::sleep(nap); }
    }
    for (who, r) in rx.iter().enumerate() { r.report(&format!("direct client {who}"), ticks); }
    eprintln!("probe: direct loop: longest round {:.1} ms, {loops_over_5ms} rounds over 5 ms", loop_max.as_secs_f64() * 1e3);
    for (who, s) in c.iter().enumerate() {
        let mut tx: Vec<f64> = s.probe.tx_lag.clone();
        tx.sort_by(|a, b| a.partial_cmp(b).unwrap());
        eprintln!("probe: direct client {who} send(): write to first datagram ms p50 {:.2} p99 {:.2} max {:.2} ({} writes)",
            percentile(&tx, 0.5), percentile(&tx, 0.99), tx.last().copied().unwrap_or(f64::NAN), tx.len());
    }
}

/// The TURN relay alone (round 4): credentials as the room mints them, an allocation over TLS on
/// 443, then a channel to a fresh SFU session's candidate (the SFU's own address); each step timed.
/// DHD_PROBE_PEER=ip:port binds to another peer instead.
#[test]
#[ignore = "needs the network and TURN credentials"]
fn probe_turn_allocate() {
    let (user, message) = turn_credentials("probe-allocate");
    let (host, port) = super::turns_server(&message["urls"]).unwrap();
    let start = Instant::now();
    let link = crate::relay::Link::open(&host, port, Arc::new(|| {}));
    let mut client = crate::turn::Client::new(&user, message["credential"].as_str().unwrap(), Instant::now());
    let until = start + Duration::from_secs(15);
    while !link.ready() { assert!(!link.failed() && Instant::now() < until, "TLS link"); std::thread::sleep(Duration::from_millis(1)); }
    eprintln!("probe: TLS to {host}:{port} ready after {} ms", start.elapsed().as_millis());
    let pump = |client: &mut crate::turn::Client| {
        link.write(&client.output());
        let (bytes, _) = link.take();
        if !bytes.is_empty() { client.receive(&bytes, Instant::now()); }
        client.tick(Instant::now());
        std::thread::sleep(Duration::from_millis(1));
    };
    while client.relayed.is_none() {
        assert!(Instant::now() < until && client.failed().is_none() && !link.failed(), "allocation: {:?}", client.failed());
        pump(&mut client);
    }
    eprintln!("probe: TURN allocation after {} ms", start.elapsed().as_millis());
    let (_sfu, offer) = Sfu::start(Arc::new(|| {}), false).unwrap();
    let answer = api("POST", "sessions/new", &json!({"sessionDescription":{"type":"offer","sdp":offer["sdp"]}}));
    let peer = std::env::var("DHD_PROBE_PEER").ok().and_then(|p| p.parse().ok()).unwrap_or_else(|| {
        let line = answer["sessionDescription"]["sdp"].as_str().unwrap().lines()
            .find(|l| l.starts_with("a=candidate:") && l.split_whitespace().nth(2).is_some_and(|t| t.eq_ignore_ascii_case("udp"))).unwrap().to_string();
        let p: Vec<&str> = line.split_whitespace().collect();
        format!("{}:{}", p[4], p[5]).parse().unwrap()
    });
    let bind = Instant::now();
    client.send_to(peer, &[0u8; 20], bind);
    while client.output_pending() || client.binding() {
        assert!(Instant::now() < bind + Duration::from_secs(5) && client.failed().is_none(), "channel bind: {:?}", client.failed());
        pump(&mut client);
    }
    eprintln!("probe: channel to the SFU bound after {} ms", bind.elapsed().as_millis());
    client.close(Instant::now());
    link.write(&client.output());
    revoke_turn(&user);
}

/// TLS on 443 and the TURN framing with no credentials (round 4: the Mac and Linux check, which
/// must not have the TURN key): the first Allocate goes unauthenticated and Cloudflare's TURN
/// answers 401 with its realm and nonce; the retry with a made-up credential is refused (4xx).
#[test]
#[ignore = "needs the network"]
fn probe_turn_tls_no_credentials() {
    let start = Instant::now();
    let link = crate::relay::Link::open("turn.cloudflare.com", 443, Arc::new(|| {}));
    let mut client = crate::turn::Client::new("not-a-user", "not-a-credential", Instant::now());
    let until = start + Duration::from_secs(15);
    while !link.ready() { assert!(!link.failed() && Instant::now() < until, "TLS link"); std::thread::sleep(Duration::from_millis(1)); }
    eprintln!("probe: TLS to turn.cloudflare.com:443 ready after {} ms", start.elapsed().as_millis());
    while client.failed().is_none() {
        assert!(Instant::now() < until && !link.failed(), "no TURN answer");
        link.write(&client.output());
        let (bytes, _) = link.take();
        if !bytes.is_empty() { client.receive(&bytes, Instant::now()); }
        std::thread::sleep(Duration::from_millis(1));
    }
    assert!(client.challenged(), "no 401 challenge with a realm and nonce");
    // 401 or 400 (Cloudflare's answer to a username it can't parse): refused either way.
    assert!(client.failed().is_some_and(|f| f.starts_with("TURN error 4")), "{:?}", client.failed());
    eprintln!("probe: TURN challenge and refusal after {} ms", start.elapsed().as_millis());
}

/// Allocations on `session` (publications, or subscriptions to `publisher`), each item checked.
fn try_allocate(session: &str, member: u64, publisher: Option<&str>) -> std::result::Result<Vec<Value>, String> {
    let (app, token) = (secret("REALTIME_APP_ID"), secret("REALTIME_APP_TOKEN"));
    let policies = [json!({"dataChannelName":"state","ordered":false,"maxRetransmits":0}),
                    json!({"dataChannelName":"events","ordered":true})];
    let channels: Vec<Value> = policies.iter().map(|p| {
        let mut p = p.clone();
        p["location"] = json!(if publisher.is_some() { "remote" } else { "local" });
        if let Some(s) = publisher { p["sessionId"] = json!(s); }
        p
    }).collect();
    let r = try_call("POST", &format!("/v1/apps/{app}/sessions/{session}/datachannels/new"), &token, &json!({"dataChannels": channels}))?;
    let items = r["dataChannels"].as_array().ok_or("no dataChannels")?;
    if items.len() != 2 { return Err(format!("{} items", items.len())); }
    items.iter().map(|c| {
        if let Some(code) = c.get("errorCode") { return Err(format!("item error {code}")); }
        let id = c["id"].as_u64().filter(|id| *id < 65535).ok_or("no channel id")?;
        Ok(json!({"channelId": id, "name": c["dataChannelName"], "memberId": member, "local": publisher.is_none()}))
    }).collect()
}

/// Finding 177's question C: may one subscriber session take several datachannels/new calls at
/// once? Cloudflare asks to serialize a session's mutations (an SDP exchange must finish before
/// the next one); a DataChannel allocation on a connected transport has none. DHD_PROBE_CLIENTS
/// (default 5) connected sessions publish; then, each round, every subscriber subscribes to every
/// other publisher with DHD_PROBE_PARALLEL (default all) calls on its session at once, every
/// subscriber at the same time. Each round checks every call's result, that every channel opens,
/// and that an event from every publisher reaches every subscriber; then the subscriptions close
/// and the next round makes them again on the same sessions (DHD_PROBE_ROUNDS, default 20).
#[test]
#[ignore = "needs the network and Realtime credentials"]
fn probe_parallel_subscriptions() {
    let n = env_or("DHD_PROBE_CLIENTS", 5).clamp(2, 10) as usize;
    let rounds = env_or("DHD_PROBE_ROUNDS", 20);
    let parallel = env_or("DHD_PROBE_PARALLEL", n as u64 - 1).clamp(1, 9) as usize;

    let mut c: Vec<Sfu> = vec![];
    let mut sessions: Vec<String> = vec![];
    for _ in 0..n {
        let (mut sfu, offer) = Sfu::start(Arc::new(|| {}), false).unwrap();
        let r = api("POST", "sessions/new", &json!({"sessionDescription":{"type":"offer","sdp":offer["sdp"]}}));
        sessions.push(r["sessionId"].as_str().unwrap().to_string());
        sfu.signal(&json!({"type":"sfu_answer","sdp":r["sessionDescription"]["sdp"]})).unwrap();
        c.push(sfu);
    }
    let poll_all = |c: &mut Vec<Sfu>, got: &mut Vec<Vec<Vec<u8>>>| -> Vec<bool> {
        let mut connected = vec![false; c.len()];
        for (who, s) in c.iter_mut().enumerate() {
            let (text, frames) = s.poll().expect("association");
            connected[who] = text.iter().any(|m| m["type"] == "sfu_connected");
            got[who].extend(frames);
        }
        connected
    };
    let mut got: Vec<Vec<Vec<u8>>> = vec![vec![]; n];
    let until = Instant::now() + Duration::from_secs(20);
    let mut connected = vec![false; n];
    while connected.iter().any(|x| !x) {
        assert!(Instant::now() < until, "SFU connect timed out");
        for (k, x) in poll_all(&mut c, &mut got).into_iter().enumerate() { connected[k] |= x; }
        std::thread::sleep(Duration::from_millis(1));
    }
    for who in 0..n {
        let channels = allocate(&sessions[who], who as u64 + 1, None);
        c[who].signal(&json!({"type":"sfu_channels","channels":channels})).unwrap();
    }
    let players: Vec<Value> = (0..n).map(|who| json!({"memberId": who + 1, "slot": who})).collect();
    eprintln!("probe: {n} sessions connected and published; {rounds} rounds, {parallel} subscription call(s) at once per subscriber, every subscriber at once");
    let (mut calls, mut failed, mut reused, mut conflicts, mut undelivered, mut unopened) = (0, 0, 0, 0, 0, 0);
    let mut ms_all: Vec<f64> = vec![];
    let mut seen_ids: Vec<std::collections::HashSet<u64>> = vec![Default::default(); n];
    for round in 0..rounds {
        let started = Instant::now();
        let workers: Vec<_> = (0..n).map(|sub| {
            let sessions = sessions.clone();
            std::thread::spawn(move || {
                let pubs: Vec<usize> = (0..n).filter(|p| *p != sub).collect();
                let mut out = vec![];
                for chunk in pubs.chunks(parallel) {
                    let calls: Vec<_> = chunk.iter().map(|&p| {
                        let (s, ps) = (sessions[sub].clone(), sessions[p].clone());
                        std::thread::spawn(move || {
                            let t = Instant::now();
                            let r = try_allocate(&s, p as u64 + 1, Some(&ps));
                            (p, r, t.elapsed().as_secs_f64() * 1e3)
                        })
                    }).collect();
                    for h in calls { out.push(h.join().unwrap()); }
                }
                out
            })
        }).collect();
        while !workers.iter().all(|w| w.is_finished()) {
            poll_all(&mut c, &mut got);
            std::thread::sleep(Duration::from_millis(1));
        }
        let mut round_failed = 0;
        let mut remote: Vec<Vec<u64>> = vec![vec![]; n];
        for (sub, w) in workers.into_iter().enumerate() {
            for (p, r, ms) in w.join().unwrap() {
                calls += 1;
                ms_all.push(ms);
                match r {
                    Ok(channels) => {
                        for ch in &channels {
                            let id = ch["channelId"].as_u64().unwrap();
                            if !seen_ids[sub].insert(id) { reused += 1; }
                            remote[sub].push(id);
                        }

                        if let Err(e) = c[sub].signal(&json!({"type":"sfu_channels","channels":channels})) {
                            conflicts += 1;
                            eprintln!("probe: round {round}: subscriber {sub} refused publisher {p}'s channels: {e}");
                        }
                    }
                    Err(e) => {
                        failed += 1;
                        round_failed += 1;
                        eprintln!("probe: round {round}: subscriber {sub} <- publisher {p} failed: {e}");
                    }
                }
            }
        }
        let calls_ms = started.elapsed().as_secs_f64() * 1e3;
        let until = Instant::now() + Duration::from_secs(10);
        let open = |c: &mut Vec<Sfu>| c.iter_mut().all(|s| s.waiting.is_empty() && s.channels.iter().all(|ch| s.rtc.channel(ch.id).is_some()));
        while !open(&mut c) && Instant::now() < until {
            poll_all(&mut c, &mut got);
            std::thread::sleep(Duration::from_millis(1));
        }
        if !open(&mut c) { unopened += 1; eprintln!("probe: round {round}: channels not all open after 10 s"); }
        let race = 1_000 + round;
        for g in got.iter_mut() { g.clear(); }
        for s in c.iter_mut() {
            s.signal(&json!({"type":"start","startAt":race,"players":players})).unwrap();
            s.released = true;
        }
        for (who, s) in c.iter_mut().enumerate() {
            for k in 0..3u8 { s.send(&[6, who as u8, round as u8, k]).unwrap(); }
            s.send(&[5, who as u8, 0, 0, 1]).unwrap();
        }
        let delivered = |got: &Vec<Vec<Vec<u8>>>| (0..n).all(|sub| (0..n).filter(|p| *p != sub)
            .all(|p| got[sub].iter().filter(|f| f[0] == 6 && f[1] == p as u8 && f[2] == round as u8).count() == 3));
        let until = Instant::now() + Duration::from_secs(5);
        while !delivered(&got) && Instant::now() < until {
            poll_all(&mut c, &mut got);
            std::thread::sleep(Duration::from_millis(1));
        }
        let ok = delivered(&got);
        if !ok {
            // Late, or lost? 10 s more.
            let late = Instant::now();
            while !delivered(&got) && late.elapsed() < Duration::from_secs(10) {
                poll_all(&mut c, &mut got);
                std::thread::sleep(Duration::from_millis(1));
            }
            eprintln!("probe: round {round}: events {} 5 s + {} ms", if delivered(&got) { "all came after" } else { "still missing after" }, late.elapsed().as_millis());
            undelivered += 1;
            for sub in 0..n {
                let from: Vec<u8> = (0..n as u8).filter(|p| got[sub].iter().any(|f| f[0] == 6 && f[1] == *p && f[2] == round as u8)).collect();
                eprintln!("probe: round {round}: subscriber {sub} got events from {from:?}");
            }
        }
        eprintln!("probe: round {round}: {} calls in {calls_ms:.0} ms, {round_failed} failed, events {}", n * (n - 1),
            if ok { "delivered both ways for every pair" } else { "MISSING" });
        // Close the subscriptions; the next round makes them again on the same sessions.
        for sub in 0..n {
            if remote[sub].is_empty() { continue; }
            let ids: Vec<Value> = remote[sub].iter().map(|id| json!({"id": id})).collect();
            let r = try_call("PUT", &format!("/v1/apps/{}/sessions/{}/datachannels/close", secret("REALTIME_APP_ID"), sessions[sub]),
                &secret("REALTIME_APP_TOKEN"), &json!({"dataChannels": ids}));
            if let Err(e) = r { eprintln!("probe: round {round}: close on subscriber {sub} failed: {e}"); }
            c[sub].signal(&json!({"type":"sfu_closed","channelIds":remote[sub]})).unwrap();
        }
        // Until str0m let go of every closed stream (DHD_PROBE_SETTLE_MS at most, default 300).
        let closed_at = Instant::now();
        let settle = closed_at + Duration::from_millis(env_or("DHD_PROBE_SETTLE_MS", 300));
        let held = |c: &mut Vec<Sfu>| (0..n).map(|s| remote[s].iter().filter(|id| c[s].rtc.direct_api().channel_id_by_sctp_stream_id(**id as u16).is_some()).count()).sum::<usize>();
        while Instant::now() < settle && held(&mut c) > 0 { poll_all(&mut c, &mut got); std::thread::sleep(Duration::from_millis(1)); }
        let mut kinds: Vec<String> = vec![];
        for s in 0..n {
            for id in &remote[s] {
                if c[s].rtc.direct_api().channel_id_by_sctp_stream_id(*id as u16).is_none() { continue; }
                kinds.push(format!("{s}:{id}"));
            }
        }
        eprintln!("probe: round {round}: closed streams still held after {} ms: {} {kinds:?}", closed_at.elapsed().as_millis(), held(&mut c));
        while Instant::now() < settle { poll_all(&mut c, &mut got); std::thread::sleep(Duration::from_millis(1)); }
    }
    ms_all.sort_by(|a, b| a.partial_cmp(b).unwrap());
    eprintln!("probe: {calls} subscription calls, {failed} failed, {unopened} round(s) with channels not open, {undelivered} round(s) with events missing; \
        channel ids reused on a session after their close: {reused} ({conflicts} refused by the client); call ms p50 {:.0} p95 {:.0} max {:.0}",
        percentile(&ms_all, 0.5), percentile(&ms_all, 0.95), ms_all.last().copied().unwrap_or(f64::NAN));
    assert_eq!((failed, unopened, undelivered), (0, 0, 0));
}

/// Finding 177: a publisher leaves the room. Sessions 0 and 1 subscribe to 2 and to each other;
/// then, as the room does for a member who leaves, 2's own channels close, then 0's and 1's
/// subscriptions to it (DHD_PROBE_SUBS_FIRST=1: the other way round), and 2's association is
/// dropped. A new session 3 publishes and 0 and 1 subscribe to it (Cloudflare reuses the ids):
/// prints, every 250 ms, which closed stream ids str0m still holds, and whether the new channels
/// open and carry an event.
#[test]
#[ignore = "needs the network and Realtime credentials"]
fn probe_publisher_leaves() {
    let subs_first = env_or("DHD_PROBE_SUBS_FIRST", 0) != 0;
    let mut c: Vec<Option<Sfu>> = vec![];
    let mut sessions = vec![];
    let connect = |c: &mut Vec<Option<Sfu>>, sessions: &mut Vec<String>| {
        let (mut sfu, offer) = Sfu::start(Arc::new(|| {}), false).unwrap();
        let r = api("POST", "sessions/new", &json!({"sessionDescription":{"type":"offer","sdp":offer["sdp"]}}));
        sessions.push(r["sessionId"].as_str().unwrap().to_string());
        sfu.signal(&json!({"type":"sfu_answer","sdp":r["sessionDescription"]["sdp"]})).unwrap();
        c.push(Some(sfu));
    };
    let poll = |c: &mut Vec<Option<Sfu>>, got: &mut Vec<Vec<Vec<u8>>>| {
        for (k, s) in c.iter_mut().enumerate() { if let Some(s) = s { let (_, f) = s.poll().expect("association"); got[k].extend(f); } }
    };
    let mut got: Vec<Vec<Vec<u8>>> = vec![vec![]; 4];
    for _ in 0..3 { connect(&mut c, &mut sessions); }
    let until = Instant::now() + Duration::from_secs(20);
    while !c.iter().all(|s| s.as_ref().unwrap().connected) { assert!(Instant::now() < until); poll(&mut c, &mut got); std::thread::sleep(Duration::from_millis(1)); }
    for k in 0..3 { let ch = allocate(&sessions[k], k as u64 + 1, None); c[k].as_mut().unwrap().signal(&json!({"type":"sfu_channels","channels":ch})).unwrap(); }
    let mut held_ids: Vec<Vec<u64>> = vec![vec![]; 2];
    for sub in 0..2 {
        for publ in 0..3 {
            if publ == sub { continue; }
            let ch = allocate(&sessions[sub], publ as u64 + 1, Some(&sessions[publ]));
            if publ == 2 { held_ids[sub] = ch.iter().map(|x| x["channelId"].as_u64().unwrap()).collect(); }
            c[sub].as_mut().unwrap().signal(&json!({"type":"sfu_channels","channels":ch})).unwrap();
        }
    }
    let settle = Instant::now() + Duration::from_secs(2);
    while Instant::now() < settle { poll(&mut c, &mut got); std::thread::sleep(Duration::from_millis(1)); }
    let (app, token) = (secret("REALTIME_APP_ID"), secret("REALTIME_APP_TOKEN"));
    let close = |session: &str, ids: &[u64]| {
        let list: Vec<Value> = ids.iter().map(|id| json!({"id": id})).collect();
        let r = try_call("PUT", &format!("/v1/apps/{app}/sessions/{session}/datachannels/close"), &token, &json!({"dataChannels": list}));
        eprintln!("probe: close {} on a session: {}", ids.len(), match &r { Ok(v) => v["dataChannels"].to_string(), Err(e) => e.clone() });
    };
    let own2: Vec<u64> = c[2].as_ref().unwrap().channels.iter().filter(|x| x.local).map(|x| x.stream as u64).collect();
    if !subs_first { close(&sessions[2], &own2); }
    for sub in 0..2 {
        close(&sessions[sub], &held_ids[sub]);
        c[sub].as_mut().unwrap().signal(&json!({"type":"sfu_closed","channelIds":held_ids[sub]})).unwrap();
    }
    if subs_first { close(&sessions[2], &own2); }
    c[2] = None;  // the leaving game drops its association
    let started = Instant::now();
    connect(&mut c, &mut sessions);
    let mut last = String::new();
    let mut subscribed = false;
    let mut new_ids: Vec<Vec<u64>> = vec![vec![]; 2];
    while started.elapsed() < Duration::from_secs(20) {
        poll(&mut c, &mut got);
        if !subscribed && c[3].as_ref().unwrap().connected {
            let ch = allocate(&sessions[3], 4, None);
            c[3].as_mut().unwrap().signal(&json!({"type":"sfu_channels","channels":ch})).unwrap();
            for sub in 0..2 {
                let ch = allocate(&sessions[sub], 4, Some(&sessions[3]));
                new_ids[sub] = ch.iter().map(|x| x["channelId"].as_u64().unwrap()).collect();
                c[sub].as_mut().unwrap().signal(&json!({"type":"sfu_channels","channels":ch})).unwrap();
            }
            eprintln!("probe: {} ms: subscribed to the new session: ids {new_ids:?} (closed ones {held_ids:?})", started.elapsed().as_millis());
            subscribed = true;
            for s in c.iter_mut().flatten() { s.signal(&json!({"type":"start","startAt":4242,"players":[{"memberId":1,"slot":0},{"memberId":2,"slot":1},{"memberId":4,"slot":3}]})).unwrap(); s.released = true; }
        }
        if subscribed { let _ = c[3].as_mut().unwrap().send(&[6, 3, 7]); }
        let state: Vec<String> = (0..2).map(|s| {
            let x = c[s].as_mut().unwrap();
            let held: Vec<u64> = held_ids[s].iter().copied().filter(|id| x.rtc.direct_api().channel_id_by_sctp_stream_id(*id as u16).is_some()).collect();
            let open = x.channels.iter().filter(|ch| new_ids[s].contains(&(ch.stream as u64))).filter(|ch| x.rtc.channel(ch.id).is_some()).count();
            format!("sub {s}: str0m holds {held:?}, waiting {}, closing {}, new open {open}, events {}", x.waiting.len(), x.closing.len(),
                got[s].iter().filter(|f| f[0] == 6 && f[1] == 3).count())
        }).collect();
        let line = state.join("; ");
        if line != last { eprintln!("probe: {} ms: {line}", started.elapsed().as_millis()); last = line; }
        if got[0].iter().any(|f| f[0] == 6 && f[1] == 3) && got[1].iter().any(|f| f[0] == 6 && f[1] == 3) { eprintln!("probe: both got the new publisher's event"); return; }
        std::thread::sleep(Duration::from_millis(5));
    }
    panic!("the new publisher's channels never worked");
}
