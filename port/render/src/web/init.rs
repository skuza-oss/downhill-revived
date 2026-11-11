//! The browser's async init (task B1): `dhdr_web_init(canvas, flags)`, a wasm-bindgen export that
//! worker.js awaits before the reactor runs. It makes the adapter and device (the limits the
//! retired JS renderer's gpu.js asked for),
//! installs it (`device::install`), makes the surface on the OffscreenCanvas (`dhdr_web_surface`),
//! builds every exact pipeline and awaits one prewarm dispatch (`on_submitted_work_done`).
//!
//! As the retired JS renderer's createGpu (port/web/www/gpu.js, last at render/rust 3882d5576): a
//! high-performance adapter; a device with no
//! features and the default limits except the adapter's `maxStorageBufferBindingSize`,
//! `maxBufferSize` and `maxTextureDimension2D` (the native renderer's upscaled surfaces); uncaptured errors are fatal (`guard::install_error_handler`: logged, then the
//! module traps), device loss is logged and makes every later read refuse (`wait::set_lost`).
//! `dhdr_device_init` never makes a device in the browser; it reports this one.
//!
//! Then, in order:
//! 1. present.wgsl's pipeline (awaited error scope). It is the probe of
//!    `DHDR_FEAT_RENDER_PIPELINES` and the surface's present pipe.
//! 2. raster.wgsl's pipelines for the raster key in `flags` (awaited error scopes; the pipeline
//!    cache keeps them, so the reactor's `dhdr_exact_create` with the same key reuses them).
//! 3. The native renderer's objects and first pipelines, and the mesh draws' pipelines inside an
//!    awaited error scope (`native::init::web_prepare`, finding 199): a refused mesh pipeline turns
//!    Native VU off before any mesh is drawn (`dhdr_native_meshes`), as natively.
//! 4. The prewarm (unless `WEB_NO_PREWARM`): a scratch exact renderer dispatches `main` over an
//!    empty batch record, `local_copy` and `scanout` over zero sizes (each returns at once, so
//!    nothing is drawn), submits, and awaits the queue. Under FXC the first dispatch of `main`
//!    stalls for seconds while the GPU process compiles it (finding 085, S4 question 7); this
//!    moves the stall here, out of the game's first read. The scratch renderer is destroyed.
//! 5. The surface on the OffscreenCanvas, when one is given (null / undefined: no surface, for
//!    headless replays). Fifo, opaque, RGBA8Unorm (`present::surface`), configured to the size C
//!    passes on the first present.
//!
//! Returns true when all of that exists. A second call after a success returns true at once and
//! changes nothing (the canvas argument is ignored then). A failure is logged; a failure before
//! the device exists latches it (`device::fail`), so `dhdr_device_init` keeps returning 0.
//!
//! `flags` (worker.js passes them; C never calls this):
//! - bits 0-2 (`WEB_RASTER`): the `DHDR_RASTER_*` key the reactor will pass to
//!   `dhdr_exact_create` (gpu_rust_web.c's WEB_RASTER_FLAGS: key 7, all three on);
//! - bit 3 (`WEB_NO_PREWARM`): skip step 4;
//! - bits 4-5 (`WEB_LOG_SHIFT`): `DHDR_LOG_*` for wgpu's diagnostics (0: errors only).
//!
//! `dhdr_web_surface` hands the surface to C once: C owns it after that (it may destroy it with
//! `dhdr_surface_destroy`), and later calls return NULL.
use crate::abi::DhdrSurface;
use std::sync::atomic::{AtomicPtr, Ordering};

#[cfg(feature = "web")]
use wasm_bindgen::prelude::*;

/// `flags` bits 0-2: the raster key (`DHDR_RASTER_*`).
pub const WEB_RASTER: u32 = 7;
/// `flags` bit 3: no prewarm dispatch.
pub const WEB_NO_PREWARM: u32 = 8;
/// `flags` bits 4-5: `DHDR_LOG_*` (0: errors only).
pub const WEB_LOG_SHIFT: u32 = 4;

