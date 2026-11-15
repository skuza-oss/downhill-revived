//! A colour surface's pixels for the EE (finding 250): `dhdr_native_surface_read_begin` /
//! `_finish` (`include/dhd_render_ext.h`).
//!
//! The native renderer draws only on the GPU, so a local-to-host transfer of a colour buffer it
//! drew read GS memory that never held the picture: Poster Maker (func_0019C220 reads the 512 x 224
//! frame at block 0x700) showed and saved a black poster (issue #102). C (native_core.inc's
//! native_colour_read) waits until every handed picture is drawn, then asks for the read's
//! rectangle here: the surface's scaled texels are copied to a buffer and mapped (natively the begin
//! blocks until mapped; in the browser C waits in web_gpu_await between begin and finish), and the
//! finish gives one RGBA8 word per GS pixel, the mean of its scale x scale block, in the bytes C's
//! CT32 import puts in a surface (native_import_page: R G B A, the GS alpha as it is).
use crate::abi::DhdrNative;
use crate::exact::readback::{map_start, MapStatus};
use crate::native::Native;

/// The read in flight: GS pixels w x h, the surface's scale, the buffer's row stride in bytes.
pub struct Reading {
    w: u32,
    h: u32,
    scale: u32,
    stride: u32,
    bytes: u64,
    buffer: wgpu::Buffer,
    status: MapStatus,
}

impl Native {
    /// Starts reading GS pixels `rect` (x, y, w, h; clipped to the texture) of colour surface
    /// `index`; Some((w, h)) as clipped, None when there is nothing to read (no such colour surface,
    /// no texture, nothing inside).
    pub fn surface_read_begin(&mut self, index: u32, rect: [u32; 4]) -> Option<(u32, u32)> {
        let gpu = self.gpu;
        self.surface_read = None;
        let s = self.surfaces.get(index as usize)?;
        let texture = s.texture.as_ref()?;
        if s.depth {
            return None;
        }
        let scale = s.scale.max(1);
        let (cols, rows) = (s.size[0] / scale, s.size[1] / scale);
        let [x, y, w, h] = rect;
        let w = w.min(cols.saturating_sub(x));
        let h = h.min(rows.saturating_sub(y));
        if w == 0 || h == 0 {
            return None;
        }
        let (tw, th) = (w * scale, h * scale);
        let stride = (tw * 4 + 255) & !255;
        let bytes = stride as u64 * th as u64;
        let buffer = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("native surface read"),
            size: bytes,
            usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let mut e = gpu.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("native surface read") });
        e.copy_texture_to_buffer(
            wgpu::TexelCopyTextureInfo {
                texture,
                mip_level: 0,
                origin: wgpu::Origin3d { x: x * scale, y: y * scale, z: 0 },
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::TexelCopyBufferInfo {
                buffer: &buffer,
                layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(stride), rows_per_image: Some(th) },
            },
            wgpu::Extent3d { width: tw, height: th, depth_or_array_layers: 1 },
        );
        crate::startup::submit(&gpu.queue, [e.finish()]);
        let status = map_start(&buffer, 0..bytes);
        #[cfg(not(target_arch = "wasm32"))]
        if let Err(err) = crate::exact::readback::wait(gpu) {
            crate::host::log(&format!("dhd_render: native surface read: {err}"));
        }
        self.surface_read = Some(Reading { w, h, scale, stride, bytes, buffer, status });
        Some((w, h))
    }

    /// The read's w * h pixels, row-major, each the mean of its texel block; false when the map
    /// failed (logged).
    pub fn surface_read_finish(&mut self, pixels: &mut [u32]) -> bool {
        let Some(r) = self.surface_read.take() else {
            crate::host::log("dhd_render: native surface read: no read in flight");
            return false;
        };
        match r.status.lock().unwrap_or_else(|p| p.into_inner()).take() {
            Some(Ok(())) => {}
            Some(Err(err)) => {
                crate::host::log(&format!("dhd_render: native surface read: map failed: {err}"));
                return false;
            }
            None => {
                crate::host::log("dhd_render: native surface read: the map did not finish");
                return false;
            }
        }
        {
            let mapped = r.buffer.get_mapped_range(0..r.bytes);
            let (s, n) = (r.scale as usize, (r.scale * r.scale) as u32);
            for y in 0..r.h as usize {
                for x in 0..r.w as usize {
                    let mut sum = [0u32; 4];
                    for j in 0..s {
                        let at = (y * s + j) * r.stride as usize + x * s * 4;
                        for texel in mapped[at..at + s * 4].chunks_exact(4) {
                            for c in 0..4 {
                                sum[c] += texel[c] as u32;
                            }
                        }
                    }
                    let mean = |c: usize| (sum[c] + n / 2) / n;
                    pixels[y * r.w as usize + x] = mean(0) | mean(1) << 8 | mean(2) << 16 | mean(3) << 24;
                }
            }
        } // the mapped view drops before unmap
        r.buffer.unmap();
        true
    }

    /// The size the read in flight returns (0, 0 when none).
    pub fn surface_read_size(&self) -> (u32, u32) {
        self.surface_read.as_ref().map_or((0, 0), |r| (r.w, r.h))
    }
}

/// # Safety
/// `n` is a live `DhdrNative` or NULL (fatal); `rect` is uint32_t[4]; `size` is uint32_t[2].
#[no_mangle]
pub unsafe extern "C" fn dhdr_native_surface_read_begin(n: *mut DhdrNative, index: u32, rect: *const u32,
                                                        size: *mut u32) -> i32 {
    const E: &str = "dhdr_native_surface_read_begin";
    let _g = crate::guard::Guard::enter(E);
    // SAFETY: rect and size are the header's arrays, or NULL (fatal).
    let (Some(rect), Some(size)) = (unsafe { (rect as *const [u32; 4]).as_ref() }, unsafe { (size as *mut [u32; 2]).as_mut() })
    else {
        crate::guard::fatal(&format!("dhd_render: {E}: NULL rect or size"))
    };
    // SAFETY: n is a live DhdrNative (`ffi.rs`: an `Owned<Native>` behind the handle), or NULL.
    let Some(owned) = (unsafe { (n as *const crate::guard::Owned<Native>).as_ref() }) else {
        crate::guard::fatal(&format!("dhd_render: {E}: NULL handle"))
    };
    owned.begin(E, |n| match n.surface_read_begin(index, *rect) {
        Some((w, h)) => {
            *size = [w, h];
            (1, true)
        }
        None => {
            *size = [0, 0];
            (0, false)
        }
    })
}

/// # Safety
/// `n` as `dhdr_native_surface_read_begin`; `pixels` holds the w * h words its begin gave.
#[no_mangle]
pub unsafe extern "C" fn dhdr_native_surface_read_finish(n: *mut DhdrNative, pixels: *mut u32) -> i32 {
    const E: &str = "dhdr_native_surface_read_finish";
    let _g = crate::guard::Guard::enter(E);
    // SAFETY: as begin.
    let Some(owned) = (unsafe { (n as *const crate::guard::Owned<Native>).as_ref() }) else {
        crate::guard::fatal(&format!("dhd_render: {E}: NULL handle"))
    };
    owned.finish(E, "dhdr_native_surface_read_begin", |n| {
        let (w, h) = n.surface_read_size();
        if pixels.is_null() {
            crate::guard::fatal(&format!("dhd_render: {E}: NULL pixels"));
        }
        // SAFETY: pixels holds the w * h words begin announced (the header).
        let pixels = unsafe { std::slice::from_raw_parts_mut(pixels, w as usize * h as usize) };
        n.surface_read_finish(pixels) as i32
    })
}
