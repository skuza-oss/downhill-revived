/* The GS renderer: the exact front end gpu_exact.inc over the Rust renderer (port/render,
   dhd_render.lib), with its native renderer (gpu_rust_native.inc, DHD_RENDER=native). This file
   defines gpu.h's gs_gpu_* and gs_native_* entry points (the x64 build's; the browser's glue is
   port/web/gpu_rust_web.c).

   The device ops forward to the dhdr_exact_* calls in the op_* seam's order (port/render/OPTRACE.md
   records them); the device, the pipelines, the buffers, the surface and the present pass are in
   Rust (port/render/include/dhd_render.h). Rust reads no environment: this file reads the DHD_*
   variables and passes them as flags.

   What is here: the device ops, the device's options (DhdrDeviceDesc from the DHD_WGPU_* variables
   and DXGI's adapter), the exact renderer's life, capture, the surface and the exact present, the
   profile line, the DHD_GS_OPTRACE recorder's header hook (op_adapter), the "gs: renderer" line,
   and, in the test build, gpu_rust_test.c's hooks (included at the end).

   Pacing: present.c calls pacing_present right before gs_gpu_present, so the picture is held to its
   due time first; dhdr_present_exact then draws it and dhdr_surface_present presents it at once.

   DHD_RENDER=native: port/gs/gpu_rust_native.inc. Its contract with this file: NativeExact is Rust
   (its members x, native, clip_on, clip[4] and window), native_core.inc's gs_gpu_native and gs_native_*
   hooks are gpu.h's, and it defines static int native_init(Gs *g, Rust *exact) (1: native_install ran) and
   static int native_waits_for_vsync(struct Native *n); native_capture and native_profile are
   native_core.inc's.

   Every native platform builds this file over its dhd_render library: the window is present.c's
   (a GsWindow: Win32's HWND, X11's or Wayland's window, Android's ANativeWindow, Apple's
   CAMetalLayer, with its size function), and the renderer picks the backend (DX12 or Vulkan on
   Windows, Metal on Apple, Vulkan elsewhere). Windows alone has DXGI (the adapter choice, the
   video memory) and the DX12 shader compiler. gs_gpu_surface_reset swaps the surface when a
   phone's app goes to the background and back (finding 091). */
#define COBJMACROS
#include "gpu.h"
#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#endif
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_timer.h>  /* the performance counter */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dhd_render.h"   /* -Iport/render/include */
#include "perf_log.h"     /* the perf: lines (finding 129) */

#ifndef DHD_GS_TEST_BUILD
#include "../net/net.h"
int touch_host_overlay(float *items, int max, float *width_mm, float *height_mm);  /* port/host/touch_host.c */
#endif

/* PC SETTINGS' FPS & Ping (DHD_PERF_COUNTER, finding 120): read at the first picture and again
   when the page changes it (gs_present_options_apply, on the game thread that sets it), not for
   every picture: getenv scans the whole environment (finding 090 SP14). -1: not read yet. */
static SDL_AtomicInt counter_on = {-1};

void gs_gpu_counter_apply(void)
{
    const char *v = getenv("DHD_PERF_COUNTER");
    SDL_SetAtomicInt(&counter_on, v && atoi(v) != 0);
}

/* The touch controls over the picture (port/host/touch_host.c, finding 141): their shapes for the
   next picture. The native renderer draws them in its present pass (finding 146), so it hands
   them over before dhdr_native_present; the exact one draws them in dhdr_surface_present. */
static void touch_present(DhdrSurface *surface)
{
#ifndef DHD_GS_TEST_BUILD
    static float shapes[512 * 16];
    float width_mm = 0, height_mm = 0;
    int n = touch_host_overlay(shapes, 512, &width_mm, &height_mm);
    dhdr_surface_touch(surface, shapes, (uint32_t)n, width_mm, height_mm);
#else
    (void)surface;
#endif
}

