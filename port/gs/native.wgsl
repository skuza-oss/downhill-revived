// Native renderer on WebGPU (DHD_RENDER=native with DHD_GS_BACKEND=wgpu; gpu_wgpu_native.inc,
// finding 084 W5-W7): raster.hlsl's DHD_NATIVE shaders in WGSL, the same maths line for line.
// Ordinary hardware triangles into real render targets; colours stay in GS units (0..255),
// stored alpha is GS alpha / 255 and the blender takes As / 128. Pictures are close to the GS,
// not exact; EE-visible state never uses them.
//
// Group 0 binding 0 is one uniform arena (dynamic offsets): each entry point reads its own
// struct there. Group 1 holds the entry point's textures. textureLoad outside a texture reads 0
// here (load_*), as D3D11's Load does; WebGPU leaves it to the implementation.
//
// No function takes a texture or a sampler as an argument: each texture has its own load_*
// reading the global. Mali's shader compiler (driver r54) crashes on a texture passed to a
// function (a null read in vkCreate*Pipelines; finding 153), and naga passes one as written.
//
// Blending: with dual-source blending (native_dual.wgsl appended after "enable
// dual_source_blending;") native_pixel_dual gives the colour and As / 128 as D3D11 does. Without
// it a draw whose blend uses As is split: native_pixel_blend writes RGB with As / 128 as its
// alpha (SrcAlpha factors), then native_pixel writes the alpha channel alone, unblended, on the
// pixels the first draw wrote (gpu_wgpu_native.inc, native_draw_passes).

// Set only when uploaded S=E is a power of two, at most eight.
// This lets the compiler fold the original arithmetic without a runtime branch.
override native_texel_scale: u32 = 0u;

// A draw pipeline specialised for its draw's state (pipelines.rs, `spec`): the fragment stage
// takes parts of the state from these bits instead of the uniform: with bit 31 the flags (bits
// 0-11), with bit 30 the CLAMP modes (bits 12-13 U, 14-15 V), with bit 29 the alpha test (bits
// 16-18); bit 19 when the texture is exactly tex_size (a decoded texture), so load_texel's bounds
// test takes the uniform's size instead of asking the texture; bit 20 when a textured draw's
// alpha_scale is 255 (every one native_core.inc makes), so a texel's alpha is its byte. Paths the draw never takes compile
// away; the arithmetic of the paths it takes is the same. Zero: the generic pipeline, all from
// the uniform.
override native_spec: u32 = 0u;
fn draw_flags() -> u32 { return select(nat.flags, native_spec & 4095u, (native_spec & 0x80000000u) != 0u); }
fn wrap_mode() -> vec2<u32> {
    return select(nat.wrap.xy, vec2<u32>(native_spec >> 12u & 3u, native_spec >> 14u & 3u), (native_spec & 0x40000000u) != 0u);
}
fn texture_sized() -> bool { return (native_spec & 0x80000u) != 0u; }
fn alpha_test() -> u32 { return select(nat.atst, native_spec >> 16u & 7u, (native_spec & 0x20000000u) != 0u); }
fn unit_alpha() -> bool { return (native_spec & 0x100000u) != 0u; }

struct Native {
    target_size: vec2<f32>, flags: u32, atst: u32,
    aref: u32, alpha_scale: f32, texa_alpha: f32, scale: u32,
    fog: vec4<f32>,
    tex_offset: vec2<i32>, tex_size: vec2<u32>,
    wrap: vec4<u32>,
    region: vec4<u32>,
}
@group(0) @binding(0) var<uniform> nat: Native;
@group(1) @binding(0) var native_texture: texture_2d<f32>;
@group(1) @binding(1) var native_replace_sampler: sampler;   // texture packs (flag 2048)
// A decoded texture (textures.rs) is R32Uint: each texel its RGBA bytes, read by the `_u` entry
// points (their pipelines' group 1 is init.rs's texels layout, the sampler only for gathers).
@group(1) @binding(0) var native_texels: texture_2d<u32>;
@group(1) @binding(1) var native_texel_sampler: sampler;

// xy is outside a texture of `size` texels (the load_* below read 0 there).
fn outside(xy: vec2<i32>, size: vec2<u32>) -> bool {
    return any(xy < vec2<i32>(0)) || any(xy >= vec2<i32>(size));
}
// A load_* of native_texture without a branch: the load always reads a texel inside the
// texture (xy clamped) and the select gives 0 outside, so a bilinear's four loads can be issued
// together instead of each waiting behind its own test (finding 131). Exactly tex_size
// (texture_sized()): the uniform's size, not the texture's.
fn load_texel(xy: vec2<i32>) -> vec4<f32> {
    var size: vec2<i32>;
    if (texture_sized()) { size = vec2<i32>(nat.tex_size); } else { size = vec2<i32>(textureDimensions(native_texture)); }
    let v = textureLoad(native_texture, clamp(xy, vec2<i32>(0), size - 1), 0);
    return select(vec4<f32>(0.0), v, all(xy >= vec2<i32>(0)) && all(xy < size));
}
// load_texel for a decoded texture's texels (0 outside).
fn load_texels(xy: vec2<i32>) -> u32 {
    var size: vec2<i32>;
    if (texture_sized()) { size = vec2<i32>(nat.tex_size); } else { size = vec2<i32>(textureDimensions(native_texels)); }
    let w = textureLoad(native_texels, clamp(xy, vec2<i32>(0), size - 1), 0).x;
    return select(0u, w, all(xy >= vec2<i32>(0)) && all(xy < size));
}

