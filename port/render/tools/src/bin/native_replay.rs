//! native_replay: replays a `.ntrace` (port/render/NTRACE.md) through dhd_render's native renderer
//! and compares every picture with the C path's (task R1b).
//!
//! ```text
//! native_replay <trace.ntrace> [--picture N | --only no-draw|convert-scenes|draw] [--all]
//!               [--no-dual] [--dxc <dxcompiler.dll>] [--check]
//! native_replay --selftest
//! ```
//!
//! Exit 0: every selected picture equal, byte for byte. Exit 1: a difference, an invalid trace, or
//! no picture selected. Exit 2: refused (an unknown trace version, an adapter, driver, API or
//! compiler other than the trace's, a dual-source trace without dual-source blending, bad usage).
//! There is no tolerance option.
//!
//! Order (NTRACE.md, "Replay rule"):
//! 1. The whole trace is checked first (header, record order, sizes, counts, ids, END's record
//!    count, per-tag counts and FNV-1a 64), streaming, so nothing is drawn from an invalid trace.
//! 2. The device is made as the header says (API, PCI ids, fallback adapter, FXC or DXC) and must
//!    then match it exactly (API, vendor, device, driver version, compiler, fallback); otherwise
//!    both are printed and the replay is refused.
//! 3. Each selected picture is replayed on its own: a fresh `Native` (dual-source exactly when
//!    the header has `DUAL`), its surfaces made at their scale and size and filled with their
//!    `before` blobs, its textures made from their `TEXTURE` records (once per replay), the
//!    picture drawn as `dhdr_native_picture` draws it, then the result counters, the display
//!    pixels and every surface's contents compared with the trace's.
//!
//! `--check` stops after step 1 (it compares no picture and says so). `--selftest` runs the
//! replayer's own checks on synthetic traces with a small CPU model renderer standing in for the
//! device: the unmodified trace passes; a mutated picture (its display, a counter, a surface's
//! `after`, an input pixel) fails and names the picture, what differs, the first differing pixel
//! and the last command that wrote there; edited, reordered, truncated or extended traces fail
//! before anything is drawn; an adapter, driver, API, compiler or fallback mismatch and a
//! dual-source trace on a device without it are refused; selections pick the right pictures.
//!
//! The replayer runs with IEEE float control (MXCSR 0x1F80 class) on its one thread for every
//! device call (`guard::Guard`, as the renderer's exports do).

use dhd_render::abi::*;
use dhd_render_tools::{fnv1a, FNV_BASIS};
use std::collections::HashMap;
use std::io::{self, BufReader, Read, Seek, SeekFrom, Write};
use std::mem::{offset_of, size_of, MaybeUninit};

// ------------------------------------------------------------------------------------ format

const MAGIC: &[u8; 8] = b"DHDNTRC\0";
/// 2 since the command head is `DhdrCommand`'s current one (NTRACE.md, "Versions").
const VERSION: u32 = 2;
const NONE: u32 = 0xFFFF_FFFF;
/// `DhdrCommand`'s bytes before its pointers, taken from the struct itself (abi.rs pins its layout).
const COMMAND_HEAD: usize = offset_of!(DhdrCommand, texture);
const HEADER_FIXED: usize = 88;

const T_TEXTURE: u32 = 1;
const T_BLOB: u32 = 2;
const T_PICTURE: u32 = 3;
const T_SURFACES: u32 = 4;
const T_VERTICES: u32 = 5;
const T_CONVERTS: u32 = 6;
const T_COMMANDS: u32 = 7;
const T_MOVIE: u32 = 8;
const T_RESULT: u32 = 9;
const T_END: u32 = 15;
const END_BYTES: u32 = 16 + 16 * 4;

const PICTURE_FIXED: usize = 104;
const SURFACE_BYTES: usize = 40 + 4;
const CONVERT_BYTES: usize = 8 + 1632 + 4;

const _: () = {
    assert!(size_of::<DhdrCommand>() == COMMAND_HEAD + 3 * size_of::<*const u8>(), "three pointers end DhdrCommand");
    assert!(size_of::<DhdrSurfaceInfo>() == 40);
    assert!(size_of::<DhdrVertex>() == 32);
    assert!(size_of::<DhdrConvertConstants>() == 1632);
    assert!(size_of::<DhdrPictureResult>() == 80);
};

const EXIT_PASS: i32 = 0;
const EXIT_FAIL: i32 = 1;
const EXIT_REFUSED: i32 = 2;

/// Plain data: no padding (abi.rs asserts every layout) and every bit pattern a valid value.
///
/// # Safety
/// Only for `#[repr(C)]` types made of integers and floats with no implicit padding.
unsafe trait Pod: Copy + Default {}
// SAFETY: integer / float fields only, no padding (abi.rs layout asserts).
unsafe impl Pod for DhdrVertex {}
// SAFETY: as above.
unsafe impl Pod for DhdrSurfaceInfo {}
// SAFETY: as above.
unsafe impl Pod for DhdrPictureResult {}
// SAFETY: as above.
unsafe impl Pod for DhdrConvertConstants {}

fn pod_read<T: Pod>(b: &[u8]) -> T {
    assert_eq!(b.len(), size_of::<T>());
    let mut v = T::default();
    // SAFETY: T is Pod (any bytes are a valid value) and both regions are size_of::<T>() bytes.
    unsafe { std::ptr::copy_nonoverlapping(b.as_ptr(), &mut v as *mut T as *mut u8, size_of::<T>()) };
    v
}

fn pod_vec<T: Pod>(b: &[u8]) -> Vec<T> {
    b.chunks_exact(size_of::<T>()).map(pod_read).collect()
}

fn pod_bytes<T: Pod>(v: &T) -> &[u8] {
    // SAFETY: T is Pod: no padding, so every byte is initialised.
    unsafe { std::slice::from_raw_parts(v as *const T as *const u8, size_of::<T>()) }
}

/// A command from its `COMMAND_HEAD` head bytes; its pointers NULL.
fn command_from_head(b: &[u8]) -> DhdrCommand {
    assert_eq!(b.len(), COMMAND_HEAD);
    let mut k = MaybeUninit::<DhdrCommand>::zeroed();
    // SAFETY: the head holds only integers and floats (any pattern valid), the pointers stay
    // zeroed (NULL), and the copy stays inside the struct.
    unsafe {
        std::ptr::copy_nonoverlapping(b.as_ptr(), k.as_mut_ptr() as *mut u8, COMMAND_HEAD);
        k.assume_init()
    }
}

fn command_head(k: &DhdrCommand) -> &[u8] {
    // SAFETY: the first COMMAND_HEAD bytes are the pointer-free head, with no implicit padding (abi.rs).
    unsafe { std::slice::from_raw_parts(k as *const DhdrCommand as *const u8, COMMAND_HEAD) }
}

fn words(b: &[u8]) -> Vec<u32> {
    b.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect()
}

fn word_bytes(w: &[u32]) -> Vec<u8> {
    w.iter().flat_map(|v| v.to_le_bytes()).collect()
}

/// Little-endian reads from one record's payload.
struct Le<'a> {
    b: &'a [u8],
    at: usize,
}

impl<'a> Le<'a> {
    fn new(b: &'a [u8]) -> Le<'a> {
        Le { b, at: 0 }
    }
    fn take(&mut self, n: u64) -> Result<&'a [u8], String> {
        let left = (self.b.len() - self.at) as u64;
        if n > left {
            return Err(format!("the payload is {} bytes, too short for its contents", self.b.len()));
        }
        let s = &self.b[self.at..self.at + n as usize];
        self.at += n as usize;
        Ok(s)
    }
    fn u32(&mut self) -> Result<u32, String> {
        let s = self.take(4)?;
        Ok(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
    }
    fn i32(&mut self) -> Result<i32, String> {
        Ok(self.u32()? as i32)
    }
    fn i16(&mut self) -> Result<i16, String> {
        let s = self.take(2)?;
        Ok(i16::from_le_bytes([s[0], s[1]]))
    }
    fn u64(&mut self) -> Result<u64, String> {
        let s = self.take(8)?;
        Ok(u64::from_le_bytes(s.try_into().unwrap()))
    }
    fn left(&self) -> usize {
        self.b.len() - self.at
    }
    fn done(&self) -> Result<(), String> {
        if self.left() != 0 {
            return Err(format!("the payload has {} bytes more than its contents", self.left()));
        }
        Ok(())
    }
}

/// Bytes of `n` items of `size` bytes after a `fixed` part, or None on overflow.
fn sized(fixed: u64, n: u64, size: u64) -> Option<u64> {
    n.checked_mul(size)?.checked_add(fixed)
}

fn tag_name(tag: u32) -> &'static str {
    match tag {
        T_TEXTURE => "TEXTURE",
        T_BLOB => "BLOB",
        T_PICTURE => "PICTURE",
        T_SURFACES => "SURFACES",
        T_VERTICES => "VERTICES",
        T_CONVERTS => "CONVERTS",
        T_COMMANDS => "COMMANDS",
        T_MOVIE => "MOVIE",
        T_RESULT => "RESULT",
        T_END => "END",
        _ => "?",
    }
}

fn kind_name(kind: u8) -> String {
    match kind {
        DHDR_CMD_DRAW => "DRAW".into(),
        DHDR_CMD_COPY => "COPY".into(),
        DHDR_CMD_IMPORT => "IMPORT".into(),
        DHDR_CMD_ALPHA_UPLOAD => "ALPHA_UPLOAD".into(),
        k => format!("kind {k}"),
    }
}

/// The pixel bytes a command carries (the recorder's rule).
fn command_pixel_bytes(k: &DhdrCommand) -> u64 {
    match k.kind {
        DHDR_CMD_IMPORT if k.w > 0 && k.h > 0 => k.w as u64 * k.h as u64 * 4,
        DHDR_CMD_ALPHA_UPLOAD => 64 * 32 * 4,
        _ => 0,
    }
}

fn surface_used(s: &DhdrSurfaceInfo) -> bool {
    s.size[0] != 0 && s.size[1] != 0
}

fn surface_format(s: &DhdrSurfaceInfo) -> u32 {
    if s.depth != 0 {
        2
    } else {
        1
    }
}

// ------------------------------------------------------------------------------------ records

#[derive(Clone, Debug, Default, PartialEq, Eq)]
struct Header {
    version: u32,
    header_bytes: u32,
    flags: u32,
    api: u32,
    vendor_id: u32,
    device_id: u32,
    compiler: u32,
    fallback_adapter: u32,
    driver_version: u64,
    recorder_sha: String,
    adapter: String,
    source: String,
}

impl Header {
    fn dual(&self) -> bool {
        self.flags & 1 != 0
    }
    fn identity(&self) -> Identity {
        Identity {
            api: self.api,
            vendor_id: self.vendor_id,
            device_id: self.device_id,
            driver_version: self.driver_version,
            compiler: self.compiler,
            fallback_adapter: self.fallback_adapter,
        }
    }
}

/// What a replay device must share with the recording one (NTRACE.md replay rule 2).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
struct Identity {
    api: u32,
    vendor_id: u32,
    device_id: u32,
    driver_version: u64,
    compiler: u32,
    fallback_adapter: u32,
}

