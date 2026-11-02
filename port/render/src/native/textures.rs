//! `DhdrTexture`: decoded textures and replacements (task N3).
//!
//! As the retired `gpu_wgpu_native.inc`'s native_texture_create / native_replace_create (last at
//! render/rust 3882d5576): a decoded texture is
//! w x h R32Uint, each texel its RGBA8 bytes (finding 131: read as integers, a 2 x 2 footprint in
//! one gather), with one bind group (shared.rs's texels layout, the nearest sampler); a replacement (texture pack, sharp font)
//! has `levels` mip levels, each level's rows after the previous level's (sides halved, down to 1;
//! `mip_bytes`), and eight bind groups (one per replacement sampler, `NativeShared::replace`).
//! Immutable after creation, shared through `Arc` (the handle is `Arc::into_raw`, lib.rs
//! `native::texture_*`), made and dropped on any thread. Rows are uploaded with
//! `Queue::write_texture` (no 256-byte padding needed there); they land before the next submit.
//!
//! Usage: TEXTURE_BINDING | COPY_DST as the C path's, plus COPY_SRC for `read_level` (a check
//! path; it changes no sampled value).
//!
//! Referee: `rustgs.py test ... -- --test native_textures`.
use crate::device::Gpu;
use crate::native::shared::{scoped, NativeShared};

/// One texture (Send + Sync).
pub struct Tex {
    pub texture: wgpu::Texture,
    /// All levels.
    pub view: wgpu::TextureView,
    /// 1 group (decoded, nearest sampler) or 8 (replacement, indexed as `NativeShared::replace`).
    groups: Box<[wgpu::BindGroup]>,
    w: u32,
    h: u32,
    levels: u32,
}

impl Tex {
    /// A decoded texture: `rgba` is w x h pixels, rows unpadded.
    pub fn create(gpu: &Gpu, rgba: &[u32], w: u32, h: u32) -> Result<Tex, String> {
        #[cfg(not(target_arch = "wasm32"))]
        let _spike = crate::native::pipelines::SpikeTimer::new("texture", 0);
        check_size(gpu, w, h)?;
        let count = w as usize * h as usize;
        if rgba.len() < count {
            return Err(format!("native texture {w} x {h}: {} pixels given", rgba.len()));
        }
        let shared = NativeShared::get(gpu)?;
        let bytes: &[u8] = cast_bytes(&rgba[..count]);
        scoped(gpu, "native texture", || {
            let texture = new_texture(gpu, w, h, 1, wgpu::TextureFormat::R32Uint, "native texture");
            write_level(gpu, &texture, 0, bytes, w, h);
            let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
            let group = shared.texels_group(gpu, &view, "native texture");
            Tex { texture, view, groups: Box::new([group]), w, h, levels: 1 }
        })
    }

    /// A replacement: `rgba_mips` holds `levels` levels (`crate::native::mip_bytes(w, h, levels)`).
    pub fn replace(gpu: &Gpu, rgba_mips: &[u8], w: u32, h: u32, levels: u32) -> Result<Tex, String> {
        check_size(gpu, w, h)?;
        let Some(total) = crate::native::mip_bytes(w, h, levels) else {
            return Err(format!("native replacement {w} x {h} with {levels} levels"));
        };
        if rgba_mips.len() < total {
            return Err(format!("native replacement {w} x {h} x {levels}: {} of {total} bytes given", rgba_mips.len()));
        }
        let shared = NativeShared::get(gpu)?;
        scoped(gpu, "native replacement", || {
            let texture = new_texture(gpu, w, h, levels, wgpu::TextureFormat::Rgba8Unorm, "native replacement");
            let (mut at, mut lw, mut lh) = (0usize, w, h);
            for level in 0..levels {
                let n = lw as usize * lh as usize * 4;
                write_level(gpu, &texture, level, &rgba_mips[at..at + n], lw, lh);
                at += n;
                lw = (lw / 2).max(1);
                lh = (lh / 2).max(1);
            }
            let view = texture.create_view(&wgpu::TextureViewDescriptor::default());
            let groups: Vec<_> =
                shared.replace.iter().map(|s| shared.group(gpu, &view, s, "native replacement")).collect();
            Tex { texture, view, groups: groups.into_boxed_slice(), w, h, levels }
        })
    }

    /// (width, height, levels)
    pub fn size(&self) -> (u32, u32, u32) {
        (self.w, self.h, self.levels)
    }

    /// Bind groups: 1 for a decoded texture, 8 for a replacement.
    pub fn groups(&self) -> usize {
        self.groups.len()
    }

