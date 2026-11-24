//! optrace_replay: replays a `.optrace` (port/render/OPTRACE.md) through dhd_render's exact ops and
//! compares every READ and CAPTURE with what the C path got (task R1a).
//!
//! ```text
//! optrace_replay TRACE [--adapter hw|fallback|VVVV:DDDD] [--vulkan] [--fxc] [--dxc PATH]
//!                      [--debug] [--validation] [--log-level N] [--all]
//! optrace_replay --selftest [--gpu [device options]]
//! ```
//!
//! The trace is read twice. The first pass checks the whole file (magic, version, header, every
//! record's tag, size and zero padding, END's record count, per-tag counts and FNV-1a 64, nothing
//! after END) and runs no op, so a truncated, reordered, dropped or edited trace fails before the
//! device sees anything. The second pass creates one exact instance with the header's
//! `raster_flags` and runs every op in trace order through the C ABI exports (`dhdr_exact_*`, the
//! calls `port/gs/gpu_rust.c` makes, each inside the MXCSR guard). Every READ is made into a
//! zero-filled 4 MiB image and its `result` and every set page are compared byte for byte; every
//! CAPTURE's size and pixels likewise. There is no tolerance option.
//!
//! Exit codes: 0 pass; 1 a mismatch, an invalid trace, or a device that could not be made (never a
//! pass); 2 refused (not an optrace, a version this replayer does not know, bad arguments, or an
//! early-depth trace on the fallback adapter). The first mismatch prints its record index, op, page
//! and first differing word, then the replay stops; `--all` keeps going and still exits 1.
//!
//! `--selftest` proves the replayer itself on a synthetic trace (no game data, no GPU): a CPU model
//! of the seam's write/copy/submit/read semantics replays it, and planted corruptions must fail: a
//! corrupted READ page and a reordered SUBMIT (each both with the END seal left stale, caught by the
//! integrity pass, and resealed, caught by the comparison), plus truncation, trailing bytes, a wrong
//! record count, wrong per-tag counts, nonzero padding, an unknown tag, a bad payload size, a record
//! after DESTROY, a capture size mismatch, an unknown version and a bad magic. `--selftest --gpu`
//! also runs the passing and the resealed cases on the real device (needs the exact ops).

use dhd_render::abi::*;
use dhd_render_tools::{fnv1a, FNV_BASIS};
use std::ffi::{c_void, CStr, CString};
use std::fs::File;
use std::io::{BufReader, Cursor, Read};

const TAG_WRITE: u32 = 1;
const TAG_COPY: u32 = 2;
const TAG_BATCH: u32 = 3;
const TAG_RECORD: u32 = 4;
const TAG_SUBMIT: u32 = 5;
const TAG_READ: u32 = 6;
const TAG_CAPTURE: u32 = 7;
const TAG_DESTROY: u32 = 8;
const TAG_END: u32 = 15;

const MAGIC: &[u8; 8] = b"DHDOPTR\0";
const VERSION: u32 = 1;
const HEADER_FIXED: usize = 88;
const PAGE: usize = DHDR_PAGE_BYTES as usize;
const PAGES: usize = DHDR_PAGES as usize;
const IMAGE_BYTES: usize = PAGE * PAGES;
const END_PAYLOAD: usize = 8 + 8 + 4 * 8;
const READ_HEAD: usize = 4 * 18;
/// The largest display texture (1024 x 1024 RGBA8).
const MAX_CAPTURE_PIXELS: u64 = 1024 * 1024;
/// The largest buffer (bins, 16 MiB): no WRITE can carry more.
const MAX_WRITE: u64 = 16 << 20;
/// No valid record is larger than a WRITE of the largest buffer.
const MAX_PAYLOAD: u64 = 12 + MAX_WRITE;

/// Buffer sizes (OPTRACE.md's table), for the selftest's CPU model.
const BUFFER_BYTES: [usize; DHDR_B_COUNT as usize] = [
    4 << 20,     // vram
    4 << 20,     // snapshot
    1024 * 3088, // states
    16384 * 208, // primitives
    16 << 20,    // bins
    48,          // display constants
    48,          // copy constants
    4 << 20,     // readback
    4 << 20,     // staged
    4096 * 256,  // batches
    4 << 20,     // upload
];

fn tag_name(tag: u32) -> &'static str {
    match tag {
        TAG_WRITE => "WRITE",
        TAG_COPY => "COPY",
        TAG_BATCH => "BATCH",
        TAG_RECORD => "RECORD",
        TAG_SUBMIT => "SUBMIT",
        TAG_READ => "READ",
        TAG_CAPTURE => "CAPTURE",
        TAG_DESTROY => "DESTROY",
        TAG_END => "END",
        _ => "unknown",
    }
}

// ------------------------------------------------------------------------------------ outcome

/// Why a replay is not a pass. `Refused` exits 2, the others 1.
#[derive(Debug, Clone, PartialEq, Eq)]
enum Failure {
    /// Not a trace this replayer runs (magic, version, options).
    Refused(String),
    /// The integrity pass failed: no op was run.
    Invalid(String),
    /// The replay disagreed with the trace (or the seam failed).
    Mismatch(String),
    /// The device or the exact instance could not be made: nothing was compared.
    Error(String),
}

impl Failure {
    fn code(&self) -> i32 {
        match self {
            Failure::Refused(_) => 2,
            _ => 1,
        }
    }
    fn kind(&self) -> &'static str {
        match self {
            Failure::Refused(_) => "refused",
            Failure::Invalid(_) => "invalid",
            Failure::Mismatch(_) => "mismatch",
            Failure::Error(_) => "error",
        }
    }
    fn text(&self) -> &str {
        match self {
            Failure::Refused(s) | Failure::Invalid(s) | Failure::Mismatch(s) | Failure::Error(s) => s,
        }
    }
}

// ------------------------------------------------------------------------------------ header

#[derive(Debug, Clone)]
struct Header {
    header_bytes: u32,
    raster_flags: u32,
    api: u32,
    vendor_id: u32,
    device_id: u32,
    compiler: u32,
    fallback: u32,
    driver_version: u64,
    recorder_sha: String,
    adapter: String,
    source: String,
}

fn le32(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
}

fn le64(b: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(b[at..at + 8].try_into().unwrap())
}

/// Reads exactly `buf.len()` bytes; false at a clean or partial end of input.
fn read_full(r: &mut dyn Read, buf: &mut [u8]) -> Result<bool, String> {
    let mut got = 0;
    while got < buf.len() {
        match r.read(&mut buf[got..]) {
            Ok(0) => return Ok(false),
            Ok(n) => got += n,
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
            Err(e) => return Err(format!("read error: {e}")),
        }
    }
    Ok(true)
}