fn api_name(api: u32) -> String {
    match api {
        1 => "DX12".into(),
        2 => "Vulkan".into(),
        3 => "WebGPU".into(),
        a => format!("api {a}"),
    }
}

fn compiler_name(c: u32) -> String {
    match c {
        0 => "none".into(),
        1 => "FXC".into(),
        2 => "DXC".into(),
        c => format!("compiler {c}"),
    }
}

fn driver_text(api: u32, v: u64) -> String {
    if api == 1 {
        format!("{}.{}.{}.{} (0x{v:016x})", v >> 48, (v >> 32) & 0xFFFF, (v >> 16) & 0xFFFF, v & 0xFFFF)
    } else {
        format!("0x{v:x}")
    }
}

fn identity_text(i: &Identity, name: &str) -> String {
    format!("{} vendor 0x{:04x} device 0x{:04x} driver {} compiler {} fallback {} \"{}\"",
            api_name(i.api), i.vendor_id, i.device_id, driver_text(i.api, i.driver_version),
            compiler_name(i.compiler), i.fallback_adapter, name)
}

#[derive(Clone, Copy, Debug)]
struct TexMeta {
    kind: u32,
    w: u32,
    h: u32,
    levels: u32,
    /// File offset of the pixels.
    data: u64,
    bytes: u64,
}

#[derive(Clone, Copy, Debug)]
struct BlobMeta {
    format: u32,
    w: u32,
    h: u32,
    data: u64,
}

#[derive(Clone, Debug, Default)]
struct PicHead {
    index: u32,
    flags: u32,
    field: u64,
    scale: u32,
    display_surface: i32,
    display: [u32; 12],
    movie_on: u32,
    movie_width: u32,
    movie_height: u32,
    vertex_count: u32,
    command_count: u32,
    convert_count: u32,
    surface_count: u32,
    label: String,
}

#[derive(Clone, Copy, Debug)]
struct ConvertRec {
    surface: [i16; 4],
    constants: DhdrConvertConstants,
    /// TEXTURE id or NONE.
    base: u32,
}

#[derive(Clone, Debug)]
struct CommandRec {
    /// The head; its pointers NULL.
    head: DhdrCommand,
    /// TEXTURE ids or NONE.
    texture: u32,
    replacement: u32,
    pixels: Vec<u32>,
}

/// What a picture gives the renderer.
#[derive(Clone, Debug, Default)]
struct PictureInput {
    head: PicHead,
    surfaces: Vec<DhdrSurfaceInfo>,
    /// BLOB ids (NONE for unused surfaces).
    before: Vec<u32>,
    vertices: Vec<DhdrVertex>,
    converts: Vec<ConvertRec>,
    commands: Vec<CommandRec>,
    /// The movie frame (movie_on), RGBA8.
    movie: Vec<u32>,
}

/// What the C path got from it.
#[derive(Clone, Debug, Default)]
struct Expect {
    result: DhdrPictureResult,
    display: Vec<u8>,
    /// BLOB ids (NONE for unused surfaces).
    after: Vec<u32>,
}

fn decode_picture(p: &[u8]) -> Result<PicHead, String> {
    if p.len() < PICTURE_FIXED {
        return Err(format!("{} bytes, fewer than PICTURE's {PICTURE_FIXED}", p.len()));
    }
    let mut r = Le::new(p);
    let mut h = PicHead {
        index: r.u32()?,
        flags: r.u32()?,
        field: r.u64()?,
        scale: r.u32()?,
        display_surface: r.i32()?,
        ..Default::default()
    };
    for d in h.display.iter_mut() {
        *d = r.u32()?;
    }
    h.movie_on = r.u32()?;
    h.movie_width = r.u32()?;
    h.movie_height = r.u32()?;
    h.vertex_count = r.u32()?;
    h.command_count = r.u32()?;
    h.convert_count = r.u32()?;
    h.surface_count = r.u32()?;
    let label = r.u32()?;
    let label = r.take(label as u64)?;
    r.done()?;
    h.label = String::from_utf8(label.to_vec()).map_err(|_| "the picture label is not UTF-8".to_string())?;
    if h.flags & !1 != 0 {
        return Err(format!("picture_flags 0x{:x} has bits other than bit 0", h.flags));
    }
    if h.movie_on > 1 {
        return Err(format!("movie_on is {}", h.movie_on));
    }
    if h.surface_count > DHDR_NATIVE_SURFACES {
        return Err(format!("{} surfaces (at most {DHDR_NATIVE_SURFACES})", h.surface_count));
    }
    Ok(h)
}

fn decode_surfaces(p: &[u8]) -> Result<(Vec<DhdrSurfaceInfo>, Vec<u32>), String> {
    let mut r = Le::new(p);
    let n = r.u32()? as u64;
    if sized(4, n, SURFACE_BYTES as u64) != Some(p.len() as u64) {
        return Err(format!("{n} surfaces in a {}-byte payload", p.len()));
    }
    let (mut infos, mut ids) = (Vec::new(), Vec::new());
    for _ in 0..n {
        infos.push(pod_read::<DhdrSurfaceInfo>(r.take(40)?));
        ids.push(r.u32()?);
    }
    r.done()?;
    Ok((infos, ids))
}

fn decode_vertices(p: &[u8]) -> Result<Vec<DhdrVertex>, String> {
    let mut r = Le::new(p);
    let n = r.u32()? as u64;
    if sized(4, n, 32) != Some(p.len() as u64) {
        return Err(format!("{n} vertices in a {}-byte payload", p.len()));
    }
    let v = pod_vec(r.take(n * 32)?);
    r.done()?;
    Ok(v)
}

fn decode_converts(p: &[u8]) -> Result<Vec<ConvertRec>, String> {
    let mut r = Le::new(p);
    let n = r.u32()? as u64;
    if sized(4, n, CONVERT_BYTES as u64) != Some(p.len() as u64) {
        return Err(format!("{n} conversions in a {}-byte payload", p.len()));
    }
    let mut v = Vec::new();
    for _ in 0..n {
        let surface = [r.i16()?, r.i16()?, r.i16()?, r.i16()?];
        let constants = pod_read::<DhdrConvertConstants>(r.take(1632)?);
        v.push(ConvertRec { surface, constants, base: r.u32()? });
    }
    r.done()?;
    Ok(v)
}

fn decode_commands(p: &[u8]) -> Result<Vec<CommandRec>, String> {
    let mut r = Le::new(p);
    let n = r.u32()? as u64;
    if sized(4, n, COMMAND_HEAD as u64 + 12).is_none_or(|least| least > p.len() as u64) {
        return Err(format!("{n} commands in a {}-byte payload", p.len()));
    }
    let mut v = Vec::new();
    for i in 0..n {
        let head = command_from_head(r.take(COMMAND_HEAD as u64)?);
        let texture = r.u32()?;
        let replacement = r.u32()?;
        let bytes = r.u32()? as u64;
        let want = command_pixel_bytes(&head);
        if bytes != want {
            return Err(format!("command {i} ({}, {}x{}) carries {bytes} pixel bytes, not {want}",
                               kind_name(head.kind), head.w, head.h));
        }
        let pixels = words(r.take(bytes)?);
        v.push(CommandRec { head, texture, replacement, pixels });
    }
    r.done()?;
    Ok(v)
}

fn decode_movie(p: &[u8]) -> Result<(u32, u32, Vec<u32>), String> {
    let mut r = Le::new(p);
    let (w, h) = (r.u32()?, r.u32()?);
    if sized(8, w as u64 * h as u64, 4) != Some(p.len() as u64) {
        return Err(format!("a {w}x{h} movie frame in a {}-byte payload", p.len()));
    }
    let px = words(r.take(w as u64 * h as u64 * 4)?);
    r.done()?;
    Ok((w, h, px))
}

fn decode_result(p: &[u8]) -> Result<Expect, String> {
    let mut r = Le::new(p);
    let result = pod_read::<DhdrPictureResult>(r.take(80)?);
    if result.shown > 1 {
        return Err(format!("shown is {}", result.shown));
    }
    if result.shown == 0 && (result.display_width != 0 || result.display_height != 0) {
        return Err(format!("not shown, but the display is {}x{}", result.display_width, result.display_height));
    }
    let px = if result.shown == 1 {
        sized(0, result.display_width as u64 * result.display_height as u64, 4).ok_or("the display size overflows")?
    } else {
        0
    };
    let display = r.take(px)?.to_vec();
    let n = r.u32()? as u64;
    if r.left() as u64 != n * 4 {
        return Err(format!("{n} surfaces after, in {} bytes", r.left()));
    }
    let after = (0..n).map(|_| r.u32()).collect::<Result<Vec<_>, _>>()?;
    r.done()?;
    Ok(Expect { result, display, after })
}

// ------------------------------------------------------------------------------------ the trace

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Stage {
    Outside,
    Picture,
    Surfaces,
    Vertices,
    Converts,
    Commands,
    Movie,
}

/// Where a picture's records are (payload offset, bytes): PICTURE, SURFACES, VERTICES,
/// CONVERTS, COMMANDS, MOVIE, RESULT.
#[derive(Clone, Debug, Default)]
struct PicMeta {
    index: u32,
    label: String,
    field: u64,
    has_draw: bool,
    has_convert_draw: bool,
    at: [Option<(u64, u32)>; 7],
}

enum Problem {
    /// Not a valid trace: exit 1.
    Invalid(String),
    /// A trace this replayer must not judge: exit 2.
    Refuse(String),
}

impl From<io::Error> for Problem {
    fn from(e: io::Error) -> Problem {
        Problem::Invalid(format!("read failed: {e}"))
    }
}

struct Trace<R: Read + Seek> {
    src: R,
    header: Header,
    textures: Vec<TexMeta>,
    blobs: Vec<BlobMeta>,
    pictures: Vec<PicMeta>,
    records: u64,
}

fn invalid<T>(at: u64, what: impl std::fmt::Display) -> Result<T, Problem> {
    Err(Problem::Invalid(format!("at byte {at}: {what}")))
}

