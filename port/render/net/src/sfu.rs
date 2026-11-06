//! One SFU association per room, owned exclusively by the WebSocket send thread.
use serde_json::{json, Value};
use std::collections::{HashMap, VecDeque};
use std::net::{IpAddr, SocketAddr, UdpSocket};
use std::time::Instant;
use str0m::change::SdpOffer;
use str0m::channel::{ChannelConfig, ChannelId, Reliability};
use str0m::net::{Protocol, Receive};
use str0m::{Candidate, Event, Input, Output, Rtc};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
struct Channel {
    id: ChannelId,
    member: u64,
    state: bool,
    local: bool,
}
pub struct Sfu {
    rtc: Rtc,
    socket: UdpSocket,
    channels: Vec<Channel>,
    slots: HashMap<u64, u8>,
    members: usize,
    connected: bool,
    drop_percent: u64,
    sent: u64,
    pending_events: VecDeque<Vec<u8>>,
    pending_state: HashMap<u8, Vec<u8>>,
}
impl Sfu {
    pub fn offer(sdp: &str) -> Result<(Self, Value)> {
        // The DO supplies SFU SDP only. No STUN discovery service, peer signaling or TURN mesh.
        let remote = sdp
            .lines()
            .filter(|l| l.starts_with("a=candidate:"))
            .find_map(|l| {
                let p: Vec<_> = l.split_whitespace().collect();
                if p.len() < 6 || !p[2].eq_ignore_ascii_case("udp") {
                    return None;
                }
                let ip: IpAddr = p[4].parse().ok()?;
                if !ip.is_ipv4() {
                    return None;
                }
                Some(SocketAddr::new(ip, p[5].parse().ok()?))
            })
            .ok_or("No SFU UDP candidate")?;
        let route = UdpSocket::bind("0.0.0.0:0")?;
        route.connect(remote)?;
        let socket = UdpSocket::bind(SocketAddr::new(route.local_addr()?.ip(), 0))?;
        socket.set_nonblocking(true)?;
        let mut rtc = Rtc::builder()
            .set_crypto_provider(str0m::crypto::from_feature_flags().into())
            .build(Instant::now());
        rtc.add_local_candidate(Candidate::host(socket.local_addr()?, "udp")?);
        // This socket uses IPv4. Do not give ICE candidates it cannot transmit to.
        let filtered = sdp
            .lines()
            .filter(|line| {
                if !line.starts_with("a=candidate:") {
                    return true;
                }
                let p: Vec<_> = line.split_whitespace().collect();
                p.len() >= 6
                    && p[2].eq_ignore_ascii_case("udp")
                    && p[4].parse::<std::net::Ipv4Addr>().is_ok()
            })
            .collect::<Vec<_>>()
            .join("\r\n")
            + "\r\n";
        let offer = SdpOffer::from_sdp_string(&filtered)?;
        let answer = rtc.sdp_api().accept_offer(offer)?;
        let answer = json!({ "type": "sfu_answer", "sdp": answer.to_sdp_string() });
        let drop_percent = std::env::var("DHD_NET_DROP_STATE")
            .ok()
            .and_then(|v| v.parse().ok())
            .unwrap_or(0)
            .min(100);
        Ok((
            Self {
                rtc,
                socket,
                channels: vec![],
                slots: HashMap::new(),
                members: 1,
                connected: false,
                drop_percent,
                sent: 0,
                pending_events: VecDeque::new(),
                pending_state: HashMap::new(),
            },
            answer,
        ))
    }
    pub fn signal(&mut self, message: &Value) -> Result<()> {
        if message["type"] == "room" {
            self.members = message["members"].as_array().map_or(1, |v| v.len());
        } else if message["type"] == "start" {
            self.slots.clear();
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
        Ok(())
    }
    pub fn usable(&mut self) -> bool {
        if self.channels.len() < self.members * 2 {
            return false;
        }
        self.channels
            .iter()
            .all(|c| self.rtc.channel(c.id).is_some())
    }
    pub fn send(&mut self, bytes: &[u8]) -> Result<()> {
        if bytes.len() < 2 || bytes.len() > 3072 {
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
        if state {
            // One newest snapshot per rider; never accumulate stale snapshots under congestion.
            if bytes.len() >= 4 {
                self.pending_state.insert(bytes[3], bytes.to_vec());
            }
        } else {
            if self.pending_events.len() >= 128 {
                return Err("Event send queue full".into());
            }
            self.pending_events.push_back(bytes.to_vec());
        }
        Ok(())
    }
    pub fn poll(&mut self) -> Result<(Vec<Value>, Vec<Vec<u8>>)> {
        let mut text = vec![];
        let mut frames = vec![];
        for state in [false, true] {
            if let Some(c) = self.channels.iter().find(|c| c.local && c.state == state) {
                if let Some(mut channel) = self.rtc.channel(c.id) {
                    if state {
                        let mut sent = vec![];
                        for (rider, bytes) in &self.pending_state {
                            if channel.write(true, bytes)? {
                                sent.push(*rider);
                            } else {
                                break;
                            }
                        }
                        for rider in sent {
                            self.pending_state.remove(&rider);
                        }
                    } else {
                        while let Some(bytes) = self.pending_events.front() {
                            if !channel.write(true, bytes)? {
                                break;
                            }
                            self.pending_events.pop_front();
                        }
                    }
                }
            }
        }
        let mut buffer = [0u8; 65536];
        for _ in 0..256 {
            match self.socket.recv_from(&mut buffer) {
                Ok((n, source)) => {
                    if let Ok(contents) = buffer[..n].try_into() {
                        let input = Input::Receive(
                            Instant::now(),
                            Receive {
                                proto: Protocol::Udp,
                                source,
                                destination: self.socket.local_addr()?,
                                contents,
                            },
                        );
                        if self.rtc.accepts(&input) {
                            self.rtc.handle_input(input)?;
                            self.drain(&mut text, &mut frames)?;
                        }
                    }
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(e) => return Err(e.into()),
            }
        }
        self.rtc.handle_input(Input::Timeout(Instant::now()))?;
        self.drain(&mut text, &mut frames)?;
        if !self.rtc.is_alive() {
            return Err("SFU disconnected".into());
        }
        Ok((text, frames))
    }

    fn drain(&mut self, text: &mut Vec<Value>, frames: &mut Vec<Vec<u8>>) -> Result<()> {
        for _ in 0..512 {
            match self.rtc.poll_output()? {
                Output::Timeout(_) => break,
                Output::Transmit(t) => {
                    if let Err(e) = self.socket.send_to(&t.contents, t.destination) {
                        if e.kind() != std::io::ErrorKind::WouldBlock {
                            return Err(e.into());
                        }
                    }
                }
                Output::Event(Event::Connected) if !self.connected => {
                    self.connected = true;
                    text.push(json!({"type":"sfu_connected"}));
                }
                Output::Event(Event::ChannelData(data)) => {
                    if let Some(c) = self.channels.iter().find(|c| c.id == data.id && !c.local) {
                        if let Some(slot) = self.slots.get(&c.member) {
                            let mut bytes = data.data;
                            if data.binary
                                && bytes.len() >= 2
                                && bytes.len() <= 3072
                                && bytes[0] == if c.state { 5 } else { 6 }
                            {
                                bytes[1] = *slot;
                                frames.push(bytes);
                            }
                        }
                    }
                }
                _ => {}
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;
    use str0m::change::SdpAnswer;

    #[test]
    fn native_channels_deliver_and_bind_sender_with_state_loss() {
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        socket.set_nonblocking(true).unwrap();
        let mut server = Rtc::builder()
            .set_crypto_provider(str0m::crypto::from_feature_flags().into())
            .build(Instant::now());
        server.add_local_candidate(Candidate::host(socket.local_addr().unwrap(), "udp").unwrap());
        let mut change = server.sdp_api();
        change.add_channel("server-events".into());
        let (offer, pending) = change.apply().unwrap();
        let (mut client, answer) = Sfu::offer(&offer.to_sdp_string()).unwrap();
        server
            .sdp_api()
            .accept_answer(
                pending,
                SdpAnswer::from_sdp_string(answer["sdp"].as_str().unwrap()).unwrap(),
            )
            .unwrap();
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
            .signal(&json!({"type":"start","players":[{"memberId":17,"slot":3}]}))
            .unwrap();
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
                    replied = c.write(true, &[6, 99, 2]).unwrap();
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
