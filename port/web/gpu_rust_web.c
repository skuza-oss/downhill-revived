/* The GS on the Rust renderer in the browser (port/render, crate dhd_render, built for wasm32 by
   port/web/build_renderer.py), inside the web GS reactor (port/web/host_graphics.c): the browser's
   Rust provider of the exact front end port/gs/gpu_exact.inc, as port/gs/gpu_rust.c is natively.
   build_graphics.py links it into the reactor.

   Each device op is one dhd_render import (port/render/include/dhd_render.h). The page satisfies
   those imports with the Rust module's raw exports, so the calls go wasm to wasm with no JS frame.
   Both modules share one memory: the reactor's pointers (data, want, image) are Rust's too.

   The read is split (plan 1.4, finding 085 S4): dhdr_exact_read_begin submits and starts the map,
   then this reactor waits in its own JSPI Suspending import web_gpu_await() (JS gives it
   new WebAssembly.Suspending(() => R.dhdr_web_pending()), the map's promise), then
   dhdr_exact_read_finish copies the pages. Rust is never on the stack while the root is suspended,
   and no other dhdr_exact_* call is made between begin and finish (the front end is single
   threaded here and makes none).

   The device, the exact pipelines, the prewarm and the canvas surface are made by the async
   dhdr_web_init (a wasm-bindgen export) that JS awaits before this reactor runs; dhdr_device_init
   only reports that device here. The raster key has all three options on, so the
   pipelines dhdr_web_init built for the same key are reused.

   DHD_RENDER=native (worker.js's default; ?env.DHD_RENDER=exact keeps the exact picture for
   checks): port/web/gpu_rust_web_native.inc, the native renderer over the same Rust module, as
   port/gs/gpu_rust_native.inc is natively. It is made at the core's first gs_reset
   (web_gpu_reattach), when the core has read the player's settings (DHD_SCALE), not at
   web_gpu_enable, which runs before the core's main. The exact renderer then only answers the Z
   probe, inside the clip native_core.inc sets. */
#include "../gs/gs.h"
#include "../gs/gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wasi/libc-environ.h>
#include <time.h>
#include "dhd_render.h"   /* -Iport/render/include */
#include "../gs/gpu_exact.inc"

/* The reactor's own JSPI import: suspends until the read in flight has mapped. 1 = mapped, 0 = failed
   (never rejects; with nothing in flight it is 1 at once). */
__attribute__((import_module("env"), import_name("web_gpu_await"))) int32_t web_gpu_await(void);

/* PC SETTINGS' FPS & Ping (DHD_PERF_COUNTER): read at the first picture and when the page sets
   it (web_env_changed), not for every picture. The browser shows no ping yet. -1: not read. */
static int counter_on = -1;

