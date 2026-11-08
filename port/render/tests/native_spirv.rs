//! native.wgsl's SPIR-V as wgpu-hal 29.0.3's Vulkan backend makes it, for every pipeline stage
//! the native renderer builds (finding 153).
//!
//!     python port/tools/rustgs.py test --target x86_64-pc-windows-msvc -- --test native_spirv
//!
//! wgpu-hal compiles a WGSL module per pipeline stage (`vulkan/device.rs` compile_stage; it sets
//! SEPARATE_ENTRY_POINTS on every device): `process_overrides` keeps the one entry point, compacts
//! the module and applies the override constants, then `spv::write_vec` writes it with the
//! device's options (`vulkan/adapter.rs`) and the pipeline layout's binding map. This test does the
//! same for each entry point and constant set the renderer uses (init.rs, pipelines.rs) and for
//! present.wgsl's render probe (the pipeline that compiles on the Mali phones), and checks that
//! each stage writes and passes no image or sampler into a function (the construct Mali's r54
//! compiler crashes on; `shaders.rs` checks the WGSL of every module for it).
//!
//! Environment:
//! - `DHD_SPIRV_OUT`: a folder to keep each stage's SPIR-V in, `<device>/<entry>[-<constants>].<stage>.spv`.
//! - `DHD_MALIOC`: Arm's Mali Offline Compiler (malioc, from Arm Performance Studio; docs/TESTING.md):
//!   every stage is compiled by it too, for `DHD_MALIOC_CORE` (default Mali-G57), and the test
//!   fails when one does not compile. Without `DHD_SPIRV_OUT` the files go to `DHD_SCRATCH` (else
//!   the system temp) and are removed after.
//! - `DHD_NATIVE_WGSL` (and `DHD_NATIVE_DUAL_WGSL`): another native.wgsl (native_dual.wgsl) in its
//!   place, an older version or a candidate change, to compare or bisect; the entry points and
//!   constants it lacks are left out.
#![cfg(feature = "native")]

mod common;
use dhd_render::native::pipelines;
use dhd_render::shaders::{NATIVE, NATIVE_DUAL, PRESENT};
use naga::back::spv;
use std::collections::HashSet;
use std::path::{Path, PathBuf};

/// What decides a device's SPIR-V options (`vulkan/adapter.rs`): its Vulkan version and whether
/// it has robust buffer and image access (each turns a bounds-check policy off).
#[derive(Clone, Copy, Debug)]
struct Device {
    /// SPIR-V version: Vulkan 1.3 devices get 1.6, Vulkan 1.1 devices 1.3.
    lang_version: (u8, u8),
    robust_buffer_access2: bool,
    robust_image_access: bool,
}

impl Device {
    fn folder(&self) -> String {
        format!("spv{}{}{}", self.lang_version.0, self.lang_version.1, if self.robust_image_access { "-robust" } else { "" })
    }
}

/// The devices written for: a Vulkan 1.3 Mali with and without robustness, a Vulkan 1.1 one.
const DEVICES: [Device; 3] = [
    Device { lang_version: (1, 6), robust_buffer_access2: true, robust_image_access: true },
    Device { lang_version: (1, 6), robust_buffer_access2: false, robust_image_access: false },
    Device { lang_version: (1, 3), robust_buffer_access2: false, robust_image_access: false },
];

/// wgpu-hal's `naga_options` for a device with none of the optional features the renderer asks
/// for (dual-source blending needs no SPIR-V capability) and without InstanceFlags::DEBUG, on a
/// non-Qualcomm GPU, as `vulkan/adapter.rs` builds them; with `compile_stage`'s per-stage changes:
/// the layout's binding map, workgroup memory not zeroed (init.rs `compilation_options`).
fn options(device: Device, binding_map: spv::BindingMap) -> spv::Options<'static> {
    use spv::Capability as C;
    let capabilities = [
        C::Shader,
        C::Matrix,
        C::Sampled1D,
        C::Image1D,
        C::ImageQuery,
        C::DerivativeControl,
        C::StorageImageExtendedFormats,
        C::SampledCubeArray,
        C::SampleRateShading,
    ];
    let policy = |unchecked: bool| {
        if unchecked { naga::proc::BoundsCheckPolicy::Unchecked } else { naga::proc::BoundsCheckPolicy::Restrict }
    };
    spv::Options {
        lang_version: device.lang_version,
        flags: spv::WriterFlags::LABEL_VARYINGS | spv::WriterFlags::FORCE_POINT_SIZE,
        capabilities: Some(capabilities.iter().cloned().collect()),
        bounds_check_policies: naga::proc::BoundsCheckPolicies {
            index: naga::proc::BoundsCheckPolicy::Restrict,
            buffer: policy(device.robust_buffer_access2),
            image_load: policy(device.robust_image_access),
            binding_array: naga::proc::BoundsCheckPolicy::Unchecked,
        },
        zero_initialize_workgroup_memory: spv::ZeroInitializeWorkgroupMemoryMode::None,
        force_loop_bounding: true,
        ray_query_initialization_tracking: true,
        use_storage_input_output_16: false,
        fake_missing_bindings: false,
        binding_map,
        debug_info: None,
        task_dispatch_limits: Some(naga::back::TaskDispatchLimits { max_mesh_workgroups_per_dim: 0, max_mesh_workgroups_total: 0 }),
        mesh_shader_primitive_indices_clamp: true,
    }
}

