// GS raster operations over its original swizzled VRAM: raster.hlsl's compute entry points (main,
// local_copy, scanout) for the WebGPU backend (gpu_wgpu.c, finding 084). Hand-ported line by line;
// raster.hlsl and the D3D11 backend stay the oracle, and the results must match theirs bit for bit.
//
// Three things differ in form, not in result:
// - The oracle is compiled with IEEE strictness, so every a*b+c is a rounded multiply and then a
//   rounded add. WGSL has no `precise`, and FXC (wgpu's DX12 compiler) or a driver may fuse them
//   into one FMA. keep() passes each such product through an integer OR with a uniform zero, which
//   no compiler can see through, so the product is rounded on its own as in the oracle.
// - WGSL has no f64. The depth plane is evaluated with an exact software binary64 (sd_*, u32
//   limbs): each multiply and add is rounded to nearest-even like the oracle's precise double
//   dmul/dadd, so Z is the same integer by construction (finding 084, section W4).
// - A subtraction clamped at zero, (a > b) ? a - b : 0, passes its difference through keep_u(),
//   the same OR with the uniform zero. PowerVR Rogue's driver compiles SPIR-V through LLVM 9,
//   which turns that select into llvm.usub.sat, and its code writer cannot lower that intrinsic in a
//   compute shader: the pipeline crashes the game (finding 170). native_spirv checks every stage.

override EARLY_DEPTH: bool = false;
override EXCLUSIVE_WORD_STORE: bool = false;
override ADDRESS_SHIFT: bool = false;

// One GS state (registers and CLUT) for the primitives that point at it (Primitive.info.w): a
// batch can hold primitives of several states (gpu_wgpu.c, finding 084 W5).
struct State {
    regs: array<vec4<u32>, 128>,
    info: vec4<u32>,     // y bit 0: independent pixels
    palette: array<vec4<u32>, 64>,
}
struct Primitive {
    position: array<vec4<u32>, 3>,
    uv: array<vec4<u32>, 3>,
    stq: array<vec4<f32>, 3>,
    info: vec4<u32>,     // x: type, y: PRIM, z: vertices, w: its State in states
    bounds: vec4<u32>,
    depth: array<vec4<u32>, 2>,
}
// One dispatch's record (a dynamic offset): x: touched tiles, y: their list in tile_list, w:
// always 0 (keep()).
@group(0) @binding(0) var<uniform> batch: vec4<u32>;
@group(0) @binding(1) var<storage, read> texture_memory: array<u32>;
@group(0) @binding(2) var<storage, read_write> memory: array<atomic<u32>>;
@group(0) @binding(3) var<storage, read> primitives: array<Primitive>;
@group(0) @binding(4) var<storage, read> tile_list: array<u32>;
@group(0) @binding(5) var<uniform> display_state: array<vec4<u32>, 3>;
@group(0) @binding(6) var<uniform> copy_state: array<vec4<u32>, 3>;
@group(0) @binding(7) var screen: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(10) var<storage, read> states: array<State>;
var<private> state: u32;   // draw_pixel's primitive's State

fn keep(x: f32) -> f32 { return bitcast<f32>(bitcast<u32>(x) | batch.w); }
fn keep2(x: vec2<f32>) -> vec2<f32> { return bitcast<vec2<f32>>(bitcast<vec2<u32>>(x) | vec2<u32>(batch.w)); }
fn keep3(x: vec3<f32>) -> vec3<f32> { return bitcast<vec3<f32>>(bitcast<vec3<u32>>(x) | vec3<u32>(batch.w)); }
fn keep4(x: vec4<f32>) -> vec4<f32> { return bitcast<vec4<f32>>(bitcast<vec4<u32>>(x) | vec4<u32>(batch.w)); }
fn keep_u(x: u32) -> u32 { return x | batch.w; }

// D3D's ftoi/ftou: NaN gives 0 and out-of-range values saturate (a WGSL conversion's
// out-of-range result is its own).
fn f2i(x: f32) -> i32 {
    if ((bitcast<u32>(x) & 0x7FFFFFFFu) > 0x7F800000u) { return 0; }
    if (x >= 2147483648.0) { return 2147483647; }
    if (x <= -2147483648.0) { return i32(-2147483647) - 1; }
    return i32(x);
}
fn f2i2(x: vec2<f32>) -> vec2<i32> { return vec2<i32>(f2i(x.x), f2i(x.y)); }
fn f2i4(x: vec4<f32>) -> vec4<i32> { return vec4<i32>(f2i(x.x), f2i(x.y), f2i(x.z), f2i(x.w)); }
fn f2u(x: f32) -> u32 {
    if ((bitcast<u32>(x) & 0x7FFFFFFFu) > 0x7F800000u || x <= 0.0) { return 0u; }
    if (x >= 4294967296.0) { return 0xFFFFFFFFu; }
    return u32(x);
}

fn bits_per_pixel(psm: u32) -> u32 {
    if (psm == 1u || psm == 49u) { return 24u; }
    if ((psm & 7u) == 2u) { return 16u; }
    if (psm == 19u || psm == 27u) { return 8u; }
    if (psm == 20u || psm == 36u || psm == 44u) { return 4u; }
    return 32u;
}
fn pixel_mask(bits: u32) -> u32 { return select((1u << bits) - 1u, 0xFFFFFFFFu, bits == 32u); }