fn read_header(r: &mut dyn Read) -> Result<Header, Failure> {
    let io = |e: String| Failure::Invalid(e);
    let mut fixed = [0u8; HEADER_FIXED + 4];
    if !read_full(r, &mut fixed[..16]).map_err(io)? {
        return Err(Failure::Refused("not an optrace: shorter than its header".into()));
    }
    if &fixed[..8] != MAGIC {
        return Err(Failure::Refused("not an optrace: bad magic".into()));
    }
    let version = le32(&fixed, 8);
    if version != VERSION {
        return Err(Failure::Refused(format!("optrace version {version}: this replayer knows version {VERSION} only")));
    }
    if !read_full(r, &mut fixed[16..]).map_err(io)? {
        return Err(Failure::Invalid("truncated header".into()));
    }
    let header_bytes = le32(&fixed, 12);
    if !header_bytes.is_multiple_of(4) || (header_bytes as usize) < HEADER_FIXED + 8 {
        return Err(Failure::Invalid(format!("header_bytes {header_bytes}: not a multiple of 4 or too small")));
    }
    if header_bytes > 1 << 20 {
        return Err(Failure::Invalid(format!("header_bytes {header_bytes}: implausibly large")));
    }
    let mut rest = vec![0u8; header_bytes as usize - fixed.len()];
    if !read_full(r, &mut rest).map_err(io)? {
        return Err(Failure::Invalid("truncated header".into()));
    }
    let mut all = fixed.to_vec();
    all.extend_from_slice(&rest);

    let sha = &all[48..88];
    if !sha.iter().all(|c| c.is_ascii_hexdigit()) {
        return Err(Failure::Invalid("recorder_sha is not 40 ASCII hex digits".into()));
    }
    let text = |at: usize, what: &str| -> Result<(String, usize), Failure> {
        if at + 4 > all.len() {
            return Err(Failure::Invalid(format!("{what}_bytes runs past header_bytes")));
        }
        let n = le32(&all, at) as usize;
        if at + 4 + n > all.len() {
            return Err(Failure::Invalid(format!("{what} ({n} bytes) runs past header_bytes")));
        }
        let s = std::str::from_utf8(&all[at + 4..at + 4 + n])
            .map_err(|_| Failure::Invalid(format!("{what} is not UTF-8")))?;
        Ok((s.to_owned(), at + 4 + n))
    };
    let (adapter, next) = text(HEADER_FIXED, "adapter")?;
    let (source, end) = text(next, "source")?;
    if all[end..].iter().any(|&b| b != 0) {
        return Err(Failure::Invalid("nonzero header padding".into()));
    }
    Ok(Header {
        header_bytes,
        raster_flags: le32(&all, 16),
        api: le32(&all, 20),
        vendor_id: le32(&all, 24),
        device_id: le32(&all, 28),
        compiler: le32(&all, 32),
        fallback: le32(&all, 36),
        driver_version: le64(&all, 40),
        recorder_sha: String::from_utf8_lossy(sha).into_owned(),
        adapter,
        source,
    })
}

fn api_name(api: u32) -> &'static str {
    match api {
        1 => "DX12",
        2 => "Vulkan",
        3 => "browser WebGPU",
        _ => "unknown",
    }
}

fn compiler_name(c: u32) -> &'static str {
    match c {
        1 => "FXC",
        2 => "DXC",
        _ => "none/unknown",
    }
}

fn print_header(path: &str, h: &Header) {
    println!("optrace: {path}");
    println!("optrace: version {VERSION}, header {} bytes, raster_flags {:#x}", h.header_bytes, h.raster_flags);
    println!("optrace: recorded on '{}' ({}, vendor {:#06x} device {:#06x}, driver {:#x}, compiler {}, fallback {})",
             h.adapter, api_name(h.api), h.vendor_id, h.device_id, h.driver_version, compiler_name(h.compiler),
             h.fallback);
    println!("optrace: recorder {}, source '{}'", h.recorder_sha, h.source);
}

// ------------------------------------------------------------------------------------ records

/// One decoded record (payload checked against its tag's exact size).
#[derive(Debug)]
enum Op<'a> {
    Write { buffer: u32, offset: u32, data: &'a [u8] },
    Copy { from: u32, from_offset: u32, to: u32, to_offset: u32, bytes: u32 },
    Batch { dynamic_offset: u32, groups_x: u32, groups_y: u32 },
    Record { pipeline: u32, x_word: u32, y_word: u32 },
    Submit,
    Read { buffer: u32, want: [u32; 16], result: u32, pages: &'a [u8] },
    Capture { width: u32, height: u32, pixels: &'a [u8] },
    Destroy,
}

fn want_pages(want: &[u32; 16]) -> usize {
    want.iter().map(|w| w.count_ones() as usize).sum()
}

fn decode(tag: u32, p: &[u8]) -> Result<Op<'_>, String> {
    let need = |n: usize| -> Result<(), String> {
        if p.len() == n {
            Ok(())
        } else {
            Err(format!("{} payload is {} bytes, must be {n}", tag_name(tag), p.len()))
        }
    };
    let w = |i: usize| le32(p, 4 * i);
    let buffer_ok = |b: u32| -> Result<(), String> {
        if b < DHDR_B_COUNT {
            Ok(())
        } else {
            Err(format!("{} names buffer {b} (DHDR_B_COUNT is {DHDR_B_COUNT})", tag_name(tag)))
        }
    };
    Ok(match tag {
        TAG_WRITE => {
            if p.len() < 12 {
                return Err(format!("WRITE payload is {} bytes, shorter than its 12-byte head", p.len()));
            }
            let bytes = w(2) as usize;
            need(12 + bytes)?;
            buffer_ok(w(0))?;
            Op::Write { buffer: w(0), offset: w(1), data: &p[12..] }
        }
        TAG_COPY => {
            need(20)?;
            buffer_ok(w(0))?;
            buffer_ok(w(2))?;
            Op::Copy { from: w(0), from_offset: w(1), to: w(2), to_offset: w(3), bytes: w(4) }
        }
        TAG_BATCH => {
            need(12)?;
            Op::Batch { dynamic_offset: w(0), groups_x: w(1), groups_y: w(2) }
        }
        TAG_RECORD => {
            need(12)?;
            if w(0) != DHDR_P_LOCAL_COPY && w(0) != DHDR_P_SCANOUT {
                return Err(format!("RECORD names pipeline {} (1 local_copy, 2 scanout)", w(0)));
            }
            Op::Record { pipeline: w(0), x_word: w(1), y_word: w(2) }
        }
        TAG_SUBMIT => {
            need(0)?;
            Op::Submit
        }
        TAG_READ => {
            if p.len() < READ_HEAD {
                return Err(format!("READ payload is {} bytes, shorter than its {READ_HEAD}-byte head", p.len()));
            }
            let buffer = w(0);
            if buffer != DHDR_B_READBACK && buffer != DHDR_B_STAGED {
                return Err(format!("READ of buffer {buffer} (7 readback or 8 staged only)"));
            }
            let mut want = [0u32; 16];
            for (i, v) in want.iter_mut().enumerate() {
                *v = w(1 + i);
            }
            let result = w(17);
            match result {
                1 => need(READ_HEAD + want_pages(&want) * PAGE)?,
                0 => need(READ_HEAD)?,
                r => return Err(format!("READ result {r} (1 ok or 0 failed only)")),
            }
            Op::Read { buffer, want, result, pages: &p[READ_HEAD..] }
        }
        TAG_CAPTURE => {
            if p.len() < 8 {
                return Err(format!("CAPTURE payload is {} bytes, shorter than its 8-byte head", p.len()));
            }
            let (width, height) = (w(0), w(1));
            let pixels = width as u64 * height as u64;
            if (width == 0) != (height == 0) || pixels > MAX_CAPTURE_PIXELS {
                return Err(format!("CAPTURE of {width} x {height}"));
            }
            need(8 + pixels as usize * 4)?;
            Op::Capture { width, height, pixels: &p[8..] }
        }
        TAG_DESTROY => {
            need(0)?;
            Op::Destroy
        }
        t => return Err(format!("unknown tag {t}")),
    })
}

/// What the trace holds, counted by the integrity pass.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
struct Totals {
    records: u64,
    counts: [u32; 8],
    read_pages: u64,
    destroyed: bool,
}

/// Streams the records after the header, keeping the running FNV-1a and the offset.
struct Records<'r> {
    r: &'r mut dyn Read,
    offset: u64,
    fnv: u64,
    index: u64,
    payload: Vec<u8>,
}

/// One raw record, or END.
enum Next {
    Record { tag: u32, offset: u64 },
    End { offset: u64 },
}

