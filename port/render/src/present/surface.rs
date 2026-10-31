//! Window surfaces (task A5; the browser's OffscreenCanvas surface is B1's, through
//! `Surface::from_wgpu`).
//!
//! As the retired `gpu_wgpu.c`'s wgpu_surface / surface_configure / wgpu_present (last at
//! render/rust 3882d5576): a surface on the window
//! (each native build takes its platform's kinds, `Surface::KINDS`: Win32's HWND, X11's or
//! Wayland's window, Android's ANativeWindow, Apple's CAMetalLayer), in RGBA8Unorm, else BGRA8Unorm
//! (`surface_format`: whichever the surface offers first in that order; Apple's CAMetalLayer has no
//! RGBA8), the present pipelines drawing into that format; present mode Immediate > Mailbox > Fifo (`DHDR_PRESENT_NO_TEARING`: no Immediate;
//! `DHDR_PRESENT_VSYNC`: Fifo); desired maximum frame latency 1; alpha Opaque; usage
//! RENDER_ATTACHMENT, plus COPY_SRC when the surface offers it (`DHDR_SURFACE_COPY_SRC`, for the
//! test hook). Configured to the window's size, reconfigured when it changes or the frame is
//! Outdated / Lost (once; a second Outdated / Lost fails, as C's wgpu_present); Timeout and
//! Occluded skip the frame. `present_exact` submits the exact work first (the scanout, as C's
//! `submit_all`), then draws the display texture (or CPU / movie pixels written into it first
//! through `Exact::display_upload`, as scanout would) with present.wgsl (`pipe.rs`); no display
//! texture yet skips. The acquired frame stays here until `present`; the export runs the test
//! hook on it first (`frame_texture`). Never blocks on vsync, never suspends.
//!
//! Referee: `test_gs_wgpu.py --impl rust --arch x86_64 --only present` (A5).
use crate::abi::*;
use crate::device::Gpu;
use crate::exact::display::Display;
use crate::exact::Exact;
use crate::present::pipe::PresentPipe;

/// Stand-ins for `exact/display.rs`'s accessors (A4: `Display::view`, `Display::size`,
/// `Exact::display_upload`, same signatures) while render/rust still has the skeleton's
/// `Display`. Inherent methods take precedence over trait methods, so once A4's display.rs is
/// merged the calls below reach A4's methods and these defaults go unused: delete both traits
/// then. Until then an exact frame has no display texture and is skipped (0).
#[allow(dead_code)]
trait SkeletonDisplay {
    fn view(&self) -> Option<&wgpu::TextureView> {
        None
    }
    fn size(&self) -> (u32, u32) {
        (0, 0)
    }
}

impl SkeletonDisplay for Display {}

#[allow(dead_code)]
trait SkeletonExact {
    fn display_upload(&mut self, pixels: &[u32], width: u32, height: u32) -> Result<(), String> {
        let _ = pixels;
        Err(format!("no display texture for a {width} x {height} frame (exact/display.rs not merged)"))
    }
}

impl SkeletonExact for Exact {}

/// The picture texture of a surface used without an exact renderer (pixels only).
struct Own {
    texture: wgpu::Texture,
    view: wgpu::TextureView,
    width: u32,
    height: u32,
}

pub struct Surface {
    pub gpu: &'static Gpu,
    /// The acquired frame, until `present` (dropped before the surface).
    frame: Option<wgpu::SurfaceTexture>,
    surface: wgpu::Surface<'static>,
    usage: wgpu::TextureUsages,
    present_mode: wgpu::PresentMode,
    alpha_mode: wgpu::CompositeAlphaMode,
    /// The frames' format (`surface_format`; the present pipelines target it).
    format: wgpu::TextureFormat,
    /// The configured size; 0 x 0 before the first configure.
    width: u32,
    height: u32,
    pipe: PresentPipe,
    pub counter: super::counter::Counter,
    #[cfg(feature = "native")]
    capture: super::counter::Capture,
    own: Option<Own>,
}

/// `pixels` as bytes (RGBA8, little-endian words).
fn bytes_of(pixels: &[u32]) -> &[u8] {
    // SAFETY: u32 has no padding and every byte pattern is a valid u8; the length is exact.
    unsafe { std::slice::from_raw_parts(pixels.as_ptr() as *const u8, std::mem::size_of_val(pixels)) }
}