fn parse_header(b: &[u8], file_len: u64) -> Result<Header, Problem> {
    if b.len() < 16 || &b[0..8] != MAGIC {
        return Err(Problem::Invalid("not a .ntrace (no DHDNTRC magic)".into()));
    }
    let u = |o: usize| u32::from_le_bytes(b[o..o + 4].try_into().unwrap());
    let version = u(8);
    if version != VERSION {
        return Err(Problem::Refuse(format!("trace version {version}: this replayer knows version {VERSION} only")));
    }
    let header_bytes = u(12);
    if header_bytes % 4 != 0 || (header_bytes as usize) < HEADER_FIXED + 8 || header_bytes as u64 > file_len {
        return Err(Problem::Invalid(format!("header_bytes {header_bytes} (file {file_len} bytes)")));
    }
    if b.len() != header_bytes as usize {
        return Err(Problem::Invalid("the header is cut short".into()));
    }
    let flags = u(16);
    if flags & !1 != 0 {
        return Err(Problem::Invalid(format!("header flags 0x{flags:x} has bits other than DUAL")));
    }
    let sha = &b[48..88];
    if !sha.iter().all(|c| c.is_ascii_hexdigit()) {
        return Err(Problem::Invalid("recorder_sha is not 40 hex digits".into()));
    }
    let mut at = HEADER_FIXED;
    let mut text = |what: &str| -> Result<String, Problem> {
        if at + 4 > b.len() {
            return Err(Problem::Invalid(format!("the header ends inside its {what}")));
        }
        let n = u32::from_le_bytes(b[at..at + 4].try_into().unwrap()) as usize;
        at += 4;
        if n > b.len() - at {
            return Err(Problem::Invalid(format!("the {what} ({n} bytes) runs past header_bytes")));
        }
        let s = String::from_utf8(b[at..at + n].to_vec())
            .map_err(|_| Problem::Invalid(format!("the {what} is not UTF-8")))?;
        at += n;
        Ok(s)
    };
    let adapter = text("adapter name")?;
    let source = text("source")?;
    if b[at..].iter().any(|&c| c != 0) {
        return Err(Problem::Invalid("the header's padding is not zero".into()));
    }
    Ok(Header {
        version,
        header_bytes,
        flags,
        api: u(20),
        vendor_id: u(24),
        device_id: u(28),
        compiler: u(32),
        fallback_adapter: u(36),
        driver_version: u64::from_le_bytes(b[40..48].try_into().unwrap()),
        recorder_sha: String::from_utf8_lossy(sha).into_owned(),
        adapter,
        source,
    })
}

impl<R: Read + Seek> Trace<R> {
    /// Reads and checks the whole trace (NTRACE.md "END and integrity").
    fn open(mut src: R) -> Result<Trace<R>, Problem> {
        let len = src.seek(SeekFrom::End(0))?;
        src.seek(SeekFrom::Start(0))?;
        let mut first = [0u8; 16];
        if len < 16 {
            return Err(Problem::Invalid(format!("{len} bytes: not a .ntrace")));
        }
        src.read_exact(&mut first)?;
        // Magic and version first, so an unknown version is refused, not misread.
        if &first[0..8] != MAGIC {
            return Err(Problem::Invalid("not a .ntrace (no DHDNTRC magic)".into()));
        }
        let version = u32::from_le_bytes(first[8..12].try_into().unwrap());
        if version != VERSION {
            return Err(Problem::Refuse(format!("trace version {version}: this replayer knows version {VERSION} only")));
        }
        let header_bytes = u32::from_le_bytes(first[12..16].try_into().unwrap()) as usize;
        if header_bytes % 4 != 0 || header_bytes < HEADER_FIXED + 8 || header_bytes as u64 > len {
            return Err(Problem::Invalid(format!("header_bytes {header_bytes} (file {len} bytes)")));
        }
        let mut hb = vec![0u8; header_bytes];
        hb[..16].copy_from_slice(&first);
        src.read_exact(&mut hb[16..])?;
        let header = parse_header(&hb, len)?;

        let mut t = Trace { src, header, textures: Vec::new(), blobs: Vec::new(), pictures: Vec::new(), records: 0 };
        let mut pos = header_bytes as u64;
        let mut fnv = FNV_BASIS;
        let mut counts = [0u32; 16];
        let mut stage = Stage::Outside;
        let mut cur = PicMeta::default();
        let mut head = PicHead::default();
        let mut infos: Vec<DhdrSurfaceInfo> = Vec::new();
        let mut last_index: Option<u32> = None;
        let mut buf: Vec<u8> = Vec::new();
        loop {
            if pos + 8 > len {
                return invalid(pos, "no END record: the trace is truncated");
            }
            let mut rh = [0u8; 8];
            t.src.read_exact(&mut rh)?;
            let tag = u32::from_le_bytes(rh[0..4].try_into().unwrap());
            let bytes = u32::from_le_bytes(rh[4..8].try_into().unwrap());
            if tag == T_END {
                if bytes != END_BYTES {
                    return invalid(pos, format!("END has {bytes} payload bytes, not {END_BYTES}"));
                }
                if pos + 8 + END_BYTES as u64 > len {
                    return invalid(pos, "END is truncated");
                }
                let mut e = [0u8; END_BYTES as usize];
                t.src.read_exact(&mut e)?;
                if pos + 8 + END_BYTES as u64 != len {
                    return invalid(pos, format!("{} bytes after END", len - (pos + 8 + END_BYTES as u64)));
                }
                if stage != Stage::Outside {
                    return invalid(pos, format!("END inside picture {}", head.index));
                }
                let mut r = Le::new(&e);
                let (records, sum) = (r.u64().unwrap(), r.u64().unwrap());
                let want: Vec<u32> = (0..16).map(|_| r.u32().unwrap()).collect();
                if records != t.records {
                    return invalid(pos, format!("END counts {records} records, the trace has {}", t.records));
                }
                if want[..] != counts[..] {
                    return invalid(pos, format!("END's per-tag counts {want:?} differ from the trace's {counts:?}"));
                }
                if sum != fnv {
                    return invalid(pos, format!("FNV-1a 0x{fnv:016x} of the records, END says 0x{sum:016x}: an edited trace"));
                }
                return Ok(t);
            }
            if !(T_TEXTURE..=T_RESULT).contains(&tag) {
                return invalid(pos, format!("tag {tag} is not valid in version {VERSION}"));
            }
            let padded = (bytes as u64 + 3) & !3;
            if pos + 8 + padded > len {
                return invalid(pos, format!("{} record of {bytes} bytes runs past the end of the file", tag_name(tag)));
            }
            buf.resize(padded as usize, 0);
            t.src.read_exact(&mut buf)?;
            fnv = fnv1a(fnv1a(fnv, &rh), &buf);
            if buf[bytes as usize..].iter().any(|&c| c != 0) {
                return invalid(pos, format!("{} record's padding is not zero", tag_name(tag)));
            }
            t.records += 1;
            counts[tag as usize] += 1;
            let payload = &buf[..bytes as usize];
            let data = pos + 8;
            let at = Some((data, bytes));
            let ctx = format!("at byte {pos}: {} record{}", tag_name(tag),
                              if stage == Stage::Outside { String::new() } else { format!(" of picture {}", head.index) });
            let bad = |what: String| Problem::Invalid(format!("{ctx}: {what}"));
            let order = |stage: Stage, want: &[Stage]| -> Result<(), Problem> {
                if want.contains(&stage) {
                    Ok(())
                } else {
                    Err(Problem::Invalid(format!("at byte {pos}: a {} record out of order (after {stage:?})", tag_name(tag))))
                }
            };
            match tag {
                T_TEXTURE => {
                    let mut r = Le::new(payload);
                    let f = (|| Ok::<_, String>((r.u32()?, r.u32()?, r.u32()?, r.u32()?, r.u32()?)))();
                    let (id, kind, w, h, levels) = f.map_err(&bad)?;
                    if id != t.textures.len() as u32 {
                        return Err(bad(format!("id {id}, the next id is {} (ids count from 0, each defined once)", t.textures.len())));
                    }
                    let want = match kind {
                        1 if levels == 1 => (w as u64).checked_mul(h as u64).and_then(|n| n.checked_mul(4)).filter(|&n| n > 0),
                        2 => dhd_render::native::mip_bytes(w, h, levels).map(|b| b as u64),
                        _ => None,
                    };
                    match want {
                        Some(b) if b.checked_add(20) == Some(bytes as u64) => {}
                        _ => return Err(bad(format!("kind {kind} {w}x{h}, {levels} levels, in {bytes} bytes"))),
                    }
                    t.textures.push(TexMeta { kind, w, h, levels, data: data + 20, bytes: bytes as u64 - 20 });
                }
                T_BLOB => {
                    let mut r = Le::new(payload);
                    let f = (|| Ok::<_, String>((r.u32()?, r.u32()?, r.u32()?, r.u32()?)))();
                    let (id, format, w, h) = f.map_err(&bad)?;
                    if id != t.blobs.len() as u32 {
                        return Err(bad(format!("id {id}, the next id is {} (ids count from 0, each defined once)", t.blobs.len())));
                    }
                    if !(1..=2).contains(&format) || w == 0 || h == 0
                        || sized(16, w as u64 * h as u64, 4) != Some(bytes as u64) {
                        return Err(bad(format!("format {format} {w}x{h} in {bytes} bytes")));
                    }
                    t.blobs.push(BlobMeta { format, w, h, data: data + 16 });
                }
                T_PICTURE => {
                    order(stage, &[Stage::Outside])?;
                    head = decode_picture(payload).map_err(&bad)?;
                    if last_index.is_some_and(|l| head.index <= l) {
                        return Err(bad(format!("index {} after picture {}: indices must increase", head.index, last_index.unwrap())));
                    }
                    last_index = Some(head.index);
                    cur = PicMeta { index: head.index, label: head.label.clone(), field: head.field, ..Default::default() };
                    cur.at[0] = at;
                    stage = Stage::Picture;
                }
                T_SURFACES => {
                    order(stage, &[Stage::Picture])?;
                    let (inf, before) = decode_surfaces(payload).map_err(&bad)?;
                    if inf.len() as u32 != head.surface_count {
                        return Err(bad(format!("{} surfaces, PICTURE says {}", inf.len(), head.surface_count)));
                    }
                    for (i, (s, &b)) in inf.iter().zip(&before).enumerate() {
                        t.check_blob(s, b, i, "before").map_err(&bad)?;
                    }
                    infos = inf;
                    cur.at[1] = at;
                    stage = Stage::Surfaces;
                }
                T_VERTICES => {
                    order(stage, &[Stage::Surfaces])?;
                    let v = decode_vertices(payload).map_err(&bad)?;
                    if v.len() as u32 != head.vertex_count {
                        return Err(bad(format!("{} vertices, PICTURE says {}", v.len(), head.vertex_count)));
                    }
                    cur.at[2] = at;
                    stage = Stage::Vertices;
                }
                T_CONVERTS => {
                    order(stage, &[Stage::Vertices])?;
                    let v = decode_converts(payload).map_err(&bad)?;
                    if v.len() as u32 != head.convert_count {
                        return Err(bad(format!("{} conversions, PICTURE says {}", v.len(), head.convert_count)));
                    }
                    for (i, c) in v.iter().enumerate() {
                        t.check_texture(c.base, 1, &format!("conversion {i}'s base")).map_err(&bad)?;
                    }
                    cur.at[3] = at;
                    stage = Stage::Converts;
                }
                T_COMMANDS => {
                    order(stage, &[Stage::Converts])?;
                    let v = decode_commands(payload).map_err(&bad)?;
                    if v.len() as u32 != head.command_count {
                        return Err(bad(format!("{} commands, PICTURE says {}", v.len(), head.command_count)));
                    }
                    for (i, c) in v.iter().enumerate() {
                        t.check_texture(c.texture, 1, &format!("command {i}'s texture")).map_err(&bad)?;
                        t.check_texture(c.replacement, 2, &format!("command {i}'s replacement")).map_err(&bad)?;
                        if c.head.kind == DHDR_CMD_DRAW {
                            cur.has_draw = true;
                            if c.head.convert != DHDR_NO_INDEX {
                                cur.has_convert_draw = true;
                            }
                        }
                    }
                    cur.at[4] = at;
                    stage = Stage::Commands;
                }
                T_MOVIE => {
                    order(stage, &[Stage::Commands])?;
                    if head.movie_on == 0 {
                        return Err(bad("a MOVIE record in a picture without movie_on".into()));
                    }
                    let (w, h, _) = decode_movie(payload).map_err(&bad)?;
                    if (w, h) != (head.movie_width, head.movie_height) {
                        return Err(bad(format!("{w}x{h}, PICTURE says {}x{}", head.movie_width, head.movie_height)));
                    }
                    cur.at[5] = at;
                    stage = Stage::Movie;
                }
                T_RESULT => {
                    if head.movie_on == 1 {
                        order(stage, &[Stage::Movie])?;
                    } else {
                        order(stage, &[Stage::Commands])?;
                    }
                    let e = decode_result(payload).map_err(&bad)?;
                    if e.after.len() as u32 != head.surface_count {
                        return Err(bad(format!("{} surfaces after, PICTURE says {}", e.after.len(), head.surface_count)));
                    }
                    for (i, (s, &a)) in infos.iter().zip(&e.after).enumerate() {
                        t.check_blob(s, a, i, "after").map_err(&bad)?;
                    }
                    cur.at[6] = at;
                    t.pictures.push(std::mem::take(&mut cur));
                    stage = Stage::Outside;
                }
                _ => unreachable!(),
            }
            pos += 8 + padded;
        }
    }