impl<'r> Records<'r> {
    fn new(r: &'r mut dyn Read, header_bytes: u32) -> Records<'r> {
        Records { r, offset: header_bytes as u64, fnv: FNV_BASIS, index: 0, payload: Vec::new() }
    }

    /// The next record (its payload in `self.payload`, padding checked and stripped).
    fn next(&mut self) -> Result<Next, String> {
        let at = self.offset;
        let mut prefix = [0u8; 8];
        if !read_full(self.r, &mut prefix)? {
            return Err(format!("truncated: no END (record {} at byte {at})", self.index));
        }
        let tag = le32(&prefix, 0);
        let bytes = le32(&prefix, 4) as u64;
        if tag == TAG_END {
            if bytes as usize != END_PAYLOAD {
                return Err(format!("END at byte {at} has a {bytes}-byte payload, must be {END_PAYLOAD}"));
            }
            self.payload.resize(END_PAYLOAD, 0);
            if !read_full(self.r, &mut self.payload)? {
                return Err(format!("truncated END at byte {at}"));
            }
            self.offset += 8 + END_PAYLOAD as u64;
            return Ok(Next::End { offset: at });
        }
        if bytes > MAX_PAYLOAD {
            return Err(format!("record {} ({}) at byte {at}: payload of {bytes} bytes is larger than any valid record",
                               self.index, tag_name(tag)));
        }
        let padded = (bytes as usize + 3) & !3;
        self.payload.resize(padded, 0);
        if !read_full(self.r, &mut self.payload)? {
            return Err(format!("truncated: record {} ({}) at byte {at} runs past the end of the file",
                               self.index, tag_name(tag)));
        }
        if self.payload[bytes as usize..].iter().any(|&b| b != 0) {
            return Err(format!("record {} ({}) at byte {at}: nonzero padding", self.index, tag_name(tag)));
        }
        self.fnv = fnv1a(self.fnv, &prefix);
        self.fnv = fnv1a(self.fnv, &self.payload);
        self.payload.truncate(bytes as usize);
        self.offset += 8 + padded as u64;
        self.index += 1;
        Ok(Next::Record { tag, offset: at })
    }
}

/// The integrity pass: the whole trace, no op run.
fn verify(r: &mut dyn Read) -> Result<(Header, Totals), Failure> {
    let h = read_header(r)?;
    let mut t = Totals::default();
    let mut recs = Records::new(r, h.header_bytes);
    loop {
        match recs.next().map_err(Failure::Invalid)? {
            Next::End { offset } => {
                let e = &recs.payload;
                let (records, fnv) = (le64(e, 0), le64(e, 8));
                let mut counts = [0u32; 8];
                for (i, c) in counts.iter_mut().enumerate() {
                    *c = le32(e, 16 + 4 * i);
                }
                if records != t.records {
                    return Err(Failure::Invalid(format!("END at byte {offset} says {records} records, the trace has {}",
                                                        t.records)));
                }
                if counts != t.counts {
                    return Err(Failure::Invalid(format!("END at byte {offset}: per-tag counts {counts:?}, the trace has {:?}",
                                                        t.counts)));
                }
                if fnv != recs.fnv {
                    return Err(Failure::Invalid(format!("END at byte {offset}: fnv {fnv:#018x}, the records hash to {:#018x} \
                                                         (a record was edited, reordered or dropped)", recs.fnv)));
                }
                let mut extra = [0u8; 1];
                if read_full(recs.r, &mut extra).map_err(Failure::Invalid)? {
                    return Err(Failure::Invalid(format!("bytes after END (at byte {})", recs.offset)));
                }
                return Ok((h, t));
            }
            Next::Record { tag, offset } => {
                let index = recs.index - 1;
                if t.destroyed {
                    return Err(Failure::Invalid(format!("record {index} ({}) at byte {offset} comes after DESTROY",
                                                        tag_name(tag))));
                }
                let op = decode(tag, &recs.payload)
                    .map_err(|e| Failure::Invalid(format!("record {index} at byte {offset}: {e}")))?;
                match op {
                    Op::Read { result: 1, want, .. } => t.read_pages += want_pages(&want) as u64,
                    Op::Destroy => t.destroyed = true,
                    _ => {}
                }
                t.records += 1;
                // END's counts cover tags 0-7 (OPTRACE.md); DESTROY (8) is in `records` only.
                if (tag as usize) < t.counts.len() {
                    t.counts[tag as usize] += 1;
                }
            }
        }
    }
}

fn print_totals(t: &Totals) {
    let c = &t.counts;
    println!("optrace: {} records: {} writes, {} copies, {} batches, {} records, {} submits, {} reads ({} pages), \
              {} captures, destroy {}",
             t.records, c[1], c[2], c[3], c[4], c[5], c[6], t.read_pages, c[7], if t.destroyed { "yes" } else { "no" });
}

// ------------------------------------------------------------------------------------ the seam

/// The exact device ops a trace replays through (`dhd_render.h`'s `dhdr_exact_*`).
trait Seam {
    fn write(&mut self, buffer: u32, offset: u32, data: &[u8]) -> Result<(), String>;
    fn copy(&mut self, from: u32, from_offset: u32, to: u32, to_offset: u32, bytes: u32) -> Result<(), String>;
    fn batch(&mut self, dynamic_offset: u32, groups_x: u32, groups_y: u32) -> Result<(), String>;
    fn record(&mut self, pipeline: u32, x_word: u32, y_word: u32) -> Result<(), String>;
    fn submit(&mut self) -> Result<(), String>;
    /// The read's `result` (1 ok, 0 failed); the wanted pages land in `image`.
    fn read(&mut self, buffer: u32, want: &[u32; 16], image: &mut [u8]) -> Result<u32, String>;
    /// The display texture: (width, height, RGBA8 bytes), (0, 0, empty) when there is none.
    fn capture(&mut self) -> Result<(u32, u32, Vec<u8>), String>;
    fn destroy(&mut self) -> Result<(), String>;
}

/// dhd_render's exact renderer through its C exports.
struct DeviceSeam {
    x: *mut DhdrExact,
}

impl DeviceSeam {
    fn create(raster_flags: u32) -> Result<DeviceSeam, String> {
        // SAFETY: plain export call; NULL on failure (logged by the renderer).
        let x = unsafe { dhd_render::ffi::dhdr_exact_create(raster_flags) };
        if x.is_null() {
            return Err(format!("dhdr_exact_create({raster_flags:#x}) failed"));
        }
        Ok(DeviceSeam { x })
    }

    fn stats(&self) -> Option<DhdrExactStats> {
        if self.x.is_null() {
            return None;
        }
        let mut s = DhdrExactStats::default();
        // SAFETY: x is live; s is an output.
        unsafe { dhd_render::ffi::dhdr_exact_stats(self.x, &mut s) };
        Some(s)
    }

