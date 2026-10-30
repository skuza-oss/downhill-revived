//! The game's interface (port/render/include/dhd_presence.h): port/hle/host.c calls these, from
//! `host_service_presence`. Strings are UTF-8 and NUL-terminated.

use crate::{enabled, start, Kind, State, Worker};
use std::ffi::{c_char, c_int, CStr, CString};
use std::sync::Mutex;

/// Writes a line to the game's log.
pub type DhdPresenceLog = unsafe extern "C" fn(line: *const c_char);

static WORKER: Mutex<Option<Worker>> = Mutex::new(None);

/// # Safety
/// `p` is NULL or a NUL-terminated string that lives for 'a.
unsafe fn text<'a>(p: *const c_char) -> &'a str {
    if p.is_null() {
        return "";
    }
    // SAFETY: the caller's contract.
    unsafe { CStr::from_ptr(p) }.to_str().unwrap_or("")
}

/// Starts the presence when `setting` (DHD_DISCORD's value, NULL when unset) has it on; `log` (may
/// be NULL) gets its few log lines. 1 when it runs. Android has no Discord to reach: 0 there.
///
/// # Safety
/// `setting` is NULL or a NUL-terminated string; `log` is callable from any thread as declared.
#[no_mangle]
pub unsafe extern "C" fn dhd_presence_start(setting: *const c_char, log: Option<DhdPresenceLog>) -> c_int {
    // SAFETY: the caller's contract.
    let setting = (!setting.is_null()).then(|| unsafe { text(setting) });
    if cfg!(target_os = "android") || !enabled(setting) {
        return 0;
    }
    let mut worker = WORKER.lock().unwrap_or_else(|e| e.into_inner());
    if worker.is_none() {
        *worker = start(Box::new(move |line: &str| {
            if let (Some(log), Ok(line)) = (log, CString::new(line)) {
                // SAFETY: NUL-terminated, alive for the call; log is callable from any thread.
                unsafe { log(line.as_ptr()) };
            }
        }));
    }
    worker.is_some() as c_int
}

/// The player's state (port/host/host_api.h's HostPresence): `kind` a PRESENCE_* value.
///
/// # Safety
/// `track` and `room` are NULL or NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn dhd_presence_set(
    kind: c_int,
    track: *const c_char,
    place: c_int,
    riders: c_int,
    rider: c_int,
    room: *const c_char,
    members: c_int,
    max_members: c_int,
) {
    let count = |n: c_int| n.max(0) as u32;
    let s = State {
        kind: Kind::from_c(kind),
        // SAFETY: the caller's contract.
        track: unsafe { text(track) }.to_string(),
        place: count(place),
        riders: count(riders),
        rider: count(rider),
        // SAFETY: the caller's contract.
        room: unsafe { text(room) }.to_string(),
        members: count(members),
        max_members: count(max_members),
    };
    if let Some(w) = WORKER.lock().unwrap_or_else(|e| e.into_inner()).as_ref() {
        w.set(s);
    }
}

/// A friend's join from Discord (the Social SDK, with ASK_TO_JOIN): its room code, NUL-terminated,
/// into `out` (`size` bytes, 8 or more), taken. 1 when there was one; 0 otherwise.
///
/// # Safety
/// `out` is NULL or `size` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn dhd_presence_take_join(out: *mut c_char, size: c_int) -> c_int {
    if out.is_null() || (size as usize) < crate::CODE_LENGTH + 1 {
        return 0;
    }
    let Some(code) = crate::take_join() else { return 0 };
    // SAFETY: the caller's contract; the code is CODE_LENGTH ASCII letters (join_code).
    unsafe {
        std::ptr::copy_nonoverlapping(code.as_ptr() as *const c_char, out, code.len());
        *out.add(code.len()) = 0;
    }
    1
}

/// Clears the presence and closes the connection: at exit, at most half a second.
#[no_mangle]
pub extern "C" fn dhd_presence_stop() {
    if let Some(w) = WORKER.lock().unwrap_or_else(|e| e.into_inner()).take() {
        w.stop();
    }
}
