// The WebGPU backend's presentation (gpu_wgpu.c's window surface, finding 084 phase W1): the
// scanout picture stretched over the window, nearest pixel, opaque. A render pass: DX12's surface
// textures are render attachments only (no storage, no copy target).
// present_state.size: x,y the picture's size, z,w the window's. Integer maths picks the source
// pixel, so a 1:1 window shows the picture's exact bytes. present_state.adjust: the player's
// picture settings (picture.wgsl, appended to this file; zeros leave the bytes as they are).
struct PresentState { size: vec4<u32>, adjust: vec4<f32>, }
@group(0) @binding(0) var picture: texture_2d<f32>;
@group(0) @binding(1) var<uniform> present_state: PresentState;

@vertex
fn present_vertex(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4<f32>(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}

@fragment
fn present_fragment(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let target_xy = vec2<u32>(position.xy);
    let size = present_state.size;
    let source = min((target_xy * 2u + 1u) * size.xy / (size.zw * 2u), size.xy - 1u);
    return vec4<f32>(picture_adjust(textureLoad(picture, source, 0).rgb, present_state.adjust), 1.0);
}