    fn live(&self) -> Result<*mut DhdrExact, String> {
        if self.x.is_null() {
            Err("the exact instance was destroyed".into())
        } else {
            Ok(self.x)
        }
    }
}

impl Seam for DeviceSeam {
    fn write(&mut self, buffer: u32, offset: u32, data: &[u8]) -> Result<(), String> {
        let x = self.live()?;
        // SAFETY: x is live; data holds data.len() bytes (decode checked it equals the record's count).
        unsafe {
            dhd_render::ffi::dhdr_exact_write(x, buffer, offset, data.as_ptr() as *const c_void, data.len() as u32)
        };
        Ok(())
    }
    fn copy(&mut self, from: u32, from_offset: u32, to: u32, to_offset: u32, bytes: u32) -> Result<(), String> {
        let x = self.live()?;
        // SAFETY: x is live.
        unsafe { dhd_render::ffi::dhdr_exact_copy(x, from, from_offset, to, to_offset, bytes) };
        Ok(())
    }
    fn batch(&mut self, dynamic_offset: u32, groups_x: u32, groups_y: u32) -> Result<(), String> {
        let x = self.live()?;
        // SAFETY: x is live.
        unsafe { dhd_render::ffi::dhdr_exact_batch(x, dynamic_offset, groups_x, groups_y) };
        Ok(())
    }
    fn record(&mut self, pipeline: u32, x_word: u32, y_word: u32) -> Result<(), String> {
        let x = self.live()?;
        // SAFETY: x is live.
        unsafe { dhd_render::ffi::dhdr_exact_record(x, pipeline, x_word, y_word) };
        Ok(())
    }
    fn submit(&mut self) -> Result<(), String> {
        let x = self.live()?;
        // SAFETY: x is live.
        unsafe { dhd_render::ffi::dhdr_exact_submit(x) };
        Ok(())
    }
    fn read(&mut self, buffer: u32, want: &[u32; 16], image: &mut [u8]) -> Result<u32, String> {
        let x = self.live()?;
        assert_eq!(image.len(), IMAGE_BYTES);
        // SAFETY: x is live; want is 16 words.
        if unsafe { dhd_render::ffi::dhdr_exact_read_begin(x, buffer, want.as_ptr()) } != 1 {
            return Ok(0);
        }
        // SAFETY: x is live and held by the begin above; image is the 4 MiB page image.
        let r = unsafe { dhd_render::ffi::dhdr_exact_read_finish(x, buffer, want.as_ptr(), image.as_mut_ptr() as *mut c_void) };
        Ok(if r == 1 { 1 } else { 0 })
    }
    fn capture(&mut self) -> Result<(u32, u32, Vec<u8>), String> {
        let x = self.live()?;
        let (mut w, mut h) = (0u32, 0u32);
        // SAFETY: x is live; w and h are outputs.
        if unsafe { dhd_render::ffi::dhdr_exact_capture_begin(x, &mut w, &mut h) } != 1 {
            return Ok((0, 0, Vec::new()));
        }
        if w as u64 * h as u64 > MAX_CAPTURE_PIXELS || w == 0 || h == 0 {
            return Err(format!("dhdr_exact_capture_begin announced {w} x {h}"));
        }
        let mut pixels = vec![0u32; w as usize * h as usize];
        // SAFETY: x is live and held by the begin; pixels holds the w * h pixels begin announced.
        if unsafe { dhd_render::ffi::dhdr_exact_capture_finish(x, pixels.as_mut_ptr()) } != 1 {
            return Err("dhdr_exact_capture_finish failed (map)".into());
        }
        Ok((w, h, pixels.iter().flat_map(|p| p.to_le_bytes()).collect()))
    }
    fn destroy(&mut self) -> Result<(), String> {
        let x = self.live()?;
        if let Some(s) = self.stats() {
            println!("replay: stats: {} submits, {} reads, {} dispatches, {} bytes written, {} copies",
                     s.submits, s.reads, s.dispatches, s.bytes_written, s.copies);
        }
        // SAFETY: x is live; it is given up here.
        unsafe { dhd_render::ffi::dhdr_exact_destroy(x) };
        self.x = std::ptr::null_mut();
        Ok(())
    }
}

impl Drop for DeviceSeam {
    fn drop(&mut self) {
        if !self.x.is_null() {
            // SAFETY: x is live and owned here.
            unsafe { dhd_render::ffi::dhdr_exact_destroy(self.x) };
        }
    }
}

/// The selftest's CPU model of the seam's buffer semantics (OPTRACE.md): a WRITE is a queue write
/// and lands at once, so before every command buffer submitted after it; COPY is recorded in the
/// open encoder and runs at the next SUBMIT (or the submit a READ starts with); READ submits, then
/// reads the wanted pages. It has no shaders (BATCH and RECORD are errors) and no display texture.
struct ModelSeam {
    buffers: Vec<Vec<u8>>,
    pending: Vec<(u32, u32, u32, u32, u32)>,
    destroyed: bool,
}

impl ModelSeam {
    fn new() -> ModelSeam {
        ModelSeam { buffers: BUFFER_BYTES.iter().map(|&n| vec![0u8; n]).collect(), pending: Vec::new(), destroyed: false }
    }
    fn range(&self, buffer: u32, offset: u32, bytes: u32) -> Result<std::ops::Range<usize>, String> {
        let size = *BUFFER_BYTES.get(buffer as usize).ok_or(format!("model: no buffer {buffer}"))?;
        let (a, b) = (offset as usize, offset as usize + bytes as usize);
        if b > size {
            return Err(format!("model: buffer {buffer} [{a}, {b}) past its {size} bytes"));
        }
        Ok(a..b)
    }
    fn alive(&self) -> Result<(), String> {
        if self.destroyed {
            Err("model: op after destroy".into())
        } else {
            Ok(())
        }
    }
}

impl Seam for ModelSeam {
    fn write(&mut self, buffer: u32, offset: u32, data: &[u8]) -> Result<(), String> {
        self.alive()?;
        let r = self.range(buffer, offset, data.len() as u32)?;
        self.buffers[buffer as usize][r].copy_from_slice(data);
        Ok(())
    }
    fn copy(&mut self, from: u32, from_offset: u32, to: u32, to_offset: u32, bytes: u32) -> Result<(), String> {
        self.alive()?;
        self.range(from, from_offset, bytes)?;
        self.range(to, to_offset, bytes)?;
        self.pending.push((from, from_offset, to, to_offset, bytes));
        Ok(())
    }
    fn batch(&mut self, _: u32, _: u32, _: u32) -> Result<(), String> {
        Err("model: BATCH needs the device (the model has no shaders)".into())
    }
    fn record(&mut self, _: u32, _: u32, _: u32) -> Result<(), String> {
        Err("model: RECORD needs the device (the model has no shaders)".into())
    }
    fn submit(&mut self) -> Result<(), String> {
        self.alive()?;
        for (from, fo, to, too, bytes) in std::mem::take(&mut self.pending) {
            let src = self.buffers[from as usize][fo as usize..(fo + bytes) as usize].to_vec();
            self.buffers[to as usize][too as usize..(too + bytes) as usize].copy_from_slice(&src);
        }
        Ok(())
    }
    fn read(&mut self, buffer: u32, want: &[u32; 16], image: &mut [u8]) -> Result<u32, String> {
        self.submit()?;
        for page in 0..PAGES {
            if want[page / 32] >> (page % 32) & 1 != 0 {
                let r = page * PAGE..(page + 1) * PAGE;
                image[r.clone()].copy_from_slice(&self.buffers[buffer as usize][r]);
            }
        }
        Ok(1)
    }
    fn capture(&mut self) -> Result<(u32, u32, Vec<u8>), String> {
        self.alive()?;
        Ok((0, 0, Vec::new()))
    }
    fn destroy(&mut self) -> Result<(), String> {
        self.submit()?;
        self.destroyed = true;
        Ok(())
    }
}

// ------------------------------------------------------------------------------------ replay

#[derive(Debug, Default, Clone, PartialEq, Eq)]
struct Compared {
    reads: u64,
    pages: u64,
    captures: u64,
    mismatches: u64,
}

/// The first differing 32-bit word of two equal-length byte slices.
fn first_word_diff(a: &[u8], b: &[u8]) -> Option<(usize, u32, u32)> {
    a.chunks(4).zip(b.chunks(4)).enumerate().find(|(_, (x, y))| x != y).map(|(i, (x, y))| {
        let word = |c: &[u8]| {
            let mut w = [0u8; 4];
            w[..c.len()].copy_from_slice(c);
            u32::from_le_bytes(w)
        };
        (i, word(x), word(y))
    })
}

/// The replay pass: every op in order on `seam`, every READ and CAPTURE compared. The trace has
/// passed `verify` already.
/// `mark` starts every mismatch line: `FAIL` in a real run, a neutral word in the selftest (whose
/// planted failures are expected, so they must not read as a failing run to `gs_verdict.py`).
fn run(r: &mut dyn Read, seam: &mut dyn Seam, all: bool, t: &Totals, mark: &str) -> (Compared, Option<Failure>) {
    let mut c = Compared::default();
    let fail_now = |c: &Compared, text: String| (c.clone(), Some(Failure::Mismatch(text)));
    let h = match read_header(r) {
        Ok(h) => h,
        Err(e) => return (c, Some(e)),
    };
    let mut recs = Records::new(r, h.header_bytes);
    let mut image = vec![0u8; IMAGE_BYTES];
    let mut first: Option<String> = None;
    let mut destroyed = false;
    loop {
        let (tag, offset) = match recs.next() {
            Ok(Next::Record { tag, offset }) => (tag, offset),
            Ok(Next::End { .. }) => break,
            Err(e) => return (c, Some(Failure::Invalid(format!("the trace changed between the passes: {e}")))),
        };
        let index = recs.index - 1;
        let op = match decode(tag, &recs.payload) {
            Ok(op) => op,
            Err(e) => return (c, Some(Failure::Invalid(format!("the trace changed between the passes: {e}")))),
        };
        let mut mismatch = |c: &mut Compared, text: String| {
            println!("{mark} record {index} (byte {offset}) {}: {text}", tag_name(tag));
            c.mismatches += 1;
            if first.is_none() {
                first = Some(format!("record {index} {}: {text}", tag_name(tag)));
            }
        };
        let step = match op {
            Op::Write { buffer, offset, data } => seam.write(buffer, offset, data),
            Op::Copy { from, from_offset, to, to_offset, bytes } => seam.copy(from, from_offset, to, to_offset, bytes),
            Op::Batch { dynamic_offset, groups_x, groups_y } => seam.batch(dynamic_offset, groups_x, groups_y),
            Op::Record { pipeline, x_word, y_word } => seam.record(pipeline, x_word, y_word),
            Op::Submit => seam.submit(),
            Op::Destroy => {
                destroyed = true;
                seam.destroy()
            }
            Op::Read { buffer, want, result, pages } => {
                image.fill(0);
                match seam.read(buffer, &want, &mut image) {
                    Err(e) => Err(e),
                    Ok(got) => {
                        c.reads += 1;
                        if got != result {
                            mismatch(&mut c, format!("buffer {buffer}: result {got}, the trace has {result}"));
                        } else if result == 1 {
                            let mut n = 0;
                            let mut bad_pages = 0u32;
                            for page in 0..PAGES {
                                if want[page / 32] >> (page % 32) & 1 == 0 {
                                    continue;
                                }
                                let expect = &pages[n * PAGE..(n + 1) * PAGE];
                                n += 1;
                                c.pages += 1;
                                if let Some((word, e, g)) = first_word_diff(expect, &image[page * PAGE..(page + 1) * PAGE]) {
                                    bad_pages += 1;
                                    if bad_pages == 1 || all {
                                        mismatch(&mut c, format!("buffer {buffer} page {page}: first differing word {word} \
                                                                  (byte {:#x} of the image): trace {e:#010x}, replay {g:#010x}",
                                                                 page * PAGE + word * 4));
                                    }
                                    if !all {
                                        break;
                                    }
                                }
                            }
                        }
                        Ok(())
                    }
                }
            }
            Op::Capture { width, height, pixels } => match seam.capture() {
                Err(e) => Err(e),
                Ok((w, h, got)) => {
                    c.captures += 1;
                    if (w, h) != (width, height) {
                        mismatch(&mut c, format!("size {w} x {h}, the trace has {width} x {height}"));
                    } else if let Some((word, e, g)) = first_word_diff(pixels, &got) {
                        mismatch(&mut c, format!("pixel {word} (x {}, y {}): trace {e:#010x}, replay {g:#010x}",
                                                 word % width.max(1) as usize, word / width.max(1) as usize));
                    }
                    Ok(())
                }
            },
        };
        if let Err(e) = step {
            println!("{mark} record {index} (byte {offset}) {}: {e}", tag_name(tag));
            return fail_now(&c, format!("record {index} {}: {e}", tag_name(tag)));
        }
        if c.mismatches > 0 && !all {
            break;
        }
    }
    if !destroyed && c.mismatches == 0 {
        if let Err(e) = seam.destroy() {
            return fail_now(&c, format!("destroy at the end: {e}"));
        }
    }
    println!("compared {} reads ({} pages), {} captures", c.reads, c.pages, c.captures);
    if let Some(f) = first {
        return (c.clone(), Some(Failure::Mismatch(format!("{} mismatch(es); first: {f}", c.mismatches))));
    }
    let (reads, captures) = (t.counts[TAG_READ as usize] as u64, t.counts[TAG_CAPTURE as usize] as u64);
    if c.reads != reads || c.pages != t.read_pages || c.captures != captures {
        return fail_now(&c, format!("compared {} reads ({} pages), {} captures; the trace has {reads} ({} pages), {captures}",
                                    c.reads, c.pages, c.captures, t.read_pages));
    }
    (c, None)
}

/// Makes the seam a trace replays on, from its header.
type MakeSeam<'m> = dyn FnMut(&Header) -> Result<Box<dyn Seam>, Failure> + 'm;

/// Both passes over a trace `open` gives (twice), replayed on the seam `make` returns (after the
/// integrity pass, so a bad trace never reaches the device).
fn replay(open: &dyn Fn() -> Result<Box<dyn Read>, String>, path: &str, selftest: bool, all: bool,
          make: &mut MakeSeam) -> Result<Compared, Failure> {
    let mut r = open().map_err(Failure::Refused)?;
    let (h, t) = verify(&mut *r)?;
    if !selftest {
        print_header(path, &h);
        print_totals(&t);
    }
    drop(r);
    let mut seam = make(&h)?;
    let mut r = open().map_err(Failure::Refused)?;
    let (c, f) = run(&mut *r, &mut *seam, all, &t, if selftest { "selftest: planted:" } else { "FAIL" });
    match f {
        Some(f) => Err(f),
        None => Ok(c),
    }
}

// ------------------------------------------------------------------------------------ device

#[derive(Debug, Clone, Default)]
struct DeviceArgs {
    flags: u32,
    vendor_id: u32,
    device_id: u32,
    log_level: u32,
    dxc: Option<String>,
}

fn device_init(d: &DeviceArgs) -> Result<(), Failure> {
    let dxc = match &d.dxc {
        Some(p) => Some(CString::new(p.as_str()).map_err(|_| Failure::Refused("--dxc path has a NUL".into()))?),
        None => None,
    };
    let desc = DhdrDeviceDesc {
        size: std::mem::size_of::<DhdrDeviceDesc>() as u32,
        abi: DHDR_ABI_VERSION,
        flags: d.flags,
        vendor_id: d.vendor_id,
        device_id: d.device_id,
        log_level: d.log_level,
        dxc_path: dxc.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
        window: std::ptr::null(),
    };
    // SAFETY: desc is a valid descriptor; dxc outlives the call.
    if unsafe { dhd_render::ffi::dhdr_device_init(&desc) } != 1 {
        return Err(Failure::Error("dhdr_device_init failed (no device; not a pass)".into()));
    }
    // SAFETY: both return static NUL-terminated strings.
    let (name, version) = unsafe {
        (CStr::from_ptr(dhd_render::ffi::dhdr_adapter_name()).to_string_lossy().into_owned(),
         CStr::from_ptr(dhd_render::ffi::dhdr_version()).to_string_lossy().into_owned())
    };
    println!("replay: {version} on '{name}' (flags {:#x}, compiler {})", d.flags,
             if d.dxc.is_some() { "DXC" } else { "FXC" });
    Ok(())
}

/// The device seam for a trace: refuses an early-depth trace on the fallback adapter (WARP's JIT
/// fault, finding 084 section 10.5.6), prints any adapter difference (never refuses on it).
fn device_seam(d: &DeviceArgs, h: &Header) -> Result<Box<dyn Seam>, Failure> {
    if d.flags & DHDR_DEV_FALLBACK != 0 && h.raster_flags & DHDR_RASTER_EARLY_DEPTH != 0 {
        return Err(Failure::Refused("the trace was recorded with early depth (raster_flags bit 1); the fallback \
                                     adapter (WARP) must replay a trace without it".into()));
    }
    device_init(d)?;
    // SAFETY: returns a static NUL-terminated string.
    let name = unsafe { CStr::from_ptr(dhd_render::ffi::dhdr_adapter_name()) }.to_string_lossy().into_owned();
    if name != h.adapter {
        println!("replay: adapter differs from the recording ('{}' there); exact results must still match", h.adapter);
    }
    let api = if d.flags & DHDR_DEV_VULKAN != 0 { 2 } else { 1 };
    if api != h.api {
        println!("replay: API {} here, {} in the recording", api_name(api), api_name(h.api));
    }
    Ok(Box::new(DeviceSeam::create(h.raster_flags).map_err(Failure::Error)?))
}

// ------------------------------------------------------------------------------------ selftest

/// A synthetic trace: header fields and records (payload, padding) the selftest mutates, then
/// writes with a fresh END (sealed) or with another trace's END (stale).
#[derive(Clone)]
struct Synthetic {
    version: u32,
    magic: [u8; 8],
    raster_flags: u32,
    records: Vec<(u32, Vec<u8>, Vec<u8>)>,
}

fn words(w: &[u32]) -> Vec<u8> {
    w.iter().flat_map(|v| v.to_le_bytes()).collect()
}

impl Synthetic {
    fn push(&mut self, tag: u32, payload: Vec<u8>) {
        let pad = vec![0u8; (4 - payload.len() % 4) % 4];
        self.records.push((tag, payload, pad));
    }

