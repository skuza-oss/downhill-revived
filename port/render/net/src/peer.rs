//! One WebRTC peer connection of a P2P room (finding 180): the host has one per guest, a guest one
//! to the host. str0m does ICE, DTLS and SCTP over this game's own UDP sockets; the TURN-over-TLS
//! relay is the SFU's (relay.rs, turn.rs). Two negotiated channels: 0 carries race snapshots
//! (unordered, no retransmission: a late one is worthless), 1 reliable events and room commands.
//!
//! Each peer has two host candidates on the same address. The first sends at once, as ICE does.
//! The second answers only: it sends nothing until the other game's packet has reached it. Some
//! computers refuse a UDP flow the game opens itself while letting through one the other side
//! opened (a Mac running a network filter: every send failed with "host unreachable", finding
//! 180), so if only the first candidate existed, the game that sends first would decide whether
//! the direct path works: the guest, which learns the host's candidates first. With the second,
//! one candidate pair is always opened from the far side.
use crate::{relay, turn};
use serde_json::{json, Value};
use std::collections::{HashMap, VecDeque};
use std::io::ErrorKind;
use std::net::{IpAddr, SocketAddr, ToSocketAddrs, UdpSocket};
use std::sync::Arc;
use std::time::{Duration, Instant};
use str0m::change::{SdpAnswer, SdpOffer, SdpPendingOffer};
use str0m::channel::{ChannelConfig, ChannelId, Reliability};
use str0m::net::{Protocol, Receive};
use str0m::{Candidate, CandidateKind, Event, IceConnectionState, Input, Output, Rtc};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
pub type Wake = Arc<dyn Fn() + Send + Sync>;

/// Reliable messages waiting for SCTP's window; more means the peer is stuck.
const EVENTS_MAX: usize = 256;
/// The largest channel message: a room command's JSON or a race frame (3 kB) with its header.
pub const MESSAGE_MAX: usize = 16384;
/// A peer not connected this long after it was made has failed (the relay is up within ~3 s).
const CONNECT_TIMEOUT: Duration = Duration::from_secs(20);
/// A connected peer whose ICE stays disconnected this long has failed. str0m reports Disconnected
/// once the path in use stopped answering its checks (about 22 s with its default timing) and no
/// other pair is left to try; it never closes the connection itself, so without this the other
/// game's rider froze for good (a relay that ended, a network gone). The wait lets a pair still
/// being checked (a late relay candidate) take over.
const ICE_LOST_GRACE: Duration = Duration::from_secs(5);
/// The STUN server for the server-reflexive candidate, and how often to ask before giving up.
const STUN_SERVER: (&str, u16) = ("stun.cloudflare.com", 3478);
const STUN_TRIES: u8 = 5;
const STUN_RETRY: Duration = Duration::from_millis(500);
/// str0m's statistics: the path's line in dhd.log (and DHD_NET_BENCH's counters each second).
const STATS_INTERVAL: Duration = Duration::from_secs(1);

/// The race channel's header: the race's start marker (8 bytes), the frame's type, its sender's
/// slot, then a state bundle's version and index (port/render/session/src/coordinator.rs).
const STATE_SENDER: usize = 10;
const STATE_INDEX: usize = 12;

/// How a connected peer is reached, for the log: a direct path inside the LAN or across the
/// internet, or Cloudflare's relay.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Path {
    Lan,
    Internet,
    Relay,
}

impl Path {
    pub fn describe(self) -> &'static str {
        match self {
            Path::Lan => "direct, inside the local network",
            Path::Internet => "direct, across the internet",
            Path::Relay => "Cloudflare's relay (TURN over TLS)",
        }
    }
}

struct Socket {
    udp: UdpSocket,
    local: SocketAddr,
    /// Sends nothing until `heard` (the module's comment says why).
    answer_only: bool,
    heard: bool,
}

struct Stun {
    server: SocketAddr,
    id: [u8; 12],
    next: Instant,
    tries: u8,
}

struct Relay {
    client: turn::Client,
    link: relay::Link,
    address: Option<SocketAddr>,
}