    fn check_blob(&self, s: &DhdrSurfaceInfo, id: u32, i: usize, what: &str) -> Result<(), String> {
        if !surface_used(s) {
            if id != NONE {
                return Err(format!("surface {i} is {}x{} but has a {what} blob", s.size[0], s.size[1]));
            }
            return Ok(());
        }
        if id == NONE {
            return Err(format!("surface {i} ({}x{}) has no {what} blob", s.size[0], s.size[1]));
        }
        let Some(b) = self.blobs.get(id as usize) else {
            return Err(format!("surface {i}'s {what} blob {id} is not defined yet"));
        };
        if (b.w, b.h) != (s.size[0], s.size[1]) || b.format != surface_format(s) {
            return Err(format!("surface {i} is {}x{} format {}, its {what} blob {id} is {}x{} format {}",
                               s.size[0], s.size[1], surface_format(s), b.w, b.h, b.format));
        }
        Ok(())
    }

    fn check_texture(&self, id: u32, kind: u32, what: &str) -> Result<(), String> {
        if id == NONE {
            return Ok(());
        }
        match self.textures.get(id as usize) {
            None => Err(format!("{what} {id} is not defined yet")),
            Some(t) if t.kind != kind => Err(format!("{what} {id} is a kind {} texture, not kind {kind}", t.kind)),
            Some(_) => Ok(()),
        }
    }

    fn read_at(&mut self, offset: u64, bytes: u64) -> Result<Vec<u8>, String> {
        let mut v = vec![0u8; bytes as usize];
        self.src.seek(SeekFrom::Start(offset)).and_then(|_| self.src.read_exact(&mut v))
            .map_err(|e| format!("read failed: {e}"))?;
        Ok(v)
    }

    fn record(&mut self, at: Option<(u64, u32)>) -> Result<Vec<u8>, String> {
        let (o, b) = at.ok_or("a record is missing")?;
        self.read_at(o, b as u64)
    }

    /// Picture `i`'s records, decoded (they were checked by `open`).
    fn load(&mut self, i: usize) -> Result<(PictureInput, Expect), String> {
        let at = self.pictures[i].at;
        let head = decode_picture(&self.record(at[0])?)?;
        let (surfaces, before) = decode_surfaces(&self.record(at[1])?)?;
        let vertices = decode_vertices(&self.record(at[2])?)?;
        let converts = decode_converts(&self.record(at[3])?)?;
        let commands = decode_commands(&self.record(at[4])?)?;
        let movie = if head.movie_on == 1 { decode_movie(&self.record(at[5])?)?.2 } else { Vec::new() };
        let expect = decode_result(&self.record(at[6])?)?;
        Ok((PictureInput { head, surfaces, before, vertices, converts, commands, movie }, expect))
    }
}

/// Blob and texture contents by id, for a renderer.
trait Inputs {
    fn blob(&mut self, id: u32) -> Result<Vec<u8>, String>;
    fn texture(&mut self, id: u32) -> Result<(TexMeta, Vec<u8>), String>;
}

impl<R: Read + Seek> Inputs for Trace<R> {
    fn blob(&mut self, id: u32) -> Result<Vec<u8>, String> {
        let b = *self.blobs.get(id as usize).ok_or(format!("blob {id} is not in the trace"))?;
        self.read_at(b.data, b.w as u64 * b.h as u64 * 4)
    }
    fn texture(&mut self, id: u32) -> Result<(TexMeta, Vec<u8>), String> {
        let t = *self.textures.get(id as usize).ok_or(format!("texture {id} is not in the trace"))?;
        Ok((t, self.read_at(t.data, t.bytes)?))
    }
}

// ------------------------------------------------------------------------------------ renderers

/// What a renderer drew for one picture.
struct Drawn {
    result: DhdrPictureResult,
    /// The display picture, `display_width * display_height` RGBA8 words (empty when not shown).
    display: Vec<u32>,
    /// Per surface: its contents after the picture (None for unused surfaces).
    surfaces: Vec<Option<Vec<u8>>>,
}

/// The device a trace is replayed on.
trait Backend {
    fn identity(&self) -> Identity;
    fn adapter_name(&self) -> String;
    fn dual_source(&self) -> bool;
    /// Draws one picture on its own: fresh renderer state, the surfaces from their `before` blobs.
    fn draw(&mut self, p: &PictureInput, dual: bool, inputs: &mut dyn Inputs) -> Result<Drawn, String>;
}

/// dhd_render's native renderer on the process's device.
struct DeviceBackend {
    gpu: &'static dhd_render::device::Gpu,
    /// TEXTURE id -> handle (one reference held; released on drop).
    textures: HashMap<u32, *mut DhdrTexture>,
    _guard: dhd_render::guard::Guard,
}

/// The DX12 user-mode driver version from wgpu-hal's "a.b.c.d" (0 when it is not that).
fn parse_driver(s: &str) -> u64 {
    let parts: Vec<u64> = s.split('.').filter_map(|p| p.parse::<u16>().ok().map(u64::from)).collect();
    if parts.len() != 4 || s.split('.').count() != 4 {
        return 0;
    }
    parts[0] << 48 | parts[1] << 32 | parts[2] << 16 | parts[3]
}

impl DeviceBackend {
    /// The device as the header describes it (it is checked against the header afterwards).
    fn new(h: &Header, dxc: Option<String>) -> Result<DeviceBackend, String> {
        let guard = dhd_render::guard::Guard::enter("native_replay");
        let mut flags = 0;
        match h.api {
            1 => {}
            2 => flags |= DHDR_DEV_VULKAN,
            3 => return Err("a browser WebGPU trace (api 3) replays only in the browser".into()),
            a => return Err(format!("api {a} is unknown")),
        }
        if h.fallback_adapter != 0 {
            flags |= DHDR_DEV_FALLBACK;
        }
        let mut dxc_path = None;
        if h.api == 1 {
            match h.compiler {
                1 => flags |= DHDR_DEV_FXC,
                2 => {
                    let path = match dxc {
                        Some(p) => p,
                        None => std::env::current_exe().ok().and_then(|e| e.parent().map(|d| d.join("dxcompiler.dll")))
                            .filter(|p| p.is_file()).map(|p| p.to_string_lossy().into_owned())
                            .ok_or("the trace was recorded with DXC: pass --dxc <dxcompiler.dll> \
                                    (none beside native_replay.exe)")?,
                    };
                    if !std::path::Path::new(&path).is_file() {
                        return Err(format!("--dxc {path}: no such file"));
                    }
                    dxc_path = Some(path);
                }
                c => return Err(format!("the trace's DX12 compiler is {}: a replay cannot match it", compiler_name(c))),
            }
        }
        let o = dhd_render::device::DeviceOptions {
            flags,
            vendor_id: h.vendor_id,
            device_id: h.device_id,
            log_level: DHDR_LOG_WARN,
            dxc_path,
        };
        if !dhd_render::device::init(&o) {
            return Err("no device (the renderer logged why)".into());
        }
        let gpu = dhd_render::device::gpu().ok_or("no device after init")?;
        Ok(DeviceBackend { gpu, textures: HashMap::new(), _guard: guard })
    }

    fn handle(&mut self, id: u32, inputs: &mut dyn Inputs) -> Result<*mut DhdrTexture, String> {
        if id == NONE {
            return Ok(std::ptr::null_mut());
        }
        if let Some(&h) = self.textures.get(&id) {
            return Ok(h);
        }
        let (t, bytes) = inputs.texture(id)?;
        use dhd_render::native::textures::Tex;
        let tex = match t.kind {
            1 => Tex::create(self.gpu, &words(&bytes), t.w, t.h),
            _ => Tex::replace(self.gpu, &bytes, t.w, t.h, t.levels),
        }
        .map_err(|e| format!("texture {id} ({}x{}, kind {}): {e}", t.w, t.h, t.kind))?;
        let h = dhd_render::native::texture_handle(std::sync::Arc::new(tex));
        self.textures.insert(id, h);
        Ok(h)
    }
}

impl Drop for DeviceBackend {
    fn drop(&mut self) {
        for (_, h) in self.textures.drain() {
            // SAFETY: each handle came from texture_handle and this map holds its one reference.
            unsafe { dhd_render::native::texture_release(h) };
        }
    }
}

impl Backend for DeviceBackend {
    fn identity(&self) -> Identity {
        let info = &self.gpu.info;
        let api = match info.backend.to_str() {
            "dx12" => 1,
            "vulkan" => 2,
            "webgpu" => 3,
            _ => 0,
        };
        let o = &self.gpu.options;
        let compiler = if api != 1 {
            0
        } else if o.flags & DHDR_DEV_FXC != 0 || o.dxc_path.is_none() {
            1
        } else {
            2
        };
        Identity {
            api,
            vendor_id: info.vendor,
            device_id: info.device,
            driver_version: if api == 1 { parse_driver(&info.driver) } else { 0 },
            compiler,
            fallback_adapter: (format!("{:?}", info.device_type) == "Cpu") as u32,
        }
    }

    fn adapter_name(&self) -> String {
        self.gpu.info.name.clone()
    }

    fn dual_source(&self) -> bool {
        dhd_render::device::has_dual_source(self.gpu)
    }

