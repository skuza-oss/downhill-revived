//! The native renderer's draw pipelines, by key (task N1).
//!
//! A key (the retired `gpu_wgpu_native.inc` native_pipeline; `port/gs/native_keys.inc`; both last
//! at render/rust 3882d5576):
//! `blend_key` (14 bits) | colour write mask << 14 | depth << 18 | variant << 23, where
//! - blend_key = enable | src << 1 | dst << 6 | op << 11, the factor and operation numbers in
//!   D3D11's numbering;
//! - depth (5 bits) = attachment 16 | write 8 | function 0-4 (never, always, gequal, greater,
//!   equal; 5-7 = always);
//! - variant 0 plain, 1 dual (dual-source module and factors), 2 RGB with A = 128
//!   (`native_pixel_blend`), 3 alpha only (no blend state).
//! Bits 25..28 are private: a power-of-two source scale (S=E), or zero for generic. Bit 29
//! (`TEXELS`): the draw reads a decoded texture's R32Uint texels (the `_u` entry points and the
//! texels layout).
//! The GS key's blend, depth and variant fields remain unchanged. The cache's key is 64 bits: this
//! key, and above it (`SPEC_SHIFT`) the fragment stage's specialisation, `spec` (native.wgsl's
//! `native_spec`; zero for the generic shader).
//!
//! The D3D11 numbers map to wgpu through EXPLICIT tables, never casts: the C path casts the
//! operation number into a `WGPUBlendOperation`, which wgpu-native then maps (0, Undefined, to Add;
//! 1-5 to Add, Subtract, ReverseSubtract, Min, Max; 6 and 7 are refused). The colour blend is
//! (src, dst, op); the alpha blend is (One, Zero, Add). Depth: Depth32Float, the stencil state
//! ignored (Always / Keep, masks 0), no bias, `Some(write)`, `Some(compare)`. Targets are
//! RGBA8Unorm. `tests/data/native_keys.json` holds the C code's tables for every key; the cache has
//! no 512-entry cap.
//!
//! Referee: `rustgs.py test ... -- --test native_keys`, and `--test device_smoke native`.
use crate::device::Gpu;
use crate::native::init::{scoped, Objects};
use std::collections::HashMap;

pub const VARIANT_PLAIN: u32 = 0;
pub const VARIANT_DUAL: u32 = 1;
pub const VARIANT_RGB_A128: u32 = 2;
pub const VARIANT_ALPHA_ONLY: u32 = 3;
pub const TEXEL_SCALE_SHIFT: u32 = 25;
/// The draw reads a decoded texture's texels (native.wgsl's `_u` entry points).
pub const TEXELS: u32 = 1 << 29;
/// Where a cache key holds the draw's fragment specialisation (`spec`).
pub const SPEC_SHIFT: u32 = 32;

/// How draws get their specialised pipelines (DHD_NATIVE_SPEC, finding 131).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SpecMode {
    /// `0`: every draw runs the generic shader, as before finding 131 (measurements, driver trouble).
    Off,
    /// `sync`: a specialised pipeline is built at its first draw, as the generic ones are; replays
    /// and picture checks use it, so every draw runs the specialised shader.
    Sync,
    /// Unset (players): built on a worker thread; until it is ready the draw runs the generic
    /// pipeline, which draws the same picture, so a new draw state costs no frame time.
    Background,
}

pub fn spec_mode() -> SpecMode {
    #[cfg(not(target_arch = "wasm32"))]
    {
        static MODE: std::sync::OnceLock<SpecMode> = std::sync::OnceLock::new();
        *MODE.get_or_init(|| match std::env::var("DHD_NATIVE_SPEC").as_deref() {
            Ok("0") => SpecMode::Off,
            Ok("sync") => SpecMode::Sync,
            _ => SpecMode::Background,
        })
    }
    // The browser has no worker for it; its createRenderPipeline compiles in the GPU process.
    #[cfg(target_arch = "wasm32")]
    SpecMode::Sync
}

pub fn spec_enabled() -> bool {
    spec_mode() != SpecMode::Off
}

/// The fragment specialisation of a draw (native.wgsl's `native_spec`): the shader path bits of
/// `flags` (0-11), the CLAMP modes (12-15) and the alpha test (16-18), each with its given bit
/// (31, 30, 29), `sized` (bit 19: the texture is exactly tex_size) and `unit_alpha` (bit 20: the
/// draw's alpha_scale is 255). Bits a draw's path never reads are left zero, so draws that differ
/// only there share a pipeline: an untextured draw has no texture state, a draw without the alpha
/// test no test.
pub fn spec(flags: u32, wrap: [u32; 2], atst: u32, sized: bool, unit_alpha: bool) -> u32 {
    let textured = flags & 1 != 0;
    let mut f = flags & 0xFFF;
    if !textured {
        f &= 16 | 64 | 128 | 512; // fog, alpha test and its failing pass, premultiply
    }
    // Each part with its "given" bit (31 flags, 30 CLAMP modes, 29 alpha test).
    let mut bits = 1 << 31 | 1 << 30 | 1 << 29 | f;
    if textured {
        bits |= (wrap[0] & 3) << 12 | (wrap[1] & 3) << 14 | (sized as u32) << 19 | (unit_alpha as u32) << 20;
    }
    if f & 64 != 0 {
        bits |= (atst & 7) << 16;
    }
    bits
}

