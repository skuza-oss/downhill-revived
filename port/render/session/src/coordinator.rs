//! A P2P room's coordinator (finding 180): the room rules an SFU room's server runs
//! (online/server/src/room.ts: ready, countdown, start, finish window, results, the Then rule),
//! run by the host's game instead, and the race-frame routing of a host star. Plain data in and
//! out, no sockets: the native send thread (port/render/net/src/p2p.rs) and the browser page
//! (port/web/www/p2p.js, through browser.rs) drive the same code. The game sees the SFU room's messages
//! (room, start, left, ended, error), so its online code is one for both kinds of room.
//!
//! On a peer connection a room message is byte 0 then JSON; a race frame is byte 1, the race's
//! start time (u64, little-endian: frames of an older race are dropped), then the game's frame.
//! The host stamps each frame with the sender's slot for the connection it came on, so no guest
//! can speak for another rider.
use serde_json::{json, Value};

#[derive(Debug)]
pub enum Action {
    /// A room message for this game (as an SFU room's server would send it).
    Game(Value),
    /// A race frame for this game.
    Frame(Vec<u8>),
    /// A message for one peer: (member, reliable, bytes).
    Peer(u64, bool, Vec<u8>),
    /// A message for the room server (online/server/src/p2p.ts).
    Server(Value),
}

/// The most members a room holds (online/server/src/protocol.ts MAX_MEMBERS).
const MAX_MEMBERS: u64 = 10;
/// The SFU room's timings (online/server/src/protocol.ts): the countdown once a grid locks, the
/// lead from the start message to the start, the AUTO rule's wait between races.
const COUNTDOWN_MS: u64 = 5000;
const START_LEAD_MS: u64 = 1500;
const NEXT_RACE_MS: u64 = 30000;
/// A room message on a peer connection is at most this long.
const COMMAND_MAX: usize = 16384;
/// A race frame with its header (marker 8, type 1, slot 1, frame up to 3070).
const FRAME_MAX: usize = 3080;
/// A state bundle fits one datagram (port/hle/state_bundle.h).
const STATE_MAX: usize = 1000;

pub struct Session {
    pub me: u64,
    pub host: u64,
    pub room: Value,
    pub race: Option<Value>,
    connected: Vec<u64>,
    grid: Vec<u64>,
    grid_rules: Value,
    out: Vec<Action>,
    last_publish: u64,
    /// The capacity last told to the server (p2p_capacity): it refuses joins beyond it.
    told_capacity: u64,
}