fn address(psm: u32, bp: u32, bw: u32, x: u32, y: u32) -> u32 {
    var pw = 64u; var ph = 32u; var bits = 32u; var high = 0u; var block = 0u; var column = 0u;
    var bx = 0u; var by = 0u;
    if (psm == 19u || psm == 20u) { pw = 128u; ph = select(128u, 64u, psm == 19u); }
    else if ((psm & 7u) == 2u) { ph = 64u; }
    var px: u32; var py: u32;
    let sx = select(6u, 7u, pw == 128u);
    let sy = select(select(5u, 6u, ph == 64u), 7u, ph == 128u);
    if (ADDRESS_SHIFT) {
        // The swizzle below selects only bits inside a page in each format;
        // higher coordinate bits are consumed solely by the page calculation.
        px = x; py = y;
    } else {
        px = x % pw; py = y % ph;
    }
    if (psm == 19u) {
        bits = 8u; bx = px / 16u; by = py / 16u;
        block = (bx & 1u) | ((by & 1u) << 1u) | ((bx & 2u) << 1u) | ((by & 2u) << 2u) | ((bx & 4u) << 2u);
        column = ((px & 1u) << 2u) | ((px & 2u) << 3u) | ((px & 4u) << 3u) | ((px & 8u) >> 2u) |
                 ((py & 1u) << 3u) | ((py & 2u) >> 1u) | ((py & 4u) << 4u) | ((py & 8u) << 4u);
        column ^= (((py >> 1u) ^ (py >> 2u)) & 1u) << 5u;
    } else if (psm == 20u) {
        bits = 4u; bx = px / 32u; by = py / 16u;
        block = (by & 1u) | ((bx & 1u) << 1u) | ((by & 2u) << 1u) | ((bx & 2u) << 2u) | ((by & 4u) << 2u);
        column = ((px & 1u) << 3u) | ((px & 2u) << 4u) | ((px & 4u) << 4u) | ((px & 8u) >> 2u) | ((px & 16u) >> 2u) |
                 ((py & 1u) << 4u) | ((py & 2u) >> 1u) | ((py & 4u) << 5u) | ((py & 8u) << 5u);
        column ^= (((py >> 1u) ^ (py >> 2u)) & 1u) << 6u;
    } else if ((psm & 7u) == 2u) {
        bits = 16u; bx = px / 16u; by = py / 8u;
        if ((psm & 8u) != 0u) {
            block = (by & 1u) | ((bx & 1u) << 1u) | (by & 4u) | ((by & 2u) << 2u) | ((bx & 2u) << 3u);
        } else {
            block = (by & 1u) | ((bx & 1u) << 1u) | ((by & 2u) << 1u) | ((bx & 2u) << 2u) | ((by & 4u) << 2u);
        }
        column = ((px & 1u) << 1u) | ((px & 2u) << 2u) | ((px & 4u) << 2u) | ((px & 8u) >> 3u) |
                 ((py & 1u) << 2u) | ((py & 6u) << 4u);
    } else {
        bx = px / 8u; by = py / 8u;
        block = (bx & 1u) | ((by & 1u) << 1u) | ((bx & 2u) << 1u) | ((by & 2u) << 2u) | ((bx & 4u) << 2u);
        column = (px & 1u) | ((px & 6u) << 1u) | ((py & 1u) << 1u) | ((py & 6u) << 3u);
        if (psm == 27u || psm == 36u) { high = 24u; }
        if (psm == 44u) { high = 28u; }
    }
    if (ADDRESS_SHIFT) {
        // Keep the original uint multiply before shifting, including its wrap.
        block += bp + ((y >> sy) * ((bw * 64u) >> sx) + (x >> sx)) * 32u;
    } else {
        block += bp + ((y / ph) * (bw * 64u / pw) + x / pw) * 32u;
    }
    if ((psm & 48u) == 48u) { block ^= 24u; }
    return (block * 2048u + column * bits + high) & 0x1FFFFFFu;
}

// raster.hlsl's pixel(..., tex): texture reads see texture_memory (the snapshot; VRAM itself in
// scanout), the rest read VRAM.
fn texture_pixel(psm: u32, bp: u32, bw: u32, xy: vec2<u32>) -> u32 {
    let a = address(psm, bp, bw, xy.x, xy.y);
    return (texture_memory[a >> 5u] >> (a & 31u)) & pixel_mask(bits_per_pixel(psm));
}
fn memory_pixel(psm: u32, bp: u32, bw: u32, xy: vec2<u32>) -> u32 {
    let a = address(psm, bp, bw, xy.x, xy.y);
    return (atomicLoad(&memory[a >> 5u]) >> (a & 31u)) & pixel_mask(bits_per_pixel(psm));
}

fn write_pixel_impl(psm: u32, bp: u32, bw: u32, xy: vec2<u32>, value_in: u32, preserve: u32, exclusive: bool) {
    let a = address(psm, bp, bw, xy.x, xy.y);
    let bits = bits_per_pixel(psm);
    let mask = (pixel_mask(bits) & ~preserve) << (a & 31u);
    let value = (value_in << (a & 31u)) & mask;
    let word = a >> 5u;
    var old = atomicLoad(&memory[word]);
    if (EXCLUSIVE_WORD_STORE) {
        // The raster host proof gives this invocation the entire storage word.
        // Keep an explicit canonical whitelist; packed formats always use CAS.
        if (exclusive && (psm == 0u || psm == 1u || psm == 48u || psm == 49u)) {
            atomicStore(&memory[word], (old & ~mask) | value);
            return;
        }
    }
    // Packed pixels may share a word; masked writes must be atomic. A weak compare-exchange may
    // fail without another writer, so the loop tests `exchanged`, not the old value.
    loop {
        let r = atomicCompareExchangeWeak(&memory[word], old, (old & ~mask) | value);
        if (r.exchanged) { break; }
        old = r.old_value;
    }
}
// Non-raster callers never inherit the raster cbuffer's ownership flag.
fn write_pixel(psm: u32, bp: u32, bw: u32, xy: vec2<u32>, value: u32, preserve: u32) {
    write_pixel_impl(psm, bp, bw, xy, value, preserve, false);
}
fn write_raster_pixel(psm: u32, bp: u32, bw: u32, xy: vec2<u32>, value: u32, preserve: u32) {
    write_pixel_impl(psm, bp, bw, xy, value, preserve, (states[state].info.y & 1u) != 0u);
}

