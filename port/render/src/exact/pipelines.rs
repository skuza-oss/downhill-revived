//! raster.wgsl's compute pipelines for the exact renderer (task A2).
//!
//! As the retired `gpu_wgpu.c` built them (`main_pipeline_layout`, `raster_pipelines`,
//! gpu_wgpu.c:220-283, last at render/rust 3882d5576):
//! `main` with its explicit layout (binding 0 the batch record, a uniform with a dynamic offset and
//! min binding size 16; binding 2 read-write storage; 1, 3, 4 and 10 read-only storage),
//! `local_copy` and `scanout` with automatic layouts. The override constants by name as f64:
//! `EARLY_DEPTH`, `ADDRESS_SHIFT`, `EXCLUSIVE_WORD_STORE` (main) and `ADDRESS_SHIFT` (local_copy),
//! from `raster_flags` (`DHDR_RASTER_*`, 8 keys); scanout takes none. Built eagerly, inside a
//! Validation and an OutOfMemory error scope, under the export's MXCSR guard; compilation options
//! `zero_initialize_workgroup_memory = false`, no pipeline cache.
//!
//! The cache: like the C path (one module, one `main` layout and one scanout per process, one
//! main / local_copy pair per key), per device. A key is built once per device and kept for the
//! process; a build that fails stores nothing. Natively `Pipelines::create` blocks on the scopes.
//! In the browser the scopes can only be awaited: `web/init.rs` awaits `create_async` for its key
//! (that fills the cache), and `create` then returns the cached pipelines. A `create` in the
//! browser for a key not built yet builds it with no scope (an error there reaches the device's
//! uncaptured-error handler).
//!
//! Referee: `rustgs.py test --target x86_64-pc-windows-msvc -- --test device_smoke exact`.
use crate::device::Gpu;
use std::num::NonZeroU64;
use std::sync::Mutex;

/// The raster keys: `DHDR_RASTER_EARLY_DEPTH | ADDRESS_SHIFT | EXCLUSIVE_WORD_STORE`.
pub const KEYS: u32 = 8;

/// main's bindings in raster.wgsl (`@group(0)`): the batch record, texture memory, memory,
/// primitives, the tile list, the states.
pub const MAIN_BINDINGS: [u32; 6] = [0, 1, 2, 3, 4, 10];

/// The pipelines of one raster key. Cheap to clone (wgpu handles).
#[derive(Clone, Debug)]
pub struct Pipelines {
    /// `DHDR_RASTER_*` they were built for.
    pub raster_flags: u32,
    /// raster.wgsl `main` over main's explicit layout.
    pub main: wgpu::ComputePipeline,
    /// main's bind group layout (binding 0: 16 bytes at a dynamic offset).
    pub main_layout: wgpu::BindGroupLayout,
    /// raster.wgsl `local_copy` (automatic layout).
    pub local_copy: wgpu::ComputePipeline,
    /// local_copy's group 0 layout (from the pipeline).
    pub local_copy_layout: wgpu::BindGroupLayout,
    /// raster.wgsl `scanout` (automatic layout; the same for every key).
    pub scanout: wgpu::ComputePipeline,
    /// scanout's group 0 layout (from the pipeline).
    pub scanout_layout: wgpu::BindGroupLayout,
}

impl Pipelines {
    /// main, local_copy and scanout for `raster_flags` (0-7). Err on any error (scope or other).
    /// Natively it blocks on the error scopes; in the browser see the module comment.
    pub fn create(gpu: &Gpu, raster_flags: u32) -> Result<Pipelines, String> {
        check_key(raster_flags)?;
        if let Some(p) = cached(gpu, raster_flags) {
            return Ok(p);
        }
        #[cfg(all(feature = "native", not(target_arch = "wasm32")))]
        {
            pollster::block_on(Self::create_async(gpu, raster_flags))
        }
        #[cfg(not(all(feature = "native", not(target_arch = "wasm32"))))]
        {
            let (shared, keyed) = build(gpu, raster_flags);
            store(gpu, &shared, raster_flags, &keyed);
            Ok(assemble(raster_flags, &shared, &keyed))
        }
    }