pub struct Peer {
    rtc: Rtc,
    /// Empty when direct UDP is off (DHD_NET_P2P_UDP=0): the relay carries everything.
    sockets: Vec<Socket>,
    pending: Option<SdpPendingOffer>,
    state: ChannelId,
    events: ChannelId,
    relay: Option<Relay>,
    /// The other game's relayed candidates: a pair to one of these is relayed at its end.
    remote_relays: Vec<SocketAddr>,
    stun: Option<Stun>,
    connected: bool,
    /// When ICE reported Disconnected after the connection was up (cleared when it recovers).
    lost_since: Option<Instant>,
    path: Option<Path>,
    created: Instant,
    outgoing_events: VecDeque<Vec<u8>>,
    /// The newest state bundle per (sender, bundle index): an older one waiting is replaced.
    outgoing_states: HashMap<(u8, u8), Vec<u8>>,
    signals: Vec<Value>,
    incoming: Vec<(bool, Vec<u8>)>,
    /// The first direct send the system refused, logged once.
    refused: Option<ErrorKind>,
}

impl Peer {
    /// `local` is the address this game reaches the internet from (its room connection's);
    /// `offerer` is the host's side; `direct` false leaves only the relay (tests of the fallback).
    pub fn new(local: IpAddr, offerer: bool, direct: bool) -> Result<Self> {
        Self::with_check_interval(local, offerer, direct, None)
    }

    /// `check_interval`: str0m's longest wait between ICE checks (its default, 3 s, when None);
    /// tests shorten it to see a lost path sooner.
    fn with_check_interval(local: IpAddr, offerer: bool, direct: bool, check_interval: Option<Duration>) -> Result<Self> {
        let mut config = Rtc::builder()
            .set_crypto_provider(str0m::crypto::from_feature_flags().into())
            .set_stats_interval(Some(STATS_INTERVAL));
        if let Some(interval) = check_interval {
            config.set_max_stun_rto(interval);
        }
        let mut rtc = config.build(Instant::now());
        let mut sockets = vec![];
        if direct {
            for answer_only in [false, true] {
                let udp = UdpSocket::bind(SocketAddr::new(local, 0))?;
                udp.set_nonblocking(true)?;
                let local = udp.local_addr()?;
                rtc.add_local_candidate(Candidate::host(local, "udp")?);
                sockets.push(Socket { udp, local, answer_only, heard: false });
            }
        }
        let stun = match (direct && !local.is_loopback(), STUN_SERVER.to_socket_addrs()) {
            (true, Ok(mut found)) => match found.find(|a| a.is_ipv4() == local.is_ipv4()) {
                Some(server) => Some(Stun { server, id: random_id()?, next: Instant::now(), tries: 0 }),
                None => None,
            },
            _ => None,
        };
        let state_config = ChannelConfig {
            label: "state".into(),
            negotiated: Some(0),
            ordered: false,
            reliability: Reliability::MaxRetransmits { retransmits: 0 },
            ..Default::default()
        };
        let event_config = ChannelConfig {
            label: "events".into(),
            negotiated: Some(1),
            ordered: true,
            reliability: Reliability::Reliable,
            ..Default::default()
        };
        let mut signals = vec![];
        let (state, events, pending) = if offerer {
            let mut change = rtc.sdp_api();
            let state = change.add_channel_with_config(state_config);
            let events = change.add_channel_with_config(event_config);
            let (offer, pending) = change.apply().ok_or("no peer offer")?;
            signals.push(json!({"type": "offer", "sdp": offer.to_sdp_string()}));
            (state, events, Some(pending))
        } else {
            // The channels are negotiated (fixed ids), so the answerer opens them itself.
            let state = rtc.direct_api().create_data_channel(state_config);
            let events = rtc.direct_api().create_data_channel(event_config);
            (state, events, None)
        };
        Ok(Self {
            rtc, sockets, pending, state, events, relay: None, remote_relays: vec![], stun, connected: false, lost_since: None, path: None,
            created: Instant::now(), outgoing_events: VecDeque::new(), outgoing_states: HashMap::new(),
            signals, incoming: vec![], refused: None,
        })
    }