/// DHD_TEST_NO_MESHES (finding 194's test hook) for the browser, whose Rust has no environment: the
/// page calls this with the variable's value before `dhdr_web_init` (graphics_common.js).
#[cfg(feature = "web")]
#[wasm_bindgen]
pub fn dhdr_web_test_no_meshes(at: u32) {
    crate::native::init::set_test_no_meshes(at);
}

/// The surface made by the init, until `dhdr_web_surface` hands it to C.
static SURFACE: AtomicPtr<DhdrSurface> = AtomicPtr::new(std::ptr::null_mut());

/// `dhdr_web_init`: true when the device, the surface and the pipelines exist.
#[cfg(feature = "web")]
#[wasm_bindgen]
pub async fn dhdr_web_init(canvas: JsValue, flags: u32) -> bool {
    crate::guard::install_hooks();
    if crate::device::gpu().is_some() {
        return true;
    }
    match imp::init(canvas, flags).await {
        Ok(()) => true,
        Err(e) => {
            crate::host::log(&format!("dhd_render: dhdr_web_init: {e}"));
            if crate::device::gpu().is_none() {
                crate::device::fail();
            }
            false
        }
    }
}

/// The surface `dhdr_web_init` made (`dhdr_web_surface`), as the handle `ffi.rs` hands to C;
/// NULL when there is none, or when C has it already.
pub fn surface() -> *mut DhdrSurface {
    SURFACE.swap(std::ptr::null_mut(), Ordering::AcqRel)
}

#[cfg(feature = "web")]
mod imp {
    use super::*;
    use crate::abi::*;
    use crate::device::{DeviceOptions, Gpu};
    use crate::exact::pipelines::Pipelines;
    use crate::exact::Exact;
    use crate::guard::Owned;
    use crate::present::pipe::PresentPipe;
    use crate::present::surface::Surface;

    fn now() -> f64 {
        js_sys::Date::now()
    }

