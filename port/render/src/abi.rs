//! The C ABI's types and constants: `#[repr(C)]` mirrors of `include/dhd_render.h` (version 1).
//!
//! This file depends on `core` only, so `port/tools/test_dhdr_abi.py` can compile it on its own
//! for every target and compare it with the C compiler's view of the header. Every size, alignment
//! and offset is also asserted below at compile time, per pointer width (4 bytes: i686 and wasm32;
//! 8 bytes: x86_64), so a build for a target whose layout differs does not compile.
//!
//! No struct has implicit padding on any target: pad fields are explicit, and pointer members come
//! last, so the bytes before them are the same everywhere. Opaque handles (`DhdrExact`,
//! `DhdrSurface`, `DhdrNative`, `DhdrTexture`) are zero-sized here; what they point at is the
//! renderer's business (`ffi.rs`).
#![allow(non_camel_case_types, dead_code)]

use core::ffi::{c_char, c_void};
use core::marker::{PhantomData, PhantomPinned};
use core::mem::{align_of, offset_of, size_of};

/// `DHDR_ABI_VERSION`: `DhdrDeviceDesc.abi` must carry it.
pub const DHDR_ABI_VERSION: u32 = 1;

// ---------------- device ----------------

pub const DHDR_DEV_VULKAN: u32 = 1;
pub const DHDR_DEV_FXC: u32 = 2;
pub const DHDR_DEV_DEBUG: u32 = 4;
pub const DHDR_DEV_VALIDATION: u32 = 8;
pub const DHDR_DEV_FALLBACK: u32 = 16;
pub const DHDR_DEV_NO_DUAL_SOURCE: u32 = 32;

pub const DHDR_LOG_ERROR: u32 = 1;
pub const DHDR_LOG_WARN: u32 = 2;
pub const DHDR_LOG_INFO: u32 = 3;

pub const DHDR_FEAT_DUAL_SOURCE: u32 = 1;
pub const DHDR_FEAT_RENDER_PIPELINES: u32 = 2;

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DhdrDeviceDesc {
    /// `size_of::<DhdrDeviceDesc>()`
    pub size: u32,
    /// `DHDR_ABI_VERSION`
    pub abi: u32,
    /// `DHDR_DEV_*`
    pub flags: u32,
    /// PCI ids of the presentation adapter; 0, 0: high performance.
    pub vendor_id: u32,
    pub device_id: u32,
    /// `DHDR_LOG_*`
    pub log_level: u32,
    /// UTF-8 path of dxcompiler.dll, NUL-terminated, or null (FXC).
    pub dxc_path: *const c_char,
}

// ---------------- exact ----------------

pub const DHDR_B_VRAM: u32 = 0;
pub const DHDR_B_SNAPSHOT: u32 = 1;
pub const DHDR_B_STATES: u32 = 2;
pub const DHDR_B_PRIMITIVES: u32 = 3;
pub const DHDR_B_BINS: u32 = 4;
pub const DHDR_B_DISPLAY: u32 = 5;
pub const DHDR_B_COPY: u32 = 6;
pub const DHDR_B_READBACK: u32 = 7;
pub const DHDR_B_STAGED: u32 = 8;
pub const DHDR_B_BATCHES: u32 = 9;
pub const DHDR_B_UPLOAD: u32 = 10;
pub const DHDR_B_COUNT: u32 = 11;

pub const DHDR_P_LOCAL_COPY: u32 = 1;
pub const DHDR_P_SCANOUT: u32 = 2;

pub const DHDR_RASTER_EARLY_DEPTH: u32 = 1;
pub const DHDR_RASTER_ADDRESS_SHIFT: u32 = 2;
pub const DHDR_RASTER_EXCLUSIVE_WORD_STORE: u32 = 4;

/// Pages of the exact readback image: 512 pages of 8192 bytes, `want` is 16 words of page bits.
pub const DHDR_PAGES: u32 = 512;
pub const DHDR_PAGE_BYTES: u32 = 8192;
pub const DHDR_WANT_WORDS: usize = 16;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DhdrExactStats {
    pub submits: u64,
    pub reads: u64,
    pub read_ticks: u64,
    pub dispatches: u64,
    pub bytes_written: u64,
    pub copies: u64,
}

// ---------------- surfaces and presentation ----------------

