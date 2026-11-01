//! The game's interface (port/render/include/dhd_settings.h): port/hle/host.c and the Android start
//! call these. Strings are UTF-8 and NUL-terminated; the environment stays the caller's (its get
//! and set functions), so the C runtime's copy of it is the one the game reads.

use std::ffi::{c_char, c_int, CStr, CString};
use std::path::Path;

/// The value of an environment variable, or NULL.
pub type DhdSettingsGet = unsafe extern "C" fn(name: *const c_char) -> *const c_char;
/// Sets an environment variable.
pub type DhdSettingsSet = unsafe extern "C" fn(name: *const c_char, value: *const c_char);
/// Told each dhd.ini key that isn't a setting.
pub type DhdSettingsUnknown = unsafe extern "C" fn(key: *const c_char);

/// # Safety
/// `p` is NULL or a NUL-terminated string that lives for 'a.
unsafe fn text<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    // SAFETY: the caller's contract.
    unsafe { CStr::from_ptr(p) }.to_str().ok()
}

/// dhd.ini at `path` into the environment (`environment`): each variable through `set`, unless
/// `get` finds it set already. `unknown` (may be NULL) hears of the lines that aren't settings.
/// 1 when the file was there.
///
/// # Safety
/// `path` is a NUL-terminated string; `get` and `set` are callable as declared.
#[no_mangle]
pub unsafe extern "C" fn dhd_settings_load(
    path: *const c_char,
    get: DhdSettingsGet,
    set: DhdSettingsSet,
    unknown: Option<DhdSettingsUnknown>,
) -> c_int {
    // SAFETY: the caller's contract.
    let ini = unsafe { text(path) }.and_then(|p| crate::read(Path::new(p)));
    let is_set = |name: &str| match CString::new(name) {
        // SAFETY: get takes a NUL-terminated name.
        Ok(n) => !unsafe { get(n.as_ptr()) }.is_null(),
        Err(_) => true,
    };
    let env = crate::environment(ini.as_ref(), is_set);
    for (name, value) in &env.vars {
        if let (Ok(n), Ok(v)) = (CString::new(name.as_str()), CString::new(value.as_str())) {
            // SAFETY: both NUL-terminated, alive for the call.
            unsafe { set(n.as_ptr(), v.as_ptr()) };
        }
    }
    if let Some(report) = unknown {
        for key in &env.unknown {
            if let Ok(k) = CString::new(key.as_str()) {
                // SAFETY: NUL-terminated, alive for the call.
                unsafe { report(k.as_ptr()) };
            }
        }
    }
    ini.is_some() as c_int
}

/// Saves `key = value` into dhd.ini at `path` (`save`: the other lines kept). 1 when saved.
///
/// # Safety
/// The three are NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn dhd_settings_save(path: *const c_char, key: *const c_char, value: *const c_char) -> c_int {
    // SAFETY: the caller's contract.
    let (Some(path), Some(key), Some(value)) = (unsafe { text(path) }, unsafe { text(key) }, unsafe { text(value) }) else {
        return 0;
    };
    matches!(crate::save(Path::new(path), key, value), Ok(true)) as c_int
}

/// The value dhd.ini at `path` gives `key`, as written, into `out` (`size` bytes, NUL-terminated,
/// cut if longer). 1 when it has one.
///
/// # Safety
/// `path` and `key` are NUL-terminated strings; `out` has room for `size` bytes.
#[no_mangle]
pub unsafe extern "C" fn dhd_settings_read(path: *const c_char, key: *const c_char, out: *mut c_char, size: usize) -> c_int {
    // SAFETY: the caller's contract.
    let (Some(path), Some(key)) = (unsafe { text(path) }, unsafe { text(key) }) else {
        return 0;
    };
    let Some(value) = crate::read(Path::new(path)).and_then(|ini| ini.written(key)) else {
        return 0;
    };
    if !out.is_null() && size > 0 {
        let bytes = value.as_bytes();
        let n = bytes.len().min(size - 1);
        // SAFETY: out has size bytes; n + 1 <= size.
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), out.cast::<u8>(), n);
            *out.add(n) = 0;
        }
    }
    1
}

/// The keys dhd.ini at `path` holds whose key starts with `prefix` (without case), joined by
/// commas, into `out` (`size` bytes, NUL-terminated, cut if longer). 1 when it found one: the
/// control profiles (`bindings_<name>`) the game lists (finding 141).
///
/// # Safety
/// `path` and `prefix` are NUL-terminated strings; `out` has room for `size` bytes.
#[no_mangle]
pub unsafe extern "C" fn dhd_settings_keys(path: *const c_char, prefix: *const c_char, out: *mut c_char, size: usize) -> c_int {
    // SAFETY: the caller's contract.
    let (Some(path), Some(prefix)) = (unsafe { text(path) }, unsafe { text(prefix) }) else {
        return 0;
    };
    let Some(keys) = crate::read(Path::new(path)).map(|ini| ini.keys_with_prefix(prefix)) else {
        return 0;
    };
    if keys.is_empty() {
        return 0;
    }
    let joined = keys.join(",");
    if !out.is_null() && size > 0 {
        let bytes = joined.as_bytes();
        let n = bytes.len().min(size - 1);
        // SAFETY: out has size bytes; n + 1 <= size.
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), out.cast::<u8>(), n);
            *out.add(n) = 0;
        }
    }
    1
}
