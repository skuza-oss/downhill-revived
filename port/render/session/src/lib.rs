//! Browser facade over the exact coordinator used by the native networking crate.
#[path = "../../net/src/session.rs"]
mod session;
use serde_json::{json, Value};
use session::{Action, Session};
use wasm_bindgen::prelude::*;

#[wasm_bindgen]
pub struct P2pSession {
    inner: Session,
}
#[wasm_bindgen]
impl P2pSession {
    #[wasm_bindgen(constructor)]
    pub fn new(welcome: &str, now: f64) -> Result<P2pSession, JsValue> {
        let w: Value =
            serde_json::from_str(welcome).map_err(|_| JsValue::from_str("Invalid welcome"))?;
        Ok(Self {
            inner: Session::new(&w, now.max(0.0) as u64),
        })
    }
    pub fn members(&mut self, members: &str, now: f64) {
        if let Ok(m) = serde_json::from_str::<Value>(members) {
            self.inner.members(&m, now as u64);
        }
    }
    pub fn link(&mut self, id: u32, up: bool, now: f64) {
        self.inner.link(id as u64, up, now as u64);
    }
    pub fn local(&mut self, message: &str, now: f64) {
        if let Ok(m) = serde_json::from_str(message) {
            self.inner.local(m, now as u64);
        }
    }
    pub fn receive(&mut self, id: u32, reliable: bool, bytes: &[u8], now: f64) {
        self.inner
            .receive(id as u64, reliable, bytes.to_vec(), now as u64);
    }
    pub fn frame(&mut self, bytes: &[u8]) {
        self.inner.frame(bytes);
    }
    pub fn tick(&mut self, now: f64) {
        self.inner.tick(now as u64);
    }
    pub fn drain(&mut self) -> String {
        let actions: Vec<Value> = self
            .inner
            .take()
            .into_iter()
            .map(|a| match a {
                Action::Client(message) => json!({"kind":"client","message":message}),
                Action::Frame(bytes) => json!({"kind":"frame","bytes":bytes}),
                Action::Peer(to, reliable, bytes) => {
                    json!({"kind":"peer","to":to,"reliable":reliable,"bytes":bytes})
                }
                Action::Service(message) => json!({"kind":"service","message":message}),
            })
            .collect();
        serde_json::to_string(&actions).unwrap_or_else(|_| "[]".into())
    }
}
