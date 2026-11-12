//! DX12 with FXC: pipelines made on several threads at once from one shader module (finding 223).
//!
//! wgpu-hal's DX12 shader cache hands each pipeline a clone of the compiled shader it keeps. With
//! FXC that was d3dcompiler_47's `ID3DBlob`, whose reference count is not atomic: one thread's
//! AddRef (cache hit, under the cache's lock) against another's Release (its pipeline made, outside
//! the lock) lost updates, so the blob was freed while the cache still held it and the next cache
//! hit called AddRef on freed memory (the 0.8.0 gate's crash in `Device::load_shader`;
//! gfx-rs/wgpu#10547). The renderer's background pipeline worker and its render thread do exactly
//! this. `vendor/wgpu-hal` keeps copies of the bytecode instead (PATCH.md); this test makes many
//! pipelines from one module on several threads and must end without a crash or an error.
//!
//! `DHDR_STRESS_SECS` (default 3) and `DHDR_STRESS_THREADS` (default 8) make it longer or wider.
#![cfg(all(feature = "native", windows, target_arch = "x86_64"))]

mod common;
use common::{Adapter, Compiler};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

const SHADER: &str = "
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
    return vec4<f32>(f32(i & 1u), f32(i >> 1u), 0.0, 1.0);
}
@fragment fn fs() -> @location(0) vec4<f32> { return vec4<f32>(1.0, 0.5, 0.25, 1.0); }
";

fn env(name: &str, default: u64) -> u64 {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

/// Pipelines from one module on `threads` threads for `secs` seconds; how many were made.
fn stress(adapter: Adapter, compiler: Compiler, secs: u64, threads: u64) -> u64 {
    let gpu = common::gpu(adapter, compiler);
    let device = &gpu.device;
    let module = device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("dx12 shader cache stress"),
        source: wgpu::ShaderSource::Wgsl(SHADER.into()),
    });
    let made = AtomicU64::new(0);
    let end = Instant::now() + Duration::from_secs(secs);
    let validation = device.push_error_scope(wgpu::ErrorFilter::Validation);
    let internal = device.push_error_scope(wgpu::ErrorFilter::Internal);
    std::thread::scope(|s| {
        // Small heap blocks freed and taken again all the time, as the game's own allocations do:
        // a blob freed too early is soon someone else's memory, and the next AddRef through its
        // overwritten vtable faults (in the gate it read a fresh Arc's count of 1).
        s.spawn(move || {
            let mut keep: Vec<Vec<u64>> = (0..4096).map(|_| Vec::new()).collect();
            let mut x = 0x9E37_79B9_7F4A_7C15u64;
            while Instant::now() < end {
                for _ in 0..4096 {
                    x ^= x << 13;
                    x ^= x >> 7;
                    x ^= x << 17;
                    keep[(x % 4096) as usize] = vec![1; 2 + (x >> 32) as usize % 10];
                }
            }
        });
        for t in 0..threads {
            let (module, made) = (&module, &made);
            s.spawn(move || {
                // Two target formats per thread: distinct pipeline states, the same two shaders
                // (each stage's FXC output comes from the cache after the first build).
                let formats = [wgpu::TextureFormat::Rgba8Unorm, wgpu::TextureFormat::Bgra8Unorm];
                let mut n = t as usize;
                while Instant::now() < end {
                    for _ in 0..16 {
                        let targets = [Some(wgpu::ColorTargetState::from(formats[n & 1]))];
                        n += 1;
                        drop(device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                            label: Some("stress"),
                            layout: None,
                            vertex: wgpu::VertexState {
                                module, entry_point: Some("vs"), compilation_options: Default::default(), buffers: &[],
                            },
                            primitive: Default::default(),
                            depth_stencil: None,
                            multisample: Default::default(),
                            fragment: Some(wgpu::FragmentState {
                                module, entry_point: Some("fs"), compilation_options: Default::default(),
                                targets: &targets,
                            }),
                            multiview_mask: None,
                            cache: None,
                        }));
                        made.fetch_add(1, Ordering::Relaxed);
                    }
                }
            });
        }
    });
    let i = pollster::block_on(internal.pop());
    let v = pollster::block_on(validation.pop());
    assert!(i.is_none(), "internal error: {i:?}");
    assert!(v.is_none(), "validation error: {v:?}");
    let n = made.load(Ordering::Relaxed);
    eprintln!("{adapter:?} {compiler:?}: {n} render pipelines on {threads} threads in {secs} s, one shader module");
    n
}

#[test]
fn fxc_pipelines_on_many_threads() {
    let n = stress(Adapter::Hardware, Compiler::Fxc, env("DHDR_STRESS_SECS", 3), env("DHDR_STRESS_THREADS", 8));
    assert!(n > 0);
}

#[test]
fn dxc_pipelines_on_many_threads() {
    let n = stress(Adapter::Hardware, Compiler::Dxc, env("DHDR_STRESS_SECS", 3), env("DHDR_STRESS_THREADS", 8));
    assert!(n > 0);
}
