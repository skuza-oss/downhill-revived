//! The native pipeline keys against the C code's tables (task N1's referee).
//!
//!     python port/tools/rustgs.py test --target x86_64-pc-windows-msvc -- --test native_keys
//!
//! `tests/data/native_keys.json` is `gen/gen_native_tables.c` run over `port/gs/native_keys.inc`
//! (gpu_wgpu_native.inc's native_pipeline; all three retired, last at render/rust 3882d5576: the
//! JSON stays as they recorded it), capturing what the C code hands wgpu-native v29.0.1.1
//! for every key: the D3D11 factor and operation numbers as webgpu.h numbers, all 16384 blend keys
//! (plain and dual), the 32 depth states, the 16 write masks and the 4 variants. The webgpu.h
//! numbers are mapped to wgpu's values here by explicit tables, as wgpu-native maps them; an
//! operation of 6 or 7 is one wgpu-native refuses (it panics), so `describe` must refuse it. No
//! device is needed.
#![cfg(feature = "native")]

mod common;
use common::*;
use dhd_render::native::pipelines::{self, blend_factor, blend_operation, describe, PipelineDesc};
use wgpu::{BlendComponent, BlendFactor as F, BlendOperation as O, BlendState, CompareFunction as C};

/// WGPUBlendFactor (webgpu.h v29) as wgpu-native maps it.
fn factor(n: u64) -> F {
    match n {
        1 => F::Zero,
        2 => F::One,
        3 => F::Src,
        4 => F::OneMinusSrc,
        5 => F::SrcAlpha,
        6 => F::OneMinusSrcAlpha,
        7 => F::Dst,
        8 => F::OneMinusDst,
        9 => F::DstAlpha,
        10 => F::OneMinusDstAlpha,
        11 => F::SrcAlphaSaturated,
        12 => F::Constant,
        13 => F::OneMinusConstant,
        14 => F::Src1,
        15 => F::OneMinusSrc1,
        16 => F::Src1Alpha,
        17 => F::OneMinusSrc1Alpha,
        _ => panic!("WGPUBlendFactor {n} is not one wgpu-native takes"),
    }
}

/// WGPUBlendOperation (the C code casts the D3D11 number): Undefined (0) is Add; 6 and 7 are not
/// operations, wgpu-native panics on them.
fn operation(n: u64) -> Option<O> {
    match n {
        0 | 1 => Some(O::Add),
        2 => Some(O::Subtract),
        3 => Some(O::ReverseSubtract),
        4 => Some(O::Min),
        5 => Some(O::Max),
        _ => None,
    }
}

/// WGPUCompareFunction.
fn compare(n: u64) -> C {
    match n {
        1 => C::Never,
        2 => C::Less,
        3 => C::Equal,
        4 => C::LessEqual,
        5 => C::Greater,
        6 => C::NotEqual,
        7 => C::GreaterEqual,
        8 => C::Always,
        _ => panic!("WGPUCompareFunction {n} is not one wgpu-native takes"),
    }
}

fn ok(key: u32) -> PipelineDesc {
    describe(key).unwrap_or_else(|e| panic!("key {key:#09x}: {e}"))
}

/// The recorded table's factor for a D3D11 number, with the one the C code never used and the
/// Rust code has since added: D3D11_BLEND_DEST_COLOR (9), recorded as One, is Dst (a blend of
/// Cd * As + Cd, finding 276).
fn expected(d3d: u32, n: u64) -> F {
    if d3d == 9 { F::Dst } else { factor(n) }
}

fn blend(key: u32, src: u64, dst: u64, op: O) -> Option<BlendState> {
    Some(BlendState { color: BlendComponent { src_factor: expected(key >> 1 & 31, src),
                                              dst_factor: expected(key >> 6 & 31, dst), operation: op },
                      alpha: BlendComponent::REPLACE })
}

#[test]
fn factors() {
    let data = data("native_keys.json");
    let factors = data.get("factors").arr();
    assert_eq!(factors.len(), 32);
    for (d3d, f) in factors.iter().enumerate() {
        assert_eq!(f.get("d3d").u(), d3d as u64);
        let d3d = d3d as u32;
        assert_eq!(blend_factor(d3d, false), expected(d3d, f.get("plain").u()), "D3D11 factor {d3d}, plain");
        assert_eq!(blend_factor(d3d, true), expected(d3d, f.get("dual").u()), "D3D11 factor {d3d}, dual");
    }
}