pub const DHDR_WINDOW_WIN32: u32 = 1;
pub const DHDR_WINDOW_WEB_CANVAS: u32 = 2;
pub const DHDR_WINDOW_XLIB: u32 = 3;
pub const DHDR_WINDOW_WAYLAND: u32 = 4;
pub const DHDR_WINDOW_ANDROID: u32 = 5;
pub const DHDR_WINDOW_METAL_LAYER: u32 = 6;

pub const DHDR_PRESENT_NO_TEARING: u32 = 1;
pub const DHDR_PRESENT_VSYNC: u32 = 2;

pub const DHDR_SURFACE_COPY_SRC: u32 = 1;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DhdrWindow {
    /// `DHDR_WINDOW_*`
    pub kind: u32,
    /// 0
    pub reserved: u32,
    /// WIN32: `a` = HWND, `b` = HINSTANCE; XLIB: `a` = Display *, `b` = Window; WAYLAND: `a` =
    /// wl_display *, `b` = wl_surface *; ANDROID: `a` = ANativeWindow *; METAL_LAYER: `a` = CAMetalLayer *.
    pub a: u64,
    pub b: u64,
}

// ---------------- native renderer ----------------

pub const DHDR_CMD_DRAW: u8 = 0;
pub const DHDR_CMD_COPY: u8 = 1;
pub const DHDR_CMD_IMPORT: u8 = 2;
pub const DHDR_CMD_ALPHA_UPLOAD: u8 = 3;

/// An absent `i16` surface or convert index.
pub const DHDR_NO_INDEX: i16 = -1;

/// Native surfaces (`index` of `dhdr_native_surface` is below this).
pub const DHDR_NATIVE_SURFACES: u32 = 48;

pub const DHDR_NATIVE_DUAL: u32 = 1;
pub const DHDR_NATIVE_POINT_FILTER: u32 = 2;

/// A picture vertex (native_core.inc's NativeVertex).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DhdrVertex {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub s: f32,
    pub t: f32,
    pub q: f32,
    pub fog: f32,
    pub rgba: u32,
}

/// A draw's uniform block (NativeConstants; native.wgsl).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DhdrNativeConstants {
    pub target: [f32; 2],
    pub flags: u32,
    pub atst: u32,
    pub aref: u32,
    pub alpha_scale: f32,
    pub texa_alpha: f32,
    pub scale: u32,
    pub fog: [f32; 4],
    pub tex_offset: [i32; 2],
    pub tex_size: [u32; 2],
    pub wrap: [u32; 4],
    pub region: [u32; 4],
}

/// A texture conversion's uniform block (NativeConvertConstants; native_convert).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DhdrConvertConstants {
    pub tex: [u32; 4],
    pub size: [u32; 4],
    /// Per source surface: page, bw, 1 for depth or 2 for 16-bit colour, Z units (float bits).
    pub slot: [[u32; 4]; 4],
    /// Per GS page: 0 the decoded base, 1-4 a source slot.
    pub pages: [u8; 512],
    pub palette: [u32; 256],
}

impl Default for DhdrConvertConstants {
    fn default() -> Self {
        DhdrConvertConstants { tex: [0; 4], size: [0; 4], slot: [[0; 4]; 4], pages: [0; 512], palette: [0; 256] }
    }
}

/// Opaque handle: the exact renderer of one Gs.
#[repr(C)]
pub struct DhdrExact {
    _private: [u8; 0],
    _marker: PhantomData<(*mut u8, PhantomPinned)>,
}

/// Opaque handle: a presentation surface.
#[repr(C)]
pub struct DhdrSurface {
    _private: [u8; 0],
    _marker: PhantomData<(*mut u8, PhantomPinned)>,
}

/// Opaque handle: the native renderer's GPU half.
#[repr(C)]
pub struct DhdrNative {
    _private: [u8; 0],
    _marker: PhantomData<(*mut u8, PhantomPinned)>,
}

/// Opaque handle: a reference-counted, immutable texture.
#[repr(C)]
pub struct DhdrTexture {
    _private: [u8; 0],
    _marker: PhantomData<(*mut u8, PhantomPinned)>,
}

