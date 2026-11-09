//! A P2P room's race transport (finding 186): the star of peer connections around the room's hub
//! and its bridge to the room's SFU for members on the fallback. Plain data in and out, no
//! sockets: the native send thread (port/render/net/src/lib.rs, its peers in peer.rs) and the
//! browser page (port/web/www/p2p.js, through browser.rs) run the same code. The room
//! (online/server/src/room.ts) runs the race's rules, picks the hub and decides each member's link
//! (its `p2p` routes); this only says which peer connections to hold and where each race frame goes.
//!
//! - The hub holds a peer connection to every member not on the SFU, each member one to the hub.
//! - A race frame (type, slot, payload) carries its sender's slot. The hub stamps a star member's
//!   frame with that member's slot (so no game speaks for another's rider), hands it to its own game
//!   if it races, and forwards it to the other racers on the star; with racers on the SFU it also
//!   publishes it there, where those racers receive the star's frames from the hub's publication.
//!   The hub forwards those racers' SFU frames into the star.
//! - On the SFU a frame from the hub may carry any racer's slot; anyone else's must carry its own.
//!
//! On a peer connection a message is 1, the race's marker (its startAt, u64 little-endian: frames of
//! another race are dropped), then the frame; or 2 alone, a keepalive.
use serde_json::{json, Value};
use std::collections::HashMap;

#[cfg(target_arch = "wasm32")]
mod browser;

/// What the star asks of its transport.
#[derive(Debug, PartialEq)]
pub enum Action {
    /// Make a peer connection to this member; the hub's side offers.
    Connect { member: u64, offerer: bool },
    /// Drop the peer connection to this member.
    Close(u64),
    /// A message on a peer connection: (member, reliable, bytes).
    Peer(u64, bool, Vec<u8>),
    /// A race frame (type, slot, payload) for this game's own SFU publication.
    Sfu(Vec<u8>),
    /// A race frame for this game.
    Game(Vec<u8>),
    /// A message for the room (a link report).
    Room(Value),
}

/// The race frame types (port/net/net.h NET_FRAME_STATE, NET_FRAME_EVENT).
const STATE: u8 = 5;
const EVENT: u8 = 6;
/// The largest race frame (port/net/net.h NET_FRAME_MAX) and a state bundle's (one datagram,
/// port/hle/state_bundle.h; a bundle has a version, index, count and its f64 stamp: 13 bytes).
const FRAME_MAX: usize = 3072;
const STATE_MAX: usize = 992;
const STATE_MIN: usize = 13;
/// The race's slots (port/net/net.h NET_MAX_SLOTS).
const SLOTS: usize = 10;
/// Peer messages: a race frame, a keepalive.
const KIND_FRAME: u8 = 1;
const KIND_ALIVE: u8 = 2;
/// A linked peer gets a keepalive this often (a byte a second), and a link that heard nothing for
/// LINK_SILENT_MS is down: in a race a frozen rider is found in seconds, not after ICE's own
/// timeout (about 27 s, peer.rs ICE_LOST_GRACE).
const ALIVE_MS: u64 = 1000;
const LINK_SILENT_MS: u64 = 5000;

#[derive(Clone, Copy, Debug, PartialEq)]
enum Link {
    Star,
    Sfu,
}

struct Race {
    marker: u64,
    /// slot -> member
    slots: Vec<u64>,
    left: Vec<u8>,
}

struct PeerState {
    up: bool,
    heard: u64,
    sent: u64,
}

pub struct Star {
    me: Option<u64>,
    hub: Option<u64>,
    /// Each member's link to the hub as the room says; a member absent here is being linked.
    members: Vec<(u64, Option<Link>)>,
    race: Option<Race>,
    peers: HashMap<u64, PeerState>,
    /// DHD_NET_P2P_UDP=0 (a test switch): this game makes no direct links and says so at once, so it
    /// races over the SFU.
    direct: bool,
    /// The links reported down without a try (no direct links here), once per hub.
    reported_down: Vec<u64>,
    out: Vec<Action>,
}

impl Star {
    pub fn new(direct: bool) -> Self {
        Self { me: None, hub: None, members: vec![], race: None, peers: HashMap::new(), direct, reported_down: vec![], out: vec![] }
    }

