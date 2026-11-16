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
//! texels layout). Bit 30 (`MESH`): a translated VU1 mesh draw (flag 4096, finding 164): the vertex
//! stage is `native_mesh_vertex`, which reads the vertex as a mesh item's (`MESH_ATTRIBUTES`), and
//! back faces are culled; bit 31 (`MESH_TWO_SIDED`) with it: no culling (finding 167).
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
use crate::native::init::{scoped_pipeline, Objects};
use std::collections::HashMap;

pub const VARIANT_PLAIN: u32 = 0;
pub const VARIANT_DUAL: u32 = 1;
pub const VARIANT_RGB_A128: u32 = 2;
pub const VARIANT_ALPHA_ONLY: u32 = 3;
pub const TEXEL_SCALE_SHIFT: u32 = 25;
/// The draw reads a decoded texture's texels (native.wgsl's `_u` entry points).
pub const TEXELS: u32 = 1 << 29;
/// A translated VU1 mesh draw (native.wgsl's `native_mesh_vertex`, back faces culled; finding 164).
pub const MESH: u32 = 1 << 30;
/// With MESH: both sides drawn (the loops without back-face culling; finding 167).
pub const MESH_TWO_SIDED: u32 = 1 << 31;
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

/// A draw's blend key and colour write mask without a blend that keeps the colour: Cs * 0 + Cd * 1
/// (or Cd * 1 - Cs * 0) leaves every colour channel as it was, so the draw is the same without
/// blending and with only its alpha channel written (the alpha blend is One / Zero, a plain write).
/// Exact: a UNORM target clamps Cs to [0, 1] before blending, so Cs * 0 is 0. The GS makes it
/// with A = B ((Cs - Cs) * As + Cd, native_blend), a draw that only writes Z or alpha. PowerVR
/// Rogue's driver (1.13), which blends in the fragment program, does not create that pipeline
/// (finding 176). A draw left with no channel and no Z write draws nothing (`render.rs` skips it).
pub fn without_kept_colour(blend_key: u32, mask: u32) -> (u32, u32) {
    let (src, dst, op) = (blend_key >> 1 & 31, blend_key >> 6 & 31, blend_key >> 11 & 7);
    // D3D11 ZERO (1) and ONE (2); operations 0 and 1 are Add, 3 ReverseSubtract (blend_operation).
    if blend_key & 1 != 0 && src == 1 && dst == 2 && matches!(op, 0 | 1 | 3) {
        (0, mask & 8)
    } else {
        (blend_key, mask)
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

/// What a failed pipeline was (its log line, finding 176): the fragment entry point and
/// specialisation, the blend state, the write mask and the depth state.
fn summary(full: u64, d: &PipelineDesc) -> String {
    let key = full as u32;
    let blend = match &d.color.blend {
        Some(b) => format!("{:?}/{:?} {:?}", b.color.src_factor, b.color.dst_factor, b.color.operation),
        None => "none".into(),
    };
    let depth = match &d.depth {
        Some(z) => format!("{:?}{}", z.depth_compare.unwrap_or(wgpu::CompareFunction::Always),
                           if z.depth_write_enabled == Some(true) { " write" } else { "" }),
        None => "none".into(),
    };
    format!("fs {}{} spec {:X} texel scale {}, blend {blend}, write mask {:X}, depth {depth}",
            d.entry, if key & TEXELS != 0 { " (texels)" } else { "" }, full >> SPEC_SHIFT,
            key >> TEXEL_SCALE_SHIFT & 15, d.color.write_mask.bits())
}

/// The vertex layout of every draw (`DhdrVertex`, 32 bytes): position xyz, stqf, rgba bytes.
const VERTEX_ATTRIBUTES: [wgpu::VertexAttribute; 3] = [
    wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 0, shader_location: 0 },
    wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x4, offset: 12, shader_location: 1 },
    wgpu::VertexAttribute { format: wgpu::VertexFormat::Uint8x4, offset: 28, shader_location: 2 },
];
const _: () = assert!(std::mem::size_of::<crate::abi::DhdrVertex>() == 32);
/// The same 32 bytes as a mesh item's vertex (gs_mesh.h's GsMeshVertex, finding 249): the
/// clip-space position, the texture coordinates with the Q factor, the colour's bytes.
const MESH_ATTRIBUTES: [wgpu::VertexAttribute; 3] = [
    wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x4, offset: 0, shader_location: 0 },
    wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x3, offset: 16, shader_location: 1 },
    wgpu::VertexAttribute { format: wgpu::VertexFormat::Uint8x4, offset: 28, shader_location: 2 },
];

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
                  layout: objects.native_draw_layout.clone(), texels_layout: objects.native_texels_draw_layout.clone() }
    }
}

/// Runtime pipeline breadcrumbs for #68's GPU, after the startup recorder retires.
/// A returned call can still have an error in the enclosing pipeline scope.
fn trace_build<T>(enabled: bool, key: u64, detail: impl FnOnce() -> String,
                  mut log: impl FnMut(&str), create: impl FnOnce() -> T) -> T {
    if !enabled { return create(); }
    static SERIAL: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let id = SERIAL.fetch_add(1, std::sync::atomic::Ordering::Relaxed) + 1;
    log(&format!("gs pipeline: begin {id} key={key:X}: {}", detail()));
    let result = create();
    log(&format!("gs pipeline: returned {id} key={key:X} (check pipeline errors for success)"));
    result
}

fn trace_gpu(backend: wgpu::Backend, name: &str) -> bool {
    backend == wgpu::Backend::Vulkan && matches!(name, "PowerVR Rogue GE8320" | "PowerVR GE8320")
}

/// Builds the pipeline of `key` (no cache, no scope).
fn build(gpu: &Gpu, objects: &Sources, full: u64, d: &PipelineDesc) -> Result<wgpu::RenderPipeline, String> {
    let key = full as u32;
    let spec = (full >> SPEC_SHIFT) as u32;
    let texels = key & TEXELS != 0;
    let mesh = key & MESH != 0;
    // A mesh draw's fragment stage is the GS draws' (native.wgsl's native_mesh_vertex gives Varyings).
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
        attributes: if mesh { &MESH_ATTRIBUTES } else { &VERTEX_ATTRIBUTES },
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
    let layout = if texels { &objects.texels_layout } else { &objects.layout };
    // a pipeline the driver died building in an earlier run is not built; this one is written
    // down while the driver builds it (list, finding 260)
    #[cfg(not(target_arch = "wasm32"))]
    let _building = {
        if list::refused(full) {
            return Err(format!("native pipeline {full:X}: not built, the driver died building it in an earlier run \
                                (pipelines.txt)"));
        }
        list::building(full)
    };
    Ok(trace_build(cfg!(target_os = "android") && trace_gpu(gpu.info.backend, &gpu.info.name), full,
        || format!("vs={} fs={entry}; {}", if mesh { "native_mesh_vertex" } else { "native_vertex" }, summary(full, d)),
        crate::host::log, || gpu.device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("native draw"),
        layout: Some(layout),
        vertex: wgpu::VertexState {
            module: &objects.module,
            entry_point: Some(if mesh { "native_mesh_vertex" } else { "native_vertex" }),
            compilation_options: crate::native::init::compilation_options(),
            buffers: &[vertex],
        },
        // A mesh's triangles come in the winding that leaves the VU's back faces with a positive
        // area in GS pixels (y down): clockwise in clip space (vu1_mesh.inc).
        primitive: wgpu::PrimitiveState {
            topology: wgpu::PrimitiveTopology::TriangleList,
            front_face: wgpu::FrontFace::Ccw,
            cull_mode: if mesh && key & MESH_TWO_SIDED == 0 { Some(wgpu::Face::Back) } else { None },
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
    })))
}

