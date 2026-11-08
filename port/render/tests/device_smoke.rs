//! The device, the exact pipelines, the native pipelines and the MXCSR guard on real adapters
//!.
//!
//!     python port/tools/rustgs.py test --target x86_64-pc-windows-msvc -- --test device_smoke exact    (A2)
//!     python port/tools/rustgs.py test --target x86_64-pc-windows-msvc -- --test device_smoke native   (N1)
//!     python port/tools/rustgs.py test --target x86_64-pc-windows-msvc -- --test device_smoke guard    (F0b)
//!
//! - `exact_*` (A2): a device per adapter (DX12 WARP, DX12 hardware, Vulkan hardware) and DX12
//!   compiler (FXC, DXC), with wgpu-native's settings (device.rs), then raster.wgsl's pipelines for
//!   all 8 raster keys, each inside Validation and OutOfMemory scopes. The process's global device
//!   once (latched, idempotent).
//! - `native_*` (N1): the native objects, plain and (where offered) dual-source, and draw pipelines
//!   over every variant, a spread of blend keys, depth states and masks.
//! - `guard_*` (F0b): the MXCSR guard. The caller runs at the EE thread's 0xFFC0 (round toward
//!   zero, FTZ, DAZ); inside the guard MXCSR is IEEE and the caller's comes back after. fold_probe.wgsl
//!   (spike S3) built and run inside the guard gives the IEEE words under FXC and DXC; the
//!   `no-fp-guard` build gives the RTZ words and fails here (the negative control, finding 085).
//!   These need no module of another task: the guard test makes its device itself.
#![cfg(all(feature = "native", windows, target_arch = "x86_64"))]

mod common;
use common::*;
use dhd_render::abi::*;
use dhd_render::device::{self, Gpu};
use dhd_render::exact::pipelines::Pipelines;
use dhd_render::guard::{ieee, mxcsr, set_mxcsr, Guard};

const EE_MXCSR: u32 = 0xFFC0;

// ---------------------------------------------------------------- the device and the exact pipelines

fn check_device(g: &Gpu, adapter: Adapter, compiler: Compiler) {
    assert_eq!(g.options, options(adapter, compiler), "the options kept");
    assert!(!g.name.to_bytes().is_empty(), "an adapter name");
    assert_eq!(g.name.to_bytes(), g.info.name.as_bytes(), "the name is the adapter's");
    match adapter {
        Adapter::Warp => assert_eq!(g.info.device_type, wgpu::DeviceType::Cpu, "WARP is the fallback (CPU) adapter"),
        _ => assert_ne!(g.info.device_type, wgpu::DeviceType::Cpu, "a hardware adapter, not {:?}", g.info.device_type),
    }
    let backend = if adapter == Adapter::Vulkan { wgpu::Backend::Vulkan } else { wgpu::Backend::Dx12 };
    assert_eq!(g.info.backend, backend);
    // wgpu-native's settings: the adapter's own limits, dual-source blending when offered.
    assert_eq!(g.device.limits(), g.adapter.limits(), "the device has the adapter's own limits");
    let offered = g.adapter.features().contains(wgpu::Features::DUAL_SOURCE_BLENDING);
    let has = g.device.features().contains(wgpu::Features::DUAL_SOURCE_BLENDING);
    assert_eq!(has, offered, "dual-source blending exactly when the adapter offers it");
    assert_eq!(g.features & DHDR_FEAT_DUAL_SOURCE != 0, has, "DHDR_FEAT_DUAL_SOURCE");
}

fn exact_pipelines(g: &Gpu, what: &str) {
    for key in 0..8 {
        let p = no_errors(g, &format!("{what} raster key {key}"), || Pipelines::create(g, key))
            .unwrap_or_else(|e| panic!("{what} raster key {key}: {e}"));
        assert_eq!(p.raster_flags, key);
    }
}

fn exact(adapter: Adapter, compiler: Compiler) {
    let g = gpu(adapter, compiler);
    check_device(g, adapter, compiler);
    exact_pipelines(g, &format!("{adapter:?} {compiler:?}"));
}

#[test]
fn exact_warp_fxc() {
    exact(Adapter::Warp, Compiler::Fxc);
}

#[test]
fn exact_warp_dxc() {
    exact(Adapter::Warp, Compiler::Dxc);
}

#[test]
fn exact_hardware_fxc() {
    exact(Adapter::Hardware, Compiler::Fxc);
}

#[test]
fn exact_hardware_dxc() {
    exact(Adapter::Hardware, Compiler::Dxc);
}

#[test]
fn exact_vulkan() {
    exact(Adapter::Vulkan, Compiler::Fxc);
}

#[test]
fn exact_hardware_no_dual_source() {
    let mut o = options(Adapter::Hardware, Compiler::Fxc);
    o.flags |= DHDR_DEV_NO_DUAL_SOURCE;
    let g = Gpu::create(&o).unwrap_or_else(|e| panic!("device: {e}"));
    assert!(!g.device.features().contains(wgpu::Features::DUAL_SOURCE_BLENDING), "dual-source refused");
    assert_eq!(g.features & DHDR_FEAT_DUAL_SOURCE, 0);
}

