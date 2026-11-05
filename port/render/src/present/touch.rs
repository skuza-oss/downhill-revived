//! The touch controls' overlay (finding 141), drawn after the game picture in surface pixels,
//! like the performance counter. C (port/hle/touch.c) lays the controls out in mm and hands over
//! a list of shapes each picture (`dhdr_surface_touch`); here each is one instanced quad whose
//! fragment shader draws it as a signed distance (discs with a two-tone rim, rings, rounded
//! boxes, the trick stick's sectors, the PS2 symbols, chevrons, stripes) or as a character of the
//! 8x8 font (`font8x8.rs`). Nothing here reads the game or the environment.

use crate::device::Gpu;
use super::font8x8;

/// Floats per shape: kind, x, y, a, b, c, d, 0, fill rgba, line rgba (C's `dhdr_surface_touch`).
pub const ITEM_FLOATS: usize = 16;
const ITEM_BYTES: u64 = (ITEM_FLOATS * 4) as u64;

#[derive(Default)]
pub struct TouchOverlay {
    items: Vec<f32>,
    width_mm: f32,
    height_mm: f32,
    objects: Option<Objects>,
    /// The shapes and the screen uniform last written to the GPU (written again only on a change).
    written: (Vec<f32>, [f32; 4]),
    /// Drawn in this picture's present pass already (`draw_in_pass`); `draw` then skips it once.
    drawn: bool,
}

struct Objects {
    pipe: wgpu::RenderPipeline,
    group: wgpu::BindGroup,
    uniforms: wgpu::Buffer,
    instances: wgpu::Buffer,
    capacity: u64,
    _font: wgpu::Texture,
}

impl TouchOverlay {
    /// The shapes for the next pictures (an empty list hides the overlay); the screen in mm.
    pub fn set(&mut self, items: &[f32], width_mm: f32, height_mm: f32) {
        self.items.clear();
        self.items.extend_from_slice(&items[..items.len() / ITEM_FLOATS * ITEM_FLOATS]);
        self.width_mm = width_mm;
        self.height_mm = height_mm;
    }

    /// The shapes' instances and the screen uniform on the GPU for a w x h frame (each written
    /// only when it changed); the number of shapes to draw, or None when there are none.
    fn prepare(&mut self, gpu: &Gpu, format: wgpu::TextureFormat, w: u32, h: u32) -> Option<u32> {
        if self.items.is_empty() || w == 0 || h == 0 || self.width_mm <= 0.0 || self.height_mm <= 0.0 { return None; }
        let o = self.objects.get_or_insert_with(|| Objects::new(gpu, format));
        let count = (self.items.len() / ITEM_FLOATS) as u64;
        if count * ITEM_BYTES > o.capacity {
            o.capacity = (count * ITEM_BYTES).next_power_of_two();
            o.instances = gpu.device.create_buffer(&wgpu::BufferDescriptor { label: Some("touch overlay shapes"), size: o.capacity,
                usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST, mapped_at_creation: false });
            self.written.0.clear();  /* a new buffer holds nothing yet */
        }
        if self.written.0 != self.items {
            let bytes: Vec<u8> = self.items.iter().flat_map(|f| f.to_le_bytes()).collect();
            gpu.queue.write_buffer(&o.instances, 0, &bytes);
            self.written.0.clone_from(&self.items);
        }
        let uniforms = [w as f32, h as f32, w as f32 / self.width_mm, h as f32 / self.height_mm];
        if self.written.1 != uniforms {
            let ub: Vec<u8> = uniforms.iter().flat_map(|f| f.to_le_bytes()).collect();
            gpu.queue.write_buffer(&o.uniforms, 0, &ub);
            self.written.1 = uniforms;
        }
        Some(count as u32)
    }

    fn record(&self, pass: &mut wgpu::RenderPass<'_>, count: u32) {
        let Some(o) = self.objects.as_ref() else { return };
        pass.set_pipeline(&o.pipe);
        pass.set_bind_group(0, &o.group, &[]);
        pass.set_vertex_buffer(0, o.instances.slice(..count as u64 * ITEM_BYTES));
        pass.draw(0..6, 0..count);
    }