    fn header(&self) -> Vec<u8> {
        let (adapter, source) = (b"selftest model".as_slice(), b"optrace_replay selftest".as_slice());
        let bytes = (HEADER_FIXED + 4 + adapter.len() + 4 + source.len() + 3) & !3;
        let mut h = vec![0u8; bytes];
        h[..8].copy_from_slice(&self.magic);
        h[8..12].copy_from_slice(&self.version.to_le_bytes());
        h[12..16].copy_from_slice(&(bytes as u32).to_le_bytes());
        h[16..20].copy_from_slice(&self.raster_flags.to_le_bytes());
        h[20..24].copy_from_slice(&1u32.to_le_bytes());
        h[48..88].fill(b'0');
        h[88..92].copy_from_slice(&(adapter.len() as u32).to_le_bytes());
        h[92..92 + adapter.len()].copy_from_slice(adapter);
        let at = 92 + adapter.len();
        h[at..at + 4].copy_from_slice(&(source.len() as u32).to_le_bytes());
        h[at + 4..at + 4 + source.len()].copy_from_slice(source);
        h
    }

    fn body(&self) -> Vec<u8> {
        let mut b = Vec::new();
        for (tag, payload, pad) in &self.records {
            b.extend_from_slice(&tag.to_le_bytes());
            b.extend_from_slice(&(payload.len() as u32).to_le_bytes());
            b.extend_from_slice(payload);
            b.extend_from_slice(pad);
        }
        b
    }

