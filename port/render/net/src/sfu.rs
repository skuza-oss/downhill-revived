//! One SFU association per room, owned exclusively by the WebSocket send thread. A reader thread
//! blocks on the UDP socket and wakes that thread for every datagram, so input waits for neither
//! the send thread's tick nor the OS timer (finding 158, round 3). When UDP to the SFU does not
//! connect within DIRECT_TIMEOUT, the room is asked for TURN credentials and a TURN allocation over
//! TLS on 443 (`turn.rs`, `relay.rs`) adds a relayed candidate: the SFU is then reached through
//! Cloudflare's TURN service (round 4).
use crate::{relay, turn};
use serde_json::{json, Value};
use std::collections::{HashMap, VecDeque};
use std::io::ErrorKind;
use std::net::{SocketAddr, ToSocketAddrs, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use str0m::change::{SdpAnswer, SdpPendingOffer};
use str0m::channel::{ChannelConfig, ChannelId, Reliability};
use str0m::net::{Protocol, Receive};
use str0m::{Candidate, Event, Input, Output, Rtc};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
/// Called by the socket reader when a datagram waits: the owner's thread polls next.
pub type Wake = Arc<dyn Fn() + Send + Sync>;
/// Datagrams the reader took off the socket and their arrival times; at most INBOX.
type Inbox = Arc<Mutex<VecDeque<(Instant, SocketAddr, Vec<u8>)>>>;
const INBOX: usize = 1024;
/// Events for a race this client hasn't seen start yet (its start message is still on the way):
/// a sender's few setup events at a race's start, kept per sender and in all.
const EARLY_EVENTS_PER_SENDER: usize = 16;
const EARLY_EVENTS_MAX: usize = 128;
/// How often the reader looks whether its association was dropped.
const READER_CHECK: Duration = Duration::from_millis(100);
/// UDP to the SFU that has not connected this long after the answer falls back to a TURN relay
/// over TLS. Direct setup is an ICE check and the DTLS handshake, about four round trips to the
/// nearest Cloudflare location (8-40 ms each for most players, 150 ms far from one): well under a
/// second, with room for a lost packet's retransmission. Waiting longer only delays the race start
/// for players behind a UDP block; going sooner would put players with a slow first packet on the
/// relay for nothing (finding 158, round 4).
pub const DIRECT_TIMEOUT: Duration = Duration::from_millis(2000);
/// The relay in use: the TURN client, its TLS stream and the relayed address once allocated.
struct Relay {
    client: turn::Client,
    link: relay::Link,
    relayed: Option<SocketAddr>,
    since: Instant,
}
struct Channel {
    id: ChannelId,
    member: u64,
    state: bool,
    local: bool,
}
pub struct Sfu {
    rtc: Rtc,
    pending: Option<SdpPendingOffer>,
    socket: UdpSocket,
    local: SocketAddr,
    inbox: Inbox,
    reader_alive: Arc<AtomicBool>,
    reader_failed: Arc<AtomicBool>,
    stats: Stats,
    channels: Vec<Channel>,
    slots: HashMap<u64, u8>,
    race: u64,
    future_events: VecDeque<(ChannelId, Vec<u8>)>,
    connected: bool,
    members: usize,
    announced: bool,
    released: bool,
    drop_percent: u64,
    sent: u64,
    pending_events: VecDeque<(Instant, Vec<u8>)>,
    pending_state: HashMap<u8, (Instant, Vec<u8>)>,
    rates: HashMap<u64, SenderRate>,
    wake: Wake,
    /// False with DHD_NET_SFU_UDP=0: a test switch that drops every UDP datagram to and from the
    /// SFU, as a network that blocks UDP does, so the TURN/TLS fallback must carry the race.
    direct: bool,
    answered_at: Option<Instant>,
    turn_asked: bool,
    relay: Option<Relay>,
    /// Whether the last datagram str0m got came through the relay; at Connected, the path used.
    last_in_relayed: bool,
    via_relay: bool,
    #[cfg(test)]
    probe: ProbeTimes,
}
/// The bare transport probe's timings (tests only).
#[cfg(test)]
#[derive(Default)]
struct ProbeTimes { wrote_at: Option<Instant>, tx_lag: Vec<f64> }
/// What the transport loop did in the last 30 s, for dhd.log: how long datagrams waited for the
/// loop, how long frames waited to be written, the longest time between polls.
#[derive(Default)]
struct Stats {
    since: Option<Instant>,
    last_poll: Option<Instant>,
    datagrams_in: u32,
    datagrams_out: u32,
    input_wait_max: f64,
    input_waits_over_5ms: u32,
    send_wait_max: f64,
    send_waits_over_5ms: u32,
    poll_gap_max: f64,
    inbox_max: usize,
    relayed_in: u32,
    relayed_out: u32,
    /// The relay's stream: the longest wait between reads with data, and the longest a read waited
    /// for this thread.
    relay_read_gap_max: f64,
    relay_wait_max: f64,
}
impl Stats {
    fn input(&mut self, waited: Duration) {
        let ms = waited.as_secs_f64() * 1e3;
        self.datagrams_in += 1;
        self.input_wait_max = self.input_wait_max.max(ms);
        self.input_waits_over_5ms += (ms > 5.0) as u32;
    }
    fn sent(&mut self, waited: Duration) {
        let ms = waited.as_secs_f64() * 1e3;
        self.send_wait_max = self.send_wait_max.max(ms);
        self.send_waits_over_5ms += (ms > 5.0) as u32;
    }
    /// Notes a poll; every 30 s of a race, the line for dhd.log.
    fn poll(&mut self, now: Instant, inbox: usize, racing: bool, via: &str) -> Option<String> {
        if let Some(last) = self.last_poll { self.poll_gap_max = self.poll_gap_max.max((now - last).as_secs_f64() * 1e3); }
        self.last_poll = Some(now);
        self.inbox_max = self.inbox_max.max(inbox);
        let since = *self.since.get_or_insert(now);
        if now - since < Duration::from_secs(30) { return None; }
        let relay = if self.relayed_in + self.relayed_out > 0 {
            format!("; relay read gap max {:.1} ms, relay input wait max {:.1} ms", self.relay_read_gap_max, self.relay_wait_max)
        } else { String::new() };
        let line = format!(
            "net: SFU 30 s via {via}: datagrams {} in, {} out ({} in, {} out relayed); input wait max {:.1} ms ({} over 5 ms); send wait max {:.1} ms ({} over 5 ms); poll gap max {:.1} ms; inbox max {}{relay}",
            self.datagrams_in, self.datagrams_out, self.relayed_in, self.relayed_out, self.input_wait_max, self.input_waits_over_5ms,
            self.send_wait_max, self.send_waits_over_5ms, self.poll_gap_max, self.inbox_max);
        *self = Stats { since: Some(now), last_poll: Some(now), ..Default::default() };
        racing.then_some(line)
    }
}

/// The reader thread: blocks on the socket, queues each datagram with its arrival time and wakes
/// the owner. It ends when the association is dropped (within READER_CHECK) or the socket fails.
fn read_socket(socket: UdpSocket, inbox: Inbox, alive: Arc<AtomicBool>, failed: Arc<AtomicBool>, wake: Wake) {
    let mut buffer = vec![0u8; 65536];
    while alive.load(Ordering::Acquire) {
        match socket.recv_from(&mut buffer) {
            Ok((n, source)) => {
                let at = Instant::now();
                {
                    let mut q = inbox.lock().unwrap_or_else(|e| e.into_inner());
                    if q.len() < INBOX { q.push_back((at, source, buffer[..n].to_vec())); }
                }
                wake();
            }
            // Windows reports an ICMP unreachable as a reset on the next read; the ICE consent
            // checks decide whether the association is gone.
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted | ErrorKind::ConnectionReset) => {}
            Err(_) => {
                failed.store(true, Ordering::Release);
                wake();
                break;
            }
        }
    }
}