    fn draw(&mut self, p: &PictureInput, dual: bool, inputs: &mut dyn Inputs) -> Result<Drawn, String> {
        use dhd_render::native::{Native, Picture};
        let mut n = Native::create(self.gpu, if dual { DHDR_NATIVE_DUAL } else { 0 })?;
        for (i, s) in p.surfaces.iter().enumerate() {
            if !surface_used(s) {
                continue;
            }
            let got = n.surface(i as u32, s.scale, s.size[0], s.size[1], s.depth != 0)
                .map_err(|e| format!("surface {i}: {e}"))?;
            if got != s.size {
                return Err(format!("surface {i}: made {}x{}, the trace's is {}x{}", got[0], got[1], s.size[0], s.size[1]));
            }
            let before = inputs.blob(p.before[i])?;
            n.surface_fill(i as u32, &before).map_err(|e| format!("surface {i}: {e}"))?;
        }
        let mut commands = Vec::with_capacity(p.commands.len());
        for c in &p.commands {
            let mut k = c.head;
            k.texture = self.handle(c.texture, inputs)?;
            k.replacement = self.handle(c.replacement, inputs)?;
            k.pixels = if c.pixels.is_empty() { std::ptr::null() } else { c.pixels.as_ptr() };
            commands.push(k);
        }
        let mut converts = Vec::with_capacity(p.converts.len());
        for c in &p.converts {
            converts.push(DhdrConvert { surface: c.surface, constants: c.constants, base: self.handle(c.base, inputs)? });
        }
        let h = &p.head;
        let pic = Picture {
            vertices: &p.vertices,
            commands: &commands,
            converts: &converts,
            surfaces: &p.surfaces,
            display_surface: h.display_surface,
            display: h.display,
            movie: if h.movie_on == 1 { Some((h.movie_width, h.movie_height, &p.movie[..])) } else { None },
            scale: h.scale,
        };
        let result = n.picture(&pic)?;
        let mut display = Vec::new();
        if result.shown == 1 {
            let (w, h) = n.read_begin().ok_or("the picture was shown, but read_begin has no display")?;
            display = vec![0u32; w as usize * h as usize];
            if !n.read_finish(&mut display) {
                return Err("the display did not read back".into());
            }
        }
        let mut surfaces = Vec::with_capacity(p.surfaces.len());
        for (i, s) in p.surfaces.iter().enumerate() {
            surfaces.push(if surface_used(s) {
                Some(n.surface_read(i as u32).map_err(|e| format!("surface {i}: {e}"))?)
            } else {
                None
            });
        }
        Ok(Drawn { result, display, surfaces })
    }
}

// ------------------------------------------------------------------------------------ compare

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Selection {
    All,
    Picture(u32),
    NoDraw,
    ConvertScenes,
    Draw,
}

#[derive(Clone, Debug)]
struct Options {
    selection: Selection,
    all: bool,
    no_dual: bool,
    check_only: bool,
}

impl Default for Options {
    fn default() -> Options {
        Options { selection: Selection::All, all: false, no_dual: false, check_only: false }
    }
}

fn selected(p: &PicMeta, s: Selection) -> bool {
    match s {
        Selection::All => true,
        Selection::Picture(n) => p.index == n,
        Selection::NoDraw => !p.has_draw,
        Selection::ConvertScenes => p.has_convert_draw,
        Selection::Draw => p.has_draw,
    }
}

#[derive(Debug, Default)]
struct Report {
    exit: i32,
    /// Pictures compared (drawn and judged).
    compared: usize,
    /// Pictures that differ.
    failed: usize,
    /// Pictures the backend was asked to draw.
    drawn: usize,
    /// Display pixels and surfaces compared.
    pixels: u64,
    surfaces: u64,
}

/// The last command that wrote surface `s`: a DRAW, COPY, IMPORT or ALPHA_UPLOAD with it as its
/// target (or, for a DRAW, its depth surface).
fn last_writer(p: &PictureInput, s: i32) -> String {
    let hit = p.commands.iter().enumerate().rev().find(|(_, c)| {
        let k = &c.head;
        matches!(k.kind, DHDR_CMD_DRAW | DHDR_CMD_COPY | DHDR_CMD_IMPORT | DHDR_CMD_ALPHA_UPLOAD)
            && (k.target as i32 == s || (k.kind == DHDR_CMD_DRAW && k.depth as i32 == s && s >= 0))
    });
    match hit {
        Some((i, c)) => format!("last command writing there: command {i} {}", kind_name(c.head.kind)),
        None => "no command of the picture writes there (its contents come from before the picture)".into(),
    }
}

/// `None` when equal; else (differing pixels, first index, got, want).
fn pixel_diff(got: &[u8], want: &[u8]) -> Option<(u64, usize, u32, u32)> {
    let mut first = None;
    let mut count = 0u64;
    for (i, (g, w)) in got.chunks_exact(4).zip(want.chunks_exact(4)).enumerate() {
        if g != w {
            count += 1;
            if first.is_none() {
                first = Some((i, u32::from_le_bytes(g.try_into().unwrap()), u32::from_le_bytes(w.try_into().unwrap())));
            }
        }
    }
    first.map(|(i, g, w)| (count, i, g, w))
}

fn value_text(v: u32, depth: bool) -> String {
    if depth {
        format!("0x{v:08x} ({})", f32::from_bits(v))
    } else {
        format!("0x{v:08x}")
    }
}

/// The differences of one picture, one line each (empty when equal).
fn compare(p: &PictureInput, e: &Expect, d: &Drawn, inputs: &mut dyn Inputs, r: &mut Report) -> Result<Vec<String>, String> {
    let mut out = Vec::new();
    let (g, w) = (&d.result, &e.result);
    let fields: [(&str, u64, u64); 11] = [
        ("shown", g.shown as u64, w.shown as u64),
        ("display_width", g.display_width as u64, w.display_width as u64),
        ("display_height", g.display_height as u64, w.display_height as u64),
        ("draws", g.draws, w.draws),
        ("self_copies", g.self_copies, w.self_copies),
        ("self_copy_px", g.self_copy_px, w.self_copy_px),
        ("copies", g.copies, w.copies),
        ("copy_px", g.copy_px, w.copy_px),
        ("imports", g.imports, w.imports),
        ("import_px", g.import_px, w.import_px),
        ("two_pass_draws", g.two_pass_draws, w.two_pass_draws),
    ];
    let diffs: Vec<String> =
        fields.iter().filter(|f| f.1 != f.2).map(|f| format!("{} got {} want {}", f.0, f.1, f.2)).collect();
    if !diffs.is_empty() {
        out.push(format!("result: {}", diffs.join(", ")));
    }
    if w.shown == 1 {
        let got = word_bytes(&d.display);
        r.pixels += w.display_width as u64 * w.display_height as u64;
        if got.len() != e.display.len() {
            out.push(format!("display: {} pixels read back, the trace has {}x{}", d.display.len(),
                             w.display_width, w.display_height));
        } else if let Some((n, i, gv, wv)) = pixel_diff(&got, &e.display) {
            let width = w.display_width.max(1) as usize;
            out.push(format!("display: {n} pixels differ; first at ({}, {}): got {} want {}; {}", i % width, i / width,
                             value_text(gv, false), value_text(wv, false), last_writer(p, p.head.display_surface)));
        }
    }
    for (i, s) in p.surfaces.iter().enumerate() {
        if !surface_used(s) {
            continue;
        }
        r.surfaces += 1;
        let want = inputs.blob(e.after[i])?;
        let depth = s.depth != 0;
        let kind = if depth { "depth" } else { "colour" };
        match d.surfaces.get(i).and_then(|x| x.as_ref()) {
            None => out.push(format!("surface {i} ({kind} {}x{}): not read back", s.size[0], s.size[1])),
            Some(got) if got.len() != want.len() => out.push(format!("surface {i} ({kind} {}x{}): {} bytes read back, not {}",
                                                                      s.size[0], s.size[1], got.len(), want.len())),
            Some(got) => {
                if let Some((n, k, gv, wv)) = pixel_diff(got, &want) {
                    let width = s.size[0] as usize;
                    out.push(format!("surface {i} ({kind} {}x{}): {n} pixels differ; first at ({}, {}): got {} want {}; {}",
                                     s.size[0], s.size[1], k % width, k / width, value_text(gv, depth),
                                     value_text(wv, depth), last_writer(p, i as i32)));
                }
            }
        }
    }
    Ok(out)
}

macro_rules! say {
    ($out:expr, $($a:tt)*) => {{ let _ = writeln!($out, $($a)*); }};
}

type MakeBackend<'a> = dyn FnMut(&Header) -> Result<Box<dyn Backend>, String> + 'a;

/// Checks, then replays and compares (NTRACE.md "Replay rule").
fn run<R: Read + Seek>(src: R, opt: &Options, make: &mut MakeBackend, out: &mut dyn Write) -> Report {
    let mut rep = Report { exit: EXIT_FAIL, ..Default::default() };
    let mut t = match Trace::open(src) {
        Ok(t) => t,
        Err(Problem::Invalid(m)) => {
            say!(out, "native_replay: FAIL invalid trace: {m}");
            return rep;
        }
        Err(Problem::Refuse(m)) => {
            say!(out, "native_replay: REFUSED: {m}");
            rep.exit = EXIT_REFUSED;
            return rep;
        }
    };
    let h = t.header.clone();
    say!(out, "native_replay: trace version {} ({}-byte header) from \"{}\", recorder {}, {}{}", h.version, h.header_bytes,
         h.source, h.recorder_sha,
         identity_text(&h.identity(), &h.adapter), if h.dual() { ", DUAL" } else { ", no DUAL" });
    say!(out, "native_replay: {} pictures, {} textures, {} blobs, {} records, integrity checked",
         t.pictures.len(), t.textures.len(), t.blobs.len(), t.records);
    if opt.check_only {
        say!(out, "native_replay: --check: the trace is valid; no picture was replayed or compared");
        rep.exit = EXIT_PASS;
        return rep;
    }
    if opt.no_dual && h.dual() {
        say!(out, "native_replay: REFUSED: --no-dual, but the trace was recorded with dual-source blending");
        rep.exit = EXIT_REFUSED;
        return rep;
    }
    let chosen: Vec<usize> = (0..t.pictures.len()).filter(|&i| selected(&t.pictures[i], opt.selection)).collect();
    if chosen.is_empty() {
        say!(out, "native_replay: FAIL compared 0 pictures: the selection {:?} matches no picture", opt.selection);
        return rep;
    }
    let mut b = match make(&h) {
        Ok(b) => b,
        Err(e) => {
            say!(out, "native_replay: REFUSED: no replay device as the trace needs: {e}");
            rep.exit = EXIT_REFUSED;
            return rep;
        }
    };
    let id = b.identity();
    if id != h.identity() {
        say!(out, "native_replay: REFUSED: the replay adapter differs from the trace's (native pictures are compared \
                   at 0 differing pixels, which holds for one adapter and driver)");
        say!(out, "  trace:  {}", identity_text(&h.identity(), &h.adapter));
        say!(out, "  replay: {}", identity_text(&id, &b.adapter_name()));
        rep.exit = EXIT_REFUSED;
        return rep;
    }
    if h.dual() && !b.dual_source() {
        say!(out, "native_replay: REFUSED: a DUAL trace, and the replay device has no dual-source blending");
        rep.exit = EXIT_REFUSED;
        return rep;
    }
    say!(out, "native_replay: replaying {} of {} pictures on {}", chosen.len(), t.pictures.len(), b.adapter_name());
    for &i in &chosen {
        let (index, label, field) = (t.pictures[i].index, t.pictures[i].label.clone(), t.pictures[i].field);
        let name = format!("picture {index} \"{label}\" (field {field})");
        let lines = match t.load(i) {
            Err(e) => vec![format!("the trace could not be read: {e}")],
            Ok((input, expect)) => {
                rep.drawn += 1;
                match b.draw(&input, h.dual(), &mut t) {
                    Err(e) => vec![format!("the renderer failed: {e}")],
                    Ok(d) => match compare(&input, &expect, &d, &mut t, &mut rep) {
                        Err(e) => vec![format!("the trace could not be read: {e}")],
                        Ok(l) => l,
                    },
                }
            }
        };
        rep.compared += 1;
        if !lines.is_empty() {
            rep.failed += 1;
            for l in &lines {
                say!(out, "native_replay: FAIL {name}: {l}");
            }
            if !opt.all {
                say!(out, "native_replay: FAIL stopped at the first differing picture ({} of {} selected compared; \
                           --all goes on)", rep.compared, chosen.len());
                return rep;
            }
        }
    }
    say!(out, "native_replay: compared {} pictures ({} display pixels, {} surfaces)", rep.compared, rep.pixels, rep.surfaces);
    if rep.failed > 0 {
        say!(out, "native_replay: FAIL {} of {} pictures differ", rep.failed, rep.compared);
    } else {
        rep.exit = EXIT_PASS;
    }
    rep
}

