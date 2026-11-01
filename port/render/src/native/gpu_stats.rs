//! DHD_GPU_STATS=N (diagnostics, finding 131): where the draw passes' GPU time goes, by kind of
//! draw. Every N-th picture (default 60) each draw is wrapped in a pipeline-statistics query
//! (fragment shader invocations: the pixels actually shaded, after any early depth rejection) and
//! two timestamps inside its pass (its incremental GPU time; the timestamps keep the GPU from
//! overlapping draws, so the times are shares, not the frame's real time). The picture is read back
//! with a wait (it is a diagnostic: the sampled frame is slow). Every 10 sampled pictures one log
//! line per kind, the costliest first, and a total: draws, Mpixels shaded and ms per picture, plus
//! the passes, pipeline switches and distinct pipelines a picture uses.
//!
//! A kind is the draw's shader path (its `flags`: textured, TFX, fog, bilinear, alpha test, its
//! failing-pixel pass, premultiply, sprite, replacement), its texel scale, its pipeline variant, its
//! depth state, whether it blends and whether it writes the alpha channel. Needs
//! TIMESTAMP_QUERY_INSIDE_PASSES (and PIPELINE_STATISTICS_QUERY for the pixels; device.rs asks for
//! them only with the variable); without the timestamps it logs once and does nothing. Off (the
//! variable unset): one cached branch per draw.
#![cfg(not(target_arch = "wasm32"))]
use crate::device::Gpu;
use std::collections::{HashMap, HashSet};

/// Draws timed per picture; later draws go uncounted (and are reported).
const DRAWS: u32 = 2048;
/// Sampled pictures per log line.
const LINE: u32 = 10;

pub fn wanted() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("DHD_GPU_STATS").is_some())
}

/// What the times need (the pixels need PIPELINE_STATISTICS_QUERY too, which wgpu offers on Vulkan
/// and Metal but not on DX12: there the pixel counts are absent).
pub fn features() -> wgpu::Features {
    wgpu::Features::TIMESTAMP_QUERY | wgpu::Features::TIMESTAMP_QUERY_INSIDE_PASSES
}

/// The features to ask for out of the adapter's `offered`.
pub fn request(offered: wgpu::Features) -> wgpu::Features {
    if !offered.contains(features()) {
        return wgpu::Features::empty();
    }
    features() | (offered & wgpu::Features::PIPELINE_STATISTICS_QUERY)
}

/// A draw's kind (see the module): the shader path bits of `flags`, the texel scale, the variant,
/// the pipeline's depth field, blending and whether it writes the alpha channel.
pub fn class(flags: u32, texel_scale: u32, variant: u32, depth: u32, blend: bool, alpha: bool) -> u32 {
    (flags & 0xEFD) | (texel_scale & 15) << 12 | (variant & 3) << 16 | (depth & 31) << 18 | (blend as u32) << 23
        | (alpha as u32) << 24
}

fn describe(c: u32) -> String {
    let f = c & 0xFFF;
    let mut s = String::new();
    if f & 1 != 0 {
        s += if f & 2048 != 0 { "replace" } else { "tex" };
        s += &format!("/tfx{}", f >> 2 & 3);
        s += if f & 32 != 0 { "/bilinear" } else { "/point" };
        let ts = c >> 12 & 15;
        if ts != 0 { s += &format!("/scale{ts}"); } else { s += "/generic"; }
    } else {
        s += "flat";
    }
    if f & 16 != 0 { s += "+fog"; }
    if f & 64 != 0 { s += if f & 128 != 0 { "+atest-fail" } else { "+atest" }; }
    if f & 512 != 0 { s += "+premul"; }
    if f & 1024 != 0 { s += "+sprite"; }
    s += ["", " dual", " rgb", " alpha-only"][(c >> 16 & 3) as usize];
    let d = c >> 18 & 31;
    if d & 16 != 0 {
        s += &format!(" z:{}{}", ["never", "always", "gequal", "greater", "equal", "always", "always", "always"][(d & 7) as usize],
                      if d & 8 != 0 { "+write" } else { "" });
    } else {
        s += " z:none";
    }
    if c >> 23 & 1 != 0 { s += " blend"; }
    if c >> 24 & 1 != 0 { s += " writes-A"; }
    s
}

