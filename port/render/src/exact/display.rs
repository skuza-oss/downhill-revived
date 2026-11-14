//! The display texture, scanout's bind group, and capture (task A4).
//!
//! As the retired `gpu_wgpu.c`'s display_texture / wgpu_capture (last at render/rust 3882d5576): an
//! RGBA8 texture of the picture's size
//! (at most 1024 x 1024; usages STORAGE_BINDING | COPY_SRC | COPY_DST | TEXTURE_BINDING), remade
//! when the size changes, with scanout's bind group (vram at 1, display constants at 5, the view
//! at 7) and a readback buffer with 256-byte rows. `record(DHDR_P_SCANOUT)` sizes it (encoder.rs
//! calls `Display::ensure`, then dispatches scanout with `Display::scanout_group`). Capture copies
//! it to the readback buffer, submits, maps (begin), then writes the rows unpadded (finish). The
//! present pass (present/) samples `Display::view`; a movie or CPU-scanout frame goes in through
//! `Exact::display_upload` (gpu_wgpu.c's wgpu_present `pixels` branch).
//!
//! The display texture exists only once a scanout or an upload has sized it, and is never
//! released before the `Exact` is, so "there is a texture" is exactly the C front end's
//! `display_valid` (`gpu_exact.inc`: set after the first scanout, never cleared): capture_begin
//! answers None (C: a 0 x 0 CAPTURE) before that.
//!
//! What this file needs from its neighbours is all in `scanout_inputs` (scanout's pipeline from
//! pipelines.rs, the vram and display-constants buffers from buffers.rs).
//!
//! Referee: optrace_replay on traces with SCANOUT and CAPTURE (A4).
use crate::abi::{DHDR_B_DISPLAY, DHDR_B_VRAM};
use crate::device::Gpu;
use crate::exact::buffers::Buffers;
use crate::exact::pipelines::Pipelines;
use crate::exact::Exact;
use std::sync::{Arc, Mutex};

/// The largest display (gpu_exact.inc's exact_display and the header's present rule: <= 1024^2).
pub const MAX_SIDE: u32 = 1024;

/// A texture copy's bytes_per_row must be a multiple of this (an 80-pixel display has 320-byte rows).
const ROW_ALIGN: u32 = wgpu::COPY_BYTES_PER_ROW_ALIGNMENT;

/// What the map callback reported: None until it ran.
type MapStatus = Arc<Mutex<Option<Result<(), wgpu::BufferAsyncError>>>>;

/// The display texture at one size, with everything made for that size.
struct Target {
    texture: wgpu::Texture,
    view: wgpu::TextureView,
    /// scanout's bind group: vram at 1, display constants at 5, the view at 7.
    group: wgpu::BindGroup,
    /// MAP_READ | COPY_DST, `stride * height` bytes.
    readback: wgpu::Buffer,
    width: u32,
    height: u32,
    /// `width * 4` rounded up to 256.
    stride: u32,
}

pub struct Display {
    target: Option<Target>,
    /// The capture in flight between capture_begin (1) and capture_finish: its map's status.
    capture: Option<MapStatus>,
}

/// scanout's pipeline, the vram buffer and the display constants buffer (the C path's `wg.scanout`,
/// `p->vram`, `p->display_constants`). The only place this file reaches into pipelines.rs (A2) and
/// buffers.rs (A3).
fn scanout_inputs<'a>(pipelines: &'a Pipelines, buffers: &'a Buffers)
                      -> (&'a wgpu::ComputePipeline, &'a wgpu::Buffer, &'a wgpu::Buffer) {
    (&pipelines.scanout, buffers.get(DHDR_B_VRAM), buffers.get(DHDR_B_DISPLAY))
}

impl Display {
    pub fn new() -> Display {
        Display { target: None, capture: None }
    }

