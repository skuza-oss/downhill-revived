// native.wgsl's dual-source entry point (raster.hlsl native_pixel's two outputs): appended to
// native.wgsl after "enable dual_source_blending;" when the device has the feature.
struct NativeDual {
    @location(0) @blend_src(0) color: vec4<f32>,
    @location(0) @blend_src(1) blend: vec4<f32>,
}

@fragment
fn native_pixel_dual(i: Varyings) -> NativeDual {
    nat = native_draws[i.draw];
    let uv = texture_uv(i);
    let across = dpdx(uv);
    let down = dpdy(uv);
    let color = shade(i, uv, across, down);
    var o: NativeDual;
    o.color = vec4<f32>(color.rgb / 255.0, saturate(color.a / 255.0));
    o.blend = vec4<f32>(0.0, 0.0, 0.0, saturate(color.a / 128.0));
    return o;
}

@fragment
fn native_pixel_dual_u(i: Varyings) -> NativeDual {
    nat = native_draws[i.draw];
    let uv = texture_uv(i);
    let across = dpdx(uv);
    let down = dpdy(uv);
    let color = shade_u(i, uv, across, down);
    var o: NativeDual;
    o.color = vec4<f32>(color.rgb / 255.0, saturate(color.a / 255.0));
    o.blend = vec4<f32>(0.0, 0.0, 0.0, saturate(color.a / 128.0));
    return o;
}

// The two entry points above for a mesh draw (native.wgsl's MeshVaryings).
@fragment
fn native_pixel_dual_mesh(m: MeshVaryings) -> NativeDual {
    let i = mesh_varyings(m);
    nat = native_draws[i.draw];
    let uv = texture_uv(i);
    let across = dpdx(uv);
    let down = dpdy(uv);
    let color = shade(i, uv, across, down);
    var o: NativeDual;
    o.color = vec4<f32>(color.rgb / 255.0, saturate(color.a / 255.0));
    o.blend = vec4<f32>(0.0, 0.0, 0.0, saturate(color.a / 128.0));
    return o;
}

@fragment
fn native_pixel_dual_u_mesh(m: MeshVaryings) -> NativeDual {
    let i = mesh_varyings(m);
    nat = native_draws[i.draw];
    let uv = texture_uv(i);
    let across = dpdx(uv);
    let down = dpdy(uv);
    let color = shade_u(i, uv, across, down);
    var o: NativeDual;
    o.color = vec4<f32>(color.rgb / 255.0, saturate(color.a / 255.0));
    o.blend = vec4<f32>(0.0, 0.0, 0.0, saturate(color.a / 128.0));
    return o;
}
