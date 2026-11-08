//! The native renderer's objects and its create (task N1).
//!
//! As the retired `gpu_wgpu_native.inc`'s native_objects / native_init (last at render/rust
//! 3882d5576): the shader modules (native.wgsl, and
//! for dual-source blending native.wgsl + native_dual.wgsl concatenated), the six bind group
//! layouts, the five pipeline layouts, the fullscreen and compute pipelines, the inverse table.
//! `Native::create` builds them all (inside a Validation error scope: Err on failure) and the
//! empty state of every other part.
//!
//! Not here (they belong to other parts): the samplers and dummy textures (`shared.rs`), the
//! import texture (`surfaces.rs`), the present uniform and filter (`present.rs`). The texture
//! layout is made here as the draw layout's group 1; wgpu deduplicates equal bind group layouts,
//! so `shared.rs`'s equal layout makes groups this layout takes.
//!
//! Referee: `rustgs.py test ... -- --test device_smoke native`.
use crate::abi::DHDR_NATIVE_DUAL;
use crate::device::{self, Gpu};
use crate::native::pipelines::{self, Pipelines};
use crate::native::Native;

/// Runs `f` inside a Validation error scope: Err (naming `what`) when it raised an error. Natively
/// the scope is popped at once (wgpu-core reports synchronously); in the browser a scope could
/// only be awaited, which a C call can't, so errors go to the uncaptured-error handler (fatal).
pub(crate) fn scoped<R>(gpu: &Gpu, what: &str, f: impl FnOnce() -> R) -> Result<R, String> {
    #[cfg(feature = "native")]
    {
        let scope = gpu.device.push_error_scope(wgpu::ErrorFilter::Validation);
        let r = f();
        match pollster::block_on(scope.pop()) {
            Some(e) => Err(format!("{what}: {e}")),
            None => Ok(r),
        }
    }
    #[cfg(not(feature = "native"))]
    {
        let _ = (gpu, what);
        Ok(f())
    }
}

/// wgpu-native's pipeline compilation options: no override constants, workgroup memory not
/// zero-initialised.
pub(crate) fn compilation_options() -> wgpu::PipelineCompilationOptions<'static> {
    wgpu::PipelineCompilationOptions { constants: &[], zero_initialize_workgroup_memory: false }
}

/// The inverse table's length in u32 words (`inverse_table`).
pub const INVERSE_WORDS: usize = 8192;

/// The inverse table's words (native_core.inc native_inverse_table): for the colour (PSMCT32) and
/// Z (PSMZ32) layouts, the word in a page (as gs_pixel_address(psm, 0, 1, x, y) >> 5 & 2047) to
/// the pixel holding it, x | y << 8; colour then Z. Then for the 16-bit colour layout (PSMCT16,
/// 64 x 64 pages), the 16-bit half in a page (gs_pixel_address >> 4 & 4095) to its pixel: what
/// native_convert reads from a 16-bit surface.
pub fn inverse_table() -> [u32; INVERSE_WORDS] {
    let mut inverse = [0u32; INVERSE_WORDS];
    for y in 0..64u32 {
        for x in 0..64u32 {
            // memory.c pixel_address for PSMCT16, bp 0, bw 1, inside page 0.
            let (bx, by) = (x / 16, y / 8);
            let block = (by & 1) | (bx & 1) << 1 | (by & 2) << 1 | (bx & 2) << 2 | (by & 4) << 2;
            let column = (x & 1) << 1 | (x & 2) << 2 | (x & 4) << 2 | (x & 8) >> 3 | (y & 1) << 2 | (y & 6) << 4;
            inverse[(4096 + block * 128 + column) as usize] = x | y << 8;
        }
    }
    for layout in 0..2u32 {
        for y in 0..32u32 {
            for x in 0..64u32 {
                // memory.c pixel_address for the 32-bit formats, bp 0, bw 1, inside page 0.
                let (bx, by) = (x / 8, y / 8);
                let mut block = (bx & 1) | (by & 1) << 1 | (bx & 2) << 1 | (by & 2) << 2 | (bx & 4) << 2;
                let column = (x & 1) | (x & 6) << 1 | (y & 1) << 1 | (y & 6) << 3;
                if layout == 1 {
                    block ^= 0x18; // PSMZ32: Z's block XOR
                }
                let word = (block * 2048 + column * 32) >> 5 & 2047;
                inverse[(layout * 2048 + word) as usize] = x | y << 8;
            }
        }
    }
    inverse
}

