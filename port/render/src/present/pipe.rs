//! present.wgsl's pipeline and the exact present pass (task A5).
//!
//! As the retired `gpu_wgpu.c`'s present_pipeline / wgpu_present (last at render/rust 3882d5576):
//! a target of the surface's format (RGBA8Unorm, or BGRA8Unorm where the surface has no RGBA8:
//! Apple's CAMetalLayer), write mask all, no
//! blending, triangle list, one sample; built inside a Validation error scope (None on failure:
//! logged, not fatal; C then keeps the surface off). The pass clears, writes the 16-byte constants
//! (display width, height, window width, height), sets the bind group (the source view at 0, the
//! constants at 1) and draws 3 vertices. present.wgsl picks each target pixel's source pixel with
//! integer maths, so a 1:1 window shows the picture's exact bytes.
use crate::device::Gpu;
use std::sync::Mutex;

pub struct PresentPipe {
    /// The target's format (the surface's).
    pub format: wgpu::TextureFormat,
    pipeline: wgpu::RenderPipeline,
    layout: wgpu::BindGroupLayout,
    /// present.wgsl's `present_state` (16 bytes; uniform, copy dst).
    constants: wgpu::Buffer,
    /// The bind group of the last source view (C's present_group): remade when the view changes,
    /// as when the display texture is remade at a new size.
    group: Mutex<Option<(wgpu::TextureView, wgpu::BindGroup)>>,
}

/// The pipeline's descriptor parts: as gpu_wgpu.c's present_pipeline (automatic layout), into a
/// `format` target.
fn build(gpu: &Gpu, format: wgpu::TextureFormat) -> wgpu::RenderPipeline {
    let _startup = crate::startup::step("present shader and pipeline");
    let module = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("present.wgsl"),
        source: wgpu::ShaderSource::Wgsl(crate::shaders::PRESENT.into()),
    });
    gpu.device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
        label: Some("present"),
        layout: None,
        vertex: wgpu::VertexState {
            module: &module,
            entry_point: Some("present_vertex"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            buffers: &[],
        },
        primitive: wgpu::PrimitiveState {
            topology: wgpu::PrimitiveTopology::TriangleList,
            ..Default::default()
        },
        depth_stencil: None,
        multisample: wgpu::MultisampleState { count: 1, mask: !0, alpha_to_coverage_enabled: false },
        fragment: Some(wgpu::FragmentState {
            module: &module,
            entry_point: Some("present_fragment"),
            compilation_options: wgpu::PipelineCompilationOptions::default(),
            targets: &[Some(wgpu::ColorTargetState {
                format,
                blend: None,
                write_mask: wgpu::ColorWrites::ALL,
            })],
        }),
        multiview_mask: None,
        cache: None,
    })
}

impl PresentPipe {
    fn finish(gpu: &Gpu, pipeline: wgpu::RenderPipeline, format: wgpu::TextureFormat) -> PresentPipe {
        let layout = pipeline.get_bind_group_layout(0);
        let constants = gpu.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("present"),
            size: 16,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        PresentPipe { format, pipeline, layout, constants, group: Mutex::new(None) }
    }

    /// present.wgsl's pipeline into `format`, or None (logged) when the device can't build it.
    /// Blocks on the error scope (native builds).
    #[cfg(feature = "native")]
    pub fn create(gpu: &Gpu, format: wgpu::TextureFormat) -> Option<PresentPipe> {
        let scope = gpu.device.push_error_scope(wgpu::ErrorFilter::Validation);
        let pipeline = build(gpu, format);
        let error = pollster::block_on(scope.pop());
        Self::checked(gpu, pipeline, error, format)
    }

    /// Without the `native` feature there is no blocking executor (the browser uses `create_async`).
    #[cfg(not(feature = "native"))]
    pub fn create(gpu: &Gpu, format: wgpu::TextureFormat) -> Option<PresentPipe> {
        let _ = (gpu, format);
        crate::host::log("dhd_render: present pipeline: no blocking executor in this build (create_async)");
        None
    }

    /// present.wgsl's pipeline into an RGBA8 target (the browser's canvas; its async init awaits
    /// the error scope).
    pub async fn create_async(gpu: &Gpu) -> Option<PresentPipe> {
        let format = wgpu::TextureFormat::Rgba8Unorm;
        let scope = gpu.device.push_error_scope(wgpu::ErrorFilter::Validation);
        let pipeline = build(gpu, format);
        let error = scope.pop().await;
        Self::checked(gpu, pipeline, error, format)
    }

    fn checked(gpu: &Gpu, pipeline: wgpu::RenderPipeline, error: Option<wgpu::Error>, format: wgpu::TextureFormat)
               -> Option<PresentPipe> {
        match error {
            None => Some(Self::finish(gpu, pipeline, format)),
            Some(e) => {
                crate::host::log(&format!("dhd_render: present pipeline: {e}"));
                None
            }
        }
    }

    /// Records the present pass: `source` drawn into `target` (cleared first). `constants` are the
    /// display's and the window's sizes; they are a queue write, so one draw per submit.
    pub fn draw(&self, gpu: &Gpu, encoder: &mut wgpu::CommandEncoder, source: &wgpu::TextureView,
                target: &wgpu::TextureView, constants: [u32; 4]) {
        let mut cache = self.group.lock().unwrap_or_else(|p| p.into_inner());
        if cache.as_ref().is_none_or(|(view, _)| view != source) {
            let group = gpu.device.create_bind_group(&wgpu::BindGroupDescriptor {
                label: Some("present"),
                layout: &self.layout,
                entries: &[
                    wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(source) },
                    wgpu::BindGroupEntry { binding: 1, resource: self.constants.as_entire_binding() },
                ],
            });
            *cache = Some((source.clone(), group));
        }
        let group = &cache.as_ref().expect("present bind group").1;
        let mut bytes = [0u8; 16];
        for (i, c) in constants.iter().enumerate() {
            bytes[i * 4..i * 4 + 4].copy_from_slice(&c.to_le_bytes());
        }
        gpu.queue.write_buffer(&self.constants, 0, &bytes);
        let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
            label: Some("present"),
            color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                view: target,
                depth_slice: None,
                resolve_target: None,
                ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT), store: wgpu::StoreOp::Store },
            })],
            depth_stencil_attachment: None,
            timestamp_writes: None,
            occlusion_query_set: None,
            multiview_mask: None,
        });
        pass.set_pipeline(&self.pipeline);
        pass.set_bind_group(0, group, &[]);
        pass.draw(0..3, 0..1);
    }
}