#[test]
fn exact_device_global_init() {
    assert!(device::gpu().is_none());
    assert_eq!(device::adapter_name().to_bytes(), b"", "no name before init");
    assert_eq!(device::features(), 0);
    let o = options(Adapter::Hardware, Compiler::Fxc);
    assert!(device::init(&o), "the process's device");
    let g = device::gpu().expect("the device after init");
    assert_eq!(device::adapter_name(), g.name.as_c_str());
    assert_eq!(device::features(), g.features);
    // Later calls return at once, whatever they pass.
    let other = options(Adapter::Warp, Compiler::Fxc);
    assert!(device::init(&other));
    assert!(std::ptr::eq(device::gpu().unwrap(), g), "the same device");
    assert_eq!(g.options, o);
}

// ---------------------------------------------------------------- the native pipelines

fn native(adapter: Adapter) {
    use dhd_render::native::init::Objects;
    use dhd_render::native::pipelines::{self, Pipelines};
    let g = gpu(adapter, Compiler::Fxc);
    let mut duals = vec![false];
    if device::has_dual_source(g) {
        duals.push(true);
    }
    // blend keys: off; one/zero add; SA / 1-SA add; constant / one reverse-subtract; DA / 1-DA
    // subtract; one / one min; one / one max (wgpu takes Min and Max with One factors only).
    let blends = [0u32, 1 | 2 << 1 | 1 << 6 | 1 << 11, 1 | 5 << 1 | 6 << 6 | 1 << 11, 1 | 14 << 1 | 2 << 6 | 3 << 11,
                  1 | 7 << 1 | 8 << 6 | 2 << 11, 1 | 2 << 1 | 2 << 6 | 4 << 11, 1 | 2 << 1 | 2 << 6 | 5 << 11];
    // depth: none; attached always no write; gequal + write; greater + write; equal.
    let depths = [0u32, 16 | 1, 16 | 8 | 2, 16 | 8 | 3, 16 | 4];
    for dual in duals {
        let objects = no_errors(g, "native objects", || Objects::create(g, dual))
            .unwrap_or_else(|e| panic!("{adapter:?} native objects (dual {dual}): {e}"));
        assert_eq!(objects.dual, dual);
        let mut cache = Pipelines::new();
        for variant in 0..4u32 {
            if variant == pipelines::VARIANT_DUAL && !dual {
                continue;
            }
            for &blend in &blends {
                for &depth in &depths {
                    for mask in [15u32, 7, 8] {
                        let key = blend | mask << 14 | depth << 18 | variant << 23;
                        assert!(pipelines::describe(key).is_ok(), "key {key:#09x} describes");
                        no_errors(g, &format!("native pipeline {key:#09x}"), || {
                            cache.get(g, &objects, key as u64).map(|_| ())
                        })
                        .unwrap_or_else(|e| panic!("{adapter:?} native pipeline {key:#09x} (dual {dual}): {e}"));
                    }
                }
            }
        }
    }
}

#[test]
fn native_pipelines_hardware() {
    native(Adapter::Hardware);
}

#[test]
fn native_pipelines_warp() {
    native(Adapter::Warp);
}

// ---------------------------------------------------------------- the MXCSR guard

#[test]
fn guard_mxcsr() {
    let before = mxcsr();
    set_mxcsr(EE_MXCSR);
    let (inner, nested, after_nested);
    {
        let _g = Guard::enter("guard_mxcsr");
        inner = mxcsr();
        {
            let _n = Guard::enter("guard_mxcsr nested");
            nested = mxcsr();
        }
        after_nested = mxcsr();
    }
    let after = mxcsr();
    set_mxcsr(before);
    assert_eq!(ieee(EE_MXCSR), 0x1F80, "IEEE of the EE thread's mode");
    assert_eq!(inner & !0x3F, 0x1F80, "inside the guard: IEEE, not the caller's {EE_MXCSR:#06X} (inner {inner:#06X})");
    assert_eq!(nested & !0x3F, 0x1F80, "a nested guard: IEEE");
    assert_eq!(after_nested & !0x3F, 0x1F80, "after a nested guard: the outer guard's IEEE");
    assert_eq!(after & !0x3F, EE_MXCSR & !0x3F, "the caller's MXCSR back after the guard (got {after:#06X})");
    assert_eq!(dhd_render::guard::current_export(), "", "no export named outside the guard");
}