struct Varyings {
    @builtin(position) @invariant position: vec4<f32>,
    @location(0) @interpolate(linear) stqf: vec4<f32>,
    @location(1) @interpolate(linear) color: vec4<f32>,
}

// nat.target_size is the attachment's size in GS pixels (its pixels / T), so GS x lands at x * T.
@vertex
fn native_vertex(@location(0) position_in: vec3<f32>, @location(1) stqf: vec4<f32>,
                 @location(2) color: vec4<u32>) -> Varyings {
    var o: Varyings;
    var position = position_in;
    // Upscaled sprites (flag 1024) cover whole GS pixels, as at 1x (raster.hlsl native_vertex).
    if ((nat.flags & 1024u) != 0u && (nat.scale & 255u) > 1u) {
        position = vec3<f32>(ceil(position.xy - 0.5), position.z);
    }
    o.position = vec4<f32>(position.xy / nat.target_size * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), position.z, 1.0);
    o.stqf = stqf;
    o.color = vec4<f32>(color);
    return o;
}

// CLAMP modes: repeat, clamp, region clamp, region repeat (MINU is the mask, MAXU the fix).
fn wrap_coordinate(c: i32, mode: u32, size: u32, low: u32, high: u32) -> u32 {
    if (mode == 0u) { return u32(c) & (size - 1u); }
    if (mode == 1u) { return u32(clamp(c, 0, i32(size) - 1)); }
    if (mode == 2u) { return u32(clamp(c, i32(low), i32(high))); }
    return (u32(c) & low) | high;
}

// The texture pixel holding GS texel c (S: the texture's scale, E: the draw's resolution scale).
fn texel_xy(c: vec2<i32>) -> vec2<i32> {
    let S = select(nat.scale >> 8u & 255u, native_texel_scale, native_texel_scale != 0u);
    let E = select(nat.scale >> 16u & 255u, native_texel_scale, native_texel_scale != 0u);
    let step = S / E;
    let whole = vec2<i32>(floor(vec2<f32>(c) / f32(E)));
    let sub = c - whole * i32(E);
    let wrap = wrap_mode();
    let at = vec2<u32>(wrap_coordinate(whole.x, wrap.x, nat.tex_size.x, nat.region.x, nat.region.y),
                       wrap_coordinate(whole.y, wrap.y, nat.tex_size.y, nat.region.z, nat.region.w));
    return (nat.tex_offset + vec2<i32>(at)) * i32(S) + sub * i32(step) + i32(step / 2u);
}

fn texel(c: vec2<i32>) -> vec4<f32> {
    var t = load_texel(texel_xy(c));
    // GS units. A loaded unorm8 times 255 is exactly the byte (f32(v / 255) * 255 rounds to v for
    // every v); round() keeps it so when a compiler fuses that product into a later add (as the
    // bilinear mix's), where the exact product, v +- 8e-6, would reach a floor() (finding 131).
    t = vec4<f32>(round(t.rgb * 255.0), select(round(t.a * nat.alpha_scale), nat.texa_alpha, (draw_flags() & 256u) != 0u));
    return t;
}

// A decoded texel's RGBA bytes in GS units: what texel() makes of the same texel as unorm8 (the
// byte; its alpha times alpha_scale / 255, which is 255 for every textured draw, native_core.inc).
fn texel_bytes(w: u32) -> vec4<f32> {
    let a = f32(w >> 24u);
    let alpha = select(select(round(a / 255.0 * nat.alpha_scale), a, nat.alpha_scale == 255.0), a, unit_alpha());
    return vec4<f32>(f32(w & 255u), f32(w >> 8u & 255u), f32(w >> 16u & 255u),
                     select(alpha, nat.texa_alpha, (draw_flags() & 256u) != 0u));
}

fn texel_u(c: vec2<i32>) -> vec4<f32> {
    return texel_bytes(load_texels(texel_xy(c)));
}

