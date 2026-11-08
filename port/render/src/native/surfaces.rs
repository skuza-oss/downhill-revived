//! The native renderer's surfaces and the operations on them (task N4).
//!
//! As the retired `gpu_wgpu_native.inc`'s (last at render/rust 3882d5576) native_surface_resources,
//! native_grow_depth, native_surface_group,
//! native_scratch, native_copy_box, native_rect_pass, native_blit, native_copy,
//! native_alpha_upload, the IMPORT case and the sample-the-target copy of native_render (the span
//! `:396-606` of the base file), and `native_core.inc`'s native_prepare_surfaces
//! (`:1470-1526`), whose sizes C works out and hands over one surface at a time
//! (`dhdr_native_surface`):
//! - up to 48 surfaces: RGBA8 colour (RenderAttachment | TextureBinding | CopySrc | CopyDst) or
//!   depth32float (RenderAttachment | TextureBinding | CopySrc). A surface is made at its scale, or
//!   grown to a larger texture keeping what it holds; new texels are zero (WebGPU's rule).
//! - Growing keeps the old texture until the next picture's start (`prepare`, called by
//!   render.rs right after `arenas.reserve`): there the old colour texture is copied into the
//!   new one, and an old Z texture is drawn into it through frag_depth (`native_grow_depth`;
//!   depth textures copy only whole). The C path does the same inside the picture's encoder, so
//!   the order is the same: grown first, then the picture's commands.
//! - the scratch texture (a copy of a region a draw or an alpha upload reads while writing it),
//!   copy_box, rect passes, blits between scales, COPY, IMPORT and ALPHA_UPLOAD (`transfer`), and
//!   the copy a draw sampling its own target draws from (`sample_target`).
//!
//! What the operations take from the other parts (init.rs's layouts and pipelines, shared.rs's
//! nearest sampler and dummy texture, the arenas and the picture's encoder) comes in a [`Kit`],
//! which render.rs makes with `Kit::new` for the picture it records. Every operation that records into the encoder expects no render pass
//! open on it: render.rs ends its pass first (C ends it only when something is recorded; a pass
//! ended earlier and begun again with Load draws the same pixels).
//!
//! Differences from the C path, none of which changes a pixel:
//! - the scratch texture also has CopySrc: a COPY of a surface onto itself copies through it
//!   (the C path's scratch lacks CopySrc, so such a copy would stop it at a validation error; no
//!   recorded picture has one);
//! - Z surfaces always have CopySrc (the C path adds it only while recording a trace), so
//!   `read` works on them.
//!
//! For replays and checks (`native_replay`, NTRACE.md's replay rule): `Native::surface_fill`
//! (a surface's contents from a blob; Z through a small frag_depth pass, as depth32float textures
//! take no copies in), `Native::surface_read` (blocking natively), `Native::surface_drop`.
//!
//! Referee: native_replay on the no-draw pictures (N4).
use crate::abi::{DhdrCommand, DhdrNativeConstants, DhdrPictureResult, DHDR_CMD_ALPHA_UPLOAD, DHDR_CMD_COPY,
                 DHDR_CMD_IMPORT, DHDR_NATIVE_SURFACES};
use crate::device::Gpu;
use crate::native::arenas::Arenas;
use crate::native::init::Objects;
use crate::native::pipelines::gputimes;
use crate::native::shared::NativeShared;
use crate::native::Native;

/// Surfaces (NATIVE_SURFACES).
pub const SURFACES: usize = DHDR_NATIVE_SURFACES as usize;

/// The import texture's side (a page of IMPORT or ALPHA_UPLOAD pixels at 1x).
const IMPORT_SIDE: u32 = 64;

/// One surface's texture. Set only by this module.
#[derive(Default)]
pub struct Surface {
    pub texture: Option<wgpu::Texture>,
    /// The whole texture's view: the attachment, and what draws, conversions and scanout sample.
    pub view: Option<wgpu::TextureView>,
    /// A colour surface's draw bind group (group 1 with the nearest sampler), made when first
    /// sampled (`group`).
    group: Option<wgpu::BindGroup>,
    /// Its resolution scale (C decides it once).
    pub scale: u32,
    /// The texture's size in scaled pixels (0 x 0 without one).
    pub size: [u32; 2],
    /// A Z surface (depth32float).
    pub depth: bool,
}

/// A grown surface's old texture, drawn or copied into the new one at the next picture's start.
struct Grow {
    index: usize,
    texture: wgpu::Texture,
    view: wgpu::TextureView,
    size: [u32; 2],
    depth: bool,
}

/// The scratch texture and its draw bind group.
struct Scratch {
    texture: wgpu::Texture,
    view: wgpu::TextureView,
    group: wgpu::BindGroup,
}

/// `surface_fill`'s Z pass: an R32Uint texture of the bit patterns drawn into the depth texture.
struct DepthFill {
    layout: wgpu::BindGroupLayout,
    pipe: wgpu::RenderPipeline,
}

