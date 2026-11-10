//! The native arenas against the C code's numbers (task N2's referee).
//!
//!     python port/tools/rustgs.py test --target x86_64-pc-windows-msvc -- --test native_arenas
//!
//! `tests/data/native_arenas.json` is `gen/gen_native_tables.c` run over `port/gs/native_keys.inc`
//! (gpu_wgpu_native.inc's native_reserve_arenas, native_constants and native_upload; all three
//! retired, last at render/rust 3882d5576: the JSON stays as they recorded it): six command
//! lists, run in order through one set of arenas, with every constants offset, every upload's
//! (offset, bytes per row), and the sizes and buffers after each list. The lists are rebuilt here as
//! the generator builds them; the arenas only size (`reserve(None, ..)`), no device is needed.
//! The recorded draw constants are 112 bytes since finding 168 (the C generator's were 96; every
//! offset is 256-aligned, so only the byte counts changed); since finding 172 the renderer puts
//! the draws' constants in their own arena (`draw_constants`), and this replays them through the
//! constants arena as the generator did.
#![cfg(feature = "native")]

mod common;
use common::*;
use dhd_render::abi::*;
use dhd_render::native::arenas::{ArenaStats, Arenas, UNIFORM_WINDOW};

/// The generator's `command(kind, count, convert)`: every other field zero.
fn command(kind: u8, count: u32, convert: i16) -> DhdrCommand {
    // SAFETY: DhdrCommand is plain C data (integers and raw pointers); all-zero is valid.
    let mut k: DhdrCommand = unsafe { std::mem::zeroed() };
    k.kind = kind;
    k.count = count;
    k.convert = convert;
    k
}

/// The generator's `import(w, h)`: w x h zero pixels.
fn import(w: i32, h: i32, keep: &mut Vec<Vec<u32>>) -> DhdrCommand {
    let mut k = command(DHDR_CMD_IMPORT, 0, -1);
    k.w = w;
    k.h = h;
    keep.push(vec![0u32; (w * h) as usize]);
    k.pixels = keep.last().unwrap().as_ptr();
    k
}

/// (name, vertex count, commands), as gen_native_tables.c's write_arenas.
fn lists(keep: &mut Vec<Vec<u32>>) -> Vec<(&'static str, u32, Vec<DhdrCommand>)> {
    let draw = DHDR_CMD_DRAW;
    vec![
        ("empty", 0, vec![]),
        ("draws", 18, vec![command(draw, 6, -1), command(draw, 3, -1), command(draw, 9, -1)]),
        ("converted draws", 15, vec![command(draw, 3, 0), command(draw, 0, 2), command(draw, 12, 1)]),
        ("copy, import and alpha upload", 0,
         vec![command(DHDR_CMD_COPY, 0, -1), import(100, 3, keep), command(DHDR_CMD_ALPHA_UPLOAD, 0, -1)]),
        ("uniform arena growth", 420, (0..420).map(|_| command(draw, 1, 1)).collect()),
        ("upload, reversed and vertex growth", 300000,
         vec![import(512, 600, keep), command(DHDR_CMD_COPY, 0, -1), command(draw, 1000, -1)]),
    ]
}

/// The convert run's constants (native_core.inc's NativeConvertConstants).
const CONVERT_BYTES: usize = 16 + 16 + 64 + 512 + 1024;