pub struct Objects {
    /// Dual-source blending is used (asked for, and the device has it).
    pub dual: bool,
    /// native.wgsl.
    pub module: wgpu::ShaderModule,
    /// `enable dual_source_blending;` + native.wgsl + native_dual.wgsl, when `dual`.
    pub dual_module: Option<wgpu::ShaderModule>,
    /// Group 0 of every pipeline: binding 0, a uniform with a dynamic offset (vertex, fragment,
    /// compute). The arenas' constants bind group and the present uniform's use it.
    pub uniform_layout: wgpu::BindGroupLayout,
    /// The draw's group 1: binding 0 a filterable 2D texture, binding 1 a filtering sampler.
    pub texture_layout: wgpu::BindGroupLayout,
    /// The `_u` draws' group 1 (shared.rs's texels layout): an R32Uint texture, a non-filtering sampler.
    pub texels_layout: wgpu::BindGroupLayout,
    /// The blit's group 1: two unfilterable 2D textures (source, previous).
    pub blit_layout: wgpu::BindGroupLayout,
    /// Scanout's group 1: an unfilterable 2D texture and a write-only RGBA8 storage texture.
    pub scanout_layout: wgpu::BindGroupLayout,
    /// Conversion's group 1: 0 the inverse table (read-only storage), 1 the base (R32Uint), 2-5 unfilterable textures,
    /// 6-9 depth textures, 10 a write-only RGBA8 storage texture.
    pub convert_layout: wgpu::BindGroupLayout,
    /// Depth growth's group 1: a depth texture.
    pub grow_layout: wgpu::BindGroupLayout,
    /// (uniform, texture): draws and the present pass.
    pub draw_layout: wgpu::PipelineLayout,
    /// (uniform, texels): draws from a decoded texture (`_u` entry points).
    pub texels_draw_layout: wgpu::PipelineLayout,
    /// (uniform, blit)
    pub blit_pipeline_layout: wgpu::PipelineLayout,
    /// (uniform, scanout)
    pub scanout_pipeline_layout: wgpu::PipelineLayout,
    /// (uniform, convert)
    pub convert_pipeline_layout: wgpu::PipelineLayout,
    /// (uniform, grow)
    pub grow_pipeline_layout: wgpu::PipelineLayout,
    /// `native_blit` (blit layout)
    pub blit_pipe: wgpu::RenderPipeline,
    /// `native_upload_alpha` (blit layout)
    pub upload_pipe: wgpu::RenderPipeline,
    /// `present_pixel` (draw layout) into RGBA8; `present_pipe` gives it per surface format
    present_rgba: wgpu::RenderPipeline,
    /// the same into BGRA8, made at the first present on a surface without RGBA8 (Apple's)
    present_bgra: std::sync::OnceLock<wgpu::RenderPipeline>,
    /// Widescreen's fill beside a 4:3 picture (present.rs): `present_shrink` (draw layout) into
    /// the small RGBA8 texture, and `present_fill` into RGBA8 or BGRA8; made at the first filled picture
    fill_shrink: std::sync::OnceLock<wgpu::RenderPipeline>,
    fill_rgba: std::sync::OnceLock<wgpu::RenderPipeline>,
    fill_bgra: std::sync::OnceLock<wgpu::RenderPipeline>,
    /// `native_grow_depth` (grow layout; depth only, writes Depth32Float with Always)
    pub grow_pipe: wgpu::RenderPipeline,
    /// `native_scanout` (scanout layout)
    pub scanout_pipe: wgpu::ComputePipeline,
    /// Bounded GS scanout: reciprocal remainder and duplicated-row reuse.
    pub scanout_fast_pipe: wgpu::ComputePipeline,
    /// `native_convert` (convert layout)
    pub convert_pipe: wgpu::ComputePipeline,
    /// The inverse table (`inverse_table`), 32768 bytes, Storage | CopyDst, written at creation.
    pub inverse: wgpu::Buffer,
}

