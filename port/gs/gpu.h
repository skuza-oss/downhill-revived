/* The GS renderer's interface: the Rust renderer (port/render, the dhd_render library) behind
   port/gs/gpu_rust.c, on every platform. */
#ifndef PORT_GS_GPU_H
#define PORT_GS_GPU_H
#include "gs.h"
/* Starts the renderer on g: 1 when it runs (g->backend set), 0 when there is no device for it. */
int gs_gpu_init(Gs *g);
void gs_gpu_destroy(Gs *g);
void gs_gpu_profile(Gs *g, double seconds);
int gs_gpu_display(Gs *g, unsigned *width, unsigned *height);
int gs_gpu_capture(Gs *g, uint32_t *pixels);
/* "wgpu" once gs_gpu_init succeeded; "none" before. */
const char *gs_gpu_backend_name(const Gs *g);
/* A window for the renderer's surface: its kind (dhd_render.h's DHDR_WINDOW_* values) and native
   handles (WIN32: a = HWND, b = HINSTANCE; XLIB: a = Display *, b = Window; WAYLAND: a = wl_display *,
   b = wl_surface *; ANDROID: a = ANativeWindow *; METAL_LAYER: a = CAMetalLayer *), and the
   function that tells its drawable size in pixels (0 x 0 while minimised; any thread). */
enum { GS_WINDOW_WIN32 = 1, GS_WINDOW_XLIB = 3, GS_WINDOW_WAYLAND = 4, GS_WINDOW_ANDROID = 5, GS_WINDOW_METAL_LAYER = 6 };
typedef struct GsWindow {
    unsigned kind;
    uint64_t a, b;
    void (*size)(unsigned *width, unsigned *height);
} GsWindow;
/* The renderer's surface on the window (copied): 1 when it presents there. gs_gpu_present then
   shows the display picture, or pixels (width x height) when given, stretched over the window's
   width x height client area. */
int gs_gpu_surface(Gs *g, const GsWindow *window);
/* The surface again on a new window, or none (NULL): the OS took the old one (Android, finding 091). */
int gs_gpu_surface_reset(Gs *g, const GsWindow *window);
/* present.c's SDL window: its drawable size in pixels (the GsWindow size it gives the renderer). */
void gs_window_pixels(unsigned *width, unsigned *height);
int gs_gpu_present(Gs *g, const uint32_t *pixels, unsigned width, unsigned height,
                   unsigned window_width, unsigned window_height);
/* 1 once a present failed on a lost device (the driver reset or removed the GPU; finding 171): no
   picture can be shown any more. Any thread. */
int gs_gpu_lost(void);
/* Counts describe submissions, not completed GPU work (the exact front end's, gpu_exact.inc). */
enum {
    GS_GPU_HW_DISABLED, GS_GPU_HW_UNAVAILABLE, GS_GPU_HW_FORMAT, GS_GPU_HW_ALIAS,
    GS_GPU_HW_PRIMITIVE, GS_GPU_HW_COORDINATE, GS_GPU_HW_DEGENERATE,
    GS_GPU_HW_REASON_COUNT
};
typedef struct {
    uint64_t hardware_batches, hardware_primitives;
    uint64_t reference_batches, reference_primitives;
    uint64_t rejected[GS_GPU_HW_REASON_COUNT];
    int hardware_enabled, hardware_available;
} GsGpuRasterStats;
/* The player whose split-screen quarter fills the window, 1-4; 0: the whole picture (present.h). */
extern volatile int gs_native_view;
/* DHD_RENDER=native: a render thread draws and presents each field (gpu_rust_native.inc). */
int gs_gpu_native(const Gs *g);
void gs_native_vsync(Gs *g, unsigned *width, unsigned *height, int resize);
/* The same with the pacing times (pacing_handed), flat limit and card mode (the field's 2D drawn for
   a card, host_widescreen_card_drawn) taken at the game's vsync, for a picture the GS worker hands
   on later (present.c, deferred vsync). */
void gs_native_vsync_handed(Gs *g, unsigned *width, unsigned *height, int resize, double due, double began,
                            int flat_limit, int card_2d);
void gs_native_bench(Gs *g, int end);  /* DHD_BENCH window start (0) and end (1) */
void gs_native_lock(Gs *g, int lock);  /* the render thread's lock (EE thread) */
void gs_native_present_options(Gs *g);  /* DHD_PRESENT_FILTER and DHD_ASPECT read again */
/* The last field's picture at the renderer's resolution (DHD_SCALE), owned by the renderer. */
uint32_t *gs_native_picture(Gs *g, unsigned *width, unsigned *height);
/* The field being recorded shows this RGBA picture instead of the display circuit's (a movie,
   port/hle/movie.c; the movie fills its middle rows), or the circuit again with NULL. Game thread,
   before gs_native_vsync. */
void gs_native_movie(Gs *g, const uint32_t *rgba, unsigned width, unsigned height, unsigned rows);
#ifdef DHD_GS_TEST_BUILD
/* Diagnostic only: coherent raw GPU image, including unpredicted writes.
   Unlike g->sync this never filters pages or merges into g->vram. */
int gs_gpu_readback_all(Gs *g, uint32_t *image);
#endif
#endif