/// What a key's pipeline is built with (the parts the key decides).
#[derive(Clone, Debug, PartialEq)]
pub struct PipelineDesc {
    /// Format RGBA8Unorm, the key's blend state (None when disabled or alpha-only) and write mask.
    pub color: wgpu::ColorTargetState,
    /// None without the depth attachment bit.
    pub depth: Option<wgpu::DepthStencilState>,
    /// The fragment stage is the dual-source module's.
    pub dual_module: bool,
    /// The fragment entry point: `native_pixel`, `native_pixel_dual` or `native_pixel_blend`.
    pub entry: &'static str,
}

/// A D3D11 blend factor number (0-31) as wgpu's, for the plain or the dual-source variant
/// (native_factor: the numbers the GS blend uses, every other one One).
pub fn blend_factor(d3d: u32, dual: bool) -> wgpu::BlendFactor {
    use wgpu::BlendFactor as F;
    match d3d {
        1 => F::Zero,                                                      // D3D11_BLEND_ZERO
        2 => F::One,                                                       // D3D11_BLEND_ONE
        5 => if dual { F::Src1Alpha } else { F::SrcAlpha },                // D3D11_BLEND_SRC_ALPHA
        6 => if dual { F::OneMinusSrc1Alpha } else { F::OneMinusSrcAlpha }, // D3D11_BLEND_INV_SRC_ALPHA
        7 => F::DstAlpha,                                                  // D3D11_BLEND_DEST_ALPHA
        8 => F::OneMinusDstAlpha,                                          // D3D11_BLEND_INV_DEST_ALPHA
        14 => F::Constant,                                                 // D3D11_BLEND_BLEND_FACTOR
        15 => F::OneMinusConstant,                                         // D3D11_BLEND_INV_BLEND_FACTOR
        _ => F::One,
    }
}

/// A D3D11 blend operation number (0-7) as wgpu's; None for the numbers wgpu-native refuses.
/// The C code casts the number to `WGPUBlendOperation` (0 Undefined, which wgpu-native takes as
/// Add; 1 Add, 2 Subtract, 3 ReverseSubtract, 4 Min, 5 Max); here the same map is written out.
pub fn blend_operation(d3d: u32) -> Option<wgpu::BlendOperation> {
    use wgpu::BlendOperation as O;
    match d3d {
        0 => Some(O::Add),             // WGPUBlendOperation_Undefined: wgpu-native's default, Add
        1 => Some(O::Add),             // D3D11_BLEND_OP_ADD
        2 => Some(O::Subtract),        // D3D11_BLEND_OP_SUBTRACT
        3 => Some(O::ReverseSubtract), // D3D11_BLEND_OP_REV_SUBTRACT
        4 => Some(O::Min),             // D3D11_BLEND_OP_MIN
        5 => Some(O::Max),             // D3D11_BLEND_OP_MAX
        _ => None,
    }
}

/// The depth function of a key's depth field (0-4: never, always, gequal, greater, equal; 5-7
/// always).
fn depth_compare(function: u32) -> wgpu::CompareFunction {
    use wgpu::CompareFunction as C;
    match function {
        0 => C::Never,
        2 => C::GreaterEqual,
        3 => C::Greater,
        4 => C::Equal,
        _ => C::Always,
    }
}

/// A colour write mask (red 1, green 2, blue 4, alpha 8, as WGPUColorWriteMask) as wgpu's.
fn write_mask(mask: u32) -> wgpu::ColorWrites {
    let mut w = wgpu::ColorWrites::empty();
    if mask & 1 != 0 {
        w |= wgpu::ColorWrites::RED;
    }
    if mask & 2 != 0 {
        w |= wgpu::ColorWrites::GREEN;
    }
    if mask & 4 != 0 {
        w |= wgpu::ColorWrites::BLUE;
    }
    if mask & 8 != 0 {
        w |= wgpu::ColorWrites::ALPHA;
    }
    w
}

/// Depth32Float with the stencil ignored (Always / Keep, masks 0) and no bias, as the C path's
/// zeroed `WGPUDepthStencilState` with Always / Keep stencil faces. Also the fullscreen depth
/// pipeline's (`init.rs`).
pub(crate) fn depth_state(write: bool, compare: wgpu::CompareFunction) -> wgpu::DepthStencilState {
    wgpu::DepthStencilState {
        format: wgpu::TextureFormat::Depth32Float,
        depth_write_enabled: Some(write),
        depth_compare: Some(compare),
        stencil: wgpu::StencilState {
            front: wgpu::StencilFaceState::IGNORE,
            back: wgpu::StencilFaceState::IGNORE,
            read_mask: 0,
            write_mask: 0,
        },
        bias: wgpu::DepthBiasState::default(),
    }
}

