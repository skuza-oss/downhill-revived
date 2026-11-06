//! A TURN client (RFC 8656) for one relayed UDP allocation over a TLS stream, sans-IO like str0m:
//! the caller writes what `output()` returns to the stream and feeds what the stream reads to
//! `receive()`. Only what the SFU fallback needs (finding 158, round 4): Allocate with the
//! long-term credential, Refresh, ChannelBind (which also installs the permission) and ChannelData.
//! Over a stream there are no retransmissions (RFC 8489 6.2.2); a transaction unanswered within
//! TRANSACTION_TIMEOUT fails the allocation.
use ring::hmac;
use std::collections::{HashMap, VecDeque};
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};
use std::time::{Duration, Instant};

const MAGIC: u32 = 0x2112_A442;
// Methods.
const ALLOCATE: u16 = 0x003;
const REFRESH: u16 = 0x004;
const DATA: u16 = 0x007;
const CHANNEL_BIND: u16 = 0x009;
// Classes, as the bits they set in the message type.
const REQUEST: u16 = 0x0000;
const INDICATION: u16 = 0x0010;
const SUCCESS: u16 = 0x0100;
const ERROR: u16 = 0x0110;
// Attributes.
const USERNAME: u16 = 0x0006;
const MESSAGE_INTEGRITY: u16 = 0x0008;
const ERROR_CODE: u16 = 0x0009;
const CHANNEL_NUMBER: u16 = 0x000C;
const LIFETIME: u16 = 0x000D;
const XOR_PEER_ADDRESS: u16 = 0x0012;
const DATA_ATTR: u16 = 0x0013;
const REALM: u16 = 0x0014;
const NONCE: u16 = 0x0015;
const XOR_RELAYED_ADDRESS: u16 = 0x0016;
const REQUESTED_TRANSPORT: u16 = 0x0019;

/// A request with no answer this long fails the allocation (RFC 8489's 39.5 s is for a call;
/// a race setup would rather fail over to "no SFU" than wait that long).
pub const TRANSACTION_TIMEOUT: Duration = Duration::from_secs(10);
/// A ChannelBind is renewed this long after its success. The binding lasts 10 minutes (RFC 8656
/// 12), but the permission it installs only 5 (RFC 8656 9), and ChannelData does not renew
/// either: renewing at 300 s reached the server after the permission had lapsed, so relayed
/// datagrams were dropped at each renewal of a race over 5 minutes (release review, finding 158).
/// 240 s leaves a minute for the request's way there, a stalled TLS stream and TRANSACTION_TIMEOUT.
const CHANNEL_REFRESH: Duration = Duration::from_secs(240);
/// At most this many datagrams wait for their channel's binding; more are dropped (ICE and
/// SCTP retransmit what matters).
const QUEUED_MAX: usize = 32;
/// Peers one allocation talks to: the SFU's candidates for this session (a handful).
const PEERS_MAX: usize = 16;

#[derive(Debug, PartialEq)]
pub enum Event {
    /// The allocation exists: the relayed address str0m gets as a candidate.
    Allocated(SocketAddr),
    /// A datagram from a peer through the relay.
    Data(SocketAddr, Vec<u8>),
}

#[derive(Clone, Copy, PartialEq, Debug)]
enum Kind {
    Allocate,
    Refresh,
    Bind(u16),
}

pub struct Client {
    username: String,
    password: String,
    realm: Option<String>,
    nonce: Option<String>,
    key: Option<Vec<u8>>,
    pub(crate) relayed: Option<SocketAddr>,
    lifetime: Duration,
    refresh_at: Option<Instant>,
    /// Requests in flight: transaction id -> what it is and when it was sent.
    pending: HashMap<[u8; 12], (Kind, Instant)>,
    /// Peer -> channel number, and channel -> (peer, bound, last bound).
    by_peer: HashMap<SocketAddr, u16>,
    channels: HashMap<u16, (SocketAddr, bool, Option<Instant>)>,
    queued: VecDeque<(u16, Vec<u8>)>,
    out: Vec<u8>,
    events: VecDeque<Event>,
    failed: Option<String>,
    rx: Vec<u8>,
    seq: u64,
}

