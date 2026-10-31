//! dhd_render: the GS renderer of the PC port on wgpu (the plan's section 1.1).
//!
//! C calls it through `include/dhd_render.h` (natively `dhd_render.lib`, in the browser the wasm
//! module's raw exports); every export is in `ffi.rs`. Rust reads no environment: C passes every
//! option. x64 Windows (`--features native`) and the browser (`--features web`) only.
//!
//! This file fixes the module tree, the two aggregates the
//! exports work on (`exact::Exact`, `native::Native`) and the picture view C hands over. Each
//! module's own file belongs to its task; a task changes its own struct's fields freely, and adds
//! the methods the exports call in an `impl Exact` / `impl Native` block of its own file.
//!
//! | Module | Task | What |
//! |---|---|---|
//! | `abi` | F0a | the `#[repr(C)]` mirrors of the header (frozen) |
//! | `ffi`, `guard`, `host`, `log`, `shaders` | F0b | exports, MXCSR guard and `Owned`, host log, logger, WGSL |
//! | `device` | A2 | the process's one device |
//! | `exact::pipelines` | A2 | raster.wgsl's compute pipelines |
//! | `exact::{buffers, encoder, readback}` | A3 (B1: wasm readback) | the 11 buffers, the open encoder and pass, reads |
//! | `exact::display` | A4 | the display texture, scanout's bind group, capture |
//! | `present::{surface, pipe}` | A5 | window surfaces, present.wgsl's pipeline and pass |
//! | `native::{pipelines, init}` | N1 | draw pipelines by key, the native objects |
//! | `native::arenas` | N2 | constants, upload and vertex arenas |
//! | `native::{shared, textures}` | N3 | samplers and layouts shared by textures, `DhdrTexture` |
//! | `native::surfaces` | N4 | the 48 surfaces, scratch, copies, blits, alpha uploads, imports |
//! | `native::{convert, scanout}` | N5 | texture conversion runs, scanout, read_display |
//! | `native::render` | N6 | the command loop and draw passes, submit |
//! | `native::present` | N7 | the native present pass and filter |
//! | `web` | B1 | the browser: allocator, async init, the pending read |
//! | `test_hooks` | HK | `dhdr_test_*` (feature `test-hooks`) |
//! | `png_decode` | TP1 | the texture packs' PNG files (`dhdr_png_*`), native only |
#![forbid(unsafe_op_in_unsafe_fn)]
#![allow(clippy::missing_safety_doc, clippy::new_without_default)]

pub mod abi;
pub mod device;
pub mod ffi;
pub mod guard;
pub mod host;
pub mod log;
pub mod shaders;

/// The texture packs' PNG decoder (port/gs/texpack.c), on every native platform.
#[cfg(feature = "native")]
pub mod png_decode;

/// The port's WebSocket and HTTPS client (port/net/ws.h), native only: linked here so its
/// `#[no_mangle]` exports reach the one static library.
#[cfg(feature = "native")]
pub use dhd_net;

/// dhd.ini's settings (port/render/include/dhd_settings.h), native only: linked here for the
/// game's host (port/hle/host.c).
#[cfg(feature = "native")]
pub use dhd_settings;

/// Discord Rich Presence (port/render/include/dhd_presence.h), native only: linked here for the
/// game's host (port/hle/host.c).
#[cfg(feature = "native")]
pub use dhd_presence;

/// The disc image's online check (port/render/include/dhd_disc.h), native only: linked here for
/// the game's host (port/host/host_sdl.c).
#[cfg(feature = "native")]
pub use dhd_disc;

/// The renderer's version line (`dhdr_version`), NUL-terminated.
pub const VERSION: &str = "dhd_render 1.0 wgpu 29.0.3 core 29.0.3 naga 29.0.3\0";