    /// The overlay as the last draws of a pass already open on the w x h frame (the native
    /// renderer's present pass, finding 146): the same pixels as `draw`'s pass of its own after
    /// it, without a second command buffer, submission and load and store of the whole frame.
    /// The next `draw` (dhdr_surface_present's) skips it.
    pub fn draw_in_pass(&mut self, gpu: &Gpu, pass: &mut wgpu::RenderPass<'_>, format: wgpu::TextureFormat, w: u32, h: u32) {
        if let Some(count) = self.prepare(gpu, format, w, h) {
            pass.set_scissor_rect(0, 0, w, h);
            self.record(pass, count);
            self.drawn = true;
        }
    }

    pub fn draw(&mut self, gpu: &Gpu, target: &wgpu::TextureView, format: wgpu::TextureFormat, w: u32, h: u32) {
        if std::mem::take(&mut self.drawn) { return; }
        let Some(count) = self.prepare(gpu, format, w, h) else { return };
        let mut encoder = gpu.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("touch overlay") });
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("touch overlay"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment { view: target, depth_slice: None, resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store } })],
                depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None, multiview_mask: None,
            });
            self.record(&mut pass, count);
        }
        gpu.queue.submit([encoder.finish()]);
    }
}

impl Objects {
    fn new(gpu: &Gpu, format: wgpu::TextureFormat) -> Self {
        let shader = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor { label: Some("touch overlay"),
            source: wgpu::ShaderSource::Wgsl(SHADER.into()) });
        let attributes = wgpu::vertex_attr_array![0 => Float32x4, 1 => Float32x4, 2 => Float32x4, 3 => Float32x4];
        let pipe = gpu.device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("touch overlay"), layout: None,
            vertex: wgpu::VertexState { module: &shader, entry_point: Some("vs"), compilation_options: Default::default(),
                buffers: &[wgpu::VertexBufferLayout { array_stride: ITEM_BYTES, step_mode: wgpu::VertexStepMode::Instance, attributes: &attributes }] },
            fragment: Some(wgpu::FragmentState { module: &shader, entry_point: Some("fs"), compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState { format, blend: Some(wgpu::BlendState::ALPHA_BLENDING), write_mask: wgpu::ColorWrites::ALL })] }),
            primitive: Default::default(), depth_stencil: None, multisample: Default::default(), multiview_mask: None, cache: None,
        });
        // The font: the 95 glyphs side by side, one byte a pixel.
        let (fw, fh) = (8 * font8x8::COUNT as u32, 8u32);
        let mut pixels = vec![0u8; (fw * fh) as usize];
        for (g, rows) in font8x8::GLYPHS.iter().enumerate() {
            for (y, row) in rows.iter().enumerate() {
                for x in 0..8 {
                    if row & (1 << x) != 0 { pixels[y * fw as usize + g * 8 + x] = 255; }
                }
            }
        }
        let font = gpu.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("touch overlay font"), size: wgpu::Extent3d { width: fw, height: fh, depth_or_array_layers: 1 },
            mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2, format: wgpu::TextureFormat::R8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST, view_formats: &[],
        });
        gpu.queue.write_texture(wgpu::TexelCopyTextureInfo { texture: &font, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            &pixels, wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(fw), rows_per_image: Some(fh) },
            wgpu::Extent3d { width: fw, height: fh, depth_or_array_layers: 1 });
        let uniforms = gpu.device.create_buffer(&wgpu::BufferDescriptor { label: Some("touch overlay screen"), size: 16,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST, mapped_at_creation: false });
        let group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor { label: Some("touch overlay"), layout: &pipe.get_bind_group_layout(0),
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: uniforms.as_entire_binding() },
                       wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(&font.create_view(&Default::default())) }] });
        let capacity = 256 * ITEM_BYTES;
        let instances = gpu.device.create_buffer(&wgpu::BufferDescriptor { label: Some("touch overlay shapes"), size: capacity,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST, mapped_at_creation: false });
        Self { pipe, group, uniforms, instances, capacity, _font: font }
    }
}

// Kinds as port/hle/touch.h numbers them (TS_*). Shapes are in mm; `screen` is the surface's
// size in pixels and its pixels per mm.
pub(crate) const SHADER: &str = r#"
struct Screen { size: vec2f, per_mm: vec2f }
@group(0) @binding(0) var<uniform> screen: Screen;
@group(0) @binding(1) var font: texture_2d<f32>;