/// One recorded command (NativeCommand, pointers last).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DhdrCommand {
    /// `DHDR_CMD_*`
    pub kind: u8,
    pub depth_key: u8,
    pub color_mask: u8,
    pub pad: u8,
    pub target: i16,
    pub depth: i16,
    pub texture_surface: i16,
    pub convert: i16,
    /// `1 | src << 1 | dst << 6 | op << 11`, D3D11 numbering.
    pub blend_key: u32,
    pub blend_fix: u32,
    /// GS pixels: left, top, right, bottom.
    pub scissor: [i32; 4],
    pub constants: DhdrNativeConstants,
    pub first: u32,
    pub count: u32,
    pub source: i16,
    pub pad2: i16,
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
    pub sx: i32,
    pub sy: i32,
    pub texture: *mut DhdrTexture,
    pub replacement: *mut DhdrTexture,
    pub pixels: *const u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DhdrConvert {
    pub surface: [i16; 4],
    pub constants: DhdrConvertConstants,
    pub base: *mut DhdrTexture,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DhdrSurfaceInfo {
    pub page: u32,
    pub bw: u32,
    pub wide: u32,
    pub depth: u32,
    pub zpsm: u32,
    pub scale: u32,
    pub size: [u32; 2],
    pub extent: [u32; 2],
}

/// `DhdrPicture::display_surface` when the display is off (PMODE has no read circuit on, as after
/// a GS reset): the picture is black, as the PS2 shows it (finding 160). -1: none, the last stays.
pub const DISPLAY_OFF: i32 = -2;

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DhdrPicture {
    /// `size_of::<DhdrPicture>()`
    pub size: u32,
    pub vertex_count: u32,
    pub command_count: u32,
    pub convert_count: u32,
    pub surface_count: u32,
    pub display_surface: i32,
    pub display: [u32; 12],
    pub movie_width: u32,
    pub movie_height: u32,
    pub movie_on: u32,
    pub scale: u32,
    pub vertices: *const DhdrVertex,
    pub commands: *const DhdrCommand,
    pub converts: *const DhdrConvert,
    pub surfaces: *const DhdrSurfaceInfo,
    pub movie: *const u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DhdrPictureResult {
    pub shown: u32,
    pub display_width: u32,
    pub display_height: u32,
    pub pad: u32,
    pub draws: u64,
    pub self_copies: u64,
    pub self_copy_px: u64,
    pub copies: u64,
    pub copy_px: u64,
    pub imports: u64,
    pub import_px: u64,
    pub two_pass_draws: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DhdrPresentRect {
    pub rect: [f32; 4],
    pub cut: [i32; 4],
    pub window_w: u32,
    pub window_h: u32,
    pub resize: u32,
    /// Nonzero: fill the window around `cut` with the picture blurred and darkened (widescreen's
    /// 4:3 pictures, finding 140).
    pub fill: u32,
}

// ---------------- function signatures ----------------
//
// The header's functions as Rust function-pointer types, `<name>_fn`, parameter names as in the
// header. `ffi.rs` asserts each export against its type (`const _: dhdr_x_fn = dhdr_x;`), and
// `port/tools/test_dhdr_abi.py` compares every one with the header's prototype.

/// Provided by C (`gpu_rust.c`), native only.
pub type dhd_host_log_fn = unsafe extern "C" fn(line: *const c_char, bytes: usize);

pub type dhdr_device_init_fn = unsafe extern "C" fn(d: *const DhdrDeviceDesc) -> i32;
pub type dhdr_adapter_name_fn = unsafe extern "C" fn() -> *const c_char;
pub type dhdr_device_features_fn = unsafe extern "C" fn() -> u32;
pub type dhdr_version_fn = unsafe extern "C" fn() -> *const c_char;

pub type dhdr_exact_create_fn = unsafe extern "C" fn(raster_flags: u32) -> *mut DhdrExact;
pub type dhdr_exact_destroy_fn = unsafe extern "C" fn(x: *mut DhdrExact);
pub type dhdr_exact_write_fn =
    unsafe extern "C" fn(x: *mut DhdrExact, buffer: u32, offset: u32, data: *const c_void, bytes: u32);
pub type dhdr_exact_copy_fn =
    unsafe extern "C" fn(x: *mut DhdrExact, from: u32, from_offset: u32, to: u32, to_offset: u32, bytes: u32);
pub type dhdr_exact_batch_fn = unsafe extern "C" fn(x: *mut DhdrExact, dynamic_offset: u32, groups_x: u32, groups_y: u32);
pub type dhdr_exact_record_fn = unsafe extern "C" fn(x: *mut DhdrExact, pipeline: u32, x_word: u32, y_word: u32);
pub type dhdr_exact_submit_fn = unsafe extern "C" fn(x: *mut DhdrExact);
pub type dhdr_exact_read_begin_fn = unsafe extern "C" fn(x: *mut DhdrExact, buffer: u32, want: *const u32) -> i32;
pub type dhdr_exact_read_finish_fn =
    unsafe extern "C" fn(x: *mut DhdrExact, buffer: u32, want: *const u32, image: *mut c_void) -> i32;
pub type dhdr_exact_capture_begin_fn = unsafe extern "C" fn(x: *mut DhdrExact, w: *mut u32, h: *mut u32) -> i32;
pub type dhdr_exact_capture_finish_fn = unsafe extern "C" fn(x: *mut DhdrExact, pixels: *mut u32) -> i32;
pub type dhdr_exact_stats_fn = unsafe extern "C" fn(x: *const DhdrExact, out: *mut DhdrExactStats);

pub type dhdr_surface_create_fn = unsafe extern "C" fn(w: *const DhdrWindow, present_flags: u32) -> *mut DhdrSurface;
pub type dhdr_surface_destroy_fn = unsafe extern "C" fn(s: *mut DhdrSurface);
pub type dhdr_surface_usage_fn = unsafe extern "C" fn(s: *const DhdrSurface) -> u32;
pub type dhdr_present_exact_fn = unsafe extern "C" fn(s: *mut DhdrSurface, x: *mut DhdrExact, pixels: *const u32,
                                                      w: u32, h: u32, window_w: u32, window_h: u32) -> i32;
pub type dhdr_surface_counter_fn =
    unsafe extern "C" fn(s: *mut DhdrSurface, enabled: i32, server_ms: i32, race_ms: i32, riders_ms: i32, seconds: f64);
pub type dhdr_surface_touch_fn =
    unsafe extern "C" fn(s: *mut DhdrSurface, items: *const f32, count: u32, width_mm: f32, height_mm: f32);
pub type dhdr_surface_present_fn = unsafe extern "C" fn(s: *mut DhdrSurface);

pub type dhdr_native_create_fn = unsafe extern "C" fn(flags: u32) -> *mut DhdrNative;
pub type dhdr_native_destroy_fn = unsafe extern "C" fn(n: *mut DhdrNative);
pub type dhdr_texture_create_fn = unsafe extern "C" fn(rgba: *const u32, w: u32, h: u32) -> *mut DhdrTexture;
pub type dhdr_replace_create_fn =
    unsafe extern "C" fn(rgba_mips: *const u8, w: u32, h: u32, levels: u32) -> *mut DhdrTexture;
pub type dhdr_texture_retain_fn = unsafe extern "C" fn(t: *mut DhdrTexture);
pub type dhdr_texture_release_fn = unsafe extern "C" fn(t: *mut DhdrTexture);
pub type dhdr_native_surface_fn = unsafe extern "C" fn(n: *mut DhdrNative, index: u32, scale: u32, w: u32, h: u32,
                                                       depth: u32, size_out: *mut u32) -> i32;
pub type dhdr_native_picture_fn =
    unsafe extern "C" fn(n: *mut DhdrNative, p: *const DhdrPicture, out: *mut DhdrPictureResult) -> i32;
pub type dhdr_native_present_fn =
    unsafe extern "C" fn(n: *mut DhdrNative, s: *mut DhdrSurface, r: *const DhdrPresentRect) -> i32;
pub type dhdr_native_read_begin_fn = unsafe extern "C" fn(n: *mut DhdrNative, w: *mut u32, h: *mut u32) -> i32;
pub type dhdr_native_read_finish_fn = unsafe extern "C" fn(n: *mut DhdrNative, pixels: *mut u32) -> i32;
pub type dhdr_native_options_fn = unsafe extern "C" fn(n: *mut DhdrNative, flags: u32);
pub type dhdr_native_zread_start_fn = unsafe extern "C" fn(n: *mut DhdrNative, index: u32, rect: *const u32, tag: u64) -> i32;
pub type dhdr_native_zread_take_fn =
    unsafe extern "C" fn(n: *mut DhdrNative, out: *mut u32, cap: u32, rect_out: *mut u32, tag_out: *mut u64) -> i32;

/// The texture packs' PNG files, native only.
pub type dhdr_png_size_fn = unsafe extern "C" fn(data: *const u8, size: usize, w: *mut u32, h: *mut u32) -> i32;
pub type dhdr_png_decode_fn =
    unsafe extern "C" fn(data: *const u8, size: usize, rgba: *mut u8, w: u32, h: u32, opaque: u32) -> i32;

/// wasm only.
pub type dhdr_web_surface_fn = unsafe extern "C" fn() -> *mut DhdrSurface;

/// Test hooks (feature `test-hooks`), native only.
pub type dhdr_test_depth_probe_fn = unsafe extern "C" fn(cases: *const c_void, count: u32, results: *mut u32) -> i32;
pub type dhdr_test_presented_fn = unsafe extern "C" fn(w: *mut u32, h: *mut u32) -> *const u32;

// ---------------- compile-time layout ----------------

/// Size of a pointer member on this target.
const P: usize = size_of::<*const u8>();
/// Bytes of a struct's pointer-free prefix plus `n` pointers.
const fn with_ptrs(prefix: usize, n: usize) -> usize {
    prefix + n * P
}

macro_rules! layout {
    ($t:ty, size $size:expr, align $align:expr, { $($f:ident @ $off:expr),* $(,)? }) => {
        const _: () = {
            assert!(size_of::<$t>() == $size, concat!("size of ", stringify!($t)));
            assert!(align_of::<$t>() == $align, concat!("align of ", stringify!($t)));
            $(assert!(offset_of!($t, $f) == $off, concat!(stringify!($t), ".", stringify!($f)));)*
        };
    };
}

const _: () = assert!(P == 4 || P == 8, "pointers are 4 or 8 bytes");

layout!(DhdrDeviceDesc, size with_ptrs(24, 1), align P, {
    size @ 0, abi @ 4, flags @ 8, vendor_id @ 12, device_id @ 16, log_level @ 20, dxc_path @ 24,
});
layout!(DhdrExactStats, size 48, align 8, {
    submits @ 0, reads @ 8, read_ticks @ 16, dispatches @ 24, bytes_written @ 32, copies @ 40,
});
layout!(DhdrWindow, size 24, align 8, { kind @ 0, reserved @ 4, a @ 8, b @ 16 });
layout!(DhdrVertex, size 32, align 4, {
    x @ 0, y @ 4, z @ 8, s @ 12, t @ 16, q @ 20, fog @ 24, rgba @ 28,
});
layout!(DhdrNativeConstants, size 96, align 4, {
    target @ 0, flags @ 8, atst @ 12, aref @ 16, alpha_scale @ 20, texa_alpha @ 24, scale @ 28,
    fog @ 32, tex_offset @ 48, tex_size @ 56, wrap @ 64, region @ 80,
});
layout!(DhdrConvertConstants, size 1632, align 4, {
    tex @ 0, size @ 16, slot @ 32, pages @ 96, palette @ 608,
});
layout!(DhdrCommand, size with_ptrs(168, 3), align P, {
    kind @ 0, depth_key @ 1, color_mask @ 2, pad @ 3, target @ 4, depth @ 6, texture_surface @ 8,
    convert @ 10, blend_key @ 12, blend_fix @ 16, scissor @ 20, constants @ 36, first @ 132,
    count @ 136, source @ 140, pad2 @ 142, x @ 144, y @ 148, w @ 152, h @ 156, sx @ 160, sy @ 164,
    texture @ 168, replacement @ 168 + P, pixels @ 168 + 2 * P,
});
layout!(DhdrConvert, size with_ptrs(1640, 1), align P, { surface @ 0, constants @ 8, base @ 1640 });
layout!(DhdrSurfaceInfo, size 40, align 4, {
    page @ 0, bw @ 4, wide @ 8, depth @ 12, zpsm @ 16, scale @ 20, size @ 24, extent @ 32,
});
layout!(DhdrPicture, size with_ptrs(88, 5), align P, {
    size @ 0, vertex_count @ 4, command_count @ 8, convert_count @ 12, surface_count @ 16,
    display_surface @ 20, display @ 24, movie_width @ 72, movie_height @ 76, movie_on @ 80, scale @ 84,
    vertices @ 88, commands @ 88 + P, converts @ 88 + 2 * P, surfaces @ 88 + 3 * P, movie @ 88 + 4 * P,
});
layout!(DhdrPictureResult, size 80, align 8, {
    shown @ 0, display_width @ 4, display_height @ 8, pad @ 12, draws @ 16, self_copies @ 24,
    self_copy_px @ 32, copies @ 40, copy_px @ 48, imports @ 56, import_px @ 64, two_pass_draws @ 72,
});
layout!(DhdrPresentRect, size 48, align 4, {
    rect @ 0, cut @ 16, window_w @ 32, window_h @ 36, resize @ 40, fill @ 44,
});