// Bilinear over GS texels b .. b + 1 with weights f. When the texture is exactly tex_size (a power
// of two) at offset 0 and unit scale, CLAMP repeat or clamp (so the wrapped coordinates are its
// texels), one gather reads the four texels the wrap gives, at .. next on each axis: the texels
// sampler repeats, so a gather at corner c (c / tex_size, exact) reads texels c - 1 and c mod the
// size. Repeat: c = at + 1, whose footprint is at and (at + 1) mod size, the wrap's next. Clamp: c
// = b + 1 inside; left of the texture both texels are texel 0 (c = 1, the left one taken twice),
// right of it both are the last (c = size - 1, the right one twice). So every pixel of such a draw
// takes the gather (no wave runs both paths); else four loads, as texel_u. Same texels, same mix.
fn bilinear_u(b: vec2<i32>, f: vec2<f32>) -> vec4<f32> {
    let wrap = wrap_mode();
    let E = select(nat.scale >> 16u & 255u, native_texel_scale, native_texel_scale != 0u);
    let S = select(nat.scale >> 8u & 255u, native_texel_scale, native_texel_scale != 0u);
    if (texture_sized() && all(wrap <= vec2<u32>(1u)) && all(nat.tex_offset == vec2<i32>(0)) && S == 1u && E == 1u) {
        let size = vec2<i32>(nat.tex_size);
        let clamped = wrap == vec2<u32>(1u);
        let low = clamped & (b < vec2<i32>(0));
        let high = clamped & (b >= size - 1) & !low;
        let repeat_corner = vec2<i32>(vec2<u32>(b) & (nat.tex_size - 1u)) + 1;
        let clamp_corner = select(select(b + 1, size - 1, high), vec2<i32>(1), low);
        let corner = select(repeat_corner, clamp_corner, clamped);
        // x: (c.x - 1, c.y), y: (c.x, c.y), z: (c.x, c.y - 1), w: (c.x - 1, c.y - 1)
        let g = textureGather(0, native_texels, native_texel_sampler, vec2<f32>(corner) / vec2<f32>(size));
        var t00 = g.w; var t10 = g.z; var t01 = g.x; var t11 = g.y;
        if (low.x) { t10 = t00; t11 = t01; }
        if (high.x) { t00 = t10; t01 = t11; }
        if (low.y) { t01 = t00; t11 = t10; }
        if (high.y) { t00 = t01; t10 = t11; }
        return mix(mix(texel_bytes(t00), texel_bytes(t10), f.x),
                   mix(texel_bytes(t01), texel_bytes(t11), f.x), f.y);
    }
    return mix(mix(texel_u(b), texel_u(b + vec2<i32>(1, 0)), f.x),
               mix(texel_u(b + vec2<i32>(0, 1)), texel_u(b + vec2<i32>(1, 1)), f.x), f.y);
}

fn replacement(uv_in: vec2<f32>, across: vec2<f32>, down: vec2<f32>) -> vec4<f32> {
    var uv = uv_in;
    let size = vec2<f32>(textureDimensions(native_texture));
    let gs_size = vec2<f32>(nat.tex_size);
    let half_texel = gs_size / size * 0.5;
    let wrap = wrap_mode();
    if (wrap.x == 2u) { uv.x = clamp(uv.x, f32(nat.region.x) + half_texel.x, f32(nat.region.y) + 1.0 - half_texel.x); }
    if (wrap.y == 2u) { uv.y = clamp(uv.y, f32(nat.region.z) + half_texel.y, f32(nat.region.w) + 1.0 - half_texel.y); }
    var t = textureSampleGrad(native_texture, native_replace_sampler, uv / gs_size, across / gs_size, down / gs_size);
    return vec4<f32>(t.rgb * 255.0, t.a * nat.alpha_scale);
}

// The texture's colour at the pixel in GS units (raster.hlsl native_pixel's sampling), from a
// float texture: a surface, a conversion or a replacement.
fn fetch(i: Varyings, uv_in: vec2<f32>, across: vec2<f32>, down: vec2<f32>) -> vec4<f32> {
    let flags = draw_flags();
    var uv = uv_in;
    var t: vec4<f32>;
    if ((flags & 2048u) != 0u) { t = replacement(uv, across, down); }
    else {
        let T = nat.scale & 255u;
        let E = select(nat.scale >> 16u & 255u, native_texel_scale, native_texel_scale != 0u);
        if ((flags & 1024u) != 0u && T > 1u && E == 1u) {
            let corner = floor(i.position.xy / f32(T)) * f32(T) - i.position.xy;
            uv += across * corner.x + down * corner.y;
        }
        uv *= f32(E);
        if ((flags & 32u) != 0u) {
            uv -= 0.5;
            let b = vec2<i32>(floor(uv));
            let f = uv - floor(uv);
            t = mix(mix(texel(b), texel(b + vec2<i32>(1, 0)), f.x),
                    mix(texel(b + vec2<i32>(0, 1)), texel(b + vec2<i32>(1, 1)), f.x), f.y);
        } else { t = texel(vec2<i32>(floor(uv))); }
    }
    return t;
}

