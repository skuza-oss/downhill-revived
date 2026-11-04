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
     limit counts the same memory, plus half the shared memory on an integrated GPU (under 1 GiB
     dedicated; native_scale_hold), DHD_SCALE_MAX publishes it (and
     DHD_SCALE_MAX_REASON why), host_service_gs_scale the scale in use;
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
    touch_present(p->surface);  /* drawn in the present pass */
    double rs = native_rstat_start(), inside = perf_log_on() ? host_wall_seconds() : 0;
    int drawn = native_present_draw(n, p->surface, w, h, n->resize != 0);
    native_rstat_add(RSTAT_PRESENT, rs);
    if (inside) perf_present_wait(host_wall_seconds() - inside);  /* the acquire: held by the GPU or the display */
    double ready = spikes ? host_wall_seconds() : 0;
    if (drawn || n->display_width) n->resize = 0;   /* without a picture yet nothing was configured */
    if (drawn <= 0) return;   /* failed, or skipped: no picture yet, timeout */
    {   /* where the picture is in the window, for the mouse (present.c) */
        float viewport[4];
        NativeRect cut;
        native_present_rect(n, w, h, viewport, &cut);
        gs_picture_rect(cut.left, cut.top, cut.right, cut.bottom, (int)w, (int)h);
        perf_render_size(n->interp ? "native interpolated" : "native", n->scale, n->display_width, n->display_height, w, h,
                         (unsigned)(cut.right - cut.left), (unsigned)(cut.bottom - cut.top));
    }
    rs = native_rstat_start();
    if (!n->interp) pacing_present(n->present_due, n->present_began);  /* holds the picture to its due time */
    native_rstat_add(RSTAT_PACE, rs);
    rs = native_rstat_start();
    inside = perf_log_on() ? host_wall_seconds() : 0;
    counter_present(p->surface, 1);
    if (inside) perf_present_wait(host_wall_seconds() - inside);  /* the present call itself */
    native_rstat_add(RSTAT_SHOW, rs);
    native_note_presented(n);
    if (spikes) {
        double end = host_wall_seconds();
        if (end - start >= 0.001)
            fprintf(stderr, "spike_present: field=%llu start=%.9f acquire_ms=%.6f pace_present_ms=%.6f\n",
                    (unsigned long long)n->field, start, (ready-start)*1e3, (end-ready)*1e3);
    }
}

/* A new surface (gs_gpu_surface_reset; context lock held): presents again, configured afresh. */
static void native_surface_reset(Native *n)
{
    n->present_failed = 0;
    n->resize = 1;
}

/* The display read: dhdr_native_read_finish waits for the map itself. */
static int native_read_wait(void) { return 1; }

/* PC SETTINGS' Resolution row (port/hle/pc_settings.c): the scale the pictures are drawn at
   (asked 0) or the one asked for (asked 1: DHD_SCALE's, or the window's match before the limit),
   native_scale_now's; 0 before the first picture and without this renderer. */
int host_service_gs_scale(int asked) { return (int)native_scale_now[asked ? 1 : 0]; }

/* ---- creation (EE thread, from rust_init) ---- */

/* 1: the native renderer is installed on g (native_install ran); 0: unavailable, g keeps the exact
   renderer. */
static int native_init(Gs *g, Rust *exact)
{
    double start;
    uint64_t scale_memory = device.memory;
    /* An integrated GPU (wgpu's adapter kind, dhdr_perf_read's word 5: 1) draws its frame and Z
       buffers in shared system memory (DXGI's SharedSystemMemory): the scale's budget counts half
       of that too, so it isn't held to the small dedicated slice (a Ryzen iGPU with 512 MiB
       dedicated ran 8x well). A discrete card keeps its dedicated memory only, however small;
       texture packs keep their budget. */
    uint64_t gpu_info[6] = {0};
    dhdr_perf_read(gpu_info, 6);
    if (gpu_info[5] == 1 && device.memory && device.shared) {
        scale_memory = device.memory + device.shared / 2;
        fprintf(stderr, "gs: integrated GPU: the resolution scale's memory counts shared memory too (%llu MiB dedicated, "
                "half of %llu MiB shared)\n", (unsigned long long)(device.memory >> 20),
                (unsigned long long)(device.shared >> 20));
    }
    /* Developer measurements: DHD_SCALE_MEMORY_MB overrides only the scale guard; texture packs
       retain their ordinary budget. */
    const char *memory_mb = getenv("DHD_SCALE_MEMORY_MB");
    if (memory_mb) {
        char *end;
        unsigned long long mb = strtoull(memory_mb, &end, 10);
        if (end != memory_mb && !*end && mb > 0 && mb <= 65536) {
            scale_memory = (uint64_t)mb << 20;
            fprintf(stderr, "gs: scale memory budget override %llu MiB (DHD_SCALE_MEMORY_MB)\n", mb);
        } else fprintf(stderr, "gs: invalid DHD_SCALE_MEMORY_MB; using adapter memory\n");
    }
    Native *n = native_create_rust(exact, scale_memory, &start);
    if (!n) return 0;
    /* A texture pack (dhd.ini texture_pack, textures): its default budget is half the card's memory. */
    n->texpack = texpack_open(&n->replace, device.memory);
    /* What PC SETTINGS > Resolution offers (pc_settings.c reads it; 4 without this renderer). */
    void host_putenv_s(const char *name, const char *value);  /* port/host */
    char most[4];
    snprintf(most, sizeof most, "%u", n->scale_limit);
    host_putenv_s("DHD_SCALE_MAX", most);
    /* and why it is less than 8: "memory" (graphics memory), "texture" (the largest texture) or
       "phone" (NATIVE_PHONE_SCALE_MAX) */
    host_putenv_s("DHD_SCALE_MAX_REASON", n->scale_reason == NATIVE_SCALE_MEMORY ? "memory" :
                  n->scale_reason == NATIVE_SCALE_TEXTURE ? "texture" :
                  n->scale_reason == NATIVE_SCALE_PHONE ? "phone" : "");
    fprintf(stderr, "gs native: WebGPU shaders and pipelines built in %.2f s\n", host_wall_seconds() - start);
    native_install(g, exact, n, "WebGPU");
    fprintf(stderr, "gs native: GS blending %s\n", n->dual ? "with dual-source blending" :
            "without dual-source blending (a draw blending on As writes its alpha in a second draw)");
    return 1;
}
