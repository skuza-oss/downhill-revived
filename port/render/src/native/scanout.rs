//! Scanout, movie scanout, the display size and reading the displayed picture (task N5).
//!
//! As `gpu_wgpu_native.inc`'s native_display_size, native_movie_scanout, native_scanout and
//! native_read_display (`:898-953,1028-1045` in the retired file, last at render/rust 3882d5576;
//! native_core.inc's display_width /
//! display_height): the picture's display surface scanned out by native.wgsl's `native_scanout`
//! with its 12 display words (width, height and scale multiplied by the surface's scale), or its
//! movie frame written straight in, into the display texture of the matched size; `read_begin` /
//! `read_finish` copy the displayed picture back (begin: its size, None = none yet).
//!
//! The display texture and its size outlive the picture: the native present (N7) samples
//! `Display::view` (rebuilding its bind group when `Display::generation` changes, as C drops
//! present_group with the texture) and a read returns the last picture scanned out.
//!
//! The caller (render.rs, N6) ends its open render pass before `scanout` and passes the scanout
//! pipeline and its group-1 layout (init.rs) and the display surface's view and scale.
//!
//! Referee: native_replay (N5; run with N6 at the gate).
use crate::device::Gpu;
use crate::native::arenas::Arenas;
use crate::native::convert::struct_bytes;
use crate::native::pipelines::gputimes;
use crate::native::Native;
use std::sync::{Arc, Mutex};

/// The scanout's native objects (init.rs's scanout_pipe and scanout_layout).
pub struct ScanoutObjects<'a> {
    pub pipeline: &'a wgpu::ComputePipeline,
    pub fast_pipeline: &'a wgpu::ComputePipeline,
    pub layout: &'a wgpu::BindGroupLayout,
}

/// The displayed picture's texture and its read-back buffer (rows padded to 256 bytes).
struct Target {
    texture: wgpu::Texture,
    view: wgpu::TextureView,
    readback: wgpu::Buffer,
    stride: u32,
}

/// A read between begin and finish: its size and the map's outcome (set by the callback).
struct Read {
    w: u32,
    h: u32,
    stride: u32,
    bytes: u64,
    status: Arc<Mutex<Option<Result<(), wgpu::BufferAsyncError>>>>,
}

pub struct Display {
    target: Option<Target>,
    /// display_width, display_height: 0, 0 while there is no display texture.
    width: u32,
    height: u32,
    /// Bumped each time the display texture is remade.
    generation: u64,
    reading: Option<Read>,
    /// The scanout's group 1 (the display surface's view, the display texture) and what it was made
    /// for: that view and the display texture's generation. Made again when either changes.
    group: Option<(wgpu::TextureView, u64, wgpu::BindGroup)>,
}

impl Default for Display {
    fn default() -> Display {
        Display::new()
    }
}

impl Display {
    pub fn new() -> Display {
        Display { target: None, width: 0, height: 0, generation: 0, reading: None, group: None }
    }