// fetch() from a decoded texture's texels (never a replacement: that is a float texture).
fn fetch_u(i: Varyings, uv_in: vec2<f32>, across: vec2<f32>, down: vec2<f32>) -> vec4<f32> {
    let flags = draw_flags();
    var uv = uv_in;
    var t: vec4<f32>;
    let T = nat.scale & 255u;
    let E = select(nat.scale >> 16u & 255u, native_texel_scale, native_texel_scale != 0u);
    if ((flags & 1024u) != 0u && T > 1u && E == 1u) {
        let corner = floor(i.position.xy / f32(T)) * f32(T) - i.position.xy;
        uv += across * corner.x + down * corner.y;
    }
    uv *= f32(E);
    if ((flags & 32u) != 0u) {
        uv -= 0.5;
        let b = vec2<i32>(floor(uv));
        let f = uv - floor(uv);
        t = bilinear_u(b, f);
    } else { t = texel_u(vec2<i32>(floor(uv))); }
    return t;
}

// The pixel's colour in GS units (raster.hlsl native_pixel, before its outputs) from the
// texture's colour t (when textured); discards as it does. The alpha first, then the alpha test,
// then the colour (the same expressions: the colour reads only the vertex alpha, the fog leaves
// the alpha as it is), so a discarded pixel computes no colour.
fn combine(i: Varyings, t: vec4<f32>) -> vec4<f32> {
    let flags = draw_flags();
    let textured = (flags & 1u) != 0u;
    let tfx = (flags >> 2u) & 3u;
    var a = i.color.a;
    if (textured && (flags & 2u) != 0u) {
        if (tfx == 0u) { a = min(floor(t.a * i.color.a / 128.0), 255.0); }
        else if (tfx == 2u) { a = min(t.a + i.color.a, 255.0); }
        else { a = t.a; }
    }
    if ((flags & 64u) != 0u) {
        let ua = u32(a);
        let r = nat.aref;
        let test = alpha_test();
        let passed = test == 1u || (test == 2u && ua < r) || (test == 3u && ua <= r) || (test == 4u && ua == r) ||
                     (test == 5u && ua >= r) || (test == 6u && ua > r) || (test == 7u && ua != r);
        if (passed == ((flags & 128u) != 0u)) { discard; }
    }
    var rgb = i.color.rgb;
    if (textured) {
        rgb = select(min(floor(t.rgb * i.color.rgb / 128.0), vec3<f32>(255.0)), t.rgb, tfx == 1u);
        if (tfx >= 2u) { rgb = min(rgb + i.color.a, vec3<f32>(255.0)); }
    }
    if ((flags & 16u) != 0u) { rgb = floor(mix(nat.fog.rgb, rgb, i.stqf.w)); }
    if ((flags & 512u) != 0u) { rgb = floor(rgb * a / 128.0); }
    return vec4<f32>(rgb, a);
}

fn shade(i: Varyings, uv: vec2<f32>, across: vec2<f32>, down: vec2<f32>) -> vec4<f32> {
    var t = vec4<f32>(0.0);
    if ((draw_flags() & 1u) != 0u) { t = fetch(i, uv, across, down); }
    return combine(i, t);
}

fn shade_u(i: Varyings, uv: vec2<f32>, across: vec2<f32>, down: vec2<f32>) -> vec4<f32> {
    var t = vec4<f32>(0.0);
    if ((draw_flags() & 1u) != 0u) { t = fetch_u(i, uv, across, down); }
    return combine(i, t);
}

fn texture_uv(i: Varyings) -> vec2<f32> {
    let q = select(1e-30, i.stqf.z, abs(i.stqf.z) > 1e-30);
    return i.stqf.xy / q * vec2<f32>(nat.tex_size);
}

// The target's colour and alpha (alpha / 255): no blending on As, or the alpha-only second draw.
@fragment
fn native_pixel(i: Varyings) -> @location(0) vec4<f32> {
    let uv = texture_uv(i);
    let across = dpdx(uv);
    let down = dpdy(uv);
    let color = shade(i, uv, across, down);
    return vec4<f32>(color.rgb / 255.0, saturate(color.a / 255.0));
}

// RGB blended with As / 128 as the source alpha (the alpha channel is masked off).
@fragment
fn native_pixel_blend(i: Varyings) -> @location(0) vec4<f32> {
    let uv = texture_uv(i);
    let across = dpdx(uv);
    let down = dpdy(uv);
    let color = shade(i, uv, across, down);
    return vec4<f32>(color.rgb / 255.0, saturate(color.a / 128.0));
}

// native_pixel and native_pixel_blend for a decoded texture's texels.
@fragment
fn native_pixel_u(i: Varyings) -> @location(0) vec4<f32> {
    let uv = texture_uv(i);
    let across = dpdx(uv);
    let down = dpdy(uv);
    let color = shade_u(i, uv, across, down);
    return vec4<f32>(color.rgb / 255.0, saturate(color.a / 255.0));
}

