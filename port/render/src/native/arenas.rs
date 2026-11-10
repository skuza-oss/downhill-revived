//! The native renderer's arenas: constants, uploads, vertices (task N2).
//!
//! As the retired `port/gs/native_keys.inc`'s native_constants / native_upload / native_arena /
//! native_reserve_arenas (the span gpu_wgpu_native.inc:226-316; both last at render/rust
//! 3882d5576):
//! - `reserve`, before anything of a picture is recorded (bind groups name the arena buffers, so
//!   they can't grow mid-picture): uniforms = 2 * 256 (the scanout's) + per command: ALPHA_UPLOAD
//!   256 and 256 * 32 upload bytes; IMPORT 256 and its rows (w * 4 rounded up to 256) * h; COPY
//!   256; a DRAW with count 2 * 256, + `UNIFORM_WINDOW` when it has a conversion, and count reversed
//!   vertices. An arena grows from 1 MiB by doubling to the need; its buffer is `capacity +
//!   window` bytes (constants: + 2048) and is remade with the uniform bind group when it grows.
//!   The vertex buffer grows from 2^18 vertices by doubling to (vertex_count + reversed) vertices.
//!   reserve resets the used counts and sets `reversed_base` = vertex_count.
//! - `constants` puts bytes at the next 256-byte offset and returns it; `upload` puts w x h pixels
//!   with rows padded to 256 bytes and returns (offset, bytes per row). Overflow (reserve counted
//!   wrong) is fatal, as in C.
//! With `gpu` None, `reserve` only sizes (tests/native_arenas.rs runs it so, against
//! `tests/data/native_arenas.json`, the C code's numbers); the sizes behave as if every buffer had
//! been made.
//!
//! Beyond the skeleton: the buffers and the uniform bind group for the passes (render.rs,
//! surfaces.rs, scanout.rs), the reversed vertices (`reverse_triangles`, the GREATER-with-writes
//! second draw, gpu_wgpu_native.inc:749-755) and the writes before the submit (`write_vertices`,
//! `write`: native_render's end and native_submit's start, :879-891).
//!
//! Referee: `rustgs.py test ... -- --test native_arenas`.
use crate::abi::{DhdrCommand, DhdrVertex, DHDR_CMD_ALPHA_UPLOAD, DHDR_CMD_COPY, DHDR_CMD_DRAW, DHDR_CMD_IMPORT};
use crate::device::Gpu;

/// The uniform binding window (NATIVE_UNIFORM_WINDOW): a binding at the last offset still fits.
pub const UNIFORM_WINDOW: u64 = 2048;

/// The draws' constants (finding 172): packed `DRAW_BYTES` apart in their own arena, a draw reading
/// its own by its instance index out of a window of `DRAW_SLOTS` (native.wgsl's `native_draws`),
/// bound at a multiple of `DRAW_WINDOW` (256-byte aligned), so group 0 changes once per 256
/// distinct constants instead of at every draw.
pub const DRAW_SLOTS: u64 = 256;
pub const DRAW_BYTES: u64 = std::mem::size_of::<crate::abi::DhdrNativeConstants>() as u64;
pub const DRAW_WINDOW: u64 = DRAW_SLOTS * DRAW_BYTES;
const _: () = assert!(DRAW_BYTES % 16 == 0 && DRAW_WINDOW % 256 == 0);

/// sizeof(NativeVertex).
const VERTEX_BYTES: u64 = std::mem::size_of::<DhdrVertex>() as u64;

/// The arenas' sizes and counts, for checks.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ArenaStats {
    pub uniform_used: u64,
    pub upload_used: u64,
    pub uniform_capacity: u64,
    pub upload_capacity: u64,
    /// Bytes of the vertex buffer.
    pub vertex_capacity: u64,
    /// Reversed vertices there is room for.
    pub reversed_capacity: u32,
    /// Where the picture's reversed (second-draw) vertices start: its vertex count.
    pub reversed_base: u32,
    /// Bytes of each arena's buffer (0 when it has none yet): constants (capacity + window),
    /// uploads, vertices.
    pub uniform_buffer: u64,
    pub upload_buffer: u64,
    pub vertex_buffer: u64,
}

/// A CPU array and its GPU buffer (native_arena's buffer, data, capacity and used count).
#[derive(Default)]
struct Arena {
    data: Vec<u8>,
    buffer: Option<wgpu::Buffer>,
    /// Bytes of the buffer, 0 before it is made (tracked when only sizing too).
    buffer_size: u64,
    capacity: u64,
    used: u64,
}

