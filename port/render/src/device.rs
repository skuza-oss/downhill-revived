//! The device: one per process, never released (task A2).
//!
//! Settings: wgpu-native v29.0.1.1's as the retired C renderer's `port/gs/gpu_wgpu.c` drove it
//! (last at render/rust 3882d5576; the line numbers below are that file's), written out explicitly
//! (finding 085 question 1):
//! - InstanceFlags empty, no `with_env()`; DEBUG | VALIDATION only for `DHDR_DEV_DEBUG` /
//!   `DHDR_DEV_VALIDATION`. Backends (`backends`): on Windows DX12, or VULKAN for
//!   `DHDR_DEV_VULKAN`; METAL on Apple platforms; VULKAN elsewhere (Linux, Android).
//! - DX12 compiler: FXC for `DHDR_DEV_FXC` or no `dxc_path`, else `DynamicDxc { dxc_path }`; every
//!   other DX12 option default. No memory budget thresholds.
//! - Adapter: `DHDR_DEV_FALLBACK` = the fallback (WARP) adapter; else the enumerated adapter with
//!   the PCI ids (not a CPU one), else high performance.
//! - Device: the adapter's own limits; DUAL_SOURCE_BLENDING when offered and not refused
//!   (`DHDR_DEV_NO_DUAL_SOURCE`); default memory hints; trace off.
//! - `guard::install_error_handler` right after creation; `log::set_level(log_level)`.
//! - `DHDR_FEAT_RENDER_PIPELINES`: present.wgsl's pipeline (an RGBA8 target) built inside a
//!   Validation scope, as the probe.
//!
//! The retired C path's device_init: `port/gs/gpu_wgpu.c:122-218`. The C side now is
//! `port/gs/gpu_rust.c`'s device_init, which only turns the DHD_WGPU_* variables into flags.
use crate::abi::*;
use std::ffi::{CStr, CString};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};

/// What C asked for (`DhdrDeviceDesc`, checked and copied by `ffi.rs`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DeviceOptions {
    /// `DHDR_DEV_*`
    pub flags: u32,
    /// PCI ids of the presentation adapter; 0, 0: high performance.
    pub vendor_id: u32,
    pub device_id: u32,
    /// `DHDR_LOG_*`
    pub log_level: u32,
    /// dxcompiler.dll, or None (FXC).
    pub dxc_path: Option<String>,
}

/// The device and what was learnt making it. Send + Sync (natively; on wasm through wgpu's
/// `fragile-send-sync-non-atomic-wasm`), so it lives in a global.
pub struct Gpu {
    pub instance: wgpu::Instance,
    pub adapter: wgpu::Adapter,
    pub device: wgpu::Device,
    pub queue: wgpu::Queue,
    pub info: wgpu::AdapterInfo,
    /// The adapter's name, NUL-terminated (`dhdr_adapter_name`).
    pub name: CString,
    /// `DHDR_FEAT_*`
    pub features: u32,
    pub options: DeviceOptions,
    /// The native renderer's shared objects, made on first use from any thread (`native/shared.rs`).
    pub shared: OnceLock<crate::native::shared::NativeShared>,
}

impl Gpu {
    /// Makes a device as `o` asks (natively; blocks). Not for wasm: there `web/init.rs` makes the
    /// device asynchronously and `install`s it.
    pub fn create(o: &DeviceOptions) -> Result<Gpu, String> {
        #[cfg(all(feature = "native", not(target_arch = "wasm32")))]
        {
            native::create(o)
        }
        #[cfg(not(all(feature = "native", not(target_arch = "wasm32"))))]
        {
            let _ = o;
            Err("device::Gpu::create: native only (the browser's device comes from web/init.rs)".into())
        }
    }