    /// (display_width, display_height): the displayed picture's size; 0, 0 when none.
    pub fn size(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    /// The display texture's view (present.rs samples it), or None.
    pub fn view(&self) -> Option<&wgpu::TextureView> {
        self.target.as_ref().map(|t| &t.view)
    }

    /// The display texture, or None.
    pub fn texture(&self) -> Option<&wgpu::Texture> {
        self.target.as_ref().map(|t| &t.texture)
    }

    /// Changes whenever the display texture is remade (a bind group on `view` is then stale).
    pub fn generation(&self) -> u64 {
        self.generation
    }

    /// native_display_size: the display texture (and its read-back buffer) at w x h; false when it
    /// can't be made (then there is none).
    pub fn display_size(&mut self, gpu: &Gpu, w: u32, h: u32) -> bool {
        if self.target.is_some() && self.width == w && self.height == h {
            return true;
        }
        self.target = None;
        self.width = 0;
        self.height = 0;
        self.generation += 1;
        let max = gpu.device.limits().max_texture_dimension_2d;
        if w == 0 || h == 0 || w > max || h > max {
            return false;
        }
        let texture = gpu.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("native display"),
            size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::STORAGE_BINDING | wgpu::TextureUsages::TEXTURE_BINDING
                | wgpu::TextureUsages::COPY_SRC | wgpu::TextureUsages::COPY_DST
                | wgpu::TextureUsages::RENDER_ATTACHMENT, // `black` clears it
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        let stride = (w * 4 + 255) & !255;
        let readback = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("native display readback"),
            size: stride as u64 * h as u64,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        self.target = Some(Target { texture, view, readback, stride });
        self.width = w;
        self.height = h;
        true
    }

    /// native_movie_scanout: the movie frame (w x h RGBA8, rows unpadded) as the displayed
    /// picture, by a queue write (it lands before the picture's submit). False: not shown.
    pub fn movie(&mut self, gpu: &Gpu, w: u32, h: u32, pixels: &[u32]) -> bool {
        if pixels.len() < w as usize * h as usize || !self.display_size(gpu, w, h) {
            return false;
        }
        let Some(t) = &self.target else { return false };
        gpu.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &t.texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            pixel_bytes(&pixels[..w as usize * h as usize]),
            wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(w * 4), rows_per_image: Some(h) },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        );
        true
    }

    /// The display off (no read circuit on, as after a GS reset; finding 160): the displayed picture
    /// black, at the size it had, cleared in `encoder` (no pass may be open on it). False before the
    /// first picture: nothing to show yet.
    pub fn black(&mut self, encoder: &mut wgpu::CommandEncoder) -> bool {
        let Some(t) = &self.target else { return false };
        encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("native display off"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: &t.view,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        true
    }

    /// native_scanout: the display surface (`source`, its view, at `scale`) through the 12
    /// display words into the display texture, recorded into `encoder` (no pass may be open on
    /// it). The caller has checked the picture has a display surface with a texture. False: not
    /// shown.
    #[allow(clippy::too_many_arguments)]
    pub fn scanout(&mut self, gpu: &Gpu, encoder: &mut wgpu::CommandEncoder, arenas: &mut Arenas, o: &ScanoutObjects,
                   source: &wgpu::TextureView, scale: u32, display: &[u32; 12]) -> bool {
        // An upscaled display surface gives a picture as many times larger (native.wgsl).
        let mut d = *display;
        d[5] = d[5].wrapping_mul(scale);
        d[6] = d[6].wrapping_mul(scale);
        d[8] = scale;
        if !self.display_size(gpu, d[5], d[6]) {
            return false;
        }
        // Masked GS origins contribute at most 2047*8; an invocation contributes
        // at most 32767. Thus the fast shader's dividend is below 65536.
        // ceil(65536 / divisor) overestimates its quotient by at most one;
        // one integer correction gives the exact remainder without GPU division.
        let fast = (1..=8).contains(&scale) && d[5] <= 32768 && d[6] <= 32768;
        if fast {
            d[9] = 2048 * scale;
            d[10] = 65536u32.div_ceil(d[9]);
        }
        let Some(t) = &self.target else { return false };
        let offset = arenas.constants(struct_bytes(&d));
        if self.group.as_ref().is_none_or(|(view, generation, _)| view != source || *generation != self.generation) {
            let group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("native scanout"),
                layout: o.layout,
                entries: &[
                    wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(source) },
                    wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(&t.view) },
                ],
            });
            self.group = Some((source.clone(), self.generation, group));
        }
        let Some((_, _, group)) = &self.group else { return false };
        let Some(uniforms) = arenas.uniform_group() else { return false };
        let times = gputimes::slot(gpu, gputimes::SCANOUT);
        let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: None, timestamp_writes: times.as_ref().map(gputimes::Slot::compute) });
        pass.set_pipeline(if fast { o.fast_pipeline } else { o.pipeline });
        pass.set_bind_group(0, uniforms, &[offset]);
        pass.set_bind_group(1, group, &[]);
        let rows = if fast && d[7] == 1 { d[6].div_ceil(2) } else { d[6] };
        pass.dispatch_workgroups(d[5].div_ceil(8), rows.div_ceil(8), 1);
        true
    }
}

