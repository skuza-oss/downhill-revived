//! The game's interface (port/render/include/dhd_settings.h): port/hle/host.c and the Android start
//! call these. Strings are UTF-8 and NUL-terminated; the environment stays the caller's (its get
//! and set functions), so the C runtime's copy of it is the one the game reads. The bindings'
//! (controls.rs, finding 157) are at the end.

use crate::controls::{self, Controls, Edit, Outcome};
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

/// The install id (install.rs, finding 161) of dhd.ini at `path`, made and saved there if it has
/// none (without the file: one for this run only), into `out` (`size` bytes, at least 33). 1 when
/// `out` has it.
///
/// # Safety
/// `path` is a NUL-terminated string; `out` has room for `size` bytes.
#[no_mangle]
pub unsafe extern "C" fn dhd_settings_install_id(path: *const c_char, out: *mut c_char, size: usize) -> c_int {
    // SAFETY: the caller's contract.
    let Some(path) = (unsafe { text(path) }) else {
        return 0;
    };
    let (id, _) = crate::install::get(Path::new(path));
    if out.is_null() || size <= id.len() {
        return 0;
    }
    // SAFETY: out has size > id.len() bytes.
    unsafe { put(out, size, &id) };
    1
}

/// `text` into `out` (`size` bytes), cut to fit, NUL-terminated.
///
/// # Safety
/// `out` is NULL or has room for `size` bytes.
unsafe fn put(out: *mut c_char, size: usize, text: &str) {
    if out.is_null() || size == 0 {
        return;
    }
    let mut n = text.len().min(size - 1);
    while !text.is_char_boundary(n) {
        n -= 1;
    }
    // SAFETY: out has size bytes; n + 1 <= size.
    unsafe {
        std::ptr::copy_nonoverlapping(text.as_ptr(), out.cast::<u8>(), n);
        *out.add(n) = 0;
    }
}

/// # Safety
/// Each of `values` is NULL or a NUL-terminated string.
unsafe fn control_values<'a>(values: [*const c_char; 3]) -> [&'a str; 3] {
    // SAFETY: the caller's contract.
    values.map(|p| unsafe { text(p) }.unwrap_or(""))
}

/// The three layouts (the keyboard, Controller 1, Controller 2: controls.rs) the variables' values
/// give (NULL or "": the defaults) into `out` (three, 52 bytes each); each problem with a line
/// through `problem` (may be NULL).
///
/// # Safety
/// The values are NULL or NUL-terminated strings; `out` has room for three layouts.
#[no_mangle]
pub unsafe extern "C" fn dhd_controls_layouts(
    keyboard: *const c_char,
    pad1: *const c_char,
    pad2: *const c_char,
    out: *mut controls::Layout,
    problem: Option<DhdSettingsUnknown>,
) {
    let mut problems = Vec::new();
    // SAFETY: the caller's contract.
    let c = Controls::from_values(unsafe { control_values([keyboard, pad1, pad2]) }, &mut problems);
    if !out.is_null() {
        // SAFETY: out has room for three.
        unsafe { std::ptr::copy_nonoverlapping(c.layouts.as_ptr(), out, 3) };
    }
    if let Some(report) = problem {
        for p in problems {
            if let Ok(line) = CString::new(p) {
                // SAFETY: NUL-terminated, alive for the call.
                unsafe { report(line.as_ptr()) };
            }
        }
    }
}

/// One change to `device`'s layout (0 the keyboard, 1 and 2 the controllers), with the three
/// variables' values as they are now: `edit` 0 assign (`input` to the target's `slot`), 1 clear, 2
/// reset, 3 swap sticks (`input` 1 on), 4 invert (`target` the INVERT_* bit, `input` 1 on); `kind`
/// the controller's PAD_KIND_*, for the message's words. The device's new line goes to `value`
/// and what the page says to `message`. -1 refused (`message` says why), 0 nothing to do, 1
/// changed.
///
/// # Safety
/// The values are NULL or NUL-terminated strings; `value` and `message` are NULL or have room for
/// their sizes.
#[no_mangle]
pub unsafe extern "C" fn dhd_controls_edit(
    keyboard: *const c_char,
    pad1: *const c_char,
    pad2: *const c_char,
    device: c_int,
    edit: c_int,
    target: c_int,
    slot: c_int,
    input: c_int,
    kind: c_int,
    value: *mut c_char,
    value_size: usize,
    message: *mut c_char,
    message_size: usize,
) -> c_int {
    // SAFETY: the caller's contract.
    let mut c = Controls::from_values(unsafe { control_values([keyboard, pad1, pad2]) }, &mut Vec::new());
    let (Ok(device), Ok(target), Ok(slot)) = (usize::try_from(device), usize::try_from(target), usize::try_from(slot)) else {
        return 0;
    };
    let input = u8::try_from(input).unwrap_or(0);
    let edit = match edit {
        0 => Edit::Assign { target, slot, input },
        1 => Edit::Clear { target, slot },
        2 => Edit::Reset,
        3 => Edit::SwapSticks(input != 0),
        4 => Edit::Invert(u8::try_from(target).unwrap_or(0), input != 0),
        _ => return 0,
    };
    if device >= controls::DEVICES.len() {
        return 0;
    }
    let (done, said) = match c.edit(device, edit, kind) {
        Outcome::Changed(m) => (1, m),
        Outcome::Unchanged => (0, String::new()),
        Outcome::Refused(m) => (-1, m),
    };
    // SAFETY: the caller's contract.
    unsafe {
        put(value, value_size, &c.values()[device]);
        put(message, message_size, &said);
    }
    done
}