#[derive(Default, Clone, Copy)]
struct Acc {
    draws: u64,
    pixels: u64,
    ns: f64,
}

pub struct Stats {
    queries: Option<(Option<wgpu::QuerySet>, wgpu::QuerySet, wgpu::Buffer, wgpu::Buffer, wgpu::Buffer, wgpu::Buffer)>,
    /// Pixel counts (pipeline statistics) are offered.
    pixels: bool,
    every: u64,
    picture: u64,
    /// This picture is sampled.
    active: bool,
    classes: Vec<u32>,
    dropped: u64,
    passes: u64,
    switches: u64,
    keys: HashSet<u64>,
    distinct: u64,
    sampled: u32,
    acc: HashMap<u32, Acc>,
    warned: bool,
}

impl Stats {
    pub fn new() -> Stats {
        Stats { queries: None, every: std::env::var("DHD_GPU_STATS").ok().and_then(|v| v.parse().ok()).filter(|&n| n > 0)
                    .unwrap_or(60),
                picture: 0, active: false, classes: Vec::new(), dropped: 0, passes: 0, switches: 0, keys: HashSet::new(),
                distinct: 0, sampled: 0, acc: HashMap::new(), warned: false, pixels: false }
    }

    /// At a picture's start: whether it is sampled.
    pub fn picture_begin(&mut self, gpu: &Gpu) {
        self.active = false;
        if !wanted() {
            return;
        }
        if !gpu.device.features().contains(features()) {
            if !self.warned {
                crate::host::log("gpu_stats: this adapter has no timestamps inside passes (DHD_GPU_STATS ignored)");
                self.warned = true;
            }
            return;
        }
        self.picture += 1;
        if self.picture % self.every != 0 {
            return;
        }
        if self.queries.is_none() {
            let buffer = |label, size, usage| gpu.device.create_buffer(&wgpu::BufferDescriptor {
                label: Some(label), size, usage, mapped_at_creation: false });
            let resolve = wgpu::BufferUsages::QUERY_RESOLVE | wgpu::BufferUsages::COPY_SRC;
            let read = wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST;
            self.pixels = gpu.device.features().contains(wgpu::Features::PIPELINE_STATISTICS_QUERY);
            if !self.pixels {
                crate::host::log("gpu_stats: no pipeline statistics on this backend: times only (Vulkan has the pixel counts)");
            }
            self.queries = Some((
                self.pixels.then(|| gpu.device.create_query_set(&wgpu::QuerySetDescriptor { label: Some("gpu stats pixels"),
                    ty: wgpu::QueryType::PipelineStatistics(wgpu::PipelineStatisticsTypes::FRAGMENT_SHADER_INVOCATIONS),
                    count: DRAWS })),
                gpu.device.create_query_set(&wgpu::QuerySetDescriptor { label: Some("gpu stats times"),
                    ty: wgpu::QueryType::Timestamp, count: DRAWS * 2 }),
                buffer("gpu stats pixels resolve", DRAWS as u64 * 8, resolve),
                buffer("gpu stats times resolve", DRAWS as u64 * 16, resolve),
                buffer("gpu stats pixels read", DRAWS as u64 * 8, read),
                buffer("gpu stats times read", DRAWS as u64 * 16, read),
            ));
        }
        self.active = true;
        self.classes.clear();
        self.keys.clear();
    }

    pub fn pass(&mut self) {
        if self.active {
            self.passes += 1;
        }
    }

    pub fn pipeline(&mut self, key: u64) {
        if self.active {
            self.switches += 1;
            self.keys.insert(key);
        }
    }

    /// Before a draw of `class`: its queries' index, or None (not sampled, or full).
    pub fn draw_begin(&mut self, pass: &mut wgpu::RenderPass<'static>, class: u32) -> Option<u32> {
        if !self.active {
            return None;
        }
        let (stats, times, ..) = self.queries.as_ref()?;
        let i = self.classes.len() as u32;
        if i >= DRAWS {
            self.dropped += 1;
            return None;
        }
        self.classes.push(class);
        pass.write_timestamp(times, i * 2);
        if let Some(stats) = stats {
            pass.begin_pipeline_statistics_query(stats, i);
        }
        Some(i)
    }