impl Client {
    pub fn new(username: &str, password: &str, now: Instant) -> Self {
        let mut c = Self {
            username: username.into(), password: password.into(), realm: None, nonce: None, key: None,
            relayed: None, lifetime: Duration::from_secs(600), refresh_at: None, pending: HashMap::new(),
            by_peer: HashMap::new(), channels: HashMap::new(), queued: VecDeque::new(), out: vec![],
            events: VecDeque::new(), failed: None, rx: vec![], seq: 0,
        };
        // The first Allocate goes without credentials: its 401 brings the realm and nonce.
        c.request(Kind::Allocate, now);
        c
    }

    pub fn failed(&self) -> Option<&str> { self.failed.as_deref() }
    /// Bytes wait in `output()` (the probe).
    #[cfg(test)]
    pub fn output_pending(&self) -> bool { !self.out.is_empty() }
    /// The server's 401 brought a realm and nonce (the probe).
    #[cfg(test)]
    pub fn challenged(&self) -> bool { self.realm.is_some() && self.nonce.is_some() }
    /// A ChannelBind is in flight (the probe).
    #[cfg(test)]
    pub fn binding(&self) -> bool { self.pending.values().any(|(k, _)| matches!(k, Kind::Bind(_))) }
    pub fn poll_event(&mut self) -> Option<Event> { self.events.pop_front() }
    /// Bytes to write to the stream, in order.
    pub fn output(&mut self) -> Vec<u8> { std::mem::take(&mut self.out) }

    /// A datagram to `peer` through the relay: ChannelData once its channel is bound, queued until
    /// then (the first datagram to a peer asks for the binding).
    pub fn send_to(&mut self, peer: SocketAddr, data: &[u8], now: Instant) {
        if self.relayed.is_none() || self.failed.is_some() || data.len() > 0xFFFF { return; }
        let channel = match self.by_peer.get(&peer) {
            Some(c) => *c,
            None => {
                if self.by_peer.len() >= PEERS_MAX { return; }
                let c = 0x4000 + self.by_peer.len() as u16;
                self.by_peer.insert(peer, c);
                self.channels.insert(c, (peer, false, None));
                self.request(Kind::Bind(c), now);
                c
            }
        };
        if self.channels[&channel].1 {
            channel_data(&mut self.out, channel, data);
        } else if self.queued.len() < QUEUED_MAX {
            self.queued.push_back((channel, data.to_vec()));
        }
    }

    /// Bytes read from the stream (any split): STUN messages and ChannelData, framed by their own
    /// lengths (RFC 8656 12.5: ChannelData is padded to 4 bytes over a stream).
    pub fn receive(&mut self, bytes: &[u8], now: Instant) {
        self.rx.extend_from_slice(bytes);
        loop {
            if self.rx.len() < 4 { return; }
            let len = u16::from_be_bytes([self.rx[2], self.rx[3]]) as usize;
            let total = match self.rx[0] >> 6 {
                0 => 20 + len,
                1 => 4 + ((len + 3) & !3),
                _ => { self.fail("garbage on the TURN stream"); return; }
            };
            if self.rx.len() < total { return; }
            let frame: Vec<u8> = self.rx.drain(..total).collect();
            if frame[0] >> 6 == 1 {
                let channel = u16::from_be_bytes([frame[0], frame[1]]);
                if let Some((peer, true, _)) = self.channels.get(&channel) {
                    self.events.push_back(Event::Data(*peer, frame[4..4 + len].to_vec()));
                }
            } else {
                self.on_message(&frame, now);
            }
        }
    }