    /// The group for replacement sampler `sampler` (bilinear | U clamped << 1 | V clamped << 2);
    /// a decoded texture has its one group whatever `sampler` is.
    pub fn group(&self, sampler: usize) -> &wgpu::BindGroup {
        if self.groups.len() == 1 {
            &self.groups[0]
        } else {
            &self.groups[sampler & 7]
        }
    }

    /// Level `level` read back from the GPU, rows unpadded (RGBA8). For checks; blocks natively.
    pub fn read_level(&self, gpu: &Gpu, level: u32) -> Result<Vec<u8>, String> {
        if level >= self.levels {
            return Err(format!("level {level} of a texture with {} levels", self.levels));
        }
        read_level(gpu, &self.texture, level, (self.w >> level).max(1), (self.h >> level).max(1))
    }
}

fn check_size(gpu: &Gpu, w: u32, h: u32) -> Result<(), String> {
    let max = gpu.device.limits().max_texture_dimension_2d;
    if w == 0 || h == 0 || w > max || h > max {
        return Err(format!("native texture size {w} x {h} (the device allows 1 to {max})"));
    }
    Ok(())
}

fn cast_bytes(p: &[u32]) -> &[u8] {
    // SAFETY: u32 has no padding and u8 alignment is 1; the length covers the same bytes.
    unsafe { std::slice::from_raw_parts(p.as_ptr() as *const u8, std::mem::size_of_val(p)) }
}

fn new_texture(gpu: &Gpu, w: u32, h: u32, levels: u32, format: wgpu::TextureFormat, label: &str) -> wgpu::Texture {
    gpu.device.create_texture(&wgpu::TextureDescriptor {
        label: Some(label),
        size: wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: levels,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format,
        usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST | wgpu::TextureUsages::COPY_SRC,
        view_formats: &[],
    })
}

fn write_level(gpu: &Gpu, texture: &wgpu::Texture, level: u32, bytes: &[u8], w: u32, h: u32) {
    gpu.queue.write_texture(
        wgpu::TexelCopyTextureInfo {
            texture,
            mip_level: level,
            origin: wgpu::Origin3d::ZERO,
            aspect: wgpu::TextureAspect::All,
        },
        bytes,
        wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(w * 4), rows_per_image: Some(h) },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
}

#[cfg(feature = "native")]
fn read_level(gpu: &Gpu, texture: &wgpu::Texture, level: u32, w: u32, h: u32) -> Result<Vec<u8>, String> {
    use std::sync::{Arc, Mutex};
    let row = w * 4;
    let padded = row.div_ceil(wgpu::COPY_BYTES_PER_ROW_ALIGNMENT) * wgpu::COPY_BYTES_PER_ROW_ALIGNMENT;
    let size = padded as u64 * h as u64;
    let buffer = scoped(gpu, "native texture read", || {
        let buffer = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("native texture read"),
            size,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut e = gpu.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("native texture read") });
        e.copy_texture_to_buffer(
            wgpu::TexelCopyTextureInfo { texture, mip_level: level, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            wgpu::TexelCopyBufferInfo {
                buffer: &buffer,
                layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(padded), rows_per_image: Some(h) },
            },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        );
        gpu.queue.submit([e.finish()]);
        buffer
    })?;
    let status = Arc::new(Mutex::new(None));
    let s = status.clone();
    buffer.map_async(wgpu::MapMode::Read, 0..size, move |r| *s.lock().unwrap_or_else(|p| p.into_inner()) = Some(r));
    gpu.device.poll(wgpu::PollType::wait_indefinitely()).map_err(|e| format!("native texture read: poll: {e}"))?;
    match status.lock().unwrap_or_else(|p| p.into_inner()).take() {
        Some(Ok(())) => {}
        Some(Err(e)) => return Err(format!("native texture read: map failed: {e}")),
        None => return Err("native texture read: the map callback did not run".into()),
    }
    let mut out = Vec::with_capacity(row as usize * h as usize);
    {
        let mapped = buffer.get_mapped_range(0..size);
        for y in 0..h as usize {
            let at = y * padded as usize;
            out.extend_from_slice(&mapped[at..at + row as usize]);
        }
    }
    buffer.unmap();
    Ok(out)
}

#[cfg(not(feature = "native"))]
fn read_level(gpu: &Gpu, texture: &wgpu::Texture, level: u32, w: u32, h: u32) -> Result<Vec<u8>, String> {
    let _ = (gpu, texture, level, w, h);
    Err("native texture read: not in the browser (a blocking read; replays and checks only)".into())
}