/// The pipeline a key asks for; Err for a key no pipeline exists for (an operation of 6 or 7 on
/// a blending key).
pub fn describe(key: u32) -> Result<PipelineDesc, String> {
    let blend_key = key & 0x3FFF;
    let mask = key >> 14 & 15;
    let depth = key >> 18 & 31;
    let variant = key >> 23 & 3;
    let dual = variant == VARIANT_DUAL;
    let blend = if blend_key & 1 != 0 && variant != VARIANT_ALPHA_ONLY {
        let op = blend_key >> 11 & 7;
        let operation = blend_operation(op)
            .ok_or_else(|| format!("native pipeline {key:07X}: blend operation {op} is not one wgpu takes"))?;
        Some(wgpu::BlendState {
            color: wgpu::BlendComponent {
                src_factor: blend_factor(blend_key >> 1 & 31, dual),
                dst_factor: blend_factor(blend_key >> 6 & 31, dual),
                operation,
            },
            alpha: wgpu::BlendComponent {
                src_factor: wgpu::BlendFactor::One,
                dst_factor: wgpu::BlendFactor::Zero,
                operation: wgpu::BlendOperation::Add,
            },
        })
    } else {
        None
    };
    let entry = match variant {
        VARIANT_DUAL => "native_pixel_dual",
        VARIANT_RGB_A128 => "native_pixel_blend",
        _ => "native_pixel",
    };
    Ok(PipelineDesc {
        color: wgpu::ColorTargetState { format: wgpu::TextureFormat::Rgba8Unorm, blend, write_mask: write_mask(mask) },
        depth: (depth & 16 != 0).then(|| depth_state(depth & 8 != 0, depth_compare(depth & 7))),
        dual_module: dual,
        entry,
    })
}

/// The vertex layout of every draw (`DhdrVertex`, 32 bytes): position xyz, stqf, rgba bytes.
const VERTEX_ATTRIBUTES: [wgpu::VertexAttribute; 3] = [
    wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 0, shader_location: 0 },
    wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x4, offset: 12, shader_location: 1 },
    wgpu::VertexAttribute { format: wgpu::VertexFormat::Uint8x4, offset: 28, shader_location: 2 },
];
const _: () = assert!(std::mem::size_of::<crate::abi::DhdrVertex>() == 32);

/// What a draw pipeline is built from (cloned handles, so a worker thread can build one).
#[derive(Clone)]
struct Sources {
    module: wgpu::ShaderModule,
    dual_module: Option<wgpu::ShaderModule>,
    layout: wgpu::PipelineLayout,
    texels_layout: wgpu::PipelineLayout,
}

impl Sources {
    fn of(objects: &Objects) -> Sources {
        Sources { module: objects.module.clone(), dual_module: objects.dual_module.clone(),
                  layout: objects.draw_layout.clone(), texels_layout: objects.texels_draw_layout.clone() }
    }
}

/// Builds the pipeline of `key` (no cache, no scope).
fn build(gpu: &Gpu, objects: &Sources, full: u64, d: &PipelineDesc) -> Result<wgpu::RenderPipeline, String> {
    let key = full as u32;
    let spec = (full >> SPEC_SHIFT) as u32;
    let texels = key & TEXELS != 0;
    let entry = match (d.entry, texels) {
        ("native_pixel", true) => "native_pixel_u",
        ("native_pixel_blend", true) => "native_pixel_blend_u",
        ("native_pixel_dual", true) => "native_pixel_dual_u",
        (e, _) => e,
    };
    let fragment_module = if d.dual_module {
        objects
            .dual_module
            .as_ref()
            .ok_or_else(|| format!("native pipeline {key:07X}: dual-source blending is not in use"))?
    } else {
        &objects.module
    };
    let vertex = wgpu::VertexBufferLayout {
        array_stride: std::mem::size_of::<crate::abi::DhdrVertex>() as u64,
        step_mode: wgpu::VertexStepMode::Vertex,
        attributes: &VERTEX_ATTRIBUTES,
    };
    let targets = [Some(d.color.clone())];
    let texel_scale = key >> TEXEL_SCALE_SHIFT & 15;
    let mut constants = Vec::with_capacity(2);
    if texel_scale != 0 {
        constants.push(("native_texel_scale", texel_scale as f64));
    }
    if spec != 0 {
        constants.push(("native_spec", spec as f64));
    }
    let fragment_options = wgpu::PipelineCompilationOptions {
        constants: &constants,
        zero_initialize_workgroup_memory: crate::native::init::compilation_options().zero_initialize_workgroup_memory,
    };
    Ok(gpu.device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("native draw"),
        layout: Some(if texels { &objects.texels_layout } else { &objects.layout }),
        vertex: wgpu::VertexState {
            module: &objects.module,
            entry_point: Some("native_vertex"),
            compilation_options: crate::native::init::compilation_options(),
            buffers: &[vertex],
        },
        primitive: wgpu::PrimitiveState {
            topology: wgpu::PrimitiveTopology::TriangleList,
            cull_mode: None,
            ..Default::default()
        },
        depth_stencil: d.depth.clone(),
        multisample: wgpu::MultisampleState { count: 1, mask: !0, alpha_to_coverage_enabled: false },
        fragment: Some(wgpu::FragmentState {
            module: fragment_module,
            entry_point: Some(entry),
            compilation_options: fragment_options,
            targets: &targets,
        }),
        multiview_mask: None,
        cache: None,
    }))
}