    /// Refreshes and timeouts; call at least every second.
    pub fn tick(&mut self, now: Instant) {
        if self.failed.is_some() { return; }
        if self.pending.values().any(|(_, at)| now.duration_since(*at) > TRANSACTION_TIMEOUT) {
            return self.fail("TURN server did not answer");
        }
        if self.refresh_at.is_some_and(|at| now >= at) && !self.pending.values().any(|(k, _)| *k == Kind::Refresh) {
            self.refresh_at = None;
            self.request(Kind::Refresh, now);
        }
        let due: Vec<u16> = self.channels.iter()
            .filter(|(c, (_, bound, at))| *bound && at.is_some_and(|at| now.duration_since(at) >= CHANNEL_REFRESH)
                && !self.pending.values().any(|(k, _)| *k == Kind::Bind(**c)))
            .map(|(c, _)| *c).collect();
        for c in due {
            if let Some(entry) = self.channels.get_mut(&c) { entry.2 = None; }
            self.request(Kind::Bind(c), now);
        }
    }

    /// Ends the allocation (Refresh with lifetime 0), best effort.
    pub fn close(&mut self, now: Instant) {
        if self.relayed.is_some() && self.failed.is_none() {
            self.lifetime = Duration::ZERO;
            self.request(Kind::Refresh, now);
        }
    }

    fn fail(&mut self, why: &str) {
        if self.failed.is_none() { self.failed = Some(why.into()); }
    }

    fn request(&mut self, kind: Kind, now: Instant) {
        self.seq += 1;
        let mut id = [0u8; 12];
        let r = ring::rand::SystemRandom::new();
        if ring::rand::SecureRandom::fill(&r, &mut id).is_err() {
            id[..8].copy_from_slice(&self.seq.to_le_bytes());
        }
        let method = match kind { Kind::Allocate => ALLOCATE, Kind::Refresh => REFRESH, Kind::Bind(_) => CHANNEL_BIND };
        let mut m = Message::new(method | REQUEST, id);
        match kind {
            Kind::Allocate => m.attr(REQUESTED_TRANSPORT, &[17, 0, 0, 0]),
            Kind::Refresh => m.attr(LIFETIME, &(self.lifetime.as_secs() as u32).to_be_bytes()),
            Kind::Bind(c) => {
                let mut v = c.to_be_bytes().to_vec();
                v.extend_from_slice(&[0, 0]);
                m.attr(CHANNEL_NUMBER, &v);
                m.attr(XOR_PEER_ADDRESS, &xor_address(self.channels[&c].0, &id));
            }
        }
        if let (Some(realm), Some(nonce), Some(key)) = (&self.realm, &self.nonce, &self.key) {
            m.attr(USERNAME, self.username.as_bytes());
            m.attr(REALM, realm.as_bytes());
            m.attr(NONCE, nonce.as_bytes());
            m.integrity(key);
        }
        self.out.extend_from_slice(&m.bytes);
        self.pending.insert(id, (kind, now));
    }

