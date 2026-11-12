# Downhill Revived Vulkan framebuffer cache

This is the crates.io `wgpu-hal` **29.0.3** source, under its original MIT OR
Apache-2.0 licence (LICENSE.MIT and LICENSE.APACHE). Local changes are marked
`Downhill Revived`: the Vulkan backend's (below) and one DX12 backport (the last section).

The device shares at most 256 framebuffer lookup entries across command encoders.
Keys retain upstream's render pass, extent, raw attachments and unique view identities.
Lookup, creation, arbitrary eviction and attachment invalidation share one mutex.
Before destroying an image view, its entries are removed; encoder-owned temporary
views follow the same rule. The monotonic identities additionally prevent handle ABA.

Eviction cannot immediately destroy a framebuffer that recorded commands still
reference: each encoder pins its distinct used framebuffers with `Arc` until its
command pool is reset or destroyed. HAL's existing contract requires completion
of all that encoder's submissions first. Pins are released after the reset/pool
destruction. Cache capacity excludes these necessary in-flight lifetime pins;
their count is bounded by the distinct framebuffers in outstanding commands.
No GPU wait or new cross-thread command-pool access is added.

The existing HAL resource-use contract still applies: an attachment view must
remain alive throughout render-pass recording. wgpu-core's owned resource
references/snatch guards enforce that contract. The cache mutex serializes its
own operations; it does not permit concurrent destruction of a borrowed view.

Invalidation removes lookup entries immediately, even if encoder pins still exist.
The existing HAL view-destruction contract forbids outstanding GPU use of the view;
commands using a destroyed view cannot be submitted again. Retained framebuffer
objects do not keep views/textures alive and are never used as lookup entries.

Framebuffer destructors hold only an ash dispatch-table clone, avoiding a cycle
with DeviceShared. Encoders retain DeviceShared and clear their pins in Drop.
DeviceShared explicitly clears the cache before destroying render passes/device.

Research (2026-10-28): current upstream trunk `command.rs` still drains the cache
in reset_all; neither dynamic rendering nor imageless framebuffer creation is
implemented there. Its module-level device-cache/imageless description is stale.
This patch is a local implementation, not an upstream backport.

- https://github.com/gfx-rs/wgpu/blob/trunk/wgpu-hal/src/vulkan/command.rs
- https://github.com/gfx-rs/wgpu/issues/8032 (discussion of framebuffer retention)
- https://github.com/gfx-rs/wgpu/issues/9754 (temporary view lifetime discussion)
- https://github.com/gfx-rs/wgpu/pull/7847 (historical framebuffer lifetime change)

The parent Cargo.toml patches only this crate; the lock removes only its registry
source/checksum. rustgs includes all vendor files in its object-cache key.

## Android: no surface format query (finding 145)

`swapchain/native.rs`: on Android the surface's formats are R8G8B8A8_UNORM and R8G8B8A8_SRGB
(sRGB non-linear), the two the platform's loader always lists first without asking gralloc;
vkGetPhysicalDeviceSurfaceFormatsKHR isn't called. The loader's query asks gralloc
(AHardwareBuffer_isSupported) about its optional formats, and Qualcomm's gralloc on an Adreno 710
phone (driver 0615.73, Android 14) then left its BufferManager lock held: the next query hung the
render thread for good.

## Swapchain image views kept across frames (finding 149)