/* touched: touch_present ran for this picture already (the native renderer's). */
static void counter_present(DhdrSurface *surface, int touched)
{
    if (SDL_GetAtomicInt(&counter_on) < 0) gs_gpu_counter_apply();
    int enabled = SDL_GetAtomicInt(&counter_on), ms = -1, riders = 0;
#ifndef DHD_GS_TEST_BUILD
    /* in a race the other riders' delay, else the room server's ping (finding 169) */
    if (enabled && (ms = net_riders_ms()) >= 0) riders = 1;
    else if (enabled) ms = net_ping_ms();
#endif
    dhdr_surface_counter(surface, enabled, ms, riders,
                         (double)SDL_GetPerformanceCounter() / SDL_GetPerformanceFrequency());
    if (!touched) touch_present(surface);
    dhdr_surface_present(surface);
    perf_presented();  /* every picture shown, by either renderer */
}

/* Rust's log lines (one UTF-8 line, no newline), from any thread: stderr, which is dhd.log in the
   game (host.c; hle_log writes there too), flushed so a line before an abort is kept. Written with
   stdio, not hle_log, so the GS test programs (no port/hle) link this file as it is. */
void dhd_host_log(const char *line, size_t bytes)
{
    fprintf(stderr, "%.*s\n", (int)bytes, line);
    fflush(stderr);
}

/* "gs: renderer <dhdr_version()>" once at start, so every run of a program with dhd_render linked
   says so (port/tools/gs_verdict.py asks for the line in every run). The game opens dhd.log later
   (host.c), so device_init says it again there, with the adapter. */
__attribute__((constructor)) static void renderer_line(void)
{
    fprintf(stderr, "gs: renderer %s\n", dhdr_version());
    fflush(stderr);
}

/* gpu_exact.inc's DHD_GS_OPTRACE recorder (port/render/OPTRACE.md), off unless the variable is set;
   op_adapter and the capture's exact_trace_capture below are its hooks. */
#ifdef _WIN64
#define EXACT_OPTRACE 1
#endif
#include "gpu_exact.inc"  /* the front end: Exact, the CPU half, and the op_* prototypes it calls */

struct Native;

/* The provider's state. Its first member is the front end's (g->backend points at this). */
typedef struct Rust {
    Exact e;
    DhdrExact *x;
    uint32_t raster;              /* the DHDR_RASTER_* flags x's pipelines were built with */
    DhdrSurface *surface;
    unsigned display_width, display_height;
    DhdrExactStats stats;         /* dhdr_exact_stats at the last profile line */
    /* DHD_RENDER=native (gpu_rust_native.inc): this backend then only answers the Z probe, and
       draws only inside clip when clip_on (nothing until a probe); window: the surface's (its size
       function NULL without a surface). native_core.inc reads and writes native, clip_on and clip
       on its NativeExact (this struct). */
    struct Native *native;
    int clip_on, clip[4];
    GsWindow window;
} Rust;

/* ---- the device ops (gpu_exact.inc's op_* seam): op_write, op_copy, op_batch, op_record,
   op_submit and op_clip are gpu_rust_shared.inc's (through gpu_rust_native.inc) ---- */

/* Natively begin blocks until the buffer is mapped; a failed map ends the process. */
static void op_read(Exact *gpu, int buffer, const uint32_t want[16], void *image)
{
    DhdrExact *x = ((Rust *)gpu)->x;
    if (!dhdr_exact_read_begin(x, (uint32_t)buffer, want) || !dhdr_exact_read_finish(x, (uint32_t)buffer, want, image)) {
        fprintf(stderr, "gs: rust readback failed\n");
        exit(1);
    }
}

/* ---- host hooks (not device calls) ---- */

static uint64_t op_clock(void) { return SDL_GetPerformanceCounter(); }