// ------------------------------------------------------------------------------------ main

const USAGE: &str = "usage: native_replay <trace.ntrace> [--picture N | --only no-draw|convert-scenes|draw] [--all] \
                     [--no-dual] [--dxc <dxcompiler.dll>] [--check]\n       native_replay --selftest";

fn main() {
    dhd_render_tools::link_host_log();
    let args: Vec<String> = std::env::args().skip(1).collect();
    let code = if args.len() == 1 && args[0] == "--selftest" {
        selftest::run_all()
    } else {
        match parse_args(&args) {
            Ok((path, opt, dxc)) => replay_file(&path, &opt, dxc),
            Err(e) => {
                eprintln!("native_replay: {e}\n{USAGE}");
                EXIT_REFUSED
            }
        }
    };
    let _ = io::stdout().flush();
    std::process::exit(code);
}

fn parse_args(args: &[String]) -> Result<(String, Options, Option<String>), String> {
    let mut opt = Options::default();
    let (mut path, mut dxc) = (None, None);
    let mut it = args.iter();
    let mut chose = false;
    while let Some(a) = it.next() {
        let mut choose = |s: Selection| -> Result<(), String> {
            if chose {
                return Err("one selection only (--picture or --only)".into());
            }
            chose = true;
            opt.selection = s;
            Ok(())
        };
        match a.as_str() {
            "--picture" => {
                let n = it.next().ok_or("--picture needs a number")?;
                choose(Selection::Picture(n.parse().map_err(|_| format!("--picture {n}: not a number"))?))?;
            }
            "--only" => match it.next().map(|s| s.as_str()) {
                Some("no-draw") => choose(Selection::NoDraw)?,
                Some("convert-scenes") => choose(Selection::ConvertScenes)?,
                Some("draw") => choose(Selection::Draw)?,
                o => return Err(format!("--only {}: not no-draw, convert-scenes or draw", o.unwrap_or(""))),
            },
            "--all" => opt.all = true,
            "--no-dual" => opt.no_dual = true,
            "--check" => opt.check_only = true,
            "--dxc" => dxc = Some(it.next().ok_or("--dxc needs a path")?.clone()),
            s if s.starts_with("--") => return Err(format!("unknown option {s}")),
            s => {
                if path.replace(s.to_string()).is_some() {
                    return Err("one trace only".into());
                }
            }
        }
    }
    Ok((path.ok_or("no trace given")?, opt, dxc))
}

fn replay_file(path: &str, opt: &Options, dxc: Option<String>) -> i32 {
    let f = match std::fs::File::open(path) {
        Ok(f) => f,
        Err(e) => {
            println!("native_replay: FAIL cannot open {path}: {e}");
            return EXIT_FAIL;
        }
    };
    let mut make = |h: &Header| -> Result<Box<dyn Backend>, String> {
        Ok(Box::new(DeviceBackend::new(h, dxc.clone())?) as Box<dyn Backend>)
    };
    let stdout = io::stdout();
    let mut out = stdout.lock();
    run(BufReader::with_capacity(1 << 20, f), opt, &mut make, &mut out).exit
}

// ------------------------------------------------------------------------------------ selftest

/// The replayer's own checks: synthetic traces written here as the recorder writes them, a small
/// CPU model renderer in place of the device, and mutations that must fail or be refused.
mod selftest {
    use super::*;
    use std::io::Cursor;

    /// A deterministic stand-in renderer: IMPORT and COPY move pixels, DRAW plots its vertices'
    /// colours on its target, the display is the movie frame or the display surface. Its output
    /// depends on the picture's inputs only (never on the trace's expected results).
    pub(super) struct Model {
        pub identity: Identity,
        pub dual: bool,
    }

    fn put(buf: &mut [u8], w: u32, h: u32, x: i32, y: i32, v: u32) {
        if x >= 0 && y >= 0 && (x as u32) < w && (y as u32) < h {
            let o = (y as usize * w as usize + x as usize) * 4;
            buf[o..o + 4].copy_from_slice(&v.to_le_bytes());
        }
    }

    fn get(buf: &[u8], w: u32, h: u32, x: i32, y: i32) -> Option<u32> {
        if x >= 0 && y >= 0 && (x as u32) < w && (y as u32) < h {
            let o = (y as usize * w as usize + x as usize) * 4;
            Some(u32::from_le_bytes(buf[o..o + 4].try_into().unwrap()))
        } else {
            None
        }
    }

    impl Backend for Model {
        fn identity(&self) -> Identity {
            self.identity.clone()
        }
        fn adapter_name(&self) -> String {
            "selftest model".into()
        }
        fn dual_source(&self) -> bool {
            self.dual
        }
        fn draw(&mut self, p: &PictureInput, _dual: bool, inputs: &mut dyn Inputs) -> Result<Drawn, String> {
            let mut s: Vec<Option<Vec<u8>>> = Vec::new();
            for (i, info) in p.surfaces.iter().enumerate() {
                s.push(if surface_used(info) { Some(inputs.blob(p.before[i])?) } else { None });
            }
            let size = |i: i16| p.surfaces.get(i as usize).map(|x| (x.size[0], x.size[1]));
            let mut r = DhdrPictureResult::default();
            for c in &p.commands {
                let k = &c.head;
                let Some((w, h)) = size(k.target) else { continue };
                match k.kind {
                    DHDR_CMD_IMPORT => {
                        r.imports += 1;
                        r.import_px += (k.w.max(0) * k.h.max(0)) as u64;
                        if let Some(t) = s[k.target as usize].as_mut() {
                            for y in 0..k.h.max(0) {
                                for x in 0..k.w.max(0) {
                                    put(t, w, h, k.x + x, k.y + y, c.pixels[(y * k.w + x) as usize]);
                                }
                            }
                        }
                    }
                    DHDR_CMD_COPY => {
                        let px = (k.w.max(0) * k.h.max(0)) as u64;
                        if k.source == k.target {
                            r.self_copies += 1;
                            r.self_copy_px += px;
                        } else {
                            r.copies += 1;
                            r.copy_px += px;
                        }
                        let Some((sw, sh)) = size(k.source) else { continue };
                        let Some(from) = s[k.source as usize].clone() else { continue };
                        if let Some(t) = s[k.target as usize].as_mut() {
                            for y in 0..k.h.max(0) {
                                for x in 0..k.w.max(0) {
                                    if let Some(v) = get(&from, sw, sh, k.sx + x, k.sy + y) {
                                        put(t, w, h, k.x + x, k.y + y, v);
                                    }
                                }
                            }
                        }
                    }
                    DHDR_CMD_DRAW => {
                        r.draws += 1;
                        if let Some(t) = s[k.target as usize].as_mut() {
                            let end = k.first as usize + k.count as usize;
                            for v in p.vertices.get(k.first as usize..end).unwrap_or(&[]) {
                                put(t, w, h, v.x as i32, v.y as i32, v.rgba);
                            }
                        }
                    }
                    _ => {}
                }
            }
            let mut display = Vec::new();
            let hd = &p.head;
            if hd.movie_on == 1 {
                (r.shown, r.display_width, r.display_height) = (1, hd.movie_width, hd.movie_height);
                display = p.movie.clone();
            } else if let Some(Some(d)) = (hd.display_surface >= 0).then(|| s.get(hd.display_surface as usize).cloned().flatten()) {
                let info = &p.surfaces[hd.display_surface as usize];
                (r.shown, r.display_width, r.display_height) = (1, info.size[0], info.size[1]);
                display = words(&d);
            }
            Ok(Drawn { result: r, display, surfaces: s })
        }
    }

    pub(super) fn encode_header(h: &Header) -> Vec<u8> {
        let fixed = HEADER_FIXED + 8 + h.adapter.len() + h.source.len();
        let total = (fixed + 3) & !3;
        let mut b = Vec::with_capacity(total);
        b.extend_from_slice(MAGIC);
        for v in [h.version, total as u32, h.flags, h.api, h.vendor_id, h.device_id, h.compiler, h.fallback_adapter] {
            b.extend_from_slice(&v.to_le_bytes());
        }
        b.extend_from_slice(&h.driver_version.to_le_bytes());
        let mut sha = h.recorder_sha.clone().into_bytes();
        sha.resize(40, b'0');
        b.extend_from_slice(&sha);
        b.extend_from_slice(&(h.adapter.len() as u32).to_le_bytes());
        b.extend_from_slice(h.adapter.as_bytes());
        b.extend_from_slice(&(h.source.len() as u32).to_le_bytes());
        b.extend_from_slice(h.source.as_bytes());
        b.resize(total, 0);
        b
    }

    /// A whole trace: the header bytes, the records, END (records, FNV, per-tag counts).
    pub(super) fn join(header: &[u8], records: &[(u32, Vec<u8>)]) -> Vec<u8> {
        let mut b = header.to_vec();
        let mut fnv = FNV_BASIS;
        let mut counts = [0u32; 16];
        for (tag, p) in records {
            let start = b.len();
            b.extend_from_slice(&tag.to_le_bytes());
            b.extend_from_slice(&(p.len() as u32).to_le_bytes());
            b.extend_from_slice(p);
            b.resize(b.len() + ((4 - p.len() % 4) % 4), 0);
            fnv = fnv1a(fnv, &b[start..]);
            counts[(*tag as usize).min(15)] += 1;
        }
        b.extend_from_slice(&T_END.to_le_bytes());
        b.extend_from_slice(&END_BYTES.to_le_bytes());
        b.extend_from_slice(&(records.len() as u64).to_le_bytes());
        b.extend_from_slice(&fnv.to_le_bytes());
        for c in counts {
            b.extend_from_slice(&c.to_le_bytes());
        }
        b
    }

