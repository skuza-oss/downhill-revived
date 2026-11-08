//! One ordinary WebRTC peer connection. ICE, DTLS and SCTP are str0m's; the existing
//! TURN/TLS client is shared with the SFU. No SFU publications or subscriptions.
use crate::{relay, turn};
use serde_json::{json, Value};
use std::collections::{HashMap, VecDeque};
use std::io::ErrorKind;
use std::net::{SocketAddr, UdpSocket};
use std::sync::Arc;
use std::time::{Duration, Instant};
use str0m::change::{SdpAnswer, SdpOffer, SdpPendingOffer};
use str0m::channel::{ChannelConfig, ChannelId, Reliability};
use str0m::net::{Protocol, Receive};
use str0m::{Candidate, Event, Input, Output, Rtc};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
pub type Wake = Arc<dyn Fn() + Send + Sync>;
const EVENTS_MAX: usize = 256;
const MESSAGE_MAX: usize = 16384;

struct Relay {
    client: turn::Client,
    link: relay::Link,
    address: Option<SocketAddr>,
}

pub struct Peer {
    rtc: Rtc,
    socket: UdpSocket,
    local: SocketAddr,
    pending: Option<SdpPendingOffer>,
    state: ChannelId,
    events: ChannelId,
    relay: Option<Relay>,
    direct: bool,
    connected: bool,
    created: Instant,
    outgoing_events: VecDeque<Vec<u8>>,
    outgoing_states: HashMap<(u8, u8), Vec<u8>>,
    signals: Vec<Value>,
    incoming: Vec<(bool, Vec<u8>)>,
    stun: Option<(SocketAddr, [u8; 12], Instant, u8)>,
}

impl Peer {
    /// `local` is the interface selected by a route to the signaling/STUN service.
    pub fn new(local: std::net::IpAddr, offerer: bool, direct: bool) -> Result<Self> {
        let local = if local.is_unspecified() {
            let route = UdpSocket::bind(SocketAddr::new(local, 0))?;
            route.connect(("stun.cloudflare.com", 3478))?;
            route.local_addr()?.ip()
        } else {
            local
        };
        let socket = UdpSocket::bind(SocketAddr::new(local, 0))?;
        socket.set_nonblocking(true)?;
        let local = socket.local_addr()?;
        let mut rtc = Rtc::builder()
            .set_crypto_provider(str0m::crypto::from_feature_flags().into())
            .build(Instant::now());
        let host = Candidate::host(local, "udp")?;
        rtc.add_local_candidate(host);
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
        let (state, events, pending, signals) = if offerer {
            let mut change = rtc.sdp_api();
            let state = change.add_channel_with_config(state_config);
            let events = change.add_channel_with_config(event_config);
            let (offer, pending) = change.apply().ok_or("No peer offer")?;
            (
                state,
                events,
                Some(pending),
                vec![json!({"type":"offer","sdp":offer.to_sdp_string()})],
            )
        } else {
            // Confirm negotiated channels directly: an answerer has no local offer.
            let state = rtc.direct_api().create_data_channel(state_config);
            let events = rtc.direct_api().create_data_channel(event_config);
            (state, events, None, vec![])
        };
        Ok(Self {
            rtc,
            socket,
            local,
            pending,
            state,
            events,
            relay: None,
            direct,
            connected: false,
            created: Instant::now(),
            outgoing_events: VecDeque::new(),
            outgoing_states: HashMap::new(),
            signals,
            incoming: vec![],
            stun: None,
        })
    }

    pub fn discover(&mut self, server: SocketAddr) -> Result<()> {
        use ring::rand::SecureRandom;
        let mut id = [0; 12];
        ring::rand::SystemRandom::new()
            .fill(&mut id)
            .map_err(|_| "STUN random id failed")?;
        self.stun = Some((server, id, Instant::now(), 0));
        Ok(())
    }

