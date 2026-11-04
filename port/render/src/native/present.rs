//! The native present pass and the present filter (task N7).
//!
//! As the retired `gpu_wgpu_native.inc`'s native_present and native_present_options (last at
//! render/rust 3882d5576; with `gpu_wgpu.c`'s
//! surface_configure and the frame acquire, now `Surface::acquire`): the last scanned-out picture
//! (`scanout.rs`'s display texture) is drawn into the window surface's frame. The pass clears the
//! whole frame to opaque black, then draws native.wgsl's `present_pixel` (one full-screen
//! triangle) with `rect` as its viewport constants, scissored to `cut` clamped to the frame.
//! `DHDR_NATIVE_POINT_FILTER` samples the picture with a nearest sampler, else linear.
//!
//! `fill` (widescreen's 4:3 pictures: loading cards and movies, finding 140): first a small pass
//! boils the picture's part inside `cut` down to `FILL_SIZE` texels (`present_shrink`), then the
//! present pass draws that over the whole frame, the cut's rectangle scaled to cover the frame,
//! blurred and darkened (`present_fill`), before the sharp picture. No bars, nothing of the
//! picture cut or stretched; about 50,000 samples of a small texture and a frame of fill pixels.
//!
//! What stays in C (`gpu_rust_native.inc` over `native_core.inc`): the client rectangle
//! (`window_w`, `window_h`), the rectangle that keeps the picture's shape and DHD_VIEW's quarter
//! (`native_present_rect`: `rect`, `cut`; a movie's 4:3 aspect too), the `resize` flag, pacing
//! (`pacing_present`, not under DHD_INTERP), `dhdr_surface_present` (which runs the presented test
//! hook first) and `native_note_presented`. DHD_INTERP paced by vsync (no DHD_INTERP_HZ) wants
//! Fifo, as C's wgpu_surface forced it: C asks for it with `DHDR_PRESENT_VSYNC` when it creates
//! the surface (`present/surface.rs` picks the mode).
//!
//! Return values as `dhdr_present_exact`: 1 drawn (C paces, then presents); 0 skipped (no picture
//! scanned out yet, minimised, Timeout / Occluded); -1 failed (logged).
//!
//! The pipeline (`present_pipe`, on the draw layout, in the surface's format) and the layouts are `init.rs`'s; the present
//! uniform, the sampler and the picture's bind group are this file's, made by the first `options`
//! call, which `Native::create` makes.
//!
//! Referee: `test_native_transfers.py --backend both --impl rust --presented` and
//! `test_gs_wgpu.py --impl rust --arch x86_64 --only present` (N7).
use crate::abi::{DhdrPresentRect, DHDR_NATIVE_POINT_FILTER};
use crate::device::Gpu;
use crate::native::pipelines::gputimes;
use crate::native::scanout::Display;
use crate::native::Native;
use crate::present::surface::Surface;

/// Stand-in for `scanout.rs`'s accessor (N5: `Display::view`, the display texture's view, None
/// before the first scanout), while render/rust still has the skeleton's `Display`. Inherent
/// methods take precedence over trait methods, so once N5's scanout.rs (with a
/// `pub fn view(&self) -> Option<&wgpu::TextureView>`) is merged the call below reaches it and this
/// default goes unused: delete the trait then. Until then there is no picture and present skips (0).
#[allow(dead_code)]
trait SkeletonDisplay {
    fn view(&self) -> Option<&wgpu::TextureView> {
        None
    }
}

impl SkeletonDisplay for Display {}

/// The present uniform (C: present_uniform, present_uniform_group).
struct Uniform {
    /// native.wgsl's `Present`: rect, cover, source (48 bytes used of 256; uniform, copy dst).
    buffer: wgpu::Buffer,
    /// On `init.rs`'s uniform layout, 48 bytes at dynamic offset 0.
    group: wgpu::BindGroup,
    /// The bytes last written to `buffer` (None: nothing yet); the same rectangles are not written
    /// again.
    written: Option<[u8; UNIFORM_BYTES]>,
}

const UNIFORM_BYTES: usize = 48;

/// The fill's small texture, texels (native.wgsl FILL_SIZE).
const FILL_SIZE: (u32, u32) = (64, 48);

/// The fill's objects, made at the first filled picture.
struct Fill {
    /// `FILL_SIZE` RGBA8, render target and texture.
    small: wgpu::TextureView,
    /// Linear, clamp to edge: the fill is smooth whatever the present filter.
    sampler: wgpu::Sampler,
    /// The small texture with `sampler`, for `present_fill`.
    small_group: wgpu::BindGroup,
    /// The display view it was made for with `sampler`, for `present_shrink`; remade with the
    /// display texture.
    source_group: Option<(wgpu::TextureView, wgpu::BindGroup)>,
}