`create_texture_view` on a swapchain image (`Texture::surface_image`, set by
`swapchain/native.rs`'s acquire) returns the view made for that image and description before
(`DeviceShared::surface_views`), with its view identity, so the present pass's framebuffer key
repeats and the framebuffer cache above finds it. Before, every acquire made a new view, so every
frame created a framebuffer: on the throttled Mi 11i's Adreno each `vkCreateFramebuffer` allocates
GPU memory (`kgsl_ioctl_gpuobj_alloc`, whose address search walks the process's allocations), 16 %
of the render thread. `destroy_texture_view` leaves such a view alone (`surface_cached`); the
swapchain's `release_resources` destroys its images' kept views and their framebuffer entries after
its device wait, and `DeviceShared`'s drop destroys any left.

## Barriers: one per stage pair, textures without the vertex stage (finding 174)

Two changes to how textures and buffers are synchronised, both in the Vulkan backend:

- `command.rs` `transition_buffers` and `transition_textures` issue one `vkCmdPipelineBarrier` per
  distinct (source, destination) stage pair of their barriers, each with only its own barriers,
  instead of one with every barrier's stages merged. Each resource keeps the synchronisation it had
  (its stages, accesses and layout transition); only the dependencies between unrelated resources
  that the merge added are gone (what `vkCmdPipelineBarrier2`'s per-barrier stages give). A batch
  whose barriers share one stage pair is one call, as before.
- `conv.rs` `map_texture_usage_to_barrier`: sampled and storage textures synchronise with the
  fragment and compute shader stages, not the vertex shader stage. No vertex shader of the game
  reads a texture; `src/shaders.rs`'s `no_vertex_texture_reads` test checks every WGSL module the
  renderer builds and fails if one ever does.

Why: on a Mali job-manager GPU a barrier whose source stages include fragment work and whose
destination stages include the vertex stage makes all later vertex and binning work wait for all
earlier fragment work, and image copies (texture uploads) run as fragment jobs there. Every picture
uploads new decoded textures, whose barrier to "sampled" named the vertex stage, so the picture's
binning and compute work waited behind the last picture's fragment work. On the Redmi 9's Mali-G52
(driver r26p0) at 2x the two changes together took finding 156's route from 48.25-48.27 to
49.09-49.96 fields/s (the race start 34.0-34.4 -> 36.4-36.5); the per-pair barriers alone changed
nothing measurable.

## A pipeline the driver does not create is that pipeline's error (finding 176)

`device.rs` `create_render_pipeline` and `create_compute_pipeline`: a VK_ERROR_OUT_OF_HOST_MEMORY
or VK_ERROR_OUT_OF_DEVICE_MEMORY from vkCreateGraphicsPipelines / vkCreateComputePipelines is a
`PipelineError::Linkage` (`mod.rs` `map_pipeline_creation_err`; wgpu-core makes it an internal
error of that pipeline), not a `DeviceError::OutOfMemory`, which wgpu-core answers by losing the
device. The Vulkan spec leaves the device usable after a failed pipeline creation, and PowerVR
Rogue's driver 1.13 returns VK_ERROR_OUT_OF_HOST_MEMORY for every shader its compilers refuse. The
temporary shader modules are destroyed on that path too (upstream returns before destroying them).
Other results keep upstream's mapping.

## Timeline submit info: a value for every semaphore (finding 201)

`semaphore_list.rs` `add_to_submit`: when a submit chains a `VkTimelineSemaphoreSubmitInfo` (the
device has timeline semaphores, so every submit signals the fence's timeline semaphore), both its
lists carry one value per semaphore, binary ones included; upstream left a list without a timeline
semaphore at count 0 and a null pointer. The spec allows that, but PowerVR's proprietary driver
(Rogue GE8322, driver 1.18@6267915, issue #82; B-Series in gfx-rs/wgpu PR #10677 and #7669) reads
`pWaitSemaphoreValues[i]` for every wait semaphore whenever the struct is chained, so the first
submit that waits on a binary semaphore (the swapchain's acquire or the relay semaphore) read
through null in `vkQueueSubmit`. Vulkan ignores a binary semaphore's value; a binary wait's padding
is 0 (`dummy_value`), so a driver that took it for a timeline wait would find it already reached.
A signal's padding stays upstream's `!0`. Devices without timeline semaphores chain nothing, as
before.

`mod.rs` re-exports `SemaphoreList`, `SemaphoreListMode` and `SemaphoreType` (doc-hidden) for the
renderer crate's test `tests/semaphore_values.rs`; `adapter.rs` `Adapter::timeline_semaphores` says
which kind the device's fences are, for dhd.log (`src/device.rs` `log_vulkan`).

## DX12: the shader cache keeps bytecode, not FXC's blobs (finding 223)

`dx12/mod.rs` `CompiledShader` is the shader's bytecode in an `Arc<[u8]>`; `shader_compilation.rs`
copies FXC's `ID3DBlob` and DXC's `IDxcBlob` output into it and releases the blob on the compiling
thread; `device.rs` takes DXIL passthrough bytes the same way. Upstream kept the blob itself in the
device's shader cache and handed each pipeline a clone. d3dcompiler_47's `ID3DBlob` counts
references without atomics, so a cache hit's AddRef (under the cache's lock) racing another
thread's Release (outside it) lost updates: a blob the cache still held was freed, and the next
hit's AddRef read a freed object's vtable. The renderer's background pipeline worker and its render
thread make pipelines from the same vertex shader at once (finding 131). This is upstream's fix
for gfx-rs/wgpu#10547, PR #10805 (merged into trunk 2026-11-11, in no release yet), applied to
29.0.3 unchanged in substance: three hunks. `tests/dx12_shader_cache.rs` is the stress test.