    /// The other game's offer, answer or trickled candidate, as the room relays it.
    pub fn signal(&mut self, m: &Value) -> Result<()> {
        let text = |key: &str, max: usize| m[key].as_str().filter(|s| s.len() <= max).ok_or("invalid peer signal");
        match m["type"].as_str() {
            Some("offer") => {
                let answer = self.rtc.sdp_api().accept_offer(SdpOffer::from_sdp_string(text("sdp", MESSAGE_MAX)?)?)?;
                self.signals.push(json!({"type": "answer", "sdp": answer.to_sdp_string()}));
            }
            Some("answer") => {
                let pending = self.pending.take().ok_or("unexpected peer answer")?;
                self.rtc.sdp_api().accept_answer(pending, SdpAnswer::from_sdp_string(text("sdp", MESSAGE_MAX)?)?)?;
            }
            Some("candidate") => {
                let candidate = Candidate::from_sdp_string(text("candidate", 2048)?)?;
                if candidate.kind() == CandidateKind::Relayed {
                    self.remote_relays.push(candidate.addr());
                }
                self.rtc.add_remote_candidate(candidate);
            }
            _ => return Err("unknown peer signal".into()),
        }
        Ok(())
    }

    /// This game's own TURN credentials (the room's p2p_turn): a relayed candidate once allocated.
    /// Later ones (the room renews them before they expire, online/server/src/p2p.ts) keep the
    /// allocation alive: its Refresh is then signed with them.
    pub fn turn(&mut self, credentials: &Value, wake: Wake) -> Result<()> {
        let field = |k: &str| credentials[k].as_str().filter(|s| !s.is_empty() && s.len() <= 256);
        let username = field("username").ok_or("TURN username missing")?;
        let password = field("credential").ok_or("TURN credential missing")?;
        if let Some(r) = self.relay.as_mut() {
            r.client.renew(username, password);
            return Ok(());
        }
        let (host, port) = crate::sfu::turns_server(&credentials["urls"]).ok_or("no Cloudflare TURN/TLS server")?;
        let client = turn::Client::new(username, password, Instant::now());
        self.relay = Some(Relay { client, link: relay::Link::open(&host, port, wake), address: None });
        Ok(())
    }

    pub fn connected(&self) -> bool {
        self.connected
    }

    /// How the connection runs, once str0m's first statistics after connecting say.
    pub fn path(&self) -> Option<Path> {
        self.path
    }

    pub fn timed_out(&self) -> bool {
        !self.connected && self.created.elapsed() > CONNECT_TIMEOUT
    }

    /// Queues a channel message: reliable on the events channel, else a race state (the newest
    /// per sender and bundle index; an older one still waiting is dropped).
    pub fn send(&mut self, reliable: bool, bytes: Vec<u8>) -> Result<()> {
        if bytes.is_empty() || bytes.len() > MESSAGE_MAX {
            return Err("invalid peer message size".into());
        }
        if reliable {
            if self.outgoing_events.len() >= EVENTS_MAX {
                return Err("peer event queue full".into());
            }
            self.outgoing_events.push_back(bytes);
        } else {
            if bytes.len() <= STATE_INDEX || bytes[STATE_SENDER] >= 10 || bytes[STATE_INDEX] >= 10 {
                return Err("invalid peer state".into());
            }
            self.outgoing_states.insert((bytes[STATE_SENDER], bytes[STATE_INDEX]), bytes);
        }
        Ok(())
    }