    /// The `Gpu` of a device already made (natively by `create`; in the browser by the async
    /// init): the name and the features (dual-source as the device has it, RENDER_PIPELINES as
    /// `render_pipelines` says).
    pub fn from_parts(instance: wgpu::Instance, adapter: wgpu::Adapter, device: wgpu::Device, queue: wgpu::Queue,
                      options: DeviceOptions, render_pipelines: bool) -> Gpu {
        let info = adapter.get_info();
        let name = CString::new(info.name.replace('\0', "")).unwrap_or_default();
        let mut features = 0;
        if device.features().contains(wgpu::Features::DUAL_SOURCE_BLENDING) {
            features |= DHDR_FEAT_DUAL_SOURCE;
        }
        if render_pipelines {
            features |= DHDR_FEAT_RENDER_PIPELINES;
        }
        Gpu { instance, adapter, device, queue, info, name, features, options, shared: OnceLock::new() }
    }
}

/// The DX12 compiler `o` asks for: FXC for `DHDR_DEV_FXC` or no dxcompiler.dll, else DXC from it.
pub fn shader_compiler(o: &DeviceOptions) -> wgpu::Dx12Compiler {
    match &o.dxc_path {
        Some(path) if o.flags & DHDR_DEV_FXC == 0 => wgpu::Dx12Compiler::DynamicDxc { dxc_path: path.clone() },
        _ => wgpu::Dx12Compiler::Fxc,
    }
}

/// The instance flags `o` asks for: DEBUG / VALIDATION for `DHDR_DEV_DEBUG` /
/// `DHDR_DEV_VALIDATION` (wgpu-native's `WGPUInstanceFlag_*`; never `with_env()`), and always
/// ALLOW_UNDERLYING_NONCOMPLIANT_ADAPTER: wgpu's Vulkan backend otherwise hides every driver
/// reporting conformance 0.0.0.0, such as Mesa's dzn (Vulkan on Direct3D 12, the GPU in WSL2),
/// and the game then finds no adapter at all. The high-performance choice still puts a discrete
/// GPU before a CPU one (llvmpipe). Only the Vulkan backend reads it.
pub fn instance_flags(o: &DeviceOptions) -> wgpu::InstanceFlags {
    let mut f = wgpu::InstanceFlags::ALLOW_UNDERLYING_NONCOMPLIANT_ADAPTER;
    if o.flags & DHDR_DEV_DEBUG != 0 {
        f |= wgpu::InstanceFlags::DEBUG;
    }
    if o.flags & DHDR_DEV_VALIDATION != 0 {
        f |= wgpu::InstanceFlags::VALIDATION;
    }
    f
}

/// The platform's backend: on Windows DX12, or Vulkan for `DHDR_DEV_VULKAN`; Metal on Apple
/// platforms; Vulkan everywhere else (Linux, Android), whatever the flags say.
pub fn backends(o: &DeviceOptions) -> wgpu::Backends {
    if cfg!(target_vendor = "apple") {
        wgpu::Backends::METAL
    } else if cfg!(windows) && o.flags & DHDR_DEV_VULKAN == 0 {
        wgpu::Backends::DX12
    } else {
        wgpu::Backends::VULKAN
    }
}

/// The probe of `DHDR_FEAT_RENDER_PIPELINES`: present.wgsl's pipeline with an RGBA8 target, built
/// inside a Validation scope. Returns the scope; the caller pops it (None = the probe was built).
pub fn render_probe(device: &wgpu::Device) -> wgpu::ErrorScopeGuard {
    let scope = device.push_error_scope(wgpu::ErrorFilter::Validation);
    let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("present.wgsl (probe)"),
        source: wgpu::ShaderSource::Wgsl(crate::shaders::PRESENT.into()),
    });
    let _pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("render probe"),
        layout: None,
        vertex: wgpu::VertexState {
            module: &module,
            entry_point: Some("present_vertex"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            buffers: &[],
        },
        primitive: wgpu::PrimitiveState::default(),
        depth_stencil: None,
        multisample: wgpu::MultisampleState::default(),
        fragment: Some(wgpu::FragmentState {
            module: &module,
            entry_point: Some("present_fragment"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            targets: &[Some(wgpu::ColorTargetState {
                format: wgpu::TextureFormat::Rgba8Unorm,
                blend: None,
                write_mask: wgpu::ColorWrites::ALL,
            })],
        }),
        multiview_mask: None,
        cache: None,
    });
    scope
}