/// The Z fill (replays only): each pixel's depth is the float whose bits the source holds.
pub(crate) const DEPTH_FILL_WGSL: &str = r#"
@group(0) @binding(0) var fill_source: texture_2d<u32>;
@vertex
fn fill_vertex(@builtin(vertex_index) id: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(select(-1.0, 3.0, id == 1u), select(1.0, -3.0, id == 2u));
    return vec4<f32>(p, 0.0, 1.0);
}
@fragment
fn fill_depth(@builtin(position) position: vec4<f32>) -> @builtin(frag_depth) f32 {
    return bitcast<f32>(textureLoad(fill_source, vec2<i32>(position.xy), 0).x);
}
"#;

pub struct Surfaces {
    list: Vec<Surface>,
    /// Grown surfaces' old textures, until `prepare`.
    grows: Vec<Grow>,
    scratch: Option<Scratch>,
    /// The scratch texture's size (kept when remade larger, as C's scratch_size).
    scratch_size: [u32; 2],
    /// 64 x 64 RGBA8 (TextureBinding | CopyDst): IMPORT pages for upscaled surfaces, ALPHA_UPLOAD
    /// pages. Made on first use.
    import: Option<(wgpu::Texture, wgpu::TextureView)>,
    fill: Option<DepthFill>,
}

/// What the surface operations use from the rest of the renderer, lent by render.rs for the
/// picture being recorded.
pub struct Kit<'a> {
    pub gpu: &'a Gpu,
    /// The picture's encoder, with no render pass open.
    pub encoder: &'a mut wgpu::CommandEncoder,
    /// Reserved for the picture (`Arenas::reserve` with the device): rect passes take their 16
    /// bytes of constants and the uniform bind group from it, IMPORT and ALPHA_UPLOAD their pixels.
    pub arenas: &'a mut Arenas,
    /// A draw's group 1: a filterable 2D texture and a filtering sampler (init.rs's
    /// texture_layout, or shared.rs's equal one).
    pub texture_layout: &'a wgpu::BindGroupLayout,
    /// The nearest sampler surfaces and the scratch texture are drawn with.
    pub nearest: &'a wgpu::Sampler,
    /// The blit's group 1: two unfilterable 2D textures (source, previous).
    pub blit_layout: &'a wgpu::BindGroupLayout,
    /// Depth growth's group 1: a depth texture.
    pub grow_layout: &'a wgpu::BindGroupLayout,
    /// `native_blit` (uniform, blit layouts)
    pub blit_pipe: &'a wgpu::RenderPipeline,
    /// `native_upload_alpha` (uniform, blit layouts)
    pub upload_pipe: &'a wgpu::RenderPipeline,
    /// `native_grow_depth` (uniform, grow layouts; depth only, Always, writes)
    pub grow_pipe: &'a wgpu::RenderPipeline,
    /// The 1 x 1 RGBA8 dummy texture's view (the blit's unused second texture).
    pub dummy_view: &'a wgpu::TextureView,
}

impl<'a> Kit<'a> {
    /// The kit from the native objects (init.rs), the shared ones (shared.rs), the arenas and the
    /// picture's encoder.
    pub fn new(gpu: &'a Gpu, objects: &'a Objects, shared: &'a NativeShared, arenas: &'a mut Arenas,
               encoder: &'a mut wgpu::CommandEncoder) -> Kit<'a> {
        Kit {
            gpu,
            encoder,
            arenas,
            texture_layout: &objects.texture_layout,
            nearest: &shared.nearest,
            blit_layout: &objects.blit_layout,
            grow_layout: &objects.grow_layout,
            blit_pipe: &objects.blit_pipe,
            upload_pipe: &objects.upload_pipe,
            grow_pipe: &objects.grow_pipe,
            dummy_view: &shared.dummy_view,
        }
    }
}

impl Default for Surfaces {
    fn default() -> Surfaces {
        Surfaces::new()
    }
}

/// A command's surface index, or None when it names none (negative or 48 and above).
fn surface_index(i: i16) -> Option<usize> {
    usize::try_from(i).ok().filter(|&i| i < SURFACES)
}

fn extent(w: u32, h: u32) -> wgpu::Extent3d {
    wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 }
}

/// native_at
fn at(texture: &wgpu::Texture, x: u32, y: u32) -> wgpu::TexelCopyTextureInfo<'_> {
    wgpu::TexelCopyTextureInfo {
        texture,
        mip_level: 0,
        origin: wgpu::Origin3d { x, y, z: 0 },
        aspect: wgpu::TextureAspect::All,
    }
}

fn view_entry(binding: u32, view: &wgpu::TextureView) -> wgpu::BindGroupEntry<'_> {
    wgpu::BindGroupEntry { binding, resource: wgpu::BindingResource::TextureView(view) }
}

fn group(gpu: &Gpu, layout: &wgpu::BindGroupLayout, entries: &[wgpu::BindGroupEntry]) -> wgpu::BindGroup {
    gpu.device.create_bind_group(&wgpu::BindGroupDescriptor { label: None, layout, entries })
}

/// native_texture_group: a sampled texture's draw bind group (the nearest sampler).
fn texture_group(gpu: &Gpu, layout: &wgpu::BindGroupLayout, nearest: &wgpu::Sampler, view: &wgpu::TextureView)
                 -> wgpu::BindGroup {
    group(gpu, layout, &[view_entry(0, view),
                         wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(nearest) }])
}