    /// As `create`, awaiting the error scopes (the browser's async init; natively `create` blocks
    /// on this).
    pub async fn create_async(gpu: &Gpu, raster_flags: u32) -> Result<Pipelines, String> {
        check_key(raster_flags)?;
        if let Some(p) = cached(gpu, raster_flags) {
            return Ok(p);
        }
        let validation = gpu.device.push_error_scope(wgpu::ErrorFilter::Validation);
        let memory = gpu.device.push_error_scope(wgpu::ErrorFilter::OutOfMemory);
        // A pipeline the driver did not create (the vendored wgpu-hal's map_pipeline_creation_err,
        // finding 176).
        let internal = gpu.device.push_error_scope(wgpu::ErrorFilter::Internal);
        let (shared, keyed) = build(gpu, raster_flags);
        let i = internal.pop().await;
        let m = memory.pop().await;
        let v = validation.pop().await;
        if let Some(e) = v.or(i) {
            return Err(format!("raster.wgsl pipelines (raster key {raster_flags}): {e}"));
        }
        if let Some(e) = m {
            return Err(format!("raster.wgsl pipelines (raster key {raster_flags}): out of memory: {e}"));
        }
        store(gpu, &shared, raster_flags, &keyed);
        Ok(assemble(raster_flags, &shared, &keyed))
    }
}

/// raster.wgsl as one module of `gpu`'s device (the cached one, or a new one; the caller checks
/// errors). For the test hooks' `depth_probe`.
pub fn raster_module(gpu: &Gpu) -> wgpu::ShaderModule {
    if let Some(s) = with_entry(gpu, |e| e.shared.clone()) {
        return s.module;
    }
    module(gpu)
}

// ---------------------------------------------------------------- the cache

/// What one device shares across keys.
#[derive(Clone)]
struct Shared {
    module: wgpu::ShaderModule,
    main_layout: wgpu::BindGroupLayout,
    main_pipeline_layout: wgpu::PipelineLayout,
    scanout: wgpu::ComputePipeline,
    scanout_layout: wgpu::BindGroupLayout,
}

/// One key's pipelines.
#[derive(Clone)]
struct Keyed {
    main: wgpu::ComputePipeline,
    local_copy: wgpu::ComputePipeline,
    local_copy_layout: wgpu::BindGroupLayout,
}

/// A device of the cache. wgpu-core compares devices by id only, and ids restart in every
/// instance, so the key is (instance, device); the kept instance clone keeps the instance's
/// address (its identity) from being reused.
struct Entry {
    instance: wgpu::Instance,
    device: wgpu::Device,
    shared: Shared,
    keys: [Option<Keyed>; KEYS as usize],
}

static CACHE: Mutex<Vec<Entry>> = Mutex::new(Vec::new());

fn check_key(raster_flags: u32) -> Result<(), String> {
    if raster_flags < KEYS {
        Ok(())
    } else {
        Err(format!("raster key {raster_flags:#x}: only DHDR_RASTER_* bits (0-7)"))
    }
}

fn mine(e: &Entry, gpu: &Gpu) -> bool {
    e.instance == gpu.instance && e.device == gpu.device
}

fn with_entry<R>(gpu: &Gpu, f: impl FnOnce(&Entry) -> R) -> Option<R> {
    let cache = CACHE.lock().unwrap_or_else(|p| p.into_inner());
    cache.iter().find(|e| mine(e, gpu)).map(f)
}

fn cached(gpu: &Gpu, key: u32) -> Option<Pipelines> {
    with_entry(gpu, |e| e.keys[key as usize].as_ref().map(|k| assemble(key, &e.shared, k))).flatten()
}

/// Keeps a successful build. The first stored `Shared` of a device stays (a concurrent build of
/// the same device's shared objects is dropped; both were valid).
fn store(gpu: &Gpu, shared: &Shared, key: u32, keyed: &Keyed) {
    let mut cache = CACHE.lock().unwrap_or_else(|p| p.into_inner());
    let at = match cache.iter().position(|e| mine(e, gpu)) {
        Some(at) => at,
        None => {
            cache.push(Entry {
                instance: gpu.instance.clone(),
                device: gpu.device.clone(),
                shared: shared.clone(),
                keys: Default::default(),
            });
            cache.len() - 1
        }
    };
    let entry = &mut cache[at];
    if entry.keys[key as usize].is_none() {
        // A pair is kept only over the stored Shared (main's pipeline layout and the bind groups
        // made from main_layout must match); one built over a racing Shared is not kept.
        if entry.shared.main_layout == shared.main_layout {
            entry.keys[key as usize] = Some(keyed.clone());
        }
    }
}