#[cfg(test)]
mod trace_tests {
    use super::*;
    #[test]
    fn only_the_reported_gpu_family() {
        assert!(trace_gpu(wgpu::Backend::Vulkan, "PowerVR Rogue GE8320"));
        assert!(trace_gpu(wgpu::Backend::Vulkan, "PowerVR GE8320"));
        assert!(!trace_gpu(wgpu::Backend::Gl, "PowerVR Rogue GE8320"));
        for name in ["PowerVR Rogue GE8322", "Mali-G52 MC2", "Adreno (TM) 505", "unknown"] {
            assert!(!trace_gpu(wgpu::Backend::Vulkan, name));
        }
    }
    #[test]
    fn begin_precedes_creation_and_error_result_is_preserved() {
        use std::cell::RefCell;
        let lines = RefCell::new(Vec::new());
        let result: Result<(), &str> = trace_build(true, 0x123456789, || "test draw state".into(),
            |line| lines.borrow_mut().push(line.to_owned()), || {
                assert_eq!(lines.borrow().len(), 1);
                Err("driver refused")
            });
        assert_eq!(result, Err("driver refused"));
        let lines = lines.into_inner();
        assert!(lines[0].contains("key=123456789: test draw state"));
        let id = lines[0].split_whitespace().nth(3).unwrap();
        assert!(lines[1].starts_with(&format!("gs pipeline: returned {id} key=123456789")));
    }
    #[test]
    fn disabled_trace_neither_formats_nor_logs() {
        assert_eq!(trace_build(false, 1, || panic!("no formatting"), |_| panic!("no log"), || 42), 42);
    }
}

/// Built pipelines by key (failures are kept too, so a bad key is reported once, not per draw).
pub struct Pipelines {
    built: HashMap<u64, Result<wgpu::RenderPipeline, String>>,
    /// SpecMode::Background's worker (made at the first specialised draw).
    #[cfg(not(target_arch = "wasm32"))]
    worker: Option<Worker>,
}

/// What the worker thread is asked.
#[cfg(not(target_arch = "wasm32"))]
enum Job {
    /// Build this key's pipeline and answer with it.
    Build(u64, PipelineDesc),
    /// The render thread built this key itself: it goes into the pipeline list.
    Learned(u64),
}

/// The thread that builds pipelines off the frame (the specialised ones, and the listed ones of
/// earlier runs), and the keys it has not answered yet.
#[cfg(not(target_arch = "wasm32"))]
struct Worker {
    jobs: std::sync::mpsc::Sender<Job>,
    done: std::sync::mpsc::Receiver<(u64, Result<wgpu::RenderPipeline, String>)>,
    /// Specialised keys a draw asked for.
    waiting: std::collections::HashSet<u64>,
    /// Listed keys sent at the start (`Pipelines::build_ahead`).
    ahead: std::collections::HashSet<u64>,
    /// When they were sent and how many, until the last one is back (one log line then).
    ahead_sent: Option<(std::time::Instant, usize)>,
}

#[cfg(not(target_arch = "wasm32"))]
impl Worker {
    /// `list`: the pipeline list, to which the thread adds every key that builds (it alone writes
    /// the file, so no write is ever on the render thread).
    fn start(gpu: &'static Gpu, sources: Sources, mut list: Option<list::File>) -> Worker {
        let (jobs, inbox) = std::sync::mpsc::channel::<Job>();
        let (outbox, done) = std::sync::mpsc::channel();
        let spawned = std::thread::Builder::new().name("dhd-pipelines".into()).spawn(move || {
            // Ends when the Pipelines (the sender) is dropped.
            while let Ok(job) = inbox.recv() {
                let (key, d) = match job {
                    Job::Build(key, d) => (key, d),
                    Job::Learned(key) => {
                        if let Some(list) = list.as_mut() {
                            list.learn(key);
                        }
                        continue;
                    }
                };
                let start = std::time::Instant::now();
                let pipe = scoped_pipeline(gpu, &format!("native pipeline {key:X}"), || build(gpu, &sources, key, &d))
                    .and_then(|r| r)
                    .map_err(|e| format!("{e} ({})", summary(key, &d)));
                let took = start.elapsed().as_secs_f64();
                if took > 0.25 {
                    crate::host::log(&format!("gs native: pipeline {key:X} built in {took:.2} s (worker)"));
                }
                if let (Ok(_), Some(list)) = (&pipe, list.as_mut()) {
                    list.learn(key);
                }
                if outbox.send((key, pipe)).is_err() {
                    break;
                }
            }
        });
        if let Err(e) = spawned {
            crate::host::log(&format!("gs native: no pipeline worker ({e}); pipelines are built at first use"));
        }
        Worker { jobs, done, waiting: std::collections::HashSet::new(), ahead: std::collections::HashSet::new(),
                 ahead_sent: None }
    }
}