fn rgba(v: u32) -> vec4<i32> {
    return vec4<i32>(i32(v & 255u), i32((v >> 8u) & 255u), i32((v >> 16u) & 255u), i32(v >> 24u));
}
fn pack32(v: vec4<i32>) -> u32 { return u32(v.r) | (u32(v.g) << 8u) | (u32(v.b) << 16u) | (u32(v.a) << 24u); }
fn pack16(v: u32) -> u32 { return ((v >> 3u) & 31u) | ((v >> 6u) & 0x3E0u) | ((v >> 9u) & 0x7C00u) | ((v >> 16u) & 0x8000u); }
fn expand(value: u32, psm: u32, tex: bool) -> u32 {
    if ((psm & 7u) == 2u) {
        var a = select(states[state].regs[59].x & 255u, states[state].regs[59].y & 255u, (value & 0x8000u) != 0u);
        if (!tex) { a = select(0u, 128u, (value & 0x8000u) != 0u); }
        if (tex && (states[state].regs[59].x & 0x8000u) != 0u && value == 0u) { a = 0u; }
        return ((value & 31u) << 3u) | ((value & 0x3E0u) << 6u) | ((value & 0x7C00u) << 9u) | (a << 24u);
    }
    if ((psm & 7u) == 1u) {
        var a = select(128u, states[state].regs[59].x & 255u, tex);
        if (tex && (states[state].regs[59].x & 0x8000u) != 0u && value == 0u) { a = 0u; }
        return value | (a << 24u);
    }
    return value;
}

fn clamp_coordinate(v: i32, mode: u32, size: u32, low: u32, high: u32) -> i32 {
    if (mode == 0u) { return v & i32(size - 1u); }
    if (mode == 1u) { return clamp(v, 0, i32(size) - 1); }
    if (mode == 2u) { return clamp(v, i32(low), i32(high)); }
    return (v & i32(low)) | i32(high);
}

fn texel(xy_in: vec2<i32>, ctxt: u32, level: u32) -> vec4<i32> {
    let t = states[state].regs[6u + ctxt].xy;
    let c = states[state].regs[8u + ctxt].xy;
    var tw = (t.x >> 26u) & 15u;
    var th = (t.x >> 30u) | ((t.y & 3u) << 2u);
    tw = select(0u, keep_u(tw - level), tw > level); th = select(0u, keep_u(th - level), th > level);
    var xy = xy_in;
    xy.x = clamp_coordinate(xy.x, c.x & 3u, 1u << tw, ((c.x >> 4u) & 1023u) >> level, ((c.x >> 14u) & 1023u) >> level);
    xy.y = clamp_coordinate(xy.y, (c.x >> 2u) & 3u, 1u << th, ((c.x >> 24u) | ((c.y & 3u) << 8u)) >> level, ((c.y >> 2u) & 1023u) >> level);
    let psm = (t.x >> 20u) & 63u;
    var location = t.x;
    if (level != 0u) {
        let mip = states[state].regs[select(54u, 52u, level <= 3u) + ctxt].xy;
        let part = (level - 1u) % 3u;
        location = select(select(mip.y >> 8u, (mip.x >> 20u) | (mip.y << 12u), part == 1u), mip.x, part == 0u);
    }
    var v = texture_pixel(psm, location & 0x3FFFu, (location >> 14u) & 63u, vec2<u32>(xy) & vec2<u32>(2047u));
    if (psm == 19u || psm == 20u || psm == 27u || psm == 36u || psm == 44u) {
        v = states[state].palette[(v & 255u) >> 2u][v & 3u];
    } else {
        v = expand(v, psm, true);
    }
    return rgba(v);
}

fn sample_texture(s: vec2<f32>, q: f32, ctxt: u32) -> vec4<i32> {
    let tex1 = states[state].regs[20u + ctxt].xy;
    let mmin = (tex1.x >> 6u) & 7u;
    let mxl = min((tex1.x >> 2u) & 7u, 6u);
    let k = f32(i32(tex1.y << 20u) >> 20u) / 16.0;
    var lod = k;
    if ((tex1.x & 1u) == 0u) {
        lod = lod - keep(log2(max(abs(q), 1.17549435e-38)) * f32(1u << ((tex1.x >> 19u) & 3u)));
    }
    // MXL=0 forces magnification filtering (verified on PS2 by PCSX2).
    var filtered: bool;
    if (mxl == 0u || lod <= 0.0) { filtered = (tex1.x & 32u) != 0u; }
    else { filtered = mmin == 1u || mmin == 4u || mmin == 5u; }
    // The GS keeps 16 fractional LOD bits; nearest rounds at half a level.
    let fixed_lod = f2u(clamp(lod, 0.0, f32(mxl)) * 65536.0);
    var level = 0u; var levels = 1u;
    if (mmin < 2u || mmin > 5u || mxl == 0u) { }
    else if ((mmin & 1u) == 0u) { level = (fixed_lod + 32768u) >> 16u; }
    else { level = fixed_lod >> 16u; levels = 2u; }
    // One texel() inside two loops with run-time counts (levels 1 or 2, taps 1 or 4): FXC
    // inlines every call and unrolls what it can, and the 16 texel() copies of unrolled
    // sampling took it two minutes (finding 084 §14). The bilinear sum is the same integer sum,
    // ((a(16-fx) + b fx)(16-fy) + (c(16-fx) + d fx) fy) >> 8 expanded; a point sample is its
    // only texel with weight 256 (texels are 0..255, so >> 8 gives it back exactly).
    // Two variables, not an array indexed by i: FXC got that local array wrong (finding 084 §14).
    let taps = select(1u, 4u, filtered);
    var first = vec4<i32>(0); var second = vec4<i32>(0);
    for (var i = 0u; i < levels; i++) {
        let l = select(level, min(level + 1u, mxl), i == 1u);
        let st = keep2(s * exp2(-f32(l)));
        var p: vec2<i32>; var f = vec2<i32>(0);
        if (filtered) { let at = st - 0.5; p = f2i2(floor(at)); f = f2i2(fract(at) * 16.0); }
        else { p = f2i2(floor(st)); }
        var sum = vec4<i32>(0);
        for (var j = 0u; j < taps; j++) {
            let o = vec2<i32>(i32(j & 1u), i32(j >> 1u));
            sum += texel(p + o, ctxt, l) * (select(16 - f.x, f.x, o.x == 1) * select(16 - f.y, f.y, o.y == 1));
        }
        if (i == 0u) { first = sum >> vec4<u32>(8u); } else { second = sum >> vec4<u32>(8u); }
    }
    if (levels == 1u) { return first; }
    return first + (((second - first) * i32((fixed_lod & 65535u) >> 1u)) >> vec4<u32>(15u));
}