    pub fn signal(&mut self, m: &Value) -> Result<()> {
        match m["type"].as_str() {
            Some("offer") => {
                let sdp = m["sdp"]
                    .as_str()
                    .filter(|s| s.len() <= 16384)
                    .ok_or("Invalid peer offer")?;
                let answer = self
                    .rtc
                    .sdp_api()
                    .accept_offer(SdpOffer::from_sdp_string(sdp)?)?;
                self.signals
                    .push(json!({"type":"answer","sdp":answer.to_sdp_string()}));
            }
            Some("answer") => {
                let sdp = m["sdp"]
                    .as_str()
                    .filter(|s| s.len() <= 16384)
                    .ok_or("Invalid peer answer")?;
                let pending = self.pending.take().ok_or("Unexpected peer answer")?;
                self.rtc
                    .sdp_api()
                    .accept_answer(pending, SdpAnswer::from_sdp_string(sdp)?)?;
            }
            Some("candidate") => {
                let s = m["candidate"]
                    .as_str()
                    .filter(|s| s.len() <= 2048)
                    .ok_or("Invalid ICE candidate")?;
                self.rtc
                    .add_remote_candidate(Candidate::from_sdp_string(s)?);
            }
            _ => return Err("Unknown peer signal".into()),
        }
        Ok(())
    }

    pub fn turn(&mut self, credentials: &Value, wake: Wake) -> Result<()> {
        if self.relay.is_some() {
            return Ok(());
        }
        let (host, port) = crate::sfu::turns_server(&credentials["urls"])
            .ok_or("No Cloudflare TURN/TLS endpoint")?;
        let field = |k: &str| {
            credentials[k]
                .as_str()
                .filter(|s| !s.is_empty() && s.len() <= 256)
        };
        let client = turn::Client::new(
            field("username").ok_or("TURN username missing")?,
            field("credential").ok_or("TURN credential missing")?,
            Instant::now(),
        );
        self.relay = Some(Relay {
            client,
            link: relay::Link::open(&host, port, wake),
            address: None,
        });
        Ok(())
    }

    pub fn connected(&self) -> bool {
        self.connected
    }
    pub fn timed_out(&self) -> bool {
        !self.connected && self.created.elapsed() > Duration::from_secs(20)
    }

    pub fn send(&mut self, reliable: bool, bytes: Vec<u8>) -> Result<()> {
        if bytes.is_empty() || bytes.len() > MESSAGE_MAX {
            return Err("Invalid peer message size".into());
        }
        if reliable {
            if self.outgoing_events.len() >= EVENTS_MAX {
                return Err("Peer event queue full".into());
            }
            self.outgoing_events.push_back(bytes);
        } else {
            // Race marker (8), type, original sender, bundle version, bundle index.
            if bytes.len() < 13 || bytes[10] >= 10 || bytes[12] >= 10 {
                return Err("Invalid peer state".into());
            }
            self.outgoing_states.insert((bytes[10], bytes[12]), bytes);
        }
        Ok(())
    }

    pub fn poll(&mut self) -> Result<(Vec<Value>, Vec<(bool, Vec<u8>)>)> {
        if self.direct && !self.local.ip().is_loopback() {
            if let Some((server, id, next, tries)) = &mut self.stun {
                if *tries < 5 && Instant::now() >= *next {
                    let mut request = vec![0, 1, 0, 0, 0x21, 0x12, 0xa4, 0x42];
                    request.extend(*id);
                    // A failed candidate path is normal during ICE gathering.
                    let _ = self.socket.send_to(&request, *server);
                    *tries += 1;
                    *next = Instant::now() + Duration::from_millis(500);
                }
            }
        }
        let mut buffer = [0u8; 65536];
        for _ in 0..256 {
            match self.socket.recv_from(&mut buffer) {
                Ok((n, source)) if self.direct => {
                    if !self.stun_response(source, &buffer[..n])? {
                        self.receive(source, self.local, &buffer[..n])?;
                    }
                }
                Ok(_) => {}
                Err(e)
                    if matches!(
                        e.kind(),
                        ErrorKind::WouldBlock | ErrorKind::Interrupted | ErrorKind::ConnectionReset
                    ) =>
                {
                    break
                }
                Err(e) => return Err(e.into()),
            }
        }
        let mut received = vec![];
        if let Some(r) = self.relay.as_mut() {
            if r.link.failed() || r.client.failed().is_some() {
                return Err(format!(
                    "Peer TURN/TLS allocation failed: {}",
                    r.client.failed().unwrap_or("TLS stream closed")
                )
                .into());
            }
            if r.link.ready() {
                let now = Instant::now();
                let (bytes, _) = r.link.take();
                r.client.receive(&bytes, now);
                r.client.tick(now);
                while let Some(event) = r.client.poll_event() {
                    match event {
                        turn::Event::Allocated(addr) => {
                            r.address = Some(addr);
                            let candidate =
                                Candidate::relayed(addr, r.link.local().unwrap_or(addr), "udp")?;
                            self.signals.push(
                                json!({"type":"candidate","candidate":candidate.to_sdp_string()}),
                            );
                            self.rtc.add_local_candidate(candidate);
                        }
                        turn::Event::Data(source, bytes) => {
                            if let Some(to) = r.address {
                                received.push((source, to, bytes));
                            }
                        }
                    }
                }
                r.link.write(&r.client.output());
            }
        }
        for (from, to, bytes) in received {
            self.receive(from, to, &bytes)?;
        }
        self.rtc.handle_input(Input::Timeout(Instant::now()))?;
        self.flush()?;
        self.drain()?;
        if let Some(r) = self.relay.as_mut().filter(|r| r.link.ready()) {
            r.link.write(&r.client.output());
        }
        if !self.rtc.is_alive() {
            return Err("Peer disconnected".into());
        }
        Ok((
            std::mem::take(&mut self.signals),
            std::mem::take(&mut self.incoming),
        ))
    }