@fragment
fn native_pixel_blend_u(i: Varyings) -> @location(0) vec4<f32> {
    let uv = texture_uv(i);
    let across = dpdx(uv);
    let down = dpdy(uv);
    let color = shade_u(i, uv, across, down);
    return vec4<f32>(color.rgb / 255.0, saturate(color.a / 128.0));
}

// The display circuit from a render target (raster.hlsl native_scanout).
struct Display { a: vec4<u32>, b: vec4<u32>, c: vec4<u32>, }
@group(0) @binding(0) var<uniform> display_state: Display;
@group(1) @binding(0) var native_source: texture_2d<f32>;
@group(1) @binding(1) var screen: texture_storage_2d<rgba8unorm, write>;

fn load_native_source(xy: vec2<i32>) -> vec4<f32> {
    if (outside(xy, textureDimensions(native_source))) { return vec4<f32>(0.0); }
    return textureLoad(native_source, xy, 0);
}

@compute @workgroup_size(8, 8, 1)
fn native_scanout(@builtin(global_invocation_id) tid: vec3<u32>) {
    let a = display_state.a;
    let b = display_state.b;
    let S = display_state.c.x;
    if (tid.x >= b.y || tid.y >= b.z) { return; }
    let at = vec2<u32>(tid.x, tid.y >> b.w);
    let xy = ((vec2<u32>(a.w, b.x) + at / S) & vec2<u32>(2047u)) * S + at % S;
    textureStore(screen, tid.xy, vec4<f32>(load_native_source(vec2<i32>(xy)).rgb, 1.0));
}

// Equivalent GS wrap for bounded display sizes (scanout.rs). The original map
// ((origin + at/S) & 2047)*S + at%S is (origin*S + at) % (2048*S).
// With dividend < 65536, the rounded-up 16-bit reciprocal needs at most one
// quotient correction. All arithmetic stays integer, including non-power-of-two scales.
@compute @workgroup_size(8, 8, 1)
fn native_scanout_fast(@builtin(global_invocation_id) tid: vec3<u32>) {
    let a = display_state.a;
    let b = display_state.b;
    let c = display_state.c;
    let doubled = b.w == 1u;
    let y = tid.y << select(0u, 1u, doubled);
    if (tid.x >= b.y || y >= b.z) { return; }
    let at = vec2<u32>(tid.x, y >> b.w);
    let dividend = (vec2<u32>(a.w, b.x) & vec2<u32>(2047u)) * c.x + at;
    var q = (dividend * c.z) >> vec2<u32>(16u);
    q -= select(vec2<u32>(0u), vec2<u32>(1u), q * c.y > dividend);
    let xy = dividend - q * c.y;
    let color = vec4<f32>(load_native_source(vec2<i32>(xy)).rgb, 1.0);
    textureStore(screen, vec2<u32>(tid.x, y), color);
    // The interlaced scanout maps both output rows to the same input texel.
    if (doubled && y + 1u < b.z) {
        textureStore(screen, vec2<u32>(tid.x, y + 1u), color);
    }
}

// A rectangle of one surface into another of a different scale (raster.hlsl native_blit), and a
// page of uploaded alpha bits (native_upload_alpha): the full-screen triangle, cut by the viewport.
struct Blit { map: vec4<f32>, }
@group(0) @binding(0) var<uniform> blit: Blit;
@group(1) @binding(0) var blit_source: texture_2d<f32>;
@group(1) @binding(1) var upload_previous: texture_2d<f32>;

fn load_blit_source(xy: vec2<i32>) -> vec4<f32> {
    if (outside(xy, textureDimensions(blit_source))) { return vec4<f32>(0.0); }
    return textureLoad(blit_source, xy, 0);
}
fn load_upload_previous(xy: vec2<i32>) -> vec4<f32> {
    if (outside(xy, textureDimensions(upload_previous))) { return vec4<f32>(0.0); }
    return textureLoad(upload_previous, xy, 0);
}

@vertex
fn fullscreen_vertex(@builtin(vertex_index) id: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(select(-1.0, 3.0, id == 1u), select(1.0, -3.0, id == 2u));
    return vec4<f32>(p, 0.0, 1.0);
}

@fragment
fn native_blit(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    return load_blit_source(vec2<i32>(floor((position.xy / blit.map.z + blit.map.xy) * blit.map.w)));
}

@fragment
fn native_upload_alpha(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let local = vec2<i32>(floor(position.xy / blit.map.z - blit.map.xy));
    let upload = vec2<u32>(round(load_blit_source(local).rg * 255.0));
    let previous = load_upload_previous(vec2<i32>(position.xy));
    let alpha = u32(round(previous.a * 255.0));
    return vec4<f32>(previous.rgb, f32((alpha & ~upload.y) | upload.x) / 255.0);
}

