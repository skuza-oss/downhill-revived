# 084: The wgpu renderer — moving the GS from D3D11 to WebGPU

Status: partly superseded by 085 (the C wgpu-native backend, gpu_wgpu.c)
Checked: 2026-11-04

The D3D11 → WebGPU analysis, the WGSL port of the shaders (§2, W4 depth plane, §12, §14 DXC) and
the test plan are current and cited by `port/gs/*.wgsl`, `test_depth.c`, `dxc.py`. The backend it
first built in C on wgpu-native (`gpu_wgpu.c`) was replaced by the Rust renderer on wgpu
(`port/render`, `port/gs/gpu_rust.c`; findings 085, 088 CL2); wgpu-native is no longer used.

This is the design document for phase 5 of finding 081 ("the platforms: wgpu renderer, Linux, web,
Android, iOS"). It inventories what the PS2 GS renderer asks of D3D11, maps every use onto webgpu.h,
decides how `raster.hlsl` becomes WGSL, pins the wgpu-native version and its build/link story,
settles the window and surface question against SDL3's own GPU API, and gives a test plan and a
costed phase plan. It changes no code. §9 (the path check against ps2-web-recomp) and §10 (what was
then built, with measurements and corrections to this design) were added the same day.

Everything below was read out of this tree at the commit above, or out of the headers and
specifications listed in Sources. Where a WebGPU answer does not exist, the document says so instead
of inventing one.

## 0. What is being ported

`port/gs/` is the whole GS. Its D3D11 surface is small in interface terms: the GS calls the backend
through six function pointers in `Gs` (`port/gs/gs.h:56-74` — `sync`, `sync_range`, `draw`,
`local_copy`, `stage_pages`, `fetch_pages`) and four free functions (`port/gs/gpu.h:4-8` — destroy,
trace present, profile, display, capture). Behind that sit two *renderers* with very different fates
under WebGPU:

- **reference (exact) — `port/gs/gpu.c`.** A binned software rasterizer written
  as compute shaders. `flush()` (`gpu.c:486-555`) bins pending primitives into
  8×8 tile lists on the CPU, uploads primitive/bin/uniform buffers, and issues
  one `Dispatch(width, height, 1)`. Each thread owns one 8×8 tile and walks that
  tile's primitive list serially, so GS ordering is preserved *without* any
  rasterizer-ordered resource. VRAM lives in a 4 MiB raw UAV byte-address
  buffer, written with masked read-modify-write CAS (`raster.hlsl:106-125`).
- **hardware — `port/gs/gpu_hw.inc`.** A real D3D11 rasterizer draw that emits
  the same pixels: `hardware_submit()` (`gpu_hw.inc:50-87`) binds no render
  target, only the VRAM UAV, and draws `pending_count*3` vertices with
  conservative rasterization tier 3 (`gpu_hw.inc:99-106`). The pixel shader
  writes VRAM through a **`RasterizerOrderedByteAddressBuffer`**
  (`raster.hlsl:24`) — a genuine ROV.
- **native presentation renderer — `port/gs/gpu_native.inc`.** A conventional
  D3D11 render-target pipeline (vertex+pixel, blend/depth state objects,
  scaled surfaces, `convert`/`blit`/`alpha upload` helper passes) used when
  `DHD_RENDER=native` (`gpu.c:1028-1045`).

The reference renderer is the authority: `flush()` prefers the hardware path only when
`hardware_rejection()` proves the batch safe, and falls back to the compute path for every other
state (`gpu_hw.inc:28-48`, `gpu.c:530-542`). That structure is exactly what the port needs, because
**the compute path is already shaped like WebGPU compute** and the hardware path is already shaped
like nothing WebGPU can express.

### Verdict up front

Keep the compute path as the portable authority, drop the hardware bridge on every non-D3D11
backend, and treat `gpu_native.inc` as a render-pipeline port with three real losses (ROV-free
self-sampling, dual-source blending on the web, typeless depth views). Hand-port `raster.hlsl` to
WGSL. Do not attempt DXC→SPIR-V→naga as the shipping route; keep it only as a reading aid while
porting.

## 1. D3D11 API inventory and the webgpu.h equivalent

### 1.1 Method

The column "WebGPU" names the actual webgpu.h struct/function, not a description. "Gap" means
webgpu.h v29 has no equivalent expression of the same semantics.

### 1.2 Device, context, shaders

| D3D11 | Where | webgpu.h v29 | Notes / gap |
|---|---|---|---|
| `D3D11CreateDevice` + `D3D11_FEATURE_LEVEL_11_1/11_0` (`gpu.c:1032-1036`) | `gs_gpu_init` | `wgpuCreateInstance`, `wgpuInstanceRequestAdapter`, `wgpuAdapterRequestDevice` | Async in the C API: the init path becomes two polls. One `WGPUDevice` for everything; the "second device for FP64" (`gpu.c:1033-1038`) has no analogue. |
| `ID3D11DeviceContext` | everywhere | `WGPUQueue` + `WGPUCommandEncoder` | Encoders are per-frame objects, not a retained context. |
| `D3DCompile` / `d3dcompiler_47.dll` (`gpu.c:101`, `145`) | `shader_compile` | `wgpuDeviceCreateShaderModule` with `WGPUShaderSourceWGSL` | Compilation happens inside the driver; there is no `D3DCOMPILE_IEEE_STRICTNESS` knob. |
| shader disk cache (`gpu.c:83-159`) | `shader_compile` | wgpu's own pipeline cache (`wgpuDeviceCreatePipelineCache`) + wgpu's on-disk shader cache on Vulkan/DX12 | The hand-rolled `%TEMP%`-style cache keyed on compiler size/date can go. |
| `ID3D11ComputeShader` ×3 (`gpu.c:42,71`) | exact | `WGPUComputePipeline` | Three pipelines: main, scanout, local_copy. |
| `ID3D11VertexShader`/`ID3D11PixelShader` (`gpu.c:43-44`) | hardware | — | **Gap.** Dies with the hardware bridge. |
| `vs_5_0`/`ps_5_0`/`cs_5_0` (`gpu.c:972,979,986`, `gpu_hw.inc:115,122`) | both | — (WGSL) | See §2. |

### 1.3 Buffers and views

| D3D11 | Where | webgpu.h v29 | Notes / gap |
|---|---|---|---|
| 4 MiB `ID3D11Buffer` + `ALLOW_RAW_VIEWS` (`gpu.c:992-994`) | VRAM | `WGPUBuffer` (4 MiB) with `WGPUBufferBindingType_Storage` / `_ReadOnlyStorage` | Plain storage buffer of `u32`. No raw-view flag needed. |
| `snapshot` buffer, `BIND_SHADER_RESOURCE` (`gpu.c:995-996`) | texture reads see a coherent snapshot | second `WGPUBuffer` | `copyBufferToBuffer` VRAM→snapshot per batch instead of `CopyResource`. |
| `readback`/`staged` STAGING (`gpu.c:997-999`) | read-back | `WGPUBuffer` with `MAP_READ` and `COPY_DST` | **Gap in shape:** WebGPU map is async (`wgpuBufferMapAsync`) and a buffer may not be mapped while pending. Needs a small ring of readback buffers plus `wgpuDevicePoll`. |
| `D3D11_BUFFER_UAV_FLAG_RAW` / `BUFFEREX_SRV_FLAG_RAW` (`gpu.c:1006-1012`) | raw views | `array<u32>` / `array<atomic<u32>>` in WGSL | **Gap:** there are no typed UAV formats — a WebGPU storage buffer is typed (`array<T>`), so the `R32_TYPELESS` raw view becomes `array<u32>` and every raw `Load`/`Store` becomes indexed `u32` access. `Load` of an `atomic<u32>` must become `atomicLoad`. |
| `ID3D11Buffer` constant buffers (`gpu.c:1000-1005`, `size 48`) | `State`/`Display`/`Copy`/`Native`/`NativeConvert`/`NativeBlit` | `WGPUBuffer` (`WGPUUniform`) + dynamic offsets | Content must respect WGSL's uniform address-space layout rules: `uniform` arrays of `uint4` are fine, but scalar/vec3 members need explicit padding. |
| `D3D11_USAGE_DYNAMIC` + `MAP_WRITE_DISCARD` vertex/constant buffers (`gpu_native.inc:1816-1825`, `1923-1928`) | native | `WGPUBuffer` + a per-frame upload ring, `wgpuQueueWriteBuffer` | **Gap:** no discard rename. Use one big uniform/vertex arena with dynamic offsets; `minStorageBufferOffsetAlignment` is 256 and dynamic uniform offsets must be 256-aligned. |
| `CopySubresourceRegion` boxed byte ranges (`gpu.c:335-337`) | `sync_range` | `wgpuCommandEncoderCopyBufferToBuffer` | Copy offsets and sizes must be multiples of 4; 8192-byte pages are. |
| `UpdateSubresource` (`gpu.c:402,524-528`) | uploads | `wgpuQueueWriteBuffer` | Only legal outside a pass, and only on buffers with `COPY_DST`. |
| `ClearRenderTargetView`/`ClearDepthStencilView` (`gpu_native.inc:1580,1587,2051`) | native | `WGPULoadOp_Clear` on the pass attachment | **Gap:** clears are pass state, not commands. |

### 1.4 Textures and views

| D3D11 | Where | webgpu.h v29 | Notes / gap |
|---|---|---|---|
| `CreateTexture2D` RGBA8 display + storage readback (`gpu.c:852-859`) | scanout | `WGPUTexture` `RGBA8Unorm` + `WGPUStorageTexture` + `COPY_SRC` | `RGBA8Unorm` is storage-capable in core. `BGRA8Unorm` storage needs the `bgra8unorm-storage` feature (`WGPUFeatureName_BGRA8UnormStorage`, value `0x0D`), so keep `rgba8unorm` and swap at present. |
| `RWTexture2D<float4> screen : register(u1)` (`raster.hlsl:40`) | scanout | `texture_storage_2d<rgba8unorm, write>` | Needs `WGPUStorageTextureAccess_WriteOnly`; `read_write` exists but only for formats and adapters that support it. |
| `R32_TYPELESS` depth texture, `DSV` + `R32_FLOAT` `SRV` (`gpu_native.inc:1572-1579`) | the depth haze reads Z as a texture | `Depth32Float` + a *separate* sampleable texture | **Gap.** WebGPU has no typeless view: a `Depth32Float` texture can be bound as a depth attachment *and* sampled (aspect `depth-only`, sample type `depth`/`unfilterable-float`), but `depth32float` is not a valid copy destination and has no colour-aspect view. The native path's "same memory, two formats" trick must become "copy Z into an `r32float` texture", which `depth32float` cannot do — so the copy must go through a compute pass (`textureLoad` + storage write). |
| `depth24plus` alternative | — | `Depth24Plus` | Not copyable at all. `Depth32FloatStencil8` exists behind `WGPUFeatureName_Depth32FloatStencil8` (value `0x03`). |
| `Texture2D<float>` `convert_depth0..3` (`raster.hlsl:620-623`) | convert | `texture_2d<f32>` with `unfilterable-float` | Sampler-less `Load` maps to `textureLoad`, which WebGPU supports on unfilterable-float. **No sampler-less-load gap.** |
| `Texture2D<float4>` ×5 + `t6` structured (`raster.hlsl:614-619`) | convert | 10 sampled textures + 1 read-only storage buffer | Fits the default `maxSampledTexturesPerShaderStage` of 16, with no headroom. |
| `SamplerState` ×several (`gpu_native.inc:2793`) | native | `WGPUSampler` | `D3D11_COMPARISON_NEVER` → `WGPUCompareFunction_Never` is *not* what "no comparison" means; omit the comparison and set `WGPUCompareFunction_Undefined`. |
| `IDXGISwapChain` + `ResizeBuffers` + `Present` (`present.c:93-241`, `468-479`) | present | `WGPUSurface` + `wgpuSurfaceConfigure` + `wgpuSurfacePresent` | See §4. |
| `DXGI_FORMAT_R8G8B8A8_UNORM` back buffer (`gpu_native.inc:2030`) | present | `wgpuSurfaceGetPreferredFormat` | Windows prefers `bgra8unorm`; the present pipeline's target format is a runtime value, not a constant. |

### 1.5 Pipeline state

| D3D11 | Where | webgpu.h v29 | Notes / gap |
|---|---|---|---|
| `RSSetViewports`/`RSSetScissorRects` per draw (`gpu_native.inc:1911-1922`) | native | `wgpuRenderPassEncoderSetViewport` / `SetScissorRect` | Allowed mid-pass. Cheap. |
| `RSSetState` + `D3D11_RASTERIZER_DESC2` w/ `ConservativeRaster=ON`, `ForcedSampleCount=1` (`gpu_hw.inc:102-106`) | hardware | — | **Gap, fatal for that path.** `GPUPrimitiveState` (`webgpu-spec`, §"GPUPrimitiveState") carries only topology, strip index format, front face, cull mode and `unclippedDepth`; there is no conservative rasterization and no forced sample count. |
| `RasterizerOrderedByteAddressBuffer` (`raster.hlsl:24`) | hardware | — | **Gap, fatal.** WebGPU has no rasterizer-ordered resources at all: no ROV texture, no ROV buffer, no ordering guarantee between fragments writing the same storage location. |
| `OMSetDepthStencilState` cached by key (`gpu_native.inc:1515-1527`) | native | `WGPUDepthStencilState` inside `WGPURenderPipelineDescriptor` | **Gap:** state is part of the pipeline object. A `depth_key`/`blend_key` cache becomes a pipeline cache. |
| `OMSetBlendState` with `D3D11_BLEND_SRC1_ALPHA` (`gpu_native.inc:1501-1504`) | native GS blend | `WGPUBlendComponent` with `WGPUBlendFactor_Src1Alpha` behind `WGPUFeatureName_DualSourceBlending` (value `0x11`) | **Gap in reach, not in the API.** `raster.hlsl:522` returns `SV_Target0`/`SV_Target1`; WGSL has no dual-source output, so the second output must be folded into the blend factor and the pipeline must be recompiled without it, or the feature must be required. Browsers do not expose the feature. |
| `D3D11_BLEND_OP` set (`gpu_native.inc:1505`) | native | `WGPUBlendOperation` = Add, Subtract, ReverseSubtract, Min, Max | **No gap.** The GS blend only ever produces op 1/2/3 (`gpu_native.inc:723-725`), all present. The factor table is also covered — see §1.8 item 8. |
| `PSSetSamplers` per draw (`gpu_native.inc:1891`) | native replacement textures | `wgpuRenderPassEncoderSetBindGroup` | Fine. |
| `OMSetRenderTargets` toggling to `NULL` around copies (`gpu_native.inc:1880,1897`) | native | end/begin a render pass | **Gap:** every attachment change is a pass boundary; the render loop becomes a pass builder, not a command recorder. |

### 1.6 Synchronisation, timing, diagnostics

| D3D11 | Where | webgpu.h v29 | Notes / gap |
|---|---|---|---|
| `D3D11_QUERY_TIMESTAMP`/`TIMESTAMP_DISJOINT` (`gpu.c:171-174`, `228-240`, `948-954`, `gpu_native.inc:2087-2117`) | `DHD_GPU_TRACE`, `DHD_PROFILE` | `WGPUQuerySet` (`Timestamp`) + `wgpuCommandEncoderWriteTimestamp` | **Gap by default.** `timestamp-query` (`WGPUFeatureName_TimestampQuery`, `0x09`) is optional, timestamps are quantised, and browsers mask them. Trace mode must degrade to wall-clock only. Note also that `WriteTimestamp` is disallowed inside a compute pass on some backends, so the paired begin/end stamps around a dispatch (`gpu.c:545-547`) need `timestamp_writes` on the pass descriptor. |
| `ID3D11DeviceContext_Flush` (`gpu.c:370`) | `fetch_pages` | `wgpuQueueSubmit` (always) | No separate flush; submit is the flush. |
| `D3D11_ASYNC_GETDATA_DONOTFLUSH` polling (`gpu.c:232-238`) | trace collection | `wgpuDevicePoll(wait=false)` | Fine. |
| `Map`+`Unmap` on a STAGING buffer (`gpu.c:298-309`) | every `sync` | `wgpuBufferMapAsync` + `wgpuDevicePoll` | **Architectural gap:** `sync`/`sync_range` are synchronous (`gpu.h` vtable, called from the EE thread), so the wgpu backend must block on `wgpuDevicePoll(instance, true, …)`. On the web there is no blocking wait — that path needs JSPI/Asyncify, which finding 083 already runs. |

### 1.7 Counted usage

Raw symbol counts across `port/gs/*` put `D3D11_BIND_SHADER_RESOURCE` (14), `D3D11_USAGE_DEFAULT`
(13), `D3D11_BOX` (10) and `D3D11_MAP_READ` (8) at the top, followed by
`D3D11_BIND_UNORDERED_ACCESS` (5) and `D3D11_QUERY_TIMESTAMP` (3). Nothing exotic is left over: no
geometry shaders, no tessellation, no stream-out, no indirect draws, no MSAA (`SampleDesc.Count = 1`
everywhere), no mip generation, no BC formats (textures are decoded by the CPU in
`texpack.c`/`gpu_native.inc`), no `D3D11_BIND_RENDER_TARGET` other than the native surfaces and the
back buffer.

### 1.8 The gap list, in the order it hurts

1. **ROV + conservative rasterization** (§1.5). The whole hardware bridge is
   unportable. Decision: the wgpu backend has no hardware path; `DHD_GS_HARDWARE`
   becomes a no-op there and the stats counters report
   `GS_GPU_HW_UNAVAILABLE`. PCSX2's Metal backend proves you *can* emulate GS
   ordering on an API without ROV by rendering into tile memory and running a
   merge pass (`pcsx2/GS/Renderers/Metal/GSDeviceMTL.mm`, `BeginFullROV`,
   `m_merge_pipeline`) — but that is a per-platform rewrite of the native path,
   not a port, and it buys nothing this project needs.
2. **No 64-bit floats.** `raster.hlsl:236-247,284-287` evaluates the depth plane
   in `double` under `precise`, and `exact_init` refuses to start without
   `D3D11_FEATURE_DOUBLES` (`gpu.c:921-926`). WGSL's scalar types are `bool`,
   `i32`, `u32`, `f32`, `f16` (extension) only — no `f64` — and 64-bit atomics
   are absent as well. See §2.4; this is the single largest technical risk in
   the port.
3. **No synchronous readback** (§1.6). Reshapes `sync`, `sync_range`,
   `stage_pages`, `fetch_pages` and `gs_gpu_capture`.
4. **No typeless depth view** (§1.4). A copy-through-compute is needed for the
   depth haze.
5. **No dual-source blending on the web** (§1.5). Native-only feature; the GS
   blend needs a second formulation.
6. **`atomicCompareExchangeWeak` may fail spuriously.** WGSL explicitly allows
   it to return `exchanged: false` without performing the exchange. The D3D11
   loop (`raster.hlsl:120-124`) breaks on `old == prior`, which is *not* a
   correct WGSL loop; the port must test `result.exchanged`. Silent pixel loss
   if it does not.
7. **256-byte row alignment.** `gs_gpu_capture` (`gpu.c:889-901`) copies rows
   with `w*4` bytes. `bytesPerRow` must be a multiple of 256, so any display
   width that is not a multiple of 64 needs a padded staging copy and a
   row-by-row unpad. This is not hypothetical: `test_gpu.c:217-226` already
   drives `gs_gpu_display` to 80×66, whose stride is 320 bytes. GS display
   widths (computed at `gpu.c:841-842` from `DISPLAY1/2`) are arbitrary, and
   the test proves it.
8. **Timestamp queries optional** (§1.6). Trace and bench numbers lose their
   GPU-side split on adapters without the feature.
9. **Storage-buffer and texture limits.** `native_convert` binds 10 sampled
   textures and 1 storage texture per dispatch. Defaults are 16 and 4
   respectively (`maxSampledTexturesPerShaderStage`,
   `maxStorageTexturesPerShaderStage`), so it fits and nothing more.

Two things that look like gaps are not, and are worth recording so nobody re-derives them:

- **Blend factors and ops.** `WGPUBlendOperation` has the five values D3D11 has,
  and the GS only uses three (`gpu_native.inc:723-725`). The factor set the GS
  can produce is eight values wide — `BLEND_ZERO`, `BLEND_ONE`, `BLEND_SA`,
  `BLEND_ISA`, `BLEND_DA`, `BLEND_IDA`, `BLEND_F`, `BLEND_IF`
  (`gpu_native.inc:705`) — and every one has a `WGPUBlendFactor` equivalent
  (`Zero`, `One`, `SrcAlpha`, `OneMinusSrcAlpha`, `DstAlpha`,
  `OneMinusDstAlpha`, `Constant`, `OneMinusConstant`). The per-draw constant
  `factor[4]` (`gpu_native.inc:1916-1918`) maps to
  `wgpuRenderPassEncoderSetBlendConstant`, which is legal mid-pass, so `BLEND_F`
  and `BLEND_IF` come out exact rather than approximate.
- **Sampler-less texture loads.** `Texture2D<float>.Load` on `convert_depth0..3`
  (`raster.hlsl:620-623`) maps to `textureLoad` on an `unfilterable-float`
  binding, which is core WebGPU. No `TEXTURE_BINDING`-without-sampler gap.

## 2. `raster.hlsl` → WGSL

### 2.1 What the shader actually is

One 34 KB file whose entry points are selected by `D3D_SHADER_MACRO` defines (`gpu.c:966-971`,
`gpu_hw.inc:111-114`) and by the entry name handed to `D3DCompile` (`gpu.c:972-989`,
`gpu_hw.inc:115-126`):

| Entry point | Stage | Threads | Purpose |
|---|---|---|---|
| `main` (`raster.hlsl:390-396`) | compute | 8×8 | the reference GS rasterizer |
| `local_copy` (`:400-406`) | compute | 8×8 | VRAM→VRAM page copy |
| `scanout` (`:409-417`) | compute | 8×8 | VRAM → `RGBA8` display texture |
| `hardware_vertex`/`hardware_pixel` (`:425`, `:443`) | vs/ps | — | ROV hardware bridge (dropped) |
| `native_vertex` (`:465`) / `native_pixel` (`:523-569`) | vs/ps | — | the native renderer |
| `native_scanout` (`:573-581`), `native_blit` (`:586-590`), `native_upload_alpha` (`:594-600`), `native_convert` (`:639-663`) | compute | 8×8 | native helper passes |
| `present_vertex`/`present_pixel` (`:670-679`) | vs/ps | — | display texture → back buffer |

Language features in use: `cbuffer` with `uint4` arrays (`:13-21`), raw and typed buffers
(`:22-27`), `StructuredBuffer` (`:36-37`), `RWTexture2D` (`:40`), `[numthreads]`, `uint`/`int` bit
twiddling, `float`/`half` maths, `precise` (`:284`), `double` and `asdouble`/`asuint` (`:236-247`),
`ddx`/`ddy` (`:527`), `discard` (`:562`), dual-source `SV_Target0/1` (`:522`), and ROV buffer writes
(`:24`, `:121`).

Every one of those has a WGSL counterpart **except** `precise`, `double`, ROV writes, and
dual-source output. That is the whole of the porting problem. Two of the four — `precise` and
`double` — sit in the reference rasterizer and are the hard problem (§2.4); the other two are
confined to the native pipeline and the hardware bridge.

### 2.2 Route comparison

**(a) Hand-port to WGSL.** Rewrite the file as `raster.wgsl` (plus small `#define`-equivalent
variants via naga `@id` specialisation or separate entry points). Full control over the exactness
questions in §2.4, which is the decisive argument: no automatic converter will preserve the
`precise`/`double` depth plane, and no converter can be trusted for the CAS loop semantics. Cost is
real (the file is dense), but mechanical: the arithmetic is already integer in the hot path, and the
reference path has no hardware state to model.

**(b) DXC → SPIR-V → naga → WGSL.** Attractive because `dxc -spirv` already
handles the HLSL we have, and naga's `spv-in` is a primary front-end.
Disqualifying for shipping:
- naga has **no HLSL front-end** — the naga front-end table lists SPIR-V, WGSL
  and GLSL only (`naga/README.md`, "Supported end-points"). So HLSL must go
  through DXC to SPIR-V first, and DXC's SPIR-V output of `cs_5_0` with
  `RasterizerOrdered*` and `precise double` is either rejected or silently
  lowered.
- **naga's WGSL back-end is its weakest link.** The README marks `spv-in`,
  `spv-out`, `wgsl-in` and `msl-out` as primary (`:white_check_mark:`) but
  `wgsl-out` and `glsl-out` only as secondary (`:ok:`). WGSL is exactly the
  target this would depend on.
- Browsers accept WGSL only, so the SPIR-V stage is dead weight the moment the
  web target matters. It cannot be the shipping artefact.

**(c) DXC → DXIL and run through `naga`'s HLSL backend.** Not a route: naga's `hlsl-out` produces
HLSL, which is the wrong direction, and DXIL is not an input to anything here.

**(d) Slang `-target wgsl`.** Slang does have a WGSL target, but its own documentation says "Slang
support for WebGPU is a work in progress" (`shader-slang.org/slang/user-guide/targets.html`,
§WebGPU) and the target notes list the exact things this shader needs as unsupported: **64-bit
atomics are not supported** (table: 32-bit integer *Yes*, everything else *No*), `Interlocked*` is
unsupported in favour of `Atomic<T>`, and `RWByteAddressBuffer` becomes `array<u32>` in `storage` —
which is the right answer, but it also means Slang is *already* telling us the 64-bit atomic and
`double` path has no expression. Using Slang would also add a compiler dependency the project does
not otherwise carry, for a 500-line shader.

