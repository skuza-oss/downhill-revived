//! The exact renderer's 11 buffers and its bind groups (task A3).
//!
//! Numbered `DHDR_B_*`; sizes and usages as the retired renderers' `gpu_wgpu.c` wgpu_init and
//! `gpu.js` had them (last at render/rust 3882d5576; OPTRACE.md's
//! buffer table): vram (storage, copy src/dst), snapshot, states, primitives, bins (storage, copy
//! dst), display and copy constants (48-byte uniforms), readback and staged (map read, copy dst),
//! batches (uniform, 4096 x 256), upload (copy src/dst). Every buffer starts zeroed (wgpu clears a
//! buffer made without `mapped_at_creation` before its first use, as D3D11's CreateBuffer without
//! data does). main's bind group (the batch record at binding 0, 16 bytes at a dynamic offset;
//! snapshot 1, vram 2, primitives 3, bins 4, states 10) and local_copy's (vram 2, copy constants 6).
//! Scanout's group belongs to the display texture (`display.rs`).
//!
//! Natively (the `native` feature) everything is made inside OutOfMemory and Validation error
//! scopes, so a failure is an `Err` (the export logs it and returns NULL, as wgpu_init's
//! `goto fail`) and never reaches the device's fatal uncaptured-error handler. In the browser a
//! scope can't be awaited here, so a failure there is the device's error (B1).
//!
//! Uses from `exact::pipelines` (A2): `Pipelines::main` and `Pipelines::local_copy`
//! (`wgpu::ComputePipeline`); their group 0 layouts come from `get_bind_group_layout(0)`, which is
//! main's explicit layout for main (as `gpu_wgpu.c`'s bind_group did).
//!
//! Referee: optrace_replay on the test_gpu and native_transfers traces (A3).
use crate::abi::*;
use crate::device::Gpu;
use crate::exact::pipelines::Pipelines;
use std::num::NonZeroU64;

/// Sizes in bytes, by `DHDR_B_*` (OPTRACE.md).
pub const SIZES: [u64; 11] = [
    4 << 20,            // vram
    4 << 20,            // snapshot
    1024 * 3088,        // states: MAX_STATES x sizeof(Constants)
    16384 * 208,        // primitives: MAX_PRIMITIVES x sizeof(Primitive)
    4 * 1024 * 1024 * 4, // bins: MAX_BIN_WORDS x 4
    48,                 // display constants
    48,                 // copy constants
    4 << 20,            // readback
    4 << 20,            // staged
    4096 * 256,         // batches: MAX_BATCHES x BATCH_STRIDE
    4 << 20,            // upload
];

/// Labels, by `DHDR_B_*` (`gpu_wgpu.c`'s).
const LABELS: [&str; 11] = ["vram", "snapshot", "states", "primitives", "bins", "display", "copy", "readback",
                            "staged", "batches", "upload"];

/// Usages, by `DHDR_B_*` (`gpu_wgpu.c`'s wgpu_init; `gpu.js`'s buffers).
pub fn usage(id: u32) -> wgpu::BufferUsages {
    use wgpu::BufferUsages as U;
    let storage = U::STORAGE | U::COPY_DST;
    match id {
        DHDR_B_VRAM => storage | U::COPY_SRC,
        DHDR_B_SNAPSHOT | DHDR_B_STATES | DHDR_B_PRIMITIVES | DHDR_B_BINS => storage,
        DHDR_B_DISPLAY | DHDR_B_COPY | DHDR_B_BATCHES => U::UNIFORM | U::COPY_DST,
        DHDR_B_READBACK | DHDR_B_STAGED => U::MAP_READ | U::COPY_DST,
        DHDR_B_UPLOAD => U::COPY_SRC | U::COPY_DST,
        _ => U::empty(),
    }
}

/// The batch record main reads at binding 0 (raster.wgsl's `Batch`): 16 bytes at each dynamic
/// offset.
pub const BATCH_RECORD_BYTES: u64 = 16;

pub struct Buffers {
    /// The 11 buffers, in `DHDR_B_*` order.
    pub list: [wgpu::Buffer; 11],
    /// main's group 0, used with one dynamic offset per dispatch (the batch record).
    pub main_group: wgpu::BindGroup,
    /// local_copy's group 0.
    pub copy_group: wgpu::BindGroup,
}

impl Buffers {
    pub fn create(gpu: &Gpu, pipelines: &Pipelines) -> Result<Buffers, String> {
        let _startup = crate::startup::step("exact buffers and error scopes");
        // pollster comes with the `native` feature (as native/init.rs's scoped).
        #[cfg(feature = "native")]
        {
            let oom = gpu.device.push_error_scope(wgpu::ErrorFilter::OutOfMemory);
            let validation = gpu.device.push_error_scope(wgpu::ErrorFilter::Validation);
            let made = Self::make(gpu, pipelines);
            let v = pollster::block_on(validation.pop());
            let o = pollster::block_on(oom.pop());
            if let Some(e) = v.or(o) {
                return Err(format!("exact buffers: {e}"));
            }
            Ok(made)
        }
        #[cfg(not(feature = "native"))]
        Ok(Self::make(gpu, pipelines))
    }

    fn make(gpu: &Gpu, pipelines: &Pipelines) -> Buffers {
        let list: [wgpu::Buffer; 11] = std::array::from_fn(|i| {
            let _startup = crate::startup::step(LABELS[i]);
            gpu.device.create_buffer(&wgpu::BufferDescriptor {
                label: Some(LABELS[i]),
                size: SIZES[i],
                usage: usage(i as u32),
                mapped_at_creation: false,
            })
        });
        let whole = |binding: u32, id: u32| wgpu::BindGroupEntry {
            binding,
            resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                buffer: &list[id as usize],
                offset: 0,
                size: NonZeroU64::new(SIZES[id as usize]),
            }),
        };
        let record = wgpu::BindGroupEntry {
            binding: 0,
            resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                buffer: &list[DHDR_B_BATCHES as usize],
                offset: 0,
                size: NonZeroU64::new(BATCH_RECORD_BYTES),
            }),
        };
        let main_group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("main"),
            layout: &pipelines.main.get_bind_group_layout(0),
            entries: &[record, whole(1, DHDR_B_SNAPSHOT), whole(2, DHDR_B_VRAM), whole(3, DHDR_B_PRIMITIVES),
                       whole(4, DHDR_B_BINS), whole(10, DHDR_B_STATES)],
        });
        let copy_group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("local_copy"),
            layout: &pipelines.local_copy.get_bind_group_layout(0),
            entries: &[whole(2, DHDR_B_VRAM), whole(6, DHDR_B_COPY)],
        });
        Buffers { list, main_group, copy_group }
    }

    /// Buffer `id` (`DHDR_B_*`), or None outside the header's numbers.
    pub fn checked(&self, id: u32) -> Option<&wgpu::Buffer> {
        self.list.get(id as usize)
    }

    /// Buffer `id` (`DHDR_B_*`). A number outside the header's is fatal (logged, then abort).
    pub fn get(&self, id: u32) -> &wgpu::Buffer {
        self.checked(id)
            .unwrap_or_else(|| crate::guard::fatal(&format!("dhd_render: {}: buffer {id} is not a DHDR_B_* number",
                                                            crate::guard::current_export())))
    }
}