    fn on_message(&mut self, frame: &[u8], now: Instant) {
        let Some(m) = Parsed::new(frame) else { return };
        let method = m.kind & 0x3EEF;
        let class = m.kind & 0x0110;
        if class == INDICATION && method == DATA {
            if let (Some(peer), Some(data)) = (m.get(XOR_PEER_ADDRESS).and_then(|v| unxor_address(v, &m.id)), m.get(DATA_ATTR)) {
                if self.by_peer.contains_key(&peer) { self.events.push_back(Event::Data(peer, data.to_vec())); }
            }
            return;
        }
        if class != SUCCESS && class != ERROR { return; }
        let Some((kind, _)) = self.pending.remove(&m.id) else { return };
        // Answers to authenticated requests must carry a valid integrity (RFC 8489 9.2.4).
        if let Some(key) = &self.key {
            if !m.verify(frame, key) && class == SUCCESS { return self.fail("TURN answer failed its integrity check"); }
        }
        if class == ERROR {
            let code = m.get(ERROR_CODE).filter(|v| v.len() >= 4).map_or(0, |v| v[2] as u16 * 100 + v[3] as u16);
            let realm = m.get(REALM).map(|v| String::from_utf8_lossy(v).into_owned());
            let nonce = m.get(NONCE).map(|v| String::from_utf8_lossy(v).into_owned());
            // 401 the first time (no key yet), 438 whenever the nonce goes stale: once more with them.
            let retry = (code == 401 && self.key.is_none()) || code == 438;
            if retry && nonce.is_some() && (realm.is_some() || self.realm.is_some()) {
                if let Some(r) = realm { self.realm = Some(r); }
                self.nonce = nonce;
                let realm = self.realm.clone().unwrap_or_default();
                self.key = Some(md5(format!("{}:{}:{}", self.username, realm, self.password).as_bytes()).to_vec());
                return self.request(kind, now);
            }
            if kind == Kind::Refresh && self.lifetime.is_zero() { return; }
            return self.fail(&format!("TURN error {code}"));
        }
        match kind {
            Kind::Allocate => {
                let Some(addr) = m.get(XOR_RELAYED_ADDRESS).and_then(|v| unxor_address(v, &m.id)) else {
                    return self.fail("TURN allocation without a relayed address");
                };
                self.relayed = Some(addr);
                self.lifetime = Duration::from_secs(m.get(LIFETIME).filter(|v| v.len() == 4)
                    .map_or(600, |v| u32::from_be_bytes([v[0], v[1], v[2], v[3]]) as u64));
                self.schedule_refresh(now);
                self.events.push_back(Event::Allocated(addr));
            }
            Kind::Refresh => {
                if let Some(v) = m.get(LIFETIME).filter(|v| v.len() == 4) {
                    self.lifetime = Duration::from_secs(u32::from_be_bytes([v[0], v[1], v[2], v[3]]) as u64);
                }
                if !self.lifetime.is_zero() { self.schedule_refresh(now); }
            }
            Kind::Bind(c) => {
                if let Some(entry) = self.channels.get_mut(&c) { entry.1 = true; entry.2 = Some(now); }
                let (ready, waiting): (VecDeque<_>, VecDeque<_>) = std::mem::take(&mut self.queued).into_iter().partition(|(q, _)| *q == c);
                self.queued = waiting;
                for (_, data) in ready { channel_data(&mut self.out, c, &data); }
            }
        }
    }

    /// The next Refresh a minute before the allocation would end (at half of a short lifetime).
    fn schedule_refresh(&mut self, now: Instant) {
        let lead = if self.lifetime > Duration::from_secs(120) { self.lifetime - Duration::from_secs(60) } else { self.lifetime / 2 };
        self.refresh_at = Some(now + lead);
    }
}

fn channel_data(out: &mut Vec<u8>, channel: u16, data: &[u8]) {
    out.extend_from_slice(&channel.to_be_bytes());
    out.extend_from_slice(&(data.len() as u16).to_be_bytes());
    out.extend_from_slice(data);
    out.resize(out.len() + (4 - data.len() % 4) % 4, 0);
}

struct Message {
    bytes: Vec<u8>,
}
impl Message {
    fn new(kind: u16, id: [u8; 12]) -> Self {
        let mut bytes = Vec::with_capacity(128);
        bytes.extend_from_slice(&kind.to_be_bytes());
        bytes.extend_from_slice(&[0, 0]);
        bytes.extend_from_slice(&MAGIC.to_be_bytes());
        bytes.extend_from_slice(&id);
        Self { bytes }
    }
    fn set_len(&mut self) {
        let n = (self.bytes.len() - 20) as u16;
        self.bytes[2..4].copy_from_slice(&n.to_be_bytes());
    }
    fn attr(&mut self, kind: u16, value: &[u8]) {
        self.bytes.extend_from_slice(&kind.to_be_bytes());
        self.bytes.extend_from_slice(&(value.len() as u16).to_be_bytes());
        self.bytes.extend_from_slice(value);
        self.bytes.resize(self.bytes.len() + (4 - value.len() % 4) % 4, 0);
        self.set_len();
    }
    /// MESSAGE-INTEGRITY: HMAC-SHA1 over the message with its length already counting the attribute.
    fn integrity(&mut self, key: &[u8]) {
        let n = (self.bytes.len() - 20 + 24) as u16;
        self.bytes[2..4].copy_from_slice(&n.to_be_bytes());
        let tag = hmac::sign(&hmac::Key::new(hmac::HMAC_SHA1_FOR_LEGACY_USE_ONLY, key), &self.bytes);
        self.attr(MESSAGE_INTEGRITY, tag.as_ref());
    }
}