    /// Reads what arrived, runs the connection and sends what is due. Returns the signals for the
    /// other game (through the room) and the channel messages received (reliable, bytes).
    pub fn poll(&mut self) -> Result<(Vec<Value>, Vec<(bool, Vec<u8>)>)> {
        self.ask_stun();
        let mut buffer = [0u8; 2048];
        for i in 0..self.sockets.len() {
            for _ in 0..256 {
                let (n, source) = match self.sockets[i].udp.recv_from(&mut buffer) {
                    Ok(got) => got,
                    // Windows reports an earlier send's ICMP error here; nothing to read now.
                    Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::Interrupted | ErrorKind::ConnectionReset) => break,
                    Err(e) => return Err(e.into()),
                };
                if self.stun_answer(i, source, &buffer[..n])? {
                    continue;
                }
                self.sockets[i].heard = true;
                let local = self.sockets[i].local;
                self.receive(source, local, &buffer[..n])?;
            }
        }
        self.poll_relay()?;
        self.rtc.handle_input(Input::Timeout(Instant::now()))?;
        self.flush()?;
        self.drain()?;
        if let Some(r) = self.relay.as_mut().filter(|r| r.link.ready()) {
            r.link.write(&r.client.output());
        }
        if !self.rtc.is_alive() {
            return Err("peer disconnected".into());
        }
        if self.lost_since.is_some_and(|at| at.elapsed() >= ICE_LOST_GRACE) {
            return Err("no path to the other game answers any more".into());
        }
        Ok((std::mem::take(&mut self.signals), std::mem::take(&mut self.incoming)))
    }

    /// The server-reflexive candidate: a Binding request from the first socket, retried a few times.
    fn ask_stun(&mut self) {
        let Some(stun) = self.stun.as_mut() else { return };
        if stun.tries >= STUN_TRIES || Instant::now() < stun.next {
            return;
        }
        // An unreachable STUN server only costs this candidate; ICE goes on without it.
        let _ = self.sockets[0].udp.send_to(&turn::binding_request(stun.id), stun.server);
        stun.tries += 1;
        stun.next = Instant::now() + STUN_RETRY;
    }

    /// True when the datagram was the STUN server's (an answer signals the candidate).
    fn stun_answer(&mut self, socket: usize, source: SocketAddr, bytes: &[u8]) -> Result<bool> {
        let Some(stun) = self.stun.as_ref().filter(|s| s.server == source && socket == 0) else { return Ok(false) };
        if let Some(mapped) = turn::binding_address(bytes, &stun.id) {
            let candidate = Candidate::server_reflexive(mapped, self.sockets[0].local, "udp")?;
            self.signals.push(json!({"type": "candidate", "candidate": candidate.to_sdp_string()}));
            self.rtc.add_local_candidate(candidate);
            self.stun = None;
        }
        Ok(true)
    }

    fn poll_relay(&mut self) -> Result<()> {
        let Some(r) = self.relay.as_mut() else { return Ok(()) };
        if r.link.failed() || r.client.failed().is_some() {
            // Only the relay is gone (its credentials end after an hour, finding 180): a direct
            // path goes on; a connection that ran over the relay fails through ICE.
            eprintln!("net: P2P: the relay failed ({}); direct paths go on", r.client.failed().unwrap_or("TLS stream closed"));
            self.relay = None;
            return Ok(());
        }
        if !r.link.ready() {
            return Ok(());
        }
        let now = Instant::now();
        let (bytes, _) = r.link.take();
        r.client.receive(&bytes, now);
        r.client.tick(now);
        let mut received = vec![];
        while let Some(event) = r.client.poll_event() {
            match event {
                turn::Event::Allocated(addr) => {
                    r.address = Some(addr);
                    let candidate = Candidate::relayed(addr, r.link.local().unwrap_or(addr), "udp")?;
                    self.signals.push(json!({"type": "candidate", "candidate": candidate.to_sdp_string()}));
                    self.rtc.add_local_candidate(candidate);
                }
                turn::Event::Data(source, data) => received.extend(r.address.map(|to| (source, to, data))),
            }
        }
        r.link.write(&r.client.output());
        for (from, to, data) in received {
            self.receive(from, to, &data)?;
        }
        Ok(())
    }

    fn receive(&mut self, source: SocketAddr, destination: SocketAddr, bytes: &[u8]) -> Result<()> {
        let Ok(contents) = bytes.try_into() else { return Ok(()) };
        let input = Input::Receive(Instant::now(), Receive { proto: Protocol::Udp, source, destination, contents });
        if self.rtc.accepts(&input) {
            self.rtc.handle_input(input)?;
            self.drain()?;
        }
        Ok(())
    }

    /// Hands str0m what is queued, as far as SCTP's buffer takes it.
    fn flush(&mut self) -> Result<()> {
        if let Some(mut channel) = self.rtc.channel(self.events) {
            while let Some(bytes) = self.outgoing_events.front() {
                if !channel.write(true, bytes)? {
                    break;
                }
                self.outgoing_events.pop_front();
            }
        }
        if let Some(mut channel) = self.rtc.channel(self.state) {
            self.outgoing_states.retain(|_, bytes| !channel.write(true, bytes).unwrap_or(false));
        }
        Ok(())
    }

    /// Runs str0m's outputs: datagrams to send, events, channel data.
    fn drain(&mut self) -> Result<()> {
        for _ in 0..512 {
            match self.rtc.poll_output()? {
                Output::Timeout(_) => break,
                Output::Transmit(t) => self.transmit(t.source, t.destination, &t.contents),
                Output::Event(Event::Connected) => self.connected = true,
                Output::Event(Event::IceConnectionStateChange(state)) => match state {
                    IceConnectionState::Disconnected if self.connected => {
                        self.lost_since.get_or_insert_with(Instant::now);
                    }
                    IceConnectionState::Connected | IceConnectionState::Completed => self.lost_since = None,
                    _ => {}
                },
                Output::Event(Event::PeerStats(stats)) => {
                    let relay = self.relay.as_ref().and_then(|r| r.address);
                    if let (true, None, Some(pair)) = (self.connected, self.path, &stats.selected_candidate_pair) {
                        self.path = Some(if relay == Some(pair.local.addr) || self.remote_relays.contains(&pair.remote.addr) {
                            Path::Relay
                        } else if is_local(pair.remote.addr.ip()) {
                            Path::Lan
                        } else {
                            Path::Internet
                        });
                    }
                    crate::diagnostics::peer("p2p", &stats, relay);
                }
                Output::Event(Event::ChannelData(data)) if data.binary && data.data.len() <= MESSAGE_MAX => {
                    if data.id == self.state || data.id == self.events {
                        self.incoming.push((data.id == self.events, data.data));
                    }
                }
                _ => {}
            }
        }
        Ok(())
    }

    fn transmit(&mut self, source: SocketAddr, destination: SocketAddr, contents: &[u8]) {
        if let Some(r) = self.relay.as_mut().filter(|r| r.address == Some(source)) {
            // A LAN candidate is tried directly: TURN servers refuse permissions to private
            // addresses, and such a refusal must not end the allocation.
            if !is_local(destination.ip()) {
                r.client.send_to(destination, contents, Instant::now());
            }
            return;
        }
        let Some(socket) = self.sockets.iter().find(|s| s.local == source) else { return };
        if socket.answer_only && !socket.heard {
            return; // ICE retransmits; once the other game reaches this socket it answers
        }
        if socket.local.ip().is_loopback() && !destination.ip().is_loopback() {
            return; // a local test server's room: only games on this machine are reachable directly
        }
        if let Err(e) = socket.udp.send_to(contents, destination) {
            if self.refused.is_none() {
                self.refused = Some(e.kind());
                eprintln!(
                    "net: P2P: this computer refused a direct send ({:?}): a firewall or network filter may hold \
                     back the game's own connections; those the other game opens, and the relay, still work",
                    e.kind()
                );
            }
        }
    }
}