/// Runs `f` inside OutOfMemory and Validation error scopes: Err (naming `what`) when it raised an
/// error, so a texture that could not be made leaves the old one in place (C checks for NULL).
/// Natively the scopes are popped at once; in the browser errors go to the uncaptured-error
/// handler (fatal), as a scope could only be awaited.
fn scoped<R>(gpu: &Gpu, what: &str, f: impl FnOnce() -> R) -> Result<R, String> {
    #[cfg(feature = "native")]
    {
        let memory = gpu.device.push_error_scope(wgpu::ErrorFilter::OutOfMemory);
        let validation = gpu.device.push_error_scope(wgpu::ErrorFilter::Validation);
        let r = f();
        let v = pollster::block_on(validation.pop());
        let m = pollster::block_on(memory.pop());
        match v.or(m) {
            Some(e) => Err(format!("{what}: {e}")),
            None => Ok(r),
        }
    }
    #[cfg(not(feature = "native"))]
    {
        let _ = (gpu, what);
        Ok(f())
    }
}

/// native_new_texture, and its whole view.
fn new_texture(gpu: &Gpu, w: u32, h: u32, format: wgpu::TextureFormat, usage: wgpu::TextureUsages, label: &str)
               -> Result<(wgpu::Texture, wgpu::TextureView), String> {
    let max = gpu.device.limits().max_texture_dimension_2d;
    if w == 0 || h == 0 || w > max || h > max {
        return Err(format!("gs native: {label} of {w} x {h} (the device allows 1 to {max} a side)"));
    }
    scoped(gpu, label, || {
        let texture = gpu.device.create_texture(&wgpu::TextureDescriptor {
            label: Some(label),
            size: extent(w, h),
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format,
            usage,
            view_formats: &[],
        });
        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
        (texture, view)
    })
}

/// native_copy_box: a box (texture pixels) between textures, cut to both.
#[allow(clippy::too_many_arguments)]
fn copy_box(encoder: &mut wgpu::CommandEncoder, from: &wgpu::Texture, from_size: [u32; 2], sx: u32, sy: u32,
            to: &wgpu::Texture, to_size: [u32; 2], x: u32, y: u32, mut w: u32, mut h: u32) {
    if sx >= from_size[0] || sy >= from_size[1] || x >= to_size[0] || y >= to_size[1] {
        return;
    }
    w = w.min(from_size[0] - sx).min(to_size[0] - x);
    h = h.min(from_size[1] - sy).min(to_size[1] - y);
    if w == 0 || h == 0 {
        return;
    }
    encoder.copy_texture_to_texture(at(from, sx, sy), at(to, x, y), extent(w, h));
}

/// native_rect_pass: a full-screen-triangle pass into a colour texture (`view`, `size`), cut to the
/// rectangle (x, y, w, h) in its pixels, with this pipeline, the 16 bytes `map` as its constants
/// and `bind` as group 1.
#[allow(clippy::too_many_arguments)]
fn rect_pass(kit: &mut Kit, view: &wgpu::TextureView, size: [u32; 2], pipe: &wgpu::RenderPipeline, map: [f32; 4],
             bind: &wgpu::BindGroup, x: u32, y: u32, mut w: u32, mut h: u32) {
    if x >= size[0] || y >= size[1] {
        return;
    }
    w = w.min(size[0] - x);
    h = h.min(size[1] - y);
    if w == 0 || h == 0 {
        return;
    }
    let mut bytes = [0u8; 16];
    for (i, v) in map.iter().enumerate() {
        bytes[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
    }
    let offset = kit.arenas.constants(&bytes);
    let Some(uniforms) = kit.arenas.uniform_group() else {
        crate::host::log("gs native: a surface pass before the arenas were reserved (skipped)");
        return;
    };
    crate::native::render::pass_trace::event("surface-rect:load/store", -1, -1);
    let times = gputimes::slot(kit.gpu, gputimes::SURFACE);
    let mut pass = kit.encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
        label: None,
        color_attachments: &[Some(wgpu::RenderPassColorAttachment {
            view,
            depth_slice: None,
            resolve_target: None,
            ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store },
        })],
        depth_stencil_attachment: None,
        timestamp_writes: times.as_ref().map(gputimes::Slot::render),
        occlusion_query_set: None,
        multiview_mask: None,
    });
    pass.set_pipeline(pipe);
    pass.set_bind_group(0, uniforms, &[offset]);
    pass.set_bind_group(1, bind, &[]);
    pass.set_viewport(x as f32, y as f32, w as f32, h as f32, 0.0, 1.0);
    pass.set_scissor_rect(x, y, w, h);
    pass.draw(0..3, 0..1);
}

/// native_blit: a w x h rectangle (GS pixels) of a texture at scale `from`, at (sx, sy), into
/// surface t at (x, y), when the scales differ (native.wgsl native_blit).
#[allow(clippy::too_many_arguments)]
fn blit(kit: &mut Kit, source: &wgpu::TextureView, from: u32, sx: i32, sy: i32, t: &Surface, x: i32, y: i32,
        mut w: i32, mut h: i32) {
    let tt = t.scale;
    let Some(view) = t.view.as_ref().filter(|_| tt != 0) else { return };
    // C: int arithmetic on the GS pixel rectangle against the texture's size / scale.
    let (tw, th) = ((t.size[0] / tt) as i32, (t.size[1] / tt) as i32);
    if x.wrapping_add(w) > tw {
        w = tw.wrapping_sub(x);
    }
    if y.wrapping_add(h) > th {
        h = th.wrapping_sub(y);
    }
    if w <= 0 || h <= 0 || x < 0 || y < 0 {
        return;
    }
    let map = [sx.wrapping_sub(x) as f32, sy.wrapping_sub(y) as f32, tt as f32, from as f32];
    let (gpu, layout, pipe, dummy) = (kit.gpu, kit.blit_layout, kit.blit_pipe, kit.dummy_view);
    let bind = group(gpu, layout, &[view_entry(0, source), view_entry(1, dummy)]);
    rect_pass(kit, view, t.size, pipe, map, &bind, (x as u32).wrapping_mul(tt), (y as u32).wrapping_mul(tt),
              (w as u32).wrapping_mul(tt), (h as u32).wrapping_mul(tt));
}