fn module(gpu: &Gpu, source: &str, label: &str) -> wgpu::ShaderModule {
    // The browser compiles WGSL itself (Tint), which wants the shifts in parentheses (below).
    #[cfg(target_arch = "wasm32")]
    let source = &match parenthesize_shifts(source) {
        Ok((fixed, _)) => fixed,
        Err(e) => {
            crate::host::log(&format!("dhd_render: {label}: shifts left as written: {e}"));
            source.to_owned()
        }
    };
    gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some(label),
        source: wgpu::ShaderSource::Wgsl(source.into()),
    })
}

/// native.wgsl for the browser's WGSL compiler (task W1, finding 086), with the number of shifts
/// put in parentheses.
///
/// The WGSL grammar has no precedence between a shift and `&`, `|` or `^`: `a >> 8u & 255u` needs
/// parentheses, and the browsers' compiler (Tint) refuses the module without them. naga, which
/// compiles native.wgsl natively (and on the C path), accepts it with C precedence: a shift's
/// operands are additive expressions, and a shift binds tighter than `&`, `^` and `|`
/// (naga 29.0.3, front/wgsl/parse/mod.rs, the general expression's levels). native.wgsl is locked
/// and written for naga, so the browser is given a copy in which every shift `a << b` / `a >> b`
/// is in parentheses: the meaning naga gives it, spelled out. Nothing else changes. Natively the
/// file goes to naga as it is.
///
/// Each operand is taken as a WGSL unary expression (an identifier, literal, call, parenthesised
/// expression, with `.member` and `[index]` after it and `-`, `!`, `~` before it). Where an
/// operand touches `+ - * / %` the two readings could differ, so that is refused (Err), as is
/// unbalanced nesting. `>>` closing a template (`array<vec4<u32>>`) is not a shift: its left
/// side is a type, or nothing follows that could start an operand.
#[cfg_attr(not(target_arch = "wasm32"), allow(dead_code))] // natively only its tests call it
pub(crate) fn parenthesize_shifts(src: &str) -> Result<(String, usize), String> {
    let b = src.as_bytes();
    // Significant tokens, as byte ranges (spaces and comments dropped).
    let mut tokens: Vec<(usize, usize)> = Vec::new();
    let mut i = 0;
    let word = |c: u8| c.is_ascii_alphanumeric() || c == b'_';
    while i < b.len() {
        let c = b[i];
        let start = i;
        if c.is_ascii_whitespace() {
            i += 1;
            continue;
        }
        if b[i..].starts_with(b"//") {
            while i < b.len() && b[i] != b'\n' {
                i += 1;
            }
            continue;
        }
        if b[i..].starts_with(b"/*") {
            let mut depth = 0; // WGSL block comments nest
            while i < b.len() {
                if b[i..].starts_with(b"/*") {
                    depth += 1;
                    i += 2;
                } else if b[i..].starts_with(b"*/") {
                    depth -= 1;
                    i += 2;
                    if depth == 0 {
                        break;
                    }
                } else {
                    i += 1;
                }
            }
            if depth != 0 {
                return Err("an unterminated block comment".into());
            }
            continue;
        }
        if c.is_ascii_alphabetic() || c == b'_' {
            while i < b.len() && word(b[i]) {
                i += 1;
            }
        } else if c.is_ascii_digit() || (c == b'.' && b.get(i + 1).is_some_and(u8::is_ascii_digit)) {
            let hex = b[i..].starts_with(b"0x") || b[i..].starts_with(b"0X");
            while i < b.len() && (word(b[i]) || b[i] == b'.') {
                if !hex && (b[i] == b'e' || b[i] == b'E') && matches!(b.get(i + 1), Some(b'+' | b'-')) {
                    i += 1;
                }
                i += 1;
            }
        } else {
            const PUNCT: [&[u8]; 21] = [b"<<=", b">>=", b"<<", b">>", b"<=", b">=", b"==", b"!=", b"&&", b"||", b"->",
                                        b"++", b"--", b"+=", b"-=", b"*=", b"/=", b"%=", b"&=", b"|=", b"^="];
            i += PUNCT.iter().find(|p| b[i..].starts_with(p)).map_or(1, |p| p.len());
        }
        tokens.push((start, i));
    }
    let t = |k: isize| -> &str {
        if k < 0 || k as usize >= tokens.len() {
            ""
        } else {
            &src[tokens[k as usize].0..tokens[k as usize].1]
        }
    };
    let ident = |s: &str| s.as_bytes().first().is_some_and(|c| c.is_ascii_alphabetic() || *c == b'_');
    let number = |s: &str| {
        let c = s.as_bytes();
        c.first().is_some_and(u8::is_ascii_digit) || (c.len() > 1 && c[0] == b'.')
    };
    let value_end = |s: &str| s == ")" || s == "]" || ident(s) || number(s);
    let unbalanced = || "unbalanced brackets".to_string();
    // The opener of the closer at k, scanning back (or the closer of the opener at k, forward).
    let back = |mut k: isize, open: &str, close: &str| -> Result<isize, String> {
        let mut depth = 0;
        while k >= 0 {
            if t(k) == close {
                depth += 1;
            } else if t(k) == open {
                depth -= 1;
                if depth == 0 {
                    return Ok(k);
                }
            }
            k -= 1;
        }
        Err(unbalanced())
    };
    let forward = |mut k: isize, open: &str, close: &str| -> Result<isize, String> {
        let mut depth = 0;
        while (k as usize) < tokens.len() {
            if t(k) == open {
                depth += 1;
            } else if t(k) == close {
                depth -= 1;
                if depth == 0 {
                    return Ok(k);
                }
            }
            k += 1;
        }
        Err(unbalanced())
    };
    const TYPES: [&str; 5] = ["u32", "i32", "f32", "f16", "bool"];
    const TEMPLATED: [&str; 8] = ["vec2", "vec3", "vec4", "mat2x2", "mat3x3", "mat4x4", "array", "bitcast"];
    const ARITH: [&str; 5] = ["+", "-", "*", "/", "%"];
    // byte offset -> (closing parentheses, opening parentheses) inserted there
    let mut at: std::collections::BTreeMap<usize, (usize, usize)> = Default::default();
    let mut count = 0;
    for k in 0..tokens.len() as isize {
        if t(k) != "<<" && t(k) != ">>" {
            continue;
        }
        let (left, right) = (t(k - 1), t(k + 1));
        if !value_end(left) || TYPES.contains(&left) {
            continue; // a template's close
        }
        if !(ident(right) || number(right) || ["(", "-", "!", "~"].contains(&right)) {
            continue;
        }
        // the left operand's first token
        let mut a = k - 1;
        loop {
            if t(a) == ")" {
                a = back(a, "(", ")")?;
                if ident(t(a - 1)) {
                    a -= 1;
                } else if t(a - 1) == ">" {
                    a = back(a - 1, "<", ">")? - 1;
                }
            } else if t(a) == "]" {
                a = back(a, "[", "]")? - 1;
                continue;
            }
            if t(a - 1) == "." {
                a -= 2;
                continue;
            }
            break;
        }
        while ["-", "!", "~"].contains(&t(a - 1)) && !value_end(t(a - 2)) {
            a -= 1;
        }
        // the right operand's last token
        let mut z = k + 1;
        while ["-", "!", "~"].contains(&t(z)) {
            z += 1;
        }
        if t(z) == "(" {
            z = forward(z, "(", ")")?;
        } else if ident(t(z)) {
            if TEMPLATED.contains(&t(z)) && t(z + 1) == "<" {
                z = forward(z + 1, "<", ">")?;
            }
            if t(z + 1) == "(" {
                z = forward(z + 1, "(", ")")?;
            }
        }
        loop {
            if t(z + 1) == "[" {
                z = forward(z + 1, "[", "]")?;
            } else if t(z + 1) == "." {
                z += 2;
            } else {
                break;
            }
        }
        if a < 0 || z as usize >= tokens.len() {
            return Err(unbalanced());
        }
        if ARITH.contains(&t(a - 1)) || ARITH.contains(&t(z + 1)) {
            let (from, to) = (tokens[(a - 1).max(0) as usize].0, tokens[((z + 1) as usize).min(tokens.len() - 1)].1);
            return Err(format!("a shift next to arithmetic: `{}`", &src[from..to]));
        }
        at.entry(tokens[a as usize].0).or_default().1 += 1;
        at.entry(tokens[z as usize].1).or_default().0 += 1;
        count += 1;
    }
    let mut out = String::with_capacity(src.len() + 2 * count);
    let mut last = 0;
    for (&pos, &(closes, opens)) in &at {
        out.push_str(&src[last..pos]);
        out.extend(std::iter::repeat_n(')', closes));
        out.extend(std::iter::repeat_n('(', opens));
        last = pos;
    }
    out.push_str(&src[last..]);
    Ok((out, count))
}