    pub fn take(&mut self) -> Vec<Action> {
        std::mem::take(&mut self.out)
    }

    /// This game is the hub.
    pub fn is_hub(&self) -> bool {
        self.me.is_some() && self.me == self.hub
    }

    /// A message from the room: the welcome (this game's member id) and the routes (`p2p`: the hub,
    /// every member's link, the running race). Any other message is not the star's.
    pub fn room(&mut self, m: &Value) {
        match m["type"].as_str() {
            Some("welcome") => self.me = m["memberId"].as_u64(),
            Some("p2p") => self.routes(m),
            _ => {}
        }
    }

    fn routes(&mut self, m: &Value) {
        let hub = m["hub"].as_u64();
        if hub != self.hub {
            // A new hub: every link starts over (the room says each member's is being made).
            for id in self.peers.drain().map(|(id, _)| id).collect::<Vec<_>>() {
                self.out.push(Action::Close(id));
            }
            self.reported_down.clear();
        }
        self.hub = hub;
        self.members = m["members"].as_array().map_or(vec![], |a| {
            a.iter().filter_map(|x| {
                let link = match x["link"].as_str() {
                    Some("star") => Some(Link::Star),
                    Some("sfu") => Some(Link::Sfu),
                    _ => None,
                };
                Some((x["id"].as_u64()?, link))
            }).collect()
        });
        let race = &m["race"];
        self.race = race["startAt"].as_u64().map(|marker| Race {
            marker,
            slots: race["slots"].as_array().map_or(vec![], |a| a.iter().filter_map(Value::as_u64).take(SLOTS).collect()),
            left: race["left"].as_array().map_or(vec![], |a| a.iter().filter_map(Value::as_u64).map(|s| s as u8).collect()),
        });
        self.reconcile();
    }

    fn link_of(&self, id: u64) -> Option<Link> {
        self.members.iter().find(|(m, _)| *m == id).and_then(|(_, l)| *l)
    }

    /// The peer connections this game should hold: the hub one to every member not on the SFU, a
    /// member one to the hub unless it is on the SFU. Those no longer wanted go.
    fn reconcile(&mut self) {
        let (Some(me), Some(hub)) = (self.me, self.hub) else { return };
        let wanted: Vec<u64> = if me == hub {
            self.members.iter().filter(|(id, link)| *id != me && *link != Some(Link::Sfu)).map(|(id, _)| *id).collect()
        } else if self.members.iter().any(|(id, _)| *id == hub) && self.link_of(me) != Some(Link::Sfu) {
            vec![hub]
        } else {
            vec![]
        };
        let gone: Vec<u64> = self.peers.keys().copied().filter(|id| !wanted.contains(id)).collect();
        for id in gone {
            self.peers.remove(&id);
            self.out.push(Action::Close(id));
        }
        for id in wanted {
            if self.peers.contains_key(&id) {
                continue;
            }
            if !self.direct {
                // No direct links here: the room puts this game (or, on the hub, that member) on the
                // SFU at once instead of after its wait.
                if !self.reported_down.contains(&id) && self.link_of(if me == hub { id } else { me }).is_none() {
                    self.reported_down.push(id);
                    self.report(id, false);
                }
                continue;
            }
            self.peers.insert(id, PeerState { up: false, heard: 0, sent: 0 });
            self.out.push(Action::Connect { member: id, offerer: me == hub });
        }
    }

    /// The link report for the room: the link between the hub and `other`'s end of it.
    fn report(&mut self, other: u64, up: bool) {
        let (Some(me), Some(hub)) = (self.me, self.hub) else { return };
        let member = if me == hub { other } else { me };
        self.out.push(Action::Room(json!({"type": "p2p_link", "hub": hub, "member": member, "up": up})));
    }

    /// The transport's news of a peer connection: up, or failed (it never came up, or its path
    /// stopped answering). A failed one is gone; the room hears both.
    pub fn link(&mut self, member: u64, up: bool, now: u64) {
        let Some(peer) = self.peers.get_mut(&member) else { return };
        if up {
            if peer.up {
                return;
            }
            peer.up = true;
            peer.heard = now;
            peer.sent = now;
        } else {
            self.peers.remove(&member);
        }
        self.report(member, up);
    }