fn edge(a: vec2<i32>, b: vec2<i32>, p: vec2<i32>) -> f32 {
    let d = b - a;
    let q = p - a;
    // Each GS coordinate difference fits 16 unsigned bits. Subtract magnitudes
    // before conversion so even nearly cancelling products retain the exact sign.
    let first = u32(abs(d.x)) * u32(abs(q.y));
    let second = u32(abs(d.y)) * u32(abs(q.x));
    let first_negative = (d.x < 0) != (q.y < 0);
    let second_negative = (d.y < 0) != (q.x < 0);
    var result: f32;
    if (first_negative == second_negative) {
        if (first >= second) { result = f32(first - second); } else { result = -f32(second - first); }
    } else {
        result = f32(first) + f32(second);
    }
    return select(result, -result, first_negative);
}
fn top_left(a: vec2<i32>, b: vec2<i32>) -> bool { let d = b - a; return d.y < 0 || (d.y == 0 && d.x > 0); }
fn inside(e: f32, a: vec2<i32>, b: vec2<i32>, sign: i32) -> bool {
    var tl: bool;
    if (sign > 0) { tl = top_left(a, b); } else { tl = top_left(b, a); }
    return e * f32(sign) > 0.0 || (e == 0.0 && tl);
}

// ---- exact binary64 for the depth plane ----
// A finite double is m * 2^e with m in [2^52, 2^53) as (m.x low 32 bits, m.y high 21), or m = 0.
// The depth plane's inputs and results are never subnormal, infinite or NaN (draw() divides
// integer differences by a nonzero integer area), so those encodings are not handled.
struct Sd { m: vec2<u32>, e: i32, neg: bool, }