fn layout(gpu: &Gpu, entries: &[wgpu::BindGroupLayoutEntry]) -> wgpu::BindGroupLayout {
    gpu.device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor { label: None, entries })
}

fn texture_entry(binding: u32, stage: wgpu::ShaderStages, sample_type: wgpu::TextureSampleType) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: stage,
        ty: wgpu::BindingType::Texture { sample_type, view_dimension: wgpu::TextureViewDimension::D2, multisampled: false },
        count: None,
    }
}

fn storage_texture_entry(binding: u32, format: wgpu::TextureFormat) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::COMPUTE,
        ty: wgpu::BindingType::StorageTexture {
            access: wgpu::StorageTextureAccess::WriteOnly,
            format,
            view_dimension: wgpu::TextureViewDimension::D2,
        },
        count: None,
    }
}

fn pipeline_layout(gpu: &Gpu, uniform: &wgpu::BindGroupLayout, group1: &wgpu::BindGroupLayout) -> wgpu::PipelineLayout {
    gpu.device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: None,
        bind_group_layouts: &[Some(uniform), Some(group1)],
        immediate_size: 0,
    })
}

/// A full-screen triangle pipeline (`fullscreen_vertex`): a `target` (RGBA8; the present pass: the
/// surface's format) written whole with no blend, or (None) no colour target and Depth32Float
/// written with Always.
fn fullscreen_pipeline(gpu: &Gpu, module: &wgpu::ShaderModule, layout: &wgpu::PipelineLayout, entry: &str,
                       target: Option<wgpu::TextureFormat>) -> wgpu::RenderPipeline {
    let depth = target.is_none();
    let target = [target.map(|format| wgpu::ColorTargetState { format, blend: None, write_mask: wgpu::ColorWrites::ALL })];
    gpu.device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some(entry),
        layout: Some(layout),
        vertex: wgpu::VertexState {
            module,
            entry_point: Some("fullscreen_vertex"),
            compilation_options: compilation_options(),
            buffers: &[],
        },
        primitive: wgpu::PrimitiveState {
            topology: wgpu::PrimitiveTopology::TriangleList,
            cull_mode: None,
            ..Default::default()
        },
        depth_stencil: depth.then(|| pipelines::depth_state(true, wgpu::CompareFunction::Always)),
        multisample: wgpu::MultisampleState { count: 1, mask: !0, alpha_to_coverage_enabled: false },
        fragment: Some(wgpu::FragmentState {
            module,
            entry_point: Some(entry),
            compilation_options: compilation_options(),
            targets: if depth { &[] } else { &target },
        }),
        multiview_mask: None,
        cache: None,
    })
}

