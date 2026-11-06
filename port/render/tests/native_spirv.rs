//! The renderer's SPIR-V as wgpu-hal 29.0.3's Vulkan backend makes it, for every pipeline stage
//! the native and the exact renderer build (findings 153, 170).
//!
//!     python port/tools/rustgs.py test --target x86_64-pc-windows-msvc -- --test native_spirv
//!
//! wgpu-hal compiles a WGSL module per pipeline stage (`vulkan/device.rs` compile_stage; it sets
//! SEPARATE_ENTRY_POINTS on every device): `process_overrides` keeps the one entry point, compacts
//! the module and applies the override constants, then `spv::write_vec` writes it with the
//! device's options (`vulkan/adapter.rs`) and the pipeline layout's binding map. This test does the
//! same for each entry point and constant set the renderer uses (init.rs, pipelines.rs,
//! exact/pipelines.rs), for present.wgsl's render probe (the pipeline that compiles on the Mali
//! phones) and raster.wgsl's stages for every raster key, and checks that each stage writes, passes
//! no image or sampler into a function (the construct Mali's r54 compiler crashes on, finding 153;
//! `shaders.rs` checks the WGSL of every module for it) and has none of the forms LLVM 9 turns into
//! an intrinsic PowerVR Rogue's driver 1.13 cannot lower (`llvm_saturating_forms`, finding 170).
//!
//! Environment:
//! - `DHD_SPIRV_OUT`: a folder to keep each stage's SPIR-V in, `<device>/<entry>[-<constants>].<stage>.spv`.
//! - `DHD_MALIOC`: Arm's Mali Offline Compiler (malioc, from Arm Performance Studio; docs/TESTING.md):
//!   every stage is compiled by it too, for `DHD_MALIOC_CORE` (default Mali-G57), and the test
//!   fails when one does not compile. Without `DHD_SPIRV_OUT` the files go to `DHD_SCRATCH` (else
//!   the system temp) and are removed after.
//! - `DHD_NATIVE_WGSL` (and `DHD_NATIVE_DUAL_WGSL`): another native.wgsl (native_dual.wgsl) in its
//!   place, an older version or a candidate change, to compare or bisect; the entry points and
//!   constants it lacks are left out. `DHD_RASTER_WGSL`: another raster.wgsl in its place.
#![cfg(feature = "native")]

mod common;
use dhd_render::native::pipelines;
use dhd_render::shaders::{NATIVE, NATIVE_DUAL, PRESENT, RASTER};
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