/* Diagnostic isolation by VU1 byte entry (DHD_DRAW_SKIP_VU, _PROGRAM, _OVERLAY). */
static int op_draw_skipped(const Gs *g)
{
    static int skip_initialized;
    static unsigned skip_entries[64], skip_count, skip_overlay, skip_programs[64], program_count;
    if (!skip_initialized) {
        const char *p = getenv("DHD_DRAW_SKIP_VU");
        const char *overlay = getenv("DHD_DRAW_SKIP_OVERLAY");
        if (overlay) skip_overlay = !strcmp(overlay, "A") ? 1 : !strcmp(overlay, "B") ? 2 : 0;
        skip_initialized = 1;
        while (p && *p && skip_count < 64) {
            char *end;
            unsigned entry = strtoul(p, &end, 16);
            if (end == p || entry >= 0x4000) break;
            skip_entries[skip_count++] = entry;
            p = *end == ',' ? end + 1 : end;
        }
        p = getenv("DHD_DRAW_SKIP_PROGRAM");
        while (p && *p && program_count < 64) {
            char *end;
            unsigned entry = strtoul(p, &end, 16);
            if (end == p || entry >= 0x4000) break;
            skip_programs[program_count++] = entry;
            p = *end == ',' ? end + 1 : end;
        }
    }
    if (g->source_path == 1 && skip_overlay && g->vu_overlay == skip_overlay) return 1;
    if (g->source_path == 1)
        for (unsigned i = 0; i < program_count; i++)
            if (g->vu_program == skip_programs[i]) return 1;
    if (g->source_path == 1)
        for (unsigned i = 0; i < skip_count; i++)
            if (g->vu_entry == skip_entries[i]) return 1;
    return 0;
}

/* ---- the device: one per process (dhdr_device_init latches it) ---- */

/* The process's one device (the first dhdr_device_init that succeeded): its options, for the
   optrace header (op_adapter), and DXGI's view of its adapter: the dedicated memory (the texture
   pack's and the resolution scale's budget, gpu_rust_native.inc) and the user-mode driver version. */
static struct { int made; uint32_t flags, vendor_id, device_id; uint64_t memory, shared, driver_version; } device;

#ifdef _WIN32
/* DXGI orders hardware by power/performance; keep EnumAdapters1 for the default. */
static int dxgi_preferred(int integrated, DXGI_ADAPTER_DESC1 *desc)
{
    IDXGIFactory6 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    int found = 0;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory6, (void **)&factory))) return 0;
    for (UINT i = 0; SUCCEEDED(IDXGIFactory6_EnumAdapterByGpuPreference(factory, i,
             integrated ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             &IID_IDXGIAdapter1, (void **)&adapter)); i++) {
        found = SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, desc)) && !(desc->Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        IDXGIAdapter1_Release(adapter);
        if (found) break;
    }
    IDXGIFactory6_Release(factory);
    return found;
}

/* A DXGI adapter's description and UMD version: the first adapter (Windows' default, the game's
   GPU) when vendor is 0, else the one with these PCI ids. 0 when there is none. */
static int dxgi_adapter(unsigned vendor, unsigned id, DXGI_ADAPTER_DESC1 *desc, uint64_t *umd)
{
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    LARGE_INTEGER version;
    int found = 0;
    *umd = 0;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) return 0;
    for (UINT i = 0; !found && SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, i, &adapter)); i++) {
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, desc)) &&
            (!vendor || (desc->VendorId == vendor && desc->DeviceId == id))) {
            found = 1;
            if (SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &version)))
                *umd = (uint64_t)version.QuadPart;
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    return found;
}
#endif

/* The device's settings as DhdrDeviceDesc flags: DHD_WGPU_BACKEND=vulkan (Windows; elsewhere the
   platform's one API), DHD_WGPU_DEBUG, DHD_WGPU_DUAL_SOURCE=0; on Windows DHD_WGPU_COMPILER=fxc
   (else DXC when dxcompiler.dll is beside the exe) and the adapter: DHD_WGPU_ADAPTER=fallback
   (WARP), else DXGI's first adapter by its PCI ids (the GPU the D3D11 presentation device took
   before), WARP when that is Microsoft's Basic Render Driver (no graphics driver); with no DXGI
   adapter, or another DHD_WGPU_ADAPTER value, wgpu's high-performance choice (WARP last).
   Elsewhere wgpu's high-performance adapter.
   Once the device exists later calls return 1 at once; a failure is latched by dhdr_device_init. */