/// Built pipelines by key (failures are kept too, so a bad key is reported once, not per draw).
pub struct Pipelines {
    built: HashMap<u64, Result<wgpu::RenderPipeline, String>>,
    /// SpecMode::Background's worker (made at the first specialised draw).
    #[cfg(not(target_arch = "wasm32"))]
    worker: Option<Worker>,
}

/// The thread that builds specialised pipelines, and the keys it has not answered yet.
#[cfg(not(target_arch = "wasm32"))]
struct Worker {
    jobs: std::sync::mpsc::Sender<(u64, PipelineDesc)>,
    done: std::sync::mpsc::Receiver<(u64, Result<wgpu::RenderPipeline, String>)>,
    waiting: std::collections::HashSet<u64>,
}

#[cfg(not(target_arch = "wasm32"))]
impl Worker {
    fn start(gpu: &'static Gpu, sources: Sources) -> Worker {
        let (jobs, inbox) = std::sync::mpsc::channel::<(u64, PipelineDesc)>();
        let (outbox, done) = std::sync::mpsc::channel();
        let spawned = std::thread::Builder::new().name("dhd-pipelines".into()).spawn(move || {
            // Ends when the Pipelines (the sender) is dropped.
            while let Ok((key, d)) = inbox.recv() {
                let start = std::time::Instant::now();
                let pipe = scoped(gpu, &format!("native pipeline {key:X}"), || build(gpu, &sources, key, &d))
                    .and_then(|r| r);
                let took = start.elapsed().as_secs_f64();
                if took > 0.25 {
                    crate::host::log(&format!("gs native: specialised pipeline {key:X} built in {took:.2} s (worker)"));
                }
                if outbox.send((key, pipe)).is_err() {
                    break;
                }
            }
        });
        if let Err(e) = spawned {
            crate::host::log(&format!("gs native: no pipeline worker ({e}); specialised pipelines are built at first use"));
        }
        Worker { jobs, done, waiting: std::collections::HashSet::new() }
    }
}

impl Pipelines {
    pub fn new() -> Pipelines {
        Pipelines {
            built: HashMap::new(),
            #[cfg(not(target_arch = "wasm32"))]
            worker: None,
        }
    }