    /// The header bytes and the records before END (no checks: for mutations).
    pub(super) fn split(t: &[u8]) -> (Vec<u8>, Vec<(u32, Vec<u8>)>) {
        let hb = u32::from_le_bytes(t[12..16].try_into().unwrap()) as usize;
        let mut v = Vec::new();
        let mut pos = hb;
        while pos + 8 <= t.len() {
            let tag = u32::from_le_bytes(t[pos..pos + 4].try_into().unwrap());
            let n = u32::from_le_bytes(t[pos + 4..pos + 8].try_into().unwrap()) as usize;
            if tag == T_END {
                break;
            }
            v.push((tag, t[pos + 8..pos + 8 + n].to_vec()));
            pos += 8 + ((n + 3) & !3);
        }
        (t[..hb].to_vec(), v)
    }

    /// Writes pictures as the recorder does, their results from the model.
    struct Gen {
        records: Vec<(u32, Vec<u8>)>,
        blobs: Vec<(u32, u32, u32, Vec<u8>)>,
        textures: Vec<(TexMeta, Vec<u8>)>,
    }

    impl Inputs for Gen {
        fn blob(&mut self, id: u32) -> Result<Vec<u8>, String> {
            self.blobs.get(id as usize).map(|b| b.3.clone()).ok_or(format!("blob {id}"))
        }
        fn texture(&mut self, id: u32) -> Result<(TexMeta, Vec<u8>), String> {
            self.textures.get(id as usize).cloned().ok_or(format!("texture {id}"))
        }
    }

    impl Gen {
        fn blob(&mut self, format: u32, w: u32, h: u32, bytes: Vec<u8>) -> u32 {
            if let Some(i) = self.blobs.iter().position(|b| b.0 == format && b.1 == w && b.2 == h && b.3 == bytes) {
                return i as u32;
            }
            let id = self.blobs.len() as u32;
            let mut p = Vec::new();
            for v in [id, format, w, h] {
                p.extend_from_slice(&v.to_le_bytes());
            }
            p.extend_from_slice(&bytes);
            self.records.push((T_BLOB, p));
            self.blobs.push((format, w, h, bytes));
            id
        }

        fn texture(&mut self, kind: u32, w: u32, h: u32, levels: u32, bytes: Vec<u8>) -> u32 {
            let id = self.textures.len() as u32;
            let mut p = Vec::new();
            for v in [id, kind, w, h, levels] {
                p.extend_from_slice(&v.to_le_bytes());
            }
            p.extend_from_slice(&bytes);
            self.records.push((T_TEXTURE, p));
            let bytes_len = bytes.len() as u64;
            self.textures.push((TexMeta { kind, w, h, levels, data: 0, bytes: bytes_len }, bytes));
            id
        }

        fn picture(&mut self, p: &PictureInput) {
            let h = &p.head;
            let mut b = Vec::new();
            for v in [h.index, h.flags] {
                b.extend_from_slice(&v.to_le_bytes());
            }
            b.extend_from_slice(&h.field.to_le_bytes());
            b.extend_from_slice(&h.scale.to_le_bytes());
            b.extend_from_slice(&h.display_surface.to_le_bytes());
            for v in h.display {
                b.extend_from_slice(&v.to_le_bytes());
            }
            for v in [h.movie_on, h.movie_width, h.movie_height, p.vertices.len() as u32, p.commands.len() as u32,
                      p.converts.len() as u32, p.surfaces.len() as u32, h.label.len() as u32] {
                b.extend_from_slice(&v.to_le_bytes());
            }
            b.extend_from_slice(h.label.as_bytes());
            self.records.push((T_PICTURE, b));
            let mut b = (p.surfaces.len() as u32).to_le_bytes().to_vec();
            for (s, id) in p.surfaces.iter().zip(&p.before) {
                b.extend_from_slice(pod_bytes(s));
                b.extend_from_slice(&id.to_le_bytes());
            }
            self.records.push((T_SURFACES, b));
            let mut b = (p.vertices.len() as u32).to_le_bytes().to_vec();
            for v in &p.vertices {
                b.extend_from_slice(pod_bytes(v));
            }
            self.records.push((T_VERTICES, b));
            let mut b = (p.converts.len() as u32).to_le_bytes().to_vec();
            for c in &p.converts {
                for s in c.surface {
                    b.extend_from_slice(&s.to_le_bytes());
                }
                b.extend_from_slice(pod_bytes(&c.constants));
                b.extend_from_slice(&c.base.to_le_bytes());
            }
            self.records.push((T_CONVERTS, b));
            let mut b = (p.commands.len() as u32).to_le_bytes().to_vec();
            for c in &p.commands {
                b.extend_from_slice(command_head(&c.head));
                for v in [c.texture, c.replacement, c.pixels.len() as u32 * 4] {
                    b.extend_from_slice(&v.to_le_bytes());
                }
                b.extend_from_slice(&word_bytes(&c.pixels));
            }
            self.records.push((T_COMMANDS, b));
            if h.movie_on == 1 {
                let mut b = Vec::new();
                for v in [h.movie_width, h.movie_height] {
                    b.extend_from_slice(&v.to_le_bytes());
                }
                b.extend_from_slice(&word_bytes(&p.movie));
                self.records.push((T_MOVIE, b));
            }
            let mut m = Model { identity: Identity::default(), dual: true };
            let d = m.draw(p, false, self).expect("the model draws every selftest picture");
            let mut after = Vec::new();
            for (s, c) in p.surfaces.iter().zip(&d.surfaces) {
                after.push(match c {
                    Some(c) => self.blob(surface_format(s), s.size[0], s.size[1], c.clone()),
                    None => NONE,
                });
            }
            let mut b = pod_bytes(&d.result).to_vec();
            b.extend_from_slice(&word_bytes(&d.display));
            b.extend_from_slice(&(after.len() as u32).to_le_bytes());
            for a in after {
                b.extend_from_slice(&a.to_le_bytes());
            }
            self.records.push((T_RESULT, b));
        }
    }

    pub(super) fn header() -> Header {
        Header {
            version: VERSION,
            header_bytes: 0,
            flags: 0,
            api: 1,
            vendor_id: 0x1234,
            device_id: 0x5678,
            compiler: 2,
            fallback_adapter: 0,
            driver_version: 0x0001_0002_0003_0004,
            recorder_sha: "0123456789abcdef0123456789abcdef01234567".into(),
            adapter: "selftest adapter".into(),
            source: "native_replay selftest".into(),
        }
    }

    fn pattern(seed: u32, n: usize) -> Vec<u8> {
        (0..n as u32).flat_map(|i| (i.wrapping_mul(0x9E37_79B9) ^ seed.wrapping_mul(0x85EB_CA6B)).to_le_bytes()).collect()
    }

    fn colour(w: u32, h: u32) -> DhdrSurfaceInfo {
        DhdrSurfaceInfo { page: 0, bw: 1, wide: 0, depth: 0, zpsm: 0, scale: 1, size: [w, h], extent: [w, h] }
    }

    fn command(kind: u8, target: i16) -> CommandRec {
        let mut head = command_from_head(&[0u8; COMMAND_HEAD]);
        head.kind = kind;
        head.target = target;
        head.depth = DHDR_NO_INDEX;
        head.texture_surface = DHDR_NO_INDEX;
        head.convert = DHDR_NO_INDEX;
        head.source = DHDR_NO_INDEX;
        CommandRec { head, texture: NONE, replacement: NONE, pixels: Vec::new() }
    }

    fn vertex(x: f32, y: f32, rgba: u32) -> DhdrVertex {
        DhdrVertex { x, y, z: 0.5, s: 0.0, t: 0.0, q: 1.0, fog: 0.0, rgba }
    }

    /// Three pictures (indices 0, 2, 3): IMPORT only (no draw); DRAW + self COPY with a texture;
    /// a DRAW with a conversion and a replacement, and a movie frame shown.
    pub(super) fn trace(h: &Header) -> Vec<u8> {
        let mut g = Gen { records: Vec::new(), blobs: Vec::new(), textures: Vec::new() };
        let mut depth = colour(4, 4);
        depth.depth = 1;
        depth.zpsm = 1;

        let a = g.blob(1, 8, 4, pattern(1, 32));
        let z = g.blob(2, 4, 4, pattern(2, 16));
        let mut imp = command(DHDR_CMD_IMPORT, 0);
        (imp.head.x, imp.head.y, imp.head.w, imp.head.h) = (1, 1, 3, 2);
        imp.pixels = (0..6).map(|i| 0xAA00_0000 | i).collect();
        let p0 = PictureInput {
            head: PicHead { index: 0, field: 100, scale: 1, display_surface: 0, label: "import".into(), ..Default::default() },
            surfaces: vec![colour(8, 4), DhdrSurfaceInfo::default(), depth],
            before: vec![a, NONE, z],
            commands: vec![imp],
            ..Default::default()
        };
        g.picture(&p0);

        let t0 = g.texture(1, 2, 2, 1, pattern(7, 4));
        let b = g.blob(1, 8, 4, pattern(3, 32));
        let mut draw = command(DHDR_CMD_DRAW, 0);
        (draw.head.depth, draw.head.first, draw.head.count) = (2, 0, 3);
        draw.texture = t0;
        let mut copy = command(DHDR_CMD_COPY, 0);
        (copy.head.source, copy.head.sx, copy.head.sy, copy.head.w, copy.head.h, copy.head.x, copy.head.y) = (0, 0, 0, 2, 1, 4, 2);
        let p1 = PictureInput {
            head: PicHead { index: 2, field: 101, scale: 1, display_surface: 0, label: "draw".into(), ..Default::default() },
            surfaces: vec![colour(8, 4), DhdrSurfaceInfo::default(), depth],
            before: vec![b, NONE, z],
            vertices: vec![vertex(0.0, 0.0, 0xFF00_00FF), vertex(2.0, 1.0, 0xFF00_FF00), vertex(7.0, 3.0, 0xFFFF_0000)],
            commands: vec![draw, copy],
            ..Default::default()
        };
        g.picture(&p1);

        let mips = dhd_render::native::mip_bytes(4, 4, 3).unwrap();
        let t1 = g.texture(2, 4, 4, 3, pattern(8, mips / 4));
        let c0 = g.blob(1, 8, 4, pattern(4, 32));
        let c1 = g.blob(1, 4, 4, pattern(5, 16));
        let mut conv = DhdrConvertConstants::default();
        conv.tex = [1, 2, 3, 4];
        conv.pages[7] = 1;
        let mut cd = command(DHDR_CMD_DRAW, 1);
        (cd.head.convert, cd.head.first, cd.head.count) = (0, 0, 2);
        cd.replacement = t1;
        let p2 = PictureInput {
            head: PicHead { index: 3, field: 102, scale: 1, display_surface: 1, movie_on: 1, movie_width: 2, movie_height: 2,
                            label: "convert".into(), ..Default::default() },
            surfaces: vec![colour(8, 4), colour(4, 4)],
            before: vec![c0, c1],
            vertices: vec![vertex(1.0, 1.0, 0x1122_3344), vertex(3.0, 2.0, 0x5566_7788)],
            converts: vec![ConvertRec { surface: [0, -1, -1, -1], constants: conv, base: t0 }],
            commands: vec![cd],
            movie: vec![0x0102_0304, 0x0506_0708, 0x090A_0B0C, 0x0D0E_0F10],
        };
        g.picture(&p2);
        join(&encode_header(h), &g.records)
    }