    /// END for these records.
    fn end(&self) -> Vec<u8> {
        let mut counts = [0u32; 8];
        for (tag, _, _) in &self.records {
            if (*tag as usize) < 8 {
                counts[*tag as usize] += 1;
            }
        }
        let mut e = words(&[TAG_END, END_PAYLOAD as u32]);
        e.extend_from_slice(&(self.records.len() as u64).to_le_bytes());
        e.extend_from_slice(&fnv1a(FNV_BASIS, &self.body()).to_le_bytes());
        e.extend_from_slice(&words(&counts));
        e
    }

    fn sealed(&self) -> Vec<u8> {
        let mut t = self.header();
        t.extend_from_slice(&self.body());
        t.extend_from_slice(&self.end());
        t
    }

    /// These records with `seal_of`'s END (a mutation whose END was left as recorded).
    fn stale(&self, seal_of: &Synthetic) -> Vec<u8> {
        let mut t = self.header();
        t.extend_from_slice(&self.body());
        t.extend_from_slice(&seal_of.end());
        t
    }
}

fn pattern(seed: u32, bytes: usize) -> Vec<u8> {
    let mut x = seed.wrapping_mul(0x9e3779b9) | 1;
    (0..bytes / 4)
        .flat_map(|_| {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            x.to_le_bytes()
        })
        .collect()
}

fn want_of(pages: &[usize]) -> [u32; 16] {
    let mut w = [0u32; 16];
    for &p in pages {
        w[p / 32] |= 1 << (p % 32);
    }
    w
}

fn read_payload(buffer: u32, pages: &[usize], data: &[u8]) -> Vec<u8> {
    let mut p = words(&[buffer]);
    p.extend_from_slice(&words(&want_of(pages)));
    p.extend_from_slice(&1u32.to_le_bytes());
    p.extend_from_slice(data);
    p
}

/// Record indices of the base trace the mutations aim at.
const SUBMIT_AT: usize = 2;
const READ_AT: usize = 4;
const ODD_WRITE_AT: usize = 5;
const CAPTURE_AT: usize = 9;

/// The base trace. Expected READ bytes come from the seam's documented semantics, written out here
/// by hand (not produced by the model):
/// - r0 WRITE upload[0..16K) = A; r1 COPY upload -> readback (16K); r2 SUBMIT (the copy runs: A);
///   r3 WRITE upload[0..16K) = B (after the submit: readback keeps A); r4 READ readback pages 0, 1 = A.
///   Swapping r2 and r3 makes the copy run after B landed, so r4 would read B.
/// - r5 WRITE vram[5 pages + 1, 3 bytes) (an odd size: one byte of padding); r6 WRITE vram page 5 = C
///   (over r5); r7 COPY vram page 5 -> staged page 5; r8 READ staged page 5 = C.
/// - r9 CAPTURE 0 x 0 (no display texture); r10 SUBMIT (nothing recorded); r11 DESTROY.
fn base_trace() -> (Synthetic, Vec<u8>) {
    let mut s = Synthetic { version: VERSION, magic: *MAGIC, raster_flags: 0, records: Vec::new() };
    let a = pattern(1, 2 * PAGE);
    let b = pattern(2, 2 * PAGE);
    let c = pattern(3, PAGE);
    let write = |buffer: u32, offset: u32, data: &[u8]| {
        let mut p = words(&[buffer, offset, data.len() as u32]);
        p.extend_from_slice(data);
        p
    };
    s.push(TAG_WRITE, write(DHDR_B_UPLOAD, 0, &a));
    s.push(TAG_COPY, words(&[DHDR_B_UPLOAD, 0, DHDR_B_READBACK, 0, 2 * PAGE as u32]));
    s.push(TAG_SUBMIT, Vec::new());
    s.push(TAG_WRITE, write(DHDR_B_UPLOAD, 0, &b));
    s.push(TAG_READ, read_payload(DHDR_B_READBACK, &[0, 1], &a));
    s.push(TAG_WRITE, write(DHDR_B_VRAM, 5 * PAGE as u32 + 1, &[0xAA, 0xBB, 0xCC]));
    s.push(TAG_WRITE, write(DHDR_B_VRAM, 5 * PAGE as u32, &c));
    s.push(TAG_COPY, words(&[DHDR_B_VRAM, 5 * PAGE as u32, DHDR_B_STAGED, 5 * PAGE as u32, PAGE as u32]));
    s.push(TAG_READ, read_payload(DHDR_B_STAGED, &[5], &c));
    s.push(TAG_CAPTURE, words(&[0, 0]));
    s.push(TAG_SUBMIT, Vec::new());
    s.push(TAG_DESTROY, Vec::new());
    assert_eq!(s.records[SUBMIT_AT].0, TAG_SUBMIT);
    assert_eq!(s.records[READ_AT].0, TAG_READ);
    assert_eq!(s.records[ODD_WRITE_AT].2.len(), 1);
    assert_eq!(s.records[CAPTURE_AT].0, TAG_CAPTURE);
    (s, b)
}

/// What a selftest case must give.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Expect {
    Pass,
    Invalid,
    Mismatch,
    Refused,
    /// Never expected: a device that could not be made fails every case.
    Error,
}