    fn stun_response(&mut self, source: SocketAddr, bytes: &[u8]) -> Result<bool> {
        let Some((server, id, _, _)) = &self.stun else {
            return Ok(false);
        };
        if source != *server || bytes.len() < 20 || bytes[..2] != [1, 1] || bytes[8..20] != *id {
            return Ok(false);
        }
        let mut at = 20;
        while at + 4 <= bytes.len() {
            let kind = u16::from_be_bytes([bytes[at], bytes[at + 1]]);
            let len = u16::from_be_bytes([bytes[at + 2], bytes[at + 3]]) as usize;
            at += 4;
            if at + len > bytes.len() {
                break;
            }
            if kind == 0x20 && len == 8 && bytes[at + 1] == 1 {
                let port = u16::from_be_bytes([bytes[at + 2], bytes[at + 3]]) ^ 0x2112;
                let mut ip = [0u8; 4];
                for i in 0..4 {
                    ip[i] = bytes[at + 4 + i] ^ [0x21, 0x12, 0xa4, 0x42][i];
                }
                let candidate = Candidate::server_reflexive(
                    SocketAddr::new(ip.into(), port),
                    self.local,
                    "udp",
                )?;
                self.signals
                    .push(json!({"type":"candidate","candidate":candidate.to_sdp_string()}));
                self.rtc.add_local_candidate(candidate);
                self.stun = None;
                return Ok(true);
            }
            at += (len + 3) & !3;
        }
        Ok(true)
    }

    fn receive(&mut self, source: SocketAddr, destination: SocketAddr, bytes: &[u8]) -> Result<()> {
        if let Ok(contents) = bytes.try_into() {
            let input = Input::Receive(
                Instant::now(),
                Receive {
                    proto: Protocol::Udp,
                    source,
                    destination,
                    contents,
                },
            );
            if self.rtc.accepts(&input) {
                self.rtc.handle_input(input)?;
                self.drain()?;
            }
        }
        Ok(())
    }
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
            self.outgoing_states
                .retain(|_, bytes| !channel.write(true, bytes).unwrap_or(false));
        }
        Ok(())
    }
    fn drain(&mut self) -> Result<()> {
        for _ in 0..512 {
            match self.rtc.poll_output()? {
                Output::Timeout(_) => break,
                Output::Transmit(t) => {
                    if let Some(r) = self.relay.as_mut().filter(|r| r.address == Some(t.source)) {
                        // LAN candidates are tried directly. TURN providers reject permissions
                        // to private/local addresses; that rejection must not poison the allocation.
                        if public_peer(t.destination.ip()) {
                            r.client.send_to(t.destination, &t.contents, Instant::now());
                        }
                    } else if self.direct {
                        let _ = self.socket.send_to(&t.contents, t.destination);
                    }
                }
                Output::Event(Event::Connected) => self.connected = true,
                Output::Event(Event::ChannelData(data))
                    if data.binary && data.data.len() <= MESSAGE_MAX =>
                {
                    if data.id == self.state || data.id == self.events {
                        self.incoming.push((data.id == self.events, data.data));
                    }
                }
                _ => {}
            }
        }
        Ok(())
    }
}