**(e) Write GLSL and let naga's `glsl-in` convert.** `glsl-in` is Vulkan-semantics GLSL 440+,
secondary support, and would put the source in a language neither the D3D11 reference nor the
compiler tests speak.

**Recommendation: (a) hand-port to WGSL**, with (b) used strictly as a *reading aid* — run `dxc
-spirv` on the existing HLSL and `naga file.spv file.wgsl` to get a machine translation to diff
against the hand port, and run `naga raster.wgsl` to validate. Never ship the machine output. Keep
`raster.hlsl` and the D3D11 backend alive as the oracle until the wgpu backend is bit-identical
(§5). This also keeps one shader language in the tree, which matters because `test_runtime.py:74-78`
and `replay_gs.py` embed the shader source into a generated header; a second language doubles that
machinery.

### 2.3 Mechanical mapping

| HLSL | WGSL | Hazard |
|---|---|---|
| `cbuffer State : register(b0) { uint4 regs[128]; ... }` | `struct State { regs: array<vec4<u32>,128>, ... }; @group(0) @binding(0) var<uniform> s: State;` | 3248 bytes (`Constants` in `gpu.c:25-28`: 128x16 + 3x16 + 3x16 + 3x16 + 4x4 + 4x4 + 256x4), under the 64 KiB uniform limit. `array<vec4<u32>,N>` stride is 16, matching HLSL. `float4 stq[3]` becomes `array<vec4<f32>,3>` — fine. |
| `ByteAddressBuffer texture_memory` | `var<storage, read> tex: array<u32>;` | `Load(off)` → `tex[off >> 2]`. |
| `RWByteAddressBuffer memory` | `var<storage, read_write> mem: array<atomic<u32>>;` | `Store` is fine (storage write), **`Load` must become `atomicLoad`**. |
| `InterlockedCompareExchange(...)` loop | `atomicCompareExchangeWeak` loop testing `.exchanged` | See §1.8 item 6. |
| `StructuredBuffer<Primitive>` | `var<storage, read> prims: array<Primitive>;` | Match the 208-byte struct layout exactly (`Primitive` in `gpu.c:30-35`); `uint4 position[3]` etc. are already 16-aligned. |
| `RWTexture2D<float4>` | `var t: texture_storage_2d<rgba8unorm, write>;` | `textureStore(t, xy, v)`. |
| `Texture2D<float4>.Load(int3)` | `textureLoad(t, vec2<i32>, 0)` | No sampler needed. |
| `Texture2D<float>.Load` w/ unfilterable float | `texture_2d<f32>` + `textureLoad` | Bind as `unfilterable-float`, not `float`. |
| `SamplerState` + `SampleLevel`/`Sample` | `var s: sampler;` + `textureSampleLevel` | `textureSample` only in fragment shaders; `SampleLevel` in compute → `textureSampleLevel`. |
| `ddx`/`ddy` | `dpdx`/`dpdy` | Fragment only, as today. |
| `discard` | `discard` | Same. |
| `[numthreads(8,8,1)]` | `@workgroup_size(8,8,1)` | Same. |
| `SV_DispatchThreadID` | `@builtin(global_invocation_id)` | Same. |
| `SV_Target0/1` dual source | `@location(0)` + a second colour target, or fold into the blend factor | §1.5. |
| `precise double` depth plane | none | §2.4. |
| `asdouble(a,b)` / `asuint(d)` | none | The CPU already stores the three doubles as `uint4 depth[2]` (`raster.hlsl:34`, `:284-286`), so the *bit pattern* crossing the boundary is unchanged; only the arithmetic changes. |

