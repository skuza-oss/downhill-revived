//! The install id (finding 161): a random id the game makes once and keeps in dhd.ini as
//! `install_id`, and sends in the online `hello` (port/net/client.c), so the server's operator can
//! ban one copy of the game rather than a name anyone can take. It names nothing about the player
//! or the PC.
//!
//! It is not a setting: no DHD_* variable comes from it at the start, the launcher's tabs don't
//! show it, and the `settings:` line of dhd.log (players post their logs) never has it: someone who
//! copied another player's id could get that player banned. A player who wants a new one deletes
//! the line.

/// The dhd.ini key.
pub const KEY: &str = "install_id";

/// An install id: 32 lowercase hex digits (128 bits), the form the server takes
/// (online/server/src/guard.ts installIdOf).
pub fn valid(id: &str) -> bool {
    id.len() == 32 && id.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

/// A new install id. The standard library has no public random source, so the bits come from
/// `RandomState`: SipHash with keys the operating system's random source gives each thread
/// (later states add one to them), over the time and the process id. Without its 128-bit key the
/// output can't be guessed, which is all an id that names nothing needs (no dependencies: this
/// crate takes the standard library only). Native only (the browser has no clock here, and no
/// online play yet).
#[cfg(not(target_arch = "wasm32"))]
pub fn fresh() -> String {
    use std::hash::{BuildHasher, Hasher, RandomState};
    let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_nanos());
    let mut id = String::with_capacity(32);
    for half in 0u8..2 {
        let mut h = RandomState::new().build_hasher();
        h.write_u8(half);
        h.write_u128(nanos);
        h.write_u32(std::process::id());
        id += &format!("{:016x}", h.finish());
    }
    id
}

/// The install id of dhd.ini at `path`: the one it keeps, else a new one, saved into it. Without
/// the file (the tools' runs, DHD_SETTINGS=none) a new one for this run only: the game creates no
/// dhd.ini for it (that would give the tools' runs the player's defaults). The bool: kept in the
/// file (it was there, or is now).
#[cfg(not(target_arch = "wasm32"))]
pub fn get(path: &std::path::Path) -> (String, bool) {
    let Some(ini) = crate::read(path) else {
        return (fresh(), false);
    };
    if let Some(id) = ini.written(KEY).filter(|id| valid(id)) {
        return (id, true);
    }
    let id = fresh();
    let saved = matches!(crate::save(path, KEY, &id), Ok(true));
    (id, saved)
}
