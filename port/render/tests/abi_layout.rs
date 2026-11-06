//! The C ABI's layout, at run time on the host.
//!
//! `src/abi.rs` asserts every size and offset at compile time; this test checks the properties the
//! header promises on top of that, for the target it runs on:
//! - no struct has implicit padding (the fields tile the struct exactly, in declaration order),
//!   so C can zero a struct field by field and a trace can store its bytes;
//! - the pointer members come last, and the bytes before them are the same on every target;
//! - the constants have the header's values, and the C-mirrored blocks have the sizes their C
//!   twins (native_core.inc) have.
//! The comparison with the C compiler's view of `include/dhd_render.h`, for i686, x86_64 and
//! wasm32, is `port/tools/test_dhdr_abi.py`.

#[path = "../src/abi.rs"]
#[allow(unused)]
mod abi;

use abi::*;
use core::mem::{align_of, offset_of, size_of};

/// One field: name, offset, size.
type Field = (&'static str, usize, usize);

macro_rules! fields {
    ($t:ty { $($f:ident : $ft:ty),* $(,)? }) => {
        (stringify!($t), size_of::<$t>(), align_of::<$t>(), vec![$((stringify!($f), offset_of!($t, $f), size_of::<$ft>())),*])
    };
}

const PTR: usize = size_of::<*const u8>();

fn all() -> Vec<(&'static str, usize, usize, Vec<Field>)> {
    vec![
        fields!(DhdrDeviceDesc { size: u32, abi: u32, flags: u32, vendor_id: u32, device_id: u32, log_level: u32,
                                 dxc_path: *const u8 }),
        fields!(DhdrExactStats { submits: u64, reads: u64, read_ticks: u64, dispatches: u64, bytes_written: u64,
                                 copies: u64 }),
        fields!(DhdrWindow { kind: u32, reserved: u32, a: u64, b: u64 }),
        fields!(DhdrVertex { x: f32, y: f32, z: f32, s: f32, t: f32, q: f32, fog: f32, rgba: u32 }),
        fields!(DhdrNativeConstants { target: [f32; 2], flags: u32, atst: u32, aref: u32, alpha_scale: f32,
                                      texa_alpha: f32, scale: u32, fog: [f32; 4], tex_offset: [i32; 2],
                                      tex_size: [u32; 2], wrap: [u32; 4], region: [u32; 4],
                                      mesh_offset: [f32; 4] }),
        fields!(DhdrConvertConstants { tex: [u32; 4], size: [u32; 4], slot: [[u32; 4]; 4], pages: [u8; 512],
                                       palette: [u32; 256] }),
        fields!(DhdrCommand { kind: u8, depth_key: u8, color_mask: u8, pad: u8, target: i16, depth: i16,
                              texture_surface: i16, convert: i16, blend_key: u32, blend_fix: u32,
                              scissor: [i32; 4], constants: DhdrNativeConstants, first: u32, count: u32,
                              source: i16, pad2: i16, x: i32, y: i32, w: i32, h: i32, sx: i32, sy: i32,
                              texture: *mut DhdrTexture, replacement: *mut DhdrTexture, pixels: *const u32 }),
        fields!(DhdrConvert { surface: [i16; 4], constants: DhdrConvertConstants, base: *mut DhdrTexture }),
        fields!(DhdrSurfaceInfo { page: u32, bw: u32, wide: u32, depth: u32, zpsm: u32, scale: u32,
                                  size: [u32; 2], extent: [u32; 2] }),
        fields!(DhdrPicture { size: u32, vertex_count: u32, command_count: u32, convert_count: u32,
                              surface_count: u32, display_surface: i32, display: [u32; 12], movie_width: u32,
                              movie_height: u32, movie_on: u32, scale: u32, vertices: *const DhdrVertex,
                              commands: *const DhdrCommand, converts: *const DhdrConvert,
                              surfaces: *const DhdrSurfaceInfo, movie: *const u32 }),
        fields!(DhdrPictureResult { shown: u32, display_width: u32, display_height: u32, pad: u32, draws: u64,
                                    self_copies: u64, self_copy_px: u64, copies: u64, copy_px: u64, imports: u64,
                                    import_px: u64, two_pass_draws: u64 }),
        fields!(DhdrPresentRect { rect: [f32; 4], cut: [i32; 4], window_w: u32, window_h: u32, resize: u32,
                                  fill: u32 }),
    ]
}

#[test]
fn every_field_listed_and_no_implicit_padding() {
    for (name, size, align, fields) in all() {
        let mut at = 0;
        for (field, offset, bytes) in &fields {
            assert_eq!(*offset, at, "{name}.{field}: a hole before it (expected offset {at}, got {offset})");
            at += bytes;
        }
        assert_eq!(at, size, "{name}: the fields cover {at} of {size} bytes (a hole at the end, or a field not listed)");
        assert!(size % align == 0, "{name}: size {size} is not a multiple of its alignment {align}");
    }
}

#[test]
fn pointers_come_last_and_the_prefix_is_target_independent() {
    // The pointer-free prefix of each struct with pointers, the same on every target.
    let prefix = [("DhdrDeviceDesc", 24usize, 1usize), ("DhdrCommand", 296, 3), ("DhdrConvert", 1640, 1),
                  ("DhdrPicture", 88, 5)];
    for (name, size, _align, fields) in all() {
        let first_ptr = fields.iter().position(|f| f.0 == "dxc_path" || f.0 == "texture" || f.0 == "base"
                                                      || f.0 == "vertices");
        match prefix.iter().find(|p| p.0 == name) {
            Some(&(_, bytes, count)) => {
                let i = first_ptr.unwrap_or_else(|| panic!("{name}: no pointer member found"));
                assert_eq!(fields[i].1, bytes, "{name}: pointers start at {} not {bytes}", fields[i].1);
                assert_eq!(fields.len() - i, count, "{name}: {} trailing members, {count} pointers expected",
                           fields.len() - i);
                for f in &fields[i..] {
                    assert_eq!(f.2, PTR, "{name}.{}: not pointer-sized", f.0);
                }
                assert_eq!(size, bytes + count * PTR, "{name}: size");
                assert_eq!(bytes % 8, 0, "{name}: pointers must start 8-aligned on every target");
            }
            None => assert!(first_ptr.is_none(), "{name}: has a pointer member but no prefix entry"),
        }
    }
}

#[test]
fn sizes_the_header_documents() {
    assert_eq!(size_of::<DhdrVertex>(), 32);
    assert_eq!(size_of::<DhdrNativeConstants>(), 224);
    assert_eq!(size_of::<DhdrConvertConstants>(), 1632);
    assert_eq!(size_of::<DhdrSurfaceInfo>(), 40);
    assert_eq!(size_of::<DhdrWindow>(), 24);
    assert_eq!(size_of::<DhdrExactStats>(), 48);
    assert_eq!(size_of::<DhdrPictureResult>(), 80);
    assert_eq!(size_of::<DhdrPresentRect>(), 48);
    assert_eq!(size_of::<DhdrDeviceDesc>(), 24 + PTR);
    assert_eq!(size_of::<DhdrCommand>(), 296 + 3 * PTR);
    assert_eq!(size_of::<DhdrConvert>(), 1640 + PTR);
    assert_eq!(size_of::<DhdrPicture>(), 88 + 5 * PTR);
    // Opaque handles are zero-sized markers; only pointers to them cross the ABI.
    assert_eq!(size_of::<DhdrExact>(), 0);
    assert_eq!(size_of::<DhdrSurface>(), 0);
    assert_eq!(size_of::<DhdrNative>(), 0);
    assert_eq!(size_of::<DhdrTexture>(), 0);
    if cfg!(target_arch = "x86_64") {
        assert_eq!((size_of::<DhdrDeviceDesc>(), size_of::<DhdrCommand>(), size_of::<DhdrConvert>(),
                    size_of::<DhdrPicture>()), (32, 320, 1648, 128));
    }
}

#[test]
fn constants() {
    assert_eq!(DHDR_ABI_VERSION, 3);
    assert_eq!([DHDR_DEV_VULKAN, DHDR_DEV_FXC, DHDR_DEV_DEBUG, DHDR_DEV_VALIDATION, DHDR_DEV_FALLBACK,
                DHDR_DEV_NO_DUAL_SOURCE], [1, 2, 4, 8, 16, 32]);
    assert_eq!([DHDR_LOG_ERROR, DHDR_LOG_WARN, DHDR_LOG_INFO], [1, 2, 3]);
    assert_eq!([DHDR_FEAT_DUAL_SOURCE, DHDR_FEAT_RENDER_PIPELINES], [1, 2]);
    assert_eq!([DHDR_B_VRAM, DHDR_B_SNAPSHOT, DHDR_B_STATES, DHDR_B_PRIMITIVES, DHDR_B_BINS, DHDR_B_DISPLAY,
                DHDR_B_COPY, DHDR_B_READBACK, DHDR_B_STAGED, DHDR_B_BATCHES, DHDR_B_UPLOAD, DHDR_B_COUNT],
               [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
    assert_eq!([DHDR_P_LOCAL_COPY, DHDR_P_SCANOUT], [1, 2]);
    assert_eq!([DHDR_RASTER_EARLY_DEPTH, DHDR_RASTER_ADDRESS_SHIFT, DHDR_RASTER_EXCLUSIVE_WORD_STORE], [1, 2, 4]);
    assert_eq!(DHDR_PAGES * DHDR_PAGE_BYTES, 4 << 20);
    assert_eq!(DHDR_WANT_WORDS * 32, DHDR_PAGES as usize);
    assert_eq!([DHDR_WINDOW_WIN32, DHDR_WINDOW_WEB_CANVAS, DHDR_WINDOW_XLIB, DHDR_WINDOW_WAYLAND,
                DHDR_WINDOW_ANDROID, DHDR_WINDOW_METAL_LAYER], [1, 2, 3, 4, 5, 6]);
    assert_eq!([DHDR_PRESENT_NO_TEARING, DHDR_PRESENT_VSYNC, DHDR_SURFACE_COPY_SRC], [1, 2, 1]);
    assert_eq!([DHDR_CMD_DRAW, DHDR_CMD_COPY, DHDR_CMD_IMPORT, DHDR_CMD_ALPHA_UPLOAD], [0, 1, 2, 3]);
    assert_eq!(DHDR_NO_INDEX, -1);
    assert_eq!(DHDR_NATIVE_SURFACES, 48);
    assert_eq!([DHDR_NATIVE_DUAL, DHDR_NATIVE_POINT_FILTER], [1, 2]);
}