pub struct Presenter {
    /// Made by the first `options` call (which `Native::create` makes).
    uniform: Option<Uniform>,
    /// The present filter's sampler (C's present_sampler).
    sampler: Option<wgpu::Sampler>,
    /// The bind group of the display view it was made for (C's present_group): remade when the
    /// display texture is remade (a new size) or the sampler changes.
    group: Option<(wgpu::TextureView, wgpu::BindGroup)>,
    fill: Option<Fill>,
}

impl Presenter {
    pub fn new() -> Presenter {
        Presenter { uniform: None, sampler: None, group: None, fill: None }
    }
}

impl Default for Presenter {
    fn default() -> Presenter {
        Presenter::new()
    }
}

fn uniform(gpu: &Gpu, layout: &wgpu::BindGroupLayout) -> Uniform {
    let buffer = gpu.device.create_buffer(&wgpu::BufferDescriptor {
        label: Some("native present"),
        size: 256,
        usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("native present uniform"),
        layout,
        entries: &[wgpu::BindGroupEntry {
            binding: 0,
            resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                buffer: &buffer,
                offset: 0,
                size: wgpu::BufferSize::new(UNIFORM_BYTES as u64),
            }),
        }],
    });
    Uniform { buffer, group, written: None }
}

/// native_present_options' sampler: clamp to edge, `filter` for magnification and minification,
/// nearest mip, LOD 0..32, no anisotropy.
fn sampler(gpu: &Gpu, filter: wgpu::FilterMode) -> wgpu::Sampler {
    gpu.device.create_sampler(&wgpu::SamplerDescriptor {
        label: Some("native present"),
        address_mode_u: wgpu::AddressMode::ClampToEdge,
        address_mode_v: wgpu::AddressMode::ClampToEdge,
        address_mode_w: wgpu::AddressMode::ClampToEdge,
        mag_filter: filter,
        min_filter: filter,
        mipmap_filter: wgpu::MipmapFilterMode::Nearest,
        lod_min_clamp: 0.0,
        lod_max_clamp: 32.0,
        compare: None,
        anisotropy_clamp: 1,
        border_color: None,
    })
}

/// `cut` ([left, top, right, bottom) in the window) clamped to a w x h frame, as a scissor
/// rectangle (x, y, width, height); None when nothing of it is left.
fn scissor(cut: [i32; 4], w: u32, h: u32) -> Option<(u32, u32, u32, u32)> {
    let (w, h) = (w.min(i32::MAX as u32) as i32, h.min(i32::MAX as u32) as i32);
    let (left, top) = (cut[0].max(0), cut[1].max(0));
    let (right, bottom) = (cut[2].min(w), cut[3].min(h));
    (right > left && bottom > top).then(|| (left as u32, top as u32, (right - left) as u32, (bottom - top) as u32))
}

/// native.wgsl's `Present` (rect, cover, source; each x, y, width, height), little-endian f32.
fn uniform_bytes(rects: [[f32; 4]; 3]) -> [u8; UNIFORM_BYTES] {
    let mut bytes = [0u8; UNIFORM_BYTES];
    for (i, v) in rects.iter().flatten().enumerate() {
        bytes[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
    }
    bytes
}

/// The fill's two rectangles for a picture drawn at `rect` (window pixels) and cut to `cut`
/// ([left, top, right, bottom)) in a w x h frame: `cover`, the cut's rectangle scaled (keeping its
/// shape) and centred so that it just covers the frame; `source`, the cut's part of the picture in
/// uv. Zeros when the cut is empty.
fn fill_rects(rect: [f32; 4], cut: [i32; 4], w: u32, h: u32) -> ([f32; 4], [f32; 4]) {
    let (cw, ch) = ((cut[2] - cut[0]) as f32, (cut[3] - cut[1]) as f32);
    if cw <= 0.0 || ch <= 0.0 || rect[2] <= 0.0 || rect[3] <= 0.0 {
        return ([0.0; 4], [0.0; 4]);
    }
    let k = (w as f32 / cw).max(h as f32 / ch);
    let cover = [(w as f32 - cw * k) / 2.0, (h as f32 - ch * k) / 2.0, cw * k, ch * k];
    let source = [(cut[0] as f32 - rect[0]) / rect[2], (cut[1] as f32 - rect[1]) / rect[3], cw / rect[2], ch / rect[3]];
    (cover, source)
}

fn fill_objects(gpu: &Gpu, layout: &wgpu::BindGroupLayout) -> Fill {
    let texture = gpu.device.create_texture(&wgpu::TextureDescriptor {
        label: Some("native present fill"),
        size: wgpu::Extent3d { width: FILL_SIZE.0, height: FILL_SIZE.1, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8Unorm,
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    });
    let small = texture.create_view(&wgpu::TextureViewDescriptor::default());
    let sampler = sampler(gpu, wgpu::FilterMode::Linear);
    let small_group = texture_group(gpu, layout, &small, &sampler, "native present fill");
    Fill { small, sampler, small_group, source_group: None }
}

/// A view and a sampler on `init.rs`'s texture layout.
fn texture_group(gpu: &Gpu, layout: &wgpu::BindGroupLayout, view: &wgpu::TextureView, sampler: &wgpu::Sampler,
                 label: &str) -> wgpu::BindGroup {
    gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some(label),
        layout,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(view) },
            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(sampler) },
        ],
    })
}