    fn nth(recs: &[(u32, Vec<u8>)], tag: u32, n: usize) -> usize {
        recs.iter().enumerate().filter(|r| r.1 .0 == tag).nth(n).expect("record").0
    }

    struct Case {
        name: &'static str,
        trace: Vec<u8>,
        opt: Options,
        model: Identity,
        dual: bool,
        exit: i32,
        /// Pictures the model must have drawn (None: any).
        drawn: Option<usize>,
        compared: Option<usize>,
        failed: Option<usize>,
        /// Text the output must hold.
        says: Vec<&'static str>,
    }

    pub(super) fn run_all() -> i32 {
        let h = header();
        let base = trace(&h);
        let ident = h.identity();
        let case = |name, trace: Vec<u8>, exit| Case {
            name, trace, opt: Options::default(), model: ident.clone(), dual: false, exit, drawn: None,
            compared: None, failed: None, says: Vec::new(),
        };
        let mutate = |f: &dyn Fn(&mut Vec<(u32, Vec<u8>)>)| {
            let (hb, mut recs) = split(&base);
            f(&mut recs);
            join(&hb, &recs)
        };
        let mut cases = Vec::new();

        // The unmodified trace passes; selections pick the right pictures.
        let mut c = case("baseline", base.clone(), EXIT_PASS);
        (c.drawn, c.compared, c.failed) = (Some(3), Some(3), Some(0));
        c.says = vec!["compared 3 pictures (", "3 pictures, 2 textures"];
        cases.push(c);
        let mut c = case("check only", base.clone(), EXIT_PASS);
        c.opt.check_only = true;
        (c.drawn, c.says) = (Some(0), vec!["no picture was replayed"]);
        cases.push(c);
        for (name, sel, n) in [("only no-draw", Selection::NoDraw, 1), ("only draw", Selection::Draw, 2),
                               ("only convert-scenes", Selection::ConvertScenes, 1), ("picture 3", Selection::Picture(3), 1)] {
            let mut c = case(name, base.clone(), EXIT_PASS);
            c.opt.selection = sel;
            (c.drawn, c.compared) = (Some(n), Some(n));
            cases.push(c);
        }
        let mut c = case("empty selection fails", base.clone(), EXIT_FAIL);
        c.opt.selection = Selection::Picture(1);
        (c.drawn, c.says) = (Some(0), vec!["compared 0 pictures"]);
        cases.push(c);

        // Mutated pictures (FNV and END fixed up, so only the comparison can catch them).
        let display = mutate(&|r| {
            let i = nth(r, T_RESULT, 1);
            r[i].1[80 + (8 + 1) * 4 + 1] ^= 0x40;
        });
        let mut c = case("mutated display fails", display, EXIT_FAIL);
        (c.compared, c.failed) = (Some(2), Some(1));
        c.says = vec!["FAIL picture 2 \"draw\"", "display: 1 pixels differ; first at (1, 1)", "command 1 COPY",
                      "stopped at the first differing picture"];
        cases.push(c);
        let counter = mutate(&|r| {
            let i = nth(r, T_RESULT, 2);
            r[i].1[16] ^= 1;
        });
        let mut c = case("mutated counter fails", counter, EXIT_FAIL);
        (c.compared, c.failed) = (Some(3), Some(1));
        c.says = vec!["FAIL picture 3 \"convert\"", "result: draws got 1 want 0"];
        cases.push(c);
        let input = mutate(&|r| {
            let i = nth(r, T_COMMANDS, 0);
            r[i].1[4 + COMMAND_HEAD + 12] ^= 0x01;
        });
        let mut c = case("mutated input pixel fails", input, EXIT_FAIL);
        (c.compared, c.failed) = (Some(1), Some(1));
        c.says = vec!["FAIL picture 0 \"import\"", "surface 0 (colour 8x4): 1 pixels differ; first at (1, 1)",
                      "command 0 IMPORT", "display: 1 pixels differ"];
        cases.push(c);
        let after = mutate(&|r| {
            // A blob only an `after` refers to: picture 3's surface 1.
            let res = nth(r, T_RESULT, 2);
            let p = &r[res].1;
            let n = p.len();
            let id = u32::from_le_bytes(p[n - 4..].try_into().unwrap());
            let i = r.iter().position(|x| x.0 == T_BLOB && x.1[0..4] == id.to_le_bytes()).unwrap();
            r[i].1[16 + 4 * 6] ^= 0x80;
        });
        let mut c = case("mutated after surface fails", after, EXIT_FAIL);
        c.failed = Some(1);
        c.says = vec!["FAIL picture 3", "surface 1 (colour 4x4): 1 pixels differ; first at (2, 1)", "command 0 DRAW"];
        cases.push(c);
        let two = mutate(&|r| {
            let i = nth(r, T_RESULT, 1);
            r[i].1[80] ^= 1;
            let i = nth(r, T_RESULT, 2);
            r[i].1[16] ^= 1;
        });
        let mut c = case("--all reports every differing picture", two, EXIT_FAIL);
        c.opt.all = true;
        (c.compared, c.failed) = (Some(3), Some(2));
        c.says = vec!["FAIL 2 of 3 pictures differ"];
        cases.push(c);

        // Invalid traces fail before anything is drawn.
        let mut raw = base.clone();
        let mid = encode_header(&h).len() + 30;
        raw[mid] ^= 0x10;
        let mut c = case("edited byte (FNV) fails", raw, EXIT_FAIL);
        (c.drawn, c.says) = (Some(0), vec!["invalid trace"]);
        cases.push(c);
        let reorder = mutate(&|r| {
            let (a, b) = (nth(r, T_SURFACES, 0), nth(r, T_VERTICES, 0));
            r.swap(a, b);
        });
        let mut c = case("reordered records fail", reorder, EXIT_FAIL);
        (c.drawn, c.says) = (Some(0), vec!["out of order"]);
        cases.push(c);
        let dropped = mutate(&|r| {
            let i = nth(r, T_RESULT, 2);
            r.remove(i);
        });
        let mut c = case("dropped record fails", dropped, EXIT_FAIL);
        c.drawn = Some(0);
        cases.push(c);
        let undefined = mutate(&|r| {
            let i = nth(r, T_COMMANDS, 1);
            r[i].1[4 + COMMAND_HEAD..4 + COMMAND_HEAD + 4].copy_from_slice(&999u32.to_le_bytes());
        });
        let mut c = case("undefined texture id fails", undefined, EXIT_FAIL);
        (c.drawn, c.says) = (Some(0), vec!["not defined yet"]);
        cases.push(c);
        let wrong_count = mutate(&|r| {
            let i = nth(r, T_PICTURE, 1);
            r[i].1[88] += 1; // command_count
        });
        let mut c = case("count disagreeing with PICTURE fails", wrong_count, EXIT_FAIL);
        (c.drawn, c.says) = (Some(0), vec!["PICTURE says"]);
        cases.push(c);
        let mut c = case("truncated trace fails", base[..base.len() - 10].to_vec(), EXIT_FAIL);
        c.drawn = Some(0);
        cases.push(c);
        let mut longer = base.clone();
        longer.push(0);
        let mut c = case("bytes after END fail", longer, EXIT_FAIL);
        (c.drawn, c.says) = (Some(0), vec!["after END"]);
        cases.push(c);
        let mut magic = base.clone();
        magic[0] = b'X';
        let mut c = case("bad magic fails", magic, EXIT_FAIL);
        c.drawn = Some(0);
        cases.push(c);

        // Refusals.
        let mut v1 = base.clone();
        v1[8] = 1;
        let mut c = case("old version 1 (168-byte command head) refused", v1, EXIT_REFUSED);
        (c.drawn, c.says) = (Some(0), vec!["REFUSED: trace version 1"]);
        cases.push(c);
        let mut v3 = base.clone();
        v3[8] = 3;
        let mut c = case("unknown version refused", v3, EXIT_REFUSED);
        (c.drawn, c.says) = (Some(0), vec!["REFUSED: trace version 3"]);
        cases.push(c);
        let tweaks: [(&'static str, fn(&mut Identity)); 6] = [
            ("adapter (vendor) mismatch refused", |i| i.vendor_id ^= 1),
            ("adapter (device) mismatch refused", |i| i.device_id ^= 1),
            ("driver mismatch refused", |i| i.driver_version += 1),
            ("API mismatch refused", |i| i.api = 2),
            ("compiler mismatch refused", |i| i.compiler = 1),
            ("fallback adapter mismatch refused", |i| i.fallback_adapter = 1),
        ];
        for (name, f) in tweaks {
            let mut c = case(name, base.clone(), EXIT_REFUSED);
            f(&mut c.model);
            (c.drawn, c.says) = (Some(0), vec!["REFUSED: the replay adapter differs", "  trace:  ", "  replay: "]);
            cases.push(c);
        }
        let mut hd = h.clone();
        hd.flags = 1;
        let dual = trace(&hd);
        let mut c = case("DUAL trace without dual-source refused", dual.clone(), EXIT_REFUSED);
        (c.drawn, c.says) = (Some(0), vec!["no dual-source blending"]);
        cases.push(c);
        let mut c = case("DUAL trace with dual-source passes", dual.clone(), EXIT_PASS);
        (c.dual, c.drawn) = (true, Some(3));
        cases.push(c);
        let mut c = case("--no-dual on a DUAL trace refused", dual, EXIT_REFUSED);
        (c.dual, c.drawn) = (true, Some(0));
        c.opt.no_dual = true;
        cases.push(c);

        let mut failures = 0;
        for c in &cases {
            let mut out = Vec::new();
            let (model, dual) = (c.model.clone(), c.dual);
            let mut make = |_: &Header| -> Result<Box<dyn Backend>, String> {
                Ok(Box::new(Model { identity: model.clone(), dual }) as Box<dyn Backend>)
            };
            let r = run(Cursor::new(c.trace.clone()), &c.opt, &mut make, &mut out);
            let text = String::from_utf8_lossy(&out).into_owned();
            let mut wrong = Vec::new();
            if r.exit != c.exit {
                wrong.push(format!("exit {} (want {})", r.exit, c.exit));
            }
            for (what, got, want) in [("drawn", r.drawn, c.drawn), ("compared", r.compared, c.compared), ("failed", r.failed, c.failed)] {
                if let Some(w) = want {
                    if got != w {
                        wrong.push(format!("{what} {got} (want {w})"));
                    }
                }
            }
            for s in &c.says {
                if !text.contains(s) {
                    wrong.push(format!("output lacks {s:?}"));
                }
            }
            if wrong.is_empty() {
                println!("selftest: ok   {} (exit {})", c.name, r.exit);
            } else {
                failures += 1;
                println!("selftest: FAIL {}: {}", c.name, wrong.join("; "));
                for l in text.lines() {
                    println!("    | {l}");
                }
            }
        }
        if failures == 0 {
            println!("selftest: {} cases passed", cases.len());
            EXIT_PASS
        } else {
            println!("selftest: FAIL {failures} of {} cases", cases.len());
            EXIT_FAIL
        }
    }
}