struct In { @location(0) p0: vec4f, @location(1) p1: vec4f, @location(2) fill: vec4f, @location(3) line: vec4f }
struct Out {
    @builtin(position) pos: vec4f,
    @location(0) local: vec2f,                 // mm from the shape's centre (y down)
    @location(1) @interpolate(flat) kind: f32,
    @location(2) @interpolate(flat) shape: vec4f,  // a, b, c, d
    @location(3) @interpolate(flat) fill: vec4f,
    @location(4) @interpolate(flat) line: vec4f,
}

const PI: f32 = 3.14159265;

fn extent(kind: f32, a: f32, b: f32) -> vec2f {
    let k = i32(kind);
    if (k == 2 || k == 10) { return vec2f(a, b); }           // rect, stripes: half sizes
    if (k == 9) { return vec2f(a * 0.5, a * 0.5); }           // a character: a high
    if (k == 3) { return vec2f(b, b); }                        // sector: outer radius
    if (k == 1) { return vec2f(a + b, a + b); }                // ring
    return vec2f(a, a);
}

@vertex fn vs(@builtin(vertex_index) i: u32, v: In) -> Out {
    let corners = array<vec2f, 6>(vec2f(-1., -1.), vec2f(1., -1.), vec2f(-1., 1.), vec2f(-1., 1.), vec2f(1., -1.), vec2f(1., 1.));
    let margin = 2.0 / min(screen.per_mm.x, screen.per_mm.y) + 0.4;   // room for the rims and the edge's blend
    let half = extent(v.p0.x, v.p0.w, v.p1.x) + vec2f(margin, margin);
    let local = corners[i] * half;
    let mm = v.p0.yz + local;
    let px = mm * screen.per_mm;
    var o: Out;
    o.pos = vec4f(px.x / screen.size.x * 2. - 1., 1. - px.y / screen.size.y * 2., 0., 1.);
    o.local = local;
    o.kind = v.p0.x;
    o.shape = vec4f(v.p0.w, v.p1.x, v.p1.y, v.p1.z);
    o.fill = v.fill;
    o.line = v.line;
    return o;
}

fn segment(p: vec2f, a: vec2f, b: vec2f) -> f32 {
    let pa = p - a; let ba = b - a;
    let h = clamp(dot(pa, ba) / dot(ba, ba), 0., 1.);
    return length(pa - ba * h);
}

fn box(p: vec2f, half: vec2f, r: f32) -> f32 {
    let q = abs(p) - half + vec2f(r, r);
    return length(max(q, vec2f(0., 0.))) + min(max(q.x, q.y), 0.) - r;
}

// An equilateral triangle pointing up, r half its side, centred on its centroid (Inigo Quilez's
// sdEquilateralTriangle, MIT; https://iquilezles.org/articles/distfunctions2d/), y down here.
fn triangle(p0: vec2f, r: f32) -> f32 {
    let k = sqrt(3.0);
    var p = vec2f(abs(p0.x) - r, -p0.y + r / k);
    if (p.x + k * p.y > 0.) { p = vec2f(p.x - k * p.y, -k * p.x - p.y) / 2.; }
    p.x = p.x - clamp(p.x, -2. * r, 0.);
    return -length(p) * sign(p.y);
}

fn over(base: vec4f, top: vec4f, cover: f32) -> vec4f {   // top over base, straight alpha
    let ta = top.a * cover;
    let a = ta + base.a * (1. - ta);
    if (a <= 0.) { return vec4f(0., 0., 0., 0.); }
    return vec4f((top.rgb * ta + base.rgb * base.a * (1. - ta)) / a, a);
}