/// native_grow_depth: a Z surface's old contents (`from`, `size`) drawn into its new, larger
/// texture `to` (cleared to 0 first).
fn grow_depth(kit: &mut Kit, from: &wgpu::TextureView, size: [u32; 2], to: &wgpu::TextureView) -> Result<(), String> {
    let (gpu, layout, pipe) = (kit.gpu, kit.grow_layout, kit.grow_pipe);
    let bind = group(gpu, layout, &[view_entry(0, from)]);
    let uniforms = kit.arenas.uniform_group().ok_or("gs native: a Z surface grown before the arenas were reserved")?;
    crate::native::render::pass_trace::event("surface-rect:load/store", -1, -1);
    let times = gputimes::slot(gpu, gputimes::SURFACE);
    let mut pass = kit.encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
        label: None,
        color_attachments: &[],
        depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
            view: to,
            depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(0.0), store: wgpu::StoreOp::Store }),
            stencil_ops: None,
        }),
        timestamp_writes: times.as_ref().map(gputimes::Slot::render),
        occlusion_query_set: None,
        multiview_mask: None,
    });
    pass.set_pipeline(pipe);
    pass.set_bind_group(0, uniforms, &[0]);
    pass.set_bind_group(1, &bind, &[]);
    pass.set_viewport(0.0, 0.0, size[0] as f32, size[1] as f32, 0.0, 1.0);
    pass.set_scissor_rect(0, 0, size[0], size[1]);
    pass.draw(0..3, 0..1);
    Ok(())
}

/// The upload of `pixels` (w x h, from `upload`'s offset and stride) as a buffer copy source.
fn upload_source(buffer: &wgpu::Buffer, offset: u64, stride: u32, h: u32) -> wgpu::TexelCopyBufferInfo<'_> {
    wgpu::TexelCopyBufferInfo {
        buffer,
        layout: wgpu::TexelCopyBufferLayout { offset, bytes_per_row: Some(stride), rows_per_image: Some(h) },
    }
}

impl Surfaces {
    pub fn new() -> Surfaces {
        Surfaces {
            list: (0..SURFACES).map(|_| Surface::default()).collect(),
            grows: Vec::new(),
            scratch: None,
            scratch_size: [0, 0],
            import: None,
            fill: None,
        }
    }

    /// Surface `index`, or None past the last.
    pub fn get(&self, index: usize) -> Option<&Surface> {
        self.list.get(index)
    }

    /// native_surface_resources: surface `index` at `scale` with a texture of at least w x h
    /// scaled pixels (a Z surface when `depth`), keeping what it holds; the texture's size. Err
    /// (nothing made) keeps the old texture. A surface that changes between colour and Z starts
    /// again, empty (C never asks for that).
    pub fn resources(&mut self, gpu: &Gpu, index: usize, scale: u32, w: u32, h: u32, depth: bool)
                     -> Result<[u32; 2], String> {
        if index >= SURFACES {
            return Err(format!("gs native: surface {index} (fewer than {SURFACES})"));
        }
        if scale == 0 {
            return Err(format!("gs native: surface {index} at scale 0"));
        }
        let s = &mut self.list[index];
        if s.texture.is_some() && s.depth != depth {
            *s = Surface::default();
            self.grows.retain(|g| g.index != index);
        }
        let s = &mut self.list[index];
        s.scale = scale;
        s.depth = depth;
        if s.texture.is_some() && s.size[0] >= w && s.size[1] >= h {
            return Ok(s.size);
        }
        let (w, h) = if s.texture.is_some() { (w.max(s.size[0]), h.max(s.size[1])) } else { (w, h) };
        let (texture, view) = if depth {
            new_texture(gpu, w, h, wgpu::TextureFormat::Depth32Float,
                        wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING
                            | wgpu::TextureUsages::COPY_SRC, "native z")?
        } else {
            new_texture(gpu, w, h, wgpu::TextureFormat::Rgba8Unorm,
                        wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING
                            | wgpu::TextureUsages::COPY_SRC | wgpu::TextureUsages::COPY_DST, "native surface")?
        };
        let old_size = s.size;
        let old = s.texture.replace(texture).zip(s.view.replace(view));
        s.group = None;
        s.size = [w, h];
        if let Some((texture, view)) = old {
            // Grown twice before a picture: the texture made the first time holds nothing yet, so
            // the first one's old contents still go into the newest.
            if !self.grows.iter().any(|g| g.index == index) {
                self.grows.push(Grow { index, texture, view, size: old_size, depth });
            }
        }
        Ok([w, h])
    }