fn sd_decode(lo: u32, hi: u32) -> Sd {
    let ef = (hi >> 20u) & 0x7FFu;
    if (ef == 0u) { return Sd(vec2<u32>(0u), 0, false); }
    return Sd(vec2<u32>(lo, (hi & 0xFFFFFu) | 0x100000u), i32(ef) - 1075, (hi >> 31u) != 0u);
}
fn mul32(a: u32, b: u32) -> vec2<u32> {
    let a0 = a & 0xFFFFu; let a1 = a >> 16u; let b0 = b & 0xFFFFu; let b1 = b >> 16u;
    let p00 = a0 * b0; let p01 = a0 * b1; let p10 = a1 * b0; let p11 = a1 * b1;
    let mid = (p00 >> 16u) + (p01 & 0xFFFFu) + (p10 & 0xFFFFu);
    return vec2<u32>((p00 & 0xFFFFu) | (mid << 16u), p11 + (p01 >> 16u) + (p10 >> 16u) + (mid >> 16u));
}
// 96-bit integers: x low, z high.
fn w_shr(p: vec3<u32>, s: u32) -> vec3<u32> {   // s < 96
    var v = p; var n = s;
    if (n >= 64u) { v = vec3<u32>(v.z, 0u, 0u); n -= 64u; } else if (n >= 32u) { v = vec3<u32>(v.y, v.z, 0u); n -= 32u; }
    if (n == 0u) { return v; }
    return vec3<u32>((v.x >> n) | (v.y << (32u - n)), (v.y >> n) | (v.z << (32u - n)), v.z >> n);
}
fn w_shl(p: vec3<u32>, s: u32) -> vec3<u32> {   // s < 96
    var v = p; var n = s;
    if (n >= 64u) { v = vec3<u32>(0u, 0u, v.x); n -= 64u; } else if (n >= 32u) { v = vec3<u32>(0u, v.x, v.y); n -= 32u; }
    if (n == 0u) { return v; }
    return vec3<u32>(v.x << n, (v.y << n) | (v.x >> (32u - n)), (v.z << n) | (v.y >> (32u - n)));
}
fn low_mask(n: u32) -> u32 { return select((1u << n) - 1u, 0xFFFFFFFFu, n >= 32u); }   // n <= 32
fn w_low_nonzero(p: vec3<u32>, n: u32) -> bool {   // any of bits [0, n), n <= 96
    if (n <= 32u) { return (p.x & low_mask(n)) != 0u; }
    if (n <= 64u) { return p.x != 0u || (p.y & low_mask(n - 32u)) != 0u; }
    return p.x != 0u || p.y != 0u || (p.z & low_mask(n - 64u)) != 0u;
}
fn w_bit(p: vec3<u32>, i: u32) -> bool {
    let limb = select(select(p.x, p.y, i >= 32u), p.z, i >= 64u);
    return ((limb >> (i & 31u)) & 1u) != 0u;
}
fn w_add(a: vec3<u32>, b: vec3<u32>) -> vec3<u32> {
    let x = a.x + b.x; let c0 = select(0u, 1u, x < a.x);
    let y1 = a.y + b.y; let c1 = select(0u, 1u, y1 < a.y);
    let y = y1 + c0; let c2 = select(0u, 1u, y < y1);
    return vec3<u32>(x, y, a.z + b.z + c1 + c2);
}
fn w_sub(a: vec3<u32>, b: vec3<u32>) -> vec3<u32> {   // a >= b
    let x = a.x - b.x; let b0 = select(0u, 1u, a.x < b.x);
    let y1 = a.y - b.y; let b1 = select(0u, 1u, a.y < b.y);
    let y = y1 - b0; let b2 = select(0u, 1u, y1 < b0);
    return vec3<u32>(x, y, a.z - b.z - b1 - b2);
}
fn w_less(a: vec3<u32>, b: vec3<u32>) -> bool {
    if (a.z != b.z) { return a.z < b.z; }
    if (a.y != b.y) { return a.y < b.y; }
    return a.x < b.x;
}
// p * 2^e rounded to a double, to nearest, ties to even. Any bits already lost below bit 0 must
// have been folded into bit 0 by the caller (with at least two bits between bit 0 and the
// rounding position).
fn sd_round(p: vec3<u32>, e: i32, neg: bool) -> Sd {
    var h: u32;
    if (p.z != 0u) { h = 64u + firstLeadingBit(p.z); }
    else if (p.y != 0u) { h = 32u + firstLeadingBit(p.y); }
    else if (p.x != 0u) { h = firstLeadingBit(p.x); }
    else { return Sd(vec2<u32>(0u), 0, false); }
    if (h <= 52u) {
        let m = w_shl(p, 52u - h);
        return Sd(m.xy, e - i32(52u - h), neg);
    }
    let s = h - 52u;
    var m = w_shr(p, s);
    if (w_bit(p, s - 1u) && (w_low_nonzero(p, s - 1u) || (m.x & 1u) != 0u)) {
        m = w_add(m, vec3<u32>(1u, 0u, 0u));
        if (m.y == 0x200000u) { return Sd(vec2<u32>(0u, 0x100000u), e + i32(s) + 1, neg); }
    }
    return Sd(m.xy, e + i32(s), neg);
}
// a * n, n an integer that is exactly a float (the oracle's (double)(float)int).
fn sd_mul_int(a: Sd, n: f32) -> Sd {
    let k = f2u(abs(n));
    if (k == 0u || (a.m.x | a.m.y) == 0u) { return Sd(vec2<u32>(0u), 0, false); }
    let lo = mul32(a.m.x, k);
    let hi = mul32(a.m.y, k);
    return sd_round(w_add(vec3<u32>(lo, 0u), vec3<u32>(0u, hi)), a.e, a.neg != (n < 0.0));
}
fn sd_add(a: Sd, b: Sd) -> Sd {
    if ((a.m.x | a.m.y) == 0u) { return b; }
    if ((b.m.x | b.m.y) == 0u) { return a; }
    var big = a; var small = b;
    if (a.e < b.e) { big = b; small = a; }
    let d = u32(big.e - small.e);
    // 40 guard bits below each 53-bit significand.
    let x = w_shl(vec3<u32>(big.m, 0u), 40u);
    var y = w_shl(vec3<u32>(small.m, 0u), 40u);
    if (d >= 93u) { y = vec3<u32>(1u, 0u, 0u); }
    else if (d > 0u) {
        let lost = w_low_nonzero(y, d);
        y = w_shr(y, d);
        if (lost) { y.x |= 1u; }
    }
    // One sd_round call (FXC inlines each one; finding 084 §14).
    var sum: vec3<u32>; var neg = big.neg;
    if (big.neg == small.neg) { sum = w_add(x, y); }
    else if (w_less(x, y)) { sum = w_sub(y, x); neg = small.neg; }
    else { sum = w_sub(x, y); }
    return sd_round(sum, big.e - 40, neg);
}
// raster.hlsl's depth_integer: the integer part, clamped to [0, 2^32 - 1].
fn sd_depth_integer(z: Sd) -> u32 {
    if ((z.m.x | z.m.y) == 0u || z.neg || z.e + 52 < 0) { return 0u; }
    if (z.e + 52 >= 32) { return 0xFFFFFFFFu; }
    let s = u32(-z.e);   // 21..52
    if (s >= 32u) { return z.m.y >> (s - 32u); }
    return (z.m.x >> s) | (z.m.y << (32u - s));
}
// precise double depth = d0 + dzdx * (double)(float)dx + dzdy * (double)(float)dy, as dmul/dadd.
// The two terms go through one loop so FXC inlines sd_add and sd_mul_int once each; its count is
// 2 | batch.w (batch.w is 0, see keep) so the compiler cannot unroll it. sd_add is commutative
// (the larger exponent leads; on equal exponents the sign rule gives the same result), so adding
// the dx term as z + term is the same as the oracle's term + z.
fn plane_depth(depth: array<vec4<u32>, 2>, dx: i32, dy: i32) -> u32 {
    var z = sd_decode(depth[0].x, depth[0].y);
    let terms = 2u | batch.w;
    for (var k = 0u; k < terms; k++) {
        let gradient = select(depth[1].xy, depth[0].zw, k == 0u);
        z = sd_add(z, sd_mul_int(sd_decode(gradient.x, gradient.y), f32(select(dy, dx, k == 0u))));
    }
    return sd_depth_integer(z);
}

