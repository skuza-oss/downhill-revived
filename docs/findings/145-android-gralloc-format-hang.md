# 145: Android black screen on an Adreno 710 (the gralloc lock in the surface format query)

Status: current
Checked: 2026-11-04

## The report

A player's Redmi Note 13 Pro 5G (Snapdragon 7s Gen 2, SM7435, Adreno 710, Qualcomm Vulkan driver
0615.73 from July 2023, Android 14, HyperOS) showed only a black screen after the disc copy, with
0.7.9's APK. Nothing ever reached the screen.

## Diagnostic builds

Three one-off APKs, never on main, carried the diagnostics (a Share log screen at the next start, a
UI-thread watchdog, Android's exit reasons, and at a freeze every thread's native stack: each thread
unwinds itself in a signal handler, frames as library+offset for llvm-addr2line). The player's logs:

- The renderer starts: Vulkan adapter, pipelines, the surface's first capabilities query (present
  modes Mailbox and Fifo). The game draws its first loading card (field 10), then the freeze
  watch reports `no field for 5 s at field 34`, with Mailbox and with Fifo alike.
- The game thread spins waiting for the render thread. The render thread waits on a
  priority-inheritance mutex (`rt_mutex_wait_proxy_lock`) in
  `libgralloccore.so gralloc::BufferManager::AllocateBuffer`, called by the QTI mapper's
  `isSupported`, `AHardwareBuffer_isSupported` and `libvulkan.so`, from wgpu-hal's
  `NativeSurface::surface_capabilities` in wgpu-core's `configure_surface`: the surface's second
  format query (the first, at startup, returned). No other thread was in gralloc.
- The Adreno driver's `VKDBGUTILWARN003` lines are a performance hint (render pass not merged into
  multipass), not related.

## Cause

Android's Vulkan loader answers vkGetPhysicalDeviceSurfaceFormatsKHR with R8G8B8A8_UNORM and _SRGB
always, then asks gralloc (AHardwareBuffer_isSupported) about optional formats (RGB565, FP16,
RGB10A2 and newer ones). On this gralloc that query leaves the BufferManager lock held; the next
one waits forever. The lock holder is an inference (Qualcomm's gralloc isn't public), but it
matches the stacks: one finished query, the next one stuck, no thread holding the lock.

The same hang was reported by another recompilation project on a Xiaomi 12T Pro (Adreno 730,
Android 15): the render thread blocked in the first vkCreateSwapchainKHR on gralloc's
BufferManager lock after the loader's format query; their workaround picks RGBA8 without the query
([automobililamborghini-recomp PR #365](https://github.com/alondero/automobililamborghini-recomp/pull/365)).

## Fix

The vendored wgpu-hal (`port/render/vendor/wgpu-hal`, PATCH.md) doesn't call
vkGetPhysicalDeviceSurfaceFormatsKHR on Android: the surface's formats are the loader's two fixed
ones, R8G8B8A8_UNORM and R8G8B8A8_SRGB in sRGB non-linear. The renderer draws RGBA8Unorm on Android
already (`surface_format`: RGBA8Unorm first), so the picture is unchanged on phones that worked.
