/* Native renderer (DHD_RENDER=native) on the Rust renderer in the browser: the backend half, as
   port/gs/gpu_rust_native.inc is natively (task W1, finding 086). The shared half is
   port/gs/native_core.inc, built with NATIVE_INLINE (port/gs/native_thread.h: one thread, no
   render thread; each field is drawn at its own vsync); the GPU work is the Rust module's
   native renderer behind the same dhdr_native_* / dhdr_texture_* calls, wasm to wasm. Included by
   port/web/gpu_rust_web.c, whose exact renderer (Web) then only answers the Z probe. What both
   sides have alike (the device ops, textures, surfaces, the picture, the present options,
   creation's common half) is port/gs/gpu_rust_shared.inc's.

   What differs from the x64 glue, and why:
   - the window is the page's OffscreenCanvas: its size comes with each web_present (worker.js
     passes canvas.width x canvas.height); DHD_SCALE=window uses the last size seen (1280 x 960, the
     page's canvas, before the first present);
   - the picture is drawn at web_display (gs_native_vsync, inline) and shown by the web_present
     that follows it: native_present only marks it, so the present has the canvas's size, and the
     JS order of the exact path (web_display, then web_present, then the pacing yield) holds;
   - pacing is the page's (worker.js host_service_gs_vsync), so pacing_handed / pacing_present
     are empty here, and there is no frame-latency wait;
   - no texture packs in the browser (texpack_* are empty; n->texpack stays 0). The sharp font is
     the same font_sharp.c (build_graphics.py builds it into the reactor);
   - widescreen: port/hle/widescreen.c runs in the core; the reactor reads the shape it publishes
     (DHD_WIDESCREEN_ASPECT, "N:1000") and the menu state from guest RAM with widescreen.c's own
     rule (port/hle/widescreen_flat.h);
   - the display read (screenshots only; the page never asks for one) suspends in web_gpu_await
     between dhdr_native_read_begin and _finish (native_read_wait), as the exact read does;
   - the scale limit (native_scale_hold) has the device's largest texture but no memory size;
   - PC SETTINGS > Resolution and Scaling apply while the game runs (native_rescale, web_display's
     web_env_changed): x64 changes the scale at the next start. */

#define NATIVE_INLINE 1

#include "../hle/widescreen_flat.h"

typedef Web NativeExact;

#define NATIVE_GPU_SIDE \
    int present_pending;               /* a picture was drawn; web_present shows it */ \
    float present_aspect;              /* the shape to show it in (a movie's 4:3) */

/* The browser has one view. */
volatile int gs_native_view;

/* Pacing is worker.js's: every field is drawn at its vsync and shown at once. */
void pacing_handed(double *due, double *began) { *due = 0; *began = 0; }
void pacing_present(double due, double began) { (void)due; (void)began; }

/* No texture packs in the browser. */
typedef struct TexPackEntry TexPackEntry;
TexPackEntry *texpack_find(const Gs *g, unsigned ctxt) { (void)g; (void)ctxt; return NULL; }
void *texpack_view(TexPackEntry *e, uint64_t field) { (void)e; (void)field; return NULL; }
void texpack_field(uint64_t field) { (void)field; }
void texpack_report(void) {}

/* widescreen.c's two answers, from what the core publishes (see the header). */
double host_widescreen_aspect(void)
{
    const char *v = getenv("DHD_WIDESCREEN_ASPECT");
    unsigned a = 0, b = 0;
    return v && sscanf(v, "%u:%u", &a, &b) == 2 && a && b ? (double)a / b : 0;
}
int host_widescreen_flat_limit(void) { return widescreen_flat_limit(); }

#define NATIVE_WINDOW "canvas"
#define gs_gpu_native web_native
#define gs_native_vsync web_native_vsync
#define gs_native_bench web_native_bench
#define gs_native_lock web_native_lock
#define gs_native_present_options web_native_present_options
#define gs_native_picture web_native_picture
#define gs_native_movie web_native_movie
#include "../gs/gpu_rust_shared.inc"   /* native_core.inc, native_snapshot.inc, the shared glue */

/* The canvas's size at the last web_present (the page's canvas before the first). */
static unsigned canvas_width = 1280, canvas_height = 960;

/* ---- textures and the picture ---- */

/* No texture packs: the sharp font only. */
static NativeView native_replacement_view(Native *n, NativeTexture *t)
{
    (void)n;
    return t->sharp;
}

/* DHD_SCALE=window's height: the canvas's. */
static unsigned native_window_height(Native *n)
{
    (void)n;
    return canvas_height;
}

/* PC SETTINGS > Resolution while the game runs (DHD_SCALE changed; web_display calls this after a
   picture, before the next field records anything). x64 makes the new scale at the next start;
   here the scaled surfaces (frame and Z buffers, native_surface_resources' rule) start over: Rust
   drops each texture (dhdr_native_surface with the other kind starts a surface over; the next
   picture asks for the right kind again, at the new scale, as it asks for every surface) and
   the pages they owned come from GS memory again at the next claim, as for a new surface. What
   only the GPU had drawn there is lost: the game draws its frame again every field, so at most
   one picture shows the CPU's copy. */