/// A pipeline layout's binding map: `groups[g]` the WGSL bindings of group g in the layout's
/// entry order (wgpu-hal numbers a set's Vulkan bindings in entry order, from 0; wgpu-core sorts a
/// derived layout's entries by binding).
fn binding_map(groups: &[&[u32]]) -> spv::BindingMap {
    let mut map = spv::BindingMap::new();
    for (group, bindings) in groups.iter().enumerate() {
        let group = group as u32;
        for (index, &binding) in bindings.iter().enumerate() {
            map.insert(naga::ResourceBinding { group, binding },
                       spv::BindingInfo { descriptor_set: group, binding: index as u32, binding_array_size: None });
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
    /// raster.wgsl, the exact renderer's compute module (exact/pipelines.rs)
    Raster,
}

/// One pipeline stage: its module, entry point, override constants and layout (bindings per group).
#[derive(Clone, Debug)]
struct Stage {
    source: Source,
    stage: naga::ShaderStage,
    entry: &'static str,
    constants: Vec<(&'static str, f64)>,
    groups: &'static [&'static [u32]],
}

/// The native layouts: group 0 the uniform arena, group 1 two entries (draw, texels, blit,
/// scanout), one (grow) or eleven (convert).
const ONE: &[u32] = &[0];
const PAIR: &[u32] = &[0, 1];
const TWO: &[&[u32]] = &[ONE, PAIR];

/// raster.wgsl's layouts (exact/pipelines.rs): main's explicit one (`MAIN_BINDINGS`), and the
/// derived ones of scanout (VRAM, display_state, screen) and local_copy (memory, copy_state).
const RASTER_MAIN: &[&[u32]] = &[&[0, 1, 2, 3, 4, 10]];
const RASTER_SCANOUT: &[&[u32]] = &[&[1, 5, 7]];
const RASTER_LOCAL_COPY: &[&[u32]] = &[&[2, 6]];

impl Stage {
    fn new(source: Source, stage: naga::ShaderStage, entry: &'static str, groups: &'static [&'static [u32]]) -> Stage {
        Stage { source, stage, entry, constants: Vec::new(), groups }
    }

    fn file_name(&self) -> String {
        let mut name = self.entry.to_string();
        for (k, v) in &self.constants {
            let short = match *k {
                "native_spec" => "spec",
                "native_texel_scale" => "scale",
                "EARLY_DEPTH" => "early",
                "ADDRESS_SHIFT" => "shift",
                _ => "store",
            };
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
/// spread of `native_texel_scale` and `native_spec` values (pipelines.rs `build`, `spec`),
/// present.wgsl's probe, and the exact renderer's raster.wgsl stages for every raster key (built
/// at start with either renderer: the native one draws, the exact one answers the Z probe).
fn stages() -> Vec<Stage> {
    use naga::ShaderStage::{Compute, Fragment, Vertex};
    use Source::{Dual, Native, Present, Raster};
    let mut out = vec![
        Stage::new(Present, Vertex, "present_vertex", &[PAIR]),
        Stage::new(Present, Fragment, "present_fragment", &[PAIR]),
        Stage::new(Native, Vertex, "fullscreen_vertex", TWO),
        Stage::new(Native, Fragment, "native_blit", TWO),
        Stage::new(Native, Fragment, "native_upload_alpha", TWO),
        Stage::new(Native, Fragment, "present_pixel", TWO),
        Stage::new(Native, Fragment, "present_shrink", TWO),
        Stage::new(Native, Fragment, "present_fill", TWO),
        Stage::new(Native, Fragment, "native_grow_depth", &[ONE, ONE]),
        Stage::new(Native, Compute, "native_scanout", TWO),
        Stage::new(Native, Compute, "native_scanout_fast", TWO),
        Stage::new(Native, Compute, "native_convert", &[ONE, &[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]]),
        Stage::new(Native, Vertex, "native_vertex", TWO),
        Stage::new(Raster, Compute, "scanout", RASTER_SCANOUT),
    ];
    // exact/pipelines.rs build_keyed: main takes the three flags of the key, local_copy the shift.
    for key in 0..8u32 {
        use dhd_render::abi::{DHDR_RASTER_ADDRESS_SHIFT, DHDR_RASTER_EARLY_DEPTH, DHDR_RASTER_EXCLUSIVE_WORD_STORE};
        let bit = |b: u32| if key & b != 0 { 1.0 } else { 0.0 };
        let constants = vec![("EARLY_DEPTH", bit(DHDR_RASTER_EARLY_DEPTH)), ("ADDRESS_SHIFT", bit(DHDR_RASTER_ADDRESS_SHIFT)),
                             ("EXCLUSIVE_WORD_STORE", bit(DHDR_RASTER_EXCLUSIVE_WORD_STORE))];
        out.push(Stage { source: Raster, stage: Compute, entry: "main", constants, groups: RASTER_MAIN });
    }
    for shift in [0.0, 1.0] {
        out.push(Stage { source: Raster, stage: Compute, entry: "local_copy", constants: vec![("ADDRESS_SHIFT", shift)],
                         groups: RASTER_LOCAL_COPY });
    }
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

/// The forms PowerVR Rogue's driver 1.13 crashes on (finding 170). It compiles SPIR-V through LLVM
/// 9, whose InstCombine turns an unsigned compare choosing between `a - b` and 0 into
/// `llvm.usub.sat`, one choosing between `a + b` and all ones into `llvm.uadd.sat`
/// (InstCombineSelect.cpp, canonicalizeSaturatedSubtract/Add), and `x << a | x >> b` into a funnel
/// shift (`llvm.fshl`); the driver's code writer lowers none of the three in a compute shader. Found
/// here in the SPIR-V as naga writes it, conservatively: the arithmetic and the compare share their
/// operands, and loads from one variable count as one value (LLVM merges them). Returns what it found.
fn llvm_saturating_forms(words: &[u32]) -> Vec<String> {
    const CONSTANT: u32 = 43;
    const CONSTANT_COMPOSITE: u32 = 44;
    const CONSTANT_NULL: u32 = 46;
    const LOAD: u32 = 61;
    const IADD: u32 = 128;
    const ISUB: u32 = 130;
    const SELECT: u32 = 169;
    const UNSIGNED_COMPARES: [u32; 4] = [172, 174, 176, 178]; // UGreaterThan(Equal), ULessThan(Equal)
    const SHIFT_RIGHT_LOGICAL: u32 = 194;
    const SHIFT_LEFT_LOGICAL: u32 = 196;
    const BITWISE_OR: u32 = 197;
    // Each result id's opcode and operands after the result id.
    let mut defs: std::collections::HashMap<u32, (u32, Vec<u32>)> = std::collections::HashMap::new();
    let mut i = 5;
    while i < words.len() {
        let (op, len) = (words[i] & 0xFFFF, (words[i] >> 16) as usize);
        assert!(len > 0 && i + len <= words.len(), "SPIR-V instruction at word {i}: length {len}");
        if [CONSTANT, CONSTANT_COMPOSITE, CONSTANT_NULL, LOAD, IADD, ISUB, SELECT, SHIFT_RIGHT_LOGICAL, SHIFT_LEFT_LOGICAL,
            BITWISE_OR]
            .contains(&op)
            || UNSIGNED_COMPARES.contains(&op)
        {
            // Result type, result id, operands.
            defs.insert(words[i + 2], (op, words[i + 3..i + len].to_vec()));
        }
        i += len;
    }
    let value = |id: u32| match defs.get(&id) {
        Some((LOAD, operands)) => (LOAD, operands[0]),
        _ => (0, id),
    };
    fn constant(defs: &std::collections::HashMap<u32, (u32, Vec<u32>)>, id: u32, want: u32) -> bool {
        match defs.get(&id) {
            Some((43, operands)) => operands.iter().all(|&w| w == want),
            Some((46, _)) => want == 0,
            Some((44, parts)) => parts.iter().all(|&p| constant(defs, p, want)),
            _ => false,
        }
    }
    let same = |a: &[u32], b: &[u32]| {
        let (a, b) = ([value(a[0]), value(a[1])], [value(b[0]), value(b[1])]);
        (a[0] == b[0] && a[1] == b[1]) || (a[0] == b[1] && a[1] == b[0])
    };
    let mut found = Vec::new();
    let mut ids: Vec<&u32> = defs.keys().collect();
    ids.sort_unstable();
    for &id in ids {
        let (op, operands) = &defs[&id];
        match *op {
            SELECT => {
                let Some((cmp, cmp_operands)) = defs.get(&operands[0]) else { continue };
                if !UNSIGNED_COMPARES.contains(cmp) {
                    continue;
                }
                for (arith, other) in [(operands[1], operands[2]), (operands[2], operands[1])] {
                    match defs.get(&arith) {
                        Some((ISUB, sub)) if constant(&defs, other, 0) && same(sub, cmp_operands) => {
                            found.push(format!("%{id}: a select of a - b and 0 (llvm.usub.sat)"));
                        }
                        Some((IADD, add)) if constant(&defs, other, u32::MAX)
                            && add.iter().any(|&a| cmp_operands.iter().any(|&c| value(a) == value(c))) =>
                        {
                            found.push(format!("%{id}: a select of a + b and all ones (llvm.uadd.sat)"));
                        }
                        _ => {}
                    }
                }
            }
            BITWISE_OR => {
                let shifted = |id: u32, want: u32| match defs.get(&id) {
                    Some((op, shift)) if *op == want => Some(value(shift[0])),
                    _ => None,
                };
                for (a, b) in [(operands[0], operands[1]), (operands[1], operands[0])] {
                    if let (Some(x), Some(y)) = (shifted(a, SHIFT_LEFT_LOGICAL), shifted(b, SHIFT_RIGHT_LOGICAL)) {
                        if x == y {
                            found.push(format!("%{id}: x << a | x >> b (llvm.fshl)"));
                        }
                    }
                }
            }
            _ => {}
        }
    }
    found
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

/// llvm_saturating_forms finds each form in a small module, and not the difference hidden behind
/// an OR with a uniform (raster.wgsl's keep_u).
#[test]
fn llvm_saturating_forms_scan() {
    let source = "
        @group(0) @binding(0) var<storage, read_write> out: array<u32>;
        @group(0) @binding(1) var<uniform> zero: vec4<u32>;
        @compute @workgroup_size(1)
        fn main() {
            var a = out[0];
            let b = out[1];
            out[2] = select(0u, a - b, a > b);
            out[3] = select(a + b, 0xFFFFFFFFu, a > ~b);
            out[4] = (a << b) | (a >> (32u - b));
            out[5] = select(0u, (a - b) | zero.w, a > b);
        }";
    let (module, info) = parse(source);
    let stage = Stage { source: Source::Raster, stage: naga::ShaderStage::Compute, entry: "main", constants: Vec::new(),
                        groups: &[&[0, 1]] };
    let found = llvm_saturating_forms(&spirv(&module, &info, &stage, DEVICES[2]));
    let kinds = ["llvm.usub.sat", "llvm.uadd.sat", "llvm.fshl"];
    assert_eq!(found.len(), 3, "{found:#?}");
    for kind in kinds {
        assert!(found.iter().any(|f| f.contains(kind)), "{kind} not found: {found:#?}");
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
    let raster = parse(&read("DHD_RASTER_WGSL", RASTER));

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
    let mut llvm_forms = Vec::new();
    let mut written = 0;
    for device in DEVICES {
        for s in stages() {
            let (module, info) = match s.source {
                Source::Native => &plain,
                Source::Dual => &dual,
                Source::Present => &present,
                Source::Raster => &raster,
            };
            let mut s = s;
            if other && matches!(s.source, Source::Native | Source::Dual) {
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
            for form in llvm_saturating_forms(&words) {
                llvm_forms.push(format!("{}/{}: {form}", device.folder(), s.file_name()));
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
    assert!(llvm_forms.is_empty(),
            "stages with a form LLVM 9 turns into an intrinsic PowerVR Rogue's driver 1.13 crashes on (finding 170; \
             raster.wgsl's keep_u hides the arithmetic):\n{}",
            llvm_forms.join("\n"));
    assert!(failures.is_empty(), "{} of {} stages do not compile for {core}:\n{}", failures.len(), files.len(),
            failures.join("\n"));
}