static int device_init(void)
{
    if (device.made) return 1;
    DhdrDeviceDesc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.abi = DHDR_ABI_VERSION;
    const char *backend = getenv("DHD_WGPU_BACKEND"), *compiler = getenv("DHD_WGPU_COMPILER");
    const char *choice = getenv("DHD_WGPU_ADAPTER"), *dual = getenv("DHD_WGPU_DUAL_SOURCE");
    if (backend && !strcmp(backend, "vulkan")) d.flags |= DHDR_DEV_VULKAN;
    if (getenv("DHD_WGPU_DEBUG")) d.flags |= DHDR_DEV_DEBUG | DHDR_DEV_VALIDATION;
    if (dual && !atoi(dual)) d.flags |= DHDR_DEV_NO_DUAL_SOURCE;
    d.log_level = getenv("DHD_WGPU_DEBUG") ? DHDR_LOG_WARN : DHDR_LOG_ERROR;
#ifdef _WIN32
    static char dxc[MAX_PATH + 16];
    DWORD length = GetModuleFileNameA(NULL, dxc, MAX_PATH);
    char *slash = length && length < MAX_PATH ? strrchr(dxc, '\\') : NULL;
    if (compiler && !strcmp(compiler, "fxc")) d.flags |= DHDR_DEV_FXC;
    else if (slash) {
        strcpy(slash + 1, "dxcompiler.dll");
        if (GetFileAttributesA(dxc) != INVALID_FILE_ATTRIBUTES) d.dxc_path = dxc;
        else d.flags |= DHDR_DEV_FXC;
    } else d.flags |= DHDR_DEV_FXC;
    DXGI_ADAPTER_DESC1 desc;
    uint64_t umd;
    int known = dxgi_adapter(0, 0, &desc, &umd);
    int preferred = choice && (!strcmp(choice, "integrated") || !strcmp(choice, "discrete"));
    if (preferred) {
        if (!dxgi_preferred(!strcmp(choice, "integrated"), &desc)) {
            fprintf(stderr, "gs: adapter preference %s unavailable\n", choice);
            return 0;
        }
        d.vendor_id = desc.VendorId; d.device_id = desc.DeviceId;
        fprintf(stderr, "gs: GPU preference %s (DXGI %s, PCI %04x:%04x)\n", choice,
                !strcmp(choice, "integrated") ? "minimum power" : "high performance", d.vendor_id, d.device_id);
    }
    if (choice ? !strcmp(choice, "fallback") : known && desc.VendorId == 0x1414 && desc.DeviceId == 0x8C)
        d.flags |= DHDR_DEV_FALLBACK;
    else if (!choice && known) { d.vendor_id = desc.VendorId; d.device_id = desc.DeviceId; }
    if (getenv("DHD_PROFILE"))
        fprintf(stderr, "gs: WebGPU shader compiler %s\n", d.dxc_path ? d.dxc_path : "FXC");
#else
    (void)compiler; (void)choice;
#endif
    if (!dhdr_device_init(&d)) return 0;
    device.made = 1;
    device.flags = d.flags;
#ifdef _WIN32
    /* the adapter's PCI ids as far as they are known here: DXGI's first adapter (asked for by its
       ids), WARP's for the fallback adapter, else unknown (the high-performance adapter) */
    if (d.flags & DHDR_DEV_FALLBACK) { device.vendor_id = 0x1414; device.device_id = 0x8C; }
    else { device.vendor_id = d.vendor_id; device.device_id = d.device_id; }
    if (device.vendor_id && dxgi_adapter(device.vendor_id, device.device_id, &desc, &device.driver_version))
        device.memory = (uint64_t)desc.DedicatedVideoMemory, device.shared = (uint64_t)desc.SharedSystemMemory;
#endif
    fprintf(stderr, "gs: renderer %s on %s\n", dhdr_version(), dhdr_adapter_name());
    fflush(stderr);
    return 1;
}