struct Parsed<'a> {
    kind: u16,
    id: [u8; 12],
    attrs: Vec<(u16, usize, &'a [u8])>,
}
impl<'a> Parsed<'a> {
    fn new(b: &'a [u8]) -> Option<Self> {
        if b.len() < 20 || u32::from_be_bytes([b[4], b[5], b[6], b[7]]) != MAGIC { return None; }
        let len = u16::from_be_bytes([b[2], b[3]]) as usize;
        if b.len() != 20 + len || len % 4 != 0 { return None; }
        let mut attrs = vec![];
        let mut at = 20;
        while at + 4 <= b.len() {
            let kind = u16::from_be_bytes([b[at], b[at + 1]]);
            let n = u16::from_be_bytes([b[at + 2], b[at + 3]]) as usize;
            if at + 4 + n > b.len() { return None; }
            attrs.push((kind, at, &b[at + 4..at + 4 + n]));
            at += 4 + ((n + 3) & !3);
        }
        Some(Self { kind: u16::from_be_bytes([b[0], b[1]]), id: b[8..20].try_into().ok()?, attrs })
    }
    fn get(&self, kind: u16) -> Option<&'a [u8]> {
        // Attributes after MESSAGE-INTEGRITY (other than FINGERPRINT) are ignored (RFC 8489 14.5).
        let mut seen_integrity = false;
        for (k, _, v) in &self.attrs {
            if seen_integrity { return None; }
            if *k == kind { return Some(v); }
            seen_integrity |= *k == MESSAGE_INTEGRITY;
        }
        None
    }
    fn verify(&self, frame: &[u8], key: &[u8]) -> bool {
        let Some((_, at, tag)) = self.attrs.iter().find(|(k, _, _)| *k == MESSAGE_INTEGRITY) else { return false };
        let mut head = frame[..*at].to_vec();
        head[2..4].copy_from_slice(&((at - 20 + 24) as u16).to_be_bytes());
        hmac::verify(&hmac::Key::new(hmac::HMAC_SHA1_FOR_LEGACY_USE_ONLY, key), &head, tag).is_ok()
    }
}

fn xor_address(addr: SocketAddr, id: &[u8; 12]) -> Vec<u8> {
    let port = addr.port() ^ (MAGIC >> 16) as u16;
    let mut v = vec![0];
    match addr.ip() {
        IpAddr::V4(ip) => {
            v.push(1);
            v.extend_from_slice(&port.to_be_bytes());
            v.extend_from_slice(&(u32::from(ip) ^ MAGIC).to_be_bytes());
        }
        IpAddr::V6(ip) => {
            v.push(2);
            v.extend_from_slice(&port.to_be_bytes());
            let mut mask = MAGIC.to_be_bytes().to_vec();
            mask.extend_from_slice(id);
            v.extend(ip.octets().iter().zip(mask).map(|(a, b)| a ^ b));
        }
    }
    v
}

fn unxor_address(v: &[u8], id: &[u8; 12]) -> Option<SocketAddr> {
    if v.len() < 8 { return None; }
    let port = u16::from_be_bytes([v[2], v[3]]) ^ (MAGIC >> 16) as u16;
    match v[1] {
        1 => Some(SocketAddr::new(Ipv4Addr::from(u32::from_be_bytes([v[4], v[5], v[6], v[7]]) ^ MAGIC).into(), port)),
        2 if v.len() >= 20 => {
            let mut mask = MAGIC.to_be_bytes().to_vec();
            mask.extend_from_slice(id);
            let o: Vec<u8> = v[4..20].iter().zip(mask).map(|(a, b)| a ^ b).collect();
            Some(SocketAddr::new(Ipv6Addr::from(<[u8; 16]>::try_from(o).ok()?).into(), port))
        }
        _ => None,
    }
}