// Four times the normal per-rider rate, with a 120-packet burst allowance.
// Exceeding it silences this member (including events) until the next race.
struct SenderRate { since: Instant, count: [u16; 10], blocked: bool }
impl SenderRate {
    fn new() -> Self { Self { since: Instant::now(), count: [0; 10], blocked: false } }
    fn accept(&mut self, rider: u8, now: Instant) -> bool {
        if self.blocked { return false; }
        if now.duration_since(self.since).as_secs() >= 1 { self.count = [0; 10]; self.since = now; }
        self.count[rider as usize] += 1;
        self.blocked = self.count[rider as usize] > 120;
        !self.blocked
    }
}
impl Sfu {
    pub fn start(wake: Wake) -> Result<(Self, Value)> {
        let route = ("rtc.live.cloudflare.com", 443).to_socket_addrs()?.find(|a| a.is_ipv4()).ok_or("No SFU IPv4 route")?;
        Self::start_at(route, wake)
    }
    fn start_at(remote: SocketAddr, wake: Wake) -> Result<(Self, Value)> {
        // The only production route is Cloudflare. No peer signaling, discovery or TURN mesh.
        let route = UdpSocket::bind("0.0.0.0:0")?;
        route.connect(remote)?;
        let socket = UdpSocket::bind(SocketAddr::new(route.local_addr()?.ip(), 0))?;
        // Blocking with a timeout: the reader thread waits in recv_from; sends never wait.
        socket.set_read_timeout(Some(READER_CHECK))?;
        let local = socket.local_addr()?;
        let inbox: Inbox = Arc::default();
        let (reader_alive, reader_failed) = (Arc::new(AtomicBool::new(true)), Arc::new(AtomicBool::new(false)));
        {
            let (socket, inbox, alive, failed) = (socket.try_clone()?, inbox.clone(), reader_alive.clone(), reader_failed.clone());
            let wake = wake.clone();
            std::thread::Builder::new().name("dhd-sfu-read".into()).spawn(move || read_socket(socket, inbox, alive, failed, wake))?;
        }
        let mut rtc = Rtc::builder().set_crypto_provider(str0m::crypto::from_feature_flags().into()).build(Instant::now());
        rtc.add_local_candidate(Candidate::host(socket.local_addr()?, "udp")?);
        let mut change = rtc.sdp_api();
        change.add_channel_with_config(ChannelConfig { label: "server-events".into(), negotiated: Some(65534), ..Default::default() });
        let (offer, pending) = change.apply().ok_or("No transport offer")?;
        let message = json!({ "type": "sfu_offer", "sdp": offer.to_sdp_string() });
        let drop_percent = std::env::var("DHD_NET_DROP_STATE").ok().and_then(|v| v.parse().ok()).unwrap_or(0).min(100);
        let direct = std::env::var("DHD_NET_SFU_UDP").map_or(true, |v| v.trim() != "0");
        if !direct { eprintln!("net: SFU: DHD_NET_SFU_UDP=0, direct UDP to the SFU is blocked (test switch)"); }
        Ok((Self { rtc, pending: Some(pending), socket, local, inbox, reader_alive, reader_failed, stats: Stats::default(), channels: vec![], slots: HashMap::new(), race: 0, future_events: VecDeque::new(),
            connected: false, members: 1, announced: false, released: false, drop_percent, sent: 0, pending_events: VecDeque::new(), pending_state: HashMap::new(), rates: HashMap::new(),
            wake, direct, answered_at: None, turn_asked: false, relay: None, last_in_relayed: false, via_relay: false,
            #[cfg(test)] probe: ProbeTimes::default() }, message))
    }
    pub fn signal(&mut self, message: &Value) -> Result<()> {
        if message["type"] == "sfu_answer" {
            let sdp = message["sdp"].as_str().ok_or("Missing SFU answer")?;
            let filtered = sdp.lines().filter(|line| {
                if !line.starts_with("a=candidate:") { return true; }
                let p: Vec<_> = line.split_whitespace().collect();
                p.len() >= 6 && p[2].eq_ignore_ascii_case("udp") && p[4].parse::<std::net::Ipv4Addr>().is_ok()
            }).collect::<Vec<_>>().join("\r\n") + "\r\n";
            let pending = self.pending.take().ok_or("Unexpected answer")?;
            self.rtc.sdp_api().accept_answer(pending, SdpAnswer::from_sdp_string(&filtered)?)?;
            self.answered_at = Some(Instant::now());
        } else if message["type"] == "sfu_turn" {
            // Credentials for this player only (the room mints them for a racer that asks).
            if self.relay.is_some() || !self.turn_asked { return Err("Unexpected TURN credentials".into()); }
            let field = |k: &str| message[k].as_str().filter(|v| !v.is_empty() && v.len() <= 256);
            let (user, pass) = (field("username").ok_or("Missing TURN username")?, field("credential").ok_or("Missing TURN credential")?);
            let (host, port) = turns_server(&message["urls"]).ok_or("No TURN over TLS server")?;
            eprintln!("net: SFU: TURN relay over TLS to {host}:{port}");
            self.relay = Some(Relay { client: turn::Client::new(user, pass, Instant::now()), link: relay::Link::open(&host, port, self.wake.clone()), relayed: None, since: Instant::now() });

        } else if message["type"] == "start" {
            self.race = message["startAt"].as_u64().filter(|n| *n > 0).ok_or("Missing race marker")?;
            self.slots.clear();
            self.rates.clear();
            self.members = message["players"].as_array().map_or(1, |p| p.len());
            self.announced = false; self.released = false;
            self.pending_state.clear();
            self.pending_events.clear();
            if let Some(players) = message["players"].as_array() {
                for p in players {
                    if let (Some(member), Some(slot)) = (p["memberId"].as_u64(), p["slot"].as_u64())
                    {
                        if slot < 10 {
                            self.slots.insert(member, slot as u8);
                        }
                    }
                }
            }
        } else if message["type"] == "left" {
            if let Some(member) = message["memberId"].as_u64() {
                self.slots.remove(&member); self.members = self.slots.len().max(1);
            }
        } else if message["type"] == "sfu_ready" {
            self.released = true;
        } else if message["type"] == "sfu_closed" {
            if let Some(ids) = message["channelIds"].as_array() {
                for id in ids.iter().filter_map(Value::as_u64) {
                    if id > 65534 {
                        continue;
                    }
                    if let Some(cid) = self
                        .rtc
                        .direct_api()
                        .channel_id_by_sctp_stream_id(id as u16)
                    {
                        self.rtc.direct_api().close_data_channel(cid);
                        self.channels.retain(|c| c.id != cid);
                    }
                }
            }
        } else if message["type"] == "sfu_channels" {
            let channels = message["channels"]
                .as_array()
                .ok_or("Missing allocations")?;
            if self.channels.len() + channels.len() > 20 {
                return Err("Too many channels".into());
            }
            for c in channels {
                let stream = c["channelId"]
                    .as_u64()
                    .filter(|id| *id < 65535)
                    .ok_or("Invalid channel ID")? as u16;
                let name = c["name"]
                    .as_str()
                    .filter(|s| *s == "state" || *s == "events")
                    .ok_or("Invalid channel name")?;
                let state = name == "state";
                if self
                    .rtc
                    .direct_api()
                    .channel_id_by_sctp_stream_id(stream)
                    .is_some()
                {
                    return Err("Duplicate channel ID".into());
                }
                let id = self.rtc.direct_api().create_data_channel(ChannelConfig {
                    label: name.into(),
                    ordered: !state,
                    negotiated: Some(stream),
                    reliability: if state {
                        Reliability::MaxRetransmits { retransmits: 0 }
                    } else {
                        Reliability::Reliable
                    },
                    ..Default::default()
                });
                self.channels.push(Channel {
                    id,
                    member: c["memberId"].as_u64().ok_or("Missing member")?,
                    state,
                    local: c["local"].as_bool().ok_or("Missing direction")?,
                });
            }
        }
        if message["type"] == "sfu_channels" { eprintln!("net: SFU channels allocated: {}", self.channels.len()); }
        Ok(())
    }
    fn usable(&mut self) -> bool {
        self.channels.len() >= self.members * 2 && self.channels.iter().all(|c| self.rtc.channel(c.id).is_some())
    }
    /// Frames queued by send() that the next poll would write now.
    pub fn unwritten(&self) -> bool {
        self.released && !(self.pending_state.is_empty() && self.pending_events.is_empty())
    }
    pub fn send(&mut self, bytes: &[u8]) -> Result<()> {
        if bytes.len() < 2 || bytes.len() > 3072 {
            return Ok(());
        }
        if self.race == 0 { return Err("Race not started".into()); }
        let state = bytes[0] == 5;
        if state {
            self.sent += 1;
            // Deterministic loss injection; never drop reliable gameplay events.
            if (self.sent * 37) % 100 < self.drop_percent {
                return Ok(());
            }
        }
        let mut wire = self.race.to_le_bytes().to_vec();
        wire.extend_from_slice(bytes);
        if state {
            // One newest snapshot per rider; never accumulate stale snapshots under congestion.
            if bytes.len() >= 4 {
                self.pending_state.insert(bytes[3], (Instant::now(), wire));
            }
        } else {
            if self.pending_events.len() >= 128 {
                return Err("Event send queue full".into());
            }
            self.pending_events.push_back((Instant::now(), wire));
        }
        Ok(())
    }
    pub fn poll(&mut self) -> Result<(Vec<Value>, Vec<Vec<u8>>)> {
        let mut text = vec![];
        let mut frames = vec![];
        let pending = std::mem::take(&mut self.future_events);
        for (id, bytes) in pending { self.receive_frame(id, bytes, &mut frames)?; }
        if self.usable() && !self.announced {
            self.announced = true; text.push(json!({"type":"sfu_ready"}));
        }
        for state in [false, true] {
            if !self.released { break; }
            if let Some(c) = self.channels.iter().find(|c| c.local && c.state == state) {
                if let Some(mut channel) = self.rtc.channel(c.id) {
                    if state {
                        let mut sent = vec![];
                        for (rider, (queued, bytes)) in &self.pending_state {
                            if channel.write(true, bytes)? {
                                #[cfg(test)]
                                self.probe.wrote_at.get_or_insert_with(Instant::now);
                                self.stats.sent(queued.elapsed());
                                sent.push(*rider);
                            } else {
                                break;
                            }
                        }
                        for rider in sent {
                            self.pending_state.remove(&rider);
                        }
                    } else {
                        while let Some((queued, bytes)) = self.pending_events.front() {
                            if !channel.write(true, bytes)? {
                                break;
                            }
                            self.stats.sent(queued.elapsed());
                            self.pending_events.pop_front();
                        }
                    }
                }
            }
        }
        let datagrams = std::mem::take(&mut *self.inbox.lock().unwrap_or_else(|e| e.into_inner()));
        let now = Instant::now();
        let via = if self.via_relay { "TURN/TLS" } else { "UDP" };
        if let Some(line) = self.stats.poll(now, datagrams.len(), self.race != 0 && self.released, via) { eprintln!("{line}"); }
        for (at, source, data) in datagrams {
            if !self.direct { continue; }
            self.stats.input(now.saturating_duration_since(at));
            if let Ok(contents) = data.as_slice().try_into() {
                // str0m's clock only moves forward: the input is stamped now, not at arrival.
                let input = Input::Receive(Instant::now(), Receive { proto: Protocol::Udp, source, destination: self.local, contents });
                if self.rtc.accepts(&input) {
                    self.last_in_relayed = false;
                    self.rtc.handle_input(input)?;
                    self.drain(&mut text, &mut frames)?;
                }
            }
        }
        if self.reader_failed.load(Ordering::Acquire) {
            return Err("SFU socket failed".into());
        }
        self.pump_relay(&mut text, &mut frames)?;
        if !self.connected && !self.turn_asked && self.answered_at.is_some_and(|t| t.elapsed() >= DIRECT_TIMEOUT) {
            self.turn_asked = true;
            eprintln!("net: SFU: no UDP connection {} ms after the answer; asking for a TURN relay", DIRECT_TIMEOUT.as_millis());
            text.push(json!({"type":"sfu_turn"}));
        }
        self.rtc.handle_input(Input::Timeout(Instant::now()))?;
        self.drain(&mut text, &mut frames)?;
        // Before its TLS link is up the TURN client keeps what it would send.
        if let Some(r) = self.relay.as_mut().filter(|r| r.link.ready()) { r.link.write(&r.client.output()); }
        if !self.rtc.is_alive() {
            return Err("SFU disconnected".into());
        }
        Ok((text, frames))
    }

    /// The relay's side of a poll: what the TLS stream read goes to the TURN client, its relayed
    /// datagrams to str0m, its allocation becomes a local candidate. A relay that fails while it
    /// carries the association (or before anything connected) fails the association.
    fn pump_relay(&mut self, text: &mut Vec<Value>, frames: &mut Vec<Vec<u8>>) -> Result<()> {
        let Some(r) = self.relay.as_mut() else { return Ok(()) };
        if r.link.failed() || r.client.failed().is_some() {
            eprintln!("net: SFU: TURN relay failed ({})", r.client.failed().unwrap_or("its TLS link"));
            self.relay = None;
            return if self.connected && !self.via_relay { Ok(()) } else { Err("TURN relay failed".into()) };
        }
        if !r.link.ready() { return Ok(()); }
        let now = Instant::now();
        let (bytes, arrived) = r.link.take();
        if let Some(at) = arrived { self.stats.relay_wait_max = self.stats.relay_wait_max.max((now - at).as_secs_f64() * 1e3); }
        // Read gaps count once race frames may flow (idle moments, the countdown, the finish wait,
        // count too: a long gap is a TCP stall only while snapshots keep coming; finding 158).
        let gap = r.link.take_read_gap().as_secs_f64() * 1e3;
        if self.released { self.stats.relay_read_gap_max = self.stats.relay_read_gap_max.max(gap); }
        if !bytes.is_empty() { r.client.receive(&bytes, now); }
        r.client.tick(now);
        let mut events = vec![];
        while let Some(e) = r.client.poll_event() { events.push(e); }
        r.link.write(&r.client.output());
        let base = r.link.local();
        let since = r.since;
        for e in events {
            match e {
                turn::Event::Allocated(addr) => {
                    if let Some(r) = self.relay.as_mut() { r.relayed = Some(addr); }
                    // The relayed candidate: str0m transmits from it, drain() routes those to the relay.
                    let candidate = Candidate::relayed(addr, base.unwrap_or(addr), "udp")?;
                    self.rtc.add_local_candidate(candidate);
                    eprintln!("net: SFU: TURN allocation ready {} ms after the credentials", since.elapsed().as_millis());
                }
                turn::Event::Data(source, data) => {
                    let Some(destination) = self.relay.as_ref().and_then(|r| r.relayed) else { continue };
                    self.stats.relayed_in += 1;
                    if let Ok(contents) = data.as_slice().try_into() {
                        let input = Input::Receive(Instant::now(), Receive { proto: Protocol::Udp, source, destination, contents });
                        if self.rtc.accepts(&input) {
                            self.last_in_relayed = true;
                            self.rtc.handle_input(input)?;
                            self.drain(text, frames)?;
                        }
                    }
                }
            }
        }
        Ok(())
    }

    fn receive_frame(&mut self, id: ChannelId, bytes: Vec<u8>, frames: &mut Vec<Vec<u8>>) -> Result<()> {
        if bytes.len() < 10 || bytes.len() > 3080 { return Ok(()); }
        let Some(c) = self.channels.iter().find(|c| c.id == id && !c.local) else { return Ok(()); };
        if bytes[8] != if c.state { 5 } else { 6 } { return Ok(()); }
        let epoch = u64::from_le_bytes(bytes[..8].try_into().unwrap());
        if epoch > self.race && !c.state {
            // A next race's events can arrive before its start: keep a few per sender. Another
            // player decides what arrives here, so a full queue drops theirs and never fails ours.
            let theirs = self.future_events.iter().filter(|(other, _)| *other == id).count();
            if theirs < EARLY_EVENTS_PER_SENDER && self.future_events.len() < EARLY_EVENTS_MAX {
                self.future_events.push_back((id, bytes));
            }
        } else if epoch == self.race && self.race != 0 {
            if c.state {
                if bytes.len() < 12 || bytes[11] >= 10 { return Ok(()); }
                if !self.rates.entry(c.member).or_insert_with(SenderRate::new).accept(bytes[11], Instant::now()) { return Ok(()); }
            }
            if self.rates.get(&c.member).is_some_and(|r| r.blocked) { return Ok(()); }
            if let Some(slot) = self.slots.get(&c.member) {
                let mut frame = bytes[8..].to_vec();
                frame[1] = *slot;
                frames.push(frame);
            }
        }
        Ok(())
    }

    fn drain(&mut self, text: &mut Vec<Value>, frames: &mut Vec<Vec<u8>>) -> Result<()> {
        for _ in 0..512 {
            match self.rtc.poll_output()? {
                Output::Timeout(_) => break,
                Output::Transmit(t) => {
                    self.stats.datagrams_out += 1;
                    #[cfg(test)]
                    if let Some(w) = self.probe.wrote_at.take() { self.probe.tx_lag.push(w.elapsed().as_secs_f64() * 1e3); }
                    if let Some(r) = self.relay.as_mut().filter(|r| r.relayed == Some(t.source)) {
                        self.stats.relayed_out += 1;
                        r.client.send_to(t.destination, &t.contents, Instant::now());
                    } else if self.direct {
                        if let Err(e) = self.socket.send_to(&t.contents, t.destination) {
                            if e.kind() != std::io::ErrorKind::WouldBlock {
                                return Err(e.into());
                            }
                        }
                    }
                }
                Output::Event(Event::Connected) if !self.connected => {
                    self.connected = true;
                    self.via_relay = self.last_in_relayed;
                    let after = self.answered_at.map_or(0, |t| t.elapsed().as_millis());
                    eprintln!("net: SFU connected via {} {after} ms after the answer", if self.via_relay { "TURN/TLS" } else { "UDP" });
                    text.push(json!({"type":"sfu_connected"}));
                }
                Output::Event(Event::ChannelData(data)) => {
                    if data.binary { self.receive_frame(data.id, data.data, frames)?; }
                }
                _ => {}
            }
        }
        Ok(())
    }
}

impl Drop for Sfu {
    fn drop(&mut self) {
        self.reader_alive.store(false, Ordering::Release);
        // Ends the TURN allocation at once instead of at its lifetime (the room also revokes the
        // credentials when the race ends).
        if let Some(r) = self.relay.as_mut().filter(|r| r.link.ready()) {
            r.client.close(Instant::now());
            r.link.write(&r.client.output());
        }
    }
}

/// The TURN-over-TLS server from the room's `urls`: `turns:` on port 443 first (what networks
/// that block UDP let through), else another `turns:` port. Only Cloudflare's TURN service: a
/// client talks to Cloudflare only (finding 158).
fn turns_server(urls: &Value) -> Option<(String, u16)> {
    let mut found: Vec<(String, u16)> = urls.as_array()?.iter().filter_map(|u| {
        let rest = u.as_str()?.strip_prefix("turns:")?;
        let rest = rest.split('?').next()?;
        let (host, port) = rest.rsplit_once(':')?;
        let port: u16 = port.parse().ok()?;
        let host = host.to_ascii_lowercase();
        (host == "turn.cloudflare.com" || host.ends_with(".turn.cloudflare.com")).then_some((host, port))
    }).collect();
    found.sort_by_key(|(_, p)| *p != 443);
    found.into_iter().next()
}

#[cfg(test)]
#[path = "probe.rs"]
mod probe;

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn snapshot_flood_silences_member_for_race() {
        let mut rate = SenderRate::new();
        let now = rate.since;
        for _ in 0..120 { assert!(rate.accept(0, now)); }
        assert!(rate.accept(1, now));
        assert!(!rate.accept(0, now));
        assert!(!rate.accept(1, now + Duration::from_secs(2)));
    }

    #[test]
    fn turn_only_over_tls_to_cloudflare_443_first() {
        let urls = json!(["turn:turn.cloudflare.com:3478?transport=udp", "turns:turn.cloudflare.com:5349?transport=tcp",
            "turns:turn.cloudflare.com:443?transport=tcp"]);
        assert_eq!(turns_server(&urls), Some(("turn.cloudflare.com".into(), 443)));
        assert_eq!(turns_server(&json!(["turns:turn.cloudflare.com:5349?transport=tcp"])), Some(("turn.cloudflare.com".into(), 5349)));
        assert_eq!(turns_server(&json!(["turns:relay.example:443?transport=tcp", "turns:turn.cloudflare.com.example:443"])), None);
        assert_eq!(turns_server(&json!(["turn:turn.cloudflare.com:443?transport=udp"])), None);
    }

    #[test]
    fn turn_credentials_only_after_asking_and_only_once() {
        let (mut client, _) = Sfu::start_at("127.0.0.1:9".parse().unwrap(), Arc::new(|| {})).unwrap();
        let turn = json!({"type":"sfu_turn","urls":["turns:turn.cloudflare.com:443?transport=tcp"],"username":"u","credential":"c"});
        assert!(client.signal(&turn).is_err()); // nobody asked
        client.answered_at = Some(Instant::now() - DIRECT_TIMEOUT);
        let (text, _) = client.poll().unwrap();
        assert_eq!(text.iter().filter(|m| m["type"] == "sfu_turn").count(), 1);
        let (text, _) = client.poll().unwrap();
        assert!(text.iter().all(|m| m["type"] != "sfu_turn")); // asked once
        assert!(client.signal(&json!({"type":"sfu_turn","urls":["turns:turn.cloudflare.com:443"],"username":"","credential":"c"})).is_err());
        let mut elsewhere = turn.clone();
        elsewhere["urls"] = json!(["turns:relay.example:443?transport=tcp"]);
        assert!(client.signal(&elsewhere).is_err());
        assert!(client.relay.is_none());
    }

    #[test]
    fn reliable_events_wait_for_start_and_old_race_frames_are_dropped() {
        let (mut client, _) = Sfu::start_at("127.0.0.1:9".parse().unwrap(), Arc::new(|| {})).unwrap();
        client.signal(&json!({"type":"sfu_channels","channels":[
            {"channelId":14,"name":"events","memberId":17,"local":false}
        ]})).unwrap();
        let id = client.channels[0].id;
        let wire = |epoch: u64, payload: u8| {
            let mut bytes = epoch.to_le_bytes().to_vec();
            bytes.extend_from_slice(&[6, 99, payload]); bytes
        };
        let mut frames = vec![];
        client.receive_frame(id, wire(100, 7), &mut frames).unwrap();
        assert!(frames.is_empty());
        assert_eq!(client.future_events.len(), 1);
        client.signal(&json!({"type":"start","startAt":100,"players":[{"memberId":17,"slot":3}]})).unwrap();
        let (_, frames) = client.poll().unwrap();
        assert_eq!(frames, vec![vec![6,3,7]]);
        client.send(&[6,0,1]).unwrap();
        assert_eq!(&client.pending_events[0].1[..8], &100u64.to_le_bytes());
        client.signal(&json!({"type":"start","startAt":200,"players":[{"memberId":17,"slot":5}]})).unwrap();
        assert!(client.pending_events.is_empty());
        let mut frames = vec![];
        client.receive_frame(id, wire(100, 8), &mut frames).unwrap();
        assert!(frames.is_empty());
        client.receive_frame(id, wire(200, 9), &mut frames).unwrap();
        assert_eq!(frames, vec![vec![6,5,9]]);
        // Another player's flood of next-race events is capped and dropped, never an error here.
        for _ in 0..200 { client.receive_frame(id, wire(u64::MAX, 1), &mut frames).unwrap(); }
        assert_eq!(client.future_events.len(), EARLY_EVENTS_PER_SENDER);
        assert!(client.poll().is_ok());
    }

    #[test]
    fn native_channels_deliver_and_bind_sender_with_state_loss() {
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        socket.set_nonblocking(true).unwrap();
        let mut server = Rtc::builder()
            .set_crypto_provider(str0m::crypto::from_feature_flags().into())
            .build(Instant::now());
        server.add_local_candidate(Candidate::host(socket.local_addr().unwrap(), "udp").unwrap());
        let (mut client, offer) = Sfu::start_at(socket.local_addr().unwrap(), Arc::new(|| {})).unwrap();
        let answer = server.sdp_api().accept_offer(str0m::change::SdpOffer::from_sdp_string(offer["sdp"].as_str().unwrap()).unwrap()).unwrap();
        client.signal(&json!({"type":"sfu_answer","sdp":answer.to_sdp_string()})).unwrap();
        let server_state = server.direct_api().create_data_channel(ChannelConfig {
            label: "state".into(),
            ordered: false,
            negotiated: Some(10),
            reliability: Reliability::MaxRetransmits { retransmits: 0 },
            ..Default::default()
        });
        let server_event = server.direct_api().create_data_channel(ChannelConfig {
            label: "events".into(),
            negotiated: Some(12),
            ..Default::default()
        });
        client
            .signal(&json!({"type":"sfu_channels","channels":[
                {"channelId":10,"name":"state","memberId":7,"local":true},
                {"channelId":12,"name":"events","memberId":7,"local":true}
            ]}))
            .unwrap();
        let server_reply = server.direct_api().create_data_channel(ChannelConfig {
            label: "events".into(),
            negotiated: Some(14),
            ..Default::default()
        });
        client
            .signal(&json!({"type":"sfu_channels","channels":[
                {"channelId":14,"name":"events","memberId":17,"local":false}
            ]}))
            .unwrap();
        client
            .signal(&json!({"type":"start","startAt":1234,"players":[{"memberId":17,"slot":3}]}))
            .unwrap();
        client.released = true;
        client.drop_percent = 5;
        let until = Instant::now() + Duration::from_secs(10);
        let mut connected = false;
        let mut received_state = 0;
        let mut received_events = 0;
        let mut sent = false;
        let mut replied = false;
        let mut bound_sender = false;
        let mut b = [0u8; 65536];
        while Instant::now() < until {
            let (text, frames) = client.poll().unwrap();
            bound_sender |= frames.iter().any(|f| f == &[6, 3, 2]);
            if !replied {
                if let Some(mut c) = server.channel(server_reply) {
                    let mut reply = 1234u64.to_le_bytes().to_vec();
                    reply.extend_from_slice(&[6, 99, 2]);
                    replied = c.write(true, &reply).unwrap();
                }
            }
            connected |= text.iter().any(|x| x["type"] == "sfu_connected");
            if connected && !sent && client.rtc.channel(client.channels[0].id).is_some() {
                // Flush each snapshot separately, so replacement in the send queue is not counted as loss.
                for i in 0..100u8 {
                    client.send(&[5, 0, 4, 0, i]).unwrap();
                    client.poll().unwrap();
                }
                client.send(&[6, 0, 99]).unwrap();
                sent = true;
            }
            for _ in 0..1 {
                let Ok((n, source)) = socket.recv_from(&mut b) else {
                    break;
                };
                if let Ok(contents) = b[..n].try_into() {
                    server
                        .handle_input(Input::Receive(
                            Instant::now(),
                            Receive {
                                proto: Protocol::Udp,
                                source,
                                destination: socket.local_addr().unwrap(),
                                contents,
                            },
                        ))
                        .unwrap();
                }
            }
            server.handle_input(Input::Timeout(Instant::now())).unwrap();
            loop {
                match server.poll_output().unwrap() {
                    Output::Timeout(_) => break,
                    Output::Transmit(t) => {
                        socket.send_to(&t.contents, t.destination).unwrap();
                    }
                    Output::Event(Event::ChannelData(d)) if d.id == server_state => {
                        received_state += 1
                    }
                    Output::Event(Event::ChannelData(d)) if d.id == server_event => {
                        received_events += 1
                    }
                    _ => {}
                }
            }
            if received_state == 95 && received_events == 1 && bound_sender {
                break;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        assert!(connected);
        assert!(bound_sender);
        assert_eq!(received_state, 95);
        assert_eq!(received_events, 1);
    }
}
