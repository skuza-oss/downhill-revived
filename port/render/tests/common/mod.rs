//! What the tests share: the host log, a small JSON reader for `tests/data`, and
//! device options per adapter and compiler.
//!
//! Environment (set by `port/tools/rustgs.py test`):
//! - `DHDR_TEST_DXC`: the x64 dxcompiler.dll (`port/tools/dxc.py`). DXC tests fail without it;
//!   they never skip.
#![allow(dead_code)]

use dhd_render::abi::*;
use dhd_render::device::{DeviceOptions, Gpu};
use std::collections::BTreeMap;
use std::ffi::c_char;
use std::io::Write;

/// The C host's log line (`dhd_render.h`), here on stderr (libtest shows it for a failing test).
#[no_mangle]
pub unsafe extern "C" fn dhd_host_log(line: *const c_char, bytes: usize) {
    // SAFETY: the renderer passes a valid (pointer, length) pair of one of its strings.
    let text = unsafe { std::slice::from_raw_parts(line as *const u8, bytes) };
    let mut err = std::io::stderr().lock();
    let _ = err.write_all(text);
    let _ = err.write_all(b"\n");
}

// ---------------------------------------------------------------- devices

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Adapter {
    /// DX12 WARP (the fallback adapter).
    Warp,
    /// DX12, the high-performance adapter.
    Hardware,
    /// Vulkan, the high-performance adapter.
    Vulkan,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Compiler {
    Fxc,
    Dxc,
}

/// The x64 dxcompiler.dll from `DHDR_TEST_DXC`; a missing one fails the test.
pub fn dxc_path() -> String {
    let p = std::env::var("DHDR_TEST_DXC")
        .expect("DHDR_TEST_DXC is not set: run the tests through port/tools/rustgs.py test (it passes dxc.py's x64 dxcompiler.dll)");
    assert!(std::path::Path::new(&p).is_file(), "DHDR_TEST_DXC={p} is not a file");
    p
}

/// The options C would pass for this adapter and compiler (log level WARN).
pub fn options(adapter: Adapter, compiler: Compiler) -> DeviceOptions {
    let mut flags = 0;
    match adapter {
        Adapter::Warp => flags |= DHDR_DEV_FALLBACK,
        Adapter::Hardware => {}
        Adapter::Vulkan => flags |= DHDR_DEV_VULKAN,
    }
    let dxc_path = match compiler {
        Compiler::Fxc => {
            flags |= DHDR_DEV_FXC;
            None
        }
        Compiler::Dxc => Some(dxc_path()),
    };
    DeviceOptions { flags, vendor_id: 0, device_id: 0, log_level: DHDR_LOG_WARN, dxc_path, window: None }
}

/// A device of the test's own (not the process's global one), kept for the process.
pub fn gpu(adapter: Adapter, compiler: Compiler) -> &'static Gpu {
    let o = options(adapter, compiler);
    match Gpu::create(&o) {
        Ok(g) => Box::leak(Box::new(g)),
        Err(e) => panic!("device {adapter:?} {compiler:?}: {e}"),
    }
}

/// Runs `f` inside a Validation and an OutOfMemory error scope and fails on any error.
#[cfg(feature = "native")]
pub fn no_errors<R>(g: &Gpu, what: &str, f: impl FnOnce() -> R) -> R {
    let validation = g.device.push_error_scope(wgpu::ErrorFilter::Validation);
    let memory = g.device.push_error_scope(wgpu::ErrorFilter::OutOfMemory);
    let r = f();
    let m = pollster::block_on(memory.pop());
    let v = pollster::block_on(validation.pop());
    assert!(m.is_none(), "{what}: out of memory: {m:?}");
    assert!(v.is_none(), "{what}: validation error: {v:?}");
    r
}

// ---------------------------------------------------------------- a small JSON reader

/// A JSON value (integers only as numbers: `tests/data` holds no fractions).
#[derive(Clone, Debug, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Int(i64),
    Str(String),
    Array(Vec<Json>),
    Object(BTreeMap<String, Json>),
}