/// MD5 (RFC 1321), only for the long-term credential's key: MD5(username:realm:password) is what
/// RFC 8489 9.2.2 prescribes; nothing else here uses it.
fn md5(input: &[u8]) -> [u8; 16] {
    const S: [u32; 64] = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];
    let k: Vec<u32> = (0..64).map(|i| ((i as f64 + 1.0).sin().abs() * 4294967296.0) as u32).collect();
    let mut h: [u32; 4] = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476];
    let mut msg = input.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 { msg.push(0); }
    msg.extend_from_slice(&((input.len() as u64).wrapping_mul(8)).to_le_bytes());
    for chunk in msg.chunks(64) {
        let m: Vec<u32> = chunk.chunks(4).map(|w| u32::from_le_bytes([w[0], w[1], w[2], w[3]])).collect();
        let [mut a, mut b, mut c, mut d] = h;
        for i in 0..64 {
            let (f, g) = match i / 16 {
                0 => ((b & c) | (!b & d), i),
                1 => ((d & b) | (!d & c), (5 * i + 1) % 16),
                2 => (b ^ c ^ d, (3 * i + 5) % 16),
                _ => (c ^ (b | !d), (7 * i) % 16),
            };
            let f = f.wrapping_add(a).wrapping_add(k[i]).wrapping_add(m[g]);
            a = d;
            d = c;
            c = b;
            b = b.wrapping_add(f.rotate_left(S[i]));
        }
        h = [h[0].wrapping_add(a), h[1].wrapping_add(b), h[2].wrapping_add(c), h[3].wrapping_add(d)];
    }
    let mut out = [0u8; 16];
    for (i, w) in h.iter().enumerate() { out[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes()); }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(b: &[u8]) -> String { b.iter().map(|x| format!("{x:02x}")).collect() }

    #[test]
    fn md5_matches_rfc_1321() {
        assert_eq!(hex(&md5(b"")), "d41d8cd98f00b204e9800998ecf8427e");
        assert_eq!(hex(&md5(b"abc")), "900150983cd24fb0d6963f7d28e17f72");
        assert_eq!(hex(&md5(b"12345678901234567890123456789012345678901234567890123456789012345678901234567890")),
            "57edf4a22be3c955ac49da2e2107b67a");
    }

    /// RFC 5769 2.4 (long-term credential; the SASLprep'd username and password are plain here).
    #[test]
    fn integrity_matches_rfc_5769_long_term_vector() {
        let key = md5("\u{30DE}\u{30C8}\u{30EA}\u{30C3}\u{30AF}\u{30B9}:example.org:TheMatrIX".as_bytes());
        let id = [0x78, 0xad, 0x34, 0x33, 0xc6, 0xad, 0x72, 0xc0, 0x29, 0xda, 0x41, 0x2e];
        let mut m = Message::new(0x0001, id);
        m.attr(USERNAME, "\u{30DE}\u{30C8}\u{30EA}\u{30C3}\u{30AF}\u{30B9}".as_bytes());
        m.attr(NONCE, b"f//499k954d6OL34oL9FSTvy64sA");
        m.attr(REALM, b"example.org");
        m.integrity(&key);
        let tag = &m.bytes[m.bytes.len() - 20..];
        assert_eq!(hex(tag), "f67024656dd64a3e02b8e0712e85c9a28ca89666");
        let parsed = Parsed::new(&m.bytes).unwrap();
        assert!(parsed.verify(&m.bytes, &key));
        assert!(!parsed.verify(&m.bytes, &md5(b"other")));
    }

    #[test]
    fn xor_addresses_round_trip() {
        let id = [7u8; 12];
        for a in ["192.0.2.1:32853", "[2001:db8::1]:3478"] {
            let a: SocketAddr = a.parse().unwrap();
            assert_eq!(unxor_address(&xor_address(a, &id), &id), Some(a));
        }
        // RFC 5769 2.2's XOR-MAPPED-ADDRESS: 192.0.2.1:32853.
        assert_eq!(unxor_address(&[0, 1, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43], &id), Some("192.0.2.1:32853".parse().unwrap()));
    }

    /// A stand-in TURN server answering one client's requests.
    struct Server { key: Vec<u8>, nonce: &'static str, stale_bind: bool }
    impl Server {
        /// Answers every request in `bytes`: 401 without credentials, 438 once on a ChannelBind
        /// when `stale_bind` (a new nonce), else success; then ChannelData from the peer if given.
        fn answer(&mut self, bytes: &[u8], peer_data: Option<&[u8]>) -> Vec<u8> {
            let mut out = vec![];
            let mut at = 0;
            while at < bytes.len() {
                if bytes[at] >> 6 == 1 {
                    let n = u16::from_be_bytes([bytes[at + 2], bytes[at + 3]]) as usize;
                    at += 4 + ((n + 3) & !3);
                    continue;
                }
                let n = 20 + u16::from_be_bytes([bytes[at + 2], bytes[at + 3]]) as usize;
                let frame = &bytes[at..at + n];
                at += n;
                let m = Parsed::new(frame).unwrap();
                let method = m.kind & 0x3EEF;
                let signed = m.get(MESSAGE_INTEGRITY).is_some();
                let stale = signed && self.stale_bind && method == CHANNEL_BIND;
                if !signed || stale {
                    let code: u16 = if stale { 438 } else { 401 };
                    if stale { self.stale_bind = false; self.nonce = "fresh"; }
                    let mut r = Message::new(method | ERROR, m.id);
                    r.attr(ERROR_CODE, &[0, 0, (code / 100) as u8, (code % 100) as u8]);
                    r.attr(REALM, b"example.org");
                    r.attr(NONCE, self.nonce.as_bytes());
                    out.extend_from_slice(&r.bytes);
                    continue;
                }
                assert!(m.verify(frame, &self.key));
                assert_eq!(m.get(NONCE).unwrap(), self.nonce.as_bytes());
                let mut r = Message::new(method | SUCCESS, m.id);
                if method == ALLOCATE {
                    r.attr(XOR_RELAYED_ADDRESS, &xor_address("198.51.100.7:50000".parse().unwrap(), &m.id));
                    r.attr(LIFETIME, &600u32.to_be_bytes());
                }
                r.integrity(&self.key);
                out.extend_from_slice(&r.bytes);
            }
            if let Some(data) = peer_data { channel_data(&mut out, 0x4000, data); }
            out
        }
    }

    #[test]
    fn allocates_binds_a_channel_and_relays_both_ways() {
        let now = Instant::now();
        let mut server = Server { key: md5(b"user:example.org:pass").to_vec(), nonce: "n1", stale_bind: true };
        let mut c = Client::new("user", "pass", now);
        let reply = server.answer(&c.output(), None); // 401 with realm and nonce
        c.receive(&reply, now);
        let reply = server.answer(&c.output(), None);
        // Split across reads: framing must not care.
        c.receive(&reply[..7], now);
        c.receive(&reply[7..], now);
        assert_eq!(c.poll_event(), Some(Event::Allocated("198.51.100.7:50000".parse().unwrap())));
        let peer: SocketAddr = "203.0.113.9:4000".parse().unwrap();
        c.send_to(peer, b"hello", now);
        let bind = c.output();
        assert_eq!(u16::from_be_bytes([bind[0], bind[1]]), CHANNEL_BIND); // the datagram waits
        let reply = server.answer(&bind, None); // 438: a stale nonce
        c.receive(&reply, now);
        let reply = server.answer(&c.output(), Some(b"world")); // the retry with the new nonce
        c.receive(&reply, now);
        let out = c.output();
        assert_eq!(&out[..4], &[0x40, 0x00, 0, 5]);
        assert_eq!(&out[4..9], b"hello");
        assert_eq!(out.len(), 8 + 4); // padded to 4
        assert_eq!(c.poll_event(), Some(Event::Data(peer, b"world".to_vec())));
        assert!(c.failed().is_none());
        // Now bound: datagrams go straight out.
        c.send_to(peer, b"again", now);
        assert_eq!(&c.output()[..4], &[0x40, 0x00, 0, 5]);
        // Data from a channel that was never bound is dropped.
        let mut stray = vec![];
        channel_data(&mut stray, 0x4001, b"x");
        c.receive(&stray, now);
        assert_eq!(c.poll_event(), None);
    }

    #[test]
    fn refreshes_before_the_lifetime_ends_and_fails_on_silence() {
        let now = Instant::now();
        let mut server = Server { key: md5(b"u:example.org:p").to_vec(), nonce: "n", stale_bind: false };
        let mut c = Client::new("u", "p", now);
        let r = server.answer(&c.output(), None);
        c.receive(&r, now);
        let r = server.answer(&c.output(), None);
        c.receive(&r, now);
        assert!(c.relayed.is_some());
        c.tick(now + Duration::from_secs(500));
        assert!(c.output().is_empty());
        c.tick(now + Duration::from_secs(541));
        let refresh = c.output();
        assert_eq!(u16::from_be_bytes([refresh[0], refresh[1]]), REFRESH);
        // Unanswered: the allocation fails after the transaction timeout.
        c.tick(now + Duration::from_secs(541) + TRANSACTION_TIMEOUT + Duration::from_secs(1));
        assert!(c.failed().is_some());
    }

    #[test]
    fn channel_binds_are_renewed_before_their_permission_ends() {
        let now = Instant::now();
        let mut server = Server { key: md5(b"u:example.org:p").to_vec(), nonce: "n", stale_bind: false };
        let mut c = Client::new("u", "p", now);
        let r = server.answer(&c.output(), None);
        c.receive(&r, now);
        let r = server.answer(&c.output(), None);
        c.receive(&r, now);
        let peer: SocketAddr = "203.0.113.9:4000".parse().unwrap();
        c.send_to(peer, b"hello", now);
        let r = server.answer(&c.output(), None);
        c.receive(&r, now);
        assert_eq!(&c.output()[..4], &[0x40, 0x00, 0, 5]);
        // The permission ChannelBind installs lasts 300 s: renewed well before, and again after
        // each renewal, so a long race never loses it.
        let mut bound = now;
        for _ in 0..2 { // the allocation's own Refresh comes at 540 s
            c.tick(bound + CHANNEL_REFRESH - Duration::from_secs(1));
            assert!(c.output().is_empty());
            let at = bound + CHANNEL_REFRESH;
            assert!(at - bound <= Duration::from_secs(300) - TRANSACTION_TIMEOUT - Duration::from_secs(30));
            c.tick(at);
            let bind = c.output();
            assert_eq!(u16::from_be_bytes([bind[0], bind[1]]), CHANNEL_BIND);
            // Datagrams keep flowing on the channel while the renewal is out.
            c.send_to(peer, b"x", at);
            assert_eq!(&c.output()[..2], &[0x40, 0x00]);
            let r = server.answer(&bind, None);
            c.receive(&r, at);
            bound = at;
        }
        assert!(c.failed().is_none());
    }

    #[test]
    fn a_forged_success_without_integrity_fails() {
        let now = Instant::now();
        let mut server = Server { key: md5(b"u:example.org:p").to_vec(), nonce: "n", stale_bind: false };
        let mut c = Client::new("u", "p", now);
        let r = server.answer(&c.output(), None);
        c.receive(&r, now);
        let req = c.output();
        let id: [u8; 12] = req[8..20].try_into().unwrap();
        let mut forged = Message::new(ALLOCATE | SUCCESS, id);
        forged.attr(XOR_RELAYED_ADDRESS, &xor_address("192.0.2.66:1".parse().unwrap(), &id));
        c.receive(&forged.bytes, now);
        assert!(c.relayed.is_none());
        assert!(c.failed().is_some());
        // Garbage on the stream fails too.
        let mut c = Client::new("u", "p", now);
        c.receive(&[0xff, 0, 0, 0], now);
        assert!(c.failed().is_some());
    }
}