#[cfg(all(feature = "native", not(target_arch = "wasm32")))]
mod native {
    use super::*;

    fn instance(o: &DeviceOptions, compiler: wgpu::Dx12Compiler) -> wgpu::Instance {
        let mut desc = wgpu::InstanceDescriptor::new_without_display_handle();
        desc.backends = backends(o);
        desc.flags = instance_flags(o);
        desc.memory_budget_thresholds = wgpu::MemoryBudgetThresholds { for_resource_creation: None, for_device_loss: None };
        desc.backend_options = wgpu::BackendOptions {
            gl: wgpu::GlBackendOptions::default(),
            dx12: wgpu::Dx12BackendOptions { shader_compiler: compiler, ..Default::default() },
            noop: wgpu::NoopBackendOptions::default(),
        };
        wgpu::Instance::new(desc)
    }

    /// gpu_wgpu.c:122-218 with wgpu-native's settings (the module comment), except that WARP always
    /// compiles with FXC (`software_fxc`).
    pub fn create(o: &DeviceOptions) -> Result<Gpu, String> {
        crate::log::set_level(o.log_level);
        let backends = backends(o);
        let compiler = shader_compiler(o);
        let mut instance = instance(o, compiler.clone());
        let mut adapter = adapter(&instance, backends, o)?;
        if software_fxc(&compiler, &adapter.get_info()) {
            crate::host::log("dhd_render: shaders for the WARP (software) adapter compiled with FXC, not DXC");
            drop(adapter);
            instance = self::instance(o, wgpu::Dx12Compiler::Fxc);
            adapter = self::adapter(&instance, backends, o)?;
        }
        let info = adapter.get_info();
        // The driver in the log, for bug reports: many draw faults are one driver's.
        crate::host::log(format!("gs: GPU {} on {:?}, driver {} {}", info.name, info.backend, info.driver,
                                 info.driver_info).trim_end());
        log_adapters(&instance, backends, &info);
        // DHD_RSTAT (finding 108): whether the adapter offers GPU timers at all. The 120 field/s
        // round had to estimate GPU time from the render thread's blocked submits because nothing
        // asks for them; this says whether a later round could measure it directly.
        if std::env::var_os("DHD_RSTAT").is_some() {
            let f = adapter.features();
            let has = |x: wgpu::Features| if f.contains(x) { "yes" } else { "no" };
            crate::host::log(&format!(
                "rstat: adapter {} ({:?}) TIMESTAMP_QUERY={} TIMESTAMP_QUERY_INSIDE_ENCODERS={} TIMESTAMP_QUERY_INSIDE_PASSES={}",
                info.name,
                info.backend,
                has(wgpu::Features::TIMESTAMP_QUERY),
                has(wgpu::Features::TIMESTAMP_QUERY_INSIDE_ENCODERS),
                has(wgpu::Features::TIMESTAMP_QUERY_INSIDE_PASSES)
            ));
        }
        let dual = adapter.features().contains(wgpu::Features::DUAL_SOURCE_BLENDING)
            && o.flags & DHDR_DEV_NO_DUAL_SOURCE == 0;
        let timers = wgpu::Features::TIMESTAMP_QUERY | wgpu::Features::TIMESTAMP_QUERY_INSIDE_ENCODERS;
        let timers = if std::env::var_os("DHD_GPU_TIMING").is_some() && adapter.features().contains(timers) {
            timers
        } else { wgpu::Features::empty() };
        let mut features = if dual { wgpu::Features::DUAL_SOURCE_BLENDING } else { wgpu::Features::empty() };
        // DHD_GPU_TIMES (diagnostics: native/pipelines.rs's gputimes), and the perf: lines' GPU time
        // (finding 129), where the adapter has timestamp queries.
        if crate::native::pipelines::gputimes::wanted() || crate::native::pipelines::gputimes::perf_on() {
            features |= adapter.features() & wgpu::Features::TIMESTAMP_QUERY;
        }
        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
            label: None,
            required_features: features | timers,
            // the adapter's own, so a 4 MB bin list always fits (gpu_wgpu.c:195)
            required_limits: adapter.limits(),
            experimental_features: wgpu::ExperimentalFeatures::disabled(),
            memory_hints: wgpu::MemoryHints::default(),
            trace: wgpu::Trace::Off,
        }))
        .map_err(|e| format!("request_device on {} ({:?}): {e}", info.name, info.backend))?;
        crate::guard::install_error_handler(&device);
        let render_pipelines = pollster::block_on(render_probe(&device).pop()).is_none();
        Ok(Gpu::from_parts(instance, adapter, device, queue, o.clone(), render_pipelines))
    }

    /// Every adapter of these backends, of what kind, and the one the game runs on (finding 129):
    /// on a laptop with two GPUs, whether the game took the integrated one.
    fn log_adapters(instance: &wgpu::Instance, backends: wgpu::Backends, picked: &wgpu::AdapterInfo) {
        let all: Vec<wgpu::AdapterInfo> =
            pollster::block_on(instance.enumerate_adapters(backends)).iter().map(|a| a.get_info()).collect();
        let list: Vec<String> = all.iter().map(|i| format!("{} ({})", i.name, kind_name(i.device_type))).collect();
        let discrete = all.iter().any(|i| i.device_type == wgpu::DeviceType::DiscreteGpu);
        let hint = if discrete && picked.device_type != wgpu::DeviceType::DiscreteGpu {
            if cfg!(windows) {
                "; a discrete GPU is there but not used (Windows Settings > System > Display > Graphics: the game, High performance)"
            } else {
                "; a discrete GPU is there but not used"
            }
        } else {
            ""
        };
        crate::host::log(&format!("gs: GPUs: {}; the game runs on {} ({}){hint}", list.join(", "), picked.name,
                                  kind_name(picked.device_type)));
    }

    /// Whether DXC was asked for and the adapter is WARP (a software DX12 adapter: CPU type, or
    /// Microsoft's 0x1414:0x8C). DXIL from DXC takes WARP tens of minutes to compile where FXC's
    /// DXBC takes seconds, on wgpu-native as here (finding 085, gate J1 open item; finding 086,
    /// F1), so the device is made again with FXC. Exact pictures are the same with either
    /// compiler (gate J1). The options kept (`Gpu::options`) stay what C asked for.
    fn software_fxc(compiler: &wgpu::Dx12Compiler, info: &wgpu::AdapterInfo) -> bool {
        matches!(compiler, wgpu::Dx12Compiler::DynamicDxc { .. })
            && info.backend == wgpu::Backend::Dx12
            && (info.device_type == wgpu::DeviceType::Cpu || (info.vendor == 0x1414 && info.device == 0x8C))
    }

    /// gpu_wgpu.c:163-189: the fallback (WARP) adapter for `DHDR_DEV_FALLBACK`; else the
    /// enumerated adapter with the presentation adapter's PCI ids (never a CPU one); else the
    /// high-performance one.
    fn adapter(instance: &wgpu::Instance, backends: wgpu::Backends, o: &DeviceOptions) -> Result<wgpu::Adapter, String> {
        let fallback = o.flags & DHDR_DEV_FALLBACK != 0;
        if !fallback && o.vendor_id != 0 {
            let found = pollster::block_on(instance.enumerate_adapters(backends)).into_iter().find(|a| {
                let i = a.get_info();
                i.vendor == o.vendor_id && i.device == o.device_id && i.device_type != wgpu::DeviceType::Cpu
            });
            if let Some(a) = found {
                return Ok(a);
            }
        }
        pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            force_fallback_adapter: fallback,
            compatible_surface: None,
        }))
        .map_err(|e| format!("no {}adapter ({backends:?}): {e}", if fallback { "fallback " } else { "" }))
    }
}