#[test]
fn operations() {
    let data = data("native_keys.json");
    let ops = data.get("operations").arr();
    assert_eq!(ops.len(), 8);
    for (d3d, o) in ops.iter().enumerate() {
        assert_eq!(o.get("d3d").u(), d3d as u64);
        assert_eq!(blend_operation(d3d as u32), operation(o.get("wgpu").u()), "D3D11 operation {d3d}");
    }
}

#[test]
fn blend_keys() {
    let data = data("native_keys.json");
    let fields: Vec<&str> = data.get("blend_fields").arr().iter().map(|f| f.str()).collect();
    assert_eq!(fields, ["key", "operation", "src", "dst", "src_dual", "dst_dual"]);
    let keys = data.get("blend_keys").arr();
    assert_eq!(keys.len(), 8192);
    for row in keys {
        let r: Vec<u64> = row.arr().iter().map(|v| v.u()).collect();
        let k = r[0] as u32;
        assert_eq!(k & 1, 1, "enabled keys only");
        let (plain, dual) = (k | 15 << 14, k | 15 << 14 | pipelines::VARIANT_DUAL << 23);
        match operation(r[1]) {
            None => {
                assert!(describe(plain).is_err(), "key {plain:#09x}: operation {} is refused", r[1]);
                assert!(describe(dual).is_err(), "key {dual:#09x}: operation {} is refused", r[1]);
            }
            Some(op) => {
                assert_eq!(ok(plain).color.blend, blend(k, r[2], r[3], op), "key {plain:#09x}");
                assert_eq!(ok(dual).color.blend, blend(k, r[4], r[5], op), "key {dual:#09x} (dual)");
            }
        }
        // The same key with blending off: no blend state, whatever its fields (C ignores them).
        assert_eq!(ok((k & !1) | 15 << 14).color.blend, None, "key {:#09x}", (k & !1) | 15 << 14);
    }
}

#[test]
fn depth_states() {
    let data = data("native_keys.json");
    let fields: Vec<&str> = data.get("depth_fields").arr().iter().map(|f| f.str()).collect();
    assert_eq!(fields, ["depth", "attached", "write", "compare"]);
    let states = data.get("depth_states").arr();
    assert_eq!(states.len(), 32);
    for row in states {
        let r: Vec<u64> = row.arr().iter().map(|v| v.u()).collect();
        let key = 1 | 15 << 14 | (r[0] as u32) << 18;
        let want = (r[1] != 0).then(|| wgpu::DepthStencilState {
            format: wgpu::TextureFormat::Depth32Float,
            depth_write_enabled: Some(r[2] != 0),
            depth_compare: Some(compare(r[3])),
            stencil: wgpu::StencilState {
                front: wgpu::StencilFaceState::IGNORE,
                back: wgpu::StencilFaceState::IGNORE,
                read_mask: 0,
                write_mask: 0,
            },
            bias: wgpu::DepthBiasState::default(),
        });
        assert_eq!(ok(key).depth, want, "depth {}", r[0]);
    }
}

#[test]
fn write_masks() {
    let data = data("native_keys.json");
    let masks = data.get("write_masks").arr();
    assert_eq!(masks.len(), 16);
    for m in masks {
        let key = 1 | (m.get("mask").u() as u32) << 14;
        let want = wgpu::ColorWrites::from_bits(m.get("wgpu").u() as u32).expect("WGPUColorWriteMask bits");
        let d = ok(key);
        assert_eq!(d.color.write_mask, want, "mask {}", m.get("mask").u());
        assert_eq!(d.color.format, wgpu::TextureFormat::Rgba8Unorm);
    }
}

#[test]
fn variants() {
    let data = data("native_keys.json");
    let variants = data.get("variants").arr();
    assert_eq!(variants.len(), 4);
    let names = [pipelines::VARIANT_PLAIN, pipelines::VARIANT_DUAL, pipelines::VARIANT_RGB_A128, pipelines::VARIANT_ALPHA_ONLY];
    for (v, want) in variants.iter().enumerate() {
        assert_eq!(want.get("variant").u(), v as u64);
        assert_eq!(names[v], v as u32);
        let key = 1 | 5 << 1 | 6 << 6 | 1 << 11 | 15 << 14 | (v as u32) << 23;
        let d = ok(key);
        assert_eq!(d.entry, want.get("entry").str(), "variant {v}: entry");
        assert_eq!(d.dual_module, want.get("module").str() == "dual", "variant {v}: module");
        assert_ne!(want.get("module").str(), "other");
        assert_eq!(d.color.blend.is_some(), want.get("blend").u() != 0, "variant {v}: blend state");
        assert_eq!(d.color.format, wgpu::TextureFormat::Rgba8Unorm);
    }
}