    /// The pipeline for `key` with its specialisation (bits above `SPEC_SHIFT`), and the key of the
    /// pipeline given: the specialised one once it is built, else the generic one (the low 32
    /// bits; SpecMode::Background), which draws the same picture. A specialised pipeline that
    /// fails is reported once and the generic one draws instead.
    pub fn get_special(&mut self, gpu: &'static Gpu, objects: &Objects, key: u64)
                       -> Result<(u64, &wgpu::RenderPipeline), String> {
        let generic = key & 0xFFFF_FFFF;
        if key != generic {
            #[cfg(not(target_arch = "wasm32"))]
            {
                if spec_mode() == SpecMode::Background {
                    self.receive();
                    let (mut here, mut failed) = (false, None);
                    if !self.built.contains_key(&key) {
                        let worker = self.worker.get_or_insert_with(|| Worker::start(gpu, Sources::of(objects)));
                        if worker.waiting.insert(key) {
                            match describe(key as u32) {
                                Ok(d) => {
                                    // No worker thread: build it here, as SpecMode::Sync does.
                                    if worker.jobs.send((key, d)).is_err() {
                                        worker.waiting.remove(&key);
                                        here = true;
                                    }
                                }
                                Err(e) => {
                                    worker.waiting.remove(&key);
                                    failed = Some(e);
                                }
                            }
                        }
                    }
                    if let Some(e) = failed {
                        self.built.insert(key, Err(e));
                    }
                    if here {
                        self.get(gpu, objects, key).ok();
                    }
                } else {
                    self.get(gpu, objects, key).ok();
                }
            }
            #[cfg(target_arch = "wasm32")]
            {
                self.get(gpu, objects, key).ok();
            }
            if self.built.get(&key).is_some_and(|r| r.is_ok()) {
                return self.get(gpu, objects, key).map(|p| (key, p));
            }
        }
        self.get(gpu, objects, generic).map(|p| (generic, p))
    }

    /// Takes the worker's finished pipelines into the cache (failures logged once).
    #[cfg(not(target_arch = "wasm32"))]
    fn receive(&mut self) {
        let Some(worker) = self.worker.as_mut() else { return };
        while let Ok((key, pipe)) = worker.done.try_recv() {
            worker.waiting.remove(&key);
            if let Err(e) = &pipe {
                crate::host::log(&format!("gs native: specialised pipeline {key:X}: {e}; the generic one draws"));
            }
            self.built.insert(key, pipe);
        }
    }

    /// Pipelines built (or failed) so far.
    pub fn len(&self) -> usize {
        self.built.len()
    }

    pub fn is_empty(&self) -> bool {
        self.built.is_empty()
    }

    /// The pipeline for `key` (a cache key: `SPEC_SHIFT`), built on first use (Err: the key has
    /// none, or it failed).
    pub fn get(&mut self, gpu: &Gpu, objects: &Objects, key: u64) -> Result<&wgpu::RenderPipeline, String> {
        let built = self.built.entry(key).or_insert_with(|| {
            #[cfg(not(target_arch = "wasm32"))]
            let _spike = SpikeTimer::new("pipeline", key as u32);
            let d = describe(key as u32)?;
            #[cfg(not(target_arch = "wasm32"))]
            let start = std::time::Instant::now();
            let pipe = scoped(gpu, &format!("native pipeline {key:X}"), || build(gpu, &Sources::of(objects), key, &d))??;
            #[cfg(not(target_arch = "wasm32"))]
            {
                let took = start.elapsed().as_secs_f64();
                if took > 0.25 {
                    crate::host::log(&format!("gs native: draw pipeline {key:X} built in {took:.2} s"));
                }
            }
            Ok(pipe)
        });
        built.as_ref().map_err(|e| e.clone())
    }
}

/// DHD_NATIVE_PIPELOG (diagnostics, AND3): the keys each picture binds, one log line per submit.
#[cfg(not(target_arch = "wasm32"))]
pub(crate) mod pipelog {
    use std::sync::{Mutex, OnceLock};
    static KEYS: Mutex<Vec<u32>> = Mutex::new(Vec::new());
    static PICTURE: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    fn on() -> bool {
        static ON: OnceLock<bool> = OnceLock::new();
        *ON.get_or_init(|| std::env::var_os("DHD_NATIVE_PIPELOG").is_some())
    }
    pub(crate) fn bind(key: u32) {
        if on() {
            let mut k = KEYS.lock().unwrap_or_else(|p| p.into_inner());
            if !k.contains(&key) {
                k.push(key);
            }
        }
    }
    pub(crate) fn submit() {
        if on() {
            let mut k = KEYS.lock().unwrap_or_else(|p| p.into_inner());
            let n = PICTURE.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let list: Vec<String> = k.iter().map(|x| format!("{x:07X}")).collect();
            crate::host::log(&format!("pipelog {n}: {}", list.join(" ")));
            k.clear();
        }
    }
}

/// Opt-in spike attribution; no clocks or formatting when DHD_SPIKES is absent.
#[cfg(not(target_arch = "wasm32"))]
pub(crate) struct SpikeTimer {
    start: Option<std::time::Instant>,
    tag: &'static str,
    key: u32,
}
#[cfg(not(target_arch = "wasm32"))]
impl SpikeTimer {
    pub(crate) fn new(tag: &'static str, key: u32) -> Self {
        static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        Self { start: ENABLED.get_or_init(|| std::env::var_os("DHD_SPIKES").is_some())
                   .then(std::time::Instant::now), tag, key }
    }
}
#[cfg(not(target_arch = "wasm32"))]
impl Drop for SpikeTimer {
    fn drop(&mut self) {
        if let Some(start) = self.start {
            let ms = start.elapsed().as_secs_f64() * 1000.0;
            if ms >= 1.0 || self.tag == "pipeline" {
                let end = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)
                    .unwrap_or_default().as_secs_f64();
                crate::host::log(&format!("spike_gpu: end_unix={end:.6} ms={ms:.6} tag={} key={:X}", self.tag, self.key));
            }
        }
    }
}

/// DHD_GPU_TIMES=N (diagnostics): the native renderer's GPU time by kind of pass, from timestamp
/// queries at each pass's start and end (wgpu's TIMESTAMP_QUERY, which device.rs asks for only
/// when this is set; Metal on Apple GPUs samples only at pass boundaries, so the passes are the
/// unit). Every N presented frames (default 600) one log line: the mean GPU milliseconds a frame
/// by kind (draw passes; surface passes: transfers, self-copies and growth; conversions; scanout;
/// present) with the passes a frame of each, and the frame's span (its first pass's start to its
/// last pass's end). Without the variable, or on an adapter without timestamps, every pass gets
/// None: no queries at all.
///
/// The light part runs for every player (finding 129, DHD_PERF_LOG=0 turns it off): one frame in
/// `SAMPLE` is timed the same way, with no log line; its totals (frames timed, the passes' GPU
/// nanoseconds, the spans) are what `dhdr_perf_read` gives port/gs/perf_log.c for its perf: lines.
/// A picture never presented (a minimised window) has its queries dropped at the next picture.
pub(crate) mod gputimes {
    #[cfg(target_arch = "wasm32")]
    use crate::device::Gpu;