    /// The texture (and scanout's bind group) at width x height, remade when the size changed.
    pub fn ensure(&mut self, gpu: &Gpu, pipelines: &Pipelines, buffers: &Buffers, width: u32, height: u32)
                  -> Result<(), String> {
        if let Some(t) = &self.target {
            if t.width == width && t.height == height {
                return Ok(());
            }
        }
        if self.capture.is_some() {
            // Owned<T> keeps every other call out between a begin and its finish.
            return Err("exact::display: the display texture resized while a capture is in flight".into());
        }
        // gpu_wgpu.c's display_release: the old size's objects go first, whatever happens next.
        self.target = None;
        if width == 0 || height == 0 || width > MAX_SIDE || height > MAX_SIDE {
            return Err(format!("exact::display: a {width} x {height} display (1..={MAX_SIDE} each way)"));
        }
        let (scanout, vram, constants) = scanout_inputs(pipelines, buffers);
        let device = &gpu.device;
        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("display"),
            size: wgpu::Extent3d { width, height, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::STORAGE_BINDING
                | wgpu::TextureUsages::COPY_SRC
                | wgpu::TextureUsages::COPY_DST
                | wgpu::TextureUsages::TEXTURE_BINDING,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        let stride = (width * 4).div_ceil(ROW_ALIGN) * ROW_ALIGN;
        let readback = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("display readback"),
            size: stride as u64 * height as u64,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("scanout"),
            layout: &scanout.get_bind_group_layout(0),
            entries: &[
                wgpu::BindGroupEntry { binding: 1, resource: vram.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 5, resource: constants.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 7, resource: wgpu::BindingResource::TextureView(&view) },
            ],
        });
        self.target = Some(Target { texture, view, group, readback, width, height, stride });
        Ok(())
    }

    /// scanout's bind group at the current size (encoder.rs dispatches scanout with it right after
    /// `ensure`); None before the first `ensure`.
    pub fn scanout_group(&self) -> Option<&wgpu::BindGroup> {
        self.target.as_ref().map(|t| &t.group)
    }

    /// The display texture's view, for the present pass; None while there is no picture.
    pub fn view(&self) -> Option<&wgpu::TextureView> {
        self.target.as_ref().map(|t| &t.view)
    }

    /// The display texture, None while there is no picture.
    pub fn texture(&self) -> Option<&wgpu::Texture> {
        self.target.as_ref().map(|t| &t.texture)
    }

    /// The display's width and height; (0, 0) while there is no picture.
    pub fn size(&self) -> (u32, u32) {
        self.target.as_ref().map_or((0, 0), |t| (t.width, t.height))
    }

    /// Copies the texture to the readback buffer, submits, and starts mapping it (natively: waits
    /// until mapped). Pending exact work was submitted by the caller first.
    fn capture_start(&mut self, gpu: &Gpu) -> Option<(u32, u32)> {
        let t = self.target.as_ref()?;
        let mut encoder = gpu.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("capture") });
        encoder.copy_texture_to_buffer(
            wgpu::TexelCopyTextureInfo {
                texture: &t.texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::TexelCopyBufferInfo {
                buffer: &t.readback,
                layout: wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(t.stride),
                    rows_per_image: Some(t.height),
                },
            },
            wgpu::Extent3d { width: t.width, height: t.height, depth_or_array_layers: 1 },
        );
        crate::startup::submit(&gpu.queue, [encoder.finish()]);
        let status: MapStatus = Arc::new(Mutex::new(None));
        let s = status.clone();
        let bytes = t.stride as u64 * t.height as u64;
        t.readback.map_async(wgpu::MapMode::Read, 0..bytes, move |r| {
            *s.lock().unwrap_or_else(|p| p.into_inner()) = Some(r);
        });
        // Natively the map completes here (gpu_wgpu.c's map_read polled until done). In the browser
        // the map resolves while the reactor awaits it between begin and finish (web/wait.rs, B1).
        #[cfg(not(target_arch = "wasm32"))]
        if let Err(e) = gpu.device.poll(wgpu::PollType::wait_indefinitely()) {
            crate::host::log(&format!("dhd_render: exact capture: poll: {e}"));
        }
        self.capture = Some(status);
        Some((t.width, t.height))
    }

    /// Writes the mapped rows, unpadded, into `pixels` (width * height), and unmaps. 1 ok, 0 the map
    /// failed or there was no capture in flight (logged).
    fn capture_end(&mut self, pixels: &mut [u32]) -> i32 {
        let Some(status) = self.capture.take() else {
            crate::host::log("dhd_render: exact capture: no capture in flight");
            return 0;
        };
        let Some(t) = self.target.as_ref() else {
            crate::host::log("dhd_render: exact capture: no display texture");
            return 0;
        };
        let result = status.lock().unwrap_or_else(|p| p.into_inner()).take();
        match result {
            Some(Ok(())) => {}
            Some(Err(e)) => {
                crate::host::log(&format!("dhd_render: exact capture: map failed: {e}"));
                return 0;
            }
            None => {
                crate::host::log("dhd_render: exact capture: the map did not complete");
                return 0;
            }
        }
        let (w, h, stride) = (t.width as usize, t.height as usize, t.stride as usize);
        if pixels.len() < w * h {
            crate::host::log(&format!("dhd_render: exact capture: {} pixels for a {w} x {h} display", pixels.len()));
            t.readback.unmap();
            return 0;
        }
        {
            let mapped = t.readback.get_mapped_range(0..(stride * h) as u64);
            let out = as_bytes_mut(&mut pixels[..w * h]);
            for (y, row) in out.chunks_exact_mut(w * 4).enumerate() {
                row.copy_from_slice(&mapped[y * stride..y * stride + w * 4]);
            }
        } // the view drops before unmap
        t.readback.unmap();
        1
    }
}

