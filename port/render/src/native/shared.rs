//! What every native texture shares, made once per device from any thread (task N3).
//!
//! As the retired `gpu_wgpu_native.inc`'s native_objects / native_init (last at render/rust
//! 3882d5576): the nearest sampler, the eight
//! replacement samplers, the texture bind group layout (native.wgsl group 1), the dummy textures
//! and the dummy group. Kept in `Gpu::shared` (`device.rs`), so textures can be made on any thread
//! before or after a `Native` exists. The texture layout here has the same entries as `init.rs`'s
//! draw layout group 1; wgpu-core pools equal bind group layouts (one object), so the groups made
//! here bind with the draw pipelines.
//!
//! Referee: `rustgs.py test ... -- --test native_textures` (N3).
use crate::device::Gpu;

pub struct NativeShared {
    /// Native.wgsl group 1: binding 0 a filterable float texture, binding 1 a filtering sampler
    /// (fragment stage).
    pub texture_layout: wgpu::BindGroupLayout,
    /// Group 1 of the `_u` entry points (decoded textures, finding 131): binding 0 an R32Uint
    /// texture, binding 1 a non-filtering sampler (the gathers').
    pub texels_layout: wgpu::BindGroupLayout,
    /// Clamped, nearest everywhere (surfaces, conversions' inputs, the dummy group).
    pub nearest: wgpu::Sampler,
    /// Repeating, nearest everywhere: the texels groups' (native.wgsl's gathers, whose footprint
    /// wraps as CLAMP repeat does; textureLoad ignores samplers).
    pub repeat: wgpu::Sampler,
    /// The replacement samplers, indexed bilinear | U clamped << 1 | V clamped << 2 (as
    /// gpu_native.inc's): trilinear minification, linear magnification and 16x anisotropy when
    /// bilinear, point magnification otherwise; clamped or repeating coordinates.
    pub replace: [wgpu::Sampler; 8],
    /// 1 x 1 RGBA8 ("native none"), never written (zeros).
    pub dummy: wgpu::Texture,
    pub dummy_view: wgpu::TextureView,
    /// 1 x 1 R32Uint ("native no texels"), never written (zeros): a conversion without a base.
    pub dummy_texels: wgpu::Texture,
    pub dummy_texels_view: wgpu::TextureView,
    /// 1 x 1 Depth32Float ("native no z"), never written (zeros).
    pub dummy_depth: wgpu::Texture,
    pub dummy_depth_view: wgpu::TextureView,
    /// The dummy texture with the nearest sampler (untextured draws).
    pub dummy_group: wgpu::BindGroup,
}

impl NativeShared {
    /// The device's shared objects, made on first use (any thread). Two threads racing here may
    /// both build them; one set is kept, the other dropped.
    pub fn get(gpu: &Gpu) -> Result<&NativeShared, String> {
        if let Some(s) = gpu.shared.get() {
            return Ok(s);
        }
        let made = scoped(gpu, "native shared objects", || NativeShared::create(gpu))?;
        let _ = gpu.shared.set(made);
        Ok(gpu.shared.get().expect("set just above"))
    }