fn compute_pipeline(gpu: &Gpu, module: &wgpu::ShaderModule, layout: &wgpu::PipelineLayout, entry: &str)
                    -> wgpu::ComputePipeline {
    gpu.device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some(entry),
        layout: Some(layout),
        module,
        entry_point: Some(entry),
        compilation_options: compilation_options(),
        cache: None,
    })
}

impl Objects {
    /// Every object; `dual` builds the dual-source module too (only when the device has
    /// dual-source blending). Inside a Validation error scope: Err on any error.
    pub fn create(gpu: &Gpu, dual: bool) -> Result<Objects, String> {
        let dual = dual && device::has_dual_source(gpu);
        scoped(gpu, "native objects", || Objects::build(gpu, dual))
    }

    /// `present_pixel` into a target of `format`, the window surface's (`Surface::format`: RGBA8,
    /// else BGRA8); the BGRA8 one is made on its first use.
    pub fn present_pipe(&self, gpu: &Gpu, format: wgpu::TextureFormat) -> &wgpu::RenderPipeline {
        if format == wgpu::TextureFormat::Rgba8Unorm {
            return &self.present_rgba;
        }
        self.present_bgra.get_or_init(|| fullscreen_pipeline(gpu, &self.module, &self.draw_layout, "present_pixel", Some(format)))
    }