/// The address the other games can reach this one at: the interface this machine reaches the
/// internet through (a route lookup toward the STUN server; nothing is sent), else `room`, the room
/// connection's own. Not the room connection's first: a local test server (or an ssh tunnel to one)
/// runs it over 127.0.0.1, where no other machine reaches.
pub fn reachable_address(room: Option<IpAddr>) -> Option<IpAddr> {
    let routed = STUN_SERVER.to_socket_addrs().ok().and_then(|mut found| found.find(|a| a.is_ipv4())).and_then(|server| {
        let probe = UdpSocket::bind(SocketAddr::new(IpAddr::from([0, 0, 0, 0]), 0)).ok()?;
        probe.connect(server).ok()?;
        Some(probe.local_addr().ok()?.ip())
    });
    routed.filter(|ip| !ip.is_unspecified()).or(room)
}

/// An address inside a local network (or this machine): direct only, never through the relay.
fn is_local(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(ip) => {
            ip.is_private() || ip.is_loopback() || ip.is_link_local() || ip.is_unspecified() || ip.is_multicast()
                || ip.is_broadcast() || ip.octets()[0] == 0
        }
        IpAddr::V6(ip) => {
            ip.is_loopback() || ip.is_unspecified() || ip.is_multicast() || ip.is_unique_local() || ip.is_unicast_link_local()
        }
    }
}

fn random_id() -> Result<[u8; 12]> {
    use ring::rand::SecureRandom;
    let mut id = [0; 12];
    ring::rand::SystemRandom::new().fill(&mut id).map_err(|_| "no random STUN transaction id")?;
    Ok(id)
}