impl Session {
    pub fn new(welcome: &Value, now: u64) -> Self {
        let me = welcome["memberId"].as_u64().unwrap_or(0);
        let host = welcome["hostId"].as_u64().unwrap_or(0);
        let mut s = Self {
            me,
            host,
            race: None,
            connected: vec![me],
            grid: vec![],
            grid_rules: Value::Null,
            out: vec![],
            last_publish: now,
            told_capacity: MAX_MEMBERS,
            // Never public: a P2P room is reached by its code only (finding 180).
            room: json!({"type":"room","code":welcome["code"],"hostId":host,"roomName":"P2P race",
                "public":false,"maxPlayers":MAX_MEMBERS,"started":false,"version":welcome["version"],
                "settings":{"track":0,"laps":0,"seed":null},"members":[],"phase":"open",
                "rules":{"finishWindow":60,"nextRace":"host","then":"same"},"rotation":[],
                "trackLimits":[],"pickOnly":[],"countdownEndsAt":null,"nextRaceAt":null,"lastRace":null}),
        };
        let mut w = welcome.clone();
        w["type"] = json!("welcome");
        w["host"] = json!(me == host);
        s.out.push(Action::Game(w));
        s.members(&welcome["members"], now);
        s
    }
    pub fn take(&mut self) -> Vec<Action> {
        std::mem::take(&mut self.out)
    }
    pub fn is_host(&self) -> bool {
        self.me == self.host
    }
    fn ids(&self) -> Vec<u64> {
        self.room["members"].as_array().map_or(vec![], |m| {
            m.iter().filter_map(|m| m["id"].as_u64()).collect()
        })
    }
    fn member_mut(&mut self, id: u64) -> Option<&mut Value> {
        self.room["members"]
            .as_array_mut()?
            .iter_mut()
            .find(|m| m["id"].as_u64() == Some(id))
    }
    fn send(&mut self, to: u64, message: Value) {
        if to == self.me {
            self.out.push(Action::Game(message));
        } else if self.connected.contains(&to) {
            let mut bytes = vec![0];
            bytes.extend(message.to_string().as_bytes());
            self.out.push(Action::Peer(to, true, bytes));
        }
    }
    fn broadcast(&mut self, message: Value) {
        for id in self.ids() {
            self.send(id, message.clone());
        }
    }
    pub fn members(&mut self, members: &Value, now: u64) {
        if !self.is_host() {
            return;
        }
        let Some(incoming) = members.as_array().filter(|a| a.len() as u64 <= MAX_MEMBERS) else {
            return;
        };
        let old = self.room["members"].as_array().cloned().unwrap_or_default();
        let mut next = vec![];
        for m in incoming {
            let Some(id) = m["id"].as_u64() else { continue };
            next.push(old.iter().find(|x| x["id"].as_u64() == Some(id)).cloned().unwrap_or_else(||
                json!({"id":id,"name":m["name"],"ready":false,"rider":0,"bike":0,"inRace":false,"choosing":false})));
        }
        for m in &old {
            let id = m["id"].as_u64().unwrap_or(u64::MAX);
            if !next.iter().any(|n| n["id"] == m["id"]) {
                self.retire(id, "disconnect");
                self.connected.retain(|x| *x != id);
            }
        }
        self.room["members"] = json!(next);
        self.publish(now);
    }
    pub fn link(&mut self, id: u64, up: bool, now: u64) {
        if up {
            if !self.connected.contains(&id) {
                self.connected.push(id);
            }
            if self.is_host() {
                self.publish(now);
            }
        } else {
            self.connected.retain(|x| *x != id);
            if id == self.host && !self.is_host() {
                self.out.push(Action::Game(
                    json!({"type":"error","code":"host_left","message":"The host left: the room has ended."}),
                ));
                self.out.push(Action::Server(json!({"type":"leave"})));
            } else if self.is_host() {
                // The host carries the race: a guest it can't reach is out of the room (its rider
                // is the host's AI for the rest of this race).
                self.retire(id, "disconnect");
                self.out.push(Action::Server(json!({"type":"p2p_kick","member":id,"reason":"unreachable"})));
                self.publish(now);
            }
        }
    }
    pub fn local(&mut self, m: Value, now: u64) {
        if self.is_host() {
            self.command(self.me, m, now);
        } else {
            let mut bytes = vec![0];
            bytes.extend(m.to_string().as_bytes());
            self.out.push(Action::Peer(self.host, true, bytes));
        }
    }
    pub fn receive(&mut self, from: u64, reliable: bool, bytes: Vec<u8>, now: u64) {
        if bytes.first() == Some(&0) {
            if !reliable || bytes.len() > COMMAND_MAX {
                return;
            }
            let Ok(m) = serde_json::from_slice::<Value>(&bytes[1..]) else {
                return;
            };
            if self.is_host() {
                self.command(from, m, now);
            } else if from == self.host {
                match m["type"].as_str() {
                    Some("room") => self.room = m.clone(),
                    Some("start") => self.race = Some(m.clone()),
                    Some("ended") => self.race = None,
                    Some("left") => {
                        if let Some(r) = self.race.as_mut() {
                            if let Some(p) = r["players"]
                                .as_array_mut()
                                .and_then(|p| p.iter_mut().find(|p| p["memberId"] == m["memberId"]))
                            {
                                p["left"] = json!(true);
                            }
                        }
                    }
                    Some("error") => {}
                    _ => return,
                }
                self.out.push(Action::Game(m));
            }
        } else if bytes.first() == Some(&1) {
            self.route_frame(from, reliable, bytes[1..].to_vec());
        }
    }
    fn active_slot(&self, id: u64) -> Option<u8> {
        self.race.as_ref()?["players"]
            .as_array()?
            .iter()
            .find(|p| p["memberId"].as_u64() == Some(id) && p["left"] != true)?["slot"]
            .as_u64()
            .map(|s| s as u8)
    }
    pub fn frame(&mut self, frame: &[u8]) {
        let Some(slot) = self.active_slot(self.me) else {
            return;
        };
        let Some(marker) = self.race.as_ref().and_then(|r| r["startAt"].as_u64()) else {
            return;
        };
        if frame.len() < 2 {
            return;
        }
        let reliable = frame[0] == 6;
        let mut bytes = vec![1];
        bytes.extend(marker.to_le_bytes());
        bytes.extend(frame);
        bytes[10] = slot;
        if self.is_host() {
            for id in self.ids().into_iter().filter(|id| *id != self.me) {
                if self.active_slot(id).is_some() {
                    self.out.push(Action::Peer(id, reliable, bytes.clone()));
                }
            }
        } else {
            self.out.push(Action::Peer(self.host, reliable, bytes));
        }
    }
    fn route_frame(&mut self, from: u64, reliable: bool, mut bytes: Vec<u8>) {
        let Some(race) = &self.race else { return };
        if bytes.len() < 10 || bytes.len() > FRAME_MAX || bytes[8] != if reliable { 6 } else { 5 } {
            return;
        }
        if u64::from_le_bytes(bytes[..8].try_into().unwrap())
            != race["startAt"].as_u64().unwrap_or(0)
        {
            return;
        }
        if !reliable
            && (bytes.len() > STATE_MAX || bytes.len() < 21 || bytes[10] != 6 || bytes[11] >= 10)
        {
            return;
        }
        if self.is_host() {
            let Some(slot) = self.active_slot(from) else {
                return;
            };
            bytes[9] = slot;
            for id in self
                .ids()
                .into_iter()
                .filter(|id| *id != self.me && *id != from)
            {
                if self.active_slot(id).is_some() {
                    let mut envelope = vec![1];
                    envelope.extend(&bytes);
                    self.out.push(Action::Peer(id, reliable, envelope));
                }
            }
        } else if from != self.host || bytes[9] >= 10 {
            return;
        }
        if self.active_slot(self.me).is_some() {
            self.out.push(Action::Frame(bytes[8..].to_vec()));
        }
    }
    fn error(&mut self, id: u64, message: &str) {
        self.send(
            id,
            json!({"type":"error","code":"bad_value","message":message}),
        );
    }
    fn command(&mut self, from: u64, m: Value, now: u64) {
        if !self.ids().contains(&from) || !self.connected.contains(&from) {
            return;
        }
        match m["type"].as_str() {
            Some("settings") if from == self.host => {
                let old_track = self.room["settings"]["track"].clone();
                let old_laps = self.room["settings"]["laps"].as_u64().unwrap_or(0);
                for key in ["track", "laps", "seed"] {
                    if let Some(n) = m[key]
                        .as_u64()
                        .filter(|n| *n <= if key == "seed" { u32::MAX as u64 } else { 255 })
                    {
                        self.room["settings"][key] = json!(n);
                    }
                }
                if let Some(n) = m["maxPlayers"]
                    .as_u64()
                    .filter(|n| (2..=MAX_MEMBERS).contains(n) && *n >= self.ids().len() as u64)
                {
                    self.room["maxPlayers"] = json!(n);
                }
                // "public" is ignored: a P2P room is never listed.
                if let Some(name) = m["roomName"].as_str().filter(|s| s.chars().count() <= 24) {
                    self.room["roomName"] = json!(name);
                }
                for key in ["rotation", "trackLimits", "pickOnly"] {
                    if let Some(a) = m[key].as_array().filter(|a| {
                        a.len() <= 64 && a.iter().all(|v| v.as_u64().is_some_and(|n| n <= 255))
                    }) {
                        self.room[key] = json!(a);
                    }
                }
                if let Some(n) = m["finishWindow"].as_u64().filter(|n| *n <= 600) {
                    self.room["rules"]["finishWindow"] = json!(n);
                }
                if ["auto", "host"].contains(&m["nextRace"].as_str().unwrap_or("")) {
                    self.room["rules"]["nextRace"] = m["nextRace"].clone();
                }
                if ["same", "next", "random"].contains(&m["then"].as_str().unwrap_or("")) {
                    self.room["rules"]["then"] = m["then"].clone();
                }
                if (old_laps ^ self.room["settings"]["laps"].as_u64().unwrap_or(0)) & 2 != 0 {
                    let active: Vec<_> = self
                        .ids()
                        .into_iter()
                        .filter(|id| self.active_slot(*id).is_some())
                        .collect();
                    for member in self.room["members"].as_array_mut().unwrap() {
                        if !active.contains(&member["id"].as_u64().unwrap()) {
                            member["ready"] = json!(false);
                        }
                    }
                }
                if old_track != self.room["settings"]["track"] {
                    if self.room["nextRaceAt"].as_u64().is_some() {
                        self.room["nextRaceAt"] =
                            json!(self.room["nextRaceAt"].as_u64().unwrap().max(now + 15000));
                    }
                    if self.race.is_none() && !self.grid.is_empty() {
                        self.lock_grid(now);
                    }
                }
            }
            Some("pick") => {
                let target = m["member"].as_u64().unwrap_or(from);
                if target != from && from != self.host {
                    return;
                }
                if let Some(member) = self.member_mut(target) {
                    for key in ["rider", "bike"] {
                        if let Some(n) = m[key].as_u64().filter(|n| *n <= 255) {
                            member[key] = json!(n);
                        }
                    }
                    if let Some(choosing) = m["choosing"].as_bool() {
                        member["choosing"] = json!(choosing);
                        if choosing {
                            member["ready"] = json!(false);
                        }
                    }
                }
            }
            Some("ready") => {
                if let Some(member) = self.member_mut(from) {
                    member["ready"] = json!(m["ready"] == true && member["choosing"] != true);
                }
                if self.race.is_none()
                    && self.grid.is_empty()
                    && m["ready"] == true
                    && self.room["rules"]["nextRace"] == "auto"
                {
                    if self.room["waitingForRider"] == true {
                        self.lock_grid(now);
                    } else if self.room["nextRaceAt"].is_null() {
                        self.room["nextRaceAt"] = json!(now + NEXT_RACE_MS);
                    }
                }
            }
            Some("start") if from == self.host => {
                if self.race.is_none() {
                    self.lock_grid(now);
                }
            }
            Some("end") if from == self.host => {
                self.close_race(now, true);
            }
            Some("kick") if from == self.host => {
                if let Some(id) = m["member"].as_u64().filter(|id| *id != self.host) {
                    self.out.push(Action::Server(json!({"type":"p2p_kick","member":id})));
                }
            }
            Some("back") => self.retire(from, "gave_up"),
            Some("finish") => {
                if let (Some(ticks), Some(place), Some(race)) = (
                    m["ticks"].as_u64().filter(|n| *n <= u32::MAX as u64),
                    m["place"].as_u64().filter(|n| (1..=10).contains(n)),
                    self.race.as_mut(),
                ) {
                    if let Some(result) = race["results"]
                        .as_array_mut()
                        .and_then(|a| a.iter_mut().find(|r| r["id"].as_u64() == Some(from)))
                    {
                        if result["ticks"].is_null() && result["dnfReason"].is_null() {
                            result["ticks"] = json!(ticks);
                            result["place"] = json!(place);
                            let window = race["rules"]["finishWindow"].as_u64().unwrap_or(60);
                            if window > 0 && race["finishEndsAt"].is_null() {
                                race["finishEndsAt"] = json!(now + window * 1000);
                            }
                        }
                    }
                }
            }
            _ => {
                self.error(from, "That command is not available.");
                return;
            }
        }
        self.update(now);
        self.publish(now);
    }
    fn limit(&self, track: &Value) -> usize {
        self.room["rotation"]
            .as_array()
            .and_then(|a| a.iter().position(|t| t == track))
            .and_then(|i| self.room["trackLimits"][i].as_u64())
            .unwrap_or(10) as usize
    }
    fn rotation(&self, count: usize) -> Vec<Value> {
        let tracks = self.room["rotation"]
            .as_array()
            .cloned()
            .unwrap_or_default();
        let at = tracks
            .iter()
            .position(|t| *t == self.room["settings"]["track"])
            .map_or(0, |i| (i + 1) % tracks.len());
        tracks[at..]
            .iter()
            .chain(&tracks[..at])
            .filter(|t| {
                self.limit(t) >= count
                    && !self.room["pickOnly"]
                        .as_array()
                        .is_some_and(|a| a.contains(t))
            })
            .cloned()
            .collect()
    }
    fn lock_grid(&mut self, now: u64) {
        if self.race.is_some() {
            return;
        }
        self.grid = self.room["members"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|m| {
                m["ready"] == true
                    && m["choosing"] != true
                    && self
                        .connected
                        .contains(&m["id"].as_u64().unwrap_or(u64::MAX))
            })
            .filter_map(|m| m["id"].as_u64())
            .collect();
        if self.grid.is_empty() {
            return;
        }
        if self.limit(&self.room["settings"]["track"]) < self.grid.len() {
            if let Some(track) = self.rotation(self.grid.len()).first() {
                self.room["settings"]["track"] = track.clone();
            } else {
                self.grid.clear();
                self.error(self.me, "This track has too few start places.");
                return;
            }
        }
        self.grid_rules = self.room["rules"].clone();
        self.room["countdownEndsAt"] = json!(now + COUNTDOWN_MS);
        self.room["nextRaceAt"] = Value::Null;
        self.room["waitingForRider"] = json!(false);
    }
    fn start(&mut self, now: u64) {
        let members = self.room["members"].as_array().unwrap();
        let mut entrants: Vec<_> = members
            .iter()
            .filter(|m| self.grid.contains(&m["id"].as_u64().unwrap()))
            .cloned()
            .collect();
        entrants.sort_by_key(|m| m["id"].as_u64() != Some(self.host));
        if entrants.is_empty() {
            self.room["countdownEndsAt"] = Value::Null;
            return;
        }
        let players: Vec<_> = entrants.iter().enumerate().map(|(slot, m)| json!({"slot":slot,"memberId":m["id"],"name":m["name"],"rider":m["rider"],"bike":m["bike"]})).collect();
        let results: Vec<_> = entrants
            .iter()
            .map(|m| json!({"id":m["id"],"name":m["name"],"rider":m["rider"],"place":0}))
            .collect();
        self.race = Some(
            json!({"type":"start","startAt":now + START_LEAD_MS,"serverTime":now,"track":self.room["settings"]["track"],"laps":self.room["settings"]["laps"],"seed":self.room["settings"]["seed"].as_u64().unwrap_or(now & 0xffff_ffff),"players":players,"results":results,"rules":self.grid_rules}),
        );
        self.room["countdownEndsAt"] = Value::Null;
        self.grid.clear();
        for (slot, member) in entrants.iter().enumerate() {
            let id = member["id"].as_u64().unwrap();
            let mut start = self.race.clone().unwrap();
            start["yourSlot"] = json!(slot);
            self.send(id, start);
        }
    }
    fn retire(&mut self, id: u64, reason: &str) {
        let Some(slot) = self.active_slot(id) else {
            return;
        };
        if let Some(race) = self.race.as_mut() {
            race["players"][slot as usize]["left"] = json!(true);
            if let Some(r) = race["results"]
                .as_array_mut()
                .unwrap()
                .iter_mut()
                .find(|r| r["id"].as_u64() == Some(id))
            {
                if r["ticks"].is_null() {
                    r["dnfReason"] = json!(reason);
                }
            }
        }
        if let Some(m) = self.member_mut(id) {
            m["ready"] = json!(false);
        }
        self.broadcast(json!({"type":"left","slot":slot,"memberId":id,"reason":reason}));
    }
    fn close_race(&mut self, now: u64, voided: bool) {
        let Some(race) = self.race.take() else { return };
        if !voided {
            let mut results = race["results"].as_array().cloned().unwrap_or_default();
            for result in &mut results {
                if result["ticks"].is_null() && result["dnfReason"].is_null() {
                    result["dnfReason"] = json!("timeout");
                }
            }
            results.sort_by_key(|r| r["ticks"].as_u64().unwrap_or(u64::MAX));
            let limit = self.limit(&race["track"]);
            let count = results.len();
            let mut place = 0;
            for (i, r) in results.iter_mut().enumerate() {
                place = (place + 1).max(
                    (r["place"].as_u64().unwrap_or(0) as usize)
                        .min(limit.saturating_sub(count - 1 - i)),
                );
                r["place"] = json!(place);
            }
            let id = self.room["lastRace"]["id"].as_u64().unwrap_or(0) + 1;
            self.room["lastRace"] = json!({"id":id,"track":race["track"],"entrants":results});
            if self.room["settings"]["track"] == race["track"] && race["rules"]["then"] != "same" {
                let rotation = self.rotation(self.ids().len());
                let pick = if race["rules"]["then"] == "random" && !rotation.is_empty() {
                    now as usize % rotation.len()
                } else {
                    0
                };
                if let Some(track) = rotation.get(pick) {
                    self.room["settings"]["track"] = track.clone();
                }
            }
        }
        for m in self.room["members"].as_array_mut().unwrap() {
            if race["players"]
                .as_array()
                .unwrap()
                .iter()
                .any(|p| p["memberId"] == m["id"] && p["left"] != true)
            {
                m["ready"] = json!(false);
            }
        }
        self.room["countdownEndsAt"] = Value::Null;
        self.room["nextRaceAt"] = if race["rules"]["nextRace"] == "auto" {
            json!(now + NEXT_RACE_MS)
        } else {
            Value::Null
        };
        self.broadcast(json!({"type":"ended","reason":if voided {"void"} else {"done"}}));
    }
    fn update(&mut self, now: u64) {
        if let Some(race) = &self.race {
            let expired = race["finishEndsAt"].as_u64().is_some_and(|t| now >= t);
            let done = race["results"]
                .as_array()
                .unwrap()
                .iter()
                .all(|r| !r["ticks"].is_null() || !r["dnfReason"].is_null());
            if expired || done {
                self.close_race(now, false);
            }
        } else if !self.grid.is_empty() {
            let members = self.room["members"].as_array().unwrap();
            self.grid.retain(|id| {
                members.iter().any(|m| {
                    m["id"].as_u64() == Some(*id) && m["ready"] == true && m["choosing"] != true
                })
            });
            if self.grid.is_empty() {
                self.room["countdownEndsAt"] = Value::Null;
            } else if self.room["countdownEndsAt"]
                .as_u64()
                .is_some_and(|t| now >= t)
            {
                self.start(now);
            }
        } else {
            let members = self.room["members"].as_array().unwrap();
            let all = members.len() >= 2
                && members.iter().all(|m| {
                    m["ready"] == true
                        && m["choosing"] != true
                        && self.connected.contains(&m["id"].as_u64().unwrap())
                });
            let timer = self.room["nextRaceAt"].as_u64().is_some_and(|t| now >= t);
            if all || timer {
                if timer {
                    self.room["nextRaceAt"] = Value::Null;
                }
                self.lock_grid(now);
                if timer && self.grid.is_empty() {
                    self.room["waitingForRider"] = json!(true);
                }
            }
        }
    }
    pub fn tick(&mut self, now: u64) {
        if self.is_host() {
            let before = (
                self.race.is_some(),
                self.grid.len(),
                self.room["nextRaceAt"].clone(),
            );
            self.update(now);
            if before
                != (
                    self.race.is_some(),
                    self.grid.len(),
                    self.room["nextRaceAt"].clone(),
                )
                || now >= self.last_publish + 1000
            {
                self.publish(now);
            }
        }
    }
    fn publish(&mut self, now: u64) {
        if !self.is_host() {
            return;
        }
        self.last_publish = now;
        let mut room = self.room.clone();
        room["serverTime"] = json!(now);
        room["started"] = json!(self.race.is_some());
        room["phase"] = json!(if self.race.is_some() {
            "racing"
        } else if !room["countdownEndsAt"].is_null() {
            "starting"
        } else if !room["nextRaceAt"].is_null() {
            "intermission"
        } else {
            "open"
        });
        room["raceStartedAt"] = self
            .race
            .as_ref()
            .map_or(Value::Null, |r| r["startAt"].clone());
        room["finishEndsAt"] = self
            .race
            .as_ref()
            .map_or(Value::Null, |r| r["finishEndsAt"].clone());
        room["rulesPending"] = self
            .race
            .as_ref()
            .is_some_and(|r| r["rules"] != room["rules"])
            .into();
        for member in room["members"].as_array_mut().unwrap() {
            let id = member["id"].as_u64().unwrap();
            member["inRace"] = json!(self.active_slot(id).is_some());
            if let Some(result) = self
                .race
                .as_ref()
                .and_then(|r| r["results"].as_array())
                .and_then(|a| a.iter().find(|r| r["id"].as_u64() == Some(id)))
            {
                member["raceState"] = json!(if !result["dnfReason"].is_null() {
                    "dnf"
                } else if !result["ticks"].is_null() {
                    "finished"
                } else {
                    "racing"
                });
                member["finishTicks"] = result["ticks"].clone();
                member["place"] = result["place"].clone();
                member["dnfReason"] = result["dnfReason"].clone();
            } else {
                member["raceState"] = Value::Null;
                member["finishTicks"] = Value::Null;
                member["place"] = Value::Null;
                member["dnfReason"] = Value::Null;
            }
        }
        let capacity = room["maxPlayers"].as_u64().unwrap_or(MAX_MEMBERS);
        if capacity != self.told_capacity {
            self.told_capacity = capacity;
            self.out.push(Action::Server(json!({"type":"p2p_capacity","maxPlayers":capacity})));
        }
        self.room = room.clone();
        self.broadcast(room);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn host() -> Session {
        Session::new(
            &json!({"memberId":1,"hostId":1,"code":"ABCDEF","members":[{"id":1,"name":"HOST"},{"id":2,"name":"GUEST"}]}),
            100,
        )
    }
    #[test]
    fn readiness_starts_race_and_guests_cannot_change_settings() {
        let mut s = host();
        s.link(2, true, 100);
        s.command(2, json!({"type":"settings","track":8}), 100);
        assert_eq!(s.room["settings"]["track"], 0);
        s.local(json!({"type":"ready","ready":true}), 100);
        s.command(2, json!({"type":"ready","ready":true}), 100);
        s.tick(5101);
        assert!(s.race.is_some());
        assert_eq!(s.active_slot(1), Some(0));
        assert_eq!(s.active_slot(2), Some(1));
    }
    #[test]
    fn guest_frame_identity_is_bound_to_connection_and_old_races_are_dropped() {
        let mut s = host();
        s.link(2, true, 100);
        s.local(json!({"type":"ready","ready":true}), 100);
        s.command(2, json!({"type":"ready","ready":true}), 100);
        s.tick(5101);
        s.take();
        let marker = s.race.as_ref().unwrap()["startAt"].as_u64().unwrap();
        let mut frame = vec![1];
        frame.extend(marker.to_le_bytes());
        frame.extend([6, 0, 1, 2]);
        s.receive(2, true, frame, 3200);
        assert!(s
            .take()
            .iter()
            .any(|a| matches!(a,Action::Frame(f) if f[1]==1)));
        s.receive(2, true, vec![0; 12], 3200);
        assert!(s.take().is_empty());
    }
    #[test]
    fn rooms_stay_private_and_the_server_hears_the_capacity_once() {
        let mut s = host();
        s.link(2, true, 100);
        s.take();
        s.local(json!({"type":"settings","public":true,"maxPlayers":4}), 200);
        assert_eq!(s.room["public"], false);
        assert_eq!(s.room["rules"]["nextRace"], "host");
        let capacity = |actions: &[Action]| actions.iter().filter(|a| matches!(a, Action::Server(m) if m["type"] == "p2p_capacity")).count();
        let first = s.take();
        assert_eq!(capacity(&first), 1);
        assert!(first.iter().any(|a| matches!(a, Action::Server(m) if m["maxPlayers"] == 4)));
        s.tick(1300); // the room is published again each second
        assert_eq!(capacity(&s.take()), 0);
    }
    #[test]
    fn an_unreachable_guest_is_removed_and_its_rider_retired() {
        let mut s = host();
        s.link(2, true, 100);
        s.local(json!({"type":"ready","ready":true}), 100);
        s.command(2, json!({"type":"ready","ready":true}), 100);
        s.tick(5101);
        s.take();
        s.link(2, false, 6000);
        let actions = s.take();
        assert!(actions.iter().any(|a| matches!(a, Action::Server(m) if m["type"] == "p2p_kick" && m["reason"] == "unreachable")));
        assert!(actions.iter().any(|a| matches!(a, Action::Game(m) if m["type"] == "left" && m["memberId"] == 2)));
        assert_eq!(s.active_slot(2), None);
    }
}