    /// The fill's pipelines (present.rs): `present_shrink` into the small RGBA8 texture, and
    /// `present_fill` into a target of `format` (as `present_pipe`).
    pub fn fill_pipes(&self, gpu: &Gpu, format: wgpu::TextureFormat) -> (&wgpu::RenderPipeline, &wgpu::RenderPipeline) {
        let make = |entry: &str, format| fullscreen_pipeline(gpu, &self.module, &self.draw_layout, entry, Some(format));
        let shrink = self.fill_shrink.get_or_init(|| make("present_shrink", wgpu::TextureFormat::Rgba8Unorm));
        let fill = if format == wgpu::TextureFormat::Rgba8Unorm { &self.fill_rgba } else { &self.fill_bgra };
        (shrink, fill.get_or_init(|| make("present_fill", format)))
    }

    fn build(gpu: &Gpu, dual: bool) -> Objects {
        use crate::shaders::{NATIVE, NATIVE_DUAL};
        use wgpu::ShaderStages as S;
        use wgpu::TextureSampleType as T;
        let main = module(gpu, NATIVE, "native.wgsl");
        let dual_module = dual.then(|| {
            const ENABLE: &str = "enable dual_source_blending;\n";
            let mut source = String::with_capacity(ENABLE.len() + NATIVE.len() + NATIVE_DUAL.len());
            source.push_str(ENABLE);
            source.push_str(NATIVE);
            source.push_str(NATIVE_DUAL);
            module(gpu, &source, "native.wgsl + native_dual.wgsl")
        });

        let unfilterable = T::Float { filterable: false };
        let uniform_layout = layout(gpu, &[wgpu::BindGroupLayoutEntry {
            binding: 0,
            visibility: S::VERTEX | S::FRAGMENT | S::COMPUTE,
            ty: wgpu::BindingType::Buffer { ty: wgpu::BufferBindingType::Uniform, has_dynamic_offset: true, min_binding_size: None },
            count: None,
        }]);
        let texture_layout = layout(gpu, &[
            texture_entry(0, S::FRAGMENT, T::Float { filterable: true }),
            wgpu::BindGroupLayoutEntry {
                binding: 1,
                visibility: S::FRAGMENT,
                ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                count: None,
            },
        ]);
        let texels_layout = layout(gpu, &crate::native::shared::texels_entries());
        let blit_layout = layout(gpu, &[texture_entry(0, S::FRAGMENT, unfilterable), texture_entry(1, S::FRAGMENT, unfilterable)]);
        let scanout_layout = layout(gpu, &[texture_entry(0, S::COMPUTE, unfilterable), storage_texture_entry(1, wgpu::TextureFormat::Rgba8Unorm)]);
        let mut convert = Vec::with_capacity(11);
        convert.push(wgpu::BindGroupLayoutEntry {
            binding: 0,
            visibility: S::COMPUTE,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Storage { read_only: true },
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        });
        convert.push(texture_entry(1, S::COMPUTE, T::Uint)); // the base: a decoded texture's texels
        for i in 2..6 {
            convert.push(texture_entry(i, S::COMPUTE, unfilterable));
        }
        for i in 6..10 {
            convert.push(texture_entry(i, S::COMPUTE, T::Depth));
        }
        // A converted texture holds texels as a decoded one does (R32Uint, convert.rs).
        convert.push(storage_texture_entry(10, wgpu::TextureFormat::R32Uint));
        let convert_layout = layout(gpu, &convert);
        let grow_layout = layout(gpu, &[texture_entry(0, S::FRAGMENT, T::Depth)]);

        let draw_layout = pipeline_layout(gpu, &uniform_layout, &texture_layout);
        let texels_draw_layout = pipeline_layout(gpu, &uniform_layout, &texels_layout);
        let blit_pipeline_layout = pipeline_layout(gpu, &uniform_layout, &blit_layout);
        let scanout_pipeline_layout = pipeline_layout(gpu, &uniform_layout, &scanout_layout);
        let convert_pipeline_layout = pipeline_layout(gpu, &uniform_layout, &convert_layout);
        let grow_pipeline_layout = pipeline_layout(gpu, &uniform_layout, &grow_layout);

        let rgba = Some(wgpu::TextureFormat::Rgba8Unorm);
        let blit_pipe = fullscreen_pipeline(gpu, &main, &blit_pipeline_layout, "native_blit", rgba);
        let upload_pipe = fullscreen_pipeline(gpu, &main, &blit_pipeline_layout, "native_upload_alpha", rgba);
        let present_rgba = fullscreen_pipeline(gpu, &main, &draw_layout, "present_pixel", rgba);
        let grow_pipe = fullscreen_pipeline(gpu, &main, &grow_pipeline_layout, "native_grow_depth", None);
        let scanout_pipe = compute_pipeline(gpu, &main, &scanout_pipeline_layout, "native_scanout");
        let scanout_fast_pipe = compute_pipeline(gpu, &main, &scanout_pipeline_layout, "native_scanout_fast");
        let convert_pipe = compute_pipeline(gpu, &main, &convert_pipeline_layout, "native_convert");

        let table = inverse_table();
        let mut bytes = Vec::with_capacity(table.len() * 4);
        for w in table {
            bytes.extend_from_slice(&w.to_le_bytes());
        }
        let inverse = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("native inverse"),
            size: bytes.len() as u64,
            usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        gpu.queue.write_buffer(&inverse, 0, &bytes);

        Objects {
            dual,
            module: main,
            dual_module,
            uniform_layout,
            texture_layout,
            texels_layout,
            blit_layout,
            scanout_layout,
            convert_layout,
            grow_layout,
            draw_layout,
            texels_draw_layout,
            blit_pipeline_layout,
            scanout_pipeline_layout,
            convert_pipeline_layout,
            grow_pipeline_layout,
            blit_pipe,
            upload_pipe,
            present_rgba,
            present_bgra: std::sync::OnceLock::new(),
            fill_shrink: std::sync::OnceLock::new(),
            fill_rgba: std::sync::OnceLock::new(),
            fill_bgra: std::sync::OnceLock::new(),
            grow_pipe,
            scanout_pipe,
            scanout_fast_pipe,
            convert_pipe,
            inverse,
        }
    }
}