### 2.4 The depth plane: the one genuinely hard problem

The GS Z for a pixel is

```
depth = d0 + dzdx*(x - ax) + dzdy*(y - ay)        // all double
z     = depth_integer(depth)                       // -> u32, clamped
```

with `d0`, `dzdx`, `dzdy` computed on the CPU as exact doubles and shipped as `uint4 depth[2]`
(`raster.hlsl:284-286`). `depth_integer` (`:236-247`) is already written defensively — it splits
into two 16-bit pieces and corrects float rounding at the boundaries — precisely because the shader
must land on the *same integer* the hardware GS would. `DHD_EARLY_DEPTH` then compares that integer
against the 16- or 24-bit Z in VRAM (`:296-303`).

This matters beyond looks: `port/gs/test_gpu.c` asserts **exact VRAM words** (`CHECK(at(x,y) ==
(0x80442200 | (pass ? 1 : 0)))` and the plane test at `:114-116`), and `replay_gs.py` produces
golden PPMs. A one-LSB Z difference that flips one depth test changes a word and fails a test. So
"close enough" is not available; the port must reproduce the integer, not the double.

Three ways, in order of preference:

1. **Double-double (2×f32) evaluation, then the same `depth_integer` split.**
   Dekker/Knuth `two_sum` + `two_prod` gives ~106 bits of significand — more
   than the 53 the original has, so if the *inputs* are exact (they are: the
   doubles are shipped as bit patterns and can be split exactly into hi+lo
   `f32`), the affine result is exact to well beyond what `depth_integer`'s
   16-bit split needs. **Risk:** WGSL has no `precise` and does not forbid
   FMA contraction, so a backend may fuse `a*b + c` and break `two_prod`.
   Mitigations: express the products as `fma`-free explicit steps with
   intermediate variables naga cannot fold, and — the real gate — measure.
2. **64-bit fixed point with `u32` pairs.** Exact and deterministic, no float
   at all, and immune to contraction. Needs a range/precision analysis of
   `dzdx`/`dzdy` (they can be tiny at the far plane and huge across a clipped
   triangle), so the Q-format must be chosen per primitive on the CPU. More
   work, zero numerical risk.
3. **Keep the depth plane on the CPU.** Impossible per-pixel at 2048×2048.

**Decision.** Attempt (1) first, behind a switch, and gate it on a Z-dump comparison across the
whole replay corpus; if any word differs, fall back to (2). Do **not** ship a wgpu backend whose Z
is "usually right": the existing word-exact tests are the project's definition of correct, and
silently relaxing them would be the one thing finding 084 must not do. Budget the fallback in the
plan (§6) rather than discovering it late.

`DHD_EARLY_DEPTH`'s comparison chain (`raster.hlsl:289-306`) is unaffected: it is already pure
`u32`.

### 2.5 The hardware bridge under WGSL

Dropped, as decided in §1.8. Concretely: `raster.hlsl:23-27` and `:106-133` compile `memory` as a
`RasterizerOrderedByteAddressBuffer` under `DHD_HARDWARE_PIXEL`, and `test_hw_coverage.hlsl:15` uses
`RasterizerOrderedStructuredBuffer<uint2>` outright. The WGSL port keeps the `RWByteAddressBuffer`
form (the `#else` branch) and the hardware bridge is `#if`-ed out of the wgpu build. `gpu_hw.inc`
itself stays as the D3D11-only fast path on Windows — it is a *win* to keep, not dead code.

## 3. wgpu-native: version, binaries, licence, linking, browser

### 3.1 Version

**v29.0.1.1**, released 2026-06-23 (the newest release at the time of writing). wgpu-native versions
are wgpu-core's version plus a fourth *build* number, so v29.0.1.1 tracks wgpu 29.0.1;
`wgpuGetVersion()` returns that as one byte per component. The release ZIP contains
`wgpu-native-meta/wgpu-native-git-tag` carrying the literal string `v29.0.1.1` — use that, not the
filename, as the build identity to record.

### 3.2 Binaries

Per-platform archives, `wgpu-<os>-<arch>-<toolchain>-release.zip`:

| OS | Arch | Toolchains |
|---|---|---|
| Windows | x86_64 | msvc, gnu |
| Windows | aarch64, i686 | msvc |
| Linux | x86_64, aarch64 | gnu |
| macOS | x86_64, aarch64 | — |
| iOS | aarch64 (+ simulator arches) | — |
| Android | aarch64, armv7, i686, x86_64 | — |

Inside each: `include/webgpu/webgpu.h` (the standard C header, generated from `webgpu.yml`; 266 KB
at v29), `include/webgpu/wgpu.h` (the wgpu-native extensions — 57 KB), `lib/` with a shared library,
an import library, a static library, and a PDB, plus `wgpu-native-meta/`.

```
include/webgpu/webgpu.h               266,335
include/webgpu/wgpu.h                  57,477
lib/wgpu_native.dll                 9,117,696
lib/wgpu_native.dll.lib                62,344
lib/wgpu_native.lib                54,811,560   (static)
lib/wgpu_native.pdb                 4,231,168
wgpu-native-meta/{webgpu.yml, wgpu-native-git-tag}
```

Two facts that matter for this project's build:

- `HOST_TARGET` is `i686-pc-windows-msvc` (`port/tools/pctarget.py`) and the
  trees link with `lld-link` against the MSVC CRT. **The `i686-msvc` archive is
  the relevant one for today's `dhd.exe`**; the x86_64 one applies to the phase
  4 64-bit host. There is no `i686-gnu` archive, which is fine — the project
  does not use the GNU toolchain on Windows.
- The static library is 54 MB before linking. It will be pruned by `lld-link`
  to what is referenced, but the *build* step must not copy it into the tree:
  download and extract under `build/`, like `sdl3.py` does, and add a SHA-256
  pin (the release assets have stable hashes; `sdl3.py` sets the precedent at
  `port/tools/sdl3.py:26-27`).

### 3.3 Licence

wgpu-native is dual-licensed **MIT OR Apache-2.0** (the repository carries `LICENSE.MIT` and
`LICENSE.APACHE`; the release ZIP does not, so the notice has to be quoted from the repo). Both are
compatible with this project's distribution, and the existing `port/release/THIRD-PARTY-NOTICES.txt`
is the place a line goes. `include/webgpu/webgpu.h` itself is BSD-3-Clause per its header
("Copyright 2019-2023 WebGPU-Native developers, SPDX-License-Identifier: BSD-3-Clause") — worth a
separate line, since it is vendored source, not a binary.

### 3.4 Linking

- **Windows, shared:** link `wgpu_native.dll.lib`, ship `wgpu_native.dll`
  (9 MB) next to the exe. Simplest, but the release currently ships a single
  self-contained exe (`package_release.py` checks the tree for stray paths), so
  a side-by-side DLL is a packaging change.
- **Windows, static:** link `wgpu_native.lib`. 54 MB of object code, pruned at
  link; this keeps the single-file release. It also matches how SDL3 is already
  built and linked statically (`port/tools/link.py:291-293`). Prefer this.
- **Linux/macOS:** static where the archive offers it, otherwise a bundled
  shared library with an rpath — a phase-5 detail for the Linux work, not this
  document.
- The C API is a flat `extern "C"` surface (`wgpuCreateInstance`,
  `wgpuAdapterRequestDevice`, …), so no C++ ABI question arises — which matters
  here because the project deliberately uses `windows-itanium`, not the MSVC
  C++ ABI (`pctarget.py` docstring). A C-only dependency sidesteps all of it.