// A Z surface grown to a larger texture (gpu_wgpu_native.inc, native_surface_resources): depth
// textures can only be copied whole, so the old one is drawn into the new one.
@group(1) @binding(0) var grow_source: texture_depth_2d;
fn load_grow_source(xy: vec2<i32>) -> f32 {
    if (outside(xy, textureDimensions(grow_source))) { return 0.0; }
    return textureLoad(grow_source, xy, 0);
}
@fragment
fn native_grow_depth(@builtin(position) position: vec4<f32>) -> @builtin(frag_depth) f32 {
    return load_grow_source(vec2<i32>(position.xy));
}

// A texture over GS memory some of which native surfaces drew (raster.hlsl native_convert). Z
// surfaces are read as depth textures directly (D3D11 needs a typeless texture for that).
struct Convert {
    tex: vec4<u32>, size: vec4<u32>,
    slot: array<vec4<u32>, 4>,
    pages: array<vec4<u32>, 32>,
    palette: array<vec4<u32>, 64>,
}
@group(0) @binding(0) var<uniform> convert: Convert;
@group(1) @binding(0) var<storage, read> native_inverse: array<u32>;
@group(1) @binding(1) var convert_base: texture_2d<u32>;   // a decoded texture's texels
@group(1) @binding(2) var convert_colour0: texture_2d<f32>;
@group(1) @binding(3) var convert_colour1: texture_2d<f32>;
@group(1) @binding(4) var convert_colour2: texture_2d<f32>;
@group(1) @binding(5) var convert_colour3: texture_2d<f32>;
@group(1) @binding(6) var convert_depth0: texture_depth_2d;
@group(1) @binding(7) var convert_depth1: texture_depth_2d;
@group(1) @binding(8) var convert_depth2: texture_depth_2d;
@group(1) @binding(9) var convert_depth3: texture_depth_2d;
// A converted texture's texels: each its RGBA bytes, as a decoded texture's (read by the `_u`
// entry points).
@group(1) @binding(10) var native_converted: texture_storage_2d<r32uint, write>;

fn load_convert_base(xy: vec2<i32>) -> u32 {
    if (outside(xy, textureDimensions(convert_base))) { return 0u; }
    return textureLoad(convert_base, xy, 0).x;
}
fn load_convert_colour0(xy: vec2<i32>) -> vec4<f32> {
    if (outside(xy, textureDimensions(convert_colour0))) { return vec4<f32>(0.0); }
    return textureLoad(convert_colour0, xy, 0);
}
fn load_convert_colour1(xy: vec2<i32>) -> vec4<f32> {
    if (outside(xy, textureDimensions(convert_colour1))) { return vec4<f32>(0.0); }
    return textureLoad(convert_colour1, xy, 0);
}
fn load_convert_colour2(xy: vec2<i32>) -> vec4<f32> {
    if (outside(xy, textureDimensions(convert_colour2))) { return vec4<f32>(0.0); }
    return textureLoad(convert_colour2, xy, 0);
}
fn load_convert_colour3(xy: vec2<i32>) -> vec4<f32> {
    if (outside(xy, textureDimensions(convert_colour3))) { return vec4<f32>(0.0); }
    return textureLoad(convert_colour3, xy, 0);
}
fn load_convert_depth0(xy: vec2<i32>) -> f32 {
    if (outside(xy, textureDimensions(convert_depth0))) { return 0.0; }
    return textureLoad(convert_depth0, xy, 0);
}
fn load_convert_depth1(xy: vec2<i32>) -> f32 {
    if (outside(xy, textureDimensions(convert_depth1))) { return 0.0; }
    return textureLoad(convert_depth1, xy, 0);
}
fn load_convert_depth2(xy: vec2<i32>) -> f32 {
    if (outside(xy, textureDimensions(convert_depth2))) { return 0.0; }
    return textureLoad(convert_depth2, xy, 0);
}
fn load_convert_depth3(xy: vec2<i32>) -> f32 {
    if (outside(xy, textureDimensions(convert_depth3))) { return 0.0; }
    return textureLoad(convert_depth3, xy, 0);
}

fn bits_per_pixel(psm: u32) -> u32 {
    if (psm == 1u || psm == 49u) { return 24u; }
    if ((psm & 7u) == 2u) { return 16u; }
    if (psm == 19u || psm == 27u) { return 8u; }
    if (psm == 20u || psm == 36u || psm == 44u) { return 4u; }
    return 32u;
}