    /// Whether the link to `member` is up (the transport polls only those it holds).
    pub fn up(&self, member: u64) -> bool {
        self.peers.get(&member).is_some_and(|p| p.up)
    }

    /// Any link is up: race frames have somewhere to go while the room connection is down.
    pub fn any_up(&self) -> bool {
        self.peers.values().any(|p| p.up)
    }

    /// The slot of a member still in the race.
    fn active_slot(&self, member: u64) -> Option<u8> {
        let race = self.race.as_ref()?;
        let slot = race.slots.iter().position(|m| *m == member)? as u8;
        (!race.left.contains(&slot)).then_some(slot)
    }

    fn slot_active(&self, slot: u8) -> bool {
        self.race.as_ref().is_some_and(|r| (slot as usize) < r.slots.len() && !r.left.contains(&slot))
    }

    /// Racers on the SFU (the hub never is).
    fn fallback_racers(&self) -> Vec<u64> {
        let Some(race) = self.race.as_ref() else { return vec![] };
        race.slots.iter().enumerate()
            .filter(|(slot, id)| !race.left.contains(&(*slot as u8)) && Some(**id) != self.hub && self.link_of(**id) == Some(Link::Sfu))
            .map(|(_, id)| *id).collect()
    }

    /// The publishers whose SFU channels this game needs open for the race (its sfu_ready waits for
    /// them): the hub, the fallback racers; a fallback racer, the hub and the other fallback racers;
    /// a star member, none.
    pub fn sfu_expect(&self) -> Vec<u64> {
        let (Some(me), Some(hub)) = (self.me, self.hub) else { return vec![] };
        let fallback = self.fallback_racers();
        if me == hub {
            fallback
        } else if fallback.contains(&me) {
            std::iter::once(hub).chain(fallback.into_iter().filter(|id| *id != me)).collect()
        } else {
            vec![]
        }
    }

    /// This game takes part in the SFU's side of the race: the hub bridging fallback racers, or a
    /// fallback racer itself. The SFU association's per-race state is for those.
    pub fn on_sfu(&self) -> bool {
        let Some(me) = self.me else { return false };
        let fallback = self.fallback_racers();
        if self.is_hub() { !fallback.is_empty() } else { fallback.contains(&me) }
    }

    /// The running race's start for this game's SFU association when the game itself doesn't race
    /// (a hub that bridges a race it sits out): the marker and the slots, no own slot.
    pub fn race_start(&self) -> Option<Value> {
        let race = self.race.as_ref()?;
        let me = self.me?;
        if race.slots.contains(&me) {
            return None;
        }
        let players: Vec<Value> = race.slots.iter().enumerate().map(|(slot, id)| json!({"slot": slot, "memberId": id})).collect();
        Some(json!({"type": "start", "startAt": race.marker, "players": players}))
    }

    fn envelope(&self, frame: &[u8]) -> Option<Vec<u8>> {
        let marker = self.race.as_ref()?.marker;
        let mut bytes = Vec::with_capacity(9 + frame.len());
        bytes.push(KIND_FRAME);
        bytes.extend(marker.to_le_bytes());
        bytes.extend(frame);
        Some(bytes)
    }

    /// Sends a stamped frame to every racer on the star but `except`.
    fn to_star(&mut self, frame: &[u8], except: u64) {
        let Some(bytes) = self.envelope(frame) else { return };
        let reliable = frame[0] == EVENT;
        let targets: Vec<u64> = self.peers.iter().filter(|(id, p)| p.up && **id != except && self.active_slot(**id).is_some())
            .map(|(id, _)| *id).collect();
        for id in targets {
            self.out.push(Action::Peer(id, reliable, bytes.clone()));
        }
    }

    fn valid(frame: &[u8]) -> bool {
        if frame.len() < 2 || frame.len() > FRAME_MAX || (frame[1] as usize) >= SLOTS {
            return false;
        }
        match frame[0] {
            EVENT => true,
            STATE => (STATE_MIN..=STATE_MAX).contains(&frame.len()),
            _ => false,
        }
    }

