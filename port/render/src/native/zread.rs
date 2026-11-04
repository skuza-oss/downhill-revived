//! The Z probe from the native renderer's own depth (finding 097): `zread_start` / `zread_take`.
//!
//! The game reads a 4x4 patch of its Z buffer every field to hide the lens flare behind the
//! scenery (func_00228328). After each picture the render thread has a small compute pass load
//! the patch's neighbourhood (at most 64 x 64 GS pixels; each the texel at the centre of its
//! scaled block) from the Z surface it just drew into a buffer, and maps a copy of it without
//! waiting (a depth texture can only be copied whole, so no texture copy); a later picture takes
//! the newest finished copy, so the EE's answer comes from the picture before the one it is
//! recording (a field or two late), as depth32float bits: the GS integer Z is `depth * unit` (C
//! converts).
use crate::exact::readback::{map_start, MapStatus};
use crate::native::Native;

/// GS pixels a copy covers at most, per side.
pub const ZREAD_MAX: u32 = 64;
const SLOTS: usize = 3;
const BYTES: u64 = (ZREAD_MAX * ZREAD_MAX * 4) as u64;

/// DHD_ZREAD_FOLD / DHD_GPU_OVERLAP (finding 111): off unless set, `0` turns one off, any other
/// value on; native_core.inc's native_gpu_switch reads them the same way.
pub fn switch(name: &str) -> bool {
    std::env::var_os(name).is_some_and(|v| v != "0")
}

/// The ordinary native render loop only; interpolation does not use its publication hook.
pub fn folded() -> bool {
    switch("DHD_ZREAD_FOLD")
        && std::env::var("DHD_INTERP").ok().and_then(|s| s.parse::<i32>().ok()).unwrap_or(0) == 0
}

const ZREAD_WGSL: &str = r#"
struct Params { origin: vec2<u32>, size: vec2<u32>, scale: u32, pad0: u32, pad1: u32, pad2: u32 }
@group(0) @binding(0) var zread_source: texture_depth_2d;
@group(0) @binding(1) var<storage, read_write> zread_out: array<u32>;
@group(0) @binding(2) var<uniform> zread_params: Params;
@compute @workgroup_size(8, 8, 1)
fn zread(@builtin(global_invocation_id) id: vec3<u32>) {
    if (id.x >= zread_params.size.x || id.y >= zread_params.size.y) { return; }
    let p = (zread_params.origin + id.xy) * zread_params.scale + zread_params.scale / 2u;
    zread_out[id.y * zread_params.size.x + id.x] = bitcast<u32>(textureLoad(zread_source, vec2<i32>(p), 0));
}
"#;

struct Slot {
    /// What the pass writes (STORAGE | COPY_SRC), its copy the CPU maps, the pass's parameters.
    storage: wgpu::Buffer,
    read: wgpu::Buffer,
    params: wgpu::Buffer,
    /// The map in flight or finished; None: free.
    status: Option<MapStatus>,
    /// GS pixels: x, y, w, h.
    rect: [u32; 4],
    tag: u64,
    needs_map: bool,
}

pub struct ZRead {
    pipe: Option<(wgpu::BindGroupLayout, wgpu::ComputePipeline)>,
    slots: Vec<Slot>,
    folded: bool,
    request: Option<(u32, [u32; 4], u64)>,
    recording: bool,
    current: Option<u64>,

}

impl ZRead {
    pub fn new() -> ZRead {
        ZRead { pipe: None, slots: Vec::new(), folded: folded(),
                request: None, recording: false, current: None }
    }
}