### 3.5 The browser story

The browser needs *no* wgpu-native. It needs three things:

1. **The same C code compiled to wasm.** `wgpuCreateInstance` etc. are provided
   by **Emscripten's `emdawnwebgpu` port** (`--use-port=emdawnwebgpu`). Note
   that `-sUSE_WEBGPU` was **removed in Emscripten 4.0.18 (2025-10-24)** in
   favour of the port flag, so any snippet found online that says
   `-sUSE_WEBGPU` is stale.
2. **A canvas, not an HWND.** The Emscripten port adds its own surface source —
   a canvas HTML selector, in `webgpu_emscripten.h`, not in the native
   `webgpu.h` — which takes either a `#canvas` selector or, if SDL owns the
   canvas, `SDL_PROP_WINDOW_EMSCRIPTEN_CANVAS_ID_STRING`. Browser surfaces also
   have a `configure`-time size and a per-frame `getCurrentTexture` that native
   does not: the swap chain texture may be invalidated on resize.
3. **No blocking.** `mapAsync` + `wgpuDevicePoll(wait=true)` is a deadlock on
   the main thread of a browser tab. Everything in `sync`/`sync_range`/
   `fetch_pages` must become JSPI-async (finding 083 already runs the core
   under JSPI in Node) or the whole GS must move to a worker with a
   SharedArrayBuffer handshake.

The compression of that: **one C renderer, two hosts.** Native links wgpu-native; web links
emdawnwebgpu. The differences are the surface source, the async readback and the absence of
`timestamp-query` — all of them behind the same three seams (§5).

## 4. Presentation: SDL3 surface or SDL3 GPU API

### 4.1 What exists today

`port/gs/present.c` is raw Win32 + DXGI: a dedicated window thread (`present.c:60-78`),
`CreateWindowA` (`:71`), a flip-model swap chain with
`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` and optional tearing (`:154`), `ResizeBuffers`
on `WM_SIZE` (`:54`, `315`), and `gs_present()` (`:468-479`). There is no SDL video anywhere:
`port/tools/sdl3.py` builds SDL 3.4.18 with `SDL_VIDEO=OFF, SDL_GPU=OFF` and only
joystick/gamepad/HIDAPI on (`sdl3.py:38-40`). SDL3 is already a hard, static, pinned dependency —
for controllers (`port/hle/gamepad.c:20`, `link.py:173-180`).

### 4.2 Option A — SDL3 window + `wgpuSurface` from the native handle

SDL3 creates the window; the renderer asks SDL for the platform handle and feeds it to
`wgpuInstanceCreateSurface` through the matching `WGPUSurfaceSource*`:

| Platform | SDL3 property | webgpu.h source struct | Handle field |
|---|---|---|---|
| Windows | `SDL_PROP_WINDOW_WIN32_HWND_POINTER` (`"SDL.window.win32.hwnd"`) | `WGPUSurfaceSourceWindowsHWND` (`webgpu.h:3260`) | `hwnd`, `hinstance` |
| Windows (store/UWP) | `SDL_PROP_WINDOW_WIN32_HWND_POINTER` | same | same |
| Linux/X11 | `SDL_PROP_WINDOW_X11_WINDOW_NUMBER` | `WGPUSurfaceSourceXlibWindow` (`:3327`) | `display`, `window` |
| Linux/X11 (xcb) | `SDL_PROP_WINDOW_X11_WINDOW_NUMBER` | `WGPUSurfaceSourceXCBWindow` (`:3294`) | `connection`, `window` |
| Linux/Wayland | `SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER` | `WGPUSurfaceSourceWaylandSurface` (`:3227`) | `display`, `surface` |
| macOS | `SDL_PROP_WINDOW_COCOA_WINDOW_POINTER` (an `NSWindow*`) | `WGPUSurfaceSourceMetalLayer` (`:3201`) | `layer`, i.e. `[window contentView].layer` as a `CAMetalLayer` |
| iOS | `SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER` | `WGPUSurfaceSourceMetalLayer` | `layer` |
| Android | `SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER` | `WGPUSurfaceSourceAndroidNativeWindow` (`:3175`) | `window` |
| Web | `SDL_PROP_WINDOW_EMSCRIPTEN_CANVAS_ID_STRING` | Emscripten canvas source | canvas id / `#canvas` |

Then `wgpuSurfaceGetCapabilities` → pick a format → `wgpuSurfaceConfigure` (width, height,
`presentMode`), and per frame `wgpuSurfaceGetCurrentTexture` → render → `wgpuSurfacePresent`.

What this keeps: the entire swap-chain policy that `present.c` already learned the hard way —
tearing when available, frame-latency-1 pacing, `WM_SIZE`-driven reconfigure, fullscreen transitions
(`present.c:248-276`), minimise handling (`gpu_native.inc:2026`), and the dedicated window thread
(`present.c:60-78`). What it changes: Win32 message pumping moves behind SDL (`SDL_PollEvent`), and
the window thread either goes away or keeps using `SDL_PumpEvents` — SDL's event queue is not
thread-safe per window, so the thread arrangement needs rework either way.

The one hard WebGPU constraint to respect: `wgpuSurfaceGetCurrentTexture` returns the same texture
object until `wgpuSurfacePresent`; a resize must reconfigure, and on the web an out-of-date texture
is an error, not a retry.

### 4.3 Option B — SDL3 GPU API instead of wgpu

SDL3's `SDL_GPU` is a small, SDL-flavoured wrapper over D3D12/Vulkan/Metal (and WebGPU on the web).
It is tempting because SDL is already linked.

1. **Shaders are precompiled and format-specific.** `SDL_GPUShader` takes
   `SDL_GPU_SHADERFORMAT_SPIRV | DXBC | DXIL | MSL | METALLIB` code — a blob per
   backend. That means shipping a compiled artefact per platform and losing
   runtime WGSL entirely, which contradicts §2's whole recommendation and kills
   the browser path (which needs WGSL).
2. **It is a narrower API than the project needs** in exactly the places the GS
   is demanding: `SDL_GPU_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE` exists, but
   there is no cross-backend story for the CAS loop's semantics, no timestamp
   queries at the granularity `DHD_GPU_TRACE` uses, and no storage-texture
   read-write tier control — and because the shaders are still per-backend
   blobs, every one of those questions would be answered per backend anyway.
3. **It would replace a surface abstraction with a renderer abstraction.** The
   D3D11 backend, the D3D12 path, the tests and the web build would all have to
   go through SDL's pipeline objects. wgpu is the thinner layer.

Verdict: **Option A.** Keep SDL3 for window/input/audio, build it with `SDL_VIDEO=ON, SDL_GPU=OFF`,
and give wgpu the raw handles. SDL becomes the window owner on every platform, which also retires
the Win32-specific `present.c` and is a prerequisite for Linux/Android/iOS anyway.

### 4.4 What the surface does *not* solve

`gs_present` (`present.c:468-479`) is one call; the surrounding logic is not. `native_present`
(`gpu_native.inc:2014-2078`) computes a letterboxed viewport with aspect handling
(`native_viewport`, `:2003-2012`), a 4-way split-screen `DHD_VIEW` crop that draws the image 2× and
scissors it to a quarter (`:2043-2049`), and a pacing function `pacing_present` (`:2070`). All of
that survives verbatim; it just sets `wgpuRenderPassEncoderSetViewport` and `SetScissorRect` instead
of the D3D11 calls.

## 5. Test plan, parameterised by backend

### 5.1 The seams that have to exist before any test can be parameterised

1. **`GsBackend` selection.** Today `gs_gpu_init` (`gpu.c:1028-1045`) hard-codes
   D3D11 and picks exact-vs-native from `DHD_RENDER`. Add
   `DHD_GS_BACKEND=d3d11|wgpu` plus a `gs_backend.h` vtable mirroring the six
   `Gs` pointers plus display/capture. The tests then take the backend from the
   environment instead of always building `gpu.c`.