impl Json {
    pub fn parse(text: &str) -> Json {
        let b = text.as_bytes();
        let mut at = 0;
        let v = value(b, &mut at);
        skip(b, &mut at);
        assert_eq!(at, b.len(), "JSON: trailing bytes at {at}");
        v
    }
    pub fn get(&self, key: &str) -> &Json {
        match self {
            Json::Object(m) => m.get(key).unwrap_or_else(|| panic!("JSON: no key {key:?}")),
            _ => panic!("JSON: {key:?} looked up in a non-object"),
        }
    }
    pub fn has(&self, key: &str) -> bool {
        matches!(self, Json::Object(m) if m.contains_key(key))
    }
    pub fn int(&self) -> i64 {
        match self {
            Json::Int(v) => *v,
            _ => panic!("JSON: not an integer: {self:?}"),
        }
    }
    pub fn u(&self) -> u64 {
        u64::try_from(self.int()).expect("JSON: negative where a size was expected")
    }
    pub fn str(&self) -> &str {
        match self {
            Json::Str(s) => s,
            _ => panic!("JSON: not a string: {self:?}"),
        }
    }
    pub fn arr(&self) -> &[Json] {
        match self {
            Json::Array(v) => v,
            _ => panic!("JSON: not an array: {self:?}"),
        }
    }
    pub fn obj(&self) -> &BTreeMap<String, Json> {
        match self {
            Json::Object(m) => m,
            _ => panic!("JSON: not an object: {self:?}"),
        }
    }
}

fn skip(b: &[u8], at: &mut usize) {
    while *at < b.len() && matches!(b[*at], b' ' | b'\n' | b'\r' | b'\t') {
        *at += 1;
    }
}

fn expect(b: &[u8], at: &mut usize, c: u8) {
    skip(b, at);
    assert!(*at < b.len() && b[*at] == c, "JSON: expected {:?} at {}", c as char, *at);
    *at += 1;
}

fn string(b: &[u8], at: &mut usize) -> String {
    expect(b, at, b'"');
    let mut s = Vec::new();
    while b[*at] != b'"' {
        assert!(b[*at] != b'\\', "JSON: escapes are not used in tests/data");
        s.push(b[*at]);
        *at += 1;
    }
    *at += 1;
    String::from_utf8(s).expect("JSON: not UTF-8")
}

fn value(b: &[u8], at: &mut usize) -> Json {
    skip(b, at);
    match b[*at] {
        b'{' => {
            *at += 1;
            let mut m = BTreeMap::new();
            skip(b, at);
            if b[*at] == b'}' {
                *at += 1;
                return Json::Object(m);
            }
            loop {
                let k = string(b, at);
                expect(b, at, b':');
                let v = value(b, at);
                assert!(m.insert(k.clone(), v).is_none(), "JSON: key {k:?} twice");
                skip(b, at);
                *at += 1;
                match b[*at - 1] {
                    b',' => continue,
                    b'}' => return Json::Object(m),
                    c => panic!("JSON: {:?} in an object at {}", c as char, *at - 1),
                }
            }
        }
        b'[' => {
            *at += 1;
            let mut v = Vec::new();
            skip(b, at);
            if b[*at] == b']' {
                *at += 1;
                return Json::Array(v);
            }
            loop {
                v.push(value(b, at));
                skip(b, at);
                *at += 1;
                match b[*at - 1] {
                    b',' => continue,
                    b']' => return Json::Array(v),
                    c => panic!("JSON: {:?} in an array at {}", c as char, *at - 1),
                }
            }
        }
        b'"' => Json::Str(string(b, at)),
        b't' if b[*at..].starts_with(b"true") => {
            *at += 4;
            Json::Bool(true)
        }
        b'f' if b[*at..].starts_with(b"false") => {
            *at += 5;
            Json::Bool(false)
        }
        b'n' if b[*at..].starts_with(b"null") => {
            *at += 4;
            Json::Null
        }
        _ => {
            let start = *at;
            if b[*at] == b'-' {
                *at += 1;
            }
            while *at < b.len() && b[*at].is_ascii_digit() {
                *at += 1;
            }
            assert!(*at > start, "JSON: unexpected {:?} at {start}", b[start] as char);
            assert!(*at >= b.len() || !matches!(b[*at], b'.' | b'e' | b'E'), "JSON: fractions are not used in tests/data");
            Json::Int(std::str::from_utf8(&b[start..*at]).unwrap().parse().expect("JSON: integer"))
        }
    }
}

/// `tests/data/<name>`, parsed.
pub fn data(name: &str) -> Json {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/data").join(name);
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    Json::parse(&text)
}