/// Writes w x h `pixels` into `texture` (queue write: lands before the next submit).
fn write_pixels(gpu: &Gpu, texture: &wgpu::Texture, pixels: &[u32], w: u32, h: u32) {
    gpu.queue.write_texture(
        wgpu::TexelCopyTextureInfo {
            texture,
            mip_level: 0,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        bytes_of(&pixels[..w as usize * h as usize]),
        wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(w * 4), rows_per_image: Some(h) },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
}

/// The window kinds this build takes (`dhdr_surface_create`): its platform's.
#[cfg(windows)]
const PLATFORM_KINDS: &[u32] = &[DHDR_WINDOW_WIN32];
#[cfg(target_os = "android")]
const PLATFORM_KINDS: &[u32] = &[DHDR_WINDOW_ANDROID];
#[cfg(target_vendor = "apple")]
const PLATFORM_KINDS: &[u32] = &[DHDR_WINDOW_METAL_LAYER];
#[cfg(all(unix, not(target_os = "android"), not(target_vendor = "apple")))]
const PLATFORM_KINDS: &[u32] = &[DHDR_WINDOW_XLIB, DHDR_WINDOW_WAYLAND];
#[cfg(target_arch = "wasm32")]
const PLATFORM_KINDS: &[u32] = &[];

/// The frames' format among the surface's: RGBA8Unorm, the format of every picture the renderer
/// draws (and of the browser's canvas), else BGRA8Unorm (the same bytes, swizzled by the present
/// pass's target: Apple's CAMetalLayer, some Vulkan drivers). None when it has neither.
pub fn surface_format(formats: &[wgpu::TextureFormat]) -> Option<wgpu::TextureFormat> {
    [wgpu::TextureFormat::Rgba8Unorm, wgpu::TextureFormat::Bgra8Unorm].into_iter().find(|f| formats.contains(f))
}

/// A non-null handle of `what`.
#[cfg(not(target_arch = "wasm32"))]
fn handle(value: u64, what: &str) -> Result<std::ptr::NonNull<std::ffi::c_void>, String> {
    std::ptr::NonNull::new(value as usize as *mut std::ffi::c_void).ok_or_else(|| format!("NULL {what}"))
}

impl Surface {
    pub const KINDS: &'static [u32] = PLATFORM_KINDS;

    /// `dhdr_surface_create` takes `kind` in this build.
    pub fn supports(kind: u32) -> bool {
        Self::KINDS.contains(&kind)
    }

    /// `dhdr_surface_create`: a surface on the window, then present.wgsl's pipeline into its format.
    pub fn create(gpu: &'static Gpu, window: &DhdrWindow, present_flags: u32) -> Result<Surface, String> {
        if !Self::supports(window.kind) {
            return Err(format!("window kind {} is not supported", window.kind));
        }
        let surface = Self::window(gpu, window)?;
        let formats = surface.get_capabilities(&gpu.adapter).formats;
        let format = surface_format(&formats)
            .ok_or_else(|| format!("the surface has neither rgba8unorm nor bgra8unorm (formats {formats:?})"))?;
        let pipe = PresentPipe::create(gpu, format).ok_or("no present pipeline")?;
        Self::from_wgpu(gpu, surface, present_flags, pipe)
    }

    /// The wgpu surface on `window` (a kind of `KINDS`: `create` checked it). C keeps the window
    /// alive until dhdr_surface_destroy (present.c destroys the surface before the window, and on
    /// a phone drops it when the app goes to the background, before SDL releases the window).
    #[cfg(not(target_arch = "wasm32"))]
    fn window(gpu: &Gpu, window: &DhdrWindow) -> Result<wgpu::Surface<'static>, String> {
        use raw_window_handle::{RawDisplayHandle as D, RawWindowHandle as W};
        let target = match window.kind {
            // a = the CAMetalLayer of SDL's Metal view (SDL_Metal_GetLayer); wgpu has the variant where
            // its Metal backend is built
            #[cfg(target_vendor = "apple")]
            DHDR_WINDOW_METAL_LAYER => wgpu::SurfaceTargetUnsafe::CoreAnimationLayer(handle(window.a, "CAMetalLayer")?.as_ptr()),
            kind => {
                let (display, window) = match kind {
                    DHDR_WINDOW_WIN32 => {
                        let hwnd = std::num::NonZeroIsize::new(window.a as isize).ok_or("NULL HWND")?;
                        let mut w = raw_window_handle::Win32WindowHandle::new(hwnd);
                        w.hinstance = std::num::NonZeroIsize::new(window.b as isize);
                        (D::Windows(raw_window_handle::WindowsDisplayHandle::new()), W::Win32(w))
                    }
                    // a = the ANativeWindow (SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER), on Vulkan
                    // (finding 091); the Vulkan surface holds its own reference to it
                    DHDR_WINDOW_ANDROID => (D::Android(raw_window_handle::AndroidDisplayHandle::new()),
                                            W::AndroidNdk(raw_window_handle::AndroidNdkWindowHandle::new(handle(window.a, "ANativeWindow")?))),
                    // a = the X11 Display, b = the Window (SDL_PROP_WINDOW_X11_*)
                    DHDR_WINDOW_XLIB => {
                        let id = std::ffi::c_ulong::try_from(window.b).ok().filter(|&id| id != 0).ok_or("no X11 window")?;
                        (D::Xlib(raw_window_handle::XlibDisplayHandle::new(Some(handle(window.a, "X11 Display")?), 0)),
                         W::Xlib(raw_window_handle::XlibWindowHandle::new(id)))
                    }
                    // a = the wl_display, b = the wl_surface (SDL_PROP_WINDOW_WAYLAND_*)
                    DHDR_WINDOW_WAYLAND => (D::Wayland(raw_window_handle::WaylandDisplayHandle::new(handle(window.a, "wl_display")?)),
                                            W::Wayland(raw_window_handle::WaylandWindowHandle::new(handle(window.b, "wl_surface")?))),
                    kind => return Err(format!("window kind {kind} is not supported")),
                };
                wgpu::SurfaceTargetUnsafe::RawHandle { raw_display_handle: Some(display), raw_window_handle: window }
            }
        };
        // SAFETY: the window outlives the surface (above).
        unsafe { gpu.instance.create_surface_unsafe(target) }.map_err(|e| format!("create_surface: {e}"))
    }

    #[cfg(target_arch = "wasm32")]
    fn window(gpu: &Gpu, window: &DhdrWindow) -> Result<wgpu::Surface<'static>, String> {
        let _ = (gpu, window);
        Err("no window surfaces in the browser (dhdr_web_surface)".into())
    }

    /// A surface over `surface` (the window's, or B1's OffscreenCanvas) in `pipe`'s format: checks
    /// the capabilities and picks the present mode, alpha mode and usage. Configured on the first
    /// acquire.
    pub fn from_wgpu(gpu: &'static Gpu, surface: wgpu::Surface<'static>, present_flags: u32, pipe: PresentPipe)
                     -> Result<Surface, String> {
        let caps = surface.get_capabilities(&gpu.adapter);
        let format = pipe.format;
        if !caps.formats.contains(&format) {
            return Err(format!("the surface has no {format:?} (formats {:?})", caps.formats));
        }
        let offers = |m: wgpu::PresentMode| caps.present_modes.contains(&m);
        let present_mode = if present_flags & DHDR_PRESENT_VSYNC != 0 {
            wgpu::PresentMode::Fifo
        } else if present_flags & DHDR_PRESENT_NO_TEARING == 0 && offers(wgpu::PresentMode::Immediate) {
            wgpu::PresentMode::Immediate
        } else if offers(wgpu::PresentMode::Mailbox) {
            wgpu::PresentMode::Mailbox
        } else {
            wgpu::PresentMode::Fifo
        };
        let alpha_mode = if caps.alpha_modes.contains(&wgpu::CompositeAlphaMode::Opaque) || caps.alpha_modes.is_empty() {
            wgpu::CompositeAlphaMode::Opaque
        } else {
            caps.alpha_modes[0]
        };
        // COPY_SRC lets the test hook read the presented texture back; not needed otherwise.
        let usage = wgpu::TextureUsages::RENDER_ATTACHMENT | (caps.usages & wgpu::TextureUsages::COPY_SRC);
        Ok(Surface { gpu, frame: None, surface, usage, present_mode, alpha_mode, format, width: 0, height: 0, pipe, own: None, counter: super::counter::Counter::default(),
            #[cfg(feature = "native")] capture: super::counter::Capture::default() })
    }

    /// `DHDR_SURFACE_COPY_SRC` when the frames can be copied from.
    pub fn usage(&self) -> u32 {
        if self.usage.contains(wgpu::TextureUsages::COPY_SRC) {
            DHDR_SURFACE_COPY_SRC
        } else {
            0
        }
    }

    /// The frames' format (the native present pass's target, `Objects::present_pipe`).
    pub fn format(&self) -> wgpu::TextureFormat {
        self.format
    }

    /// The configured size (the present pass's target size).
    pub fn size(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    /// DHD_FRAME_LATENCY (1-3, default 1): pictures the swapchain may hold. 1 has the least input
    /// delay, but each acquire then waits until the compositor (G-Sync, overlays) has released the
    /// last picture; 2 or 3 give the render thread room when that release is late (stutter tests).
    fn configure(&mut self, width: u32, height: u32) {
        let max = self.gpu.device.limits().max_texture_dimension_2d;
        let (width, height) = (width.min(max), height.min(max));
        self.surface.configure(&self.gpu.device, &wgpu::SurfaceConfiguration {
            usage: self.usage,
            format: self.format,
            width,
            height,
            present_mode: self.present_mode,
            desired_maximum_frame_latency: frame_latency(),
            alpha_mode: self.alpha_mode,
            view_formats: vec![],
        });
        self.width = width;
        self.height = height;
    }

    /// Configures to window_w x window_h (when that changed, or `reconfigure`), then acquires the
    /// frame: Ok(Some(view)) acquired and kept until `present`; Ok(None) skipped (0 x 0, Timeout,
    /// Occluded); Err failed. The native present pass (native/present.rs) draws through this too.
    pub fn acquire(&mut self, window_w: u32, window_h: u32, reconfigure: bool) -> Result<Option<wgpu::TextureView>, String> {
        self.frame = None; // a frame drawn but never presented is dropped
        if window_w == 0 || window_h == 0 {
            return Ok(None);
        }
        // Android destroys the window's surface on its own thread when the app leaves the screen;
        // a present racing it gets validation errors (configure: "Surface does not support the
        // adapter's queue family"). There they fail this present instead of the process; the C
        // side recreates the surface when the app comes back (gs_gpu_surface_reset).
        #[cfg(target_os = "android")]
        {
            let scope = self.gpu.device.push_error_scope(wgpu::ErrorFilter::Validation);
            let acquired = self.acquire_unscoped(window_w, window_h, reconfigure);
            if let Some(e) = pollster::block_on(scope.pop()) {
                self.frame = None;
                return Err(format!("the surface is gone: {e}"));
            }
            acquired
        }
        #[cfg(not(target_os = "android"))]
        self.acquire_unscoped(window_w, window_h, reconfigure)
    }

    fn acquire_unscoped(&mut self, window_w: u32, window_h: u32, reconfigure: bool) -> Result<Option<wgpu::TextureView>, String> {
        let max = self.gpu.device.limits().max_texture_dimension_2d;
        if reconfigure || (window_w.min(max), window_h.min(max)) != (self.width, self.height) {
            self.configure(window_w, window_h);
        }
        for attempt in 0..2 {
            match self.surface.get_current_texture() {
                wgpu::CurrentSurfaceTexture::Success(t) | wgpu::CurrentSurfaceTexture::Suboptimal(t) => {
                    let view = t.texture.create_view(&wgpu::TextureViewDescriptor::default());
                    self.frame = Some(t);
                    return Ok(Some(view));
                }
                wgpu::CurrentSurfaceTexture::Timeout | wgpu::CurrentSurfaceTexture::Occluded => return Ok(None),
                wgpu::CurrentSurfaceTexture::Outdated | wgpu::CurrentSurfaceTexture::Lost if attempt == 0 => {
                    self.configure(window_w, window_h);
                }
                wgpu::CurrentSurfaceTexture::Outdated => return Err("the surface stayed outdated after reconfiguring".into()),
                wgpu::CurrentSurfaceTexture::Lost => return Err("the surface was lost".into()),
                wgpu::CurrentSurfaceTexture::Validation => return Err("get_current_texture: validation error".into()),
            }
        }
        unreachable!("the second attempt always returns")
    }

    /// The acquired frame's texture (the test hook reads it before `present`), if any.
    pub fn frame_texture(&self) -> Option<&wgpu::Texture> {
        self.frame.as_ref().map(|f| &f.texture)
    }

    /// The surface-owned picture texture at w x h (pixels presented without an exact renderer).
    fn own_texture(&mut self, w: u32, h: u32) -> &Own {
        if self.own.as_ref().is_none_or(|o| (o.width, o.height) != (w, h)) {
            let texture = self.gpu.device.create_texture(&wgpu::TextureDescriptor {
                label: Some("present pixels"),
                size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: wgpu::TextureFormat::Rgba8Unorm,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            });
            let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
            self.own = Some(Own { texture, view, width: w, height: h });
        }
        self.own.as_ref().expect("own texture")
    }

    /// `dhdr_present_exact`: 1 acquired and drawn, 0 skipped, -1 failed (logged). As gpu_wgpu.c's
    /// wgpu_present: pending exact work (the scanout) is submitted first; `pixels` (w x h, checked
    /// by `ffi.rs`) go into the exact display texture, as scanout would, so a later capture returns
    /// them, even when the window is minimised; without an exact renderer they go into a texture
    /// of the surface's own. No display texture yet (no scanout, no pixels) skips.
    pub fn present_exact(&mut self, exact: Option<&mut Exact>, pixels: Option<&[u32]>, w: u32, h: u32,
                         window_w: u32, window_h: u32) -> i32 {
        let gpu = self.gpu;
        let log = |e: &str| crate::host::log(&format!("dhd_render: present: {e}"));
        // The picture: (view, width, height).
        let source = match exact {
            Some(e) => {
                e.submit(); // the scanout, before the picture is replaced or shown
                e.frame_end(); // the GPU one frame behind at most, minimised too
                if let Some(pixels) = pixels {
                    // C returns 0 (skip, keep presenting) for a frame the display texture can't hold.
                    if let Err(err) = e.display_upload(pixels, w, h) {
                        log(&err);
                        return 0;
                    }
                }
                let Some(view) = e.display.view() else { return 0 };
                let (dw, dh) = e.display.size();
                (view.clone(), dw, dh)
            }
            None => match pixels {
                Some(pixels) => {
                    let own = self.own_texture(w, h);
                    write_pixels(gpu, &own.texture, pixels, w, h);
                    (own.view.clone(), w, h)
                }
                None => return 0, // nothing to show
            },
        };
        let target = match self.acquire(window_w, window_h, false) {
            Ok(Some(view)) => view,
            Ok(None) => return 0,
            Err(e) => {
                log(&e);
                return -1;
            }
        };
        let mut encoder = gpu.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("present") });
        self.pipe.draw(gpu, &mut encoder, &source.0, &target, [source.1, source.2, self.width, self.height]);
        gpu.queue.submit([encoder.finish()]);
        1
    }

    pub fn draw_counter(&mut self) {
        if let Some(frame) = self.frame.as_ref() {
            let target = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
            self.counter.draw(self.gpu, &target, self.format, self.width, self.height);
            #[cfg(feature = "native")]
            if self.usage.contains(wgpu::TextureUsages::COPY_SRC) { self.capture.frame(self.gpu, &frame.texture); }
        }
    }

    /// `dhdr_surface_present`: presents the frame the last call that returned 1 drew.
    pub fn present(&mut self) {
        if let Some(frame) = self.frame.take() {
            frame.present();
        }
    }
}

/// DHD_FRAME_LATENCY, read once (Surface::configure).
fn frame_latency() -> u32 {
    static LATENCY: std::sync::OnceLock<u32> = std::sync::OnceLock::new();
    *LATENCY.get_or_init(|| {
        std::env::var("DHD_FRAME_LATENCY").ok().and_then(|s| s.trim().parse::<u32>().ok()).map_or(1, |n| n.clamp(1, 3))
    })
}