    pub const DRAW: u8 = 0;
    pub const SURFACE: u8 = 1;
    pub const CONVERT: u8 = 2;
    pub const SCANOUT: u8 = 3;
    pub const PRESENT: u8 = 4;

    /// A pass's two queries (start, end) in the frame's query set.
    pub struct Slot(wgpu::QuerySet, u32);

    impl Slot {
        pub fn render(&self) -> wgpu::RenderPassTimestampWrites<'_> {
            wgpu::RenderPassTimestampWrites { query_set: &self.0, beginning_of_pass_write_index: Some(self.1),
                                              end_of_pass_write_index: Some(self.1 + 1) }
        }
        pub fn compute(&self) -> wgpu::ComputePassTimestampWrites<'_> {
            wgpu::ComputePassTimestampWrites { query_set: &self.0, beginning_of_pass_write_index: Some(self.1),
                                               end_of_pass_write_index: Some(self.1 + 1) }
        }
    }

    /// Whether DHD_GPU_TIMES is set (device.rs then asks for timestamp queries).
    #[cfg(not(target_arch = "wasm32"))]
    pub fn wanted() -> bool {
        std::env::var_os("DHD_GPU_TIMES").is_some()
    }

    /// Whether the perf: lines are on (DHD_PERF_LOG unset or not 0; device.rs then asks for
    /// timestamp queries too, where the adapter has them).
    #[cfg(not(target_arch = "wasm32"))]
    pub fn perf_on() -> bool {
        static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        *ON.get_or_init(|| std::env::var("DHD_PERF_LOG").map_or(true, |v| v != "0"))
    }

    /// The timer's state for `dhdr_perf_read`: 0 off (not asked for, or no native picture yet), 1
    /// on, 2 the adapter has no timestamp queries.
    pub static STATE: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(0);

    /// Frames timed, their passes' GPU nanoseconds and their spans' nanoseconds, so far.
    pub fn totals() -> (u64, u64, u64) {
        #[cfg(not(target_arch = "wasm32"))]
        return timed::totals();
        #[cfg(target_arch = "wasm32")]
        (0, 0, 0)
    }

    #[cfg(target_arch = "wasm32")]
    pub fn slot(_gpu: &Gpu, _kind: u8) -> Option<Slot> {
        None
    }
    #[cfg(target_arch = "wasm32")]
    pub fn end_frame(_gpu: &Gpu, _encoder: &mut wgpu::CommandEncoder) {}
    #[cfg(target_arch = "wasm32")]
    pub fn submitted(_gpu: &Gpu) {}
    #[cfg(target_arch = "wasm32")]
    pub fn picture_begin(_gpu: &Gpu) {}

    #[cfg(not(target_arch = "wasm32"))]
    pub use timed::*;

    #[cfg(not(target_arch = "wasm32"))]
    mod timed {
        use super::Slot;
        use crate::device::Gpu;
        use crate::exact::readback::{map_start, MapStatus};
        use std::sync::{Mutex, OnceLock};

        const NAMES: [&str; 5] = ["draw", "surface", "convert", "scanout", "present"];
        /// Queries in one frame's set (two a pass); passes beyond go untimed.
        const QUERIES: u32 = 1024;
        /// Frames in flight; when the next set is still being read, a frame goes untimed.
        const SETS: usize = 3;
        /// Without DHD_GPU_TIMES (the perf: lines' light part): one frame timed in this many.
        const SAMPLE: u64 = 8;

        struct Set {
            queries: wgpu::QuerySet,
            resolve: wgpu::Buffer,
            read: wgpu::Buffer,
            /// Each timed pass's kind, in query order.
            kinds: Vec<u8>,
            /// Queries resolved and copied for reading (at the frame's end).
            resolved: u32,
            /// The read buffer's map, once the frame is submitted.
            map: Option<MapStatus>,
        }

        struct Times {
            sets: Vec<Set>,
            current: usize,
            every: u32,
            period: f64,
            frames: u32,
            ns: [f64; 5],
            passes: [u64; 5],
            span: f64,
            untimed: u64,
            /// DHD_GPU_TIMES: every frame timed and the log line; else one in SAMPLE, no line.
            every_frame: bool,
            /// Whether this frame is timed (decided after the last present).
            sampling: bool,
            frame: u64,
            /// A present since the last picture began (else that picture was never shown).
            presented: bool,
            total_frames: u64,
            total_ns: f64,
            total_span: f64,
        }

        static TIMES: OnceLock<Option<Mutex<Times>>> = OnceLock::new();

        fn times(gpu: &Gpu) -> Option<&'static Mutex<Times>> {
            TIMES.get_or_init(|| {
                let every_frame = super::wanted();
                if !every_frame && !super::perf_on() {
                    return None;
                }
                if !gpu.device.features().contains(wgpu::Features::TIMESTAMP_QUERY) {
                    if every_frame {
                        crate::host::log("gpu_times: this adapter has no timestamp queries (DHD_GPU_TIMES ignored)");
                    }
                    super::STATE.store(2, std::sync::atomic::Ordering::Relaxed);
                    return None;
                }
                super::STATE.store(1, std::sync::atomic::Ordering::Relaxed);
                let every = std::env::var("DHD_GPU_TIMES").ok().and_then(|v| v.parse().ok()).filter(|&n| n > 0)
                    .unwrap_or(600);
                let buffer = |label, usage| gpu.device.create_buffer(&wgpu::BufferDescriptor {
                    label: Some(label), size: QUERIES as u64 * 8, usage, mapped_at_creation: false });
                let sets = (0..SETS).map(|_| Set {
                    queries: gpu.device.create_query_set(&wgpu::QuerySetDescriptor {
                        label: Some("gpu times"), ty: wgpu::QueryType::Timestamp, count: QUERIES }),
                    resolve: buffer("gpu times resolve", wgpu::BufferUsages::QUERY_RESOLVE | wgpu::BufferUsages::COPY_SRC),
                    read: buffer("gpu times read", wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST),
                    kinds: Vec::new(),
                    resolved: 0,
                    map: None,
                }).collect();
                if every_frame {
                    crate::host::log(&format!("gpu_times: GPU time by pass kind every {every} frames (DHD_GPU_TIMES)"));
                }
                Some(Mutex::new(Times { sets, current: 0, every, period: gpu.queue.get_timestamp_period() as f64,
                                        frames: 0, ns: [0.0; 5], passes: [0; 5], span: 0.0, untimed: 0, every_frame,
                                        sampling: true, frame: 0, presented: true, total_frames: 0, total_ns: 0.0,
                                        total_span: 0.0 }))
            }).as_ref()
        }

        fn lock(t: &Mutex<Times>) -> std::sync::MutexGuard<'_, Times> {
            t.lock().unwrap_or_else(|p| p.into_inner())
        }

        /// The current set, unless it is still being read (in flight from SETS frames ago).
        fn open(t: &mut Times) -> Option<&mut Set> {
            let current = t.current;
            let set = &mut t.sets[current];
            set.map.is_none().then_some(set)
        }

        /// The totals so far (`super::totals`); zeros before the first picture.
        pub fn totals() -> (u64, u64, u64) {
            match TIMES.get() {
                Some(Some(t)) => {
                    let t = lock(t);
                    (t.total_frames, t.total_ns as u64, t.total_span as u64)
                }
                _ => (0, 0, 0),
            }
        }

        /// At a picture's start (render.rs): when the last picture was never presented, its
        /// queries are dropped, and this frame goes untimed (its earlier passes went with them).
        pub fn picture_begin(gpu: &Gpu) {
            let Some(t) = times(gpu) else { return };
            let mut t = lock(t);
            if !t.presented {
                if let Some(set) = open(&mut t) {
                    if set.resolved == 0 {
                        set.kinds.clear();
                    }
                }
                if !t.every_frame {
                    t.sampling = false;
                }
            }
            t.presented = false;
        }

        /// The queries of a pass of `kind`, or None (timing off, the set full or still being read).
        pub fn slot(gpu: &Gpu, kind: u8) -> Option<Slot> {
            let mut t = lock(times(gpu)?);
            if !t.sampling {
                return None;
            }
            let slot = open(&mut t).filter(|s| s.kinds.len() as u32 * 2 + 2 <= QUERIES).map(|set| {
                let index = set.kinds.len() as u32 * 2;
                set.kinds.push(kind);
                Slot(set.queries.clone(), index)
            });
            if slot.is_none() {
                t.untimed += 1;
            }
            slot
        }

        /// In the present's encoder, after its pass: reads every finished frame, then resolves this
        /// frame's queries (written by this and the picture's earlier command buffers; a resolve
        /// offset must be 256-byte aligned, so all at once) and copies them for reading
        /// (`submitted` maps them after the submit).
        pub fn end_frame(gpu: &Gpu, encoder: &mut wgpu::CommandEncoder) {
            let Some(t) = times(gpu) else { return };
            let (sampling, reading) = {
                let t = lock(t);
                (t.sampling, t.sets.iter().any(|s| s.map.is_some()))
            };
            if sampling || reading {
                collect(gpu, t);
            }
            if !sampling {
                return;
            }
            let mut t = lock(t);
            let Some(set) = open(&mut t) else { return };
            let used = set.kinds.len() as u32 * 2;
            if used > 0 {
                encoder.resolve_query_set(&set.queries, 0..used, &set.resolve, 0);
                encoder.copy_buffer_to_buffer(&set.resolve, 0, &set.read, 0, used as u64 * 8);
            }
            set.resolved = used;
        }

        /// After the present's submit: maps the frame's copy and moves on to the next set.
        pub fn submitted(gpu: &Gpu) {
            let Some(t) = times(gpu) else { return };
            let mut t = lock(t);
            t.presented = true;
            t.frame += 1;
            let sampled = t.sampling;
            t.sampling = t.every_frame || t.frame % SAMPLE == 0;
            if !sampled {
                return;
            }
            let Some(set) = open(&mut t) else { return };
            if set.resolved == 0 {
                return;
            }
            set.map = Some(map_start(&set.read, 0..set.resolved as u64 * 8));
            t.current = (t.current + 1) % SETS;
        }

        /// Reads (and frees) every set whose map finished, adds it up, and logs every `every` frames.
        fn collect(gpu: &Gpu, t: &Mutex<Times>) {
            let _ = gpu.device.poll(wgpu::PollType::Poll);
            let mut t = lock(t);
            let period = t.period;
            for i in 0..SETS {
                let set = &mut t.sets[i];
                let Some(result) = set.map.as_ref().and_then(|m| m.lock().unwrap_or_else(|p| p.into_inner()).clone())
                else { continue };
                let (mut ns, mut passes) = ([0.0f64; 5], [0u64; 5]);
                let (mut first, mut last) = (u64::MAX, 0u64);
                if result.is_ok() {
                    {
                        let data = set.read.slice(..set.resolved as u64 * 8).get_mapped_range();
                        let stamp = |q: usize| u64::from_le_bytes(data[q * 8..q * 8 + 8].try_into().expect("8 bytes"));
                        for (p, &kind) in set.kinds.iter().enumerate() {
                            let (a, b) = (stamp(p * 2), stamp(p * 2 + 1));
                            if a == 0 || b < a {
                                continue;
                            }
                            ns[kind as usize] += (b - a) as f64 * period;
                            passes[kind as usize] += 1;
                            first = first.min(a);
                            last = last.max(b);
                        }
                    }
                    set.read.unmap();
                }
                set.kinds.clear();
                set.resolved = 0;
                set.map = None;
                for k in 0..5 {
                    t.ns[k] += ns[k];
                    t.passes[k] += passes[k];
                }
                if last > first {
                    t.span += (last - first) as f64 * period;
                    t.total_span += (last - first) as f64 * period;
                }
                t.frames += 1;
                if passes.iter().any(|&p| p > 0) {
                    t.total_frames += 1;
                    t.total_ns += ns.iter().sum::<f64>();
                }
            }
            if !t.every_frame {
                t.frames = 0;
                t.ns = [0.0; 5];
                t.passes = [0; 5];
                t.span = 0.0;
                t.untimed = 0;
            } else if t.frames >= t.every {
                let f = t.frames as f64;
                let total: f64 = t.ns.iter().sum();
                let parts: Vec<String> = (0..5)
                    .map(|k| format!("{}={:.3}/{:.1}", NAMES[k], t.ns[k] / f / 1e6, t.passes[k] as f64 / f))
                    .collect();
                crate::host::log(&format!("gpu_times: frames={} gpu_ms={:.3} span_ms={:.3} ms/passes {} untimed_passes={}",
                                          t.frames, total / f / 1e6, t.span / f / 1e6, parts.join(" "), t.untimed));
                t.frames = 0;
                t.ns = [0.0; 5];
                t.passes = [0; 5];
                t.span = 0.0;
                t.untimed = 0;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::spec;

    /// `spec` keeps only what the draw's path reads, so draws that differ elsewhere share a pipeline.
    #[test]
    fn spec_keeps_what_the_path_reads() {
        let given = 1 << 31 | 1 << 30 | 1 << 29;
        // Untextured: no texture bits, CLAMP modes or size; fog, the alpha test and premultiply stay.
        assert_eq!(spec(2 | 4 | 32 | 256 | 1024 | 2048, [3, 2], 5, true, true), given);
        assert_eq!(spec(16 | 64 | 128 | 512 | 32, [1, 1], 6, true, true), given | 16 | 64 | 128 | 512 | 6 << 16);
        // Textured: every flag bit, the CLAMP modes, the size and the unit alpha; the test only with
        // the alpha test on.
        assert_eq!(spec(1 | 32 | 16, [2, 3], 5, false, false), given | 1 | 32 | 16 | 2 << 12 | 3 << 14);
        assert_eq!(spec(1 | 64, [0, 1], 7, true, true), given | 1 | 64 | 1 << 14 | 7 << 16 | 1 << 19 | 1 << 20);
        // Upper flag bits are not the shader's.
        assert_eq!(spec(1 | 1 << 12, [0, 0], 0, false, false), given | 1);
    }
}
