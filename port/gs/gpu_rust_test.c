/* The test build's hooks of the Rust renderer (DHD_GS_TEST_BUILD; task HK):
   gs_wgpu_depth_probe, gs_wgpu_adapter, gs_wgpu_presented and gs_wgpu_present_again, over dhd_render's dhdr_test_*
   exports (cargo feature test-hooks), for test_depth.c, test_present.c and test_native_transfers.c.

   Not a translation unit of its own: gpu_rust.c includes it at its end, in the test build only
   (#ifdef DHD_GS_TEST_BUILD / #include "gpu_rust_test.c" / #endif), so the probe starts the
   device through gpu_rust.c's static device_init, with the same DHD_WGPU_* options. Listing this
   file as a source of its own fails to link (device_init is never defined there). */
#ifdef DHD_GS_TEST_BUILD
#include <stdint.h>
#include "dhd_render.h"

static int device_init(void);

/* port/gs/test_depth.c: raster.wgsl's depth_probe over cases (8 + 4 + 8 words each, see
   DepthCase), results as (exact, double-float) Z pairs. 1 ok; 0 no device or the probe failed
   (dhd_render logged why). */
int gs_wgpu_depth_probe(const void *cases, unsigned count, uint32_t *results)
{
    if (!device_init()) return 0;
    return dhdr_test_depth_probe(cases, count, results);
}

/* The device's adapter name ("" before the device exists). */
const char *gs_wgpu_adapter(void) { return dhdr_adapter_name(); }

/* test_present.c, test_native_transfers.c: the last presented surface texture, read back before it
   was presented (width x height RGBA8), or NULL when none was. */
const uint32_t *gs_wgpu_presented(unsigned *width, unsigned *height)
{
    uint32_t w = 0, h = 0;
    const uint32_t *pixels = dhdr_test_presented(&w, &h);
    *width = w; *height = h;
    return pixels;
}

/* test_native_transfers.c: the last scanned-out picture presented again (native_present), as a
   repeated refresh presents it (DHD_INTERP_MODE=once, the browser's frames between two fields). */
void gs_wgpu_present_again(Gs *g)
{
    Native *n = ((NativeExact *)g->backend)->native;
    native_mutex_lock(&n->context_lock);
    native_present(n);
    native_mutex_unlock(&n->context_lock);
}
#endif