// raster.wgsl's address() without ADDRESS_SHIFT (raster.hlsl's default, which native_convert uses).
fn address(psm: u32, bp: u32, bw: u32, x: u32, y: u32) -> u32 {
    var pw = 64u; var ph = 32u; var bits = 32u; var high = 0u; var block = 0u; var column = 0u;
    var bx = 0u; var by = 0u;
    if (psm == 19u || psm == 20u) { pw = 128u; ph = select(128u, 64u, psm == 19u); }
    else if ((psm & 7u) == 2u) { ph = 64u; }
    let px = x % pw;
    let py = y % ph;
    if (psm == 19u) {
        bits = 8u; bx = px / 16u; by = py / 16u;
        block = (bx & 1u) | (by & 1u) << 1u | (bx & 2u) << 1u | (by & 2u) << 2u | (bx & 4u) << 2u;
        column = (px & 1u) << 2u | (px & 2u) << 3u | (px & 4u) << 3u | (px & 8u) >> 2u |
                 (py & 1u) << 3u | (py & 2u) >> 1u | (py & 4u) << 4u | (py & 8u) << 4u;
        column ^= ((py >> 1u ^ py >> 2u) & 1u) << 5u;
    } else if (psm == 20u) {
        bits = 4u; bx = px / 32u; by = py / 16u;
        block = (by & 1u) | (bx & 1u) << 1u | (by & 2u) << 1u | (bx & 2u) << 2u | (by & 4u) << 2u;
        column = (px & 1u) << 3u | (px & 2u) << 4u | (px & 4u) << 4u | (px & 8u) >> 2u | (px & 16u) >> 2u |
                 (py & 1u) << 4u | (py & 2u) >> 1u | (py & 4u) << 5u | (py & 8u) << 5u;
        column ^= ((py >> 1u ^ py >> 2u) & 1u) << 6u;
    } else if ((psm & 7u) == 2u) {
        bits = 16u; bx = px / 16u; by = py / 8u;
        if ((psm & 8u) != 0u) {
            block = (by & 1u) | (bx & 1u) << 1u | (by & 4u) | (by & 2u) << 2u | (bx & 2u) << 3u;
        } else {
            block = (by & 1u) | (bx & 1u) << 1u | (by & 2u) << 1u | (bx & 2u) << 2u | (by & 4u) << 2u;
        }
        column = (px & 1u) << 1u | (px & 2u) << 2u | (px & 4u) << 2u | (px & 8u) >> 3u |
                 (py & 1u) << 2u | (py & 6u) << 4u;
    } else {
        bx = px / 8u; by = py / 8u;
        block = (bx & 1u) | (by & 1u) << 1u | (bx & 2u) << 1u | (by & 2u) << 2u | (bx & 4u) << 2u;
        column = (px & 1u) | (px & 6u) << 1u | (py & 1u) << 1u | (py & 6u) << 3u;
        if (psm == 27u || psm == 36u) { high = 24u; }
        if (psm == 44u) { high = 28u; }
    }
    block += bp + ((y / ph) * (bw * 64u / pw) + x / pw) * 32u;
    if ((psm & 48u) == 48u) { block ^= 24u; }
    return (block * 2048u + column * bits + high) & 0x1FFFFFFu;
}

fn convert_word(slot: u32, xy: vec2<i32>, depth: bool, unit: f32) -> u32 {
    if (depth) {
        var d: f32;
        if (slot == 0u) { d = load_convert_depth0(xy); } else if (slot == 1u) { d = load_convert_depth1(xy); }
        else if (slot == 2u) { d = load_convert_depth2(xy); } else { d = load_convert_depth3(xy); }
        return select(u32(d * unit), 0xFFFFFFFFu, d >= 1.0);
    }
    var c: vec4<f32>;
    if (slot == 0u) { c = load_convert_colour0(xy); } else if (slot == 1u) { c = load_convert_colour1(xy); }
    else if (slot == 2u) { c = load_convert_colour2(xy); } else { c = load_convert_colour3(xy); }
    let u = vec4<u32>(round(c * 255.0));
    return u.r | u.g << 8u | u.b << 16u | u.a << 24u;
}

// A 16-bit surface's pixel at `at` (x | y << 8) of the page at `origin` (GS pixels), as the GS
// stores it: 5 bits a colour and the alpha bit.
fn convert_half(slot: u32, origin: vec2<u32>, at: u32, scale: u32) -> u32 {
    let p = (origin + vec2<u32>(at & 255u, at >> 8u)) * scale + scale / 2u;
    let w = convert_word(slot, vec2<i32>(p), false, 0.0);
    return (w >> 3u & 31u) | (w >> 11u & 31u) << 5u | (w >> 19u & 31u) << 10u | (w >> 31u) << 15u;
}

