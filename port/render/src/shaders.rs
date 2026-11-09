//! The WGSL sources, embedded from `port/gs` as checked out. `.gitattributes` makes
//! `*.wgsl` `eol=lf`, so every checkout embeds the same bytes; a carriage return anywhere stops the
//! build here (the library must be byte-identical from every worktree). The C path embeds the
//! same files (`gs_wgsl.h`).

/// raster.wgsl: the exact renderer (`main`, `local_copy`, `scanout`, `depth_probe`).
pub const RASTER: &str = include_str!("../../gs/raster.wgsl");
/// present.wgsl: the exact present pass (`present_vertex`, `present_fragment`).
pub const PRESENT: &str = include_str!("../../gs/present.wgsl");
/// native.wgsl: the native renderer.
pub const NATIVE: &str = include_str!("../../gs/native.wgsl");
/// native_dual.wgsl: the dual-source blending entry points, appended to native.wgsl for the dual
/// module (`native/init.rs`).
pub const NATIVE_DUAL: &str = include_str!("../../gs/native_dual.wgsl");

const fn has_cr(s: &str) -> bool {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'\r' {
            return true;
        }
        i += 1;
    }
    false
}

const _: () = assert!(!has_cr(RASTER), "port/gs/raster.wgsl has CR line endings (it must be LF)");
const _: () = assert!(!has_cr(PRESENT), "port/gs/present.wgsl has CR line endings (it must be LF)");
const _: () = assert!(!has_cr(NATIVE), "port/gs/native.wgsl has CR line endings (it must be LF)");
const _: () = assert!(!has_cr(NATIVE_DUAL), "port/gs/native_dual.wgsl has CR line endings (it must be LF)");

#[cfg(all(test, feature = "native"))]
mod tests {
    /// Every WGSL module the renderer builds.
    fn modules() -> Vec<(&'static str, String)> {
        vec![
            ("raster.wgsl", super::RASTER.into()),
            ("present.wgsl", super::PRESENT.into()),
            ("native.wgsl", super::NATIVE.into()),
            ("native.wgsl + native_dual.wgsl", format!("enable dual_source_blending;\n{}{}", super::NATIVE, super::NATIVE_DUAL)),
            ("native/surfaces.rs DEPTH_FILL_WGSL", crate::native::surfaces::DEPTH_FILL_WGSL.into()),
            ("native/zread.rs ZREAD_WGSL", crate::native::zread::ZREAD_WGSL.into()),
            ("present/counter.rs SHADER", crate::present::counter::SHADER.into()),
            ("present/touch.rs SHADER", crate::present::touch::SHADER.into()),
        ]
    }

    /// No function takes a texture or a sampler as an argument (finding 153). naga passes one as
    /// a pointer parameter, and Mali's shader compiler on driver r54 crashes on that inside
    /// vkCreate*Pipelines (a null read at 0x40); the Mali Offline Compiler does not show it.
    #[test]
    fn no_texture_arguments() {
        for (name, source) in modules() {
            let module = naga::front::wgsl::parse_str(&source).unwrap_or_else(|e| panic!("{name}: {}", e.emit_to_string(&source)));
            let handle = |ty: naga::Handle<naga::Type>| match module.types[ty].inner {
                naga::TypeInner::Image { .. } | naga::TypeInner::Sampler { .. } => true,
                naga::TypeInner::BindingArray { base, .. } => {
                    matches!(module.types[base].inner, naga::TypeInner::Image { .. } | naga::TypeInner::Sampler { .. })
                }
                _ => false,
            };
            for (_, f) in module.functions.iter() {
                for a in &f.arguments {
                    assert!(!handle(a.ty), "{name}: fn {} takes the texture or sampler `{}`: read the global in a function of its own",
                            f.name.as_deref().unwrap_or("?"), a.name.as_deref().unwrap_or("?"));
                }
            }
        }
    }

    /// No vertex shader reads a texture (or a storage texture): the vendored wgpu-hal synchronises
    /// sampled and storage textures with the fragment and compute stages only (PATCH.md, finding
    /// 174), so a texture read in a vertex shader could race with the texture's last write.
    #[test]
    fn no_vertex_texture_reads() {
        for (name, source) in modules() {
            let module = naga::front::wgsl::parse_str(&source).unwrap_or_else(|e| panic!("{name}: {}", e.emit_to_string(&source)));
            let info = naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
                .validate(&module)
                .unwrap_or_else(|e| panic!("{name}: {e:?}"));
            let image = |ty: naga::Handle<naga::Type>| match module.types[ty].inner {
                naga::TypeInner::Image { .. } => true,
                naga::TypeInner::BindingArray { base, .. } => matches!(module.types[base].inner, naga::TypeInner::Image { .. }),
                _ => false,
            };
            let mut vertex = 0;
            for (i, ep) in module.entry_points.iter().enumerate() {
                if ep.stage != naga::ShaderStage::Vertex {
                    continue;
                }
                vertex += 1;
                let uses = info.get_entry_point(i);
                for (handle, global) in module.global_variables.iter() {
                    assert!(!(image(global.ty) && !uses[handle].is_empty()),
                            "{name}: vertex entry point {} reads the texture `{}`", ep.name, global.name.as_deref().unwrap_or("?"));
                }
            }
            // The check sees the vertex stages it is meant for (raster.wgsl and zread have none).
            if name.contains("touch") || name.contains("counter") || name.starts_with("native.wgsl") || name.starts_with("present") {
                assert!(vertex > 0, "{name}: no vertex entry point found");
            }
        }
    }

    /// The check above catches a vertex shader that reads a texture.
    #[test]
    fn vertex_texture_reads_are_found() {
        let source = "@group(0) @binding(0) var t: texture_2d<f32>;
            @vertex fn v() -> @builtin(position) vec4<f32> { return textureLoad(t, vec2<i32>(0), 0); }";
        let module = naga::front::wgsl::parse_str(source).unwrap();
        let info = naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
            .validate(&module)
            .unwrap();
        let (handle, _) = module.global_variables.iter().next().unwrap();
        assert!(!info.get_entry_point(0)[handle].is_empty());
    }
}