/// A pipeline layout's binding map: `groups[g]` bindings in group g, numbered from 0 (wgpu-hal
/// numbers a set's Vulkan bindings in entry order; the renderer's entries are in order).
fn binding_map(groups: &[u32]) -> spv::BindingMap {
    let mut map = spv::BindingMap::new();
    for (group, &count) in groups.iter().enumerate() {
        let group = group as u32;
        for binding in 0..count {
            map.insert(naga::ResourceBinding { group, binding },
                       spv::BindingInfo { descriptor_set: group, binding, binding_array_size: None });
        }
    }
    map
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Source {
    Native,
    /// native.wgsl + native_dual.wgsl (init.rs's dual module)
    Dual,
    Present,
}

/// One pipeline stage: its module, entry point, override constants and layout (bindings per group).
#[derive(Clone, Debug)]
struct Stage {
    source: Source,
    stage: naga::ShaderStage,
    entry: &'static str,
    constants: Vec<(&'static str, f64)>,
    groups: &'static [u32],
}

/// The native layouts: group 0 the uniform arena, group 1 two entries (draw, texels, blit,
/// scanout), one (grow) or eleven (convert).
const TWO: &[u32] = &[1, 2];

impl Stage {
    fn new(source: Source, stage: naga::ShaderStage, entry: &'static str, groups: &'static [u32]) -> Stage {
        Stage { source, stage, entry, constants: Vec::new(), groups }
    }

    fn file_name(&self) -> String {
        let mut name = self.entry.to_string();
        for (k, v) in &self.constants {
            let short = if *k == "native_spec" { "spec" } else { "scale" };
            name.push_str(&format!("-{short}{:X}", *v as u64));
        }
        let ext = match self.stage {
            naga::ShaderStage::Vertex => "vert",
            naga::ShaderStage::Fragment => "frag",
            _ => "comp",
        };
        format!("{name}.{ext}.spv")
    }
}

/// Every stage the renderer builds: init.rs's objects, the draw pipelines' entry points with a
/// spread of `native_texel_scale` and `native_spec` values (pipelines.rs `build`, `spec`), and
/// present.wgsl's probe.
fn stages() -> Vec<Stage> {
    use naga::ShaderStage::{Compute, Fragment, Vertex};
    use Source::{Dual, Native, Present};
    let mut out = vec![
        Stage::new(Present, Vertex, "present_vertex", &[2]),
        Stage::new(Present, Fragment, "present_fragment", &[2]),
        Stage::new(Native, Vertex, "fullscreen_vertex", TWO),
        Stage::new(Native, Fragment, "native_blit", TWO),
        Stage::new(Native, Fragment, "native_upload_alpha", TWO),
        Stage::new(Native, Fragment, "present_pixel", TWO),
        Stage::new(Native, Fragment, "present_shrink", TWO),
        Stage::new(Native, Fragment, "present_fill", TWO),
        Stage::new(Native, Fragment, "native_grow_depth", &[1, 1]),
        Stage::new(Native, Compute, "native_scanout", TWO),
        Stage::new(Native, Compute, "native_scanout_fast", TWO),
        Stage::new(Native, Compute, "native_convert", &[1, 11]),
        Stage::new(Native, Vertex, "native_vertex", TWO),
    ];
    // Specialisations: each flag alone and in the common mixes, every CLAMP pair, every alpha test.
    let mut specs = vec![0u32];
    let flag_sets = [0u32, 1, 1 | 2, 1 | 2 | 4, 1 | 2 | 8, 1 | 2 | 12, 1 | 32, 1 | 2 | 32 | 64, 16, 64, 64 | 128,
                     512, 1 | 256, 1 | 1024, 1 | 2048, 1 | 2 | 16 | 32 | 64 | 512 | 1024, 0xFFF];
    for &f in &flag_sets {
        for sized in [false, true] {
            specs.push(pipelines::spec(f, [0, 0], 0, sized, true));
        }
    }
    for u in 0..4 {
        for v in 0..4 {
            specs.push(pipelines::spec(1 | 32, [u, v], 0, true, true));
        }
    }
    for atst in 0..8 {
        specs.push(pipelines::spec(1 | 2 | 64, [1, 1], atst, false, false));
    }
    specs.sort_unstable();
    specs.dedup();
    let fragments = [
        (Native, "native_pixel"),
        (Native, "native_pixel_blend"),
        (Native, "native_pixel_u"),
        (Native, "native_pixel_blend_u"),
        (Dual, "native_pixel_dual"),
        (Dual, "native_pixel_dual_u"),
    ];
    for (source, entry) in fragments {
        for scale in [0u32, 1, 2, 4, 8] {
            for &spec in &specs {
                // Every scale with the generic shader; the specialisations at scales 0 and 2.
                if spec != 0 && scale != 0 && scale != 2 {
                    continue;
                }
                let mut constants = Vec::new();
                if scale != 0 {
                    constants.push(("native_texel_scale", scale as f64));
                }
                if spec != 0 {
                    constants.push(("native_spec", spec as f64));
                }
                out.push(Stage { source, stage: Fragment, entry, constants, groups: TWO });
            }
        }
    }
    out
}

fn parse(source: &str) -> (naga::Module, naga::valid::ModuleInfo) {
    let module = naga::front::wgsl::parse_str(source).unwrap_or_else(|e| panic!("{}", e.emit_to_string(source)));
    let info = naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
        .validate(&module)
        .unwrap_or_else(|e| panic!("{}", e.emit_to_string(source)));
    (module, info)
}

/// The SPIR-V words of one stage, as compile_stage makes them.
fn spirv(module: &naga::Module, info: &naga::valid::ModuleInfo, s: &Stage, device: Device) -> Vec<u32> {
    let mut constants = naga::back::PipelineConstants::default();
    for (k, v) in &s.constants {
        constants.insert(k.to_string(), *v);
    }
    let (module, info) = naga::back::pipeline_constants::process_overrides(module, info, Some((s.stage, s.entry)), &constants)
        .unwrap_or_else(|e| panic!("{}: overrides: {e}", s.file_name()));
    let pipeline = spv::PipelineOptions { shader_stage: s.stage, entry_point: s.entry.to_string() };
    spv::write_vec(&module, &info, &options(device, binding_map(s.groups)), Some(&pipeline))
        .unwrap_or_else(|e| panic!("{}: SPIR-V: {e}", s.file_name()))
}

/// The function parameters of `words` that are an image or a sampler, or a pointer to one
/// (UniformConstant): what naga writes for a texture passed to a WGSL function.
fn handle_parameters(words: &[u32]) -> usize {
    const TYPE_IMAGE: u32 = 25;
    const TYPE_SAMPLER: u32 = 26;
    const TYPE_SAMPLED_IMAGE: u32 = 27;
    const TYPE_POINTER: u32 = 32;
    const FUNCTION_PARAMETER: u32 = 55;
    const UNIFORM_CONSTANT: u32 = 0;
    let mut handles = HashSet::new();
    let mut count = 0;
    let mut i = 5; // after the header
    while i < words.len() {
        let (op, len) = (words[i] & 0xFFFF, (words[i] >> 16) as usize);
        assert!(len > 0 && i + len <= words.len(), "SPIR-V instruction at word {i}: length {len}");
        let operand = |k: usize| words[i + k];
        match op {
            TYPE_IMAGE | TYPE_SAMPLER | TYPE_SAMPLED_IMAGE => {
                handles.insert(operand(1));
            }
            TYPE_POINTER if operand(2) == UNIFORM_CONSTANT => {
                handles.insert(operand(1));
            }
            FUNCTION_PARAMETER if handles.contains(&operand(1)) => count += 1,
            _ => {}
        }
        i += len;
    }
    count
}

fn words_to_bytes(words: &[u32]) -> Vec<u8> {
    words.iter().flat_map(|w| w.to_le_bytes()).collect()
}

/// malioc on one file (the stage from its name): None when it compiled, else what it said.
fn malioc(malioc: &Path, core: &str, file: &Path) -> Option<String> {
    let out = std::process::Command::new(malioc)
        .args(["--core", core, "--vulkan"])
        .arg(file)
        .output()
        .unwrap_or_else(|e| panic!("{}: {e}", malioc.display()));
    let text = format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
    if out.status.success() && !text.contains("ERROR") {
        None
    } else {
        Some(format!("exit {:?}\n{text}", out.status.code()))
    }
}

#[test]
fn native_spirv() {
    let read = |var: &str, default: &str| match std::env::var_os(var) {
        Some(path) => std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", PathBuf::from(&path).display())),
        None => default.to_string(),
    };
    let other = std::env::var_os("DHD_NATIVE_WGSL").is_some();
    let native = read("DHD_NATIVE_WGSL", NATIVE);
    let dual_source = format!("enable dual_source_blending;\n{native}{}", read("DHD_NATIVE_DUAL_WGSL", NATIVE_DUAL));
    let plain = parse(&native);
    let dual = parse(&dual_source);
    let present = parse(PRESENT);

    let out_dir = std::env::var_os("DHD_SPIRV_OUT").map(PathBuf::from);
    let malioc_path = std::env::var_os("DHD_MALIOC").map(PathBuf::from);
    let core = std::env::var("DHD_MALIOC_CORE").unwrap_or_else(|_| "Mali-G57".into());
    let dir = out_dir.clone().or_else(|| {
        malioc_path.as_ref().map(|_| {
            let scratch = std::env::var_os("DHD_SCRATCH").map(PathBuf::from).unwrap_or_else(std::env::temp_dir);
            scratch.join(format!("dhd-native-spirv-{}", std::process::id()))
        })
    });

    let mut files = Vec::new();
    let mut with_handles = Vec::new();
    let mut written = 0;
    for device in DEVICES {
        for s in stages() {
            let (module, info) = match s.source {
                Source::Native => &plain,
                Source::Dual => &dual,
                Source::Present => &present,
            };
            let mut s = s;
            if other && s.source != Source::Present {
                if !module.entry_points.iter().any(|e| e.name == s.entry) {
                    continue;
                }
                s.constants.retain(|(k, _)| module.overrides.iter().any(|(_, o)| o.name.as_deref() == Some(*k)));
            }
            let words = spirv(module, info, &s, device);
            assert_eq!(words.first(), Some(&0x0723_0203), "{}: not SPIR-V", s.file_name());
            written += 1;
            let handles = handle_parameters(&words);
            if handles != 0 {
                with_handles.push(format!("{}/{}: {handles}", device.folder(), s.file_name()));
            }
            if let Some(dir) = &dir {
                let dir = dir.join(device.folder());
                std::fs::create_dir_all(&dir).unwrap();
                let file = dir.join(s.file_name());
                std::fs::write(&file, words_to_bytes(&words)).unwrap();
                files.push(file);
            }
        }
    }
    eprintln!("native_spirv: {written} stages written");

    let mut failures = Vec::new();
    if let Some(malioc_path) = &malioc_path {
        let found = std::sync::Mutex::new(Vec::new());
        let next = std::sync::atomic::AtomicUsize::new(0);
        std::thread::scope(|scope| {
            for _ in 0..4 {
                scope.spawn(|| loop {
                    let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    let Some(file) = files.get(i) else { break };
                    if let Some(report) = malioc(malioc_path, &core, file) {
                        found.lock().unwrap().push(format!("{}: {report}", file.display()));
                    }
                });
            }
        });
        failures = found.into_inner().unwrap();
        eprintln!("native_spirv: malioc --core {core}: {} of {} stages compiled", files.len() - failures.len(), files.len());
        if out_dir.is_none() {
            if let Some(dir) = &dir {
                let _ = std::fs::remove_dir_all(dir);
            }
        }
    }
    assert!(with_handles.is_empty(), "stages that pass a texture or sampler into a function (Mali r54 crashes on them):\n{}",
            with_handles.join("\n"));
    assert!(failures.is_empty(), "{} of {} stages do not compile for {core}:\n{}", failures.len(), files.len(),
            failures.join("\n"));
}