impl Native {
    /// `dhdr_native_present`: the last scanned-out picture into `surface`'s frame, as
    /// native_present. 1 drawn (C paces, then calls `dhdr_surface_present`), 0 skipped, -1 failed
    /// (logged).
    pub fn present(&mut self, surface: &mut Surface, r: &DhdrPresentRect) -> i32 {
        let whole = crate::native::render::rstat::T::new();
        let gpu = self.gpu;
        let log = |e: &str| crate::host::log(&format!("dhd_render: dhdr_native_present: {e}"));
        // No picture scanned out yet: nothing to show.
        let Some(display) = self.display.view().cloned() else { return 0 };
        let objects = &self.objects;
        let p = &mut self.presenter;
        let Some(uniform) = p.uniform.as_mut() else {
            log("no present uniform (options was not called)");
            return -1;
        };
        let Some(sampler) = p.sampler.as_ref() else {
            log("no present sampler");
            return -1;
        };
        // Minimised (0 x 0) skips; a new size or `resize` reconfigures; Outdated / Lost reconfigure
        // once; Timeout skips.
        #[cfg(not(target_arch = "wasm32"))]
        let acquiring = crate::native::pipelines::SpikeTimer::new("acquire", 0);
        let t = crate::native::render::rstat::T::new();
        let acquired = surface.acquire(r.window_w, r.window_h, r.resize != 0);
        t.add(crate::native::render::rstat::ACQUIRE);
        #[cfg(not(target_arch = "wasm32"))]
        drop(acquiring);
        let target = match acquired {
            Ok(Some(view)) => view,
            Ok(None) => return 0,
            Err(e) => {
                log(&e);
                return -1;
            }
        };
        // The frame's size (the configured one; the surface clamps it to the device's limit).
        let (w, h) = surface.frame_texture().map_or((r.window_w, r.window_h), |t| (t.width(), t.height()));
        let cut = scissor(r.cut, w, h);
        // The fill only where the picture leaves some of the frame uncovered.
        let fill = r.fill != 0 && cut.is_some_and(|c| c != (0, 0, w, h));
        if fill {
            let fill = p.fill.get_or_insert_with(|| fill_objects(gpu, &objects.texture_layout));
            if fill.source_group.as_ref().is_none_or(|(view, _)| *view != display) {
                let group = texture_group(gpu, &objects.texture_layout, &display, &fill.sampler, "native present fill source");
                fill.source_group = Some((display.clone(), group));
            }
        }
        if p.group.as_ref().is_none_or(|(view, _)| *view != display) {
            let group = texture_group(gpu, &objects.texture_layout, &display, sampler, "native present");
            p.group = Some((display, group));
        }
        let group = &p.group.as_ref().expect("present bind group").1;
        let (cover, source) = if fill { fill_rects(r.rect, r.cut, w, h) } else { ([0.0; 4], [0.0; 4]) };
        let bytes = uniform_bytes([r.rect, cover, source]);
        if uniform.written != Some(bytes) {
            gpu.queue.write_buffer(&uniform.buffer, 0, &bytes);
            uniform.written = Some(bytes);
        }
        let mut separate = (!self.pass.deferred).then(|| gpu.device.create_command_encoder(
            &wgpu::CommandEncoderDescriptor { label: Some("native present") }));
        let encoder = match separate.as_mut() {
            Some(encoder) => encoder,
            None => self.pass.encoder(gpu),
        };
        let t = crate::native::render::rstat::T::new();
        let fill_pipes = fill.then(|| objects.fill_pipes(gpu, surface.format()));
        if let (Some((shrink, _)), Some(f)) = (fill_pipes, p.fill.as_ref()) {
            crate::native::render::pass_trace::event("present:fill shrink", -1, -1);
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("native present fill"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &f.small,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            pass.set_pipeline(shrink);
            pass.set_bind_group(0, &uniform.group, &[0]);
            pass.set_bind_group(1, &f.source_group.as_ref().expect("fill source group").1, &[]);
            pass.draw(0..3, 0..1);
        }
        let times = gputimes::slot(gpu, gputimes::PRESENT);
        {
            crate::native::render::pass_trace::event("present:clear/store", -1, -1);
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("native present"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &target,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color { r: 0.0, g: 0.0, b: 0.0, a: 1.0 }),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: times.as_ref().map(gputimes::Slot::render),
                occlusion_query_set: None,
                multiview_mask: None,
            });
            if let (Some((_, fill_pipe)), Some(f)) = (fill_pipes, p.fill.as_ref()) {
                pass.set_pipeline(fill_pipe);
                pass.set_bind_group(0, &uniform.group, &[0]);
                pass.set_bind_group(1, &f.small_group, &[]);
                pass.draw(0..3, 0..1);
            }
            if let Some((x, y, sw, sh)) = cut {
                pass.set_pipeline(objects.present_pipe(gpu, surface.format()));
                pass.set_bind_group(0, &uniform.group, &[0]);
                pass.set_bind_group(1, group, &[]);
                pass.set_scissor_rect(x, y, sw, sh);
                pass.draw(0..3, 0..1);
            }
            // The touch controls over the picture, in this pass (dhdr_surface_present's overlays
            // then skip them; C hands their shapes over before this call).
            let format = surface.format();
            surface.touch.draw_in_pass(gpu, &mut pass, format, w, h);
        }
        #[cfg(not(target_arch = "wasm32"))]
        let _spike = crate::native::pipelines::SpikeTimer::new("present_submit", 0);
        t.add(crate::native::render::rstat::PRESENT_RECORD);
        let t = crate::native::render::rstat::T::new();
        if let Some(encoder) = separate {
            let mut encoder = encoder;
            gputimes::end_frame(gpu, &mut encoder);
            gpu.queue.submit([encoder.finish()]);
        } else {
            gputimes::end_frame(gpu, self.pass.encoder(gpu));
            self.pass.submit_now(gpu, &self.arenas);
            self.zread_map();
        }
        t.add(crate::native::render::rstat::PRESENT_SUBMIT);
        whole.add(crate::native::render::rstat::PRESENT);
        gputimes::submitted(gpu);
        1
    }

    /// `dhdr_native_options`: the present filter, `DHDR_NATIVE_POINT_FILTER` nearest, else linear
    /// (native_present_options; C reads DHD_PRESENT_FILTER). The first call makes the present
    /// uniform too (native_objects).
    pub fn options(&mut self, flags: u32) {
        let gpu = self.gpu;
        let p = &mut self.presenter;
        if p.uniform.is_none() {
            p.uniform = Some(uniform(gpu, &self.objects.uniform_layout));
        }
        let filter = if flags & DHDR_NATIVE_POINT_FILTER != 0 { wgpu::FilterMode::Nearest } else { wgpu::FilterMode::Linear };
        p.sampler = Some(sampler(gpu, filter));
        p.group = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scissor_clamps_to_the_frame() {
        assert_eq!(scissor([-5, -3, 700, 500], 640, 480), Some((0, 0, 640, 480)));
        assert_eq!(scissor([80, 0, 560, 480], 640, 480), Some((80, 0, 480, 480)));
        assert_eq!(scissor([640, 0, 700, 480], 640, 480), None);
        assert_eq!(scissor([10, 10, 10, 20], 640, 480), None);
    }

    #[test]
    fn uniform_is_little_endian_f32() {
        let b = uniform_bytes([[1.0, 2.0, 640.0, 480.0], [0.0, -60.0, 1280.0, 960.0], [0.0, 0.25, 1.0, 0.5]]);
        assert_eq!(&b[0..4], &1.0f32.to_le_bytes());
        assert_eq!(&b[12..16], &480.0f32.to_le_bytes());
        assert_eq!(&b[20..24], &(-60.0f32).to_le_bytes());
        assert_eq!(&b[44..48], &0.5f32.to_le_bytes());
    }

    #[test]
    fn fill_covers_the_frame() {
        // a 4:3 picture in the middle of a 16:9 frame: the fill is it 4/3 as large, its top and
        // bottom eighths outside the frame; the source is the whole picture
        let (cover, source) = fill_rects([160.0, 0.0, 960.0, 720.0], [160, 0, 1120, 720], 1280, 720);
        assert_eq!(cover, [0.0, -120.0, 1280.0, 960.0]);
        assert_eq!(source, [0.0, 0.0, 1.0, 1.0]);
        // a movie's rows cut from a taller picture
        let (cover, source) = fill_rects([0.0, -100.0, 1000.0, 700.0], [0, 0, 1000, 500], 2400, 1080);
        assert_eq!(cover, [0.0, -60.0, 2400.0, 1200.0]);
        assert_eq!(source, [0.0, 100.0 / 700.0, 1.0, 500.0 / 700.0]);
        assert_eq!(fill_rects([0.0; 4], [5, 5, 5, 9], 640, 480), ([0.0; 4], [0.0; 4]));
    }
}