/// D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA (BLEND_SA, BLEND_ISA in the C code).
const BLEND_SA: u32 = 5;
const BLEND_ISA: u32 = 6;

/// One draw pipeline of each variant, built at create so a device that can't build them fails
/// there (native_objects' last step).
fn probe_keys(dual: bool) -> Vec<u32> {
    use pipelines::{VARIANT_ALPHA_ONLY, VARIANT_DUAL, VARIANT_PLAIN, VARIANT_RGB_A128};
    let mut keys = vec![2 << 1 | 1 << 6 | 15 << 14 | VARIANT_PLAIN << 23];
    if dual {
        keys.push(1 | BLEND_SA << 1 | BLEND_ISA << 6 | 1 << 11 | 15 << 14 | VARIANT_DUAL << 23);
    }
    keys.push(1 | BLEND_SA << 1 | BLEND_ISA << 6 | 1 << 11 | 7 << 14 | VARIANT_RGB_A128 << 23);
    keys.push(8 << 14 | (16 | 4) << 18 | VARIANT_ALPHA_ONLY << 23);
    keys
}

impl Native {
    /// `dhdr_native_create`: `flags` `DHDR_NATIVE_*`.
    pub fn create(gpu: &'static Gpu, flags: u32) -> Result<Native, String> {
        let objects = Objects::create(gpu, flags & DHDR_NATIVE_DUAL != 0)?;
        let mut pipelines = Pipelines::new();
        for key in probe_keys(objects.dual) {
            pipelines.get(gpu, &objects, key as u64)?;
        }
        let mut n = Native {
            gpu,
            flags,
            objects,
            pipelines,
            arenas: crate::native::arenas::Arenas::new(),
            surfaces: crate::native::surfaces::Surfaces::new(),
            converts: crate::native::convert::Converts::new(),
            display: crate::native::scanout::Display::new(),
            pass: crate::native::render::Pass::new(),
            presenter: crate::native::present::Presenter::new(),
            zread: crate::native::zread::ZRead::new(),
        };
        n.options(flags); // the present filter, as native_objects' native_present_options
        Ok(n)
    }
}

