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
/// A game says sfu_ready once it has every racer's channels. A racer whose own connection never
/// comes up has none to give, so every other game waited for it, and a room without SFU_CONNECT_MS
/// (servers before b0812d6) took the whole race out at its setup deadline (0.7.12, rooms of 4-7).
/// This long after its own association connected, with its own channels open, a game says
/// sfu_ready all the same (finding 175). The room releases the race only when every connected
/// racer is subscribed both ways, so nothing is lost: the missing racer has nothing published.
/// Longer than another racer's TURN setup (2.9-3.2 s from the answer, measured in finding 175),
/// and with this game's own connection (about 1 s after the start) before the room's 10 s
/// SFU_CONNECT_MS.
pub const READY_TIMEOUT: Duration = Duration::from_millis(8_000);
/// An offer the room never answers (it refused it: too many sessions) fails the association this
/// long after it was made; the send thread offers again later.
pub const ANSWER_TIMEOUT: Duration = Duration::from_secs(15);
/// The room's TURN credentials last an hour (online/server/src/sfu.ts TURN_TTL_S) and a relay
/// can't change its credentials (RFC 8656: every request of an allocation uses its username), while
/// the association now lasts the whole room stay (finding 177). Between races, once its credentials
/// are this old, the game replaces its association (a new session asks for new credentials at
/// once): 25 minutes left are more than a race with its finish wait, and the allocation outlives
/// its credentials by up to 10 minutes more (its last Refresh).
pub const TURN_RENEW: Duration = Duration::from_secs(35 * 60);
/// The relay in use: the TURN client, its TLS stream and the relayed address once allocated.
struct Relay {
    client: turn::Client,
    link: relay::Link,
    relayed: Option<SocketAddr>,
    since: Instant,
}
struct Channel {
    id: ChannelId,
    stream: u16,
    member: u64,
    state: bool,
    local: bool,
    /// When it was created, and whether it has been seen open.
    since: Instant,
    opened: bool,
}
/// A channel the room allocated that waits for str0m to let go of its stream id (open_waiting).
struct Waiting {
    stream: u16,
    member: u64,
    state: bool,
    local: bool,
    since: Instant,
}
/// A channel the room closed that str0m still holds: Cloudflare resets the stream itself when the
/// room closes a subscription, and str0m answers with its own reset (finding 177). Closing it from
/// this side at the same moment made the two resets collide and left the stream id unusable, so
/// this side closes it only if Cloudflare's reset hasn't come CLOSE_GRACE later.
struct Closing {
    id: ChannelId,
    stream: u16,
    since: Instant,
    closed: bool,
    /// str0m had given it its stream (it can be reset).
    seen: bool,
}
/// How long a closed channel waits for Cloudflare's own stream reset before this side resets it.
const CLOSE_GRACE: Duration = Duration::from_secs(2);
/// A channel the room allocated that hasn't opened this long after (its stream id still held by a
/// reset that never completed) fails the association: a new one starts clean.
const OPEN_LIMIT: Duration = Duration::from_secs(10);
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
    waiting: Vec<Waiting>,
    closing: Vec<Closing>,
    /// Stream ids a closed channel that never got its stream still holds in str0m.
    orphans: Vec<u16>,
    /// The racers of the latest race: member id -> slot.
    slots: HashMap<u64, u8>,
    /// The latest race's marker (its start's startAt; 0 before the first), kept after its end so
    /// its late frames are told apart from the next race's early ones.
    race: u64,
    /// That race is running for this game: from its start message to its `ended`.
    race_open: bool,
    /// This game's own member id in that race (from the start's yourSlot), and when the start came.
    me: Option<u64>,
    started_at: Option<Instant>,
    created: Instant,
    future_events: VecDeque<(ChannelId, Vec<u8>)>,
    connected: bool,
    /// This race's sfu_ready was said (announced) and answered (released).
    announced: bool,
    released: bool,
    drop_percent: u64,
    sent: u64,
    /// DHD_NET_SFU_DROP_IN (test switch): this share in percent of the DTLS application records
    /// received from the SFU over UDP is dropped before str0m, as a lossy link would.
    drop_in_percent: u64,
    records_in: u64,
    pending_events: VecDeque<(Instant, Vec<u8>)>,
    pending_state: HashMap<u8, (Instant, Vec<u8>)>,
    rates: HashMap<u64, SenderRate>,
    wake: Wake,
    /// False with DHD_NET_SFU_UDP=0: a test switch that drops every UDP datagram to and from the
    /// SFU, as a network that blocks UDP does, so the TURN/TLS fallback must carry the race.
    direct: bool,
    /// False with DHD_NET_SFU_TURN=0 (test switch, finding 175): the TURN fallback is never asked
    /// for; with DHD_NET_SFU_UDP=0 too, a racer whose connection never comes up.
    turn_allowed: bool,
    answered_at: Option<Instant>,
    connected_at: Option<Instant>,
    turn_asked: bool,
    /// Ask for TURN at once, not DIRECT_TIMEOUT after the answer: the association this one
    /// replaces had connected through the relay (UDP to the SFU is blocked here).
    turn_first: bool,
    /// When this association's TURN credentials came (their TTL runs from then: TURN_RENEW).
    turn_at: Option<Instant>,
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