    pub async fn init(canvas: JsValue, flags: u32) -> Result<(), String> {
        let t0 = now();
        let key = flags & WEB_RASTER;
        let log_level = ((flags >> WEB_LOG_SHIFT) & 3).max(DHDR_LOG_ERROR);
        crate::log::set_level(log_level);
        let canvas = if canvas.is_undefined() || canvas.is_null() {
            None
        } else {
            Some(canvas.dyn_into::<web_sys::OffscreenCanvas>().map_err(|_| "the canvas is not an OffscreenCanvas")?)
        };

        // ---- adapter and device, as the retired gpu.js made them
        let mut desc = wgpu::InstanceDescriptor::new_without_display_handle();
        desc.backends = wgpu::Backends::BROWSER_WEBGPU;
        let instance = wgpu::Instance::new(desc);
        let surface = match canvas {
            Some(c) => Some(instance.create_surface(wgpu::SurfaceTarget::OffscreenCanvas(c))
                .map_err(|e| format!("create_surface: {e}"))?),
            None => None,
        };
        let adapter = instance
            .request_adapter(&wgpu::RequestAdapterOptions {
                power_preference: wgpu::PowerPreference::HighPerformance,
                force_fallback_adapter: false,
                compatible_surface: surface.as_ref(),
            })
            .await
            .map_err(|e| format!("no WebGPU adapter: {e}"))?;
        let offered = adapter.limits();
        let mut limits = wgpu::Limits::default();
        limits.max_storage_buffer_binding_size = offered.max_storage_buffer_binding_size;
        limits.max_buffer_size = offered.max_buffer_size;
        // The native renderer's upscaled surfaces (DHD_SCALE times up to 2048 GS pixels); its
        // textures are clamped to this limit (native/surfaces.rs).
        limits.max_texture_dimension_2d = offered.max_texture_dimension_2d;
        let (device, queue) = adapter
            .request_device(&wgpu::DeviceDescriptor {
                label: Some("dhd_render"),
                required_features: wgpu::Features::empty(),
                required_limits: limits,
                ..Default::default()
            })
            .await
            .map_err(|e| format!("request_device: {e}"))?;
        crate::guard::install_error_handler(&device);
        device.set_device_lost_callback(|reason, message| {
            crate::host::log(&format!("dhd_render: WebGPU device lost ({reason:?}): {message}"));
            crate::web::wait::set_lost();
        });
        let t_device = now();

        // ---- the Gpu: present.wgsl's pipeline is the RENDER_PIPELINES probe
        let info = adapter.get_info();
        let label = if info.name.is_empty() { "WebGPU".to_owned() } else { info.name.clone() };
        let options = DeviceOptions { log_level, ..DeviceOptions::default() };
        let mut gpu = Gpu::from_parts(instance, adapter, device, queue, options, false);
        let pipe = PresentPipe::create_async(&gpu).await;
        if pipe.is_some() {
            gpu.features |= DHDR_FEAT_RENDER_PIPELINES;
        }
        if !crate::device::install(gpu) {
            return Err("a device exists already".into());
        }
        let gpu: &'static Gpu = crate::device::gpu().ok_or("the device was not installed")?;

        // ---- raster.wgsl's pipelines for the reactor's key (kept by the pipeline cache)
        Pipelines::create_async(gpu, key).await?;
        // ---- the native renderer's objects; the mesh pipelines in an awaited error scope
        crate::native::init::web_prepare(gpu).await?;
        let t_pipelines = now();

        // ---- the prewarm dispatch
        if flags & WEB_NO_PREWARM == 0 {
            prewarm(gpu, key).await?;
        }
        let t_prewarm = now();

        // ---- the surface on the canvas
        if let Some(surface) = surface {
            let pipe = pipe.ok_or("no present pipeline for the canvas")?;
            let s = Surface::from_wgpu(gpu, surface, DHDR_PRESENT_VSYNC, pipe)?;
            let handle = Box::into_raw(Box::new(Owned::new(s))) as *mut DhdrSurface;
            let old = SURFACE.swap(handle, Ordering::AcqRel);
            if !old.is_null() {
                // SAFETY: a handle this init made and C never took (the swap took it back).
                drop(unsafe { Box::from_raw(old as *mut Owned<Surface>) });
            }
        }
        let heap = crate::web::alloc::stats();
        crate::host::log(&format!(
            "dhd_render: browser renderer on {label}: device {:.0} ms, pipelines {:.0} ms, prewarm {}, heap {} KiB in use",
            t_device - t0,
            t_pipelines - t_device,
            if flags & WEB_NO_PREWARM == 0 { format!("{:.0} ms", t_prewarm - t_pipelines) } else { "off".into() },
            heap.used / 1024
        ));
        Ok(())
    }

    /// Step 3 of the module comment: every pipeline of `key` dispatched once over zero work.
    async fn prewarm(gpu: &'static Gpu, key: u32) -> Result<(), String> {
        let scope = gpu.device.push_error_scope(wgpu::ErrorFilter::Validation);
        let mut x = Exact::create(gpu, key).map_err(|e| format!("prewarm: {e}"))?;
        // main over an empty record (batch.x = 0); local_copy over a 0 x 0 copy (COPY is zero);
        // scanout over a 0 x 0 display (DISPLAY is zero) into a 1 x 1 display texture.
        x.write(DHDR_B_BATCHES, 256, &[0u8; 16]);
        x.batch(256, 1, 1);
        x.record(DHDR_P_LOCAL_COPY, 1, 1);
        x.record(DHDR_P_SCANOUT, 1 | 1 << 16, 1 | 1 << 16);
        x.submit();
        let error = scope.pop().await;
        crate::web::wait::work_done(&gpu.queue).await;
        x.destroy();
        match error {
            Some(e) => Err(format!("prewarm: {e}")),
            None => Ok(()),
        }
    }
}