static GPU: OnceLock<Gpu> = OnceLock::new();
static FAILED: AtomicBool = AtomicBool::new(false);
static INIT: Mutex<()> = Mutex::new(());

/// The process's device (`dhdr_device_init`): true when it exists (later calls return true at once,
/// whatever `o` holds); false when it failed, and every later call returns false too.
pub fn init(o: &DeviceOptions) -> bool {
    if GPU.get().is_some() {
        return true;
    }
    let _lock = INIT.lock().unwrap_or_else(|p| p.into_inner());
    if GPU.get().is_some() {
        return true;
    }
    if FAILED.load(Ordering::Acquire) {
        return false;
    }
    crate::log::set_level(o.log_level);
    match Gpu::create(o) {
        Ok(g) => install(g),
        Err(e) => {
            crate::host::log(&format!("dhd_render: no device: {e}"));
            FAILED.store(true, Ordering::Release);
            false
        }
    }
}

/// Makes `g` the process's device (the browser's async init). False if there is one already.
pub fn install(g: Gpu) -> bool {
    GPU.set(g).is_ok()
}

/// Latches a failed init (the browser's async init failed).
pub fn fail() {
    FAILED.store(true, Ordering::Release);
}

/// The process's device, if made.
pub fn gpu() -> Option<&'static Gpu> {
    GPU.get()
}