fn cases() -> Vec<(&'static str, Vec<u8>, Expect)> {
    let (base, b) = base_trace();
    let mut v: Vec<(&'static str, Vec<u8>, Expect)> = Vec::new();
    v.push(("base trace", base.sealed(), Expect::Pass));

    // A corrupted READ page: one byte of r4's second page.
    let mut m = base.clone();
    let at = READ_HEAD + PAGE + 100;
    m.records[READ_AT].1[at] ^= 0x40;
    v.push(("corrupted READ page, END left stale", m.stale(&base), Expect::Invalid));
    v.push(("corrupted READ page, resealed", m.sealed(), Expect::Mismatch));

    // A reordered SUBMIT: r2 moved after r3.
    let mut m = base.clone();
    m.records.swap(SUBMIT_AT, SUBMIT_AT + 1);
    v.push(("reordered SUBMIT, END left stale", m.stale(&base), Expect::Invalid));
    v.push(("reordered SUBMIT, resealed", m.sealed(), Expect::Mismatch));
    // ... and that resealed trace with the READ bytes it now really gives (B) passes: the model
    // follows the ordering, it does not just reject any change.
    let mut m2 = m.clone();
    m2.records[READ_AT].1 = read_payload(DHDR_B_READBACK, &[0, 1], &b);
    v.push(("reordered SUBMIT with its true READ bytes, resealed", m2.sealed(), Expect::Pass));

    let mut t = base.sealed();
    t.truncate(t.len() - 4);
    v.push(("truncated END", t, Expect::Invalid));
    let mut t = base.sealed();
    let end_len = 8 + END_PAYLOAD;
    t.truncate(t.len() - end_len);
    v.push(("no END", t, Expect::Invalid));
    let mut t = base.sealed();
    t.truncate(t.len() - end_len - 100);
    v.push(("record runs past the end", t, Expect::Invalid));
    let mut t = base.sealed();
    t.push(0);
    v.push(("a byte after END", t, Expect::Invalid));

    let mut t = base.sealed();
    let n = t.len();
    t[n - END_PAYLOAD..n - END_PAYLOAD + 8].copy_from_slice(&(base.records.len() as u64 + 1).to_le_bytes());
    v.push(("END record count off by one", t, Expect::Invalid));
    let mut t = base.sealed();
    let n = t.len();
    let c_submit = n - END_PAYLOAD + 16 + 4 * TAG_SUBMIT as usize;
    let c_copy = n - END_PAYLOAD + 16 + 4 * TAG_COPY as usize;
    t[c_submit] = t[c_submit].wrapping_sub(1);
    t[c_copy] = t[c_copy].wrapping_add(1);
    v.push(("END per-tag counts swapped (same total)", t, Expect::Invalid));

    let mut m = base.clone();
    m.records.remove(CAPTURE_AT);
    v.push(("dropped CAPTURE, END left stale", m.stale(&base), Expect::Invalid));

    let mut m = base.clone();
    m.records[ODD_WRITE_AT].2[0] = 1;
    v.push(("nonzero padding, resealed", m.sealed(), Expect::Invalid));

    let mut m = base.clone();
    m.records[SUBMIT_AT].0 = 9;
    v.push(("unknown tag 9, resealed", m.sealed(), Expect::Invalid));

    let mut m = base.clone();
    m.records[READ_AT].1.truncate(READ_HEAD + PAGE);
    v.push(("READ missing a page, resealed", m.sealed(), Expect::Invalid));

    let mut m = base.clone();
    m.records[READ_AT].1[4 * 17..4 * 18].copy_from_slice(&2u32.to_le_bytes());
    v.push(("READ result 2, resealed", m.sealed(), Expect::Invalid));

    let mut m = base.clone();
    m.records[READ_AT].1 = read_payload(DHDR_B_READBACK, &[0, 1], &[]);
    m.records[READ_AT].1.truncate(READ_HEAD);
    m.records[READ_AT].1[4 * 17..4 * 18].copy_from_slice(&0u32.to_le_bytes());
    v.push(("READ result 0 where the replay reads, resealed", m.sealed(), Expect::Mismatch));

    let mut m = base.clone();
    m.records[READ_AT].1[0..4].copy_from_slice(&DHDR_B_VRAM.to_le_bytes());
    v.push(("READ of the vram buffer, resealed", m.sealed(), Expect::Invalid));

    let mut m = base.clone();
    let last = m.records.len() - 1;
    m.records.swap(last, last - 1);
    v.push(("a record after DESTROY, resealed", m.sealed(), Expect::Invalid));

    let mut m = base.clone();
    let mut cap = words(&[2, 1]);
    cap.extend_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8]);
    m.records[CAPTURE_AT].1 = cap;
    v.push(("CAPTURE size the replay does not give, resealed", m.sealed(), Expect::Mismatch));

    let mut m = base.clone();
    m.records[CAPTURE_AT].1 = words(&[2, 0]);
    v.push(("CAPTURE 2 x 0, resealed", m.sealed(), Expect::Invalid));

    let mut m = base.clone();
    m.version = 2;
    v.push(("version 2", m.sealed(), Expect::Refused));
    let mut m = base.clone();
    m.magic = *b"DHDNTRC\0";
    v.push(("bad magic", m.sealed(), Expect::Refused));
    let mut t = base.sealed();
    t[12..16].copy_from_slice(&6u32.to_le_bytes());
    v.push(("header_bytes not a multiple of 4", t, Expect::Invalid));
    let mut t = base.sealed();
    t[48] = b'g';
    v.push(("recorder_sha not hex", t, Expect::Invalid));
    v
}

fn outcome(r: &Result<Compared, Failure>) -> Expect {
    match r {
        Ok(_) => Expect::Pass,
        Err(Failure::Invalid(_)) => Expect::Invalid,
        Err(Failure::Mismatch(_)) => Expect::Mismatch,
        Err(Failure::Refused(_)) => Expect::Refused,
        Err(Failure::Error(_)) => Expect::Error,
    }
}