impl Arena {
    /// native_arena: at least `need` bytes (and `window` bytes beyond them in the buffer, for a
    /// binding at the last offset); true when the buffer is new.
    fn grow(&mut self, gpu: Option<&Gpu>, need: u64, window: u64, usage: wgpu::BufferUsages,
            label: &str) -> Result<bool, String> {
        if self.buffer_size != 0 && need <= self.capacity {
            return Ok(false);
        }
        let mut size = if self.capacity != 0 { self.capacity } else { 1 << 20 };
        while size < need {
            size *= 2;
        }
        let bytes = usize::try_from(size).map_err(|_| format!("gs native: {label}: {size} bytes"))?;
        self.data.try_reserve_exact(bytes.saturating_sub(self.data.len()))
            .map_err(|_| format!("gs native: out of memory ({label})"))?;
        self.data.resize(bytes, 0);
        self.buffer = gpu.map(|g| g.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some(label),
            size: size + window,
            usage: usage | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        }));
        self.buffer_size = size + window;
        self.capacity = size;
        Ok(true)
    }
}

pub struct Arenas {
    uniforms: Arena,
    uploads: Arena,
    /// The draws' constants (`reserve_draws`, `draw_constants`) and their bind group (group 0 of
    /// the draw pipelines: `DRAW_WINDOW` bytes at a dynamic offset).
    draws: Arena,
    draw_group: Option<wgpu::BindGroup>,
    /// The constants arena's bind group (binding 0, `UNIFORM_WINDOW` bytes at a dynamic offset).
    uniform_group: Option<wgpu::BindGroup>,
    vertex_buffer: Option<wgpu::Buffer>,
    /// The mesh draws' group 2 over `vertex_buffer` (`mesh_group`), made again with a new buffer.
    mesh_group: Option<wgpu::BindGroup>,
    /// Bytes of the vertex buffer (0 before it is made).
    vertex_capacity: u64,
    /// The picture's reversed vertices (its len is reversed_count); room for `reversed_capacity`.
    reversed: Vec<DhdrVertex>,
    reversed_capacity: u32,
    reversed_base: u32,
    /// An indexed picture's (`reserve_indices`): the index buffer and its bytes (0 before it is
    /// made), the reversed (second-draw) triangles' indices and where they start (its index count).
    index_buffer: Option<wgpu::Buffer>,
    index_capacity: u64,
    reversed_indices: Vec<u32>,
    index_base: u32,
}

impl Default for Arenas {
    fn default() -> Arenas {
        Arenas::new()
    }
}

impl Arenas {
    pub fn new() -> Arenas {
        Arenas {
            uniforms: Arena::default(),
            uploads: Arena::default(),
            draws: Arena::default(),
            draw_group: None,
            uniform_group: None,
            vertex_buffer: None,
            mesh_group: None,
            vertex_capacity: 0,
            reversed: Vec::new(),
            reversed_capacity: 0,
            reversed_base: 0,
            index_buffer: None,
            index_capacity: 0,
            reversed_indices: Vec::new(),
            index_base: 0,
        }
    }