/// A DX12 hardware device with wgpu-native's settings, made here (no device.rs needed).
fn probe_device(compiler: Compiler) -> (wgpu::Device, wgpu::Queue) {
    let shader_compiler = match compiler {
        Compiler::Fxc => wgpu::Dx12Compiler::Fxc,
        Compiler::Dxc => wgpu::Dx12Compiler::DynamicDxc { dxc_path: dxc_path() },
    };
    let mut desc = wgpu::InstanceDescriptor::new_without_display_handle();
    desc.backends = wgpu::Backends::DX12;
    desc.flags = wgpu::InstanceFlags::empty();
    desc.memory_budget_thresholds = wgpu::MemoryBudgetThresholds { for_resource_creation: None, for_device_loss: None };
    desc.backend_options = wgpu::BackendOptions {
        gl: wgpu::GlBackendOptions::default(),
        dx12: wgpu::Dx12BackendOptions { shader_compiler, ..Default::default() },
        noop: wgpu::NoopBackendOptions::default(),
    };
    let instance = wgpu::Instance::new(desc);
    let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::HighPerformance,
        force_fallback_adapter: false,
        compatible_surface: None,
    }))
    .expect("a DX12 adapter");
    pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
        label: None,
        required_features: wgpu::Features::empty(),
        required_limits: adapter.limits(),
        experimental_features: wgpu::ExperimentalFeatures::disabled(),
        memory_hints: wgpu::MemoryHints::default(),
        trace: wgpu::Trace::Off,
    }))
    .expect("a DX12 device")
}

/// fold_probe.wgsl's 8 words: the module and pipeline are built, and the probe run, inside the
/// guard, with the caller at the EE thread's MXCSR.
fn fold_probe(device: &wgpu::Device, queue: &wgpu::Queue) -> ([u32; 8], u32) {
    use wgpu::util::DeviceExt;
    let before = mxcsr();
    set_mxcsr(EE_MXCSR);
    let words = {
        let _g = Guard::enter("guard_fold_probe");
        let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("fold_probe.wgsl"),
            source: wgpu::ShaderSource::Wgsl(include_str!("data/fold_probe.wgsl").into()),
        });
        let pipe = device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
            label: Some("fold"),
            layout: None,
            module: &module,
            entry_point: Some("fold"),
            compilation_options: wgpu::PipelineCompilationOptions { constants: &[], zero_initialize_workgroup_memory: false },
            cache: None,
        });
        let zero = device.create_buffer_init(&wgpu::util::BufferInitDescriptor {
            label: Some("zero"), contents: &[0u8; 16], usage: wgpu::BufferUsages::UNIFORM,
        });
        let out = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("out"), size: 32, usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
            mapped_at_creation: false,
        });
        let read = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("read"), size: 32, usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None,
            layout: &pipe.get_bind_group_layout(0),
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: out.as_entire_binding() },
                       wgpu::BindGroupEntry { binding: 1, resource: zero.as_entire_binding() }],
        });
        let mut e = device.create_command_encoder(&wgpu::CommandEncoderDescriptor::default());
        {
            let mut pass = e.begin_compute_pass(&wgpu::ComputePassDescriptor::default());
            pass.set_pipeline(&pipe);
            pass.set_bind_group(0, &group, &[]);
            pass.dispatch_workgroups(1, 1, 1);
        }
        e.copy_buffer_to_buffer(&out, 0, &read, 0, Some(32));
        queue.submit([e.finish()]);
        read.map_async(wgpu::MapMode::Read, .., |r| r.expect("probe map"));
        device.poll(wgpu::PollType::wait_indefinitely()).expect("poll");
        let view = read.get_mapped_range(..);
        let mut words = [0u32; 8];
        for (i, w) in words.iter_mut().enumerate() {
            *w = u32::from_le_bytes(view[i * 4..i * 4 + 4].try_into().unwrap());
        }
        words
    };
    let after = mxcsr();
    set_mxcsr(before);
    (words, after)
}

fn guard_fold(compiler: Compiler) {
    let (device, queue) = probe_device(compiler);
    let (words, after) = fold_probe(&device, &queue);
    assert_eq!(after & !0x3F, EE_MXCSR & !0x3F, "the caller's MXCSR back after the guard");
    // The IEEE values (round to nearest). Words 2, 5 and 7 are subnormal or run-time-only and
    // are not compared (spike S3).
    let want = [(0, 0x3DCCCCCD, "literal 0.1"), (1, 0x3EAAAAAB, "const 1.0 / 3.0"), (3, 0x3F333333, "literal 0.7"),
                (4, 0x4F800000, "literal 4294967295.0"), (6, 0x3DCCCCCD, "0.1 + uniform 0")];
    let bad: Vec<String> = want.iter().filter(|w| words[w.0] != w.1)
        .map(|w| format!("{} {:08X} (IEEE {:08X})", w.2, words[w.0], w.1)).collect();
    assert!(bad.is_empty(), "{compiler:?}: built under the caller's rounding: {}", bad.join(", "));
}

#[test]
fn guard_fold_probe_fxc() {
    guard_fold(Compiler::Fxc);
}

#[test]
fn guard_fold_probe_dxc() {
    guard_fold(Compiler::Dxc);
}