2. **A software-adapter mode.** `test_gpu.c:325` already runs on
   `D3D_DRIVER_TYPE_WARP`, so the CI floor exists on D3D11. The wgpu equivalent
   is `WGPURequestAdapterOptions.forceFallbackAdapter = true` (Vulkan on
   `lavapipe`, DX12 on WARP), and the wgpu backend must pass the whole suite
   there. That also means requesting no optional feature without an explicit
   `wgpuAdapterHasFeature` check — and treating
   `WGPUFeatureName_CoreFeaturesAndLimits` (the v29 header's `0x01`) as the
   floor rather than assuming it.
3. **A shader-source seam.** `test_runtime.py:74-78` and `replay_gs.py:25-27`
   both embed `raster.hlsl` into `gs_shader.h` as a C string. The wgpu backend
   needs the same treatment for `raster.wgsl` (one more file in the generator),
   or `#embed`-style inclusion; either way the two generators must stay in
   lockstep or the tests silently run stale shaders.

### 5.2 The matrix

Each test runs against `{d3d11, wgpu} × {exact, native} × {feature-tier}`:

| Test | What it proves | Backend-parameterised how |
|---|---|---|
| `port/gs/test_gpu.c` | VRAM words: swizzles, TFX, alpha test, mip chain, the depth plane (`:114-116`), and the display readback at 80×66 (`:217-226`) — a row stride of 320 bytes, which WebGPU cannot copy directly | backend from env; expectations unchanged — this is the golden set |
| `port/gs/test_gs.c` | register-level GS behaviour, VRAM uploads, transfers | unaffected by renderer, but must run after a wgpu init |
| `port/gs/test_hw_renderer.c` | the hardware bridge's rejection rules and primitive conversion | d3d11-only for the draw path; the *rejection* logic must still compile and run under wgpu (it decides `GS_GPU_HW_UNAVAILABLE` there) |
| `port/gs/test_hw_coverage.c` + `.hlsl` | ROV coverage equivalence | d3d11-only; under wgpu, assert the bridge reports unavailable and the compute path produces the same VRAM |
| `port/gs/test_native_transfers.c` | import/blit/convert/alpha-upload paths | runs on both; this is where the depth-haze copy-through-compute and the padded readback are proven |
| `port/gs/test_native_frontend.c` | command-stream shape | renderer-agnostic |
| `port/gs/test_capture.c` | capture format | d3d11-only (records a D3D11 swap chain), skip on wgpu |
| `replay_gs.py` corpus | golden PPM per field | run twice, diff images; this is the end-to-end gate |

### 5.3 New tests this document requires

- **Z-exactness harness.** Dump the Z buffer (or the `depth_integer` value per
  pixel) from both backends for every replay capture and compare word by word.
  This is the gate on §2.4 option (1). Without it, "the pictures look the same"
  is the only evidence, and that is not this project's standard.
- **Readback alignment harness.** Display widths spanning `64·n` and `64·n ± 1`
  wrapped through `gs_gpu_capture` to prove the 256-byte padding/unpad. Start
  from the 80×66 case `test_gpu.c:217-226` already produces (320-byte stride),
  then sweep the odd widths `gs_gpu_display` derives from `DISPLAY1`
  (`gpu.c:841-842`).
- **CAS-contention test.** Force `batch_safe = 0` (packed formats) so the
  atomic loop is exercised on overlapping words, and check for lost writes.
  WGSL's spurious-failure rule makes this a real risk, not a formality.
- **Feature-absence matrix.** Run the suite on an adapter with
  `dual-source-blending`, `timestamp-query` and `bgra8unorm-storage` denied, and
  assert the renderer still produces correct images (with degraded tracing).
  This is the browser profile.
- **Fallback-adapter run.** `forceFallbackAdapter = true`, full suite. This is
  the CI floor and the closest thing to a "must work on anything" guarantee.

### 5.4 Non-goals for the test plan

No pixel-perfect comparison against PCSX2, no reference capture from a real PS2, no GPU-vendor
matrix. The D3D11 backend remains the oracle; the goal is "wgpu == d3d11", not "wgpu == hardware".

## 6. Phased plan and risks

### 6.1 Phases, in hours

They assume §2.4 option (1) works.

| # | Phase | Deliverable | Hours |
|---|---|---|---|
| W0 | **Seams** | `gs_backend.h` vtable, `DHD_GS_BACKEND`, shader-source generator for `.wgsl`, fallback-adapter handling, `wgpuCreateInstance`/adapter/device init, `wgpuGetVersion` build stamp, download+pin script for wgpu-native | 16 |
| W1 | **Surface + present** | SDL3 `SDL_VIDEO=ON` build option, window creation through SDL on Windows, `WGPUSurface` from `SDL_PROP_WINDOW_WIN32_HWND_POINTER`, configure/reconfigure on resize, `native_present`'s viewport/letterbox/`DHD_VIEW` maths on a wgpu pass, the `present` pipeline | 24 |
| W2 | **VRAM + transfers** | 4 MiB storage buffer, snapshot copy, `sync`/`sync_range`/`stage_pages`/`fetch_pages` on a readback ring with `mapAsync`+poll, 256-byte-padded `gs_gpu_capture` | 20 |
| W3 | **Compute rasterizer in WGSL** | `raster.wgsl` for `main` + `scanout` + `local_copy`; the CAS loop fix; binding layout; parity on `test_gpu.c` except the depth plane | 40 |
| W4 | **Depth plane** | double-double implementation, Z-dump harness, corpus run; **plus** the fixed-point fallback if §2.4 (1) fails | 16 (+24 fallback) |
| W5 | **Native render pipeline** | vertex/pixel WGSL for `native_pixel`; blend/depth/pipeline cache keyed like `gpu_native.inc:1493-1527`; dynamic-uniform-offset arena replacing `Map(WRITE_DISCARD)`; render-pass builder replacing the `OMSetRenderTargets(NULL)` pattern; self-copy pass breaks | 40 |
| W6 | **Native helper passes** | `native_convert` (10 sampled textures, storage texture), `native_blit`, `native_upload_alpha`, `native_scanout`, depth-haze copy-through-compute replacing the typeless view | 24 |
| W7 | **GS blend without dual source** | reformulate `SRC1_ALPHA` (`gpu_native.inc:1501`, `raster.hlsl:522-567`) so it works with and without `dual-source-blending`; prove image equality on both | 16 |
| W8 | **Tests + corpus** | parameterise the seven test binaries, the feature-absence matrix, fallback-adapter run, replay corpus diff, CI wiring | 24 |
| W9 | **Trace/bench degradation** | `timestamp-query` optional path; wall-clock fallback; keep the `DHD_GPU_TRACE` CSV columns stable so existing analysis still parses (`gpu.c:942-946`) | 8 |
| | **Total** | | **228** (252 with the fixed-point fallback) |

Ordering is deliberate: W4 is placed before the native pipeline because a failure there changes the
shader design, and discovering that after W5–W7 would mean redoing them.

### 6.2 Risks

| Risk | Likelihood | Impact | Response |
|---|---|---|---|
| **`atomicCompareExchangeWeak` spurious failure** loses VRAM writes under contention | Medium | High — silent corruption | Test with `batch_safe = 0` (§5.3). The loop must test `.exchanged`. Cheap fix once known; expensive if found in a race replay. |
| **Synchronous readback does not survive the web** | High (it is certain) | Medium — blocks the web phase, not native | Native keeps `wgpuDevicePoll(wait=true)`; the web path is JSPI/Asyncify, which finding 083 already proved in Node. Do not design the vtable around it. |
| **Dual-source blending unavailable in browsers** | High | Medium — the GS blend needs a second formulation | W7 is a hard requirement, not an optimisation. Gate on the feature where present, use the reformulation everywhere else, and prove image equality. |
| **wgpu-native i686 is unproven for this use** | Low–Medium | Medium — the 32-bit exe is the shipping artefact today | The 32-bit build is a *transition* target (phase 4/5 of 081 move to a 64-bit host). If i686 misbehaves, land the wgpu backend on x64 and keep D3D11 on i686 until the host moves. |
| **54 MB static library bloats the build and the release check** | Medium | Low–Medium | Put it under `build/`, pin the SHA-256, and check what `lld-link` actually keeps. `package_release.py`'s path scan may need a rule, as it did for SDL's `atan` table. |
| **Pipeline-count explosion** in the native path | Medium | Medium — startup hitch | The state caches at `gpu_native.inc:1493-1527` already bound the space (32 blends × 16 depth keys). Pre-warm on first use rather than during the first draw. |
| **Pass-break cost** from self-copy and convert interleaving | Medium | Medium — frame time | The command stream is already a flat list (`gpu_native.inc:1828-1931`), so the pass builder can reorder independent work and coalesce breaks. Measure against `DHD_PROFILE` before optimising. |
| **Typeless depth view loss changes the haze** | Low | Medium | Copy-through-compute in W6; verify against `test_native_transfers.c` and the corpus. |
| **256-byte padding bug is visually invisible** in the letterbox | Medium | Low | The dedicated width-sweep harness (§5.3) rather than eyeballing. |
| **wgpu-native v29 churn** | Low | Low | Pin the tag, record it with `wgpuGetVersion`, and treat upgrades as their own finding. |

### 6.3 What is explicitly *not* planned

- No attempt to emulate ROV or conservative rasterization on WebGPU.
- No reimplementation of the GS as a hardware rasterizer under WebGPU.
- No SPIR-V artefact in the release.
- No SDL_GPU renderer.
- No change to `src/` or `include/`, and no weakening of any existing test.

## 7. Decisions, in one place

1. **Reference compute renderer is the portable backend.** The hardware bridge
   stays D3D11-only and reports unavailable elsewhere.
2. **WGSL, hand-written, is the shipping shader language.** DXC→SPIR-V→naga is
   a reading aid; Slang and GLSL front-ends are rejected.
3. **Depth Z is reproduced exactly**, by double-double first and 64-bit fixed
   point if the measurement disagrees. No tolerance is introduced into the
   existing VRAM-word tests.
4. **wgpu-native v29.0.1.1, statically linked on Windows**, MIT/Apache-2.0,
   downloaded and SHA-256-pinned under `build/`; the browser uses Emscripten's
   `emdawnwebgpu` port and never wgpu-native.
5. **SDL3 owns the window; wgpu gets the raw handle.** `SDL_GPU=OFF` stays off.
6. **The `Gs` vtable and `DHD_GS_BACKEND` are the seam**, so every existing test
   runs twice and the D3D11 backend stays as the oracle until the corpus is
   bit-identical.
7. **Dual-source blending is a feature, not an assumption**, and the GS blend
   gets a formulation that does not need it.

## 8. Sources

Read while writing this finding; all accessed 2026-10-24.

- WebGPU specification, W3C Candidate Recommendation Draft 15 September 2026 —
  <https://www.w3.org/TR/webgpu/> (GPUPrimitiveState, GPUBlendOperation,
  GPUImageDataLayout `bytesPerRow` = 256, feature names, default limits).
- WebGPU Shading Language, W3C CRD 21 October 2026 —
  <https://www.w3.org/TR/WGSL/> (scalar types §6.2.5: `i32`/`u32`/`f32`/`f16`,
  no `f64`; atomic types §6.2.8: `atomic<i32>`/`atomic<u32>` only;
  `atomicCompareExchangeWeak` spurious failure).
- wgpu-native repository and release assets —
  <https://github.com/gfx-rs/wgpu-native> (dual MIT/Apache-2.0).
- wgpu / naga source tree, `naga/README.md` front-end and back-end tables —
  <https://github.com/gfx-rs/wgpu/blob/trunk/naga/README.md>.
- Slang user guide, "Supported Compilation Targets", WebGPU section —
  <https://shader-slang.org/slang/user-guide/targets.html>.
- Slang user guide, "WGSL Target-Specific Functionalities" —
  <https://github.com/shader-slang/slang/blob/master/docs/user-guide/a2-03-wgsl-target-specific.md>
  (atomic-type table, `Interlocked*` unsupported, `RWByteAddressBuffer` →
  `array<u32>` in `storage`).
- SDL3 `SDL_CreateWindow` properties and `SDL_PROP_WINDOW_*` —
  <https://wiki.libsdl.org/SDL3/SDL_CreateWindow>,
  <https://wiki.libsdl.org/SDL3/SDL_PROP_WINDOW_WIN32_HWND_POINTER>,
  <https://wiki.libsdl.org/SDL3/SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER>.
- SDL3 GPU API and shader formats —
  <https://wiki.libsdl.org/SDL3/CategoryGPU>,
  <https://wiki.libsdl.org/SDL3/SDL_GPUShaderFormat>.
- Emscripten WebGPU port (`-sUSE_WEBGPU` removed in 4.0.18, `emdawnwebgpu`) —
  <https://emscripten.org/docs/porting/connecting_cpp_and_javascript/WebGPU.html>.
- PCSX2 GS backends, used as the comparable for "PS2 GS without ROV" —
  <https://github.com/PCSX2/pcsx2/tree/master/pcsx2/GS/Renderers/Vulkan>,
  <https://github.com/PCSX2/pcsx2/tree/master/pcsx2/GS/Renderers/Metal>
  (`GSDeviceMTL::BeginFullROV`, `m_merge_pipeline` in `GSDeviceMTL.mm`).
- ps2-web-recomp, Samir Nuri, MIT — <https://github.com/snuri00/ps2-web-recomp>, commit
  `333999564ad0fe84f24b504416869dc182404a61`, `runtime/src/gs` (§9: interface, accuracy,
  batching design; no code copied).

## 9. Path check: our WGSL port or ps2-web-recomp's WebGPU GS

Two paths:

- **(a)** our exact compute rasterizer (`raster.hlsl`, the D3D11 oracle) ported to WGSL. This is
  §2's plan. W0, W2, W3 and W4 are built and bit-identical (§10).
- **(b)** ps2-web-recomp's WebGPU GS (<https://github.com/snuri00/ps2-web-recomp>,
  `runtime/src/gs`, MIT, "Copyright (c) 2026 Samir Nuri", read at commit `33399956`) adopted as
  our renderer behind `port/gs`: our GS stream goes in and a picture comes out. Native would
  use wgpu-native and the browser Dawn.

Read: `gpu/gs.wgsl` (1144 lines), `gpu/blit.wgsl`, `gpu/gpu.cpp` (1862), `gs.hpp`, `queue.hpp`. It
runs Downhill Domination PAL at full speed in Chrome. Its design is good, and its speed comes from
the queue, not from the shader maths.

### 9.1 Interface fit

The fit is at the draw hook, and it is easy. Their `gs::write`, `write_packed`, `write_image` and
`read_image` are our `gs_write`, `gs_feed` and `gs_read_image` in `gs.c`. Their `gpu::draw(State,
Vertex*, count)` is our `g->draw(g, type, v, count)`. Their `local_copy`, `download` and
`display(DisplaySource)` are our `g->local_copy`, `g->sync`/`sync_range` and `gs_gpu_display`.
`gpu.cpp` would sit beside `gpu_wgpu.c` as one more `GsGpuBackend` (C++17, compiled with clang-cl
like the rest). Their `attach_surface` (X11, Wayland, canvas) is the same idea as our
`gs_gpu_surface`.

### 9.2 Accuracy against the GS rules our checks enforce

(b) is approximate by design, in exactly the places our gate checks bit for bit:

| | ps2-web-recomp (`gs.wgsl`) | ours (`raster.hlsl`/`raster.wgsl`) |
|---|---|---|
| Triangle coverage | f32 barycentrics at the pixel corner, `w > 0 \|\| (w == 0 && inclusive)` (`shade_at`, `edge_in`) | the GS's integer edge test on 12.4 coordinates |
| Z | `z0 + floor(f32 dz)`, where dz comes from f32 barycentrics times f32 Z deltas (`z_at`) | the exact binary64 plane, `d0 + dzdx·dx + dzdy·dy`, floored (§2.4, §10.4) |
| Sprites | f32 texture interpolation, with `abs(du) >= abs(dx) * 0.999` heuristics for point sampling | the GS's sprite stepping |
| Lines | the pixel found by rounding three guesses along the line | the GS's line walk |
| Mipmapping (TEX1 LOD, MIPTBP1/2) | none | yes, checked by test_gpu's `mipmapping` section |
| Dither (DTHE/DIMX) | none | yes |
| Partial-word stores | `atomicAnd` then `atomicOr` | one CAS loop on `.exchanged` (§2.3) |

I modelled their `z_at` on 200,000 random triangle pixels, with vertices on the 1/16 grid of a
640×448 screen, and compared it with the exact plane:

| Z format | pixels that differ | median error | maximum error |
|---|---|---|---|
| Z24 | 41,096 (20.5 %) | 1 | 595 |
| Z32 | 196,895 (98.4 %) | 27 | 486,318 |

On 5.24 M cases per device (§10.4), all three of our exact paths (CPU, D3D11, WGSL) give 0. Random Z
ranges are harsher than a game's. Still, test_gpu's `depth precision` section alone would fail under
(b), and so would the VRAM words that later Z tests read back. I did not run (b) under the PCSX2-GS
referee. That would mean building their C++ runtime around our capture format, which takes hours,
and the code above already settles the question: these are design choices, not bugs.

### 9.3 What (b) lacks against what our renderer supports

- **Exactness:** see above. Our gate is VRAM words, not pictures.
- **Render-to-texture and framebuffer-as-palette:** (b) handles both well. It tracks texture
  ranges against the batch's frame and Z ranges, uses a partial `snapshot` of only the shared
  words, a CLUT slot ring and `texflush` generations. Ours handles them too: a snapshot SRV
  and `texture_pages`/`independent_pixels` in `gpu.c`. These match in function.
- **Upscaling:** (b) keeps `SCALE`×`SCALE` samples per VRAM word (`PS2_SCALE` 1–3), so the
  swizzled layout survives upscaling. That is a neat trick. Ours upscales in the separate
  native renderer (`gpu_native.inc`, 2962 lines: `DHD_SCALE`, widescreen, texpack,
  font_sharp, `DHD_VIEW`). (b) has no widescreen projection, no texture packs and no sharp
  font.
- **D3D11 hardware bridge (ROV):** neither path has one on WebGPU (§2.5, §6.3).

### 9.4 Estimates (hours)

- **(a), from here:**
  - present.c on the surface: 3;
  - (b)-style batching (§9.5): 16–24;
  - native renderer phases W5–W7: 80;
  - tests and the browser build: 24–32.
  - Total: about 125–140, all inside our exactness gate. W0, W2, W3, W4 and the W1 backend
    are done.
- **(b):**
  - wrapping `gpu.cpp` as a `GsGpuBackend`: 20–30;
  - replacing coverage, Z, sprite and line maths and adding mipmapping and dither, so that
    test_gpu, test_hw_renderer and the replays pass bit for bit: 50–70 (in effect porting
    `raster.hlsl` into their shader);
  - checking their swizzle tables, CLUT, alpha, fog and FBA against the oracle: 15;
  - the native-renderer features: the same 80 as (a).
  - Total: about 165–195, and the exactness work is the same work (a) has already finished.

### 9.5 Recommendation: (a), taking (b)'s design

Keep our WGSL port, which already proves exact. Adopt (b)'s engineering, which is where its speed
comes from (credit it in the files that use it):

1. **Record, then submit once.** Commands go into a list, data into one arena buffer, and
   per-command parameters into one records buffer. Each flush point (readback, present) gets
   one `wgpuQueueWriteBuffer` per buffer and one compute pass. Today `gpu_wgpu.c` submits
   every dispatch on its own, so a frame's 600-odd dispatches cost 600-odd submits.
2. **Many GS states per dispatch.** Each primitive carries a setup index into deduplicated
   state records (`SETUP_WORDS`). A batch flushes only on a real hazard: the texture overlaps
   the frame or Z written so far, an upload overlaps a pending read, or the CLUT changes.
   Ours flushes on every state change. This is (b)'s 650→80 dispatches per frame.
3. **Dispatch only touched tiles.** Send a compacted list of tiles that have primitives, not
   the bounding grid with empty groups returning early.
4. **A texture decode cache, keyed by setup, and a content-keyed upload cache.**

None of this touches per-pixel arithmetic, so the VRAM-word gate is unchanged. No code from (b) is
in the tree yet. When the batching lands, its file gets (b)'s MIT notice and a pointer here.

## 10. Built (W0, W1, W2, W3, W4) and measured

Device: NVIDIA GeForce RTX 3070 (hardware), WARP / "Microsoft Basic Render Driver" (software).

### 10.1 What exists

| Piece | Where | Notes |
|---|---|---|
| Backend seam | `gpu_backend.h`, `gpu_backend.inc` (end of `gpu.c`) | `DHD_GS_BACKEND=d3d11\|wgpu` per `Gs`; the D3D11 code is unchanged, its functions only renamed by macro to `d3d11_*` behind the table |
| WebGPU backend | `gpu_wgpu.c` | `gpu.c`'s CPU half copied unchanged (binning, page tracking, batch rules); 4 MiB VRAM storage buffer; snapshot copy; readback by `mapAsync` + `wgpuDevicePoll`; capture with 256-byte rows, unpadded on the CPU. It picks the D3D11 device's adapter by PCI ids, and WARP → the fallback adapter. `DHD_WGPU_ADAPTER`, `DHD_WGPU_BACKEND=vulkan`, `DHD_WGPU_DEBUG` |
| Shaders | `raster.wgsl` (main, local_copy, scanout, depth probe), `present.wgsl` | hand-ported from `raster.hlsl`; DX12 compiles them with FXC, like the oracle |
| Presentation (W1) | `gs_gpu_surface` / `gs_gpu_present`, `present.c` | a WGPUSurface on SDL's HWND; nearest-pixel stretch over the client area; frame latency 1; present mode Immediate (tearing), else Mailbox, else Fifo, matching the DXGI chain. present.c then creates no DXGI chain. |
| Game link | `link.py` | dhd.exe links the backend; D3D11 stays the default. dhd.exe grows from 6.4 MB to 13.5 MB. |
| Tests | `port/tools/test_gs_wgpu.py`, `test_present.c`, `test_depth.c`, `test_gpu_stubs.c`, `test_gpu.c` checkpoints, `test_hw_renderer --reference-backend`, `replay_gs.py --backend both` | |

### 10.2 Parity (both arches give the same result)

- **test_gpu.** It runs every check on each backend and logs the raw VRAM after each of 63
  checkpoints (1,048,576 words each), compared word for word:

  | Device, options | d3d11 | wgpu | VRAM d3d11 vs wgpu |
  |---|---|---|---|
  | RTX 3070, `DHD_GS_EARLY_DEPTH=0` | 0 failures | 0 failures | all identical |
  | WARP, `DHD_GS_EARLY_DEPTH=0` | 0 failures | 0 failures | all identical |
  | RTX 3070, defaults (1,1,1) | **233 failures** | 0 failures | 15 words differ, the D3D11 errors below |
  | WARP, defaults (1,1,1) | 233 failures | 24,574–24,822 failures | differ (WARP's DX12 JIT, below) |

- **test_hw_renderer** (`--reference-backend d3d11` and `wgpu`): 77 checks, 71 positive and 6
  fallback cases, 80,740,352 full-VRAM words, 0 failures, PASS on both.
- **test_present** (never-shown window, each presented texture read back):
  - 0 pixels differ at 1:1, 2×, 1280×720, shrunk to 200×150, and for a 512×256 CPU picture on
    896×672;
  - this holds on x64 DX12 (WARP and RTX 3070), x64 Vulkan and i686 Vulkan.
  - On i686 DX12 there is no surface; see 10.5.
- **replay_gs.py --backend both** on six real game captures (`release029/gate/exact/port-gs`,
  fields +0, +2, +12, +16, +3600, +4500): every picture, every field's whole VRAM (hash) and
  the last field's VRAM (word by word) are **bit-identical**.
- **i686 game, default backend, after the relink:**
  - menu, phase, clock, scratch, hw, vu0, gs_stream, gs_regs, gs_vram, sound and code are
    identical. cpu differs from field 150 (the known word).
  - ram differs from field 25, and in exactly one word, guest 0x0093C620. Dumps at boot fields
    25, 300 and 590 and race fields 2500 and 4990 show nothing else.
  - That word is a 64-bit register spill in `func_00254D68`'s stack frame (watched with
    `DHD_STATE_WATCH`). The spilled register holds a value derived from a game-data address
    inside dhd.exe's image: a1 = 0x4056E1A9 in the base and 0x40AE51A9 here, because
    wgpu-native makes the image about 5.5 MB larger before that data. Base 0x4657FDxx, here
    0x46AFFDxx, with the same low half.
  - So it is a layout effect of the relink, not a behaviour change. Two runs of one build
    give the same value.

### 10.3 What the D3D11 oracle got wrong (found by the wgpu comparison; fixed in round 2)

With all three shader options on (the runtime default since finding 032), test_gpu's D3D11
run failed 233 `depth precision` checks, on WARP and on the RTX 3070 alike. Every failure was a
Z32 value at or above **0xFFFFFF00** (4294967040), written as 0xFFFFFFFF:
- 216 triangle pixels for bases 0xFFFFFF00 and 0xFFFFFFF0, across all three passes;
- 2 pixels from the masked-Z check;
- 15 pixels from the line at 0xFFFFFFF0..0xFFFFFFFE.

**Cause: the shader cache held a blob compiled under the EE thread's SSE mode.** (Round 1 put
it at 2^31 and in FXC's codegen for that option set; both were wrong.)
- The game compiles its shaders with `D3DCompile` on the EE thread. That thread runs with
  MXCSR set to round toward zero with FTZ/DAZ (`port/host/host_win32.c`).
- FXC folds constants with the caller's FPU mode. Under round-toward-zero, `depth_integer`'s
  clamp literal `4294967295.0` became `4294967040.0` (`d(4294967040.000000l)` in the
  disassembly, against `d(4294967296.000000l)` from a normal compile). A few other constants
  and the register allocation changed with it.
- `shader_cache_path` keys the blob on source, entry, target, defines, flags and the
  compiler DLL, but not on the FPU mode. So the game's blob
  (`%LOCALAPPDATA%\DownhillRevived\shaders\main-c9052f889e9866b6.cso`, written by the game) was
  what test_gpu loaded for the default options.
- With the other option sets the game had never compiled anything, so test_gpu compiled them
  itself under the normal mode. That is why turning off any one option "fixed" it.

- test_gpu with `DHD_NO_SHADER_CACHE=1` gives 0 failures.
- test_gpu compiling under the EE's MXCSR (`(csr & 63) | 0xFFC0`, set just before
  `D3DCompile`) gives exactly the 233 failures, on WARP and on the RTX 3070.
- The fixed `shader_compile` under that same MXCSR gives 0.

- `shader_compile` saves MXCSR, compiles with round to nearest, no FTZ/DAZ and all exceptions
  masked (`(csr | 0x1F80) & ~(63 | 0xE040)`, the graphics threads' mode,
  `port/hle/graphics_queue.c`), then restores it.
- `shader_cache_path` hashes an `"ieee-fp"` tag, so no blob from before the fix is ever loaded
  again. The old files stay on disk unused.
- `D3DCompile` has no other caller in the game (`gpu_native.inc` goes through
  `shader_compile`). test_depth.c and test_hw_coverage.c compile their own probes, but on a
  thread in the normal mode.

After the fix, test_gpu's D3D11 runs give 0 failures with the cache on: defaults and
`DHD_GS_EARLY_DEPTH=0`, WARP and RTX 3070, on i686.

**The shipped game has this bug**, wherever it compiled the exact renderer's `main` itself, which is
everywhere: the cache is only ever filled by the game. A Z32 at or above 0xFFFFFF00 is stored as
0xFFFFFFFF. The game's Z values in the six captures and the 5000-field race never reach that range
(the replays are bit-identical to wgpu, which has no such bug). So no visible effect is known. The
native renderer's shaders went through the same compile and could have other folded constants; the
fix covers them too, since they share `shader_compile`.

```
gs: compile D3D11 shaders in IEEE mode (Z32 >= 0xFFFFFF00 written as 0xFFFFFFFF)

FXC folds constants with the caller's SSE mode. The game compiles on the EE
thread (toward zero, FTZ/DAZ), where 4294967295.0 folded to 4294967040.0, so
every Z32 at or above 0xFFFFFF00 was stored as 0xFFFFFFFF; the cached blob kept
it. shader_compile now compiles with nearest, no FTZ/DAZ (as the graphics
threads run) and puts the caller's mode back; the cache key gets an "ieee-fp"
tag so blobs compiled before are never used again.
```
The diff: `#include <xmmintrin.h>`; in `shader_cache_path`, after the `ftLastWriteTime` hash,
`h = fnv(h, "ieee-fp", 8);`; in `shader_compile`, `unsigned csr = _mm_getcsr();
_mm_setcsr((csr | 0x1F80u) & ~(63u | 0xE040u));` before `D3DCompile` and `_mm_setcsr(csr);`
after it. `git show 8eab06ac` applies to main as is.