/* The DHD_GS_* shader options, as DHDR_RASTER_* flags. */
static uint32_t raster_flags(void)
{
    const char *early = getenv("DHD_GS_EARLY_DEPTH"), *shift = getenv("DHD_GS_ADDRESS_SHIFT");
    const char *store = getenv("DHD_GS_EXCLUSIVE_WORD_STORE");
    return (!early || atoi(early) ? DHDR_RASTER_EARLY_DEPTH : 0) | (!shift || atoi(shift) ? DHDR_RASTER_ADDRESS_SHIFT : 0) |
           (!store || atoi(store) ? DHDR_RASTER_EXCLUSIVE_WORD_STORE : 0);
}

#ifdef EXACT_OPTRACE
/* The optrace header (a host hook, not a device op): the API and DX12 shader compiler (the
   device's options), the adapter's ids, name and DXGI user-mode driver version, and the raster
   options this instance's pipelines were built with. */
static void op_adapter(const Exact *gpu, ExactAdapter *a)
{
    const Rust *p = (const Rust *)gpu;
    a->raster_flags = p->raster;
    if (device.made) {
        a->api = device.flags & DHDR_DEV_VULKAN ? 2 : 1;
        a->compiler = a->api == 1 ? (device.flags & DHDR_DEV_FXC ? 1 : 2) : 0;
        a->fallback = (device.flags & DHDR_DEV_FALLBACK) != 0;
        a->vendor_id = device.vendor_id; a->device_id = device.device_id;
        a->driver_version = device.driver_version;
    }
    snprintf(a->name, sizeof a->name, "%s", dhdr_adapter_name());
}
#endif

/* The window's drawable size in pixels, from its own size function (present.c's SDL window, or a
   test program's window). 0 x 0 when there is none (minimised, or the app in the background). */
static void window_size(const GsWindow *window, unsigned *width, unsigned *height)
{
    *width = *height = 0;
    if (window->size) window->size(width, height);
}

/* ---- the native renderer (DHD_RENDER=native) ---- */

#include "gpu_rust_native.inc"  /* the shared device ops, the native hooks, native_init, native_waits_for_vsync */

/* ---- the renderer's interface (gpu.h) ---- */

/* What was recorded reaches VRAM and the GPU first (sync, then submit). */
void gs_gpu_destroy(Gs *g)
{
    Rust *p = g->backend;
    if (!p) return;
    if (p->x) {
        sync_vram(g);
        if (p->e.recorded) submit_all(&p->e);
    }
    dhdr_surface_destroy(p->surface);
    dhdr_exact_destroy(p->x);
    exact_free(&p->e);
    free(p); g->backend = NULL; g->draw = NULL; g->sync = NULL; g->sync_range = NULL;
    g->local_copy = NULL; g->stage_pages = NULL; g->fetch_pages = NULL;
    g->flush_draw = NULL;
    g->write_high = NULL;
}

const char *gs_gpu_backend_name(const Gs *g) { return g && g->backend ? "wgpu" : "none"; }

/* The front end's counts, then dhd_render's own since the last line (dhdr_exact_stats): its reads,
   the time they waited for the mapping, and the bytes written into its buffers. */