    /// A race frame from this game: stamped with its slot, then to the hub, or as the hub to the
    /// star (and the SFU when it bridges), or on the SFU for a fallback racer. Dropped while this
    /// game has no way into the race (its link to a new hub still being made).
    pub fn game_frame(&mut self, frame: &[u8]) {
        let (Some(me), Some(hub)) = (self.me, self.hub) else { return };
        let Some(slot) = self.active_slot(me) else { return };
        let mut frame = frame.to_vec();
        if frame.len() < 2 {
            return;
        }
        frame[1] = slot;
        if !Self::valid(&frame) {
            return;
        }
        if me == hub {
            self.to_star(&frame, me);
            if self.on_sfu() {
                self.out.push(Action::Sfu(frame));
            }
        } else if self.up(hub) {
            if let Some(bytes) = self.envelope(&frame) {
                self.out.push(Action::Peer(hub, frame[0] == EVENT, bytes));
            }
        } else if self.link_of(me) == Some(Link::Sfu) {
            self.out.push(Action::Sfu(frame));
        }
    }

    /// A message from a peer connection.
    pub fn peer_message(&mut self, from: u64, reliable: bool, bytes: &[u8], now: u64) {
        let Some(peer) = self.peers.get_mut(&from) else { return };
        peer.heard = now;
        if bytes.first() != Some(&KIND_FRAME) || bytes.len() < 11 {
            return;
        }
        let marker = u64::from_le_bytes(bytes[1..9].try_into().unwrap());
        if self.race.as_ref().map(|r| r.marker) != Some(marker) {
            return;
        }
        let mut frame = bytes[9..].to_vec();
        // The channel says what it carries: a state on the unreliable one, an event on the other.
        if frame[0] != if reliable { EVENT } else { STATE } {
            return;
        }
        let (Some(me), Some(hub)) = (self.me, self.hub) else { return };
        if me == hub {
            let Some(slot) = self.active_slot(from) else { return };
            frame[1] = slot;
            if !Self::valid(&frame) {
                return;
            }
            if self.active_slot(me).is_some() {
                self.out.push(Action::Game(frame.clone()));
            }
            self.to_star(&frame, from);
            if self.on_sfu() {
                self.out.push(Action::Sfu(frame));
            }
        } else if from == hub && Self::valid(&frame) && self.slot_active(frame[1]) && self.active_slot(me).is_some_and(|s| s != frame[1]) {
            self.out.push(Action::Game(frame));
        }
    }

    /// A race frame the SFU delivered from publisher `from` (its own channel; the frame as sent). The
    /// hub's may carry any racer's slot (it bridges the star); anyone else's must carry its own. The
    /// hub forwards a fallback racer's frame into the star.
    pub fn sfu_frame(&mut self, from: u64, frame: Vec<u8>) {
        let (Some(me), Some(hub)) = (self.me, self.hub) else { return };
        if !Self::valid(&frame) || !self.slot_active(frame[1]) {
            return;
        }
        let mine = self.active_slot(me);
        if from == hub {
            if me != hub && mine.is_some_and(|s| s != frame[1]) {
                self.out.push(Action::Game(frame));
            }
            return;
        }
        if self.active_slot(from) != Some(frame[1]) {
            return;
        }
        if mine.is_some_and(|s| s != frame[1]) {
            self.out.push(Action::Game(frame.clone()));
        }
        if me == hub {
            self.to_star(&frame, from);
        }
    }

    /// Keepalives on quiet links, and a link silent for LINK_SILENT_MS is down (its peer goes).
    pub fn tick(&mut self, now: u64) {
        let mut silent = vec![];
        for (id, p) in self.peers.iter_mut().filter(|(_, p)| p.up) {
            if now.saturating_sub(p.heard) > LINK_SILENT_MS {
                silent.push(*id);
            } else if now.saturating_sub(p.sent) >= ALIVE_MS {
                p.sent = now;
                self.out.push(Action::Peer(*id, true, vec![KIND_ALIVE]));
            }
        }
        for id in silent {
            self.out.push(Action::Close(id));
            self.link(id, false, now);
        }
    }
}

#[cfg(test)]
mod tests;