### 10.4 Depth: the decision, from data

`test_depth`:
- generates depth planes the way `draw()` does: random triangles, small triangles, lines,
  far-24-bit planes and quarter-pixel planes;
- evaluates Z on the CPU (binary64), through `raster.hlsl` on D3D11 (dmul/dadd), through
  raster.wgsl's exact software binary64, and through the double-float pair the design
  proposed first (§2.4 (1)).

| | cases per device | D3D11 vs CPU | exact WGSL vs CPU | double-float pair vs CPU |
|---|---|---|---|---|
| i686, WARP and RTX 3070 | 5,242,880 | 0 | 0 | 1,226 (44 small, 67 lines, 3 far24, 1,112 quarters), each one too low by 1 |
| x86_64, WARP and RTX 3070 | 1,310,720 | 0 | 0 | 311 |

**Decision: exact software binary64 (`sd_*` in raster.wgsl), not double-float.** A double does not
split exactly into two f32s (24 + 24 < 53 bits), so the pair loses the low bits that decide `floor`
at integer boundaries. The exact path keeps the significand in two u32s and multiplies and adds in
96-bit limbs with 40 guard bits and a sticky bit, rounding to nearest even, as `dmul`/`dadd` do. It
uses integers only, so FMA contraction can't touch it. It is §2.4's fixed-point fallback in spirit,
but it reproduces binary64 itself, so it equals the oracle by construction rather than nearly.

### 10.5 Corrections to the design (§1–§6)

1. **The oracle has no `mad`.** The DXBC (IEEE strictness) computes interpolation as
   `(a·wx + b·wy) + c·wz`, `lerp(a,b,t)` as `a + (b−a)·t`, and depth as
   `fl(fl(fl(dzdx·fx) + d0) + fl(dzdy·fy))`. raster.wgsl blocks contraction with `keep()`: it
   ORs each product's bits with a uniform zero (`st.info.w`).
2. **`depth_integer` is `clamp(floor(z), 0, 2^32−1)`.** Its `4294967295.0` literal is a float,
   that is 2^32.
3. **D3D `ftoi`/`ftou` saturate** (NaN → 0). WGSL's conversions are implementation-defined
   outside range, so `f2i`/`f2u` reproduce D3D's.
5. **DX12 surface textures take RenderAttachment and CopySrc only (usages 0x11).** There is no
   CopyDst or storage, so presenting needs the render pass (`present.wgsl`), not a compute
   write or a copy.
6. **WARP's DX12 JIT loses `main`'s output for three of the four option sets tried.** Only
   (early 0, shift 1, store 1) passes on WARP; the same shaders pass under every set on the RTX
   3070, and D3D11 WARP runs raster.hlsl correctly. The WARP runs therefore also use
   `DHD_GS_EARLY_DEPTH=0` (`test_gs_wgpu.py`). Not pursued further.
7. **The standalone GS tests (test_gpu, test_hw_renderer, replay) did not link at the base**
   (native-renderer host symbols). `test_gpu_stubs.c` supplies them, plus `guest_base` for x64.
8. **wgpu-native rejects `WGPUErrorFilter_Internal`** in `wgpuDevicePushErrorScope` (it
   panics), and reports that pipeline failure as a Validation error. The present pipeline is
   built inside a Validation scope.

### 10.6 Not done / next

- **Batching (§9.5):** done in round 2, see §11.
- **The D3D11 early-depth Z32 failure (10.3).**
- **W5–W9**: the native renderer, helper passes, blending without dual source (W5–W7 done: §13),
  the test matrix and timestamps.

## 11. Round 2: batching (W5) and the game under wgpu

Commits: 8eab06ac (Z32 fix, §10.3), ebf69c56 (W5), b8330702 (`build_host64.py --wgpu`).

**W5, after ps2-web-recomp's GS** (MIT licence, Copyright (c) 2026 Samir Nuri; credited in
`gpu_wgpu.c`). Section 9.5's items 1–3 are in; item 4, the texture decode cache, is not.
- **One submit per flush point.** Commands are recorded in one encoder. Each submit writes
  four arenas, one queue write each: the state records, the primitives, the tile lists, and
  the batch records (256-byte stride, bound with a dynamic offset in an explicit layout for
  `main`).
- **What forces a submit.** Readback (sync, sync_range, fetch_pages, capture), scanout, local
  copies (their constants), an upload into a page already uploaded in this submit, and full
  arenas. Uploads go through an upload buffer and a copy in the stream. The texture snapshot
  is a copy in the stream too.
- **Several GS states per dispatch.** `Primitive.info.w` indexes a state record (registers
  and CLUT), deduplicated per state change. raster.wgsl reads `states[state]` where it read
  the uniform `st`, and `keep()`'s zero is now the batch record's `.w`.
- **When a batch ends.** At its size limit (`DHD_GS_BATCH`, 256), around a primitive whose
  pixels are not independent (it runs alone, as before), and before a state that has a
  different frame/Z layout from the batch and shares a page with it.
- **Only touched tiles.** Each batch's tile list starts with one (position, first, count)
  entry per touched 8×8 tile. `main` runs one workgroup per entry, so no empty workgroups are
  dispatched.
- **Check.** `replay_gs.py --backend both` on the six captures: all bit-identical to D3D11.
- **Timing.** wgpu replay of g+0, `DHD_GS_REPLAY_TIME=1` (new in replay.c), RTX 3070, median
  of 3: 80.8 ms before, 34.6 ms after.

**The game under `DHD_GS_BACKEND=wgpu`.** Runs were minimised (SW_SHOWMINNOACTIVE, never
focused), 60 s each, on the race recipe of bench_race.py with `DHD_PROFILE`. The PC was busy
with other lanes, so the rates are rough.
- **x64, `dhd64.exe` from `build_host64.py --wgpu` (W5):** presents on the WebGPU surface. It
  reached the menus and the race navigation at 10–11 fields/s, with about 2,000 dispatches/s
  in 42–51 submits/s. No errors in the log.
- **i686, `dhd.exe` before W5:** the present pipeline fails as in §10.5 (4), and the game
  falls back to D3D11 presentation. It ran at 7 fields/s, and draw took 660–745 ms of every
  second (one submit per dispatch).
- **i686, D3D11 (default backend), same recipe:** 9–12 fields/s.
- **Cold start.** The first wgpu run of each exe printed nothing after device init within
  60 s; the next run went on normally. The likely cause is a cold start (first-run shader and
  pipeline work). Not pursued.
- **Not done:** an i686 relink with W5, and the i686 DX12 render-pipeline problem (§10.5 (4);
  i686 Vulkan presents, per test_present).

## 12. Round 3: raster.wgsl in Chrome, and the browser replay

The separate browser build of gpu_wgpu.c (emscripten/Dawn) was dropped. This round made the shared
shader valid in Chrome and checked the result against the native replay.

**WGSL precedence.** naga parses binary operators with C's precedence: `||` < `&&` < `|` <
`^` < `&` < `==` `!=` < relational < shifts < `+` `-` < `*` `/` `%`. The WGSL grammar, which
Chrome's Tint enforces, is stricter:
- the operands of `&`, `|`, `^`, `<<` and `>>` must be unary expressions; `&`, `|` and `^` may
  chain only with the same operator;
- relational operands cannot be comparisons or bitwise expressions;
- `&&` and `||` cannot mix with each other or with bitwise expressions.