/// The exact renderer: raster.wgsl compute over the C front end's device ops (`gpu_exact.inc`'s
/// `op_*` seam; OPTRACE.md). One per Gs, behind one `Owned` (`ffi.rs`).
pub mod exact {
    pub mod buffers;
    pub mod display;
    pub mod encoder;
    pub mod pipelines;
    pub mod readback;

    use crate::abi::DhdrExactStats;
    use crate::device::Gpu;

    /// One exact renderer. Created by `Exact::create` (`encoder.rs`); the ops are methods in
    /// their modules' files: `write`, `copy`, `batch`, `record`, `submit`, `stats`, `destroy`
    /// (`encoder.rs`), `read_begin`, `read_finish` (`readback.rs`), `capture_begin`,
    /// `capture_size`, `capture_finish` (`display.rs`).
    pub struct Exact {
        pub gpu: &'static Gpu,
        /// `DHDR_RASTER_*`, as the pipelines were built.
        pub raster_flags: u32,
        pub pipelines: pipelines::Pipelines,
        pub buffers: buffers::Buffers,
        pub encoder: encoder::Encoder,
        pub reading: readback::Reading,
        pub display: display::Display,
        pub stats: DhdrExactStats,
    }
}

/// Presentation: a surface on the window (Win32 HWND natively, the OffscreenCanvas in the
/// browser) and present.wgsl's pass for exact frames.
pub mod present {
    pub mod counter;
    pub mod pipe;
    pub mod surface;
}

/// The native renderer's GPU half (once the retired C path's gpu_wgpu_native.inc, last at
/// render/rust 3882d5576). C (`native_core.inc`)
/// records; the render thread hands each picture over as a `DhdrPicture`.
pub mod native {
    pub mod arenas;
    pub mod convert;
    pub mod init;
    pub mod pipelines;
    pub mod present;
    pub mod render;
    pub mod scanout;
    pub mod shared;
    pub mod surfaces;
    pub mod textures;
    pub mod zread;

    use crate::abi::*;
    use crate::device::Gpu;
    use std::sync::Arc;

    /// One native renderer (the render thread's, under native_core's context_lock). Created by
    /// `Native::create` (`init.rs`); the exports' methods: `surface` (`surfaces.rs`), `picture`
    /// (`render.rs`), `present`, `options` (`present.rs`), `read_begin`, `read_size`,
    /// `read_finish` (`scanout.rs`).
    pub struct Native {
        pub gpu: &'static Gpu,
        /// `DHDR_NATIVE_*` given to create (DUAL: dual-source blending may be used).
        pub flags: u32,
        pub objects: init::Objects,
        pub pipelines: pipelines::Pipelines,
        pub arenas: arenas::Arenas,
        pub surfaces: surfaces::Surfaces,
        pub converts: convert::Converts,
        pub display: scanout::Display,
        pub pass: render::Pass,
        pub presenter: present::Presenter,
        pub zread: zread::ZRead,
    }

    /// One picture as C handed it over (`DhdrPicture`), with its arrays as slices. Built by
    /// `ffi.rs` from the pointer; valid for the one `dhdr_native_picture` call.
    pub struct Picture<'a> {
        pub vertices: &'a [DhdrVertex],
        pub commands: &'a [DhdrCommand],
        pub converts: &'a [DhdrConvert],
        pub surfaces: &'a [DhdrSurfaceInfo],
        /// The scanned-out surface, or -1.
        pub display_surface: i32,
        pub display: [u32; 12],
        /// `movie_on`: the movie frame (width, height, RGBA8 pixels), shown instead.
        pub movie: Option<(u32, u32, &'a [u32])>,
        pub scale: u32,
    }