fn draw_pixel(xy: vec2<u32>, index: u32) {
    var d = primitives[index];   // a variable: WGSL indexes value arrays by constants only
    state = d.info.w;
    if (any(xy < d.bounds.xy) || any(xy >= d.bounds.xy + d.bounds.zw)) { return; }
    let scanmask = states[state].regs[34].x & 3u;
    if ((scanmask == 2u && (xy.y & 1u) == 0u) || (scanmask == 3u && (xy.y & 1u) != 0u)) { return; }
    let prim = d.info.y; let ctxt = (prim >> 9u) & 1u; let type_ = d.info.x; let last = d.info.z - 1u;
    let p = vec2<i32>(xy) * 16;
    let a = vec2<i32>(d.position[0].xy); let b = vec2<i32>(d.position[1].xy); let c = vec2<i32>(d.position[2].xy);
    var weight = vec3<f32>(1.0, 0.0, 0.0);
    var sprite_fraction = vec2<f32>(0.0);
    if (type_ == 0u) {
        if (any((a >> vec2<u32>(4u)) != vec2<i32>(xy))) { return; }
    } else if (type_ == 6u) {
        if (any(p < min(a, b)) || any(p >= max(a, b))) { return; }
        sprite_fraction = vec2<f32>(p - a) / vec2<f32>(b - a);
        weight = vec3<f32>(0.0, 1.0, 0.0);
    } else if (type_ == 1u || type_ == 2u) {
        let dd = b - a;
        let major = max(abs(dd.x), abs(dd.y));
        if (major == 0) { return; }
        var along: f32;
        if (abs(dd.x) >= abs(dd.y)) { along = f32(p.x - a.x) / f32(dd.x); } else { along = f32(p.y - a.y) / f32(dd.y); }
        if (along < 0.0 || along >= 1.0 || abs(edge(a, b, p)) >= f32(major) * 8.0) { return; }
        weight = vec3<f32>(1.0 - along, along, 0.0);
    } else {
        if (any(p < min(a, min(b, c))) || any(p > max(a, max(b, c)))) { return; }
        let area = edge(a, b, c); let e0 = edge(b, c, p); let e1 = edge(c, a, p); let e2 = edge(a, b, p);
        if (area == 0.0) { return; }
        let sign = select(-1, 1, area > 0.0);
        if (!inside(e0, b, c, sign) || !inside(e1, c, a, sign) || !inside(e2, a, b, sign)) { return; }
        weight = vec3<f32>(e0, e1, e2) / area;
    }
    var color = rgba(d.position[last].w);
    if ((prim & 8u) != 0u && type_ != 6u) {
        color = f2i4((keep4(vec4<f32>(rgba(d.position[0].w)) * weight.x) + keep4(vec4<f32>(rgba(d.position[1].w)) * weight.y)) +
                     keep4(vec4<f32>(rgba(d.position[2].w)) * weight.z));
    }
    var z = d.position[last].z;
    let constant_z = d.position[0].z == d.position[1].z && (d.info.z < 3u || d.position[0].z == d.position[2].z);
    if (type_ != 0u && type_ != 6u && !constant_z) {
        z = plane_depth(d.depth, p.x - a.x, p.y - a.y);
    }
    // info.y bit 0 is the host's independent_pixels proof for this dispatch.
    // Unsafe aliases retain the original late load: another pixel may write Z
    // while this invocation shades. Texture reads use a separate snapshot.
    let early_depth = EARLY_DEPTH && (states[state].info.y & 1u) != 0u;
    if (early_depth) {
        let early_test = states[state].regs[71u + ctxt].x;
        if ((early_test & 0x10000u) != 0u) {
            let early_frame = states[state].regs[76u + ctxt].x; let early_zbuf = states[state].regs[78u + ctxt].x;
            let early_zpsm = ((early_zbuf >> 24u) & 15u) | 48u;
            let early_z = min(z, pixel_mask(bits_per_pixel(early_zpsm)));
            let old_z = memory_pixel(early_zpsm, (early_zbuf & 511u) * 32u, (early_frame >> 16u) & 63u, xy);
            let ztest = (early_test >> 17u) & 3u;
            if (ztest == 0u || (ztest == 2u && early_z < old_z) || (ztest == 3u && early_z <= old_z)) { return; }
        }
    }
    if ((prim & 16u) != 0u) {
        var s: vec2<f32>;
        var interp = (keep3(d.stq[0].xyz * weight.x) + keep3(d.stq[1].xyz * weight.y)) + keep3(d.stq[2].xyz * weight.z);
        if (type_ == 6u) {
            interp = vec3<f32>(d.stq[0].xy + keep2((d.stq[1].xy - d.stq[0].xy) * sprite_fraction), d.stq[1].z);
        }
        if ((prim & 256u) != 0u) {
            let uv0 = vec2<f32>(d.uv[0].xy); let uv1 = vec2<f32>(d.uv[1].xy); let uv2 = vec2<f32>(d.uv[2].xy);
            s = ((keep2(uv0 * weight.x) + keep2(uv1 * weight.y)) + keep2(uv2 * weight.z)) / 16.0;
            if (type_ == 6u) { s = (uv0 + keep2((uv1 - uv0) * sprite_fraction)) / 16.0; }
        } else {
            let t = states[state].regs[6u + ctxt].xy;
            let size = vec2<f32>(f32(1u << ((t.x >> 26u) & 15u)), f32(1u << ((t.x >> 30u) | ((t.y & 3u) << 2u))));
            s = keep2(interp.xy * size) / select(interp.z, 1.17549435e-38, interp.z == 0.0);
        }
        let tex = sample_texture(s, interp.z, ctxt);
        let tfx = (states[state].regs[6u + ctxt].y >> 3u) & 3u; let tcc = (states[state].regs[6u + ctxt].y >> 2u) & 1u;
        let alpha = color.a;
        if (tfx == 0u) { color = (color * tex) >> vec4<u32>(7u); }
        else if (tfx == 1u) { color = tex; }
        else {
            color = vec4<i32>(((color.rgb * tex.rgb) >> vec3<u32>(7u)) + alpha, select(tex.a, tex.a + alpha, tfx == 2u));
        }
        if (tcc == 0u) { color.a = alpha; }
        color = clamp(color, vec4<i32>(0), vec4<i32>(255));
    }
    if ((prim & 32u) != 0u) {
        let fog = f2u((keep(f32(d.uv[0].z) * weight.x) + keep(f32(d.uv[1].z) * weight.y)) + keep(f32(d.uv[2].z) * weight.z));
        let fc = rgba(states[state].regs[61].x).rgb;
        color = vec4<i32>(fc + (((color.rgb - fc) * i32(fog)) >> vec3<u32>(8u)), color.a);
    }
    let test = states[state].regs[71u + ctxt].x;
    var write_frame = true; var write_z = (states[state].regs[78u + ctxt].y & 1u) == 0u; var rgb_only = false;
    if ((test & 1u) != 0u) {
        let atst = (test >> 1u) & 7u; let ref_ = i32((test >> 4u) & 255u);
        let alpha_pass = atst == 1u || (atst == 2u && color.a < ref_) || (atst == 3u && color.a <= ref_) ||
                         (atst == 4u && color.a == ref_) || (atst == 5u && color.a >= ref_) ||
                         (atst == 6u && color.a > ref_) || (atst == 7u && color.a != ref_);
        if (!alpha_pass) {
            let fail = (test >> 12u) & 3u;
            if (fail == 0u) { return; }
            write_frame = fail != 2u; write_z = write_z && fail == 2u; rgb_only = fail == 3u;
        }
    }
    let frame = states[state].regs[76u + ctxt].x; let bw = (frame >> 16u) & 63u; let bp = (frame & 511u) * 32u; let psm = (frame >> 24u) & 63u;
    let zbuf = states[state].regs[78u + ctxt].x; let zpsm = ((zbuf >> 24u) & 15u) | 48u; let zbp = (zbuf & 511u) * 32u;
    z = min(z, pixel_mask(bits_per_pixel(zpsm)));
    let raw = memory_pixel(psm, bp, bw, xy);
    let dst = rgba(expand(raw, psm, false));
    if ((test & 0x4000u) != 0u && psm != 1u && ((dst.a >> 7u) != i32((test >> 15u) & 1u))) { return; }
    if ((test & 0x10000u) != 0u) {
        if (!early_depth) {
            let ztest = (test >> 17u) & 3u; let old_z = memory_pixel(zpsm, zbp, bw, xy);
            if (ztest == 0u || (ztest == 2u && z < old_z) || (ztest == 3u && z <= old_z)) { return; }
        }
    } else { write_z = false; }
    if (write_frame) {
        if ((prim & 64u) != 0u && ((states[state].regs[73].x & 1u) == 0u || (color.a & 128) != 0)) {
            let alpha = states[state].regs[66u + ctxt].x;
            let aa = alpha & 3u; let bb = (alpha >> 2u) & 3u; let cc = (alpha >> 4u) & 3u; let ddd = (alpha >> 6u) & 3u;
            let av = select(select(vec3<i32>(0), dst.rgb, aa == 1u), color.rgb, aa == 0u);
            let bv = select(select(vec3<i32>(0), dst.rgb, bb == 1u), color.rgb, bb == 0u);
            let cv = select(select(i32(states[state].regs[66u + ctxt].y & 255u), dst.a, cc == 1u), color.a, cc == 0u);
            let dv = select(select(vec3<i32>(0), dst.rgb, ddd == 1u), color.rgb, ddd == 0u);
            color = vec4<i32>((((av - bv) * cv) >> vec3<u32>(7u)) + dv, color.a);
        }
        if ((psm & 7u) == 2u && (states[state].regs[69].x & 1u) != 0u) {
            let index = (xy.y & 3u) * 4u + (xy.x & 3u);
            let word = states[state].regs[68][index >> 3u];
            let dither = i32(((word >> ((index & 7u) * 4u)) & 7u) << 29u) >> 29u;
            color = vec4<i32>(color.rgb + dither, color.a);
        }
        if ((states[state].regs[70].x & 1u) != 0u) { color = clamp(color, vec4<i32>(0), vec4<i32>(255)); } else { color = color & vec4<i32>(255); }
        if ((states[state].regs[74u + ctxt].x & 1u) != 0u) { color.a |= 128; }
        var mask = states[state].regs[76u + ctxt].y | select(0u, 0xFF000000u, rgb_only);
        var value = pack32(color);
        if ((psm & 7u) == 2u) { value = pack16(value); mask = pack16(mask); }
        write_raster_pixel(psm, bp, bw, xy, value, mask);
    }
    if (write_z) { write_raster_pixel(zpsm, zbp, bw, xy, z, 0u); }
}

