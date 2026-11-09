//! The star for the browser page (port/web/www/p2p.js): the same code as the native games run, its
//! actions out as JSON. The page owns WebRTC (RTCPeerConnection belongs to Window).
use crate::{Action, Star};
use serde_json::{json, Value};
use wasm_bindgen::prelude::*;

#[wasm_bindgen]
pub struct P2pStar {
    inner: Star,
}

#[wasm_bindgen]
impl P2pStar {
    #[wasm_bindgen(constructor)]
    pub fn new(direct: bool) -> P2pStar {
        Self { inner: Star::new(direct) }
    }
    /// A room message (JSON text): the welcome and the routes count, anything else is ignored.
    pub fn room(&mut self, message: &str) {
        if let Ok(m) = serde_json::from_str::<Value>(message) {
            self.inner.room(&m);
        }
    }
    pub fn link(&mut self, member: u32, up: bool, now: f64) {
        self.inner.link(member as u64, up, now as u64);
    }
    pub fn game_frame(&mut self, frame: &[u8]) {
        self.inner.game_frame(frame);
    }
    pub fn peer_message(&mut self, from: u32, reliable: bool, bytes: &[u8], now: f64) {
        self.inner.peer_message(from as u64, reliable, bytes, now as u64);
    }
    pub fn sfu_frame(&mut self, from: u32, frame: &[u8]) {
        self.inner.sfu_frame(from as u64, frame.to_vec());
    }
    pub fn tick(&mut self, now: f64) {
        self.inner.tick(now as u64);
    }
    /// The publishers whose SFU channels this game needs for the race (JSON array of member ids).
    pub fn sfu_expect(&self) -> String {
        json!(self.inner.sfu_expect()).to_string()
    }
    /// The actions since the last call, as a JSON array of {kind, ...}.
    pub fn drain(&mut self) -> String {
        let actions: Vec<Value> = self.inner.take().into_iter().map(|a| match a {
            Action::Connect { member, offerer } => json!({"kind": "connect", "member": member, "offerer": offerer}),
            Action::Close(member) => json!({"kind": "close", "member": member}),
            Action::Peer(to, reliable, bytes) => json!({"kind": "peer", "to": to, "reliable": reliable, "bytes": bytes}),
            Action::Sfu(frame) => json!({"kind": "sfu", "bytes": frame}),
            Action::Game(frame) => json!({"kind": "game", "bytes": frame}),
            Action::Room(message) => json!({"kind": "room", "message": message}),
        }).collect();
        Value::Array(actions).to_string()
    }
}