static void native_rescale(Native *n)
{
    const char *scale = getenv("DHD_SCALE");
    unsigned setting = 2;
    if (scale && (!native_stricmp(scale, "window") || !native_stricmp(scale, "match"))) setting = 0;
    else if (scale && atoi(scale) >= 1) setting = (unsigned)atoi(scale);
    if (setting > n->scale_limit) setting = n->scale_limit;   /* native_scale_hold's */
    if (setting == n->scale_setting) return;
    unsigned count = (unsigned)n->surface_count;
    uint8_t dropped[NATIVE_SURFACES] = {0};
    for (unsigned i = 0; i < count; i++) {
        NativeSurface *s = &n->surfaces[i];
        if (s->bw < NATIVE_SCALED_BW) continue;   /* 1x at every scale */
        if (s->texture) {
            uint32_t size[2];
            dhdr_native_surface(n->r, i, 1, 64, 64, !s->depth, size);
        }
        s->scale = 0; s->size[0] = s->size[1] = 0;
        s->texture = s->srv = 0;
        dropped[i] = 1;
    }
    for (unsigned page = 0; page < 512; page++) {
        if (n->owner[page] >= 0 && dropped[n->owner[page]]) n->owner[page] = -1;
        if (n->top_owner[page] >= 0 && dropped[n->top_owner[page]]) n->top_owner[page] = -1;
    }
    n->scale_setting = setting;
    n->scale = 0;   /* native_picture: the setting, or the canvas's match */
    if (setting) fprintf(stderr, "gs native: resolution scale now %u (DHD_SCALE)\n", setting);
    else fprintf(stderr, "gs native: resolution scale now matching the canvas (DHD_SCALE)\n");
}

/* Drawn: web_present shows it (native_show), in the shape it has now. */
static void native_present(Native *n)
{
    if (n->present_failed) return;
    n->present_pending = 1;
    n->present_aspect = n->aspect;
}

/* The picture native_present marked onto the canvas surface, configured to w x h: as
   gpu_rust_native.inc's native_present (the rectangle that keeps its shape, then the present).
   1 drawn and presented, 0 not. */
static int native_show(Native *n, DhdrSurface *surface, unsigned w, unsigned h)
{
    if (!n->present_pending || n->present_failed || !surface || !w || !h) return 0;
    n->present_pending = 0;
    int resize = w != canvas_width || h != canvas_height;
    canvas_width = w; canvas_height = h;
    float aspect = n->aspect;
    n->aspect = n->present_aspect;
    int drawn = native_present_draw(n, surface, w, h, resize);
    n->aspect = aspect;
    if (drawn <= 0) return 0;
    counter_present(surface);
    native_note_presented(n);
    return 1;
}

/* The display read (native_read_display) suspends here between begin and finish, so only from the
   core's calls (a promising root). */
static int native_read_wait(void) { return web_gpu_await(); }

/* A read of GS memory (gs_read_image's pages). The browser's reads are never deferred
   (port/hle/graphics.c defers only behind a memory fault guard, which wasm has not), so the Z
   probe's bookkeeping that natively runs as the read is deferred (native_stage_pages: the watch
   rectangle, the replay of the logged Z writes outside it, the band) runs here, then the exact
   renderer's sync of the range reads what it drew. native_stage_pages also stages the dirty pages
   for a fetch that never comes: a copy of the probe's page or two, never read. gs.c's other
   callers (CLUT loads: 1024 or 4 bytes, not whole pages) only sync. */
static void native_sync_range(Gs *g, unsigned address, unsigned bytes)
{
    uint32_t want[16] = {0}, staged[16];
    int read = (g->reg[GS_TRXDIR] & 3) == 1 && !g->transfer_count && !g->transfer_have;
    if (read && bytes && address % 8192 == 0 && bytes % 8192 == 0) {
        unsigned last = (address + bytes - 1) / 8192;
        for (unsigned page = address / 8192; page <= last && page < 512; page++) want[page / 32] |= 1u << (page & 31);
        native_stage_pages(g, want, staged);
    }
    sync_range(g, address, bytes);
}

/* ---- creation (from web_gpu_reattach, after the core's first gs_reset) ---- */

/* 1: the native renderer is installed on g; 0: unavailable, g keeps the exact renderer. */
static int native_init(Gs *g, Web *exact)
{
    double start;
    Native *n = native_create_rust(exact, 0, &start);   /* memory unknown; the sharp font's replacements (no texture packs) */
    if (!n) return 0;
    fprintf(stderr, "gs native: WebGPU shaders and pipelines built in %.2f s\n", host_wall_seconds() - start);
    native_install(g, exact, n, "WebGPU (browser)");
    g->sync_range = native_sync_range;
    if (n->wide) fprintf(stderr, "gs native: widescreen pictures %.3f:1 (DHD_WIDESCREEN_ASPECT)\n", n->wide);
    fprintf(stderr, "gs native: GS blending %s\n", n->dual ? "with dual-source blending" :
            "without dual-source blending (a draw blending on As writes its alpha in a second draw)");
    return 1;
}

/* The native GS hooks again after a later gs_reset (which clears the whole Gs): native_install's,
   on top of exact_attach's. */
static void native_reattach(Gs *g, Web *exact)
{
    exact->clip_on = 1;
    g->draw = native_draw;
    g->local_copy = native_local_copy;
    g->flush_draw = native_flush_draw;
    g->write_high = native_write_high;
    g->stage_pages = native_stage_pages;
    g->sync_range = native_sync_range;
}