    /// The grown surfaces' old contents into their new textures: the start of each picture (C's
    /// native_prepare_surfaces runs inside the picture's encoder), after `arenas.reserve`.
    pub fn prepare(&mut self, kit: &mut Kit) -> Result<(), String> {
        if self.grows.is_empty() {
            return Ok(());
        }
        if self.grows.iter().any(|g| g.depth) && kit.arenas.uniform_group().is_none() {
            return Err("gs native: a Z surface grown before the arenas were reserved".into());
        }
        for g in std::mem::take(&mut self.grows) {
            let s = &self.list[g.index];
            let (Some(texture), Some(view)) = (&s.texture, &s.view) else { continue };
            if g.depth {
                grow_depth(kit, &g.view, g.size, view)?;
            } else {
                kit.encoder.copy_texture_to_texture(at(&g.texture, 0, 0), at(texture, 0, 0), extent(g.size[0], g.size[1]));
            }
        }
        Ok(())
    }

    /// native_surface_group: a colour surface's draw bind group (made when first sampled); None
    /// for a Z surface or one without a texture.
    pub fn group(&mut self, gpu: &Gpu, layout: &wgpu::BindGroupLayout, nearest: &wgpu::Sampler, index: usize)
                 -> Option<&wgpu::BindGroup> {
        let s = self.list.get_mut(index)?;
        if s.group.is_none() && !s.depth {
            let view = s.view.as_ref()?;
            s.group = Some(texture_group(gpu, layout, nearest, view));
        }
        s.group.as_ref()
    }

    /// native_scratch: the scratch texture holds at least w x h pixels; false when it could not
    /// be made (then it has none).
    fn scratch(&mut self, kit: &Kit, w: u32, h: u32) -> bool {
        if self.scratch.is_some() && self.scratch_size[0] >= w && self.scratch_size[1] >= h {
            return true;
        }
        self.scratch = None;
        let (w, h) = (w.max(self.scratch_size[0]), h.max(self.scratch_size[1]));
        let made = new_texture(kit.gpu, w, h, wgpu::TextureFormat::Rgba8Unorm,
                               wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST
                                   | wgpu::TextureUsages::COPY_SRC, "native scratch");
        match made {
            Ok((texture, view)) => {
                let group = texture_group(kit.gpu, kit.texture_layout, kit.nearest, &view);
                self.scratch = Some(Scratch { texture, view, group });
                self.scratch_size = [w, h];
                true
            }
            Err(e) => {
                crate::host::log(&e);
                self.scratch_size = [0, 0];
                false
            }
        }
    }

    /// The import texture, made on first use; false when it could not be.
    fn import_texture(&mut self, gpu: &Gpu) -> bool {
        if self.import.is_none() {
            match new_texture(gpu, IMPORT_SIDE, IMPORT_SIDE, wgpu::TextureFormat::Rgba8Unorm,
                              wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST, "native import") {
                Ok(t) => self.import = Some(t),
                Err(e) => crate::host::log(&e),
            }
        }
        self.import.is_some()
    }

    /// A COPY, IMPORT or ALPHA_UPLOAD command (`pixels`: `Picture::pixels` of it); the counts go
    /// to `out`. False for any other kind (nothing done).
    pub fn transfer(&mut self, kit: &mut Kit, k: &DhdrCommand, pixels: Option<&[u32]>, out: &mut DhdrPictureResult)
                    -> bool {
        match k.kind {
            DHDR_CMD_ALPHA_UPLOAD => self.alpha_upload(kit, k, pixels),
            DHDR_CMD_IMPORT => self.import(kit, k, pixels, out),
            DHDR_CMD_COPY => self.copy(kit, k, out),
            _ => return false,
        }
        true
    }

    /// native_copy (the COPY case of native_render): a w x h rectangle (GS pixels) of surface
    /// `source` at (sx, sy) to surface `target` at (x, y); through the scratch texture onto
    /// itself, or blitted when the scales differ.
    fn copy(&mut self, kit: &mut Kit, k: &DhdrCommand, out: &mut DhdrPictureResult) {
        let (Some(si), Some(ti)) = (surface_index(k.source), surface_index(k.target)) else { return };
        let (s, t) = (&self.list[si], &self.list[ti]);
        if s.texture.is_none() || t.texture.is_none() || s.depth || t.depth {
            return;
        }
        if s.scale != t.scale {
            if let Some(view) = &s.view {
                blit(kit, view, s.scale, k.sx, k.sy, t, k.x, k.y, k.w, k.h);
            }
            return;
        }
        let ss = s.scale as i32;
        let (sx, sy, x, y) = (k.sx, k.sy, k.x, k.y);
        let (mut w, mut h) = (k.w, k.h);
        if ss == 0 {
            return;
        }
        let (sw, sh) = (s.size[0] as i32 / ss, s.size[1] as i32 / ss);
        let (tw, th) = (t.size[0] as i32 / ss, t.size[1] as i32 / ss);
        if sx.wrapping_add(w) > sw {
            w = sw.wrapping_sub(sx);
        }
        if sy.wrapping_add(h) > sh {
            h = sh.wrapping_sub(sy);
        }
        if x.wrapping_add(w) > tw {
            w = tw.wrapping_sub(x);
        }
        if y.wrapping_add(h) > th {
            h = th.wrapping_sub(y);
        }
        if w <= 0 || h <= 0 || sx < 0 || sy < 0 || x < 0 || y < 0 {
            return;
        }
        out.copies += 1;
        out.copy_px += w as u64 * h as u64;
        let (bx, by) = ((sx * ss) as u32, (sy * ss) as u32);
        let (bw, bh) = ((w * ss) as u32, (h * ss) as u32);
        let (dx, dy) = ((x * ss) as u32, (y * ss) as u32);
        if si == ti {
            if !self.scratch(kit, bx.wrapping_add(bw), by.wrapping_add(bh)) {
                return;
            }
            let (s, scratch) = (&self.list[si], self.scratch.as_ref().expect("scratch made"));
            let texture = s.texture.as_ref().expect("checked above");
            copy_box(kit.encoder, texture, s.size, bx, by, &scratch.texture, self.scratch_size, bx, by, bw, bh);
            copy_box(kit.encoder, &scratch.texture, self.scratch_size, bx, by, texture, s.size, dx, dy, bw, bh);
        } else {
            let (s, t) = (&self.list[si], &self.list[ti]);
            let (from, to) = (s.texture.as_ref().expect("checked above"), t.texture.as_ref().expect("checked above"));
            copy_box(kit.encoder, from, s.size, bx, by, to, t.size, dx, dy, bw, bh);
        }
    }