/// The adapter's name ("" before init).
pub fn adapter_name() -> &'static CStr {
    GPU.get().map_or(c"", |g| g.name.as_c_str())
}

/// An adapter's kind as the perf: lines say it.
fn kind_name(t: wgpu::DeviceType) -> &'static str {
    match t {
        wgpu::DeviceType::IntegratedGpu => "integrated",
        wgpu::DeviceType::DiscreteGpu => "discrete",
        wgpu::DeviceType::VirtualGpu => "virtual",
        wgpu::DeviceType::Cpu => "cpu (software)",
        wgpu::DeviceType::Other => "other",
    }
}

/// `dhdr_perf_read` (`dhd_render_ext.h`, finding 129): up to `count` values into `out`: [0] the GPU
/// timer (0 off, 1 on, 2 the adapter has none), [1] frames timed, [2] their GPU nanoseconds (the
/// passes' sum), [3] their spans' nanoseconds, [4] the last surface's present mode (0 none, 1
/// immediate, 2 mailbox, 3 fifo, 4 fifo-relaxed), [5] the adapter's kind (0 other, 1 integrated, 2
/// discrete, 3 virtual, 4 cpu). Returns how many it wrote.
#[no_mangle]
pub extern "C" fn dhdr_perf_read(out: *mut u64, count: u32) -> u32 {
    let _g = crate::guard::Guard::enter("dhdr_perf_read");
    if out.is_null() {
        return 0;
    }
    let (frames, ns, span) = crate::native::pipelines::gputimes::totals();
    let kind = GPU.get().map_or(0, |g| match g.info.device_type {
        wgpu::DeviceType::Other => 0,
        wgpu::DeviceType::IntegratedGpu => 1,
        wgpu::DeviceType::DiscreteGpu => 2,
        wgpu::DeviceType::VirtualGpu => 3,
        wgpu::DeviceType::Cpu => 4,
    });
    let values = [
        crate::native::pipelines::gputimes::STATE.load(Ordering::Relaxed) as u64,
        frames,
        ns,
        span,
        crate::present::surface::PRESENT_MODE.load(Ordering::Relaxed) as u64,
        kind,
    ];
    let n = (count as usize).min(values.len());
    // SAFETY: the caller gives room for `count` values.
    unsafe { std::ptr::copy_nonoverlapping(values.as_ptr(), out, n) };
    n as u32
}

/// `DHDR_FEAT_*` (0 before init).
pub fn features() -> u32 {
    GPU.get().map_or(0, |g| g.features)
}

/// `dhdr_device_max_texture` (`dhd_render_ext.h`): the device's largest 2D texture side, 0 before
/// init. The native renderer's scale limit in C (`gpu_rust_shared.inc`'s `native_scale_hold`).
#[no_mangle]
pub extern "C" fn dhdr_device_max_texture() -> u32 {
    let _g = crate::guard::Guard::enter("dhdr_device_max_texture");
    GPU.get().map_or(0, |g| g.device.limits().max_texture_dimension_2d)
}

/// `DHDR_FEAT_DUAL_SOURCE` of `g`.
pub fn has_dual_source(g: &Gpu) -> bool {
    g.features & DHDR_FEAT_DUAL_SOURCE != 0
}