@fragment fn fs(v: Out) -> @location(0) vec4f {
    let k = i32(v.kind);
    let p = v.local;
    let aa = 1.0 / min(screen.per_mm.x, screen.per_mm.y);   // one pixel, mm
    let a = v.shape.x; let b = v.shape.y; let c = v.shape.z; let d = v.shape.w;
    var out = vec4f(0., 0., 0., 0.);
    if (k == 0) {            // disc: fill, white inner rim, dark outer rim
        let dist = length(p) - a;
        let rim = max(0.3, aa * 1.5);
        out = over(out, v.fill, clamp(0.5 - dist / aa, 0., 1.));
        out = over(out, vec4f(0., 0., 0., 0.5 * v.line.a), clamp(0.5 - (abs(dist - rim * 0.5) - rim * 0.5) / aa, 0., 1.));
        out = over(out, v.line, clamp(0.5 - (abs(dist + rim * 0.5) - rim * 0.5) / aa, 0., 1.));
    } else if (k == 1) {     // ring
        let dist = abs(length(p) - a) - b * 0.5;
        out = over(out, v.line, clamp(0.5 - dist / aa, 0., 1.));
    } else if (k == 2) {     // rounded box, with a rim when the line colour has alpha
        let dist = box(p, vec2f(a, b), min(c, min(a, b)));
        out = over(out, v.fill, clamp(0.5 - dist / aa, 0., 1.));
        let rim = max(0.25, aa);
        out = over(out, v.line, clamp(0.5 - (abs(dist + rim * 0.5) - rim * 0.5) / aa, 0., 1.));
    } else if (k == 3) {     // sector between radii a and b, angles c..d (radians, y up)
        let r = length(p);
        var ang = atan2(-p.y, p.x);
        let mid = (c + d) * 0.5;
        let half = (d - c) * 0.5;
        let off = abs(atan2(sin(ang - mid), cos(ang - mid)));
        let radial = max(a - r, r - b);
        let angular = (off - half) * r;
        let dist = max(radial, angular);
        out = over(out, v.fill, clamp(0.5 - dist / aa, 0., 1.));
    } else if (k >= 4 && k <= 8) {   // the symbols and the chevrons: strokes in the line colour
        let w = max(a * 0.16, aa);
        var dist = 1e9;
        if (k == 4) {        // cross
            let e = a * 0.62;
            dist = min(segment(p, vec2f(-e, -e), vec2f(e, e)), segment(p, vec2f(-e, e), vec2f(e, -e))) - w * 0.5;
        } else if (k == 5) { // circle
            dist = abs(length(p) - a * 0.72) - w * 0.5;
        } else if (k == 6) { // triangle
            dist = abs(triangle(p, a * 0.75)) - w * 0.5;
        } else if (k == 7) { // square
            dist = abs(box(p, vec2f(a * 0.62, a * 0.62), 0.)) - w * 0.5;
        } else {             // chevron: c = pi points left
            let s = select(1.0, -1.0, c > 1.0);
            let w2 = max(a * 0.3, aa);
            dist = min(segment(p, vec2f(-s * a * 0.4, -a), vec2f(s * a * 0.4, 0.)), segment(p, vec2f(s * a * 0.4, 0.), vec2f(-s * a * 0.4, a))) - w2 * 0.5;
        }
        out = over(out, v.line, clamp(0.5 - dist / aa, 0., 1.));
    } else if (k == 9) {     // a character of the font, a mm high
        let uv = p / a + vec2f(0.5, 0.5);
        if (uv.x >= 0. && uv.x < 1. && uv.y >= 0. && uv.y < 1.) {
            let glyph = i32(c) - 32;
            if (glyph >= 0 && glyph < 95) {
                let texel = vec2i(glyph * 8 + i32(uv.x * 8.), i32(uv.y * 8.));
                let on = textureLoad(font, texel, 0).r;
                // a dark edge keeps the letters readable over bright snow
                var shadow = 0.;
                for (var dy = -1; dy <= 1; dy++) { for (var dx = -1; dx <= 1; dx++) {
                    let t = texel + vec2i(dx, dy);
                    if (t.y >= 0 && t.y < 8 && t.x >= glyph * 8 && t.x < glyph * 8 + 8) { shadow = max(shadow, textureLoad(font, t, 0).r); }
                } }
                out = over(out, vec4f(0., 0., 0., 0.6 * v.fill.a), shadow);
                out = over(out, v.fill, on);
            }
        }
    } else if (k == 10) {    // stripes: the strips nothing dragged may start in
        let inside = clamp(0.5 - box(p, vec2f(a, b), 0.) / aa, 0., 1.);
        let stripe = step(0.5, fract((p.x + p.y) / 2.4));
        out = over(out, v.fill, inside * stripe);
    }
    if (out.a <= 0.002) { discard; }
    return out;
}
"#;