#[cfg(test)]
mod tests {
    use super::parenthesize_shifts;

    /// Each layout's part of the inverse table names every pixel of its page once.
    #[test]
    fn inverse_table_covers_pages() {
        let table = super::inverse_table();
        for (start, words, height) in [(0, 2048, 32), (2048, 2048, 32), (4096, 4096, 64)] {
            let mut seen = vec![false; 64 * height];
            for &at in &table[start..start + words] {
                let (x, y) = ((at & 255) as usize, (at >> 8) as usize);
                assert!(x < 64 && y < height && !seen[y * 64 + x], "layout at {start}: pixel {x}, {y}");
                seen[y * 64 + x] = true;
            }
        }
        // PSMCT16: pixel (1, 0) is the page's third half-word (memory.c pixel_address: column 2).
        assert_eq!(table[4096 + 2], 1);
    }

    #[test]
    fn shifts_get_parentheses() {
        let p = |s: &str| parenthesize_shifts(s).map(|(t, _)| t);
        assert_eq!(p("let S = nat.scale >> 8u & 255u;").unwrap(), "let S = (nat.scale >> 8u) & 255u;");
        assert_eq!(p("x = (a & 1u) | b[i].c << 2u | f(y) >> 1u ^ -z << w.x;").unwrap(),
                   "x = (a & 1u) | (b[i].c << 2u) | (f(y) >> 1u) ^ (-z << w.x);");
        assert_eq!(p("v = vec2<u32>(t.x, t.y >> b.w); /* a >> b & c */ // d << e | f").unwrap(),
                   "v = vec2<u32>(t.x, (t.y >> b.w)); /* a >> b & c */ // d << e | f");
        let templates = "var<storage> v: array<vec4<u32>>; let w = bitcast<vec2<u32>>(z); let k = 1u <<= 2u;";
        assert_eq!(parenthesize_shifts(templates).unwrap(), (templates.to_owned(), 0));
        assert!(parenthesize_shifts("let q = x - y << 2u;").is_err());
        assert!(parenthesize_shifts("let q = x << y * 2u;").is_err());
    }

    /// The browser's copy means what native.wgsl means: naga reads both to the same module (the
    /// source spans aside).
    #[cfg(feature = "native")]
    #[test]
    fn native_wgsl_keeps_its_meaning() {
        use crate::shaders::{NATIVE, NATIVE_DUAL};
        fn module(src: &str) -> String {
            let m = naga::front::wgsl::parse_str(src).unwrap_or_else(|e| panic!("{}", e.emit_to_string(src)));
            let text = format!("{m:?}");
            let mut out = String::with_capacity(text.len());
            let mut rest = text.as_str();
            while let Some(at) = rest.find("Span { start: ") {
                out.push_str(&rest[..at]);
                let end = rest[at..].find('}').expect("a span's end") + at + 1;
                rest = &rest[end..];
            }
            out.push_str(rest);
            out
        }
        let dual = format!("enable dual_source_blending;\n{NATIVE}{NATIVE_DUAL}");
        for src in [NATIVE, dual.as_str()] {
            let (fixed, count) = parenthesize_shifts(src).expect("native.wgsl parenthesized");
            assert!(count > 0);
            assert_ne!(fixed, src);
            assert_eq!(module(src), module(&fixed));
        }
        // Negative control: the other grouping is a different module.
        const LINE: &str = "nat.scale >> 8u & 255u";
        assert!(NATIVE.contains(LINE));
        assert_ne!(module(NATIVE), module(&NATIVE.replacen(LINE, "nat.scale >> (8u & 255u)", 1)));
    }
}