/// The pixels as their bytes, in memory order (C's memcpy of RGBA8 rows into uint32_t pixels).
fn as_bytes_mut(pixels: &mut [u32]) -> &mut [u8] {
    let len = std::mem::size_of_val(pixels);
    // SAFETY: u8 has alignment 1 and no invalid values; the slice covers exactly the same bytes
    // and borrows `pixels` mutably for its whole life.
    unsafe { std::slice::from_raw_parts_mut(pixels.as_mut_ptr().cast::<u8>(), len) }
}

/// The pixels as their bytes, in memory order.
fn as_bytes(pixels: &[u32]) -> &[u8] {
    let len = std::mem::size_of_val(pixels);
    // SAFETY: as as_bytes_mut, shared.
    unsafe { std::slice::from_raw_parts(pixels.as_ptr().cast::<u8>(), len) }
}

impl Exact {
    /// `dhdr_exact_capture_begin`: the size, or None when there is no display texture.
    pub fn capture_begin(&mut self) -> Option<(u32, u32)> {
        self.display.target.as_ref()?;
        // The scanout and anything else recorded go before the copy (gpu_wgpu.c recorded the copy
        // into the open encoder and submits it all; two submits in order are the same).
        self.submit();
        let gpu = self.gpu;
        self.display.capture_start(gpu)
    }

    /// The size the capture in flight returns (finish's pixel count is its product).
    pub fn capture_size(&self) -> (u32, u32) {
        if self.display.capture.is_some() { self.display.size() } else { (0, 0) }
    }

    /// `dhdr_exact_capture_finish`: 1 ok, 0 the map failed.
    pub fn capture_finish(&mut self, pixels: &mut [u32]) -> i32 {
        self.display.capture_end(pixels)
    }

    /// A movie or CPU-scanout frame into the display texture, as scanout would leave it
    /// (gpu_wgpu.c's wgpu_present with pixels): sized to width x height (<= 1024 each way), then a
    /// queue write of the `width * height` RGBA8 pixels (lands before the next submit).
    pub fn display_upload(&mut self, pixels: &[u32], width: u32, height: u32) -> Result<(), String> {
        if pixels.len() < width as usize * height as usize {
            return Err(format!("exact::display: {} pixels for a {width} x {height} frame", pixels.len()));
        }
        let gpu = self.gpu;
        self.display.ensure(gpu, &self.pipelines, &self.buffers, width, height)?;
        let texture = self.display.texture().expect("ensure made the display texture");
        gpu.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            as_bytes(&pixels[..width as usize * height as usize]),
            wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(width * 4), rows_per_image: Some(height) },
            wgpu::Extent3d { width, height, depth_or_array_layers: 1 },
        );
        Ok(())
    }
}
