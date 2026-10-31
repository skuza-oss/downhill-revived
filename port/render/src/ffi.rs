//! Every export of `include/dhd_render.h`. Each one enters `guard::Guard` first (MXCSR,
//! the export's name), checks what C passed, turns pointers into references and slices, and calls
//! the module that does the work; it holds no logic of its own. Errors a module returns are logged
//! here, `dhd_render: <export>: <message>`. A NULL handle where the header has none, or a pointer
//! missing where data must be, is a C bug and fatal.
//!
//! Objects: `DhdrExact` = `Owned<exact::Exact>`, `DhdrNative` = `Owned<native::Native>`,
//! `DhdrSurface` = `Owned<present::surface::Surface>`, each a `Box` behind the handle;
//! `DhdrTexture` = `Arc<native::textures::Tex>` (`native::texture_*`). Every export's type is
//! checked against `abi.rs`'s `<name>_fn` at compile time.
use crate::abi::*;
use crate::exact::Exact;
use crate::guard::{fatal, Guard, Owned};
use crate::native::textures::Tex;
use crate::native::{Native, Picture};
use crate::present::surface::Surface;
use std::ffi::{c_char, c_void};
use std::sync::Arc;

fn log_err(export: &str, message: &str) {
    crate::host::log(&format!("dhd_render: {export}: {message}"));
}

fn gpu_or_log(export: &str) -> Option<&'static crate::device::Gpu> {
    let g = crate::device::gpu();
    if g.is_none() {
        log_err(export, "no device (dhdr_device_init first)");
    }
    g
}

/// The object behind a handle; NULL is fatal.
///
/// # Safety
/// `h` is NULL or a live handle of this object type.
unsafe fn object<'a, T>(h: *const c_void, export: &'static str) -> &'a Owned<T> {
    // SAFETY: the caller's contract; the Box stays alive until its destroy.
    match unsafe { (h as *const Owned<T>).as_ref() } {
        Some(o) => o,
        None => fatal(&format!("dhd_render: {export}: NULL handle")),
    }
}

fn into_handle<T, H>(value: T) -> *mut H {
    Box::into_raw(Box::new(Owned::new(value))) as *mut H
}

/// `len` elements at `p` (NULL is allowed only for 0); fatal otherwise.
///
/// # Safety
/// A non-NULL `p` points at `len` readable elements, alive for `'a`.
unsafe fn slice<'a, T>(p: *const T, len: usize, export: &'static str) -> &'a [T] {
    if len == 0 {
        return &[];
    }
    if p.is_null() {
        fatal(&format!("dhd_render: {export}: NULL data for {len} elements"));
    }
    // SAFETY: the caller's contract.
    unsafe { std::slice::from_raw_parts(p, len) }
}

/// As `slice`, writable.
///
/// # Safety
/// A non-NULL `p` points at `len` writable elements, alive for `'a`, aliased by nothing else.
unsafe fn slice_mut<'a, T>(p: *mut T, len: usize, export: &'static str) -> &'a mut [T] {
    if len == 0 {
        return &mut [];
    }
    if p.is_null() {
        fatal(&format!("dhd_render: {export}: NULL output for {len} elements"));
    }
    // SAFETY: the caller's contract.
    unsafe { std::slice::from_raw_parts_mut(p, len) }
}

/// A required output or input pointer; NULL is fatal.
///
/// # Safety
/// A non-NULL `p` is valid for the access.
unsafe fn must<'a, T>(p: *mut T, export: &'static str) -> &'a mut T {
    // SAFETY: the caller's contract.
    match unsafe { p.as_mut() } {
        Some(r) => r,
        None => fatal(&format!("dhd_render: {export}: NULL pointer argument")),
    }
}

// ---------------------------------------------------------------- device

const _: dhdr_device_init_fn = dhdr_device_init;
const _: dhdr_adapter_name_fn = dhdr_adapter_name;
const _: dhdr_device_features_fn = dhdr_device_features;
const _: dhdr_version_fn = dhdr_version;