// One workgroup per touched 8x8 tile: tile_list[batch.y + 3n] is tile n's position (x | y << 16,
// in tiles), then the first word and count of its primitive indices, in submission order.
@compute @workgroup_size(8, 8, 1)
fn main(@builtin(local_invocation_id) local: vec3<u32>, @builtin(workgroup_id) group: vec3<u32>,
        @builtin(num_workgroups) groups: vec3<u32>) {
    let n = group.y * groups.x + group.x;
    if (n >= batch.x) { return; }
    let entry = batch.y + n * 3u;
    let tile = tile_list[entry];
    let xy = vec2<u32>(tile & 0xFFFFu, tile >> 16u) * 8u + local.xy;
    let first = tile_list[entry + 1u];
    let count = tile_list[entry + 2u];
    for (var i = 0u; i < count; i++) { draw_pixel(xy, tile_list[first + i]); }
}

// A local-to-local transfer (TRXDIR 2) between rectangles on different pages: each pixel is
// independent, so the GS's scan order (TRXPOS DIR) does not matter.
@compute @workgroup_size(8, 8, 1)
fn local_copy(@builtin(global_invocation_id) tid: vec3<u32>) {
    let a = copy_state[0]; let b = copy_state[1]; let c = copy_state[2];
    if (tid.x >= c.x || tid.y >= c.y) { return; }
    let v = memory_pixel(a.x, a.y, a.z, (vec2<u32>(a.w, b.x) + tid.xy) & vec2<u32>(2047u));
    write_pixel(b.y, b.z, b.w, (vec2<u32>(c.z, c.w) + tid.xy) & vec2<u32>(2047u), v, 0u);
}

