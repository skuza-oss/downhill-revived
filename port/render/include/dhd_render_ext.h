/* dhd_render.h's additions after the v1 freeze: exports the Rust renderer (crate dhd_render) has
   beyond the frozen v1 header. Each is optional for C, adds no type and
   changes no v1 struct; include it after dhd_render.h. port/web/build_graphics.py reads the
   DHDR_FN declarations of both headers.

   SP10 (docs/findings/090-speed.md): an indexed native picture. Finding 109: the device's largest
   texture. Finding 129: the perf: lines' GPU counters. Finding 171: a lost device. */
#ifndef DHD_RENDER_EXT_H
#define DHD_RENDER_EXT_H

#include "dhd_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/* dhdr_native_picture for an indexed picture: each DRAW command's first and count are a range of
   indices[0, index_count) (in place of a range of vertices), and each index names one of the
   picture's vertex_count vertices. The triangles drawn, their order and their vertices' bytes are
   those of the expanded picture (vertices[indices[i]] for every i of the range), so the picture
   is the same. indices stays alive for the call. Returns as dhdr_native_picture. */
DHDR_FN(dhdr_native_picture_indexed) int dhdr_native_picture_indexed(DhdrNative *n, const DhdrPicture *p,
                                                                     const uint32_t *indices, uint32_t index_count,
                                                                     DhdrPictureResult *out);

/* The device's largest 2D texture side in texels (its maxTextureDimension2D; the device is made
   with the adapter's own), 0 before init. The native renderer's scale limit
   (port/gs/gpu_rust_shared.inc, native_scale_hold). */
DHDR_FN(dhdr_device_max_texture) uint32_t dhdr_device_max_texture(void);

/* port/gs/perf_log.c's perf: lines (finding 129): up to count values into out: [0] the GPU timer (0
   off, 1 on, 2 the adapter has no timestamp queries), [1] frames timed so far (the native renderer
   times one frame in 8), [2] their GPU nanoseconds (the sum of their passes), [3] their spans'
   nanoseconds (first pass's start to last pass's end), [4] the last surface's present mode (0 none,
   1 immediate, 2 mailbox, 3 fifo, 4 fifo-relaxed), [5] the adapter's kind (0 other, 1 integrated,
   2 discrete, 3 virtual, 4 cpu). Returns how many it wrote. Any thread. */
DHDR_FN(dhdr_perf_read) uint32_t dhdr_perf_read(uint64_t *out, uint32_t count);

/* Finding 171: 1 when the device was lost (the driver reset or removed it: every later call draws
   nothing), else 0. The first 1 logs why (on D3D12 the removed reason, and DRED's breadcrumbs with
   DHD_GPU_DRED=1). C asks after a present failed and then ends the game with a message. Any
   thread; the browser's device always answers 0 (it has its own lost handling). */
DHDR_FN(dhdr_device_lost) int dhdr_device_lost(void);

#ifdef __cplusplus
}
#endif

#endif