impl Native {
    /// `dhdr_native_read_begin`: the displayed picture's size, or None when there is none. Copies
    /// the display texture to its read-back buffer in a command buffer of its own, submits it and
    /// maps the buffer; natively it waits for the map (device.poll Wait), in the browser C waits for
    /// the pending read's promise (`web::wait`).
    pub fn read_begin(&mut self) -> Option<(u32, u32)> {
        let gpu = self.gpu;
        let d = &mut self.display;
        d.reading = None;
        let t = d.target.as_ref()?;
        let (w, h, stride) = (d.width, d.height, t.stride);
        let mut e = gpu.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("native read display") });
        e.copy_texture_to_buffer(
            wgpu::TexelCopyTextureInfo {
                texture: &t.texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::TexelCopyBufferInfo {
                buffer: &t.readback,
                layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(stride), rows_per_image: Some(h) },
            },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        );
        gpu.queue.submit([e.finish()]);
        let bytes = stride as u64 * h as u64;
        let status = Arc::new(Mutex::new(None));
        let s = status.clone();
        let on_mapped = move |r| {
            *s.lock().unwrap_or_else(|p| p.into_inner()) = Some(r);
        };
        // In the browser the map's promise goes to the pending slot: C waits in web_gpu_await
        // between begin and finish (gpu_rust_web_native.inc's native_read_display).
        #[cfg(target_arch = "wasm32")]
        crate::web::wait::map_read(&t.readback, 0..bytes, on_mapped);
        #[cfg(not(target_arch = "wasm32"))]
        t.readback.map_async(wgpu::MapMode::Read, 0..bytes, on_mapped);
        #[cfg(not(target_arch = "wasm32"))]
        if let Err(err) = gpu.device.poll(wgpu::PollType::wait_indefinitely()) {
            crate::host::log(&format!("dhd_render: native read display: poll: {err}"));
        }
        d.reading = Some(Read { w, h, stride, bytes, status });
        Some((w, h))
    }

    /// The size the read in flight returns (0, 0 when none).
    pub fn read_size(&self) -> (u32, u32) {
        self.display.reading.as_ref().map_or((0, 0), |r| (r.w, r.h))
    }

    /// `dhdr_native_read_finish`: the pixels, rows unpadded; false when the map failed (logged).
    pub fn read_finish(&mut self, pixels: &mut [u32]) -> bool {
        let d = &mut self.display;
        let Some(r) = d.reading.take() else {
            crate::host::log("dhd_render: native read display: no read in flight");
            return false;
        };
        let Some(t) = d.target.as_ref() else {
            crate::host::log("dhd_render: native read display: the display texture is gone");
            return false;
        };
        let status = r.status.lock().unwrap_or_else(|p| p.into_inner()).take();
        match status {
            Some(Ok(())) => {}
            Some(Err(err)) => {
                crate::host::log(&format!("dhd_render: native read display: map failed: {err}"));
                return false;
            }
            None => {
                crate::host::log("dhd_render: native read display: the map did not finish");
                t.readback.unmap();
                return false;
            }
        }
        let row = r.w as usize * 4;
        {
            let mapped = t.readback.get_mapped_range(0..r.bytes);
            let out = pixel_bytes_mut(pixels);
            for y in 0..r.h as usize {
                let at = y * r.stride as usize;
                out[y * row..(y + 1) * row].copy_from_slice(&mapped[at..at + row]);
            }
        } // the mapped view drops before unmap
        t.readback.unmap();
        true
    }
}

fn pixel_bytes(p: &[u32]) -> &[u8] {
    // SAFETY: u32 has no padding and u8 alignment is 1; the same memory, 4 bytes per pixel.
    unsafe { std::slice::from_raw_parts(p.as_ptr() as *const u8, std::mem::size_of_val(p)) }
}

fn pixel_bytes_mut(p: &mut [u32]) -> &mut [u8] {
    // SAFETY: as pixel_bytes; every byte pattern is a valid u32.
    unsafe { std::slice::from_raw_parts_mut(p.as_mut_ptr() as *mut u8, std::mem::size_of_val(p)) }
}
