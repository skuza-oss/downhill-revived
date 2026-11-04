# Downhill Revived Vulkan framebuffer cache

This is the crates.io `wgpu-hal` **29.0.3** source, under its original MIT OR
Apache-2.0 licence (LICENSE.MIT and LICENSE.APACHE). Other backends are unchanged.
Local changes are marked `Downhill Revived` in the five Vulkan source files.

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
