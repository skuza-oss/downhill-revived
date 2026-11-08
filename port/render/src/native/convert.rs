//! Texture conversion runs (task N5).
//!
//! As the retired `gpu_wgpu_native.inc`'s native_run_convert (`:670-714`, last at render/rust
//! 3882d5576): a picture's `DhdrConvert`
//! drawn by native.wgsl's `native_convert` compute pass into a converted texture (the
//! `converted[64]` cache, one per conversion index, remade only when its size changes), each
//! source slot's scale byte set in tex[3]; the constants go through the arena (1632 bytes).
//!
//! The caller (render.rs, N6) ends its open render pass first (native_end_pass) and passes the
//! native objects it owns (`ConvertObjects`) and the four source surfaces as they stand
//! (`ConvertSource`), so this module needs nothing of init.rs, shared.rs or surfaces.rs beyond
//! wgpu handles. The returned bind group is the draw's group 1 (the converted texture's R32Uint
//! texels with the texels sampler, as a decoded texture's); it is a cheap clone of the cached one.
//!
//! Referee: native_replay on the conversion pictures (N5; run with N6 at the gate).
use crate::abi::{DhdrConvert, DhdrConvertConstants};
use crate::device::Gpu;
use crate::native::arenas::Arenas;
use crate::native::init::INVERSE_WORDS;
use crate::native::pipelines::gputimes;

/// NATIVE_CONVERTS: conversions a picture holds at most (native_core.inc).
pub const CONVERTS: usize = 64;

/// The native objects a conversion uses (init.rs's and shared.rs's).
pub struct ConvertObjects<'a> {
    /// `native_convert` (native_objects' convert_pipe).
    pub pipeline: &'a wgpu::ComputePipeline,
    /// Group 1 of the convert pipeline (convert_layout: inverse, base, 4 colour, 4 depth, output).
    pub layout: &'a wgpu::BindGroupLayout,
    /// The inverse table (init.rs INVERSE_WORDS u32, inverse_table).
    pub inverse: &'a wgpu::Buffer,
    /// The draw texels layout and its sampler (a decoded texture's group, `NativeShared::texels_group`).
    pub texels_layout: &'a wgpu::BindGroupLayout,
    pub texels_sampler: &'a wgpu::Sampler,
    /// Bound where a slot has no colour (or no depth) surface, and where there is no base.
    pub dummy_view: &'a wgpu::TextureView,
    pub dummy_texels_view: &'a wgpu::TextureView,
    pub dummy_depth_view: &'a wgpu::TextureView,
}

/// A source slot's surface as it stands when the conversion runs.
#[derive(Clone, Copy)]
pub struct ConvertSource<'a> {
    /// Its texture's view; None when the surface has no texture yet (the conversion is skipped,
    /// as C returns NULL).
    pub view: Option<&'a wgpu::TextureView>,
    /// A Z (depth32float) surface.
    pub depth: bool,
    /// Its resolution scale (1-4).
    pub scale: u32,
}

/// One cached converted texture (converted[i]).
struct Converted {
    _texture: wgpu::Texture,
    view: wgpu::TextureView,
    group: wgpu::BindGroup,
    w: u32,
    h: u32,
}

pub struct Converts {
    /// converted[64]: by conversion index.
    slots: Vec<Option<Converted>>,
    /// Conversions run (n->stats.conversions_run).
    pub runs: u64,
}

impl Default for Converts {
    fn default() -> Converts {
        Converts::new()
    }
}

impl Converts {
    pub fn new() -> Converts {
        Converts { slots: (0..CONVERTS).map(|_| None).collect(), runs: 0 }
    }

    /// The size of converted texture `i` (0, 0 when it has none), for checks.
    pub fn size(&self, i: usize) -> (u32, u32) {
        self.slots.get(i).and_then(|s| s.as_ref()).map_or((0, 0), |c| (c.w, c.h))
    }