fn assemble(key: u32, s: &Shared, k: &Keyed) -> Pipelines {
    Pipelines {
        raster_flags: key,
        main: k.main.clone(),
        main_layout: s.main_layout.clone(),
        local_copy: k.local_copy.clone(),
        local_copy_layout: k.local_copy_layout.clone(),
        scanout: s.scanout.clone(),
        scanout_layout: s.scanout_layout.clone(),
    }
}

// ---------------------------------------------------------------- building

/// The device's shared objects (cached or new) and the key's pair, inside the caller's scopes.
fn build(gpu: &Gpu, key: u32) -> (Shared, Keyed) {
    let shared = with_entry(gpu, |e| e.shared.clone()).unwrap_or_else(|| build_shared(gpu));
    let keyed = build_keyed(gpu, &shared, key);
    (shared, keyed)
}

fn module(gpu: &Gpu) -> wgpu::ShaderModule {
    gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("raster.wgsl"),
        source: wgpu::ShaderSource::Wgsl(crate::shaders::RASTER.into()),
    })
}

fn buffer_entry(binding: u32, ty: wgpu::BufferBindingType, dynamic: bool, min: u64) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::COMPUTE,
        ty: wgpu::BindingType::Buffer { ty, has_dynamic_offset: dynamic, min_binding_size: NonZeroU64::new(min) },
        count: None,
    }
}

fn compute(gpu: &Gpu, module: &wgpu::ShaderModule, layout: Option<&wgpu::PipelineLayout>, entry: &str,
           constants: &[(&str, f64)]) -> wgpu::ComputePipeline {
    gpu.device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some(entry),
        layout,
        module,
        entry_point: Some(entry),
        compilation_options: wgpu::PipelineCompilationOptions { constants, zero_initialize_workgroup_memory: false },
        cache: None,
    })
}

/// gpu_wgpu.c:237-261 (main's layout: as the automatic one, but the batch record takes a dynamic
/// offset, so every dispatch of a submit shares one bind group) and the scanout pipeline.
fn build_shared(gpu: &Gpu) -> Shared {
    use wgpu::BufferBindingType as T;
    let module = module(gpu);
    let ro = T::Storage { read_only: true };
    let main_layout = gpu.device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("main"),
        entries: &[
            buffer_entry(MAIN_BINDINGS[0], T::Uniform, true, 16),
            buffer_entry(MAIN_BINDINGS[1], ro, false, 0),
            buffer_entry(MAIN_BINDINGS[2], T::Storage { read_only: false }, false, 0),
            buffer_entry(MAIN_BINDINGS[3], ro, false, 0),
            buffer_entry(MAIN_BINDINGS[4], ro, false, 0),
            buffer_entry(MAIN_BINDINGS[5], ro, false, 0),
        ],
    });
    let main_pipeline_layout = gpu.device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("main"),
        bind_group_layouts: &[Some(&main_layout)],
        immediate_size: 0,
    });
    let scanout = compute(gpu, &module, None, "scanout", &[]);
    let scanout_layout = scanout.get_bind_group_layout(0);
    Shared { module, main_layout, main_pipeline_layout, scanout, scanout_layout }
}

/// gpu_wgpu.c:264-283: the DHD_GS_* shader options as raster.wgsl's override constants.
fn build_keyed(gpu: &Gpu, s: &Shared, key: u32) -> Keyed {
    use crate::abi::{DHDR_RASTER_ADDRESS_SHIFT, DHDR_RASTER_EARLY_DEPTH, DHDR_RASTER_EXCLUSIVE_WORD_STORE};
    let bit = |b: u32| if key & b != 0 { 1.0 } else { 0.0 };
    let c = [
        ("EARLY_DEPTH", bit(DHDR_RASTER_EARLY_DEPTH)),
        ("ADDRESS_SHIFT", bit(DHDR_RASTER_ADDRESS_SHIFT)),
        ("EXCLUSIVE_WORD_STORE", bit(DHDR_RASTER_EXCLUSIVE_WORD_STORE)),
    ];
    let main = compute(gpu, &s.module, Some(&s.main_pipeline_layout), "main", &c);
    let local_copy = compute(gpu, &s.module, None, "local_copy", &c[1..2]);
    let local_copy_layout = local_copy.get_bind_group_layout(0);
    Keyed { main, local_copy, local_copy_layout }
}
