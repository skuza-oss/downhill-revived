//! A small host overlay, drawn after the game picture in surface pixels.
use crate::device::Gpu;

// Digits and FPS / MS PING from Daniel Hepper's font8x8_basic.h:
// https://github.com/dhepper/font8x8/blob/master/font8x8_basic.h
// Public Domain (the source header and README both state this); derived from
// Marcel Sondaar / IBM public-domain VGA fonts. LSB is the leftmost pixel.
const CHARS: &[u8] = b"0123456789FPSINGM ";
const FONT: [[u8; 8]; 18] = [
    [0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00], // 0
    [0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00], // 1
    [0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00], // 2
    [0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00], // 3
    [0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00], // 4
    [0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00], // 5
    [0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00], // 6
    [0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00], // 7
    [0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00], // 8
    [0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00], // 9
    [0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00], // F
    [0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00], // P
    [0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00], // S
    [0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00], // I
    [0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00], // N
    [0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00], // G
    [0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00], // M
    [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], //  
];

#[derive(Default)]
pub struct Counter {
    enabled: bool,
    ping: i32,
    start: Option<f64>,
    frames: u32,
    fps: u32,
    shown: Option<(u32, i32)>,
    objects: Option<Objects>,
}
struct Objects {
    pipe: wgpu::RenderPipeline,
    texture: wgpu::Texture,
    group: wgpu::BindGroup,
}
impl Counter {
    // Called only for acquired pictures immediately before presenting. Count those
    // pictures, including interpolated ones, rather than simulation ticks/vblanks.
    pub fn update(&mut self, enabled: bool, ping: i32, now: f64) {
        self.enabled = enabled;
        self.ping = ping;
        if let Some(start) = self.start {
            self.frames += 1;
            let elapsed = now - start;
            if elapsed >= 0.5 {
                self.fps = (self.frames as f64 / elapsed).round().clamp(0.0, 99999.0) as u32;
                self.start = Some(now);
                self.frames = 0;
            }
        } else {
            self.start = Some(now);
        }
    }

    pub fn draw(&mut self, gpu: &Gpu, target: &wgpu::TextureView, format: wgpu::TextureFormat, w: u32, h: u32) {
        if !self.enabled { return; }
        let o = self.objects.get_or_insert_with(|| Objects::new(gpu, format));
        // The numbers change twice a second at most: the bitmap is written again only then.
        if self.shown != Some((self.fps, self.ping)) {
            self.shown = Some((self.fps, self.ping));
            o.write(gpu, self.fps, self.ping);
        }
        // Integer scaling keeps the bitmap crisp: eight pixels is about 2% of height.
        let scale = ((h as f32 / 400.0).round() as u32).max(1);
        let margin = (4 * scale).min(w / 4).min(h / 4);
        let vw = 128 * scale;
        let vh = 22 * scale;
        if vw + margin > w || vh + margin > h { return; }
        let mut encoder = gpu.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("performance counter") });
        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("performance counter"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment { view: target, depth_slice: None, resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Load, store: wgpu::StoreOp::Store } })],
                depth_stencil_attachment: None, timestamp_writes: None, occlusion_query_set: None, multiview_mask: None,
            });
            pass.set_pipeline(&o.pipe);
            pass.set_bind_group(0, &o.group, &[]);
            // Draw the whole fixed texture; transparent unused pixels cost no background.
            let full_w = 128 * scale;
            let full_h = 22 * scale;
            pass.set_viewport((w - margin - vw) as f32, margin as f32, full_w as f32, full_h as f32, 0.0, 1.0);
            pass.set_scissor_rect(w - margin - vw, margin, vw, vh);
            pass.draw(0..3, 0..1);
        }
        gpu.queue.submit([encoder.finish()]);
    }
}
impl Objects {
    fn write(&self, gpu: &Gpu, fps: u32, ping: i32) {
        let lines = [format!("{} FPS", fps), if ping >= 0 { format!("{} MS PING", ping.min(99999)) } else { String::new() }];
        let width = lines.iter().map(|s| s.len()).max().unwrap_or(0) as u32 * 8 + 4;
        let height = if ping >= 0 { 22 } else { 12 };
        let xoff = 128 - width;
        let mut pixels = vec![0u8; 128 * 22 * 4];
        for y in 0..height { for x in 0..width {
            pixels[(y * 128 + x + xoff) as usize * 4 + 3] = 180;
        }}
        for (line, text) in lines.iter().enumerate() {
            for (col, ch) in text.bytes().enumerate() {
                let glyph = &FONT[CHARS.iter().position(|&c| c == ch).unwrap_or(17)];
                for (y, row) in glyph.iter().enumerate() { for x in 0..8 {
                    if row & (1 << x) != 0 {
                        let at = ((line * 10 + y + 2) * 128 + col * 8 + x + 2 + xoff as usize) * 4;
                        pixels[at..at+4].fill(255);
                    }
                }}
            }
        }
        gpu.queue.write_texture(wgpu::TexelCopyTextureInfo { texture: &self.texture, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
            &pixels, wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(512), rows_per_image: Some(22) },
            wgpu::Extent3d { width: 128, height: 22, depth_or_array_layers: 1 });
    }