fn pipe(gpu: &crate::device::Gpu) -> (wgpu::BindGroupLayout, wgpu::ComputePipeline) {
    let module = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("native zread"),
        source: wgpu::ShaderSource::Wgsl(ZREAD_WGSL.into()),
    });
    let entry = |binding, ty| wgpu::BindGroupLayoutEntry { binding, visibility: wgpu::ShaderStages::COMPUTE, ty, count: None };
    let layout = gpu.device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("native zread"),
        entries: &[
            entry(0, wgpu::BindingType::Texture {
                sample_type: wgpu::TextureSampleType::Depth,
                view_dimension: wgpu::TextureViewDimension::D2,
                multisampled: false,
            }),
            entry(1, wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Storage { read_only: false },
                has_dynamic_offset: false,
                min_binding_size: None,
            }),
            entry(2, wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: false, min_binding_size: None }),
        ],
    });
    let pipeline_layout = gpu.device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("native zread"),
        bind_group_layouts: &[Some(&layout)],
        immediate_size: 0,
    });
    let pipeline = gpu.device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("native zread"),
        layout: Some(&pipeline_layout),
        module: &module,
        entry_point: Some("zread"),
        compilation_options: crate::native::init::compilation_options(),
        cache: None,
    });
    (layout, pipeline)
}