#[test]
fn native_arenas() {
    let data = data("native_arenas.json");
    assert_eq!(data.get("uniform_window").u(), UNIFORM_WINDOW, "NATIVE_UNIFORM_WINDOW");
    let constants_bytes = data.get("constants_bytes").u() as usize;
    assert_eq!(constants_bytes, std::mem::size_of::<DhdrNativeConstants>(), "sizeof NativeConstants");
    assert_eq!(CONVERT_BYTES, std::mem::size_of::<DhdrConvertConstants>(), "sizeof NativeConvertConstants");
    let mut keep = Vec::new();
    let lists = lists(&mut keep);
    let want = data.get("lists").arr();
    assert_eq!(want.len(), lists.len());
    let convert = vec![0u8; CONVERT_BYTES];
    let constants = vec![0u8; constants_bytes];
    let alpha = vec![0u32; 64 * 32];
    let mut arenas = Arenas::new();
    for ((name, vertices, commands), w) in lists.iter().zip(want) {
        assert_eq!(w.get("name").str(), *name);
        assert_eq!(w.get("commands").u(), commands.len() as u64, "{name}");
        assert_eq!(w.get("vertices").u(), *vertices as u64, "{name}");
        arenas.reserve(None, commands, *vertices).unwrap_or_else(|e| panic!("{name}: reserve: {e}"));
        assert_eq!(arenas.stats().reversed_base as u64, w.get("reversed_base").u(), "{name}: reversed_base");

        // The calls a picture makes, in its order (the generator's comment).
        let mut offsets = w.get("offsets").arr().iter();
        let mut last_convert = -1i32;
        for (i, k) in commands.iter().enumerate() {
            if k.kind == DHDR_CMD_DRAW && k.count == 0 {
                continue;
            }
            let o = offsets.next().unwrap_or_else(|| panic!("{name}: command {i} is not in the data"));
            assert_eq!(o.get("command").u(), i as u64, "{name}");
            assert_eq!(o.get("kind").u(), k.kind as u64, "{name} command {i}");
            let mut got = Vec::new();
            match k.kind {
                DHDR_CMD_DRAW => {
                    if k.convert >= 0 && k.convert as i32 != last_convert {
                        got.push((arenas.constants(&convert), CONVERT_BYTES));
                    }
                    got.push((arenas.constants(&constants), constants_bytes));
                    last_convert = if k.convert >= 0 { k.convert as i32 } else { -1 };
                }
                DHDR_CMD_COPY => got.push((arenas.constants(&constants[..16]), 16)),
                _ => {}
            }
            let want_constants: Vec<(u32, usize)> = o.get("constants").arr().iter()
                .map(|c| (c.arr()[0].u() as u32, c.arr()[1].u() as usize)).collect();
            assert_eq!(got, want_constants, "{name} command {i}: constants (offset, bytes)");
            if k.kind == DHDR_CMD_IMPORT || k.kind == DHDR_CMD_ALPHA_UPLOAD {
                let (pixels, w_, h_) = if k.kind == DHDR_CMD_IMPORT {
                    // SAFETY: k.pixels points at keep's w x h pixels.
                    (unsafe { std::slice::from_raw_parts(k.pixels, (k.w * k.h) as usize) }, k.w as u32, k.h as u32)
                } else {
                    (&alpha[..], 64, 32)
                };
                let (offset, bpr) = arenas.upload(pixels, w_, h_);
                let u = o.get("upload").arr();
                assert_eq!((offset, bpr), (u[0].u(), u[1].u() as u32), "{name} command {i}: upload (offset, bytes per row)");
            } else {
                assert!(!o.has("upload"), "{name} command {i}");
            }
        }
        assert!(offsets.next().is_none(), "{name}: the data has more commands than were replayed");

        let buffers = w.get("buffers");
        let buffer = |label: &str| if buffers.has(label) { buffers.get(label).u() } else { 0 };
        let want_stats = ArenaStats {
            uniform_used: w.get("uniform_used").u(),
            upload_used: w.get("upload_used").u(),
            uniform_capacity: w.get("uniform_capacity").u(),
            upload_capacity: w.get("upload_capacity").u(),
            vertex_capacity: w.get("vertex_capacity").u(),
            reversed_capacity: w.get("reversed_capacity").u() as u32,
            reversed_base: w.get("reversed_base").u() as u32,
            uniform_buffer: buffer("native constants"),
            upload_buffer: buffer("native uploads"),
            vertex_buffer: buffer("native vertices"),
        };
        assert_eq!(buffers.obj().len(), ["native constants", "native uploads", "native vertices"].iter()
            .filter(|l| buffers.has(l)).count(), "{name}: only the three arena buffers");
        assert_eq!(arenas.stats(), want_stats, "{name}: the arenas after the list");
    }
}