    /// The IMPORT case of native_render: w x h CPU pixels into surface `target` at (x, y); at 1x
    /// a copy cut to the texture, upscaled (pages of at most 64 x 64) through the import texture
    /// and a blit, a block per pixel.
    fn import(&mut self, kit: &mut Kit, k: &DhdrCommand, pixels: Option<&[u32]>, out: &mut DhdrPictureResult) {
        let Some(ti) = surface_index(k.target) else { return };
        {
            let s = &self.list[ti];
            if s.texture.is_none() || s.depth {
                return;
            }
        }
        if self.list[ti].scale == 1 {
            let s = &self.list[ti];
            let texture = s.texture.as_ref().expect("checked above");
            let (mut w, mut h) = (k.w as u32, k.h as u32);
            let (x, y) = (k.x as u32, k.y as u32);
            if x < s.size[0] && y < s.size[1] {
                w = w.min(s.size[0] - x);
                h = h.min(s.size[1] - y);
                if let Some(p) = pixels {
                    let (offset, stride) = kit.arenas.upload(p, k.w as u32, k.h as u32);
                    if let Some(buffer) = kit.arenas.upload_buffer() {
                        kit.encoder.copy_buffer_to_texture(upload_source(buffer, offset, stride, k.h as u32),
                                                           at(texture, x, y), extent(w, h));
                    }
                }
            }
        } else if k.w <= IMPORT_SIDE as i32 && k.h <= IMPORT_SIDE as i32 {
            if let Some(p) = pixels.filter(|_| self.import_texture(kit.gpu)) {
                let (import, import_view) = self.import.as_ref().expect("made above");
                let (w, h) = (k.w as u32, k.h as u32);
                let (offset, stride) = kit.arenas.upload(p, w, h);
                if let Some(buffer) = kit.arenas.upload_buffer() {
                    kit.encoder.copy_buffer_to_texture(upload_source(buffer, offset, stride, h), at(import, 0, 0),
                                                       extent(w, h));
                }
                blit(kit, import_view, 1, 0, 0, &self.list[ti], k.x, k.y, k.w, k.h);
            }
        }
        out.imports += 1;
        // C: (uint64_t)k->w * k->h
        out.import_px = out.import_px.wrapping_add((k.w as i64 as u64).wrapping_mul(k.h as i64 as u64));
    }

    /// native_alpha_upload: a page (64 x 32 GS pixels at (x, y)) of masked alpha bits applied to
    /// surface `target` without resampling its scaled RGB: the region goes to the scratch texture,
    /// the page to the import texture, and native_upload_alpha draws both back.
    fn alpha_upload(&mut self, kit: &mut Kit, k: &DhdrCommand, pixels: Option<&[u32]>) {
        let Some(ti) = surface_index(k.target) else { return };
        let ss = self.list[ti].scale;
        let left = (k.x as u32).wrapping_mul(ss);
        let top = (k.y as u32).wrapping_mul(ss);
        let right = (k.x.wrapping_add(64) as u32).wrapping_mul(ss);
        let bottom = (k.y.wrapping_add(32) as u32).wrapping_mul(ss);
        {
            let s = &self.list[ti];
            if s.texture.is_none() || s.depth {
                return;
            }
        }
        if !self.scratch(kit, right, bottom) {
            return;
        }
        let Some(p) = pixels else { return };
        if !self.import_texture(kit.gpu) {
            return;
        }
        let s = &self.list[ti];
        let (texture, view) = (s.texture.as_ref().expect("checked above"), s.view.as_ref().expect("made with it"));
        let scratch = self.scratch.as_ref().expect("scratch made");
        copy_box(kit.encoder, texture, s.size, left, top, &scratch.texture, self.scratch_size, left, top,
                 right.wrapping_sub(left), bottom.wrapping_sub(top));
        let (import, import_view) = self.import.as_ref().expect("made above");
        let (offset, stride) = kit.arenas.upload(p, 64, 32);
        if let Some(buffer) = kit.arenas.upload_buffer() {
            kit.encoder.copy_buffer_to_texture(upload_source(buffer, offset, stride, 32), at(import, 0, 0), extent(64, 32));
        }
        let map = [k.x as f32, k.y as f32, ss as f32, 0.0];
        let (gpu, layout, pipe) = (kit.gpu, kit.blit_layout, kit.upload_pipe);
        let bind = group(gpu, layout, &[view_entry(0, import_view), view_entry(1, &scratch.view)]);
        rect_pass(kit, view, s.size, pipe, map, &bind, left, top, 64u32.wrapping_mul(ss), 32u32.wrapping_mul(ss));
    }