So `a | b << 1u` compiled natively and failed in Chrome ("mixing '|' and '<<' requires
parenthesis"). `port/tools/wgsl_parens.py` parses each expression with naga's precedence and
parenthesizes exactly the operands the grammar rejects, so the meaning cannot change. It added 107
pairs on 47 lines of raster.wgsl; present.wgsl needed none. `--check` reports without writing; new
WGSL can go through it before commit.

**Tint check.** `node port/tools/tint_check.mjs port/gs/raster.wgsl port/gs/present.wgsl` compiles
both files in headless Chromium: Playwright's Chromium 140, on the RTX 3070's D3D12. It also builds
raster.wgsl's four pipelines (main, local_copy, scanout, depth_probe) with the backend's override
constants. Result: 0 errors, all four pipelines ok.

- `--disable-dawn-features=use_dxc`: Playwright's build has no dxil.dll, so `requestDevice`
  fails otherwise.
- An explicit `Page.navigate`: the first tab is about:blank, where WebGPU is not exposed.

**Native.** The g+0 replay (`replay_gs.py --backend both`) is bit-identical to d3d11, and its VRAM
hash is unchanged from before the parentheses.

against the native one.
- It writes PREFIX-web-vram.txt in the native `-vram.txt` format, plus a screenshot.

g+0 gives the same per-field VRAM hashes as the native wgpu replay (6F6541B48360F8AA,
357EA3E7BF55F142), and the screenshot shows the correct start-gate picture (512x448).

The replay took 104 s, almost all of it compiling `main` at its first dispatch; tint_check's
pipeline builds take about 90 s too. That is FXC (DXC is off in this Chromium) on the large shader.
A stock Chrome uses DXC, which should be faster; this was not measured. If the first load is slow
there, the page should build the pipeline with `createComputePipelineAsync` while it shows something
else.

## 13. The native renderer on WebGPU (W20: W5, W6, W7)

`DHD_GS_BACKEND=wgpu` with `DHD_RENDER=native` (the game's default) now runs the native renderer:
upscaling (`DHD_SCALE`), widescreen, `DHD_ASPECT`, `DHD_VIEW`, `DHD_PRESENT_FILTER`, `DHD_INTERP`
and screenshots behave as on D3D11. The WebGPU exact backend answers the Z probe, and it clips its
draws the way gpu.c does.

### 13.1 Layout

| File | What it holds |
|---|---|
| `native_core.inc` | the part every API shares, moved out of `gpu_native.inc` without changing it: recording, the Z probe, decoding, frame hand-off, DHD_INTERP, reports. Each backend defines `NativeExact`, `NativeView`, `NATIVE_TEXTURE_GPU`, `NATIVE_SURFACE_GPU` and `NATIVE_GPU`, then includes it. The shared helpers (`native_prepare_surfaces`, `native_present_rect`, `native_create`, `native_install`, ...) replace code that was copied before. |
| `gpu_native.inc` | the D3D11 half, as before |
| `gpu_wgpu_native.inc` | the WebGPU half (included by gpu_wgpu.c) |
| `native.wgsl`, `native_dual.wgsl` | raster.hlsl's `DHD_NATIVE` shaders in WGSL, line for line |
| `gpu_backend.h/.inc` | `gs_native_*` dispatched through `GsGpuBackend`; each translation unit renames its own (`d3d11_native_*`, `wgpu_native_*`) |

### 13.2 How WebGPU differs from the D3D11 half

- **One encoder and one submit per picture (W5).** The vertices, every draw's constants and
  every pixel upload are queue writes made before that submit:
  - constants go to an arena with 256-byte dynamic offsets, replacing `Map(WRITE_DISCARD)` per
    draw;
  - pixel uploads go to an arena with rows padded to 256 bytes, then `copyBufferToTexture`
    inside the encoder.

  So imports, alpha uploads and draws keep their order. The arenas are sized from the field's
  commands before anything is recorded, because bind groups name the buffers.
- **Render passes replace `OMSetRenderTargets`.** A pass stays open while the target and Z
  surface stay the same. Copies, self-copies, uploads, conversions and blits end it.
- **Viewport and scissor.** The viewport is the whole attachment and `native_vertex` maps GS
  pixels onto it (`target_size` = attachment / T). WebGPU allows no viewport larger than the
  target, which D3D11's 2048·T one is. The scissor is clipped to the attachment.
  - Presentation does the same: whole-surface viewport, picture rectangle in a uniform, scissor
    to the picture. This also covers DHD_VIEW's 2× shift.
- **Z surfaces (W6).**
  - Native Z surfaces are depth32float. `native_convert` reads them as `texture_depth_2d`
    directly. This corrects §2/§6: no typeless view and no copy through compute are needed for
    the depth haze.
  - A Z surface that grows is filled by a `frag_depth` draw from the old one, because depth
    textures copy only whole.
  - New textures are zero, as D3D11's cleared ones are.
- **Out-of-range loads.** `textureLoad` outside a texture reads 0 (`load_rgba`/`load_depth`),
  as D3D11's `Load` does. WebGPU leaves that to the implementation.
- **GS blending without dual-source blending (W7).** This corrects §1's "WGSL has no
  dual-source output": wgpu has `enable dual_source_blending;` with `@blend_src`. It is
  requested when the adapter has it (`WGPUFeatureName_DualSourceBlending`, additive in
  `device_init`). Without it, a draw whose blend uses As (SA/ISA) and writes alpha is drawn
  twice:
  1. RGB with As / 128 as the source alpha (SrcAlpha factors, alpha masked).
  2. The alpha channel alone, unblended, on the pixels the first draw wrote. The Z test picks
     those pixels:
     - with Z writes, EQUAL against what the first draw left, and the triangles in reverse
       order for GREATER, where a later triangle at an equal Z lost;
     - without Z writes, the same test again.

  `DHD_WGPU_DUAL_SOURCE=0` forces this path. One case can differ, and the scenes don't exercise
  it: blending on Ad (`blend_dest_alpha`) with triangles of one draw that overlap. There, the
  second triangle sees the old alpha. (Round 5, §15: this case can't happen.)
- **Not on WebGPU yet:**
  - texture packs and the sharp font (D3D11 views in texpack.c and font_sharp.c); done in §15;
  - GPU timestamps (`DHD_NATIVE_TRACE` writes gpu_ms -1); still open, §15;
  - the vsync present mode for DHD_INTERP (the surface keeps the exact backend's mode); done in §15.

Draw pipelines are built when first needed: 0.03–0.4 s for all of them on Vulkan and DX12.

### 13.3 Results

| Check | Result |
|---|---|
| `test_native_transfers.py` (D3D11 i686, unchanged checks) | 13/13 PASS after the split |
| `test_native_transfers.py --backend both` (x64; 13 transfer checks plus 16 new blend/Z scenes: As, Ad, FIX, subtract, masks, ALWAYS/GEQUAL/GREATER with and without Z writes, AFAIL) | every check passes on all three runs. **Dual-source vs two-draw: 29 of 29 pictures identical.** VRAM after every picture identical on all three. D3D11 vs WebGPU: the 13 transfer pictures are identical. The 16 scenes differ by ±1 in one channel on 34–50 of 458,752 pixels each, and the unblended scene does too, so the cause is Gouraud interpolation (naga's HLSL/SPIR-V and the NDC mapping), not blending. |
| `replay_gs.py gs-g+3600.gs --backend both --native` (r47 race capture, scale 2, WebGPU on Vulkan) | both fields: **0 of 917,504 pixels differ**; VRAM per field and last VRAM bit-identical |
| i686 relink and boot trace (600 fields) vs base-boot | link exit 0, 23 stubs; every section identical except cpu from field 150 |
| Game, x64 dhd64.exe (`build_host64.py --wgpu`), `DHD_WGPU_BACKEND=vulkan` | menus 15 → 32 → 33; 360 pictures presented through the WebGPU surface; no error lines |
| Game, i686 dhd.exe, WebGPU on Vulkan, 66 s | to the race load (nav step 8, menu 9, field 1228); field 900 saved (1280×896, widescreen menu, correct) |

Notes from the runs:
- **x64 on DX12 never got past start-up in 58 s.** The exact backend's raster.wgsl compute
  pipelines take **80 s cold** through FXC (measured in the test exe). The native renderer's
  own objects take 0.4 s. This is the exact backend's pipeline build (W12), not the native
  renderer. Until it is faster, Vulkan or a cached build is needed on x64.
- **i686 DX12** builds no render pipeline (§10.5.4). There the native renderer reports
  "unavailable" and the exact renderer draws.
- No visible picture differences were noticed.

## 14. Round 4: the cold shader compile (W12)

**Cause.** FXC took 124 s on raster.wgsl's `main`, which made the first start unusable. DXC
compiles the same HLSL in 3.1 s. FXC inlines every function call, and two places multiplied
the code:
- **Texture sampling:** `sample_texture` called `sample_level` from 4 sites, and each call held
  4 `texel()` fetches (bilinear). That is 16 inlined copies of `texel` with `address()`'s
  per-format switch and the CLUT lookup. Stubbing the texture path alone brought FXC to 7.9 s.
- **The binary64 depth plane:** 2 × `sd_mul_int` + 2 × `sd_add` with three `sd_round` returns
  each, about 3 s.

`port/tools/shader_compile_time.py` measures this without a device.

**Fix (e96a5d1ea).** Results are unchanged, bit for bit:
- **Sampling:** one `texel()` inside a level loop (1 or 2 levels) and a tap loop (1 or 4 taps).
  Both counts are only known at run time, so FXC can't unroll them. The bilinear sum
  `((a(16-fx) + b fx)(16-fy) + (c(16-fx) + d fx) fy) >> 8` is exactly the sum of the four
  `texel × weight` products in integers. A point sample is its one texel with weight 256, and
  `>> 8` gives it back exactly because texels are 0..255.
- **FXC pitfall:** the per-level results must be two plain variables. A local `array` written
  with the loop index gave different VRAM on g+0 (FXC, or naga's HLSL for it, gets it wrong).
- **Depth plane:** `sd_add` calls `sd_round` once, and the two plane terms go through one loop
  whose count is `2u | batch.w`. `batch.w` is 0 (see `keep`), but the compiler can't know that.
  `sd_add` is commutative (on equal exponents the sign rule gives the same result), so `z + term`
  equals the oracle's `term + z`.

Compile times, all entry points, from naga's HLSL:

| | before | after |
|---|---|---|
| FXC, `main` | 124 s | 7.2 s |
| FXC, all | 125 s | 7.6 s |
| DXC, all | 3.6 s | 0.7 s |

**Checks.**
- g+0 `replay_gs.py --backend both`: bit-identical to d3d11 (`357EA3E7BF55F142` at field 1).
- `web_gs_replay.mjs` in headless Chrome gives the same VRAM hashes. The replay's
  `-vram.txt` is CRLF and the web one LF, so compare them ignoring CR.
- A trap while bisecting: `replay_gs.py --backend wgpu` writes `PREFIX-vram.txt`, while `both`
  writes `PREFIX-wgpu-vram.txt`. Reading the other name gives a stale file.

**Cold start, measured.**

| Where | Before | After |
|---|---|---|
| wgpu replay of g+0 on DX12, i686 (whole run, mostly the compile) | ~105 s | 7.0 s with FXC, 1.8 s with DXC |
| whole browser replay | 107 s | 11 s |

**DXC (888ddb0bd).** gpu_wgpu.c sets `dx12ShaderCompiler = Dxc` when `dxcompiler.dll` is next
to the executable, and falls back to FXC otherwise. `DHD_WGPU_COMPILER=fxc` forces FXC, and
`DHD_PROFILE` logs which compiler is used.
- wgpu-native v29 needs DXC 1.8.2502 or later, and only `dxcompiler.dll`.
- `dxcompiler.dll` is 19.5 MB for x86 and 22.3 MB for x64. It is under LLVM's NCSA licence
  plus MIT (`LICENSE-LLVM.txt`, `LICENCE-MIT.txt`), so it can be redistributed.
- `dxil.dll` is not needed, and it falls under Microsoft's licence instead.
- Shipping it saves about 6 s on a cold start, at about 20 MB. Since round 5 the packages ship
  it (§15).

**Not done.** Asynchronous pipeline builds on native: at 7 s with FXC, or under 1 s with DXC,
starting the game on another path while compiling isn't worth the complexity. wgpu has no DX12
pipeline cache.

**Minimised start (01f665d49).**
- The launcher passes its STARTUPINFO show state to the relaunched game process, which owns
  the window. `start_minimised()` (splash.c) reads it.
- On a minimised start there is no splash and no exclusive fullscreen.
- SDL creates the window `SDL_WINDOW_MINIMIZED` and shows it with
  `SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN=0`. When the window was minimised only through Win32,
  SDL restored it once it applied its pending window state.
- `gs_window_apply` doesn't re-show an iconic window.
- Checked with a `SW_SHOWMINNOACTIVE` start in `DHD_WINDOW=fullscreen` over 20 s: never shown,
  never in front.

## 15. Round 5: DXC in the packages, and WebGPU feature parity (W12)

**DXC in the packages (f3c15bf1a).**
- `package_release.py` puts the x86 DLL beside dhd.exe; `package_x64_test.py` puts the x64 DLL
  beside dhd64.exe. THIRD-PARTY-NOTICES.txt gains DXC (NCSA and MIT), wgpu-native (MIT) and
  webgpu.h (BSD-3-Clause).
- The refusals still apply to everything of ours. A third-party file must be the pinned bytes;
  the byte scans would only find coincidences in it: the words 0..14, which match a 65-byte run
  of the ELF's .data, and the text `\temp`.
  - `exe_problems` now takes the pinned libraries linked in. It reads their build roots from the
    library's own bytes (the first three folder levels) and zeroes those paths before the checks.
  - The ELF run check skips bytes found in such a library; wgpu-native holds the same 0..14.
  - The chance-bytes rule also covers a short match right after a non-ASCII byte: a C runtime
    double table spells `Y:/(A6>`.
  - dhd64.exe may import ntdll, bcryptprimitives and opengl32 (wgpu-native's).
- Checked: `package_x64_test.py` builds the folder from a fresh `--wgpu` dhd64.exe. The i686
  link's dhd.exe passes `exe_problems` and the ELF run check (0 problems; 336 before).

**Texture packs and the sharp font on WebGPU (a05e39fa9).**
- texpack.c and font_sharp.c no longer create D3D11 views. They pass pixels, with every mip
  level, to the backend's `ReplaceDevice` (`replace_texture.h`: create, retain, release) and
  keep the opaque view it returns.
- D3D11 makes the same immutable texture and SRV as before.
- WebGPU makes a texture and 8 bind groups, one per sampler. The 8 samplers cover U/V clamp or
  repeat and linear or nearest magnification, like D3D11's replacement samplers. Draws use flag
  2048, as on D3D11.
- The pack's memory budget comes from the exact backend's D3D11 device when there is one;
  otherwise it is 1 GB.

**Vsync for `DHD_INTERP` (e9312590a).** With `DHD_INTERP=1` and no `DHD_INTERP_HZ`, the surface is
made Fifo, as D3D11's `Present(1)`. The choice is made when the surface is created, so it holds for
a window that starts minimised. With `DHD_INTERP_HZ`, the surface keeps its mode and the timer paces
the pictures.

**The two-draw dest-alpha case (§13.2) cannot happen.** The two-draw path is taken only when the
blend uses As (SA/ISA factors). A GS blend has one C (As, Ad or FIX), so a draw blending on Ad never
splits, and a draw that splits never reads the destination alpha. The destination alpha test (DATE)
is ignored on both backends (`date_ignored`). So no draw reads an alpha that its own second draw has
not written yet, and nothing was changed.

**GPU timestamps: not done.**
- A picture spans several command encoders, so it needs wgpu-native's
  `TimestampQueryInsideEncoders` feature on the shared device, a query set, a resolve buffer and
  an asynchronous map.
- That is not quick, and it changes the device that every WebGPU run uses. `DHD_NATIVE_TRACE`
  still writes gpu_ms -1 on WebGPU; cpu_ms is measured.

| Check (x64 dhd64.exe, DX12 + DXC, minimised, `DHD_RENDER=native DHD_SCALE=3`, field 1800) | Result |
|---|---|
| PCSX2 pack (`DHD_TEXTURE_PACK`) | 37 of 37 textures drawn are replaced; the frame equals D3D11 with the pack within 1 per channel (mean 0.0) |
| `DHD_INTERP=1` | present mode 1 (Fifo; 3, Immediate, without it); frame saved and correct. A minimised window presents nothing, so the pacing was not measured |
| i686 boot trace, 600 fields | equals HEAD's (built without this round's renderer changes) except cpu from field 150, as in every round. Both differ from base-boot in `code` from field 1, from the 0.3.1 merge |

## See also

- 081 (portable core) — the phase plan this document fits into, phase 5.
- 082 (host64 runtime) — the 64-bit host the wgpu renderer will first run in.
- 083 (web core) — JSPI in Node, the prerequisite for async readback on the web.