fn selftest(gpu: Option<&DeviceArgs>) -> i32 {
    let mut failed = 0;
    let mut check = |name: &str, ok: bool, detail: String| {
        if ok {
            println!("selftest: ok   {name}: {detail}");
        } else {
            println!("FAIL selftest: {name}: {detail}");
            failed += 1;
        }
    };
    let runs: Vec<(&str, bool)> = if gpu.is_some() { vec![("model", false), ("device", true)] } else { vec![("model", false)] };
    for (seam_name, on_device) in runs {
        for (name, bytes, expect) in cases() {
            // On the device only the cases that reach the replay pass mean anything new.
            if on_device && !matches!(expect, Expect::Pass | Expect::Mismatch) {
                continue;
            }
            let open = |bytes: &Vec<u8>| -> Box<dyn Read> { Box::new(Cursor::new(bytes.clone())) };
            for all in [false, true] {
                if all && expect != Expect::Mismatch {
                    continue;
                }
                let mut make = |h: &Header| -> Result<Box<dyn Seam>, Failure> {
                    match gpu.filter(|_| on_device) {
                        Some(d) => device_seam(d, h),
                        None => Ok(Box::new(ModelSeam::new())),
                    }
                };
                println!("selftest: [{seam_name}{}] {name}", if all { ", --all" } else { "" });
                let r = replay(&|| Ok(open(&bytes)), name, true, all, &mut make);
                let got = outcome(&r);
                let detail = match &r {
                    Ok(c) => format!("pass, compared {} reads ({} pages), {} captures", c.reads, c.pages, c.captures),
                    Err(f) => format!("{} (exit {}): {}", f.kind(), f.code(), f.text()),
                };
                let mut ok = got == expect;
                if name == "base trace" {
                    // The pass must have compared everything the trace holds: 2 reads, 3 pages, 1 capture.
                    ok &= matches!(&r, Ok(c) if (c.reads, c.pages, c.captures) == (2, 3, 1));
                }
                if expect == Expect::Mismatch && name.starts_with("corrupted READ page") {
                    // The comparison names the corrupted page (1) and word ((PAGE + 100 - PAGE) / 4 = 25).
                    ok &= matches!(&r, Err(Failure::Mismatch(t)) if t.contains("page 1:") && t.contains("word 25 "));
                }
                if expect == Expect::Mismatch && name.starts_with("reordered SUBMIT") {
                    ok &= matches!(&r, Err(Failure::Mismatch(t)) if t.contains("record 4 READ") && t.contains("page 0:"));
                }
                check(&format!("[{seam_name}{}] {name}", if all { ", --all" } else { "" }), ok,
                      format!("expected {expect:?}, got {detail}"));
            }
        }
    }
    // The file path the real run uses: a base trace through a temporary file.
    let dir = std::env::temp_dir().join(format!("optrace_replay_selftest_{}", std::process::id()));
    let file = dir.join("base.optrace");
    let written = std::fs::create_dir_all(&dir).and_then(|_| std::fs::write(&file, base_trace().0.sealed()));
    match written {
        Err(e) => check("file read", false, format!("cannot write {}: {e}", file.display())),
        Ok(()) => {
            let path = file.to_string_lossy().into_owned();
            let open = || -> Result<Box<dyn Read>, String> {
                File::open(&file).map(|f| Box::new(BufReader::new(f)) as Box<dyn Read>).map_err(|e| e.to_string())
            };
            let r = replay(&open, &path, false, false, &mut |_| Ok(Box::new(ModelSeam::new())));
            check("file read", outcome(&r) == Expect::Pass, format!("{r:?}"));
        }
    }
    let _ = std::fs::remove_dir_all(&dir);

    if failed == 0 {
        println!("selftest: PASS");
        0
    } else {
        println!("FAIL selftest: {failed} check(s) failed");
        1
    }
}

// ------------------------------------------------------------------------------------ main

const USAGE: &str = "usage: optrace_replay TRACE [--adapter hw|fallback|VVVV:DDDD] [--vulkan] [--fxc] [--dxc PATH] \
                     [--debug] [--validation] [--log-level 1|2|3] [--all]\n       optrace_replay --selftest [--gpu [device options]]";

fn refuse(text: &str) -> ! {
    eprintln!("optrace_replay: refused: {text}\n{USAGE}");
    std::process::exit(2);
}

fn main() {
    dhd_render_tools::link_host_log();
    // IEEE float control on this thread (every export sets it again inside its guard).
    dhd_render::guard::set_mxcsr(dhd_render::guard::ieee(dhd_render::guard::mxcsr()));

    let mut args = std::env::args().skip(1);
    let mut path: Option<String> = None;
    let (mut self_test, mut gpu, mut all) = (false, false, false);
    let mut d = DeviceArgs { log_level: DHDR_LOG_WARN, ..Default::default() };
    while let Some(a) = args.next() {
        let mut value = |name: &str| args.next().unwrap_or_else(|| refuse(&format!("{name} needs a value")));
        match a.as_str() {
            "--selftest" => self_test = true,
            "--gpu" => gpu = true,
            "--all" => all = true,
            "--vulkan" => d.flags |= DHDR_DEV_VULKAN,
            "--fxc" => d.flags |= DHDR_DEV_FXC,
            "--debug" => d.flags |= DHDR_DEV_DEBUG,
            "--validation" => d.flags |= DHDR_DEV_VALIDATION,
            "--dxc" => d.dxc = Some(value("--dxc")),
            "--log-level" => {
                d.log_level = match value("--log-level").as_str() {
                    "1" => DHDR_LOG_ERROR,
                    "2" => DHDR_LOG_WARN,
                    "3" => DHDR_LOG_INFO,
                    v => refuse(&format!("--log-level {v}")),
                }
            }
            "--adapter" => match value("--adapter").as_str() {
                "hw" => {}
                "fallback" => d.flags |= DHDR_DEV_FALLBACK,
                v => {
                    let ids = v.split_once(':').and_then(|(a, b)| {
                        Some((u32::from_str_radix(a.trim_start_matches("0x"), 16).ok()?,
                              u32::from_str_radix(b.trim_start_matches("0x"), 16).ok()?))
                    });
                    match ids {
                        Some((vendor, device)) => (d.vendor_id, d.device_id) = (vendor, device),
                        None => refuse(&format!("--adapter {v}: hw, fallback or VVVV:DDDD (hex PCI ids)")),
                    }
                }
            },
            s if s.starts_with("--") => refuse(&format!("unknown option {s}")),
            s => {
                if path.replace(s.to_owned()).is_some() {
                    refuse("more than one trace");
                }
            }
        }
    }
    if d.flags & DHDR_DEV_FXC != 0 && d.dxc.is_some() {
        refuse("--fxc and --dxc together");
    }
    if self_test {
        if path.is_some() {
            refuse("--selftest takes no trace");
        }
        std::process::exit(selftest(if gpu { Some(&d) } else { None }));
    }
    if gpu {
        refuse("--gpu is a --selftest option (a trace always replays on the device)");
    }
    let Some(path) = path else { refuse("no trace given") };
    let open = || -> Result<Box<dyn Read>, String> {
        File::open(&path)
            .map(|f| Box::new(BufReader::with_capacity(1 << 20, f)) as Box<dyn Read>)
            .map_err(|e| format!("cannot open {path}: {e}"))
    };
    let mut make = |h: &Header| device_seam(&d, h);
    let r = replay(&open, &path, false, all, &mut make);
    match r {
        Ok(c) => {
            println!("optrace_replay: PASS ({} reads, {} pages, {} captures byte-identical)", c.reads, c.pages, c.captures);
            std::process::exit(0);
        }
        Err(f) => {
            println!("FAIL optrace_replay: {}: {}", f.kind(), f.text());
            std::process::exit(f.code());
        }
    }
}