    impl<'a> Picture<'a> {
        /// Command `i`'s decoded texture, or None.
        pub fn texture(&self, i: usize) -> Option<&'a textures::Tex> {
            // SAFETY: C keeps every handle in the picture alive for the call (native_snapshot.inc).
            unsafe { texture_ref(self.commands[i].texture) }
        }
        /// Command `i`'s replacement (flag 2048), or None.
        pub fn replacement(&self, i: usize) -> Option<&'a textures::Tex> {
            // SAFETY: as `texture`.
            unsafe { texture_ref(self.commands[i].replacement) }
        }
        /// Conversion `i`'s base texture, or None.
        pub fn convert_base(&self, i: usize) -> Option<&'a textures::Tex> {
            // SAFETY: as `texture`.
            unsafe { texture_ref(self.converts[i].base) }
        }
        /// Command `i`'s pixels: IMPORT `w * h`, ALPHA_UPLOAD `64 * 32` RGBA8; None otherwise or
        /// when NULL (or a negative size).
        pub fn pixels(&self, i: usize) -> Option<&'a [u32]> {
            let k = &self.commands[i];
            let count = match k.kind {
                DHDR_CMD_IMPORT if k.w >= 0 && k.h >= 0 => k.w as usize * k.h as usize,
                DHDR_CMD_ALPHA_UPLOAD => 64 * 32,
                _ => return None,
            };
            if k.pixels.is_null() {
                return None;
            }
            // SAFETY: native_snapshot.inc passes count pixels for these kinds, alive for the call.
            Some(unsafe { std::slice::from_raw_parts(k.pixels, count) })
        }
    }

    /// Bytes of a replacement's `levels` mip levels of RGBA8 (each side halved per level, down to
    /// 1); None when w, h or levels is 0, levels is more than the chain has, or it overflows.
    pub fn mip_bytes(w: u32, h: u32, levels: u32) -> Option<usize> {
        if w == 0 || h == 0 || levels == 0 || levels > 32 - w.max(h).leading_zeros() {
            return None;
        }
        let mut total = 0usize;
        for l in 0..levels {
            let (lw, lh) = ((w >> l).max(1) as usize, (h >> l).max(1) as usize);
            total = total.checked_add(lw.checked_mul(lh)?.checked_mul(4)?)?;
        }
        Some(total)
    }

    /// A new texture's handle: the `Arc` becomes one reference (`dhdr_texture_create`).
    pub fn texture_handle(t: Arc<textures::Tex>) -> *mut DhdrTexture {
        Arc::into_raw(t) as *mut DhdrTexture
    }

    /// The texture behind a handle, or None for NULL.
    ///
    /// # Safety
    /// `h` is NULL or a live handle from `texture_handle`, and stays live for `'a`.
    pub unsafe fn texture_ref<'a>(h: *mut DhdrTexture) -> Option<&'a textures::Tex> {
        // SAFETY: the caller's contract: h came from Arc::into_raw and holds a reference.
        unsafe { (h as *const textures::Tex).as_ref() }
    }

    /// One more reference (`dhdr_texture_retain`). NULL is ignored.
    ///
    /// # Safety
    /// `h` is NULL or a live handle.
    pub unsafe fn texture_retain(h: *mut DhdrTexture) {
        if !h.is_null() {
            // SAFETY: a live handle is an Arc<Tex> pointer with at least one reference.
            unsafe { Arc::increment_strong_count(h as *const textures::Tex) }
        }
    }

    /// One reference less (`dhdr_texture_release`); the last one frees the texture. NULL is
    /// ignored.
    ///
    /// # Safety
    /// `h` is NULL or a live handle, and the caller gives up one reference.
    pub unsafe fn texture_release(h: *mut DhdrTexture) {
        if !h.is_null() {
            // SAFETY: as texture_retain; the caller owned the reference dropped here.
            unsafe { Arc::decrement_strong_count(h as *const textures::Tex) }
        }
    }
}

/// The browser platform layer (wasm32 only).
#[cfg(target_arch = "wasm32")]
pub mod web {
    pub mod alloc;
    pub mod init;
    pub mod wait;
}

/// Test hooks behind `dhdr_test_*` (feature `test-hooks`, native only).
#[cfg(feature = "test-hooks")]
pub mod test_hooks {
    pub mod depth_probe;
    pub mod presented;
}
