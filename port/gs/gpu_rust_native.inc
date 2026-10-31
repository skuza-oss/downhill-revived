/* Native renderer (DHD_RENDER=native) on the Rust renderer (port/render, dhd_render.lib): the
   backend half. The shared half is native_core.inc;
   the GPU work (pipelines, arenas, surfaces, textures, conversions, the command loop, scanout and
   the present pass) is Rust's, behind the dhdr_native_* calls of port/render/include/dhd_render.h.
   Included by gpu_rust.c, whose Rust is the exact renderer answering the Z probe.

   What the browser's glue does alike (the device ops, textures, surfaces, the picture as
   native_snapshot.inc's DhdrPicture, the present options, the scale limit and DHD_SCALE=window's
   match, the display read, creation's common half) is gpu_rust_shared.inc's. What stays here, x64's own:
   - the texture packs: a decoded texture's replacement is its pack's (texpack_view), else its
     sharp font; their memory budget is half the device's dedicated memory (DXGI's
     DedicatedVideoMemory, gpu_rust.c's device.memory), or 1 GB when it is unknown; the scale
     limit counts the same memory (native_scale_hold), and DHD_SCALE_MAX publishes it;
   - the window: its size (gpu_rust.c's window_size: the GsWindow's own function) for
     DHD_SCALE=window (native_window_height) and for each present, pacing (pacing_present,
     not under DHD_INTERP), dhdr_surface_present and the present counts.

   gpu_rust.c's contract with this file (its header): NativeExact is Rust (members x, native,
   clip_on, clip[4], window, surface), native_core.inc's gs_gpu_native and gs_native_* hooks are
   gpu.h's as they are, and this file defines native_init and native_waits_for_vsync. The presented
   test hook (DHD_GS_TEST_BUILD) runs inside dhdr_surface_present. */

typedef Rust NativeExact;

#define NATIVE_WINDOW "window"
#include "gpu_rust_shared.inc"   /* native_core.inc, native_snapshot.inc, the shared glue */
#ifdef DHD_GS_TEST_BUILD   /* the GS test programs (replay.c, test_native_*.c) link without present.c */
static void gs_picture_rect(int left, int top, int right, int bottom, int w, int h)
{
    (void)left; (void)top; (void)right; (void)bottom; (void)w; (void)h;
}
#else
void gs_picture_rect(int left, int top, int right, int bottom, int w, int h);  /* present.c: for the mouse */
#endif

static NativeView native_replacement_view(Native *n, NativeTexture *t)
{
    return t->pack ? texpack_view(t->pack, n->field) : t->sharp;
}

/* ---- render thread (context lock held) ---- */

/* DHD_SCALE=window's height: the window's. */
static unsigned native_window_height(Native *n)
{
    unsigned width, height;
    window_size(&n->exact->window, &width, &height);
    return height;
}

/* DHD_INTERP shows one picture per display refresh: the surface waits for vsync (Fifo; gpu_rust.c
   asks for DHDR_PRESENT_VSYNC). With DHD_INTERP_HZ its own timer paces the pictures. */
static int native_waits_for_vsync(Native *n)
{
    return n->interp && !n->interp_hz;
}

/* The displayed picture into the window's surface (gpu_rust.c's, made by rust_surface on
   present.c's window), in the rectangle that keeps its shape. */