void gs_gpu_profile(Gs *g, double seconds)
{
    Rust *p = g->backend;
    Exact *gpu = p ? &p->e : NULL;
    if (!gpu || !gpu->profile) return;
    double ms = 1000.0 / (double)SDL_GetPerformanceFrequency() / seconds;
    fprintf(stderr, "profile GS (rust): %.1f draws/s, %.1f snapshots/s, %.1f uploads/s, draw %.1f ms/s, readback %.1f ms/s, %.1f dispatches/s, %.1f submits/s, %.1f local copies/s\n",
            gpu->draws / seconds, gpu->snapshots / seconds, gpu->uploads / seconds,
            gpu->draw_ticks * ms, gpu->sync_ticks * ms, gpu->dispatches / seconds, gpu->submits / seconds,
            gpu->copies / seconds);
    DhdrExactStats now;
    memset(&now, 0, sizeof now);
    if (p->x) dhdr_exact_stats(p->x, &now);
    fprintf(stderr, "profile GS (rust) dhd_render: %.1f reads/s, map wait %.1f ms/s, %.2f MB/s written\n",
            (double)(now.reads - p->stats.reads) / seconds, (double)(now.read_ticks - p->stats.read_ticks) / 1e6 / seconds,
            (double)(now.bytes_written - p->stats.bytes_written) / 1048576.0 / seconds);
    p->stats = now;
    if (p->native) native_profile(p->native);
    gpu->draw_ticks = gpu->sync_ticks = gpu->draws = gpu->snapshots = gpu->uploads = 0;
    gpu->dispatches = gpu->submits = gpu->copies = 0;
}

int gs_gpu_display(Gs *g, unsigned *width, unsigned *height)
{
    return exact_display(g, width, height);
}

/* The display texture's pixels (rows unpadded); its size in display_width x display_height. A
   capture with no display picture is traced too (0 x 0). */
int gs_gpu_capture(Gs *g, uint32_t *pixels)
{
    Rust *p = g->backend;
    if (!p) return 0;
    if (p->native) return native_capture(p->native, pixels);
    if (!p->e.display_valid) { exact_trace_capture(&p->e, 0, 0, NULL); return 0; }
    exact_commands(&p->e);
    submit_all(&p->e);
    uint32_t w = 0, h = 0;
    if (!dhdr_exact_capture_begin(p->x, &w, &h)) {
        exact_trace_capture(&p->e, 0, 0, NULL);
        return 0;
    }
    if (!dhdr_exact_capture_finish(p->x, pixels)) {
        fprintf(stderr, "gs: rust display readback failed\n");
        exit(1);
    }
    exact_trace_capture(&p->e, w, h, pixels);
    p->display_width = w; p->display_height = h;
    return 1;
}

_Static_assert(GS_WINDOW_WIN32 == DHDR_WINDOW_WIN32 && GS_WINDOW_XLIB == DHDR_WINDOW_XLIB &&
               GS_WINDOW_WAYLAND == DHDR_WINDOW_WAYLAND && GS_WINDOW_ANDROID == DHDR_WINDOW_ANDROID &&
               GS_WINDOW_METAL_LAYER == DHDR_WINDOW_METAL_LAYER, "gpu.h's window kinds are dhd_render.h's");

/* A surface on the window (present.c's SDL window: its HWND, X11 or Wayland window, ANativeWindow
   or CAMetalLayer): frame latency 1, Immediate unless DHD_TEARING=0, else Mailbox, else Fifo; Fifo
   when the native renderer waits for vsync. */
int gs_gpu_surface(Gs *g, const GsWindow *window)
{
    Rust *p = g->backend;
    if (!p || !window || !window->a || p->surface) return p && p->surface;
    p->window = *window;
    DhdrWindow w;
    memset(&w, 0, sizeof w);
    w.kind = window->kind;
    w.a = window->a;
    w.b = window->b;
    /* V-Sync (dhd.ini "vsync", PC SETTINGS): no tearing, without waiting where the display allows
       it (Mailbox, else Fifo; present/surface.rs). Off: pictures go out at once and may tear.
       DHD_TEARING=0 is the older name for it. */
    const char *tearing = getenv("DHD_TEARING"), *vsync = getenv("DHD_VSYNC");
    uint32_t flags = (vsync && atoi(vsync)) || (tearing && !atoi(tearing)) ? DHDR_PRESENT_NO_TEARING : 0;
    if (p->native && native_waits_for_vsync(p->native)) flags |= DHDR_PRESENT_VSYNC;
    p->surface = dhdr_surface_create(&w, flags);
    if (!p->surface) memset(&p->window, 0, sizeof p->window);
    if (p->surface && getenv("DHD_PROFILE"))
        fprintf(stderr, "gs: WebGPU surface (dhd_render), present flags %u, usage %u\n", flags,
                dhdr_surface_usage(p->surface));
    return p->surface != NULL;
}