impl Drop for Peer {
    fn drop(&mut self) {
        // Ends the allocation now rather than at its lifetime's end.
        if let Some(r) = self.relay.as_mut().filter(|r| r.link.ready()) {
            r.client.close(Instant::now());
            r.link.write(&r.client.output());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Runs two peers against each other until `done` says so, passing their signals across.
    fn exchange(a: &mut Peer, b: &mut Peer, seconds: u64, mut done: impl FnMut(&mut Peer, &[(bool, Vec<u8>)]) -> bool) {
        let end = Instant::now() + Duration::from_secs(seconds);
        while Instant::now() < end {
            let (signals, _) = a.poll().unwrap();
            for s in signals {
                b.signal(&s).unwrap();
            }
            let (signals, frames) = b.poll().unwrap();
            for s in signals {
                a.signal(&s).unwrap();
            }
            if done(a, &frames) {
                return;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        panic!("the peers did not exchange both message classes");
    }

    fn state(sender: u8) -> Vec<u8> {
        let mut bytes = vec![0; 16];
        bytes[8] = 5;
        bytes[STATE_SENDER] = sender;
        bytes
    }

    fn both_classes(a: &mut Peer, frames: &[(bool, Vec<u8>)], event: &mut bool, text: &[u8]) -> bool {
        if a.connected() {
            a.send(true, text.to_vec()).unwrap();
            a.send(false, state(1)).unwrap();
        }
        *event |= frames.iter().any(|(reliable, f)| *reliable && f == text);
        *event && frames.iter().any(|(reliable, f)| !reliable && f[8] == 5)
    }

    #[test]
    fn direct_peers_exchange_reliable_events_and_unreliable_states() {
        let ip = "127.0.0.1".parse().unwrap();
        let (mut a, mut b) = (Peer::new(ip, true, true).unwrap(), Peer::new(ip, false, true).unwrap());
        let mut event = false;
        exchange(&mut a, &mut b, 5, |a, frames| both_classes(a, frames, &mut event, b"hello"));
    }

    /// The other game goes silent after connecting (its network gone, its relay ended): this one
    /// fails within str0m's ICE timeout plus the grace, rather than keeping a frozen rider.
    #[test]
    fn a_connected_peer_whose_path_stops_answering_fails() {
        let ip = "127.0.0.1".parse().unwrap();
        let fast = Some(Duration::from_millis(400));
        let mut a = Peer::with_check_interval(ip, true, true, fast).unwrap();
        let mut b = Peer::with_check_interval(ip, false, true, fast).unwrap();
        let mut event = false;
        exchange(&mut a, &mut b, 5, |a, frames| both_classes(a, frames, &mut event, b"hello"));
        drop(b);
        let lost = Instant::now();
        // str0m keeps a pair that the other side checked in the last 15 s (its
        // RECENT_BINDING_REQUEST; its own checks give up sooner at this interval), then the grace.
        let limit = Duration::from_secs(15) + ICE_LOST_GRACE + Duration::from_secs(10);
        loop {
            match a.poll() {
                Err(e) => {
                    assert!(lost.elapsed() >= ICE_LOST_GRACE, "failed before the grace: {e}");
                    break;
                }
                Ok(_) => assert!(lost.elapsed() < limit, "the peer never failed after its path went silent"),
            }
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    #[test]
    fn the_answer_only_socket_waits_to_be_reached() {
        let ip = "127.0.0.1".parse().unwrap();
        let mut a = Peer::new(ip, true, true).unwrap();
        let (offer, _) = a.poll().unwrap();
        // Both host candidates are offered.
        assert_eq!(offer[0]["sdp"].as_str().unwrap().matches("typ host").count(), 2);
        assert!(a.sockets[1].answer_only && !a.sockets[1].heard);
        // A send from it before it was reached goes nowhere (and is no error).
        let (from, to) = (a.sockets[1].local, a.sockets[0].local);
        a.transmit(from, to, b"x");
        std::thread::sleep(Duration::from_millis(20));
        let mut buffer = [0u8; 16];
        assert!(a.sockets[0].udp.recv_from(&mut buffer).is_err());
        // Once something reached it, it sends.
        a.sockets[1].heard = true;
        a.transmit(from, to, b"x");
        std::thread::sleep(Duration::from_millis(20));
        assert_eq!(a.sockets[0].udp.recv_from(&mut buffer).unwrap().0, 1);
    }

    #[test]
    fn states_keep_only_the_newest_per_sender_and_index() {
        let mut a = Peer::new("127.0.0.1".parse().unwrap(), true, true).unwrap();
        let mut older = state(2);
        older[15] = 1;
        a.send(false, older).unwrap();
        a.send(false, state(2)).unwrap();
        a.send(false, state(3)).unwrap();
        assert_eq!(a.outgoing_states.len(), 2);
        assert_eq!(a.outgoing_states[&(2, 0)][15], 0);
        assert!(a.send(false, vec![0; 8]).is_err());
    }

    /// Revokes the test's TURN credentials when it ends, passed or not.
    struct Revoke(Vec<String>);
    impl Drop for Revoke {
        fn drop(&mut self) {
            for user in &self.0 {
                crate::sfu::probe::revoke_turn(user);
            }
        }
    }

    #[test]
    #[ignore = "needs Cloudflare TURN credentials and the network"]
    fn cloudflare_turn_tls_without_direct_udp() {
        let (_, ca) = crate::sfu::probe::turn_credentials("p2p-qualification-a");
        let (_, cb) = crate::sfu::probe::turn_credentials("p2p-qualification-b");
        let _revoke = Revoke(vec![ca["username"].as_str().unwrap().into(), cb["username"].as_str().unwrap().into()]);
        let ip = "0.0.0.0".parse().unwrap();
        let (mut a, mut b) = (Peer::new(ip, true, false).unwrap(), Peer::new(ip, false, false).unwrap());
        a.turn(&ca, Arc::new(|| {})).unwrap();
        b.turn(&cb, Arc::new(|| {})).unwrap();
        let mut event = false;
        exchange(&mut a, &mut b, 30, |a, frames| both_classes(a, frames, &mut event, b"turn-event"));
    }

    /// Two relay-only peers; `a`'s first credentials last 60 s and are renewed once connected:
    /// Cloudflare takes the Refresh signed with the new ones on the live allocation (a refusal,
    /// 401 or 441, fails the client at once), and the relay carries the peers' events past the
    /// first credentials' end. About 3 minutes. (Not renewed, a relay outlived its expired
    /// credentials by 11 minutes on 2026-11-09: when Cloudflare ends one is not pinned down, so
    /// no test relies on it; finding 180.)
    #[test]
    #[ignore = "needs Cloudflare TURN credentials and the network; about 3 minutes"]
    fn cloudflare_turn_takes_renewed_credentials_on_the_live_allocation() {
        use crate::sfu::probe::{turn_credentials, turn_credentials_lasting};
        let (ua, ca) = turn_credentials_lasting("p2p-renewal-a", 60);
        let (ub, cb) = turn_credentials("p2p-renewal-b");
        let (ua2, ca2) = turn_credentials("p2p-renewal-a2");
        let _revoke = Revoke(vec![ua, ub, ua2.clone()]);
        let ip = "0.0.0.0".parse().unwrap();
        let (mut a, mut b) = (Peer::new(ip, true, false).unwrap(), Peer::new(ip, false, false).unwrap());
        a.turn(&ca, Arc::new(|| {})).unwrap();
        b.turn(&cb, Arc::new(|| {})).unwrap();
        let mut event = false;
        exchange(&mut a, &mut b, 30, |a, frames| both_classes(a, frames, &mut event, b"first"));
        a.turn(&ca2, Arc::new(|| {})).unwrap();
        let start = Instant::now();
        let mut last_heard = Instant::now();
        while start.elapsed() < Duration::from_secs(150) {
            let (signals, _) = a.poll().unwrap();
            for s in signals {
                b.signal(&s).unwrap();
            }
            let (signals, frames) = b.poll().unwrap();
            for s in signals {
                a.signal(&s).unwrap();
            }
            if frames.iter().any(|(reliable, f)| *reliable && f == b"tick") {
                last_heard = Instant::now();
            }
            if a.outgoing_events.is_empty() {
                a.send(true, b"tick".to_vec()).unwrap();
            }
            assert!(last_heard.elapsed() < Duration::from_secs(10), "nothing arrived for 10 s after the renewal");
            std::thread::sleep(Duration::from_millis(5));
        }
        let client = &a.relay.as_ref().expect("the relay is gone").client;
        assert!(client.failed().is_none());
        assert_eq!(client.username(), ua2, "the renewal was never applied");
    }
}