#[no_mangle]
pub unsafe extern "C" fn dhdr_device_init(d: *const DhdrDeviceDesc) -> i32 {
    const E: &str = "dhdr_device_init";
    let _g = Guard::enter(E);
    if crate::device::gpu().is_some() {
        return 1;
    }
    #[cfg(target_arch = "wasm32")]
    {
        // The device is made by the async dhdr_web_init; this only reports it.
        let _ = d;
        0
    }
    #[cfg(not(target_arch = "wasm32"))]
    {
        // SAFETY: C passes NULL or a DhdrDeviceDesc.
        let Some(d) = (unsafe { d.as_ref() }) else {
            log_err(E, "NULL descriptor");
            crate::device::fail();
            return 0;
        };
        if d.size as usize != std::mem::size_of::<DhdrDeviceDesc>() || d.abi != DHDR_ABI_VERSION {
            log_err(E, &format!("descriptor size {} abi {}; this library is abi {} size {}", d.size, d.abi,
                                DHDR_ABI_VERSION, std::mem::size_of::<DhdrDeviceDesc>()));
            crate::device::fail();
            return 0;
        }
        let dxc_path = if d.dxc_path.is_null() {
            None
        } else {
            // SAFETY: a non-NULL dxc_path is a NUL-terminated string (the header).
            match unsafe { std::ffi::CStr::from_ptr(d.dxc_path) }.to_str() {
                Ok(s) => Some(s.to_owned()),
                Err(_) => {
                    log_err(E, "dxc_path is not UTF-8");
                    crate::device::fail();
                    return 0;
                }
            }
        };
        let o = crate::device::DeviceOptions { flags: d.flags, vendor_id: d.vendor_id, device_id: d.device_id,
                                               log_level: d.log_level, dxc_path };
        crate::device::init(&o) as i32
    }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_adapter_name() -> *const c_char {
    let _g = Guard::enter("dhdr_adapter_name");
    crate::device::adapter_name().as_ptr()
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_device_features() -> u32 {
    let _g = Guard::enter("dhdr_device_features");
    crate::device::features()
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_version() -> *const c_char {
    crate::VERSION.as_ptr() as *const c_char
}

// ---------------------------------------------------------------- exact

const _: dhdr_exact_create_fn = dhdr_exact_create;
const _: dhdr_exact_destroy_fn = dhdr_exact_destroy;
const _: dhdr_exact_write_fn = dhdr_exact_write;
const _: dhdr_exact_copy_fn = dhdr_exact_copy;
const _: dhdr_exact_batch_fn = dhdr_exact_batch;
const _: dhdr_exact_record_fn = dhdr_exact_record;
const _: dhdr_exact_submit_fn = dhdr_exact_submit;
const _: dhdr_exact_read_begin_fn = dhdr_exact_read_begin;
const _: dhdr_exact_read_finish_fn = dhdr_exact_read_finish;
const _: dhdr_exact_capture_begin_fn = dhdr_exact_capture_begin;
const _: dhdr_exact_capture_finish_fn = dhdr_exact_capture_finish;
const _: dhdr_exact_stats_fn = dhdr_exact_stats;

/// # Safety
/// `x` is NULL or a live `DhdrExact`.
unsafe fn exact<'a>(x: *const DhdrExact, export: &'static str) -> &'a Owned<Exact> {
    // SAFETY: the caller's contract.
    unsafe { object(x as *const c_void, export) }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_exact_create(raster_flags: u32) -> *mut DhdrExact {
    const E: &str = "dhdr_exact_create";
    let _g = Guard::enter(E);
    let Some(gpu) = gpu_or_log(E) else { return std::ptr::null_mut() };
    if raster_flags & !7 != 0 {
        log_err(E, &format!("raster_flags {raster_flags:#x} has bits outside DHDR_RASTER_*"));
        return std::ptr::null_mut();
    }
    match Exact::create(gpu, raster_flags) {
        Ok(x) => into_handle(x),
        Err(e) => {
            log_err(E, &e);
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_exact_destroy(x: *mut DhdrExact) {
    const E: &str = "dhdr_exact_destroy";
    let _g = Guard::enter(E);
    if !x.is_null() {
        // SAFETY: a live handle from dhdr_exact_create; C gives it up here.
        let b = unsafe { Box::from_raw(x as *mut Owned<Exact>) };
        b.into_inner(E).destroy();
    }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_exact_write(x: *mut DhdrExact, buffer: u32, offset: u32, data: *const c_void, bytes: u32) {
    const E: &str = "dhdr_exact_write";
    let _g = Guard::enter(E);
    // SAFETY: the header's contract: data holds bytes bytes.
    let data = unsafe { slice(data as *const u8, bytes as usize, E) };
    // SAFETY: as exact().
    unsafe { exact(x, E) }.with(E, |e| e.write(buffer, offset, data))
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_exact_copy(x: *mut DhdrExact, from: u32, from_offset: u32, to: u32, to_offset: u32,
                                         bytes: u32) {
    const E: &str = "dhdr_exact_copy";
    let _g = Guard::enter(E);
    // SAFETY: as exact().
    unsafe { exact(x, E) }.with(E, |e| e.copy(from, from_offset, to, to_offset, bytes))
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_exact_batch(x: *mut DhdrExact, dynamic_offset: u32, groups_x: u32, groups_y: u32) {
    const E: &str = "dhdr_exact_batch";
    let _g = Guard::enter(E);
    // SAFETY: as exact().
    unsafe { exact(x, E) }.with(E, |e| e.batch(dynamic_offset, groups_x, groups_y))
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_exact_record(x: *mut DhdrExact, pipeline: u32, x_word: u32, y_word: u32) {
    const E: &str = "dhdr_exact_record";
    let _g = Guard::enter(E);
    // SAFETY: as exact().
    unsafe { exact(x, E) }.with(E, |e| e.record(pipeline, x_word, y_word))
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_exact_submit(x: *mut DhdrExact) {
    const E: &str = "dhdr_exact_submit";
    let _g = Guard::enter(E);
    // SAFETY: as exact().
    unsafe { exact(x, E) }.with(E, |e| e.submit())
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_exact_read_begin(x: *mut DhdrExact, buffer: u32, want: *const u32) -> i32 {
    const E: &str = "dhdr_exact_read_begin";
    let _g = Guard::enter(E);
    // SAFETY: want is uint32_t[16] (the header).
    let want: &[u32; 16] = unsafe { slice(want, 16, E) }.try_into().unwrap_or_else(|_| fatal("want"));
    // SAFETY: as exact().
    unsafe { exact(x, E) }.begin(E, |e| {
        let r = e.read_begin(buffer, want);
        (r, r == 1)
    })
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_exact_read_finish(x: *mut DhdrExact, buffer: u32, want: *const u32, image: *mut c_void) -> i32 {
    const E: &str = "dhdr_exact_read_finish";
    let _g = Guard::enter(E);
    // SAFETY: want is uint32_t[16]; image is the 4 MiB page image (512 pages of 8192 bytes).
    let want: &[u32; 16] = unsafe { slice(want, 16, E) }.try_into().unwrap_or_else(|_| fatal("want"));
    let image = unsafe { slice_mut(image as *mut u8, (DHDR_PAGES * DHDR_PAGE_BYTES) as usize, E) };
    // SAFETY: as exact().
    unsafe { exact(x, E) }.finish(E, "dhdr_exact_read_begin", |e| e.read_finish(buffer, want, image))
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_exact_capture_begin(x: *mut DhdrExact, w: *mut u32, h: *mut u32) -> i32 {
    const E: &str = "dhdr_exact_capture_begin";
    let _g = Guard::enter(E);
    // SAFETY: w and h are outputs (the header).
    let (w, h) = unsafe { (must(w, E), must(h, E)) };
    // SAFETY: as exact().
    unsafe { exact(x, E) }.begin(E, |e| match e.capture_begin() {
        Some((cw, ch)) => {
            (*w, *h) = (cw, ch);
            (1, true)
        }
        None => {
            (*w, *h) = (0, 0);
            (0, false)
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_exact_capture_finish(x: *mut DhdrExact, pixels: *mut u32) -> i32 {
    const E: &str = "dhdr_exact_capture_finish";
    let _g = Guard::enter(E);
    // SAFETY: as exact().
    unsafe { exact(x, E) }.finish(E, "dhdr_exact_capture_begin", |e| {
        let (w, h) = e.capture_size();
        // SAFETY: pixels holds the w * h pixels begin announced.
        let pixels = unsafe { slice_mut(pixels, w as usize * h as usize, E) };
        e.capture_finish(pixels)
    })
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_exact_stats(x: *const DhdrExact, out: *mut DhdrExactStats) {
    const E: &str = "dhdr_exact_stats";
    let _g = Guard::enter(E);
    // SAFETY: out is an output (the header).
    let out = unsafe { must(out, E) };
    // SAFETY: as exact().
    *out = unsafe { exact(x, E) }.with(E, |e| e.stats());
}

// ---------------------------------------------------------------- surfaces and presentation

const _: dhdr_surface_create_fn = dhdr_surface_create;
const _: dhdr_surface_destroy_fn = dhdr_surface_destroy;
const _: dhdr_surface_usage_fn = dhdr_surface_usage;
const _: dhdr_present_exact_fn = dhdr_present_exact;
const _: dhdr_surface_present_fn = dhdr_surface_present;

/// # Safety
/// `s` is NULL or a live `DhdrSurface`.
unsafe fn surface<'a>(s: *const DhdrSurface, export: &'static str) -> &'a Owned<Surface> {
    // SAFETY: the caller's contract.
    unsafe { object(s as *const c_void, export) }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_surface_create(w: *const DhdrWindow, present_flags: u32) -> *mut DhdrSurface {
    const E: &str = "dhdr_surface_create";
    let _g = Guard::enter(E);
    let Some(gpu) = gpu_or_log(E) else { return std::ptr::null_mut() };
    // SAFETY: C passes NULL or a DhdrWindow.
    let Some(w) = (unsafe { w.as_ref() }) else {
        log_err(E, "NULL window");
        return std::ptr::null_mut();
    };
    if !Surface::supports(w.kind) {
        log_err(E, &format!("window kind {} is not supported here (this build: {:?}; the browser's \
                             canvas surface comes from dhdr_web_surface)", w.kind, Surface::KINDS));
        return std::ptr::null_mut();
    }
    match Surface::create(gpu, w, present_flags) {
        Ok(s) => into_handle(s),
        Err(e) => {
            log_err(E, &e);
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_surface_destroy(s: *mut DhdrSurface) {
    const E: &str = "dhdr_surface_destroy";
    let _g = Guard::enter(E);
    if !s.is_null() {
        // SAFETY: a live handle from dhdr_surface_create / dhdr_web_surface; C gives it up here.
        drop(unsafe { Box::from_raw(s as *mut Owned<Surface>) }.into_inner(E));
    }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_surface_usage(s: *const DhdrSurface) -> u32 {
    const E: &str = "dhdr_surface_usage";
    let _g = Guard::enter(E);
    // SAFETY: as surface().
    unsafe { surface(s, E) }.with(E, |s| s.usage())
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_present_exact(s: *mut DhdrSurface, x: *mut DhdrExact, pixels: *const u32, w: u32,
                                            h: u32, window_w: u32, window_h: u32) -> i32 {
    const E: &str = "dhdr_present_exact";
    let _g = Guard::enter(E);
    let pixels = if pixels.is_null() {
        None
    } else {
        if w == 0 || h == 0 || w as u64 * h as u64 > 1024 * 1024 {
            log_err(E, &format!("pixels of {w} x {h} (1 to 1024 * 1024 pixels)"));
            return 0;
        }
        // SAFETY: pixels holds w * h RGBA8 pixels (the header).
        Some(unsafe { slice(pixels, w as usize * h as usize, E) })
    };
    // SAFETY: as surface() and exact().
    unsafe { surface(s, E) }.with(E, |s| {
        if x.is_null() {
            s.present_exact(None, pixels, w, h, window_w, window_h)
        } else {
            // SAFETY: as exact().
            unsafe { exact(x, E) }.with(E, |e| s.present_exact(Some(e), pixels, w, h, window_w, window_h))
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_surface_counter(s: *mut DhdrSurface, enabled: i32, ping: i32, seconds: f64) {
    const E: &str = "dhdr_surface_counter";
    let _g = Guard::enter(E);
    // SAFETY: the caller owns a live surface, as for surface_present.
    unsafe { surface(s, E) }.with(E, |s| s.counter.update(enabled != 0, ping, seconds));
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_surface_present(s: *mut DhdrSurface) {
    const E: &str = "dhdr_surface_present";
    let _g = Guard::enter(E);
    // SAFETY: as surface().
    unsafe { surface(s, E) }.with(E, |s| {
        s.draw_counter();
        // The present test-hook point: the frame, read back before it is presented.
        #[cfg(feature = "test-hooks")]
        if s.usage() & DHDR_SURFACE_COPY_SRC != 0 {
            if let Some(frame) = s.frame_texture() {
                crate::test_hooks::presented::capture(s.gpu, frame);
            }
        }
        s.present()
    })
}

// ---------------------------------------------------------------- native renderer

const _: dhdr_native_create_fn = dhdr_native_create;
const _: dhdr_native_destroy_fn = dhdr_native_destroy;
const _: dhdr_texture_create_fn = dhdr_texture_create;
const _: dhdr_replace_create_fn = dhdr_replace_create;
const _: dhdr_texture_retain_fn = dhdr_texture_retain;
const _: dhdr_texture_release_fn = dhdr_texture_release;
const _: dhdr_native_surface_fn = dhdr_native_surface;
const _: dhdr_native_picture_fn = dhdr_native_picture;
const _: dhdr_native_present_fn = dhdr_native_present;
const _: dhdr_native_read_begin_fn = dhdr_native_read_begin;
const _: dhdr_native_read_finish_fn = dhdr_native_read_finish;
const _: dhdr_native_options_fn = dhdr_native_options;
const _: dhdr_native_zread_start_fn = dhdr_native_zread_start;
const _: dhdr_native_zread_take_fn = dhdr_native_zread_take;

/// # Safety
/// `n` is NULL or a live `DhdrNative`.
unsafe fn native<'a>(n: *const DhdrNative, export: &'static str) -> &'a Owned<Native> {
    // SAFETY: the caller's contract.
    unsafe { object(n as *const c_void, export) }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_native_create(flags: u32) -> *mut DhdrNative {
    const E: &str = "dhdr_native_create";
    let _g = Guard::enter(E);
    let Some(gpu) = gpu_or_log(E) else { return std::ptr::null_mut() };
    match Native::create(gpu, flags) {
        Ok(n) => into_handle(n),
        Err(e) => {
            log_err(E, &e);
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_native_destroy(n: *mut DhdrNative) {
    const E: &str = "dhdr_native_destroy";
    let _g = Guard::enter(E);
    if !n.is_null() {
        // SAFETY: a live handle from dhdr_native_create; C gives it up here.
        drop(unsafe { Box::from_raw(n as *mut Owned<Native>) }.into_inner(E));
    }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_texture_create(rgba: *const u32, w: u32, h: u32) -> *mut DhdrTexture {
    const E: &str = "dhdr_texture_create";
    let _g = Guard::enter(E);
    let Some(gpu) = gpu_or_log(E) else { return std::ptr::null_mut() };
    if w == 0 || h == 0 {
        log_err(E, &format!("size {w} x {h}"));
        return std::ptr::null_mut();
    }
    // SAFETY: rgba holds w * h pixels (the header).
    let rgba = unsafe { slice(rgba, w as usize * h as usize, E) };
    match Tex::create(gpu, rgba, w, h) {
        Ok(t) => crate::native::texture_handle(Arc::new(t)),
        Err(e) => {
            log_err(E, &e);
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_replace_create(rgba_mips: *const u8, w: u32, h: u32, levels: u32) -> *mut DhdrTexture {
    const E: &str = "dhdr_replace_create";
    let _g = Guard::enter(E);
    let Some(gpu) = gpu_or_log(E) else { return std::ptr::null_mut() };
    let Some(bytes) = crate::native::mip_bytes(w, h, levels) else {
        log_err(E, &format!("size {w} x {h} with {levels} levels"));
        return std::ptr::null_mut();
    };
    // SAFETY: rgba_mips holds the levels' bytes (the header).
    let data = unsafe { slice(rgba_mips, bytes, E) };
    match Tex::replace(gpu, data, w, h, levels) {
        Ok(t) => crate::native::texture_handle(Arc::new(t)),
        Err(e) => {
            log_err(E, &e);
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_texture_retain(t: *mut DhdrTexture) {
    let _g = Guard::enter("dhdr_texture_retain");
    // SAFETY: t is NULL or a live handle (the header).
    unsafe { crate::native::texture_retain(t) }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_texture_release(t: *mut DhdrTexture) {
    let _g = Guard::enter("dhdr_texture_release");
    // SAFETY: t is NULL or a live handle whose reference C gives up.
    unsafe { crate::native::texture_release(t) }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_native_surface(n: *mut DhdrNative, index: u32, scale: u32, w: u32, h: u32, depth: u32,
                                             size_out: *mut u32) -> i32 {
    const E: &str = "dhdr_native_surface";
    let _g = Guard::enter(E);
    // SAFETY: size_out is uint32_t[2] (the header).
    let size_out = unsafe { slice_mut(size_out, 2, E) };
    if index >= DHDR_NATIVE_SURFACES {
        log_err(E, &format!("surface index {index} (fewer than {DHDR_NATIVE_SURFACES})"));
        return 0;
    }
    // SAFETY: as native().
    unsafe { native(n, E) }.with(E, |n| match n.surface(index, scale, w, h, depth != 0) {
        Ok(size) => {
            size_out.copy_from_slice(&size);
            1
        }
        Err(e) => {
            log_err(E, &e);
            0
        }
    })
}

/// The picture's arrays as slices.
///
/// # Safety
/// `p` is a valid DhdrPicture whose arrays hold their counts, alive for `'a`.
unsafe fn picture<'a>(p: &'a DhdrPicture, export: &'static str) -> Picture<'a> {
    // SAFETY: the caller's contract, array by array.
    unsafe {
        Picture {
            vertices: slice(p.vertices, p.vertex_count as usize, export),
            commands: slice(p.commands, p.command_count as usize, export),
            converts: slice(p.converts, p.convert_count as usize, export),
            surfaces: slice(p.surfaces, p.surface_count as usize, export),
            display_surface: p.display_surface,
            display: p.display,
            movie: (p.movie_on != 0).then(|| {
                (p.movie_width, p.movie_height,
                 slice(p.movie, p.movie_width as usize * p.movie_height as usize, export))
            }),
            scale: p.scale,
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_native_picture(n: *mut DhdrNative, p: *const DhdrPicture, out: *mut DhdrPictureResult) -> i32 {
    const E: &str = "dhdr_native_picture";
    let _g = Guard::enter(E);
    // SAFETY: p and out are a picture and an output (the header).
    let (p, out) = unsafe { (must(p as *mut DhdrPicture, E), must(out, E)) };
    if p.size as usize != std::mem::size_of::<DhdrPicture>() {
        log_err(E, &format!("DhdrPicture.size {} (this library's is {})", p.size, std::mem::size_of::<DhdrPicture>()));
        return 0;
    }
    // SAFETY: native_snapshot.inc fills every array with its count, alive for the call.
    let pic = unsafe { picture(p, E) };
    // SAFETY: as native().
    unsafe { native(n, E) }.with(E, |n| match n.picture(&pic) {
        Ok(r) => {
            *out = r;
            1
        }
        Err(e) => {
            log_err(E, &e);
            0
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_native_present(n: *mut DhdrNative, s: *mut DhdrSurface, r: *const DhdrPresentRect) -> i32 {
    const E: &str = "dhdr_native_present";
    let _g = Guard::enter(E);
    // SAFETY: r is a DhdrPresentRect (the header).
    let r = unsafe { must(r as *mut DhdrPresentRect, E) };
    // SAFETY: as native() and surface().
    unsafe { native(n, E) }.with(E, |n| unsafe { surface(s, E) }.with(E, |s| n.present(s, r)))
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_native_read_begin(n: *mut DhdrNative, w: *mut u32, h: *mut u32) -> i32 {
    const E: &str = "dhdr_native_read_begin";
    let _g = Guard::enter(E);
    // SAFETY: w and h are outputs (the header).
    let (w, h) = unsafe { (must(w, E), must(h, E)) };
    // SAFETY: as native().
    unsafe { native(n, E) }.begin(E, |n| match n.read_begin() {
        Some((rw, rh)) => {
            (*w, *h) = (rw, rh);
            (1, true)
        }
        None => {
            (*w, *h) = (0, 0);
            (0, false)
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_native_read_finish(n: *mut DhdrNative, pixels: *mut u32) -> i32 {
    const E: &str = "dhdr_native_read_finish";
    let _g = Guard::enter(E);
    // SAFETY: as native().
    unsafe { native(n, E) }.finish(E, "dhdr_native_read_begin", |n| {
        let (w, h) = n.read_size();
        // SAFETY: pixels holds the w * h pixels begin announced.
        let pixels = unsafe { slice_mut(pixels, w as usize * h as usize, E) };
        n.read_finish(pixels) as i32
    })
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_native_zread_start(n: *mut DhdrNative, index: u32, rect: *const u32, tag: u64) -> i32 {
    const E: &str = "dhdr_native_zread_start";
    let _g = Guard::enter(E);
    // SAFETY: rect is uint32_t[4] (the header).
    let rect = unsafe { slice(rect, 4, E) };
    let rect = [rect[0], rect[1], rect[2], rect[3]];
    // SAFETY: as native().
    unsafe { native(n, E) }.with(E, |n| n.zread_start(index, rect, tag))
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_native_zread_take(n: *mut DhdrNative, out: *mut u32, cap: u32, rect_out: *mut u32,
                                                tag_out: *mut u64) -> i32 {
    const E: &str = "dhdr_native_zread_take";
    let _g = Guard::enter(E);
    // SAFETY: out holds cap words, rect_out 4, tag_out one (the header).
    let (out, rect_out, tag_out) = unsafe { (slice_mut(out, cap as usize, E), slice_mut(rect_out, 4, E), must(tag_out, E)) };
    let mut rect = [0u32; 4];
    // SAFETY: as native().
    let taken = unsafe { native(n, E) }.with(E, |n| n.zread_take(out, &mut rect, tag_out));
    if taken == 1 {
        rect_out.copy_from_slice(&rect);
    }
    taken
}

#[no_mangle]
pub unsafe extern "C" fn dhdr_native_options(n: *mut DhdrNative, flags: u32) {
    const E: &str = "dhdr_native_options";
    let _g = Guard::enter(E);
    // SAFETY: as native().
    unsafe { native(n, E) }.with(E, |n| n.options(flags))
}

// ---------------------------------------------------------------- texture packs' PNG files

#[cfg(feature = "native")]
const _: dhdr_png_size_fn = dhdr_png_size;
#[cfg(feature = "native")]
const _: dhdr_png_decode_fn = dhdr_png_decode;

#[cfg(feature = "native")]
#[no_mangle]
pub unsafe extern "C" fn dhdr_png_size(data: *const u8, size: usize, w: *mut u32, h: *mut u32) -> i32 {
    const E: &str = "dhdr_png_size";
    let _g = Guard::enter(E);
    // SAFETY: size bytes at data; w and h are outputs (the header).
    let (data, w, h) = unsafe { (slice(data, size, E), must(w, E), must(h, E)) };
    match crate::png_decode::size(data) {
        Ok(size) => {
            (*w, *h) = size;
            1
        }
        Err(e) => {
            log_err(E, &e);
            0
        }
    }
}

#[cfg(feature = "native")]
#[no_mangle]
pub unsafe extern "C" fn dhdr_png_decode(data: *const u8, size: usize, rgba: *mut u8, w: u32, h: u32, opaque: u32) -> i32 {
    const E: &str = "dhdr_png_decode";
    let _g = Guard::enter(E);
    let Some(bytes) = (w as usize).checked_mul(h as usize).and_then(|p| p.checked_mul(4)) else {
        log_err(E, &format!("size {w} x {h}"));
        return 0;
    };
    // SAFETY: size bytes at data, w * h * 4 writable bytes at rgba (the header).
    let (data, rgba) = unsafe { (slice(data, size, E), slice_mut(rgba, bytes, E)) };
    match crate::png_decode::decode(data, rgba, w, h, opaque as u8) {
        Ok(()) => 1,
        Err(e) => {
            log_err(E, &e);
            0
        }
    }
}

// ---------------------------------------------------------------- browser

#[cfg(target_arch = "wasm32")]
const _: dhdr_web_surface_fn = dhdr_web_surface;

#[cfg(target_arch = "wasm32")]
#[no_mangle]
pub unsafe extern "C" fn dhdr_web_surface() -> *mut DhdrSurface {
    let _g = Guard::enter("dhdr_web_surface");
    crate::web::init::surface()
}

// ---------------------------------------------------------------- test hooks

#[cfg(feature = "test-hooks")]
const _: dhdr_test_depth_probe_fn = dhdr_test_depth_probe;
#[cfg(feature = "test-hooks")]
const _: dhdr_test_presented_fn = dhdr_test_presented;

#[cfg(feature = "test-hooks")]
#[no_mangle]
pub unsafe extern "C" fn dhdr_test_depth_probe(cases: *const c_void, count: u32, results: *mut u32) -> i32 {
    const E: &str = "dhdr_test_depth_probe";
    let _g = Guard::enter(E);
    let Some(gpu) = gpu_or_log(E) else { return 0 };
    // SAFETY: count cases of 80 bytes in, 8 bytes out each (the header).
    let (cases, results) = unsafe {
        (slice(cases as *const u8, count as usize * 80, E), slice_mut(results, count as usize * 2, E))
    };
    match crate::test_hooks::depth_probe::run(gpu, cases, results) {
        Ok(()) => 1,
        Err(e) => {
            log_err(E, &e);
            0
        }
    }
}

#[cfg(feature = "test-hooks")]
#[no_mangle]
pub unsafe extern "C" fn dhdr_test_presented(w: *mut u32, h: *mut u32) -> *const u32 {
    const E: &str = "dhdr_test_presented";
    let _g = Guard::enter(E);
    // SAFETY: w and h are outputs (the header).
    let (w, h) = unsafe { (must(w, E), must(h, E)) };
    let (pixels, pw, ph) = crate::test_hooks::presented::last();
    (*w, *h) = (pw, ph);
    pixels
}