    /// A draw sampling its own target (surface `index`, the command's constants `c`): the region
    /// its texture can reach (tex_offset, tex_size, cut to the surface) copied to the scratch
    /// texture, which it then draws from; that bind group, or None to skip the draw (C's
    /// `continue`). Counts a self-copy.
    pub fn sample_target(&mut self, kit: &mut Kit, index: usize, c: &DhdrNativeConstants, out: &mut DhdrPictureResult)
                         -> Option<&wgpu::BindGroup> {
        let s = self.list.get(index)?;
        let ss = s.scale;
        if ss == 0 {
            return None;
        }
        let (x, y) = (c.tex_offset[0], c.tex_offset[1]);
        let (mut w, mut h) = (c.tex_size[0] as i32, c.tex_size[1] as i32);
        let (sw, sh) = ((s.size[0] / ss) as i32, (s.size[1] / ss) as i32);
        if x.wrapping_add(w) > sw {
            w = sw.wrapping_sub(x);
        }
        if y.wrapping_add(h) > sh {
            h = sh.wrapping_sub(y);
        }
        w = w.max(0);
        h = h.max(0);
        if x < 0 || y < 0 {
            return None;
        }
        if !self.scratch(kit, (x.wrapping_add(w) as u32).wrapping_mul(ss), (y.wrapping_add(h) as u32).wrapping_mul(ss)) {
            return None;
        }
        let s = &self.list[index];
        let scratch = self.scratch.as_ref().expect("scratch made");
        if w != 0 && h != 0 {
            if let Some(texture) = &s.texture {
                let (bx, by) = ((x as u32).wrapping_mul(ss), (y as u32).wrapping_mul(ss));
                copy_box(kit.encoder, texture, s.size, bx, by, &scratch.texture, self.scratch_size, bx, by,
                         (w as u32).wrapping_mul(ss), (h as u32).wrapping_mul(ss));
            }
        }
        out.self_copies += 1;
        out.self_copy_px += w as u64 * h as u64;
        Some(&scratch.group)
    }

    /// No surface `index` any more (its texture and any pending growth dropped).
    pub fn drop_surface(&mut self, index: usize) {
        if let Some(s) = self.list.get_mut(index) {
            *s = Surface::default();
            self.grows.retain(|g| g.index != index);
        }
    }