/// The pipeline list (finding 256): the keys of the draw pipelines this device built in earlier
/// runs, `pipelines.txt` in the player's data folder. A draw state the game uses for the first
/// time needs its generic pipeline at once, and the render thread stops for the driver's compile
/// (about 150 ms each on a weak phone: the title's first pictures, the flight over the track, four
/// times in a race). The keys are the same in every run, so each one that builds is written down,
/// and the next start builds them all on the worker thread while the game boots and shows its
/// menus (`Pipelines::build_ahead`); a draw then finds its pipeline made. Only keys this device
/// built itself are listed, so nothing is compiled ahead that it would not compile anyway, and a
/// listed key that no longer builds is left to its first draw. The first line names the list's
/// format, the GPU with its driver and the shaders' hash: another GPU, driver or shader source
/// starts a new list. `DHD_PIPELINE_LIST=0` turns the building ahead off (comparisons).
///
/// The guard (finding 260): a driver whose shader compiler crashes on a pipeline ends the game at
/// every start that builds it (0.8.6's mesh pipelines on the Redmi 9's Mali-G52). So each draw
/// pipeline's key is written to `pipelines-building.txt` beside the list while the driver builds
/// it (`building`), under the list's first line, and taken out when the build returns. A key left
/// there proves only that the run ended during the build (a crash, or the player or the system
/// closing the game), so the next start (`recover`) refuses a key for good only on evidence:
/// the game's crash handler (port/hle/host.c `crashed`) writes `pipelines-crash.txt` with the key
/// its own thread was building (`dhdr_native_pipeline_crash`). A key so named goes into the list
/// as a refused key, `!KEY`, and is not built again on this GPU, driver and shaders (`refused`); a
/// refused mesh key turns Native VU off at the renderer's create. A key left without that evidence
/// (killed, closed, a crash on another thread or one the handler did not see) becomes a suspect,
/// `?KEY` in the marker: built again, but alone (no other build at the same time, so a second
/// death names one key), and refused only when it is left behind a second time with no build of
/// it returning in between. A crash that names a key builds the other keys left with it again. A
/// listed key is one this device built before with the same driver and shaders: logged, never
/// refused. Each step is a line in dhd.log; the crash itself is in the run before's log
/// (dhd-previous.log). The way back needs no file: a new driver or a release with other native
/// shaders is a new first line, and every refused key is tried again.
#[cfg(not(target_arch = "wasm32"))]
pub(crate) mod list {
    use std::collections::HashSet;
    use std::io::Write;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicPtr, AtomicU64, AtomicUsize, Ordering};
    use std::sync::{Mutex, RwLock};

    /// The keys' format: a change of a key's meaning (the module's first comment) takes a new number.
    /// 2: the shaders' hash in the first line, and refused keys (finding 260).
    const FORMAT: u32 = 2;
    /// More keys than any session builds; a list this long is not added to.
    const MOST: usize = 4096;
    /// The largest list file read, in bytes: MOST keys of 17 bytes and a header, with room.
    const LARGEST: u64 = 256 * 1024;

    static PATH: Mutex<Option<PathBuf>> = Mutex::new(None);

    /// `dhdr_native_pipeline_list`: where the list is (None: no list).
    pub fn set_path(path: Option<PathBuf>) {
        *PATH.lock().unwrap_or_else(|p| p.into_inner()) = path;
    }

    /// The list's file, unless there is none or `DHD_PIPELINE_LIST=0`.
    pub fn path() -> Option<PathBuf> {
        if std::env::var("DHD_PIPELINE_LIST").is_ok_and(|v| v == "0") {
            return None;
        }
        PATH.lock().unwrap_or_else(|p| p.into_inner()).clone()
    }

    /// The first line for this GPU, driver and shaders.
    pub fn header(info: &wgpu::AdapterInfo) -> String {
        let text = format!("dhd-pipelines {FORMAT} {:?} {:04X}:{:04X} {} | {} | {} | shaders {:016X}", info.backend,
                           info.vendor, info.device, info.name, info.driver, info.driver_info, shaders_hash());
        text.replace(['\n', '\r'], " ")
    }

    /// FNV-1a of the native renderer's shader sources: a new shader is a new list (and its refused
    /// keys are tried again).
    fn shaders_hash() -> u64 {
        let mut h: u64 = 0xcbf2_9ce4_8422_2325;
        for b in crate::shaders::NATIVE.bytes().chain(crate::shaders::NATIVE_DUAL.bytes()) {
            h = (h ^ b as u64).wrapping_mul(0x0000_0100_0000_01b3);
        }
        h
    }

    /// The keys a list's text refuses (`!KEY` lines), when its first line is `header`.
    pub fn parse_refused(text: &str, header: &str) -> Vec<u64> {
        let mut lines = text.lines();
        if lines.next() != Some(header) {
            return Vec::new();
        }
        lines.filter_map(|l| l.trim().strip_prefix('!')).filter_map(|k| u64::from_str_radix(k.trim(), 16).ok()).collect()
    }

    /// The guard's state: the first line and the marker's path (None until `recover` ran: nothing
    /// is written), the keys being built now (any thread), the suspects (left by the run before
    /// without a crash note, not built since), the refused keys, and whether the marker can't be
    /// written.
    struct Guard {
        header: Option<String>,
        marker: Option<PathBuf>,
        flying: Vec<u64>,
        suspects: Vec<u64>,
        refused: Option<HashSet<u64>>,
        failed: bool,
    }
    static GUARD: Mutex<Guard> = Mutex::new(Guard {
        header: None,
        marker: None,
        flying: Vec::new(),
        suspects: Vec::new(),
        refused: None,
        failed: false,
    });

    fn guard() -> std::sync::MutexGuard<'static, Guard> {
        GUARD.lock().unwrap_or_else(|p| p.into_inner())
    }

    /// Which pipeline each thread is in the driver's build of, for the crash handler: atomics
    /// only, read without a lock or an allocation (`crash_note`).
    struct Slot {
        thread: AtomicUsize,
        key: AtomicU64,
    }
    static SLOTS: [Slot; 8] = [const { Slot { thread: AtomicUsize::new(0), key: AtomicU64::new(0) } }; 8];
    /// `pipelines-crash.txt`'s path, NUL-terminated (set by `recover`, never freed).
    static CRASH_PATH: AtomicPtr<std::ffi::c_char> = AtomicPtr::new(std::ptr::null_mut());
    /// A suspect's build takes it for writing (alone), every other build for reading.
    static TURNS: RwLock<()> = RwLock::new(());

    /// The calling thread's id, the same in `building` and in a signal handler.
    fn thread_id() -> usize {
        #[cfg(unix)]
        {
            extern "C" {
                fn pthread_self() -> usize;
            }
            // SAFETY: no arguments; async-signal-safe in practice (a register or TLS read).
            unsafe { pthread_self() }
        }
        #[cfg(windows)]
        {
            extern "system" {
                fn GetCurrentThreadId() -> u32;
            }
            // SAFETY: no arguments.
            unsafe { GetCurrentThreadId() as usize }
        }
    }

    /// The crash handler's question: the crash note's path (NULL before `recover`) and the key the
    /// calling thread is building (0: none). No lock, no allocation: a signal handler calls it.
    pub fn crash_note() -> (*const std::ffi::c_char, u64) {
        let me = thread_id();
        let key = SLOTS.iter().find(|s| s.thread.load(Ordering::Acquire) == me).map_or(0, |s| s.key.load(Ordering::Acquire));
        (CRASH_PATH.load(Ordering::Acquire), key)
    }

    /// What `recover` decides about the keys a run left in the marker.
    #[derive(Debug, Default, PartialEq)]
    pub struct Judged {
        /// Named by the crash note: refused.
        pub crashed: Vec<u64>,
        /// Suspects left behind again: refused.
        pub twice: Vec<u64>,
        /// Built again as before (listed, or another thread's crash).
        pub again: Vec<u64>,
        /// Built again alone, refused if left behind again.
        pub suspects: Vec<u64>,
    }

    /// `recover`'s judgement of a marker and a crash note. Nothing when the marker is another
    /// GPU's, driver's or shaders'. The note is `crash KEY` (the crashing thread's build) or
    /// `crash` (a crash on a thread that built nothing).
    pub fn judge(marker: &str, crash: &str, list: &str, header: &str) -> Judged {
        let mut j = Judged::default();
        let mut lines = marker.lines();
        if lines.next() != Some(header) {
            return j;
        }
        let hex = |t: &str| u64::from_str_radix(t.trim(), 16).ok();
        let (mut flying, mut before) = (Vec::new(), Vec::new());
        for line in lines {
            match line.trim().strip_prefix('?') {
                Some(k) => before.extend(hex(k)),
                None => flying.extend(hex(line)),
            }
        }
        let named = crash.lines().find_map(|l| l.trim().strip_prefix("crash")).and_then(hex);
        let listed: HashSet<u64> = parse(list, header).unwrap_or_default().into_iter().collect();
        let refused: HashSet<u64> = parse_refused(list, header).into_iter().collect();
        let mut seen = HashSet::new();
        for key in flying {
            if refused.contains(&key) || !seen.insert(key) {
                continue;
            }
            if listed.contains(&key) {
                j.again.push(key);
            } else if named == Some(key) {
                j.crashed.push(key);
            } else if before.contains(&key) {
                j.twice.push(key);
            } else if named.is_some() {
                j.again.push(key);
            } else {
                j.suspects.push(key);
            }
        }
        // a suspect not built again stays one
        j.suspects.extend(before.into_iter().filter(|k| !refused.contains(k) && seen.insert(*k)));
        j
    }

    /// At the native renderer's create, before any draw pipeline is built: the keys the run before
    /// left are judged (`judge`), the list gets the refused ones, the marker and the crash note are
    /// removed, and from now on every build is written down (`building`). Returns every refused key.
    pub fn recover(info: &wgpu::AdapterInfo) -> Vec<u64> {
        let Some(list) = PATH.lock().unwrap_or_else(|p| p.into_inner()).clone() else { return Vec::new() };
        {
            // once a process: a renderer made again (a lost device) finds its own builds in the marker
            let g = guard();
            if g.header.is_some() {
                let mut refused: Vec<u64> = g.refused.iter().flatten().copied().collect();
                refused.sort_unstable();
                return refused;
            }
        }
        let (marker, crash) = (list.with_file_name("pipelines-building.txt"), list.with_file_name("pipelines-crash.txt"));
        let owner = take_guard(&list.with_file_name("pipelines-guard.lock"), crate::host::log);
        recover_files(&list, &marker, &crash, &header(info), owner, crate::host::log)
    }

    /// The lock held for a run's whole life by the game that owns the guard's files in its data
    /// folder (`claim`). The system frees it when the process ends in any way (a crash, a kill), so
    /// a marker is judged only when the run that wrote it is over.
    static OWNER: Mutex<Option<std::fs::File>> = Mutex::new(None);

    /// The guard's lock file, taken without waiting: Some(file) to keep while the game runs, None when
    /// another live game holds it; an error when the lock can't be used here at all.
    pub fn claim(path: &std::path::Path) -> std::io::Result<Option<std::fs::File>> {
        let file = std::fs::OpenOptions::new().read(true).write(true).create(true).truncate(false).open(path)?;
        match file.try_lock() {
            Ok(()) => Ok(Some(file)),
            Err(std::fs::TryLockError::WouldBlock) => Ok(None),
            Err(std::fs::TryLockError::Error(e)) => Err(e),
        }
    }

    /// Whether this game owns the guard's files: it holds the lock (kept in `OWNER` until the process
    /// ends), or the lock can't be used here (a file system without locks, a folder where the lock
    /// file can't be made): then this game is assumed alone, as before the lock (0.8.7), so a
    /// pipeline the driver dies on is still refused. Only a lock another live game holds makes it a
    /// non-owner.
    pub fn take_guard(path: &std::path::Path, mut log: impl FnMut(&str)) -> bool {
        match claim(path) {
            Ok(Some(lock)) => {
                *OWNER.lock().unwrap_or_else(|p| p.into_inner()) = Some(lock);
                true
            }
            Ok(None) => {
                log("gs native: another game runs from this data folder (pipelines-guard.lock is held); its \
                     pipelines-building.txt is not judged and this game writes none");
                false
            }
            Err(e) => {
                log(&format!("gs native: pipelines-guard.lock can't be used ({e}); this game is taken to be the \
                              only one in its data folder and keeps the pipeline guard"));
                true
            }
        }
    }

    /// `recover` on these files (the tests use their own). `owner`: this game holds the guard's lock
    /// (`claim`); else another live game in the same folder owns the marker and the crash note, and
    /// this one only reads the refused keys and writes neither.
    pub fn recover_files(list: &std::path::Path, marker: &std::path::Path, crash: &std::path::Path, header: &str,
                         owner: bool, mut log: impl FnMut(&str)) -> Vec<u64> {
        let read = |path: &std::path::Path| {
            std::fs::metadata(path).is_ok_and(|m| m.len() <= LARGEST).then(|| std::fs::read_to_string(path).ok()).flatten()
        };
        let mut text = read(list).unwrap_or_default();
        if !owner {
            let refused = refused_in(&text, header, &mut log);
            let mut g = guard();
            g.header = Some(header.to_string());
            g.marker = None; // nothing is written (write_marker)
            g.refused = Some(refused.iter().copied().collect());
            g.suspects = Vec::new();
            g.failed = false;
            return refused;
        }
        let note = read(crash).unwrap_or_default();
        let mut suspects = Vec::new();
        let mut failed = false;
        // a marker is judged only when it can be taken away: one that stays (a folder this game can't
        // write) would be judged again at every start, and a stale one must never become a refusal
        let dead = read(marker).filter(|_| match std::fs::remove_file(marker) {
            Err(e) if e.kind() != std::io::ErrorKind::NotFound => {
                log(&format!("gs native: pipelines-building.txt can't be removed ({e}); it is not judged, and \
                              the guard is off in this run"));
                failed = true;
                false
            }
            _ => true,
        });
        if let Some(dead) = dead {
            let j = judge(&dead, &note, &text, header);
            for key in &j.again {
                log(&format!("gs native: the run before ended while pipeline {key:X} was built, not in its build \
                              (built before, or another thread's crash); it is built again (pipelines-building.txt)"));
            }
            for key in &j.suspects {
                log(&format!("gs native: the run before ended while pipeline {key:X} was built, with no crash in its \
                              build on record (closed, killed, or not seen); it is built again, alone, and refused \
                              only if a run ends in its build again (pipelines-building.txt: ?{key:X})"));
            }
            let refuse: Vec<(u64, &str)> = j.crashed.iter().map(|&k| (k, "the run before crashed inside the driver's build of"))
                .chain(j.twice.iter().map(|&k| (k, "a second run in a row ended inside the driver's build of")))
                .collect();
            if !refuse.is_empty() {
                if parse(&text, header).is_none() {
                    text = format!("{header}\n"); // a new list for this GPU, driver and shaders
                }
                if !text.ends_with('\n') {
                    text.push('\n');
                }
                for (key, why) in &refuse {
                    text.push_str(&format!("!{key:X}\n"));
                    log(&format!("gs native: {why} pipeline {key:X}; it is not built again on this GPU, driver and \
                                  shaders (pipelines.txt: !{key:X})"));
                }
                if let Err(e) = std::fs::write(list, &text) {
                    log(&format!("gs native: the pipeline list can't be written ({e}); the refused pipelines are \
                                  refused in this run only"));
                }
            }
            suspects = j.suspects;
        }
        let _ = std::fs::remove_file(crash);
        let refused = refused_in(&text, header, &mut log);
        if let Some(path) = crash.to_str().and_then(|p| std::ffi::CString::new(p).ok()) {
            CRASH_PATH.store(path.into_raw(), Ordering::Release); // an earlier one is left (a handler may read it)
        }
        let mut g = guard();
        g.header = Some(header.to_string());
        g.marker = Some(marker.to_path_buf());
        g.refused = Some(refused.iter().copied().collect());
        g.suspects = suspects;
        g.failed = failed;
        write_marker(&mut g); // the suspects stay suspects until they build
        refused
    }

    /// The refused keys of a list's text, sorted, and their log line.
    fn refused_in(text: &str, header: &str, log: &mut impl FnMut(&str)) -> Vec<u64> {
        let mut refused = parse_refused(text, header);
        refused.sort_unstable();
        refused.dedup();
        if !refused.is_empty() {
            let keys: Vec<String> = refused.iter().map(|k| format!("{k:X}")).collect();
            log(&format!("gs native: pipelines refused on this GPU, driver and shaders (the driver died building them \
                          in an earlier run; pipelines.txt): {}", keys.join(" ")));
        }
        refused
    }

    /// Whether `key` died in the driver's build in an earlier run (`recover`).
    pub fn refused(key: u64) -> bool {
        guard().refused.as_ref().is_some_and(|r| r.contains(&key))
    }

    /// The marker: the first line, the suspects (`?KEY`) and the keys being built, or no file when
    /// there are none. Two small writes per pipeline build: a build is a driver compile of tens to
    /// hundreds of ms, and in a race the listed ones are built ahead by the worker thread.
    fn write_marker(g: &mut Guard) {
        let (Some(header), Some(marker)) = (&g.header, &g.marker) else { return };
        if g.failed {
            return;
        }
        let written = if g.flying.is_empty() && g.suspects.is_empty() {
            match std::fs::remove_file(marker) {
                Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(e),
                _ => Ok(()),
            }
        } else {
            let mut text = format!("{header}\n");
            for key in &g.suspects {
                text.push_str(&format!("?{key:X}\n"));
            }
            for key in &g.flying {
                text.push_str(&format!("{key:X}\n"));
            }
            std::fs::write(marker, text)
        };
        if let Err(e) = written {
            // once a run; what an earlier write left would be stale, so it goes (when it can: a
            // marker the next start can't remove is not judged there)
            g.failed = true;
            let _ = std::fs::remove_file(marker);
            crate::host::log(&format!("gs native: pipelines-building.txt can't be written ({e}); a pipeline the driver \
                                       dies on is not refused next time"));
        }
    }

    /// `key` is in the marker, and in a slot for the crash handler, while this lives (the driver's
    /// build of it); a suspect is built alone. No sync to the disk: a process that dies leaves its
    /// written file to the system.
    pub struct Building {
        key: u64,
        slot: Option<&'static Slot>,
        _turn: Turn,
    }

    /// The build's hold on `TURNS` (only held, never read).
    #[allow(dead_code)]
    enum Turn {
        Shared(std::sync::RwLockReadGuard<'static, ()>),
        Alone(std::sync::RwLockWriteGuard<'static, ()>),
    }

    pub fn building(key: u64) -> Building {
        let alone = guard().suspects.contains(&key);
        let turn = if alone {
            Turn::Alone(TURNS.write().unwrap_or_else(|p| p.into_inner()))
        } else {
            Turn::Shared(TURNS.read().unwrap_or_else(|p| p.into_inner()))
        };
        {
            let mut g = guard();
            g.flying.push(key);
            write_marker(&mut g);
        }
        let me = thread_id();
        let slot = SLOTS.iter().find(|s| s.thread.compare_exchange(0, me, Ordering::AcqRel, Ordering::Relaxed).is_ok());
        if let Some(s) = slot {
            s.key.store(key, Ordering::Release);
        }
        Building { key, slot, _turn: turn }
    }

    impl Drop for Building {
        fn drop(&mut self) {
            if let Some(s) = self.slot {
                s.key.store(0, Ordering::Release);
                s.thread.store(0, Ordering::Release);
            }
            let mut g = guard();
            if let Some(at) = g.flying.iter().position(|&k| k == self.key) {
                g.flying.remove(at);
            }
            g.suspects.retain(|&k| k != self.key); // its build returned
            write_marker(&mut g);
        } // then the turn ends
    }

    /// The keys of a list's text in the order to build them: the generic ones first (a draw
    /// cannot wait for those), then the specialised ones (their draws run the generic pipeline
    /// meanwhile), each in the order it was first used. None when the first line is not `header`.
    pub fn parse(text: &str, header: &str) -> Option<Vec<u64>> {
        let mut lines = text.lines();
        if lines.next() != Some(header) {
            return None;
        }
        let mut seen = HashSet::new();
        let mut keys: Vec<u64> = lines
            .filter_map(|l| u64::from_str_radix(l.trim(), 16).ok())
            .filter(|&k| seen.insert(k))
            .take(MOST)
            .collect();
        keys.sort_by_key(|&k| k >> super::SPEC_SHIFT != 0); // stable: first-use order within each
        Some(keys)
    }

    /// The list's file as the worker thread keeps it.
    pub struct File {
        path: PathBuf,
        header: String,
        known: HashSet<u64>,
        /// The file starts with `header` (else the first key writes a new file).
        valid: bool,
        failed: bool,
    }

    impl File {
        /// The file at `path` and its keys to build (none when it is absent or another GPU's).
        pub fn open(path: PathBuf, header: String) -> (File, Vec<u64>) {
            // A file far larger than MOST keys can be is not ours: it is not read (and the first
            // key writes a new one).
            let small = std::fs::metadata(&path).is_ok_and(|m| m.len() <= LARGEST);
            let keys = if small { std::fs::read_to_string(&path).ok().and_then(|text| parse(&text, &header)) } else { None };
            let valid = keys.is_some();
            let keys = keys.unwrap_or_default();
            (File { path, header, known: keys.iter().copied().collect(), valid, failed: false }, keys)
        }

        /// A key that built: one more line, once.
        pub fn learn(&mut self, key: u64) {
            if self.failed || self.known.len() >= MOST || !self.known.insert(key) {
                return;
            }
            let written = if self.valid {
                std::fs::OpenOptions::new().append(true).open(&self.path).and_then(|mut f| writeln!(f, "{key:X}"))
            } else {
                std::fs::write(&self.path, format!("{}\n{key:X}\n", self.header))
            };
            match written {
                Ok(()) => self.valid = true,
                Err(e) => {
                    self.failed = true;
                    crate::host::log(&format!("gs native: the pipeline list can't be written ({e}); pipelines are built at first use next time too"));
                }
            }
        }
    }

    #[cfg(test)]
    mod tests {
        use super::*;

        #[test]
        fn generic_keys_first_each_in_first_use_order() {
            let text = "H\nE0180033E26BC000\n3C000\n\nE9C98B\nnot a key\n3C000\nE000000000E9C98B\n";
            assert_eq!(parse(text, "H"), Some(vec![0x3C000, 0xE9C98B, 0xE0180033E26BC000, 0xE000000000E9C98B]));
        }

        #[test]
        fn another_gpus_list_is_not_used() {
            assert_eq!(parse("H\n3C000\n", "other"), None);
            assert_eq!(parse("", "H"), None);
            assert_eq!(parse("H\n", "H"), Some(vec![]));
        }

        #[test]
        fn a_key_is_written_once_and_read_back() {
            let path = std::env::temp_dir().join(format!("dhd-pipelines-test-{}.txt", std::process::id()));
            let _ = std::fs::remove_file(&path);
            let (mut file, keys) = File::open(path.clone(), "H 1".into());
            assert!(keys.is_empty());
            file.learn(0x3C000);
            file.learn(0xE0180033E26BC000);
            file.learn(0x3C000);
            assert_eq!(std::fs::read_to_string(&path).unwrap(), "H 1\n3C000\nE0180033E26BC000\n");
            let (mut again, keys) = File::open(path.clone(), "H 1".into());
            assert_eq!(keys, vec![0x3C000, 0xE0180033E26BC000]);
            again.learn(0x3C000);
            again.learn(0x1C000);
            assert_eq!(std::fs::read_to_string(&path).unwrap(), "H 1\n3C000\nE0180033E26BC000\n1C000\n");
            // another driver: the old keys are not built, and the first new key starts the file again
            let (mut other, keys) = File::open(path.clone(), "H 2".into());
            assert!(keys.is_empty());
            other.learn(0x5);
            assert_eq!(std::fs::read_to_string(&path).unwrap(), "H 2\n5\n");
            std::fs::remove_file(&path).unwrap();
        }

        #[test]
        fn only_a_crash_note_refuses_at_once() {
            let list = "H\n3C000\n!E26BC000\n";
            let j = |marker, crash| judge(marker, crash, list, "H");
            // killed or closed during a build: no refusal, a suspect
            assert_eq!(j("H\nE0180033E26BC000\n", ""), Judged { suspects: vec![0xE0180033E26BC000], ..Default::default() });
            // the crash handler named the key: refused
            assert_eq!(j("H\nE0180033E26BC000\n", "crash E0180033E26BC000\n"),
                       Judged { crashed: vec![0xE0180033E26BC000], ..Default::default() });
            // two keys in building, the crash in one: only that one; the other is built again
            assert_eq!(j("H\nE0180033E26BC000\n1D20000\n", "crash 00000000001D20000\n"),
                       Judged { crashed: vec![0x1D20000], again: vec![0xE0180033E26BC000], ..Default::default() });
            // two keys, a crash on a thread that built nothing: both suspects, none refused
            assert_eq!(j("H\nE0180033E26BC000\n1D20000\n", "crash\n"),
                       Judged { suspects: vec![0xE0180033E26BC000, 0x1D20000], ..Default::default() });
            // a suspect left behind again: refused; one not built again stays a suspect
            assert_eq!(j("H\n?E0180033E26BC000\n?5\nE0180033E26BC000\n", ""),
                       Judged { twice: vec![0xE0180033E26BC000], suspects: vec![5], ..Default::default() });
            // listed: built before here, never refused; refused already: once
            assert_eq!(j("H\n3C000\nE26BC000\n", "crash 3C000\n"), Judged { again: vec![0x3C000], ..Default::default() });
            // another driver's or shaders' marker says nothing about this one
            assert_eq!(j("old\nE0180033E26BC000\n", "crash E0180033E26BC000\n"), Judged::default());
            assert_eq!(parse_refused(list, "H"), vec![0xE26BC000]);
            assert_eq!(parse_refused(list, "other"), Vec::<u64>::new());
            // a refused key is not built ahead
            assert_eq!(parse(list, "H"), Some(vec![0x3C000]));
        }

        /// A run's end inside `building`: the marker as the dead process left it (the build never
        /// returned), and the crash note if the crash handler wrote one.
        fn die_in_build(marker: &std::path::Path, crash: &std::path::Path, key: u64, crashed: bool) {
            let dying = building(key);
            let left = std::fs::read_to_string(marker).unwrap();
            if crashed {
                // what host.c's crashed() writes, from the dying thread
                let (path, named) = crash_note();
                assert!(!path.is_null() && named == key);
                // SAFETY: recover's NUL-terminated path.
                let path = unsafe { std::ffi::CStr::from_ptr(path) }.to_str().unwrap();
                assert_eq!(std::path::Path::new(path), crash);
                std::fs::write(crash, format!("crash {named:016X}\n")).unwrap();
            }
            drop(dying);
            std::fs::write(marker, left).unwrap();
            assert_eq!(crash_note().1, 0);
        }

        /// The whole round on files (one test: the guard is the process's).
        #[test]
        fn a_run_that_died_building_is_judged_on_evidence() {
            let dir = std::env::temp_dir().join(format!("dhd-pipeline-guard-{}", std::process::id()));
            std::fs::create_dir_all(&dir).unwrap();
            let (list, marker, crash) = (dir.join("pipelines.txt"), dir.join("pipelines-building.txt"),
                                         dir.join("pipelines-crash.txt"));
            std::fs::write(&list, "H\n3C000\n").unwrap();
            let _ = std::fs::remove_file(&marker);
            let _ = std::fs::remove_file(&crash);
            let mut lines = Vec::new();
            let start = |header: &str, lines: &mut Vec<String>| {
                guard().header = None; // a new process
                recover_files(&list, &marker, &crash, header, true, |l| lines.push(l.to_owned()))
            };
            assert!(start("H", &mut lines).is_empty());
            {
                let _ok = building(0x1D20000);
                assert_eq!(std::fs::read_to_string(&marker).unwrap(), "H\n1D20000\n");
            }
            assert!(!marker.exists());

            // killed during a build (no crash note): no refusal, a suspect, built alone
            die_in_build(&marker, &crash, 0xE0180033E26BC000, false);
            assert!(start("H", &mut lines).is_empty());
            assert!(!refused(0xE0180033E26BC000));
            assert!(lines.iter().any(|l| l.contains("pipeline E0180033E26BC000 was built, with no crash")));
            assert_eq!(std::fs::read_to_string(&marker).unwrap(), "H\n?E0180033E26BC000\n");
            {
                let _alone = building(0xE0180033E26BC000);
                assert!(TURNS.try_read().is_err(), "a suspect builds alone");
            }
            assert!(!marker.exists(), "a suspect that built is cleared");
            assert_eq!(std::fs::read_to_string(&list).unwrap(), "H\n3C000\n");

            // two keys in building, the crash in one: only that one is refused
            let (built, end) = (std::sync::mpsc::channel(), std::sync::mpsc::channel::<()>());
            let other = std::thread::spawn(move || {
                let _other = building(0x1D20000); // the worker thread's build
                built.0.send(()).unwrap();
                end.1.recv().unwrap();
            });
            built.1.recv().unwrap();
            die_in_build(&marker, &crash, 0xE0180033E26BC000, true);
            let left = std::fs::read_to_string(&marker).unwrap();
            end.0.send(()).unwrap();
            other.join().unwrap();
            std::fs::write(&marker, left).unwrap();
            assert_eq!(start("H", &mut lines), vec![0xE0180033E26BC000]);
            assert!(refused(0xE0180033E26BC000) && !refused(0x1D20000));
            assert!(!marker.exists() && !crash.exists());
            assert_eq!(std::fs::read_to_string(&list).unwrap(), "H\n3C000\n!E0180033E26BC000\n");
            assert!(lines.iter().any(|l| l.contains("crashed inside the driver's build of pipeline E0180033E26BC000")));
            assert!(lines.iter().any(|l| l.contains("pipeline 1D20000 was built, not in its build")));

            // the next start still refuses it, and a new driver or shader tries it again
            assert_eq!(start("H", &mut lines), vec![0xE0180033E26BC000]);
            assert!(start("H2", &mut lines).is_empty());
            assert!(!refused(0xE0180033E26BC000));

            // a key left behind by two runs in a row with no crash note: refused at the second
            die_in_build(&marker, &crash, 0x77, false);
            assert!(start("H2", &mut lines).is_empty());
            die_in_build(&marker, &crash, 0x77, false);
            assert_eq!(start("H2", &mut lines), vec![0x77]);

            // a second game in the same folder while the first runs (it holds the lock): the first's
            // live marker is neither judged nor written, twice in a row; the refusals still hold
            let live = "H2\n?5\n99\n"; // the first game's: a suspect, and a build now
            std::fs::write(&marker, live).unwrap();
            let listed = std::fs::read_to_string(&list).unwrap();
            for _ in 0..2 {
                guard().header = None; // a new process, without the lock
                assert_eq!(recover_files(&list, &marker, &crash, "H2", false, |l| lines.push(l.to_owned())), vec![0x77]);
                assert!(refused(0x77) && !refused(0x99));
                {
                    let _second = building(0x99);
                    assert_eq!(std::fs::read_to_string(&marker).unwrap(), live, "the second game writes no marker");
                }
                assert_eq!(std::fs::read_to_string(&marker).unwrap(), live);
                assert_eq!(std::fs::read_to_string(&list).unwrap(), listed);
            }
            // the first run over (its lock freed): its marker is judged at last, a kill's way
            assert_eq!(start("H2", &mut lines), vec![0x77]);
            assert_eq!(std::fs::read_to_string(&marker).unwrap(), "H2\n?99\n?5\n");
            assert_eq!(std::fs::read_to_string(&list).unwrap(), listed);
            guard().header = None;
            std::fs::remove_dir_all(&dir).unwrap();
        }

        /// The guard's lock: one live holder per folder; free again once it is dropped (the
        /// system does the same when a process ends).
        #[test]
        fn one_game_a_folder_holds_the_guard() {
            let path = std::env::temp_dir().join(format!("dhd-pipelines-guard-{}.lock", std::process::id()));
            let first = claim(&path).unwrap().expect("a free lock is taken");
            assert!(claim(&path).unwrap().is_none(), "a second holder is refused");
            drop(first);
            let again = claim(&path).unwrap();
            assert!(again.is_some(), "a dropped lock is free");
            drop(again);
            std::fs::remove_file(&path).unwrap();
        }

        /// Only a lock another game holds makes a non-owner; a lock that can't be used keeps the
        /// guard (one game assumed, 0.8.7's way), with one line saying why.
        #[test]
        fn only_a_held_lock_gives_up_the_guard() {
            let path = std::env::temp_dir().join(format!("dhd-pipelines-held-{}.lock", std::process::id()));
            let other = claim(&path).unwrap().expect("a free lock is taken"); // another live game
            let mut lines = Vec::new();
            assert!(!take_guard(&path, |l| lines.push(l.to_owned())), "held by another game: not the owner");
            assert!(lines.last().unwrap().contains("another game runs from this data folder"));
            drop(other);
            std::fs::remove_file(&path).unwrap();
            // the lock file can't even be made (its folder is missing): an error, not contention
            let missing = std::env::temp_dir().join(format!("dhd-no-such-folder-{}", std::process::id())).join("x.lock");
            let mut lines = Vec::new();
            assert!(take_guard(&missing, |l| lines.push(l.to_owned())), "a lock that can't be used keeps the guard");
            assert_eq!(lines.len(), 1);
            assert!(lines[0].contains("can't be used") && lines[0].contains("keeps the pipeline guard"), "{}", lines[0]);
            // a direct claim's error is told apart from contention
            assert!(claim(&missing).is_err());
        }
    }
}

/// `dhdr_native_pipeline_crash` (`dhd_render_ext.h`, finding 260): for the game's crash handler
/// (port/hle/host.c `crashed`), the crash note's path (NULL: no list) and in `*key` the draw
/// pipeline the calling thread is in the driver's build of (0: none). Atomics only, no lock and
/// no allocation, so no `Guard::enter`: a signal handler calls it.
///
/// # Safety
/// `key` is NULL or writable.
#[cfg(not(target_arch = "wasm32"))]
#[no_mangle]
pub unsafe extern "C" fn dhdr_native_pipeline_crash(key: *mut u64) -> *const std::ffi::c_char {
    let (path, building) = list::crash_note();
    if !key.is_null() {
        // SAFETY: the caller's writable u64.
        unsafe { *key = building };
    }
    path
}

/// `dhdr_native_pipeline_list` (`dhd_render_ext.h`): the pipeline list's file (finding 256), a
/// UTF-8 path, or NULL for none; before `dhdr_native_create`. The browser has no list.
///
/// # Safety
/// `path` is NULL or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn dhdr_native_pipeline_list(path: *const std::ffi::c_char) {
    let _g = crate::guard::Guard::enter("dhdr_native_pipeline_list");
    #[cfg(not(target_arch = "wasm32"))]
    {
        // SAFETY: NULL or a NUL-terminated string (the caller's contract).
        let path = (!path.is_null()).then(|| unsafe { std::ffi::CStr::from_ptr(path) });
        list::set_path(path.and_then(|p| p.to_str().ok()).filter(|p| !p.is_empty()).map(std::path::PathBuf::from));
    }
    #[cfg(target_arch = "wasm32")]
    let _ = path;
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
        #[cfg(not(target_arch = "wasm32"))]
        self.receive();
        if key != generic {
            #[cfg(not(target_arch = "wasm32"))]
            {
                if spec_mode() == SpecMode::Background {
                    let (mut here, mut failed) = (false, None);
                    if !self.built.contains_key(&key) {
                        let worker = self.worker.get_or_insert_with(|| Worker::start(gpu, Sources::of(objects), None));
                        // a listed key is on its way already
                        if !worker.ahead.contains(&key) && worker.waiting.insert(key) {
                            match describe(key as u32) {
                                Ok(d) => {
                                    // No worker thread: build it here, as SpecMode::Sync does.
                                    if worker.jobs.send(Job::Build(key, d)).is_err() {
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
        if worker.waiting.is_empty() && worker.ahead.is_empty() {
            return;
        }
        while let Ok((key, pipe)) = worker.done.try_recv() {
            let asked = worker.waiting.remove(&key);
            let ahead = worker.ahead.remove(&key);
            if self.built.get(&key).is_some_and(|r| r.is_ok()) {
                continue; // a draw needed it before the worker came to it, and built it
            }
            if let Err(e) = &pipe {
                if ahead && !asked {
                    continue; // a listed key that no longer builds: its first draw decides and reports
                }
                crate::host::log(&format!("gs native: specialised pipeline {key:X}: {e}; the generic one draws"));
            }
            self.built.insert(key, pipe);
        }
        if worker.ahead.is_empty() {
            if let Some((sent, count)) = worker.ahead_sent.take() {
                crate::host::log(&format!("gs native: the {count} listed pipelines were built {:.1} s after the renderer's start",
                                          sent.elapsed().as_secs_f64()));
            }
        }
    }

    /// The pipeline list's keys (`list`, finding 256) go to the worker thread, which builds them
    /// while the game starts; at the native renderer's create. Without a list: nothing.
    #[cfg(not(target_arch = "wasm32"))]
    pub fn build_ahead(&mut self, gpu: &'static Gpu, objects: &Objects) {
        let Some(path) = list::path() else { return };
        let (file, keys) = list::File::open(path, list::header(&gpu.info));
        let worker = self.worker.get_or_insert_with(|| Worker::start(gpu, Sources::of(objects), Some(file)));
        let special = spec_mode() == SpecMode::Background;
        let meshes = !crate::native::init::meshes_refused();
        for &key in &keys {
            let wanted = (key >> SPEC_SHIFT == 0 || special) && (key as u32 & MESH == 0 || meshes);
            if !wanted || self.built.contains_key(&key) {
                continue;
            }
            if let Ok(d) = describe(key as u32) {
                if worker.jobs.send(Job::Build(key, d)).is_ok() {
                    worker.ahead.insert(key);
                }
            }
        }
        if !worker.ahead.is_empty() {
            worker.ahead_sent = Some((std::time::Instant::now(), worker.ahead.len()));
        }
        crate::host::log(&format!("gs native: pipelines.txt lists {} draw pipelines of earlier runs; {} are built ahead on a worker thread",
                                  keys.len(), worker.ahead.len()));
    }

    /// Takes `other`'s pipelines into this cache (the browser's probed mesh pipelines, init.rs
    /// `web_prepare`).
    #[cfg_attr(not(feature = "web"), allow(dead_code))]
    pub fn extend(&mut self, other: Pipelines) {
        self.built.extend(other.built);
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
        #[cfg(not(target_arch = "wasm32"))]
        let worker = &self.worker;
        let built = self.built.entry(key).or_insert_with(|| {
            #[cfg(not(target_arch = "wasm32"))]
            let _spike = SpikeTimer::new("pipeline", key as u32);
            let d = describe(key as u32)?;
            #[cfg(not(target_arch = "wasm32"))]
            let start = std::time::Instant::now();
            let pipe = scoped_pipeline(gpu, &format!("native pipeline {key:X}"), || build(gpu, &Sources::of(objects), key, &d))
                .and_then(|r| r)
                .map_err(|e| format!("{e} ({})", summary(key, &d)))?;
            #[cfg(not(target_arch = "wasm32"))]
            {
                let took = start.elapsed().as_secs_f64();
                if took > 0.25 {
                    crate::host::log(&format!("gs native: draw pipeline {key:X} built in {took:.2} s"));
                }
                // built at its first use, on this thread: the next start builds it ahead
                if let Some(worker) = worker {
                    let _ = worker.jobs.send(Job::Learned(key));
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
    use super::{spec, without_kept_colour};

    /// Cs * 0 + Cd * 1 (and Cd * 1 - Cs * 0) loses its blend and its colour channels; every other
    /// blend stays as it is.
    #[test]
    fn kept_colour_is_not_blended() {
        let key = |src: u32, dst: u32, op: u32| 1 | src << 1 | dst << 6 | op << 11;
        assert_eq!(without_kept_colour(key(1, 2, 1), 15), (0, 8));
        assert_eq!(without_kept_colour(key(1, 2, 1), 7), (0, 0));
        assert_eq!(without_kept_colour(key(1, 2, 0), 7), (0, 0));
        assert_eq!(without_kept_colour(key(1, 2, 3), 15), (0, 8));
        // Cs * 0 - Cd is not Cd; other factors read Cs.
        assert_eq!(without_kept_colour(key(1, 2, 2), 7), (key(1, 2, 2), 7));
        assert_eq!(without_kept_colour(key(1, 6, 1), 7), (key(1, 6, 1), 7));
        assert_eq!(without_kept_colour(key(2, 2, 1), 7), (key(2, 2, 1), 7));
        assert_eq!(without_kept_colour(key(5, 6, 1), 15), (key(5, 6, 1), 15));
        // Min and Max ignore the factors; no blend stays no blend.
        assert_eq!(without_kept_colour(key(1, 2, 4), 7), (key(1, 2, 4), 7));
        assert_eq!(without_kept_colour(1 << 1 | 2 << 6, 7), (1 << 1 | 2 << 6, 7));
    }

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