// Per bundle index (a STATE message's byte 3, port/hle/state_bundle.h): four times the normal
// rate (bundle 0 carries a player's own rider, 60 a second), so 240 a second; more in one second
// are dropped. A sender is never silenced for the rest of the race (0.7.12 did that): after a
// network stall the backlog can arrive at once, several seconds of an honest sender's snapshots,
// and that froze its rider for the rest of the race (finding 169).
//
// Over the limit a message is still taken when its stamp (the moment its snapshots show) is newer
// than the newest taken from that index, at most FLOOD_PER_SECOND more a second (finding 175): a
// backlog arrives oldest first, so the excess the limit used to drop was the newest, the one the
// puppet needed to catch up. A flood stays bounded (twice the limit) and old or repeated stamps
// over the limit are dropped.
const FLOOD_PER_SECOND: u16 = 240;
struct SenderRate { since: Instant, count: [u16; 10], newer: [u16; 10], newest: [f64; 10], dropped: u32 }
impl SenderRate {
    fn new() -> Self { Self { since: Instant::now(), count: [0; 10], newer: [0; 10], newest: [f64::MIN; 10], dropped: 0 } }
    /// `stamp`: the bundle's server-clock moment (None: a message without one, counted only).
    fn accept(&mut self, rider: u8, stamp: Option<f64>, now: Instant) -> bool {
        if now.duration_since(self.since).as_secs() >= 1 { self.count = [0; 10]; self.newer = [0; 10]; self.since = now; }
        let i = rider as usize;
        self.count[i] = self.count[i].saturating_add(1);
        let newest = stamp.is_some_and(|t| t.is_finite() && t > self.newest[i]);
        let ok = self.count[i] <= FLOOD_PER_SECOND || (newest && self.newer[i] < FLOOD_PER_SECOND);
        if ok && self.count[i] > FLOOD_PER_SECOND { self.newer[i] += 1; }
        if ok && newest { self.newest[i] = stamp.unwrap(); }
        if !ok { self.dropped += 1; }
        ok
    }
}
impl Sfu {
    /// A new association and its offer. `turn_first`: ask for TURN as soon as the answer comes.
    pub fn start(wake: Wake, turn_first: bool) -> Result<(Self, Value)> {
        let route = ("rtc.live.cloudflare.com", 443).to_socket_addrs()?.find(|a| a.is_ipv4()).ok_or("No SFU IPv4 route")?;
        let (mut sfu, offer) = Self::start_at(route, wake)?;
        sfu.turn_first = turn_first;
        Ok((sfu, offer))
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
        let turn_allowed = std::env::var("DHD_NET_SFU_TURN").map_or(true, |v| v.trim() != "0");
        if !turn_allowed { eprintln!("net: SFU: DHD_NET_SFU_TURN=0, no TURN relay is asked for (test switch)"); }
        let drop_in_percent = std::env::var("DHD_NET_SFU_DROP_IN").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(0u64).min(100);
        if drop_in_percent > 0 { eprintln!("net: SFU: DHD_NET_SFU_DROP_IN={drop_in_percent}, that share of the SFU's records is dropped on arrival (test switch)"); }
        Ok((Self { rtc, pending: Some(pending), socket, local, inbox, reader_alive, reader_failed, stats: Stats::default(), channels: vec![], waiting: vec![], closing: vec![], orphans: vec![], slots: HashMap::new(),
            race: 0, race_open: false, me: None, started_at: None, created: Instant::now(), future_events: VecDeque::new(),
            connected: false, announced: false, released: false, drop_percent, sent: 0, drop_in_percent, records_in: 0, pending_events: VecDeque::new(), pending_state: HashMap::new(), rates: HashMap::new(),
            wake, direct, turn_allowed, answered_at: None, connected_at: None, turn_asked: false, turn_first: false, turn_at: None, relay: None, last_in_relayed: false, via_relay: false,
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
            self.turn_at = Some(Instant::now());
        } else if message["type"] == "start" {
            // A race on the association the room stay keeps (finding 177): its per-race state
            // starts over (racers, flood counters, queued frames, the acknowledgement), the
            // channels stay.
            self.race = message["startAt"].as_u64().filter(|n| *n > 0).ok_or("Missing race marker")?;
            self.race_open = true;
            self.started_at = Some(Instant::now());
            self.slots.clear();
            self.rates.clear();
            self.announced = false; self.released = false;
            self.pending_state.clear();
            self.pending_events.clear();
            let you = message["yourSlot"].as_u64();
            self.me = None;
            if let Some(players) = message["players"].as_array() {
                for p in players {
                    if let (Some(member), Some(slot)) = (p["memberId"].as_u64(), p["slot"].as_u64())
                    {
                        if slot < 10 {
                            self.slots.insert(member, slot as u8);
                            if you == Some(slot) { self.me = Some(member); }
                        }
                    }
                }
            }
        } else if message["type"] == "ended" {
            // The race is over; the association stays for the next one. Its late frames are dropped.
            self.race_open = false;
            self.announced = false; self.released = false;
            self.pending_state.clear();
            self.pending_events.clear();
        } else if message["type"] == "left" {
            if let Some(member) = message["memberId"].as_u64() {
                self.slots.remove(&member);
            }
        } else if message["type"] == "sfu_ready" {
            // This race's release (an older race's says nothing about this one).
            if self.race_open && message["race"].as_u64().is_none_or(|r| r == self.race) && !self.released {
                self.released = true;
                eprintln!("net: SFU: race released {} ms after its start message ({} channels)",
                    self.started_at.map_or(0, |t| t.elapsed().as_millis()), self.channels.len());
            }
        } else if message["type"] == "sfu_closed" {
            if let Some(ids) = message["channelIds"].as_array() {
                for id in ids.iter().filter_map(Value::as_u64).filter(|id| *id < 65535) {
                    self.close_stream(id as u16);
                }
            }
        } else if message["type"] == "sfu_channels" {
            let channels = message["channels"]
                .as_array()
                .ok_or("Missing allocations")?;
            // A rejoin's whole list (finding 175): the channels it lacks are new, those it has stay,
            // and those not listed closed while the room connection was down (their sfu_closed missed).
            let full = message["full"] == true;
            if full {
                let listed: Vec<u64> = channels.iter().filter_map(|c| c["channelId"].as_u64()).collect();
                let gone: Vec<u16> = self.channels.iter().map(|c| c.stream).chain(self.waiting.iter().map(|w| w.stream))
                    .filter(|s| !listed.contains(&(*s as u64))).collect();
                for stream in gone { self.close_stream(stream); }
            }
            let known = |s: &Self, c: &Value| c["channelId"].as_u64().is_some_and(|id| s.has_stream(id));
            let new = if full { channels.iter().filter(|c| !known(self, c)).count() } else { channels.len() };
            if self.channels.len() + self.waiting.len() + new > 20 {
                return Err("Too many channels".into());
            }
            for c in channels {
                if full && known(self, c) { continue; }
                let stream = c["channelId"]
                    .as_u64()
                    .filter(|id| *id < 65535)
                    .ok_or("Invalid channel ID")? as u16;
                let name = c["name"]
                    .as_str()
                    .filter(|s| *s == "state" || *s == "events")
                    .ok_or("Invalid channel name")?;
                // A live channel of ours only: before the association is up str0m knows no
                // stream ids yet, and a duplicate would panic inside it.
                if self.has_stream(stream as u64) {
                    return Err("Duplicate channel ID".into());
                }
                self.waiting.push(Waiting {
                    stream,
                    member: c["memberId"].as_u64().ok_or("Missing member")?,
                    state: name == "state",
                    local: c["local"].as_bool().ok_or("Missing direction")?,
                    since: Instant::now(),
                });
            }
            self.open_waiting();
        }
        if message["type"] == "sfu_channels" { eprintln!("net: SFU channels allocated: {}", self.channels.len() + self.waiting.len()); }
        Ok(())
    }
    /// This association has a channel on that stream id, open or waiting.
    fn has_stream(&self, stream: u64) -> bool {
        self.channels.iter().any(|c| c.stream as u64 == stream) || self.waiting.iter().any(|w| w.stream as u64 == stream)
    }
    /// A channel the room closed (sfu_closed, or missing from a rejoin's whole list): forgotten
    /// here, and left to Cloudflare's own reset (Closing).
    fn close_stream(&mut self, stream: u16) {
        self.waiting.retain(|w| w.stream != stream);
        let gone: Vec<ChannelId> = self.channels.iter().filter(|c| c.stream == stream).map(|c| c.id).collect();
        for id in gone {
            self.channels.retain(|c| c.id != id);
            let seen = self.rtc.direct_api().channel_id_by_sctp_stream_id(stream) == Some(id);
            self.closing.push(Closing { id, stream, since: Instant::now(), closed: false, seen });
        }
    }
    /// Creates the channels the room allocated once str0m has let go of their stream id. Cloudflare
    /// gives a closed subscription's id to the next one on that session at once (finding 177), while
    /// str0m keeps the old channel until its stream reset has gone through; creating the new one
    /// before that would panic in str0m (it then waits by itself for the reset's completion).
    fn open_waiting(&mut self) {
        let mut i = 0;
        while i < self.waiting.len() {
            let stream = self.waiting[i].stream;
            if self.rtc.direct_api().channel_id_by_sctp_stream_id(stream).is_some() || self.closing.iter().any(|c| c.stream == stream)
                || self.orphans.contains(&stream) {
                i += 1;
                continue;
            }
            let w = self.waiting.remove(i);
            let id = self.rtc.direct_api().create_data_channel(ChannelConfig {
                label: if w.state { "state" } else { "events" }.into(),
                ordered: !w.state,
                negotiated: Some(stream),
                reliability: if w.state { Reliability::MaxRetransmits { retransmits: 0 } } else { Reliability::Reliable },
                ..Default::default()
            });
            self.channels.push(Channel { id, stream, member: w.member, state: w.state, local: w.local, since: w.since, opened: false });
        }
    }
    /// Closed channels str0m dropped are done; one whose reset from Cloudflare hasn't come in
    /// CLOSE_GRACE is reset from here. One that never got its stream (made while its id was still
    /// held) can't be reset: its id stays unusable on this association (`orphans`). A channel that
    /// hasn't opened in OPEN_LIMIT fails the association: a new one starts clean.
    fn upkeep(&mut self) -> Result<()> {
        let mut i = 0;
        while i < self.closing.len() {
            let c = &mut self.closing[i];
            let held = self.rtc.direct_api().channel_id_by_sctp_stream_id(c.stream) == Some(c.id);
            if held {
                c.seen = true;
                if !c.closed && c.since.elapsed() >= CLOSE_GRACE {
                    c.closed = true;
                    let id = c.id;
                    self.rtc.direct_api().close_data_channel(id);
                }
                i += 1;
            } else if c.seen {
                self.closing.remove(i);
            } else if c.since.elapsed() >= CLOSE_GRACE {
                self.orphans.push(c.stream);
                self.closing.remove(i);
            } else {
                i += 1;
            }
        }
        for c in self.channels.iter_mut().filter(|c| !c.opened) {
            if self.rtc.channel(c.id).is_some() { c.opened = true; }
            else if self.connected && c.since.elapsed() >= OPEN_LIMIT { return Err("An SFU channel never opened".into()); }
        }
        if self.connected && self.waiting.iter().any(|w| w.since.elapsed() >= OPEN_LIMIT) {
            return Err("An SFU channel's stream id stayed held".into());
        }
        Ok(())
    }    /// The association has its answer: the room made its session (a rejoin may keep it).
    pub fn answered(&self) -> bool { self.answered_at.is_some() }
    /// It connected through the TURN relay (a replacement asks for TURN at once).
    pub fn via_relay(&self) -> bool { self.via_relay }
    /// Its TURN credentials are TURN_RENEW old and no race runs: time for a new association.
    pub fn renew_due(&self) -> bool {
        !self.race_open && self.relay.is_some() && self.turn_at.is_some_and(|t| t.elapsed() >= TURN_RENEW)
    }
    /// A race's channels on this association: its own two, open.
    fn open(&mut self, member: u64, local: bool) -> bool {
        let ids: Vec<ChannelId> = self.channels.iter().filter(|c| c.local == local && c.member == member).map(|c| c.id).collect();
        ids.len() >= 2 && ids.iter().all(|id| self.rtc.channel(*id).is_some())
    }
    fn own_open(&mut self) -> bool {
        let ids: Vec<ChannelId> = self.channels.iter().filter(|c| c.local).map(|c| c.id).collect();
        ids.len() >= 2 && ids.iter().all(|id| self.rtc.channel(*id).is_some())
    }
    /// READY_TIMEOUT after this race's start (and its own connection), its own channels open, some
    /// racer's still missing (an older room that never says `left` for a racer who never connects).
    fn ready_anyway(&mut self) -> bool {
        let since = match (self.started_at, self.connected_at) { (Some(s), Some(c)) => s.max(c), _ => return false };
        if since.elapsed() < READY_TIMEOUT || !self.own_open() { return false; }
        eprintln!("net: SFU: ready without every racer's channels {} s after the start", READY_TIMEOUT.as_secs());
        true
    }
    /// Every other racer's two channels and this game's own are open: the race may be released.
    fn usable(&mut self) -> bool {
        if !self.own_open() { return false; }
        let others: Vec<u64> = self.slots.keys().copied().filter(|m| Some(*m) != self.me).collect();
        others.into_iter().all(|m| self.open(m, false))
    }
    /// Frames queued by send() that the next poll would write now.
    pub fn unwritten(&self) -> bool {
        self.released && !(self.pending_state.is_empty() && self.pending_events.is_empty())
    }
    /// Queues a race frame. Outside a race (a frame the game sent as its race ended) it is dropped.
    pub fn send(&mut self, bytes: &[u8]) -> Result<()> {
        if bytes.len() < 2 || bytes.len() > 3072 || !self.race_open {
            return Ok(());
        }
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
            // One newest bundle per bundle index (byte 3, port/hle/state_bundle.h); never
            // accumulate stale snapshots under congestion.
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
    }    pub fn poll(&mut self) -> Result<(Vec<Value>, Vec<Vec<u8>>)> {
        let mut text = vec![];
        let mut frames = vec![];
        let pending = std::mem::take(&mut self.future_events);
        for (id, bytes) in pending { self.receive_frame(id, bytes, &mut frames)?; }
        if !(self.waiting.is_empty() && self.closing.is_empty()) { self.upkeep()?; self.open_waiting(); }
        else if self.channels.iter().any(|c| !c.opened) { self.upkeep()?; }
        if self.pending.is_some() && self.created.elapsed() >= ANSWER_TIMEOUT {
            return Err("No SFU answer".into());
        }
        if self.race_open && !self.announced && (self.usable() || self.ready_anyway()) {
            self.announced = true; text.push(json!({"type":"sfu_ready","race":self.race}));
            eprintln!("net: SFU: channels ready {} ms after the start message ({} racers, association up {} s)",
                self.started_at.map_or(0, |t| t.elapsed().as_millis()), self.slots.len(),
                self.connected_at.map_or(0, |t| t.elapsed().as_secs()));
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
        if let Some(line) = self.stats.poll(now, datagrams.len(), self.race_open && self.released, via) { eprintln!("{line}"); }
        for (at, source, data) in datagrams {
            if !self.direct { continue; }
            self.stats.input(now.saturating_duration_since(at));
            // 23: a DTLS application record (SCTP), not ICE or the handshake.
            if self.drop_in_percent > 0 && data.first() == Some(&23) {
                self.records_in += 1;
                if (self.records_in * 37) % 100 < self.drop_in_percent { continue; }
            }
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
        let wait = if self.turn_first { Duration::ZERO } else { DIRECT_TIMEOUT };
        if !self.connected && !self.turn_asked && self.turn_allowed && self.answered_at.is_some_and(|t| t.elapsed() >= wait) {
            self.turn_asked = true;
            if self.turn_first { eprintln!("net: SFU: the last association went through TURN; asking for a relay at once"); }
            else { eprintln!("net: SFU: no UDP connection {} ms after the answer; asking for a TURN relay", DIRECT_TIMEOUT.as_millis()); }
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
        } else if epoch == self.race && self.race_open {
            if c.state {
                if bytes.len() < 12 || bytes[11] >= 10 { return Ok(()); }
                // A bundle's stamp (version 6: bytes 5-12 of the frame, after the 8-byte race marker).
                let stamp = (bytes.len() >= 21 && bytes[10] == 6).then(|| f64::from_le_bytes(bytes[13..21].try_into().unwrap()));
                let rate = self.rates.entry(c.member).or_insert_with(SenderRate::new);
                if !rate.accept(bytes[11], stamp, Instant::now()) {
                    if rate.dropped == 1 { eprintln!("net: SFU: member {} sent over {FLOOD_PER_SECOND} snapshot messages in a second; the excess is dropped", c.member); }
                    return Ok(());
                }
            }
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
                    self.connected_at = Some(Instant::now());
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
    fn snapshot_flood_drops_the_excess_and_never_silences() {
        let mut rate = SenderRate::new();
        let now = rate.since;
        for _ in 0..FLOOD_PER_SECOND { assert!(rate.accept(0, None, now)); }
        assert!(rate.accept(1, None, now));
        assert!(!rate.accept(0, None, now));
        for _ in 0..70_000 { rate.accept(0, None, now); }  // no overflow
        assert_eq!(rate.dropped, 70_001);
        // a backlog after a stall costs the excess of that second, not the rest of the race
        assert!(rate.accept(0, None, now + Duration::from_secs(2)));
        assert!(rate.accept(1, None, now + Duration::from_secs(2)));
    }

    /// A backlog arrives oldest first: over the limit the newest still come through (finding 175),
    /// old and repeated stamps don't, and a flood of ever newer stamps stays at twice the limit.
    #[test]
    fn snapshot_flood_keeps_the_newest() {
        let mut rate = SenderRate::new();
        let now = rate.since;
        // 5 s of a 60 Hz sender at once: 300 messages, stamps rising 16.7 ms apart
        let taken: Vec<bool> = (0..300).map(|k| rate.accept(0, Some(1000.0 + k as f64 * 16.7), now)).collect();
        assert!(taken.iter().all(|t| *t), "every one of the backlog, the newest last");
        assert!(!rate.accept(0, Some(1000.0), now), "an old stamp over the limit");
        assert!(!rate.accept(0, Some(1000.0 + 299.0 * 16.7), now), "a repeated one");
        assert!(!rate.accept(0, None, now), "none at all");
        // a flood of ever newer stamps: at most twice the limit a second, per bundle index
        let mut flood = SenderRate::new();
        let now = flood.since;
        let taken = (0..10_000).filter(|k| flood.accept(3, Some(*k as f64), now)).count();
        assert_eq!(taken, 2 * FLOOD_PER_SECOND as usize);
        assert!(flood.accept(3, Some(1e9), now + Duration::from_secs(1)));
    }

    /// A rejoin's whole channel list (finding 175): kept channels stay the same, missing ones are
    /// opened, those it doesn't list go (their sfu_closed was missed); a plain list still refuses a
    /// channel it already has.
    #[test]
    fn a_rejoins_full_channel_list_reconciles() {
        let (mut client, _) = Sfu::start_at("127.0.0.1:9".parse().unwrap(), Arc::new(|| {})).unwrap();
        client.signal(&json!({"type":"sfu_channels","channels":[
            {"channelId":14,"name":"events","memberId":17,"local":false},
            {"channelId":16,"name":"state","memberId":17,"local":false}
        ]})).unwrap();
        let kept = client.channels.iter().find(|c| c.stream == 16).unwrap().id;
        client.signal(&json!({"type":"sfu_channels","full":true,"channels":[
            {"channelId":16,"name":"state","memberId":17,"local":false},
            {"channelId":18,"name":"state","memberId":21,"local":false}
        ]})).unwrap();
        let streams: Vec<u16> = client.channels.iter().map(|c| c.stream).collect();
        assert_eq!(streams, vec![16, 18]);
        assert_eq!(client.channels[0].id, kept);
        assert!(client.signal(&json!({"type":"sfu_channels","channels":[
            {"channelId":18,"name":"state","memberId":21,"local":false}]})).is_err());
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

    /// A racer whose connection never comes up publishes nothing, so this game never has every
    /// racer's channels: READY_TIMEOUT after its own association connected it says sfu_ready all
    /// the same, once (finding 175), not before.
    #[test]
    fn ready_without_a_racer_that_never_connects() {
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        socket.set_nonblocking(true).unwrap();
        let local = socket.local_addr().unwrap();
        let mut server = Rtc::builder().set_crypto_provider(str0m::crypto::from_feature_flags().into()).build(Instant::now());
        server.add_local_candidate(Candidate::host(local, "udp").unwrap());
        let (mut client, offer) = Sfu::start_at(local, Arc::new(|| {})).unwrap();
        let answer = server.sdp_api().accept_offer(str0m::change::SdpOffer::from_sdp_string(offer["sdp"].as_str().unwrap()).unwrap()).unwrap();
        client.signal(&json!({"type":"sfu_answer","sdp":answer.to_sdp_string()})).unwrap();
        for (id, label) in [(10, "state"), (12, "events")] {
            server.direct_api().create_data_channel(ChannelConfig { label: label.into(), negotiated: Some(id), ..Default::default() });
        }
        client.signal(&json!({"type":"sfu_channels","channels":[
            {"channelId":10,"name":"state","memberId":7,"local":true},
            {"channelId":12,"name":"events","memberId":7,"local":true}]})).unwrap();
        // Two racers: member 17's channels never come.
        client.signal(&json!({"type":"start","startAt":1234,"players":[{"memberId":7,"slot":0},{"memberId":17,"slot":1}]})).unwrap();
        let mut b = [0u8; 65536];
        let mut readies = 0;
        let mut open = false;
        let until = Instant::now() + Duration::from_secs(10);
        while Instant::now() < until && !open {
            let (text, _) = client.poll().unwrap();
            readies += text.iter().filter(|m| m["type"] == "sfu_ready").count();
            open = client.connected && client.channels.iter().all(|c| client.rtc.channel(c.id).is_some());
            while let Ok((n, source)) = socket.recv_from(&mut b) {
                if let Ok(contents) = b[..n].try_into() {
                    server.handle_input(Input::Receive(Instant::now(), Receive { proto: Protocol::Udp, source, destination: local, contents })).unwrap();
                }
            }
            server.handle_input(Input::Timeout(Instant::now())).unwrap();
            while let Output::Transmit(t) = server.poll_output().unwrap() { socket.send_to(&t.contents, t.destination).unwrap(); }
            std::thread::sleep(Duration::from_millis(1));
        }
        assert!(open, "its own channels opened");
        let (text, _) = client.poll().unwrap();
        readies += text.iter().filter(|m| m["type"] == "sfu_ready").count();
        assert_eq!(readies, 0, "not before READY_TIMEOUT");
        client.connected_at = Some(Instant::now() - READY_TIMEOUT);
        client.started_at = client.connected_at;
        let (text, _) = client.poll().unwrap();
        assert_eq!(text.iter().filter(|m| m["type"] == "sfu_ready").count(), 1);
        let (text, _) = client.poll().unwrap();
        assert!(text.iter().all(|m| m["type"] != "sfu_ready"), "once");
    }

    /// Cloudflare forwards a subscribed STATE channel ordered and reliable, whatever policy was
    /// asked (finding 173). Here the "SFU" sends that way and loses message 10 and its
    /// retransmissions (the only datagrams that large): every later message must come out at
    /// once, not behind message 10.
    #[test]
    fn relay_sent_ordered_state_is_delivered_unordered() {
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        socket.set_nonblocking(true).unwrap();
        let local = socket.local_addr().unwrap();
        let mut server = Rtc::builder().set_crypto_provider(str0m::crypto::from_feature_flags().into()).build(Instant::now());
        server.add_local_candidate(Candidate::host(local, "udp").unwrap());
        let (mut client, offer) = Sfu::start_at(local, Arc::new(|| {})).unwrap();
        let answer = server.sdp_api().accept_offer(str0m::change::SdpOffer::from_sdp_string(offer["sdp"].as_str().unwrap()).unwrap()).unwrap();
        client.signal(&json!({"type":"sfu_answer","sdp":answer.to_sdp_string()})).unwrap();
        // As the SFU sends it: ordered, reliable (U bit 0, retransmitted).
        let relayed = server.direct_api().create_data_channel(ChannelConfig { label: "state".into(), negotiated: Some(16), ..Default::default() });
        client.signal(&json!({"type":"sfu_channels","channels":[{"channelId":16,"name":"state","memberId":17,"local":false}]})).unwrap();
        client.signal(&json!({"type":"start","startAt":1234,"players":[{"memberId":17,"slot":3}]})).unwrap();
        let mut b = [0u8; 65536];
        // Moves the server's datagrams to the client, except those carrying message 10.
        let pump = |server: &mut Rtc, lose_large: bool, b: &mut [u8; 65536]| {
            while let Ok((n, source)) = socket.recv_from(b) {
                if let Ok(contents) = b[..n].try_into() {
                    server.handle_input(Input::Receive(Instant::now(), Receive { proto: Protocol::Udp, source, destination: local, contents })).unwrap();
                }
            }
            server.handle_input(Input::Timeout(Instant::now())).unwrap();
            loop {
                match server.poll_output().unwrap() {
                    Output::Timeout(_) => break,
                    Output::Transmit(t) => { if !lose_large || t.contents.len() < 300 { socket.send_to(&t.contents, t.destination).unwrap(); } }
                    _ => {}
                }
            }
        };
        let mut arrived: HashMap<u8, Instant> = HashMap::new();
        let mut sent_at: HashMap<u8, Instant> = HashMap::new();
        let mut next = 0u8;
        let until = Instant::now() + Duration::from_secs(10);
        while Instant::now() < until && arrived.len() < 49 {
            let (_, frames) = client.poll().unwrap();
            for f in frames { if f.len() >= 5 && f[0] == 5 { arrived.entry(f[4]).or_insert_with(Instant::now); } }
            let open = server.channel(relayed).is_some() && client.rtc.channel(client.channels[0].id).is_some();
            if open && next < 50 && sent_at.get(&next.wrapping_sub(1)).map_or(true, |t| t.elapsed() >= Duration::from_millis(5)) {
                let mut wire = 1234u64.to_le_bytes().to_vec();
                wire.extend_from_slice(&[5, 0, 4, 0, next]);
                if next == 10 { wire.resize(400, 0); }
                if server.channel(relayed).unwrap().write(true, &wire).unwrap() {
                    sent_at.insert(next, Instant::now());
                    next += 1;
                }
            }
            pump(&mut server, next > 10, &mut b);
            std::thread::sleep(Duration::from_millis(1));
        }
        assert_eq!(next, 50, "the channel opened and every message was written");
        for i in 11..50u8 {
            let at = arrived.get(&i).unwrap_or_else(|| panic!("message {i} never arrived"));
            let held = at.duration_since(sent_at[&i]);
            assert!(held < Duration::from_millis(300), "message {i} waited {held:?} behind the lost message 10");
        }
        assert!(!arrived.contains_key(&10), "message 10 was lost");
    }

    /// A local str0m "SFU" for the room-stay tests: one socket, the client's datagrams in, its out.
    struct LocalSfu { server: Rtc, socket: UdpSocket, local: SocketAddr }
    impl LocalSfu {
        fn start() -> (Self, Sfu) {
            let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
            socket.set_nonblocking(true).unwrap();
            let local = socket.local_addr().unwrap();
            let mut server = Rtc::builder().set_crypto_provider(str0m::crypto::from_feature_flags().into()).build(Instant::now());
            server.add_local_candidate(Candidate::host(local, "udp").unwrap());
            let (mut client, offer) = Sfu::start_at(local, Arc::new(|| {})).unwrap();
            let answer = server.sdp_api().accept_offer(str0m::change::SdpOffer::from_sdp_string(offer["sdp"].as_str().unwrap()).unwrap()).unwrap();
            client.signal(&json!({"type":"sfu_answer","sdp":answer.to_sdp_string()})).unwrap();
            (Self { server, socket, local }, client)
        }
        /// One round both ways; the server's channel data by channel.
        fn pump(&mut self, client: &mut Sfu, text: &mut Vec<Value>, frames: &mut Vec<Vec<u8>>) -> Vec<(ChannelId, Vec<u8>)> {
            let (t, f) = client.poll().unwrap();
            text.extend(t);
            frames.extend(f);
            let mut b = [0u8; 65536];
            while let Ok((n, source)) = self.socket.recv_from(&mut b) {
                if let Ok(contents) = b[..n].try_into() {
                    self.server.handle_input(Input::Receive(Instant::now(), Receive { proto: Protocol::Udp, source, destination: self.local, contents })).unwrap();
                }
            }
            self.server.handle_input(Input::Timeout(Instant::now())).unwrap();
            let mut got = vec![];
            loop {
                match self.server.poll_output().unwrap() {
                    Output::Timeout(_) => break,
                    Output::Transmit(t) => { self.socket.send_to(&t.contents, t.destination).unwrap(); }
                    Output::Event(Event::ChannelData(d)) => got.push((d.id, d.data)),
                    _ => {}
                }
            }
            std::thread::sleep(Duration::from_millis(1));
            got
        }
        fn channel(&mut self, id: u16, state: bool) -> ChannelId {
            self.server.direct_api().create_data_channel(ChannelConfig {
                label: if state { "state" } else { "events" }.into(), ordered: !state, negotiated: Some(id),
                reliability: if state { Reliability::MaxRetransmits { retransmits: 0 } } else { Reliability::Reliable }, ..Default::default() })
        }
    }
    fn event(race: u64, payload: u8) -> Vec<u8> {
        let mut wire = race.to_le_bytes().to_vec();
        wire.extend_from_slice(&[6, 9, payload]);
        wire
    }

    /// Finding 177: one association for the room stay. Two races on it: each race starts its own
    /// state (acknowledgement for that race, release, queued frames, flood counters) while the
    /// connection and the channels stay; an ended race's frames are dropped both ways.
    #[test]
    fn the_association_outlives_races_and_each_race_starts_its_own_state() {
        let (mut sfu, mut client) = LocalSfu::start();
        let (own_state, own_events) = (sfu.channel(10, true), sfu.channel(12, false));
        let their_events = sfu.channel(14, false);
        sfu.channel(16, true);
        client.signal(&json!({"type":"sfu_channels","channels":[
            {"channelId":10,"name":"state","memberId":7,"local":true}, {"channelId":12,"name":"events","memberId":7,"local":true}]})).unwrap();
        client.signal(&json!({"type":"sfu_channels","channels":[
            {"channelId":14,"name":"events","memberId":17,"local":false}, {"channelId":16,"name":"state","memberId":17,"local":false}]})).unwrap();
        let (mut text, mut frames) = (vec![], vec![]);
        let until = Instant::now() + Duration::from_secs(10);
        while Instant::now() < until && !(client.connected && client.channels.iter().all(|c| client.rtc.channel(c.id).is_some())
            && sfu.server.channel(own_events).is_some() && sfu.server.channel(their_events).is_some()) {
            sfu.pump(&mut client, &mut text, &mut frames);
        }
        assert!(text.iter().any(|m| m["type"] == "sfu_connected"));
        assert!(text.iter().all(|m| m["type"] != "sfu_ready"), "no acknowledgement outside a race");
        let ids: Vec<ChannelId> = client.channels.iter().map(|c| c.id).collect();
        for (race, payload) in [(1000u64, 1u8), (2000, 2)] {
            text.clear();
            client.signal(&json!({"type":"start","startAt":race,"yourSlot":0,"players":[{"memberId":7,"slot":0},{"memberId":17,"slot":1}]})).unwrap();
            assert!(client.rates.is_empty() && client.pending_events.is_empty() && client.pending_state.is_empty() && !client.announced);
            sfu.pump(&mut client, &mut text, &mut frames);
            let acks: Vec<&Value> = text.iter().filter(|m| m["type"] == "sfu_ready").collect();
            assert_eq!(acks.len(), 1, "race {race}: acknowledged at once, its channels being open");
            assert_eq!(acks[0]["race"], race);
            // Frames wait for this race's release; an older race's release says nothing.
            client.send(&[6, 0, payload]).unwrap();
            client.signal(&json!({"type":"sfu_ready","race":race - 1})).unwrap();
            assert!(!client.released);
            client.signal(&json!({"type":"sfu_ready","race":race})).unwrap();
            let mut got = vec![];
            frames.clear();
            let until = Instant::now() + Duration::from_secs(5);
            while Instant::now() < until && (got.is_empty() || frames.is_empty()) {
                if frames.is_empty() { sfu.server.channel(their_events).unwrap().write(true, &event(race, payload)).unwrap(); }
                got.extend(sfu.pump(&mut client, &mut text, &mut frames));
            }
            assert!(got.iter().any(|(id, d)| *id == own_events && d[..8] == race.to_le_bytes() && d[10] == payload), "race {race}: ours sent with its marker");
            assert_eq!(frames[0], vec![6, 1, payload], "race {race}: theirs taken");
            client.signal(&json!({"type":"ended"})).unwrap();
            assert!(!client.race_open && !client.released);
            // After the end: nothing more goes out, an ended race's frames are dropped.
            client.send(&[6, 0, 99]).unwrap();
            assert!(client.pending_events.is_empty());
            frames.clear();
            sfu.server.channel(their_events).unwrap().write(true, &event(race, 99)).unwrap();
            for _ in 0..50 { sfu.pump(&mut client, &mut text, &mut frames); }
            assert!(frames.is_empty(), "race {race}: a frame after its end");
        }
        assert_eq!(client.channels.iter().map(|c| c.id).collect::<Vec<_>>(), ids, "the same channels throughout");
        assert!(client.connected && client.rtc.is_alive());
        let _ = own_state;
    }

    /// Finding 177: the room closes a subscription and Cloudflare gives its stream id to the next
    /// one at once. The client waits for the old channel's reset before it makes the new one
    /// (str0m would panic on a live duplicate), and the new channel carries the new member's frames.
    #[test]
    fn a_closed_subscriptions_stream_id_serves_the_next_one() {
        let (mut sfu, mut client) = LocalSfu::start();
        sfu.channel(10, true);
        sfu.channel(12, false);
        let old = sfu.channel(14, false);
        client.signal(&json!({"type":"sfu_channels","channels":[
            {"channelId":10,"name":"state","memberId":7,"local":true}, {"channelId":12,"name":"events","memberId":7,"local":true},
            {"channelId":14,"name":"events","memberId":17,"local":false}]})).unwrap();
        let (mut text, mut frames) = (vec![], vec![]);
        let until = Instant::now() + Duration::from_secs(10);
        while Instant::now() < until && !(client.channels.iter().all(|c| client.rtc.channel(c.id).is_some()) && sfu.server.channel(old).is_some()) {
            sfu.pump(&mut client, &mut text, &mut frames);
        }
        // Member 17 leaves: the room closes its subscription (the SFU resets the stream), and the
        // same id comes back at once for member 21's events.
        client.signal(&json!({"type":"sfu_closed","channelIds":[14]})).unwrap();
        sfu.server.direct_api().close_data_channel(old);
        client.signal(&json!({"type":"sfu_channels","channels":[{"channelId":14,"name":"events","memberId":21,"local":false}]})).unwrap();
        assert!(client.channels.iter().all(|c| c.stream != 14), "not made while the old one is held");
        client.signal(&json!({"type":"start","startAt":3000,"yourSlot":0,"players":[{"memberId":7,"slot":0},{"memberId":21,"slot":4}]})).unwrap();
        let mut new = None;
        let until = Instant::now() + Duration::from_secs(10);
        while Instant::now() < until && frames.is_empty() {
            sfu.pump(&mut client, &mut text, &mut frames);
            if new.is_none() && sfu.server.direct_api().channel_id_by_sctp_stream_id(14).is_none() { new = Some(sfu.channel(14, false)); }
            if let Some(id) = new { if let Some(mut c) = sfu.server.channel(id) { let _ = c.write(true, &event(3000, 5)); } }
        }
        assert_eq!(frames.first(), Some(&vec![6, 4, 5]), "the new subscription's frames, from member 21");
    }
}