    /// Surface `index`'s contents set to `bytes` (its size, rows unpadded: RGBA8, or for a Z
    /// surface depth32float bit patterns), replacing any pending growth. Replays (NTRACE.md's
    /// `before` blobs).
    pub fn fill(&mut self, gpu: &Gpu, index: usize, bytes: &[u8]) -> Result<(), String> {
        let s = self.list.get(index).ok_or_else(|| format!("gs native: surface {index} (fewer than {SURFACES})"))?;
        let (Some(texture), Some(view)) = (&s.texture, &s.view) else {
            return Err(format!("gs native: surface {index} has no texture to fill"));
        };
        let [w, h] = s.size;
        if bytes.len() as u64 != w as u64 * h as u64 * 4 {
            return Err(format!("gs native: surface {index} is {w} x {h}, the fill {} bytes", bytes.len()));
        }
        let layout = wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(w * 4), rows_per_image: Some(h) };
        if !s.depth {
            gpu.queue.write_texture(at(texture, 0, 0), bytes, layout, extent(w, h));
        } else {
            if self.fill.is_none() {
                self.fill = Some(scoped(gpu, "native z fill", || DepthFill::create(gpu))?);
            }
            let fill = self.fill.as_ref().expect("made above");
            let (source, source_view) = new_texture(gpu, w, h, wgpu::TextureFormat::R32Uint,
                                                    wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                                                    "native z fill")?;
            gpu.queue.write_texture(at(&source, 0, 0), bytes, layout, extent(w, h));
            let bind = group(gpu, &fill.layout, &[view_entry(0, &source_view)]);
            let mut encoder = gpu.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("native z fill") });
            {
                let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: None,
                    color_attachments: &[],
                    depth_stencil_attachment: Some(wgpu::RenderPassDepthStencilAttachment {
                        view,
                        depth_ops: Some(wgpu::Operations { load: wgpu::LoadOp::Clear(0.0), store: wgpu::StoreOp::Store }),
                        stencil_ops: None,
                    }),
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                pass.set_pipeline(&fill.pipe);
                pass.set_bind_group(0, &bind, &[]);
                pass.set_viewport(0.0, 0.0, w as f32, h as f32, 0.0, 1.0);
                pass.set_scissor_rect(0, 0, w, h);
                pass.draw(0..3, 0..1);
            }
            gpu.queue.submit([encoder.finish()]);
        }
        self.grows.retain(|g| g.index != index);
        Ok(())
    }

    /// Surface `index`'s contents as the GPU holds them after what was submitted (pending growth
    /// not applied), rows unpadded: RGBA8, or depth32float bit patterns. Blocks natively. Replays
    /// and checks (NTRACE.md's `after` blobs).
    pub fn read(&self, gpu: &Gpu, index: usize) -> Result<Vec<u8>, String> {
        let s = self.list.get(index).ok_or_else(|| format!("gs native: surface {index} (fewer than {SURFACES})"))?;
        let Some(texture) = &s.texture else {
            return Err(format!("gs native: surface {index} has no texture to read"));
        };
        let [w, h] = s.size;
        let row = w * 4;
        let stride = (row + 255) & !255;
        let bytes = stride as u64 * h as u64;
        let buffer = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("native surface read"),
            size: bytes,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let mut encoder = gpu.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("native surface read") });
        let mut from = at(texture, 0, 0);
        if s.depth {
            from.aspect = wgpu::TextureAspect::DepthOnly;
        }
        encoder.copy_texture_to_buffer(from, upload_source(&buffer, 0, stride, h), extent(w, h));
        gpu.queue.submit([encoder.finish()]);
        let status = std::sync::Arc::new(std::sync::Mutex::new(None));
        let done = status.clone();
        buffer.map_async(wgpu::MapMode::Read, .., move |r| *done.lock().unwrap_or_else(|p| p.into_inner()) = Some(r));
        gpu.device.poll(wgpu::PollType::wait_indefinitely()).map_err(|e| format!("gs native: poll: {e}"))?;
        match status.lock().unwrap_or_else(|p| p.into_inner()).take() {
            Some(Ok(())) => {}
            Some(Err(e)) => return Err(format!("gs native: surface {index} read: {e}")),
            None => return Err(format!("gs native: surface {index} read: not mapped")),
        }
        let mut pixels = Vec::with_capacity(row as usize * h as usize);
        {
            let mapped = buffer.get_mapped_range(..);
            for y in 0..h as usize {
                let o = y * stride as usize;
                pixels.extend_from_slice(&mapped[o..o + row as usize]);
            }
        }
        buffer.unmap();
        Ok(pixels)
    }
}

impl DepthFill {
    fn create(gpu: &Gpu) -> DepthFill {
        let module = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("native z fill"),
            source: wgpu::ShaderSource::Wgsl(DEPTH_FILL_WGSL.into()),
        });
        let layout = gpu.device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: None,
            entries: &[wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::FRAGMENT,
                ty: wgpu::BindingType::Texture {
                    sample_type: wgpu::TextureSampleType::Uint,
                    view_dimension: wgpu::TextureViewDimension::D2,
                    multisampled: false,
                },
                count: None,
            }],
        });
        let pipeline_layout = gpu.device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: None,
            bind_group_layouts: &[Some(&layout)],
            immediate_size: 0,
        });
        let options = || wgpu::PipelineCompilationOptions { constants: &[], zero_initialize_workgroup_memory: false };
        let pipe = gpu.device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("native z fill"),
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &module,
                entry_point: Some("fill_vertex"),
                compilation_options: options(),
                buffers: &[],
            },
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                cull_mode: None,
                ..Default::default()
            },
            depth_stencil: Some(wgpu::DepthStencilState {
                format: wgpu::TextureFormat::Depth32Float,
                depth_write_enabled: Some(true),
                depth_compare: Some(wgpu::CompareFunction::Always),
                stencil: wgpu::StencilState::default(),
                bias: wgpu::DepthBiasState::default(),
            }),
            multisample: wgpu::MultisampleState { count: 1, mask: !0, alpha_to_coverage_enabled: false },
            fragment: Some(wgpu::FragmentState {
                module: &module,
                entry_point: Some("fill_depth"),
                compilation_options: options(),
                targets: &[],
            }),
            multiview_mask: None,
            cache: None,
        });
        DepthFill { layout, pipe }
    }
}

impl Native {
    /// `dhdr_native_surface`: surface `index` (< 48) at `scale`, at least w x h scaled pixels; its
    /// texture's size. Err keeps the old texture. Growth is drawn at the next picture's start
    /// (`Surfaces::prepare`).
    pub fn surface(&mut self, index: u32, scale: u32, w: u32, h: u32, depth: bool) -> Result<[u32; 2], String> {
        self.surfaces.resources(self.gpu, index as usize, scale, w, h, depth)
    }

    /// Surface `index`'s contents from a blob (`Surfaces::fill`; replays).
    pub fn surface_fill(&mut self, index: u32, bytes: &[u8]) -> Result<(), String> {
        self.surfaces.fill(self.gpu, index as usize, bytes)
    }

    /// Surface `index`'s contents (`Surfaces::read`; replays and checks).
    pub fn surface_read(&self, index: u32) -> Result<Vec<u8>, String> {
        self.surfaces.read(self.gpu, index as usize)
    }

    /// Surface `index` dropped (replays make only a picture's surfaces).
    pub fn surface_drop(&mut self, index: u32) {
        self.surfaces.drop_surface(index as usize)
    }
}