    /// native_run_convert: rebuilds conversion `i` (`v`, with `base` its decoded texture's view
    /// or None, `sources[k]` its slot k surface or None for -1) into its texture, recording the
    /// compute pass into `encoder` (no pass may be open on it). Returns the draw bind group, or
    /// None when the conversion can't run (size 0, a source without texture, `i` out of range).
    #[allow(clippy::too_many_arguments)]
    pub fn run(&mut self, gpu: &Gpu, encoder: &mut wgpu::CommandEncoder, arenas: &mut Arenas, o: &ConvertObjects,
               i: usize, v: &DhdrConvert, base: Option<&wgpu::TextureView>,
               sources: &[Option<ConvertSource>; 4]) -> Option<wgpu::BindGroup> {
        let (w, h) = (v.constants.size[0], v.constants.size[1]);
        if w == 0 || h == 0 || i >= CONVERTS {
            return None;
        }
        let mut constants: DhdrConvertConstants = v.constants;
        constants.tex[3] = 0; // each source's scale, a byte per slot
        let mut colour: [&wgpu::TextureView; 4] = [o.dummy_view; 4];
        let mut depth: [&wgpu::TextureView; 4] = [o.dummy_depth_view; 4];
        for (k, s) in sources.iter().enumerate() {
            let scale = match s {
                Some(s) => {
                    let view = s.view?;
                    if s.depth {
                        depth[k] = view;
                    } else {
                        colour[k] = view;
                    }
                    s.scale
                }
                None => 1,
            };
            constants.tex[3] |= scale << (k * 8);
        }
        let slot = &mut self.slots[i];
        if slot.as_ref().is_none_or(|c| c.w != w || c.h != h) {
            *slot = None;
            let max = gpu.device.limits().max_texture_dimension_2d;
            if w > max || h > max {
                return None;
            }
            let texture = gpu.device.create_texture(&wgpu::TextureDescriptor {
                label: Some("native converted"),
                size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                // Texels as a decoded texture's (each its RGBA bytes): the draws read it by the
                // `_u` entry points, the same bytes a unorm8 texture gave (finding 149).
                format: wgpu::TextureFormat::R32Uint,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::STORAGE_BINDING,
                view_formats: &[],
            });
            let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
            let group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("native converted group"),
                layout: o.texels_layout,
                entries: &[
                    wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&view) },
                    wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(o.texels_sampler) },
                ],
            });
            *slot = Some(Converted { _texture: texture, view, group, w, h });
        }
        let converted = slot.as_ref()?;
        let offset = arenas.constants(struct_bytes(&constants));
        let tv = wgpu::BindingResource::TextureView;
        let entries = [
            wgpu::BindGroupEntry {
                binding: 0,
                resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                    buffer: o.inverse,
                    offset: 0,
                    size: wgpu::BufferSize::new(INVERSE_WORDS as u64 * 4),
                }),
            },
            wgpu::BindGroupEntry { binding: 1, resource: tv(base.unwrap_or(o.dummy_texels_view)) },
            wgpu::BindGroupEntry { binding: 2, resource: tv(colour[0]) },
            wgpu::BindGroupEntry { binding: 3, resource: tv(colour[1]) },
            wgpu::BindGroupEntry { binding: 4, resource: tv(colour[2]) },
            wgpu::BindGroupEntry { binding: 5, resource: tv(colour[3]) },
            wgpu::BindGroupEntry { binding: 6, resource: tv(depth[0]) },
            wgpu::BindGroupEntry { binding: 7, resource: tv(depth[1]) },
            wgpu::BindGroupEntry { binding: 8, resource: tv(depth[2]) },
            wgpu::BindGroupEntry { binding: 9, resource: tv(depth[3]) },
            wgpu::BindGroupEntry { binding: 10, resource: tv(&converted.view) },
        ];
        let group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("native convert"),
            layout: o.layout,
            entries: &entries,
        });
        let uniforms = arenas.uniform_group()?;
        {
            let times = gputimes::slot(gpu, gputimes::CONVERT);
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: None, timestamp_writes: times.as_ref().map(gputimes::Slot::compute) });
            pass.set_pipeline(o.pipeline);
            pass.set_bind_group(0, uniforms, &[offset]);
            pass.set_bind_group(1, &group, &[]);
            pass.dispatch_workgroups(w.div_ceil(8), h.div_ceil(8), 1);
        }
        self.runs += 1;
        Some(converted.group.clone())
    }
}

/// A `#[repr(C)]` value's bytes (the constants blocks have no padding: u32 and u8 arrays only).
pub(crate) fn struct_bytes<T: Copy>(v: &T) -> &[u8] {
    // SAFETY: T is Copy plain data read as bytes for the value's own size; the callers pass
    // padding-free #[repr(C)] blocks (DhdrConvertConstants, [u32; 12]).
    unsafe { std::slice::from_raw_parts(v as *const T as *const u8, std::mem::size_of::<T>()) }
}