impl Native {
    /// `dhdr_native_zread_start`: loads the centre texel of each GS pixel of `rect` (x, y, w, h,
    /// clipped to the texture and to ZREAD_MAX) of Z surface `index` into a free buffer and starts
    /// mapping it, tagged `tag`. 1 started; 0 not (no such surface, nothing inside, no free buffer).
    pub fn zread_start(&mut self, index: u32, rect: [u32; 4], tag: u64) -> i32 {
        if self.zread.folded && !self.zread.recording {
            self.zread.request = Some((index, rect, tag));
            self.zread.current = Some(tag);
            return 1;
        }
        let whole = crate::native::render::rstat::T::new();
        let gpu = self.gpu;
        let Some(s) = self.surfaces.get(index as usize) else { return 0 };
        let (Some(view), true) = (s.view.as_ref(), s.depth) else { return 0 };
        let scale = s.scale.max(1);
        let (cols, rows) = (s.size[0] / scale, s.size[1] / scale);
        let [x, y, w, h] = rect;
        let w = w.min(ZREAD_MAX).min(cols.saturating_sub(x));
        let h = h.min(ZREAD_MAX).min(rows.saturating_sub(y));
        if w == 0 || h == 0 {
            return 0;
        }
        if self.zread.pipe.is_none() {
            self.zread.pipe = Some(pipe(gpu));
        }
        if self.zread.slots.len() < SLOTS && self.zread.slots.iter().all(|s| s.status.is_some()) {
            let buffer = |label, size, usage| gpu.device.create_buffer(&wgpu::BufferDescriptor { label: Some(label), size, usage, mapped_at_creation: false });
            self.zread.slots.push(Slot {
                storage: buffer("native zread", BYTES, wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC),
                read: buffer("native zread map", BYTES, wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST),
                params: buffer("native zread params", 32, wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST),
                status: None,
                rect: [0; 4],
                tag: 0,
                needs_map: false,
            });
        }
        let Some((layout, pipeline)) = self.zread.pipe.as_ref() else { return 0 };
        let Some(slot) = self.zread.slots.iter_mut().find(|s| s.status.is_none()) else { return 0 };
        let params: [u32; 8] = [x, y, w, h, scale, 0, 0, 0];
        let mut bytes = [0u8; 32];
        for (i, v) in params.iter().enumerate() {
            bytes[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
        }
        gpu.queue.write_buffer(&slot.params, 0, &bytes);
        let group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("native zread"),
            layout,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(view) },
                wgpu::BindGroupEntry { binding: 1, resource: slot.storage.as_entire_binding() },
                wgpu::BindGroupEntry { binding: 2, resource: slot.params.as_entire_binding() },
            ],
        });
        let mut separate = (!self.zread.folded).then(|| gpu.device.create_command_encoder(
            &wgpu::CommandEncoderDescriptor { label: Some("native zread") }));
        let encoder = match separate.as_mut() {
            Some(encoder) => encoder,
            None => self.pass.encoder(gpu),
        };
        {
            crate::native::render::pass_trace::event("compute-zprobe", -1, -1);
            let mut pass = encoder.begin_compute_pass(&wgpu::ComputePassDescriptor { label: Some("native zread"), timestamp_writes: None });
            pass.set_pipeline(pipeline);
            pass.set_bind_group(0, &group, &[]);
            pass.dispatch_workgroups(w.div_ceil(8), h.div_ceil(8), 1);
        }
        let used = (w * h * 4) as u64;
        encoder.copy_buffer_to_buffer(&slot.storage, 0, &slot.read, 0, used);
        if self.zread.folded {
            // Reserve the slot until the picture is submitted; mapping starts only afterwards.
            slot.status = Some(std::sync::Arc::new(std::sync::Mutex::new(None)));
            slot.needs_map = true;

        } else {
            gpu.queue.submit([separate.expect("separate probe encoder").finish()]);
            slot.status = Some(map_start(&slot.read, 0..used));
        }
        slot.rect = [x, y, w, h];
        slot.tag = tag;
        whole.add(crate::native::render::rstat::ZREAD_START);
        1
    }

    /// Append this field's requested probe after the picture's draws, before its submit.
    pub fn zread_record(&mut self) {
        if let Some((index, rect, tag)) = self.zread.request.take() {
            self.zread.recording = true;
            self.zread_start(index, rect, tag);
            self.zread.recording = false;
        }
    }

    /// Start the asynchronous maps once the picture's buffer copies have been submitted.
    pub fn zread_map(&mut self) {
        for slot in &mut self.zread.slots {
            if slot.needs_map {
                slot.status = Some(map_start(&slot.read, 0..(slot.rect[2] * slot.rect[3] * 4) as u64));
                slot.needs_map = false;
            }
        }
    }

    /// `dhdr_native_zread_take`: the newest finished copy (never waits): its rect and tag, and
    /// w * h depth32float bit patterns, row-major, in `out`. 1 a new copy; 0 none finished.
    /// Older finished copies are dropped.
    pub fn zread_take(&mut self, out: &mut [u32], rect: &mut [u32; 4], tag: &mut u64) -> i32 {
        let whole = crate::native::render::rstat::T::new();
        // A minimised/missing window skips present; the picture must still reach the queue.
        if self.pass.deferred {
            self.pass.submit_now(self.gpu, &self.arenas);
            self.zread_map();
        }
        #[cfg(not(target_arch = "wasm32"))]
        let _ = self.gpu.device.poll(wgpu::PollType::Poll);
        // The old take runs before starting the current field's probe. Folding must never
        // publish that current copy early, even when the GPU has already finished it.
        let current = self.zread.current;
        let folded = self.zread.folded;
        let done = |s: &Slot| {
            (!folded || current != Some(s.tag)) && s.status.as_ref().is_some_and(|m| m.lock().unwrap_or_else(|p| p.into_inner()).is_some())
        };
        let newest = self.zread.slots.iter().filter(|s| done(s)).map(|s| s.tag).max();
        let Some(newest) = newest else { return 0 };
        let mut taken = 0;
        for s in self.zread.slots.iter_mut() {
            if !done(s) || s.tag > newest {
                continue;
            }
            let ok = s.status.as_ref().is_some_and(|m| matches!(*m.lock().unwrap_or_else(|p| p.into_inner()), Some(Ok(()))));
            if s.tag == newest && ok {
                let [_, _, w, h] = s.rect;
                let count = (w * h) as usize;
                if out.len() >= count {
                    let data = s.read.slice(..(count * 4) as u64).get_mapped_range();
                    // One copy of the mapped bytes (wide loads), then each word from its little-endian
                    // bytes: four byte loads a word from the mapped memory cost ~0.4 ms a
                    // picture on the throttled phone (finding 146).
                    let words = &mut out[..count];
                    // SAFETY: the u32s' bytes, as many as `words` holds, all of them written here.
                    unsafe { std::slice::from_raw_parts_mut(words.as_mut_ptr().cast::<u8>(), count * 4) }
                        .copy_from_slice(&data[..count * 4]);
                    for word in words.iter_mut() {
                        *word = u32::from_le(*word);
                    }
                    *rect = s.rect;
                    *tag = s.tag;
                    taken = 1;
                }
            }
            if ok {
                s.read.unmap();
            }
            s.status = None;
        }
        whole.add(crate::native::render::rstat::ZREAD_TAKE);
        taken
    }
}