// Scanout stays on the GPU. texture_memory is VRAM itself here.
@compute @workgroup_size(8, 8, 1)
fn scanout(@builtin(global_invocation_id) tid: vec3<u32>) {
    let a = display_state[0]; let b = display_state[1];
    if (tid.x >= b.y || tid.y >= b.z) { return; }
    let xy = vec2<u32>(a.w + tid.x, b.x + (tid.y >> b.w)) & vec2<u32>(2047u);
    var v = texture_pixel(a.x, a.y, a.z, xy);
    if (a.x == 2u || a.x == 10u) { v = ((v & 31u) << 3u) | ((v & 0x3E0u) << 6u) | ((v & 0x7C00u) << 9u); }
    textureStore(screen, tid.xy, vec4<f32>(f32(v & 255u), f32((v >> 8u) & 255u), f32((v >> 16u) & 255u), 255.0) / 255.0);
}

// ---- the depth harness (port/gs/test_depth.c): not used by the renderer ----
// Each case: the plane (as Primitive.depth), the pixel offset from vertex 0, and the same three
// doubles split into float pairs (hi = (float)v, lo = (float)(v - hi)) for the double-float
// evaluation the design proposed first. Output: x the exact (sd_*) Z, y the double-float Z.
struct DepthCase { depth: array<vec4<u32>, 2>, offset: vec4<i32>, pairs: array<vec4<f32>, 2>, }
@group(0) @binding(8) var<storage, read> depth_cases: array<DepthCase>;
@group(0) @binding(9) var<storage, read_write> depth_results: array<vec2<u32>>;

fn two_sum(a: f32, b: f32) -> vec2<f32> {
    let s = keep(a + b); let bb = keep(s - a);
    return vec2<f32>(s, keep(a - keep(s - bb)) + keep(b - bb));
}
fn split(a: f32) -> vec2<f32> {   // Veltkamp, 12+12 bits
    let t = keep(4097.0 * a); let hi = keep(t - keep(t - a));
    return vec2<f32>(hi, keep(a - hi));
}
fn two_prod(a: f32, b: f32) -> vec2<f32> {   // Dekker, no FMA
    let p = keep(a * b); let x = split(a); let y = split(b);
    let err = keep(keep(keep(keep(x.x * y.x) - p) + keep(x.x * y.y)) + keep(x.y * y.x)) + keep(x.y * y.y);
    return vec2<f32>(p, err);
}
fn dd_add(a: vec2<f32>, b: vec2<f32>) -> vec2<f32> {
    let s = two_sum(a.x, b.x); let t = two_sum(a.y, b.y);
    let u = two_sum(s.x, keep(s.y + t.x));
    return two_sum(u.x, keep(keep(u.y + t.y)));
}
fn dd_mul_f(a: vec2<f32>, b: f32) -> vec2<f32> {
    let p = two_prod(a.x, b);
    return two_sum(p.x, keep(p.y + keep(a.y * b)));
}
fn dd_depth_integer(z: vec2<f32>) -> u32 {
    if (z.x < 0.0 || (z.x == 0.0 && z.y <= 0.0)) { return 0u; }
    if (z.x >= 4294967296.0) { return 0xFFFFFFFFu; }
    // floor(hi + lo) = floor(hi) + floor((hi - floor(hi)) + lo); hi - floor(hi) is exact, and lo
    // can be hundreds when hi is near 2^32.
    let f = floor(z.x);
    let n = i32(floor(keep(z.x - f) + z.y));
    if (f >= 2147483648.0) { return u32(f) + u32(n); }
    return u32(max(i32(f) + n, 0));
}

@compute @workgroup_size(64, 1, 1)
fn depth_probe(@builtin(global_invocation_id) id: vec3<u32>) {
    if (id.x >= arrayLength(&depth_cases)) { return; }
    let k = depth_cases[id.x];
    let exact = plane_depth(k.depth, k.offset.x, k.offset.y);
    var z = vec2<f32>(k.pairs[0].x, k.pairs[0].y);
    z = dd_add(dd_mul_f(vec2<f32>(k.pairs[0].z, k.pairs[0].w), f32(k.offset.x)), z);
    z = dd_add(z, dd_mul_f(vec2<f32>(k.pairs[1].x, k.pairs[1].y), f32(k.offset.y)));
    depth_results[id.x] = vec2<u32>(exact, dd_depth_integer(z));
}