    fn new(gpu: &Gpu, format: wgpu::TextureFormat) -> Self {
        let shader = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor { label: Some("performance counter"), source: wgpu::ShaderSource::Wgsl(r#"
@group(0) @binding(0) var image: texture_2d<f32>;
struct Out { @builtin(position) pos: vec4f, @location(0) uv: vec2f }
@vertex fn vs(@builtin(vertex_index) i: u32) -> Out {
    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    return Out(vec4f(uv * vec2f(2., -2.) + vec2f(-1., 1.), 0., 1.), uv);
}
@fragment fn fs(p: Out) -> @location(0) vec4f {
    return textureLoad(image, vec2i(p.uv * vec2f(128., 22.)), 0);
}
"#.into()) });
        let pipe = gpu.device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("performance counter"), layout: None,
            vertex: wgpu::VertexState { module: &shader, entry_point: Some("vs"), compilation_options: Default::default(), buffers: &[] },
            fragment: Some(wgpu::FragmentState { module: &shader, entry_point: Some("fs"), compilation_options: Default::default(),
                targets: &[Some(wgpu::ColorTargetState { format, blend: Some(wgpu::BlendState::ALPHA_BLENDING), write_mask: wgpu::ColorWrites::ALL })] }),
            primitive: Default::default(), depth_stencil: None, multisample: Default::default(), multiview_mask: None, cache: None,
        });
        let texture = gpu.device.create_texture(&wgpu::TextureDescriptor {
            label: Some("performance counter"), size: wgpu::Extent3d { width: 128, height: 22, depth_or_array_layers: 1 },
            mip_level_count: 1, sample_count: 1, dimension: wgpu::TextureDimension::D2, format: wgpu::TextureFormat::Rgba8Unorm,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST, view_formats: &[],
        });
        let group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor { label: Some("performance counter"), layout: &pipe.get_bind_group_layout(0),
            entries: &[wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&texture.create_view(&Default::default())) }] });
        Self { pipe, texture, group }
    }
}

// Optional tool capture of the final surface. DHD_SCREENSHOT remains the GS-only
// diagnostic image; DHD_CAPTURE_PRESENTED names a PNG prefix for this host image.
#[cfg(feature = "native")]
#[derive(Default)]
pub struct Capture { start: Option<std::time::Instant>, saved: u32 }
#[cfg(feature = "native")]
impl Capture {
    pub fn frame(&mut self, gpu: &Gpu, frame: &wgpu::Texture) {
        // Read once: nothing sets them while the game runs, and each read scans the environment.
        static ENV: std::sync::OnceLock<Option<(String, f64, f64)>> = std::sync::OnceLock::new();
        let Some((prefix, at, interval)) = ENV.get_or_init(|| {
            let prefix = std::env::var_os("DHD_CAPTURE_PRESENTED")?.to_string_lossy().into_owned();
            let at = std::env::var("DHD_CAPTURE_AT").ok().and_then(|s| s.parse::<f64>().ok()).unwrap_or(10.0);
            let interval = std::env::var("DHD_CAPTURE_INTERVAL").ok().and_then(|s| s.parse::<f64>().ok()).unwrap_or(0.0);
            Some((prefix, at, interval))
        }) else { return };
        let (at, interval) = (*at, *interval);
        let elapsed = self.start.get_or_insert_with(std::time::Instant::now).elapsed().as_secs_f64();
        if (self.saved != 0 && interval <= 0.0) || elapsed < at + self.saved as f64 * interval { return; }
        let path = format!("{}-{:03}.png", prefix, self.saved);
        self.saved += 1;
        if let Err(e) = capture_png(gpu, frame, &path) { crate::host::log(&format!("present capture: {e}")); }
    }
}
#[cfg(feature = "native")]
fn capture_png(gpu: &Gpu, frame: &wgpu::Texture, path: &str) -> Result<(), String> {
    let (w, h) = (frame.width(), frame.height());
    let stride = (w as u64 * 4 + 255) & !255;
    let size = stride * h as u64;
    let read = gpu.device.create_buffer(&wgpu::BufferDescriptor { label: Some("present capture"), size,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST, mapped_at_creation: false });
    let mut encoder = gpu.device.create_command_encoder(&Default::default());
    encoder.copy_texture_to_buffer(wgpu::TexelCopyTextureInfo { texture: frame, mip_level: 0, origin: wgpu::Origin3d::ZERO, aspect: wgpu::TextureAspect::All },
        wgpu::TexelCopyBufferInfo { buffer: &read, layout: wgpu::TexelCopyBufferLayout { offset: 0, bytes_per_row: Some(stride as u32), rows_per_image: Some(h) } },
        wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 });
    gpu.queue.submit([encoder.finish()]);
    let status = std::sync::Arc::new(std::sync::Mutex::new(None));
    let result = status.clone();
    read.map_async(wgpu::MapMode::Read, .., move |r| *result.lock().unwrap() = Some(r));
    gpu.device.poll(wgpu::PollType::wait_indefinitely()).map_err(|e| e.to_string())?;
    status.lock().unwrap().take().ok_or("no capture callback")?.map_err(|e| e.to_string())?;
    let bytes = read.get_mapped_range(..);
    let mut pixels = Vec::with_capacity(w as usize * h as usize * 4);
    for row in bytes.chunks_exact(stride as usize) { pixels.extend_from_slice(&row[..w as usize * 4]); }
    drop(bytes);
    read.unmap();
    if frame.format() == wgpu::TextureFormat::Bgra8Unorm { for p in pixels.chunks_exact_mut(4) { p.swap(0, 2); } }
    let file = std::fs::File::create(path).map_err(|e| e.to_string())?;
    let mut encoder = png::Encoder::new(std::io::BufWriter::new(file), w, h);
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    encoder.write_header().map_err(|e| e.to_string())?.write_image_data(&pixels).map_err(|e| e.to_string())?;
    crate::host::log(&format!("present capture: {path}"));
    Ok(())
}