@compute @workgroup_size(8, 8, 1)
fn native_convert(@builtin(global_invocation_id) tid: vec3<u32>) {
    if (tid.x >= convert.size.x || tid.y >= convert.size.y) { return; }
    let psm = convert.tex.x;
    let a = address(psm, convert.tex.y, convert.tex.z, tid.x, tid.y);
    let word = a >> 5u;
    let page = (word >> 11u) & 511u;
    let source = (convert.pages[page >> 4u][(page >> 2u) & 3u] >> ((page & 3u) * 8u)) & 255u;
    if (source == 0u) {
        // The base texture's texel as it is.
        textureStore(native_converted, tid.xy, vec4<u32>(load_convert_base(vec2<i32>(tid.xy)), 0u, 0u, 0u));
        return;
    }
    let s = convert.slot[source - 1u];
    let rel = (page - s.x) & 511u;
    let bw = s.y;
    let scale = convert.tex.w >> ((source - 1u) * 8u) & 255u;
    var value: u32;
    if (s.z == 2u) {
        // A 16-bit colour surface (64 x 64 pixel pages): the word's two pixels, the low half first.
        let origin = vec2<u32>((rel % bw) * 64u, (rel / bw) * 64u);
        let half = 4096u + (word & 2047u) * 2u;
        value = convert_half(source - 1u, origin, native_inverse[half], scale) |
                convert_half(source - 1u, origin, native_inverse[half + 1u], scale) << 16u;
    } else {
        let at = native_inverse[select(0u, 2048u, s.z == 1u) + (word & 2047u)];
        let xy = vec2<i32>(i32(((rel % bw) * 64u + (at & 255u)) * scale + scale / 2u),
                           i32(((rel / bw) * 32u + (at >> 8u)) * scale + scale / 2u));
        value = convert_word(source - 1u, xy, s.z == 1u, bitcast<f32>(s.w));
    }
    let bits = bits_per_pixel(psm);
    let texa = convert.size.z;
    value = (value >> (a & 31u)) & select((1u << bits) - 1u, 0xFFFFFFFFu, bits == 32u);
    var rgba = value;
    if (bits <= 8u) { rgba = convert.palette[value >> 2u][value & 3u]; }
    else if ((psm & 7u) == 2u) {
        var alpha = select(texa & 255u, convert.size.w & 255u, (value & 0x8000u) != 0u);
        if ((texa & 0x8000u) != 0u && value == 0u) { alpha = 0u; }
        rgba = (value & 31u) << 3u | (value & 0x3E0u) << 6u | (value & 0x7C00u) << 9u | alpha << 24u;
    } else if ((psm & 7u) == 1u) {
        rgba = (value & 0xFFFFFFu) | select((texa & 255u) << 24u, 0u, (texa & 0x8000u) != 0u && (value & 0xFFFFFFu) == 0u);
    }
    textureStore(native_converted, tid.xy, vec4<u32>(rgba, 0u, 0u, 0u));
}

// Presentation (native_present): the scanned-out picture into the window's surface texture,
// filtered. The viewport is the whole surface (WebGPU keeps viewports inside the attachment), so
// the picture's rectangle (x, y, width, height in window pixels; twice as large and shifted for
// DHD_VIEW) maps each pixel centre to its uv here; the scissor cuts it to the picture.
// Widescreen fills the window's sides beside a 4:3 picture (a loading card, a movie; finding 140)
// with the same picture blurred and darkened: present_shrink first boils the picture's content
// (source: its uv rectangle, a movie's rows only) down to a small texture, then present_fill draws
// that over the whole window, the content's rectangle scaled to cover it (cover, window pixels),
// before present_pixel draws the sharp picture in the middle.
struct Present { rect: vec4<f32>, cover: vec4<f32>, source: vec4<f32>, }
@group(0) @binding(0) var<uniform> present: Present;
@group(1) @binding(0) var present_source: texture_2d<f32>;
@group(1) @binding(1) var present_sampler: sampler;

// The small texture's size (present.rs FILL_SIZE) and how dark the fill is.
const FILL_SIZE: vec2<f32> = vec2<f32>(64.0, 48.0);
const FILL_SHADE: f32 = 0.45;

@fragment
fn present_pixel(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let uv = (position.xy - present.rect.xy) / present.rect.zw;
    return vec4<f32>(textureSampleLevel(present_source, present_sampler, uv, 0.0).rgb, 1.0);
}

// A texel of the small texture: the mean of 4 x 4 filtered samples spread over its part of the
// content.
@fragment
fn present_shrink(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    var sum = vec3<f32>(0.0);
    for (var j = 0; j < 4; j++) {
        for (var i = 0; i < 4; i++) {
            let at = (floor(position.xy) + (vec2<f32>(f32(i), f32(j)) + 0.5) / 4.0) / FILL_SIZE;
            sum += textureSampleLevel(present_source, present_sampler, present.source.xy + at * present.source.zw, 0.0).rgb;
        }
    }
    return vec4<f32>(sum / 16.0, 1.0);
}

// The fill: the small texture under a 3 x 3 tent of filtered samples 1.5 texels apart, darkened.
@fragment
fn present_fill(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let uv = (position.xy - present.cover.xy) / present.cover.zw;
    var sum = vec3<f32>(0.0);
    for (var j = -1; j <= 1; j++) {
        for (var i = -1; i <= 1; i++) {
            let weight = f32((2 - abs(i)) * (2 - abs(j)));
            sum += weight * textureSampleLevel(present_source, present_sampler, uv + vec2<f32>(f32(i), f32(j)) * 1.5 / FILL_SIZE, 0.0).rgb;
        }
    }
    return vec4<f32>(sum / 16.0 * FILL_SHADE, 1.0);
}