/* The surface again on a new window, or none (window NULL): the OS took the old one (Android: the
   app went to the background; its surface comes back new). The native renderer's render thread
   is held off meanwhile (its context lock); a native present that failed on the lost surface is
   forgiven, and the next one reconfigures. */
int gs_gpu_surface_reset(Gs *g, const GsWindow *window)
{
    Rust *p = g->backend;
    if (!p) return 0;
    if (p->native) gs_native_lock(g, 1);
    dhdr_surface_destroy(p->surface);
    p->surface = NULL;
    memset(&p->window, 0, sizeof p->window);
    int ok = !window || gs_gpu_surface(g, window);
    if (p->native) native_surface_reset(p->native);
    if (p->native) gs_native_lock(g, 0);
    return ok;
}

/* 0 makes present.c stop presenting (-1 from Rust, or no picture yet); a skipped frame (minimised,
   timeout) is 1. present.c has paced the frame already (pacing_present, right before this). pixels
   (a movie or the CPU scanout) go into the display texture, as a scanout would, so a later capture
   returns them. */
int gs_gpu_present(Gs *g, const uint32_t *pixels, unsigned width, unsigned height,
                   unsigned window_width, unsigned window_height)
{
    Rust *p = g->backend;
    if (!p || !p->surface) return 0;
    submit_all(&p->e); /* the scanout, before the picture is replaced or shown */
    if (pixels && (!width || !height || width > 1024 || height > 1024)) return 0;
    if (!pixels && !p->e.display_valid) return 0;
    int r = dhdr_present_exact(p->surface, p->x, pixels, width, height, window_width, window_height);
    if (r < 0) return 0;
    if (pixels) p->e.display_valid = 1;
    if (r == 1) {
        perf_render_size(pixels ? "exact (a movie or the CPU scanout)" : "exact", 1, width, height, window_width,
                         window_height, 0, 0);
        counter_present(p->surface, 0);
    }
    return 1;
}

int gs_gpu_init(Gs *g)
{
    const char *render = getenv("DHD_RENDER");
    int native = render && !strcmp(render, "native");
    if (!device_init()) return 0;
    Rust *p = calloc(1, sizeof *p);
    if (!p) return 0;
    g->backend = p;
    if (!exact_alloc(&p->e)) goto fail;
    const char *limit = getenv("DHD_GS_BATCH");
    exact_config(&p->e, getenv("DHD_PROFILE") != NULL, limit ? strtoul(limit, NULL, 0) : 0);
    p->raster = raster_flags();
    if (!(p->x = dhdr_exact_create(p->raster))) goto fail;
    exact_hooks(g, &p->e);
    /* DHD_RENDER=native draws on this device too; the exact renderer then answers only the Z probe. */
    if (native && !native_init(g, p))
        fprintf(stderr, "gs: native renderer unavailable; using the exact renderer\n");
    return 1;
fail:
    fprintf(stderr, "gs: WebGPU rasterizer initialization failed\n");
    gs_gpu_destroy(g);
    return 0;
}

#ifdef DHD_GS_TEST_BUILD
int gs_gpu_readback_all(Gs *g, uint32_t *image) { return g->backend && readback_all(g, image); }
#endif

#ifdef DHD_GS_TEST_BUILD
#include "gpu_rust_test.c"  /* gs_wgpu_depth_probe, gs_wgpu_adapter, gs_wgpu_presented (task HK) */
#endif