    fn create(gpu: &Gpu) -> NativeShared {
        let d = &gpu.device;
        let texture_layout = d.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("native texture layout"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let texels_layout = d.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("native texels layout"),
            entries: &texels_entries(),
        });
        let nearest = d.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("native nearest"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            mipmap_filter: wgpu::MipmapFilterMode::Nearest,
            lod_min_clamp: 0.0,
            lod_max_clamp: 32.0,
            compare: None,
            anisotropy_clamp: 1,
            border_color: None,
        });
        let repeat = d.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("native texels repeat"),
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            address_mode_w: wgpu::AddressMode::Repeat,
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            mipmap_filter: wgpu::MipmapFilterMode::Nearest,
            lod_min_clamp: 0.0,
            lod_max_clamp: 32.0,
            compare: None,
            anisotropy_clamp: 1,
            border_color: None,
        });
        let replace = std::array::from_fn(|i| {
            let address = |clamped: bool| if clamped { wgpu::AddressMode::ClampToEdge } else { wgpu::AddressMode::Repeat };
            let bilinear = i & 1 != 0;
            d.create_sampler(&wgpu::SamplerDescriptor {
                label: Some("native replacement"),
                address_mode_u: address(i & 2 != 0),
                address_mode_v: address(i & 4 != 0),
                address_mode_w: wgpu::AddressMode::ClampToEdge,
                mag_filter: if bilinear { wgpu::FilterMode::Linear } else { wgpu::FilterMode::Nearest },
                min_filter: wgpu::FilterMode::Linear,
                mipmap_filter: wgpu::MipmapFilterMode::Linear,
                lod_min_clamp: 0.0,
                lod_max_clamp: 32.0,
                compare: None,
                anisotropy_clamp: if bilinear { 16 } else { 1 },
                border_color: None,
            })
        });
        let one = |format: wgpu::TextureFormat, label: &str| {
            d.create_texture(&wgpu::TextureDescriptor {
                label: Some(label),
                size: wgpu::Extent3d { width: 1, height: 1, depth_or_array_layers: 1 },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format,
                usage: wgpu::TextureUsages::TEXTURE_BINDING,
                view_formats: &[],
            })
        };
        let dummy = one(wgpu::TextureFormat::Rgba8Unorm, "native none");
        let dummy_depth = one(wgpu::TextureFormat::Depth32Float, "native no z");
        let dummy_texels = one(wgpu::TextureFormat::R32Uint, "native no texels");
        let dummy_texels_view = dummy_texels.create_view(&wgpu::TextureViewDescriptor::default());
        let dummy_view = dummy.create_view(&wgpu::TextureViewDescriptor::default());
        let dummy_depth_view = dummy_depth.create_view(&wgpu::TextureViewDescriptor::default());
        let dummy_group = texture_group(gpu, &texture_layout, &dummy_view, &nearest, "native none");
        NativeShared { texture_layout, texels_layout, nearest, repeat, replace, dummy, dummy_view, dummy_texels, dummy_texels_view,
                       dummy_depth, dummy_depth_view, dummy_group }
    }

    /// A group of `layout`-shape for `view` with `sampler` (native.wgsl group 1).
    pub fn group(&self, gpu: &Gpu, view: &wgpu::TextureView, sampler: &wgpu::Sampler, label: &str) -> wgpu::BindGroup {
        texture_group(gpu, &self.texture_layout, view, sampler, label)
    }

    /// A decoded texture's group (texels layout: its R32Uint view and the repeating sampler).
    pub fn texels_group(&self, gpu: &Gpu, view: &wgpu::TextureView, label: &str) -> wgpu::BindGroup {
        texture_group(gpu, &self.texels_layout, view, &self.repeat, label)
    }
}

/// The texels layout's entries (shared.rs's and init.rs's, equal so wgpu-core pools them).
pub fn texels_entries() -> [wgpu::BindGroupLayoutEntry; 2] {
    [
        wgpu::BindGroupLayoutEntry {
            binding: 0,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Uint,
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        },
        wgpu::BindGroupLayoutEntry {
            binding: 1,
            visibility: wgpu::ShaderStages::FRAGMENT,
            ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::NonFiltering),
            count: None,
        },
    ]
}

fn texture_group(
    gpu: &Gpu,
    layout: &wgpu::BindGroupLayout,
    view: &wgpu::TextureView,
    sampler: &wgpu::Sampler,
    label: &str,
) -> wgpu::BindGroup {
    gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some(label),
        layout,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(view) },
            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(sampler) },
        ],
    })
}

/// Runs `f` inside a Validation and an OutOfMemory error scope (this thread's: wgpu's scopes are
/// thread-local with `std`) and returns Err naming `what` when it raised an error, so a failed
/// create is an answer, not the uncaptured-error abort. Natively the scopes are popped at once
/// (wgpu-core reports synchronously). In the browser a scope could only be awaited, which a C
/// call can't; there errors go to the uncaptured-error handler (fatal).
pub(crate) fn scoped<R>(gpu: &Gpu, what: &str, f: impl FnOnce() -> R) -> Result<R, String> {
    #[cfg(feature = "native")]
    {
        let validation = gpu.device.push_error_scope(wgpu::ErrorFilter::Validation);
        let memory = gpu.device.push_error_scope(wgpu::ErrorFilter::OutOfMemory);
        let r = f();
        let m = pollster::block_on(memory.pop());
        let v = pollster::block_on(validation.pop());
        match (m, v) {
            (None, None) => Ok(r),
            (Some(e), _) | (None, Some(e)) => Err(format!("{what}: {e}")),
        }
    }
    #[cfg(not(feature = "native"))]
    {
        let _ = (gpu, what);
        Ok(f())
    }
}