    pub fn draw_end(&mut self, pass: &mut wgpu::RenderPass<'static>, index: Option<u32>) {
        let (Some(i), Some((stats, times, ..))) = (index, self.queries.as_ref()) else { return };
        if stats.is_some() {
            pass.end_pipeline_statistics_query();
        }
        pass.write_timestamp(times, i * 2 + 1);
    }

    /// In the picture's encoder before it is finished: the queries into the read buffers.
    pub fn resolve(&mut self, encoder: &mut wgpu::CommandEncoder) {
        if !self.active || self.classes.is_empty() {
            return;
        }
        let Some((stats, times, rs, rt, ps, pt)) = self.queries.as_ref() else { return };
        let n = self.classes.len() as u32;
        if let Some(stats) = stats {
            encoder.resolve_query_set(stats, 0..n, rs, 0);
            encoder.copy_buffer_to_buffer(rs, 0, ps, 0, n as u64 * 8);
        }
        encoder.resolve_query_set(times, 0..n * 2, rt, 0);
        encoder.copy_buffer_to_buffer(rt, 0, pt, 0, n as u64 * 16);
    }

    /// After the picture's submit: waits for its queries and adds them up.
    pub fn collect(&mut self, gpu: &Gpu) {
        if !self.active {
            return;
        }
        self.active = false;
        self.distinct += self.keys.len() as u64;
        let n = self.classes.len();
        if n > 0 {
            let Some((_, _, _, _, ps, pt)) = self.queries.as_ref() else { return };
            let (a, b) = (ps.slice(..n as u64 * 8), pt.slice(..n as u64 * 16));
            let pixels_on = self.pixels;
            if pixels_on {
                a.map_async(wgpu::MapMode::Read, |_| {});
            }
            b.map_async(wgpu::MapMode::Read, |_| {});
            let _ = gpu.device.poll(wgpu::PollType::wait_indefinitely());
            {
                let pixels = pixels_on.then(|| a.get_mapped_range());
                let times = b.get_mapped_range();
                let word = |d: &[u8], q: usize| u64::from_le_bytes(d[q * 8..q * 8 + 8].try_into().expect("8 bytes"));
                let period = gpu.queue.get_timestamp_period() as f64;
                for (i, &c) in self.classes.iter().enumerate() {
                    let e = self.acc.entry(c).or_default();
                    e.draws += 1;
                    e.pixels += pixels.as_ref().map_or(0, |p| word(p, i));
                    let (t0, t1) = (word(&times, i * 2), word(&times, i * 2 + 1));
                    if t1 > t0 {
                        e.ns += (t1 - t0) as f64 * period;
                    }
                }
            }
            if pixels_on {
                ps.unmap();
            }
            pt.unmap();
        }
        self.sampled += 1;
        if self.sampled < LINE {
            return;
        }
        let f = self.sampled as f64;
        let mut rows: Vec<(u32, Acc)> = self.acc.drain().collect();
        rows.sort_by(|a, b| b.1.ns.total_cmp(&a.1.ns));
        let total = rows.iter().fold(Acc::default(), |t, (_, a)| Acc { draws: t.draws + a.draws, pixels: t.pixels + a.pixels,
                                                                       ns: t.ns + a.ns });
        crate::host::log(&format!(
            "gpu_stats: pictures={} draws={:.0} mpix_shaded={:.3} draw_ms={:.3} passes={:.1} pipeline_sets={:.1} pipelines={:.1} dropped={}",
            self.sampled, total.draws as f64 / f, total.pixels as f64 / f / 1e6, total.ns / f / 1e6, self.passes as f64 / f,
            self.switches as f64 / f, self.distinct as f64 / f, self.dropped));
        for (c, a) in rows {
            crate::host::log(&format!("gpu_stats:   {:6.2}% ms={:.3} mpix={:.3} draws={:.1} ns/pix={:.2} class={:07X} {}",
                                      100.0 * a.ns / total.ns.max(1.0), a.ns / f / 1e6, a.pixels as f64 / f / 1e6,
                                      a.draws as f64 / f, if a.pixels > 0 { a.ns / a.pixels as f64 } else { 0.0 }, c,
                                      describe(c)));
        }
        self.sampled = 0;
        self.passes = 0;
        self.switches = 0;
        self.distinct = 0;
        self.dropped = 0;
    }
}