    /// Room for a picture's constants, uploads and reversed vertices. `gpu`: the device and the
    /// uniform bind group layout (`init::Objects`); None sizes only.
    pub fn reserve(&mut self, gpu: Option<(&Gpu, &wgpu::BindGroupLayout)>, commands: &[DhdrCommand],
                   vertex_count: u32) -> Result<(), String> {
        let device = gpu.map(|(g, _)| g);
        let mut uniforms: u64 = 2 * 256; // + the scanout's
        let mut uploads: u64 = 0;
        let mut reversed: u64 = 0;
        for k in commands {
            match k.kind {
                DHDR_CMD_ALPHA_UPLOAD => {
                    uniforms += 256;
                    uploads += 256 * 32;
                }
                DHDR_CMD_IMPORT => {
                    uniforms += 256;
                    // C: (uint64_t)((k->w * 4 + 255) & ~255u) * k->h, in unsigned arithmetic.
                    uploads += row_stride(k.w as u32) as u64 * (k.h as u32) as u64;
                }
                DHDR_CMD_COPY => uniforms += 256,
                _ if k.count != 0 => {
                    uniforms += 2 * 256 + if k.convert >= 0 { UNIFORM_WINDOW } else { 0 };
                    reversed += k.count as u64;
                }
                _ => {}
            }
        }
        if self.uniforms.grow(device, uniforms, UNIFORM_WINDOW, wgpu::BufferUsages::UNIFORM, "native constants")? {
            self.uniform_group = match (gpu, &self.uniforms.buffer) {
                (Some((g, layout)), Some(buffer)) => Some(g.device.create_bind_group(&wgpu::BindGroupDescriptor {
                    label: None,
                    layout,
                    entries: &[wgpu::BindGroupEntry {
                        binding: 0,
                        resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                            buffer,
                            offset: 0,
                            size: wgpu::BufferSize::new(UNIFORM_WINDOW),
                        }),
                    }],
                })),
                _ => None,
            };
        }
        if uploads != 0 {
            self.uploads.grow(device, uploads, 0, wgpu::BufferUsages::COPY_SRC, "native uploads")?;
        }
        if reversed > self.reversed_capacity as u64 {
            let n = u32::try_from(reversed).map_err(|_| format!("gs native: {reversed} reversed vertices"))?;
            self.reversed.try_reserve_exact((n as usize).saturating_sub(self.reversed.len()))
                .map_err(|_| "gs native: out of memory (reversed vertices)".to_string())?;
            self.reversed_capacity = n;
        }
        let vertices = (vertex_count as u64 + reversed) * VERTEX_BYTES;
        if self.vertex_capacity == 0 || vertices > self.vertex_capacity {
            let mut size = if self.vertex_capacity != 0 { self.vertex_capacity } else { (1 << 18) * VERTEX_BYTES };
            while size < vertices {
                size *= 2;
            }
            // Also storage for the mesh draws' object records (native.wgsl's mesh_records; not where
            // the device draws no meshes, the browser among them: init.rs's refuse_meshes).
            let storage = if crate::native::init::meshes_refused() { wgpu::BufferUsages::empty() } else { wgpu::BufferUsages::STORAGE };
            self.vertex_buffer = device.map(|g| g.device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("native vertices"),
                size,
                usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST | storage,
                mapped_at_creation: false,
            }));
            self.mesh_group = None;
            self.vertex_capacity = size;
        }
        self.uniforms.used = 0;
        self.uploads.used = 0;
        self.reversed.clear();
        self.reversed_base = vertex_count;
        Ok(())
    }

    /// An indexed picture (`dhdr_native_picture_indexed`), after `reserve`: room for its
    /// `index_count` indices and the reversed triangles' indices after them (each draw's count, as
    /// `reserve` counts reversed vertices). The index buffer grows from 2^18 indices by doubling.
    pub fn reserve_indices(&mut self, device: &Gpu, commands: &[DhdrCommand], index_count: u32) -> Result<(), String> {
        let reversed: u64 = commands.iter()
            .filter(|k| !matches!(k.kind, DHDR_CMD_ALPHA_UPLOAD | DHDR_CMD_IMPORT | DHDR_CMD_COPY))
            .map(|k| k.count as u64)
            .sum();
        let bytes = (index_count as u64 + reversed) * 4;
        if self.index_capacity == 0 || bytes > self.index_capacity {
            let mut size = if self.index_capacity != 0 { self.index_capacity } else { (1 << 18) * 4 };
            while size < bytes {
                size *= 2;
            }
            self.index_buffer = Some(device.device.create_buffer(&wgpu::BufferDescriptor {
                label: Some("native indices"),
                size,
                usage: wgpu::BufferUsages::INDEX | wgpu::BufferUsages::COPY_DST,
                mapped_at_creation: false,
            }));
            self.index_capacity = size;
        }
        let n = usize::try_from(reversed).map_err(|_| format!("gs native: {reversed} reversed indices"))?;
        self.reversed_indices.clear();
        self.reversed_indices.try_reserve_exact(n)
            .map_err(|_| "gs native: out of memory (reversed indices)".to_string())?;
        self.index_base = index_count;
        Ok(())
    }

    /// `reverse_triangles` for an indexed picture: the indices `first..first + count` with the
    /// triangles in reverse order, after the picture's reversed indices so far; the index to draw
    /// them from. `count` is a multiple of 3.
    pub fn reverse_triangle_indices(&mut self, indices: &[u32], first: u32, count: u32) -> u32 {
        let start = self.index_base + self.reversed_indices.len() as u32;
        if self.reversed_indices.len() + count as usize > self.reversed_indices.capacity() {
            fatal("gs native: reversed index overflow");
        }
        let (first, count) = (first as usize, count as usize);
        for t in (0..count).step_by(3) {
            let s = first + count - 3 - t;
            self.reversed_indices.extend_from_slice(&indices[s..s + 3]);
        }
        start
    }

    /// An indexed picture's indices, then its reversed ones at its index count, into the index
    /// buffer (with `write_vertices`, which writes its vertices).
    pub fn write_indices(&self, queue: &wgpu::Queue, indices: &[u32]) {
        let Some(buffer) = &self.index_buffer else { return };
        if !indices.is_empty() {
            queue.write_buffer(buffer, 0, as_bytes(indices));
        }
        if !self.reversed_indices.is_empty() {
            queue.write_buffer(buffer, self.index_base as u64 * 4, as_bytes(&self.reversed_indices));
        }
    }

    /// The index buffer (`reserve_indices`), bound whole (`index_capacity` bytes) in an indexed
    /// picture's draw passes.
    pub fn index_buffer(&self) -> Option<&wgpu::Buffer> {
        self.index_buffer.as_ref()
    }

    /// Bytes of the index buffer.
    pub fn index_capacity(&self) -> u64 {
        self.index_capacity
    }

    /// `data` at the next 256-byte offset of the constants arena; that offset.
    pub fn constants(&mut self, data: &[u8]) -> u32 {
        let a = &mut self.uniforms;
        let at = a.used;
        let size = data.len() as u64;
        if at + size > a.capacity {
            fatal("gs native: uniform arena overflow");
        }
        a.data[at as usize..(at + size) as usize].copy_from_slice(data);
        a.used += (size + 255) & !255;
        at as u32
    }

    /// w x h pixels into the upload arena, rows padded to 256 bytes: (offset, bytes per row).
    pub fn upload(&mut self, pixels: &[u32], w: u32, h: u32) -> (u64, u32) {
        let a = &mut self.uploads;
        let stride = row_stride(w);
        let at = a.used;
        let bytes = stride as u64 * h as u64;
        if at + bytes > a.capacity {
            fatal("gs native: upload arena overflow");
        }
        let (w, row) = (w as usize, w as usize * 4);
        for y in 0..h as usize {
            let o = at as usize + y * stride as usize;
            a.data[o..o + row].copy_from_slice(as_bytes(&pixels[y * w..(y + 1) * w]));
        }
        a.used += bytes;
        (at, stride)
    }

    /// The second draw of a GREATER-with-writes draw: its `count` vertices from `first` with the
    /// triangles in reverse order, after the picture's reversed vertices so far
    /// (native_draw_passes); the vertex to draw them from. `count` is a multiple of 3.
    pub fn reverse_triangles(&mut self, vertices: &[DhdrVertex], first: u32, count: u32) -> u32 {
        let start = self.reversed_base + self.reversed.len() as u32;
        if self.reversed.len() as u64 + count as u64 > self.reversed_capacity as u64 {
            fatal("gs native: reversed vertex overflow");
        }
        let (first, count) = (first as usize, count as usize);
        for t in (0..count).step_by(3) {
            let s = first + count - 3 - t;
            self.reversed.extend_from_slice(&vertices[s..s + 3]);
        }
        start
    }

    /// The picture's vertices, then its reversed ones at `reversed_base`, into the vertex buffer
    /// (native_render's end).
    pub fn write_vertices(&self, queue: &wgpu::Queue, vertices: &[DhdrVertex]) {
        let Some(buffer) = &self.vertex_buffer else { return };
        if !vertices.is_empty() {
            queue.write_buffer(buffer, 0, as_bytes(vertices));
        }
        if !self.reversed.is_empty() {
            queue.write_buffer(buffer, self.reversed_base as u64 * VERTEX_BYTES, as_bytes(&self.reversed));
        }
    }

    /// The used constants and uploads, which the encoder's commands read (native_submit, before
    /// the submit).
    pub fn write(&self, queue: &wgpu::Queue) {
        for a in [&self.uniforms, &self.uploads, &self.draws] {
            if let Some(buffer) = a.buffer.as_ref().filter(|_| a.used != 0) {
                queue.write_buffer(buffer, 0, &a.data[..a.used as usize]);
            }
        }
    }

    /// Room for the picture's draw constants, one per DRAW command at most (`layout`: init.rs's
    /// draws layout); the arena grows from 1 MiB by doubling, its bind group made again with it.
    pub fn reserve_draws(&mut self, gpu: &Gpu, layout: &wgpu::BindGroupLayout, commands: &[DhdrCommand])
                         -> Result<(), String> {
        let draws = commands.iter().filter(|k| k.kind == DHDR_CMD_DRAW && k.count != 0).count() as u64;
        if self.draws.grow(Some(gpu), (draws + 1) * DRAW_BYTES, DRAW_WINDOW, wgpu::BufferUsages::UNIFORM,
                           "native draw constants")? {
            self.draw_group = self.draws.buffer.as_ref().map(|buffer| gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("native draw constants"),
                layout,
                entries: &[wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                        buffer,
                        offset: 0,
                        size: wgpu::BufferSize::new(DRAW_WINDOW),
                    }),
                }],
            }));
        }
        self.draws.used = 0;
        Ok(())
    }

    /// A draw's constants: the next slot, its index (slot / DRAW_SLOTS is its window, slot %
    /// DRAW_SLOTS its instance index). Overflow (`reserve_draws` counted wrong) is fatal.
    pub fn draw_constants(&mut self, data: &[u8]) -> u32 {
        let a = &mut self.draws;
        let at = a.used;
        if data.len() as u64 != DRAW_BYTES || at + DRAW_BYTES > a.capacity {
            fatal("gs native: draw constants arena overflow");
        }
        a.data[at as usize..(at + DRAW_BYTES) as usize].copy_from_slice(data);
        a.used += DRAW_BYTES;
        (at / DRAW_BYTES) as u32
    }

    /// The draw constants' bind group (`reserve_draws`).
    pub fn draw_group(&self) -> Option<&wgpu::BindGroup> {
        self.draw_group.as_ref()
    }

    /// The constants arena's bind group (group 0, one dynamic offset from `constants`).
    pub fn uniform_group(&self) -> Option<&wgpu::BindGroup> {
        self.uniform_group.as_ref()
    }

    pub fn uniform_buffer(&self) -> Option<&wgpu::Buffer> {
        self.uniforms.buffer.as_ref()
    }

    /// The source buffer of `upload`'s offsets.
    pub fn upload_buffer(&self) -> Option<&wgpu::Buffer> {
        self.uploads.buffer.as_ref()
    }

    /// The vertex buffer, bound whole (`vertex_capacity` bytes) at slot 0 of every draw pass.
    pub fn vertex_buffer(&self) -> Option<&wgpu::Buffer> {
        self.vertex_buffer.as_ref()
    }

    /// The mesh draws' group 2 (`layout`: init.rs's mesh layout): the whole vertex buffer, where
    /// native_core.inc's native_mesh_record puts the objects' records (finding 168).
    /// None (and Native VU off, finding 194) when the buffer is larger than one storage binding.
    pub fn mesh_group(&mut self, gpu: &Gpu, layout: &wgpu::BindGroupLayout) -> Option<wgpu::BindGroup> {
        if self.mesh_group.is_none() {
            let buffer = self.vertex_buffer.as_ref()?;
            if !buffer.usage().contains(wgpu::BufferUsages::STORAGE) {
                return None;   // made after the meshes were refused
            }
            if buffer.size() > gpu.device.limits().max_storage_buffer_binding_size {
                crate::native::init::refuse_meshes(Some("the picture's vertices exceed one storage binding"));
                return None;
            }
            self.mesh_group = Some(gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("native mesh records"),
                layout,
                entries: &[wgpu::BindGroupEntry { binding: 0, resource: buffer.as_entire_binding() }],
            }));
        }
        self.mesh_group.clone()
    }

    /// Bytes of the vertex buffer.
    pub fn vertex_capacity(&self) -> u64 {
        self.vertex_capacity
    }

    pub fn stats(&self) -> ArenaStats {
        ArenaStats {
            uniform_used: self.uniforms.used,
            upload_used: self.uploads.used,
            uniform_capacity: self.uniforms.capacity,
            upload_capacity: self.uploads.capacity,
            vertex_capacity: self.vertex_capacity,
            reversed_capacity: self.reversed_capacity,
            reversed_base: self.reversed_base,
            uniform_buffer: self.uniforms.buffer_size,
            upload_buffer: self.uploads.buffer_size,
            vertex_buffer: self.vertex_capacity,
        }
    }
}

/// Bytes per upload row: w * 4 rounded up to 256 (C's unsigned (w * 4 + 255) & ~255u).
fn row_stride(w: u32) -> u32 {
    w.wrapping_mul(4).wrapping_add(255) & !255u32
}

/// An arena overflow: `reserve` counted wrong, never expected (C aborts). Logs, then panics (the
/// library's abort profile ends the process).
fn fatal(what: &str) -> ! {
    crate::host::log(what);
    panic!("{what}")
}

/// Plain C data with no padding (u32, DhdrVertex) as its bytes.
fn as_bytes<T: Copy>(v: &[T]) -> &[u8] {
    // SAFETY: T is u32 or DhdrVertex (repr(C), eight 4-byte fields, no padding), so every byte is
    // initialised; the length is the slice's size in bytes.
    unsafe { std::slice::from_raw_parts(v.as_ptr().cast::<u8>(), std::mem::size_of_val(v)) }
}