static void native_present(Native *n)
{
    Rust *p = n->exact;
    if (!p->surface || !p->window.size || n->present_failed) return;
    unsigned w, h;
    window_size(&p->window, &w, &h);
    if (!w || !h) return;  /* minimised */
    static int spikes = -1;
    if (spikes < 0) spikes = getenv("DHD_SPIKES") != NULL;
    double start = spikes ? host_wall_seconds() : 0;
    double rs = native_rstat_start();
    int drawn = native_present_draw(n, p->surface, w, h, n->resize != 0);
    native_rstat_add(RSTAT_PRESENT, rs);
    double ready = spikes ? host_wall_seconds() : 0;
    if (drawn || n->display_width) n->resize = 0;   /* without a picture yet nothing was configured */
    if (drawn <= 0) return;   /* failed, or skipped: no picture yet, timeout */
    {   /* where the picture is in the window, for the mouse (present.c) */
        float viewport[4];
        NativeRect cut;
        native_present_rect(n, w, h, viewport, &cut);
        gs_picture_rect(cut.left, cut.top, cut.right, cut.bottom, (int)w, (int)h);
    }
    rs = native_rstat_start();
    if (!n->interp) pacing_present(n->present_due, n->present_began);  /* holds the picture to its due time */
    native_rstat_add(RSTAT_PACE, rs);
    rs = native_rstat_start();
    dhdr_surface_present(p->surface);
    native_rstat_add(RSTAT_SHOW, rs);
    native_note_presented(n);
    if (spikes) {
        double end = host_wall_seconds();
        if (end - start >= 0.001)
            fprintf(stderr, "spike_present: field=%llu start=%.9f acquire_ms=%.6f pace_present_ms=%.6f\n",
                    (unsigned long long)n->field, start, (ready-start)*1e3, (end-ready)*1e3);
    }
}

/* PC SETTINGS > Resolution while the game runs (DHD_SCALE changed; finding 124): the upscaled
   surfaces (the frame and Z buffers, native_surface_resources' rule) start over at the new scale,
   as the browser's native_rescale does (gpu_rust_web_native.inc). Rust drops each texture (calling
   dhdr_native_surface with the other kind starts a surface over; the next picture asks for the
   right kind again, at the new scale, as it asks for every surface) and the pages they owned come
   from GS memory again at the next claim, as for a new surface. What only the GPU had drawn there
   is lost: the game draws its frame again every field, so at most one picture shows the CPU's copy.
   The exact backend answering the game's reads stays at 1x. */
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
    n->scale = 0;   /* native_picture: the setting, or the window's match */
    if (setting) fprintf(stderr, "gs native: resolution scale now %u (DHD_SCALE)\n", setting);
    else fprintf(stderr, "gs native: resolution scale now matching the window (DHD_SCALE)\n");
}

/* present.c (PC SETTINGS > Resolution): DHD_SCALE read again. The EE thread, so the render thread
   is held off (context_lock) while the surfaces are dropped; its next picture makes them again
   (native_prepare_surfaces), at the new scale. */
void gs_native_scale_apply(Gs *g)
{
    Native *n = ((NativeExact *)g->backend)->native;
    if (!n) return;
    native_mutex_lock(&n->context_lock);
    native_rescale(n);
    native_mutex_unlock(&n->context_lock);
}

/* A new surface (gs_gpu_surface_reset; context lock held): presents again, configured afresh. */
static void native_surface_reset(Native *n)
{
    n->present_failed = 0;
    n->resize = 1;
}

/* The display read: dhdr_native_read_finish waits for the map itself. */
static int native_read_wait(void) { return 1; }

/* ---- creation (EE thread, from rust_init) ---- */

/* 1: the native renderer is installed on g (native_install ran); 0: unavailable, g keeps the exact
   renderer. */
static int native_init(Gs *g, Rust *exact)
{
    double start;
    Native *n = native_create_rust(exact, device.memory, &start);   /* the scale limit with the card's memory */
    if (!n) return 0;
    /* A texture pack (dhd.ini texture_pack, textures): its default budget is half the card's memory. */
    n->texpack = texpack_open(&n->replace, device.memory);
    /* What PC SETTINGS > Resolution offers (pc_settings.c reads it; 4 without this renderer). */
    void host_putenv_s(const char *name, const char *value);  /* port/host */
    char most[4];
    snprintf(most, sizeof most, "%u", n->scale_limit);
    host_putenv_s("DHD_SCALE_MAX", most);
    fprintf(stderr, "gs native: WebGPU shaders and pipelines built in %.2f s\n", host_wall_seconds() - start);
    native_install(g, exact, n, "WebGPU");
    fprintf(stderr, "gs native: GS blending %s\n", n->dual ? "with dual-source blending" :
            "without dual-source blending (a draw blending on As writes its alpha in a second draw)");
    return 1;
}
