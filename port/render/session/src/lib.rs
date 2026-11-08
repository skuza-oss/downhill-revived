//! A P2P room's coordinator (finding 180), for the native games (dhd_net's p2p.rs) and the browser.
pub mod coordinator;
pub use coordinator::{Action, Session};

#[cfg(target_arch = "wasm32")]
mod browser;