fn public_peer(ip: std::net::IpAddr) -> bool {
    match ip {
        std::net::IpAddr::V4(ip) => {
            !ip.is_private()
                && !ip.is_loopback()
                && !ip.is_link_local()
                && !ip.is_unspecified()
                && !ip.is_multicast()
                && !ip.is_broadcast()
                && ip.octets()[0] != 0
        }
        std::net::IpAddr::V6(ip) => {
            !ip.is_loopback()
                && !ip.is_unspecified()
                && !ip.is_multicast()
                && !ip.is_unique_local()
                && !ip.is_unicast_link_local()
        }
    }
}

impl Drop for Peer {
    fn drop(&mut self) {
        if let Some(r) = self.relay.as_mut().filter(|r| r.link.ready()) {
            r.client.close(Instant::now());
            r.link.write(&r.client.output());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    #[ignore = "requires Cloudflare TURN credentials and network access"]
    fn cloudflare_turn_tls_without_direct_udp() {
        let (_, ca) = crate::sfu::probe::turn_credentials("p2p-qualification-a");
        let (_, cb) = crate::sfu::probe::turn_credentials("p2p-qualification-b");
        struct Revoke(Vec<String>);
        impl Drop for Revoke {
            fn drop(&mut self) {
                for user in &self.0 {
                    crate::sfu::probe::revoke_turn(user);
                }
            }
        }
        let _revoke = Revoke(vec![
            ca["username"].as_str().unwrap().into(),
            cb["username"].as_str().unwrap().into(),
        ]);
        let ip = "0.0.0.0".parse().unwrap();
        let mut a = Peer::new(ip, true, false).unwrap();
        let mut b = Peer::new(ip, false, false).unwrap();
        a.turn(&ca, Arc::new(|| {})).unwrap();
        b.turn(&cb, Arc::new(|| {})).unwrap();
        let end = Instant::now() + Duration::from_secs(30);
        let mut event = false;
        let mut state = false;
        while Instant::now() < end {
            let (signals, _) = a.poll().unwrap();
            for s in signals {
                b.signal(&s).unwrap();
            }
            let (signals, frames) = b.poll().unwrap();
            for s in signals {
                a.signal(&s).unwrap();
            }
            if a.connected() {
                a.send(true, b"turn-event".to_vec()).unwrap();
                let mut bytes = vec![0; 32];
                bytes[8] = 5;
                bytes[10] = 6;
                a.send(false, bytes).unwrap();
            }
            for (reliable, bytes) in frames {
                event |= reliable && bytes == b"turn-event";
                state |= !reliable && bytes.len() == 32 && bytes[8] == 5;
            }
            if event && state {
                eprintln!("P2P MATCH: reliable events and unreliable states over Cloudflare TURN/TLS; direct UDP disabled on both endpoints");
                return;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        panic!("TURN/TLS did not carry both message classes");
    }
    #[test]
    fn direct_peers_exchange_reliable_events_and_unreliable_states() {
        let ip = "127.0.0.1".parse().unwrap();
        let mut a = Peer::new(ip, true, true).unwrap();
        let mut b = Peer::new(ip, false, true).unwrap();
        let end = Instant::now() + Duration::from_secs(5);
        let mut got = false;
        while Instant::now() < end {
            let (signals, _) = a.poll().unwrap();
            for s in signals {
                b.signal(&s).unwrap();
            }
            let (signals, frames) = b.poll().unwrap();
            for s in signals {
                a.signal(&s).unwrap();
            }
            if b.connected() {
                b.send(true, b"reply".to_vec()).unwrap();
            }
            if a.connected() {
                a.send(true, b"hello".to_vec()).unwrap();
                let mut state = vec![0; 16];
                state[8] = 5;
                state[10] = 6;
                a.send(false, state).unwrap();
            }
            if frames
                .iter()
                .any(|(reliable, f)| *reliable && f == b"hello")
            {
                got = true;
            }
            if got && frames.iter().any(|(reliable, f)| !reliable && f[8] == 5) {
                return;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        panic!("peer data channels did not exchange both message classes");
    }
}