static void counter_present(DhdrSurface *surface)
{
    struct timespec now;
    if (counter_on < 0) {
        const char *v = getenv("DHD_PERF_COUNTER");
        counter_on = v && atoi(v) != 0;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    dhdr_surface_counter(surface, counter_on, -1, now.tv_sec + now.tv_nsec * 1e-9);
    dhdr_surface_present(surface);
}

/* The raster key passed to dhdr_exact_create; worker.js passes the same one to dhdr_web_init. */
#define WEB_RASTER_FLAGS (DHDR_RASTER_EARLY_DEPTH | DHDR_RASTER_ADDRESS_SHIFT | DHDR_RASTER_EXCLUSIVE_WORD_STORE)

/* The provider's state. Its first member is the front end's (g->backend points at this). */
typedef struct Web {
    Exact e;
    DhdrExact *x;
    DhdrSurface *surface;   /* the canvas surface, taken once from dhdr_web_surface */
    int present_off;        /* a present failed (-1): no more presents through the surface */
    /* DHD_RENDER=native (gpu_rust_web_native.inc): this renderer then only answers the Z probe,
       and draws only inside clip when clip_on (nothing until a probe). native_core.inc reads and
       writes native, clip_on and clip on its NativeExact (this struct). */
    struct Native *native;
    int clip_on, clip[4];
} Web;

/* ---- the device ops (gpu_exact.inc's op_* seam): op_write, op_copy, op_batch, op_record,
   op_submit and op_clip are port/gs/gpu_rust_shared.inc's (through gpu_rust_web_native.inc) ---- */

/* begin (submit, start the map), suspend in web_gpu_await until it has mapped, finish (copy, unmap).
   A read that can't start or whose map failed (Rust has logged why) ends the run: the trap rejects
   the promising root's promise, as gpu_rust.c's op_read exits natively. */
static void op_read(Exact *gpu, int buffer, const uint32_t want[16], void *image)
{
    DhdrExact *x = ((Web *)gpu)->x;
    if (!dhdr_exact_read_begin(x, (uint32_t)buffer, want)) __builtin_trap();
    int mapped = web_gpu_await();
    if (!dhdr_exact_read_finish(x, (uint32_t)buffer, want, image) || !mapped) __builtin_trap();
}

/* The host hooks: the browser has no profile clock and no draw isolation. */
static uint64_t op_clock(void) { return 0; }
static int op_draw_skipped(const Gs *g) { (void)g; return 0; }

#include "gpu_rust_web_native.inc"  /* the shared device ops; native_init, native_reattach, native_show (DHD_RENDER=native) */

static Web *web;

/* The exact renderer on dhdr_web_init's device (0 when JS has not made it, or on failure); then the
   front end's hooks on gs, again after gs_reset, which clears the whole Gs and its VRAM. */
__attribute__((export_name("web_gpu_enable"))) int web_gpu_enable(void)
{
    if (!web) {
        DhdrDeviceDesc d;
        memset(&d, 0, sizeof d);
        d.size = sizeof d;
        d.abi = DHDR_ABI_VERSION;
        if (!dhdr_device_init(&d)) return 0;
        Web *p = calloc(1, sizeof *p);
        if (!p) return 0;
        if (!exact_alloc(&p->e) || !(p->x = dhdr_exact_create(WEB_RASTER_FLAGS))) {
            exact_free(&p->e);
            free(p);
            return 0;
        }
        web = p;
    }
    exact_attach(&gs, &web->e);
    return 1;
}

/* After every gs_reset (build_graphics.py's host_service_gs_reset wrapper): the hooks again. The
   first one is the core's kernel_init, after its settings load: DHD_RENDER=native makes the
   native renderer then, with the environment as the core has it now (worker.js's WASI environment
   is live, and libc reads it again here). */
void web_gpu_reattach(void)
{
    static int native_tried;
    if (!web) return;
    exact_attach(&gs, &web->e);
    if (web->native) { native_reattach(&gs, web); return; }
    if (native_tried) return;
    native_tried = 1;
    __wasilibc_deinitialize_environ();
    const char *render = getenv("DHD_RENDER");
    if (render && !strcmp(render, "native") && !native_init(&gs, web))
        fprintf(stderr, "gs: native renderer unavailable; using the exact renderer\n");
}

uint64_t web_gs_mmio(unsigned addr);  /* host_graphics.c */

/* The page changed a variable the native renderer follows while the game runs (PC SETTINGS >
   Resolution, Scaling: DHD_SCALE, DHD_PRESENT_FILTER, DHD_ASPECT; worker.js, or graphics.js in the
   threaded build, calls this between the reactor's calls). The next web_display reads the
   environment again and applies them. */
static int env_changed;
__attribute__((export_name("web_env_changed"))) void web_env_changed(void) { env_changed = 1; }

/* The scanout into the display texture, submitted. Returns
   width << 16 | height, or 0 when nothing is displayed. */
__attribute__((export_name("web_display"))) uint32_t web_display(void)
{
    Gs *g = &gs;
    unsigned width, height;
    /* present.c's gs_vsync: plain MMIO pages are the authoritative privileged register storage
       (host_graphics.c's web_gs_mmio: the threaded build's copy from when the vsync was queued). */
    for (unsigned addr = 0; addr <= 0xE0; addr += 16)
        gs_priv_write(g, addr, web_gs_mmio(addr));
    if (!web) return 0;
    int changed = env_changed;
    env_changed = 0;
    if (changed) __wasilibc_deinitialize_environ(), counter_on = -1;
    if (web->native) {
        /* the present options before this picture, a new scale after it (native_rescale) */
        if (changed) web_native_present_options(g);
        /* the field is drawn now (NATIVE_INLINE) and shown by the web_present that follows */
        width = 640; height = 448;
        web_native_movie(g, NULL, 0, 0);
        web_native_vsync(g, &width, &height, 0);
        if (changed) native_rescale(web->native);
        return web->native->present_pending ? width << 16 | height : 0;
    }
    if (!exact_display(g, &width, &height)) return 0;
    return width << 16 | height;
}

/* The last scanout onto the canvas surface, configured to canvas_w x canvas_h.
   Called from JS's vsync after web_display, before the pacing yield; it never suspends. Returns
   dhdr_present_exact's 1 (drawn and presented) or 0 (nothing to present: no surface, no picture
   yet, skipped, or presentation stopped after a failure). */
__attribute__((export_name("web_present"))) int web_present(uint32_t canvas_w, uint32_t canvas_h)
{
    Web *p = web;
    if (!p || p->present_off || !canvas_w || !canvas_h) return 0;
    if (!p->surface && !(p->surface = dhdr_web_surface())) return 0;
    if (p->native) return native_show(p->native, p->surface, canvas_w, canvas_h);
    submit_all(&p->e); /* the scanout, before the picture is shown */
    if (!p->e.display_valid) return 0;
    int r = dhdr_present_exact(p->surface, p->x, NULL, 0, 0, canvas_w, canvas_h);
    if (r < 0) {
        p->present_off = 1;
        dhdr_surface_destroy(p->surface);
        p->surface = NULL;
        return 0;
    }
    if (r == 1) counter_present(p->surface);
    return r;
}

/* Draws (the native renderer's when it runs), dispatches, readbacks and submits so far (the page's
   status line); 4: the native renderer's resolution scale (0 in exact mode). */
__attribute__((export_name("web_gpu_stats"))) uint32_t web_gpu_stats(int which)
{
    if (!web) return 0;
    if (which == 4) return web->native ? web->native->scale : 0;
    if (which == 0 && web->native) return (uint32_t)web->native->stats.draws;
    const Exact *e = &web->e;
    return (uint32_t)(which == 0 ? e->draws : which == 1 ? e->dispatches : which == 2 ? e->reads : e->submits);
}
