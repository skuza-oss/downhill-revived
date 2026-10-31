/* Scanout and the window. GS drawing and texture transfers share local memory; presentation
   reads the framebuffer selected by the game's DISPFB/DISPLAY MMIO registers, and the renderer
   (gpu.h, the Rust renderer) presents it on its surface on the window. */
#ifdef _WIN32
#include <windows.h>
#endif
/* The window is SDL's (W11), on every platform. It is made and driven on the SDL thread, the
   process's main thread (host/sdl_host.c): its events come there, and its calls go there through
   sdl_host_run. The renderer's surface takes the window's native handle (window_target, the one
   platform part): Win32's HWND, X11's or Wayland's window, Android's ANativeWindow, or on Apple
   the CAMetalLayer of an SDL Metal view. On a phone the surface is dropped while the app is in
   the background and made again when it comes back (app_background_wait, finding 091). */
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "present.h"
#include "gpu.h"
#include "capture.h"
#include "../hle/hle.h"
#include "../include/guest.h"
#include "../hle/text_input.h"

static uint32_t pixels[1024 * 1024];
static int initialized, disabled, resize_pending;
static int surface;                 /* the renderer presents to the window (init) */
static volatile int window_closed;  /* the close button (or EXIT GAME): the game ends at the next field */
static SDL_Window *sdl_window;
static GsWindow target;             /* sdl_window's native handle, for the renderer's surface */
static volatile int target_ready;   /* window_target found it (a phone: not while in the background) */
/* sdl_window's drawable size in pixels, w << 16 | h (0 while minimised): measured on the thread that
   pumps SDL's events (window_measure, window_watch) and read on any thread (gs_window_pixels), as
   Cocoa's window calls belong to the main thread. */
static SDL_AtomicInt drawable;
typedef struct { int x, y, w, h, borderless; } WindowSpec;
static void display_of(SDL_Window *w, SDL_Rect *full, SDL_Rect *usable);

/* ---- the platform part: SDL's window as the renderer's window ---- */
#if defined(_WIN32)
#define WINDOW_FLAGS 0
#define FIXED_SCREEN 0
/* Laptops with two graphics chips run a program they don't know on the integrated one; these
   exports ask the NVIDIA and AMD drivers for the dedicated chip, as most PC games do. In the exe
   (this file is host code). */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#elif defined(__ANDROID__)
#define WINDOW_FLAGS 0
#define FIXED_SCREEN 1  /* the whole screen, immersive (landscape: android_main.c's hint) */
#elif defined(__APPLE__)
#define WINDOW_FLAGS SDL_WINDOW_METAL  /* the renderer draws into an SDL Metal view's layer */
#define FIXED_SCREEN 0
static SDL_MetalView metal_view;
#else
#define WINDOW_FLAGS 0
#define FIXED_SCREEN 0
#endif

/* Main thread: the native handle of sdl_window into target; target_ready 0 while it has none (a
   phone: until the OS has made the surface, and again after the app comes back from the background). */
static void window_target(void *unused)
{
    (void)unused;
    SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window);
    memset(&target, 0, sizeof target);
    target.size = gs_window_pixels;
#if defined(_WIN32)
    target.kind = GS_WINDOW_WIN32;
    target.a = (uintptr_t)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    target.b = (uintptr_t)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, NULL);
#elif defined(__ANDROID__)
    target.kind = GS_WINDOW_ANDROID;
    target.a = (uintptr_t)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
#elif defined(__APPLE__)
    if (!metal_view) metal_view = SDL_Metal_CreateView(sdl_window);
    target.kind = GS_WINDOW_METAL_LAYER;
    target.a = (uintptr_t)(metal_view ? SDL_Metal_GetLayer(metal_view) : NULL);
#else  /* Linux and the BSDs: X11 or Wayland, whichever SDL's video driver is */
    if (!strcmp(SDL_GetCurrentVideoDriver(), "wayland")) {
        target.kind = GS_WINDOW_WAYLAND;
        target.a = (uintptr_t)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
        target.b = (uintptr_t)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    } else {
        target.kind = GS_WINDOW_XLIB;
        target.a = (uintptr_t)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
        target.b = (uint64_t)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    }
#endif
    target_ready = target.a != 0;
}

/* Main thread: the window's icon (from n0buholic, PR #13). Windows and Android show the program's
   own: the exe's (port/res/dhd.rc, every size from 16 to 256 pixels, on SDL's window class:
   make_window's hints; SDL_SetWindowIcon would give the title bar one 512-pixel picture scaled
   down) and the APK's. Elsewhere nothing else gives the window one (Linux) or it is the bundle's
   own picture (macOS): port/res/icon.png, embedded by build_host64.py. */
#if defined(_WIN32) || defined(__ANDROID__)
static void window_icon(void) {}
#else
#include "dhd_icon.inc"  /* dhd_icon_png: port/res/icon.png's bytes */
static void window_icon(void)
{
    SDL_Surface *icon = SDL_LoadPNG_IO(SDL_IOFromConstMem(dhd_icon_png, sizeof dhd_icon_png), true);
    if (!icon || !SDL_SetWindowIcon(sdl_window, icon)) hle_log("gs: no window icon (%s)\n", SDL_GetError());
    SDL_DestroySurface(icon);
}
#endif

/* The whole monitor (DHD_WINDOW borderless and fullscreen). Windows: a borderless window over the
   monitor, as the port always had it (no mode change; alt-tab is a window's). Elsewhere SDL's
   desktop fullscreen: macOS's menu bar and Wayland's compositor leave a window no other way to
   cover the screen. Main thread. */
static void fill_monitor(void)
{
#ifdef _WIN32
    SDL_Rect monitor, usable;
    display_of(sdl_window, &monitor, &usable);
    SDL_SetWindowBordered(sdl_window, false);
    SDL_SetWindowSize(sdl_window, monitor.w, monitor.h);
    SDL_SetWindowPosition(sdl_window, monitor.x, monitor.y);
#else
    SDL_SetWindowFullscreen(sdl_window, true);
#endif
}

/* Started minimised (Windows: STARTUPINFO's show state, test and recording runs on a PC in use):
   the game's window shows minimised without taking the focus (show_window). */
#ifdef _WIN32
static int start_minimised(void)
{
    STARTUPINFOA si = {sizeof si};
    GetStartupInfoA(&si);
    if (!(si.dwFlags & STARTF_USESHOWWINDOW)) return 0;
    return si.wShowWindow == SW_SHOWMINIMIZED || si.wShowWindow == SW_MINIMIZE ||
           si.wShowWindow == SW_SHOWMINNOACTIVE || si.wShowWindow == SW_FORCEMINIMIZE;
}

static void show_minimised(void)
{
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    SDL_ShowWindow(sdl_window);
    ShowWindow((HWND)(uintptr_t)target.a, SW_SHOWMINNOACTIVE);
}
#else
static int start_minimised(void) { return 0; }
static void show_minimised(void) {}
#endif

/* ---- the window, the same on every platform ---- */

/* The primary display's resolution: its desktop mode, the one the OS runs it at (the largest
   fullscreen mode can be bigger than the screen: NVIDIA DSR lists 2560x1440 on a 1080p monitor),
   else SDL's largest fullscreen mode. Pixels (host_dpi_aware). 0 x 0 when SDL knows none. */
static void monitor_mode(int *w, int *h)
{
    SDL_DisplayID id = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(id);
    *w = *h = 0;
    if (desktop) *w = desktop->w, *h = desktop->h;
    if (*w > 0 && *h > 0) return;
    int count = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(id, &count);
    if (modes && count > 0 && modes[0]) *w = modes[0]->w, *h = modes[0]->h;
    SDL_free(modes);
}

/* DHD_MONITOR_MODE: the monitor's resolution, for PC SETTINGS > Window Size's list. A window
   size of "monitor" (dhd.ini's default, port/render/settings) becomes that size, in numbers, so
   everything after (gs_window_apply, the settings page) reads the size it has. Main thread, before
   the window is made: the game thread waits in sdl_host_run. */
static void monitor_size(WindowSpec *s)
{
    int w, h;
    char text[16];
    monitor_mode(&w, &h);
    if (w <= 0 || h <= 0) return;
    snprintf(text, sizeof text, "%dx%d", w, h);
    host_putenv_s("DHD_MONITOR_MODE", text);
    const char *width = getenv("DHD_WINDOW_WIDTH"), *height = getenv("DHD_WINDOW_HEIGHT");
    int named = (width && !strcmp(width, "monitor")) || (height && !strcmp(height, "monitor"));
    if (!named) return;
    snprintf(text, sizeof text, "%d", w);
    host_putenv_s("DHD_WINDOW_WIDTH", text);
    snprintf(text, sizeof text, "%d", h);
    host_putenv_s("DHD_WINDOW_HEIGHT", text);
    s->w = w, s->h = h;
    hle_log("gs: window size %dx%d, the monitor's resolution\n", w, h);
}

static void window_measure(void)
{
    int w = 0, h = 0;
    if (sdl_window && !(SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_MINIMIZED))
        SDL_GetWindowSizeInPixels(sdl_window, &w, &h);
    SDL_SetAtomicInt(&drawable, (w > 0 ? w : 0) << 16 | (h > 0 ? h : 0));
}

/* SDL's window events as SDL sends them (on Windows inside a modal drag or resize too, which the
   queued copies wait out): the drawable size follows at once. */
static bool SDLCALL window_watch(void *unused, SDL_Event *e)
{
    (void)unused;
    if (e->type >= SDL_EVENT_WINDOW_FIRST && e->type <= SDL_EVENT_WINDOW_LAST) window_measure();
    return true;
}

/* Main thread: the window, hidden until init has its surface (show_window), in real pixels
   (SDL_WINDOW_HIGH_PIXEL_DENSITY: macOS's and Wayland's scaled displays), and its native handle. */
static void make_window(void *arg)
{
    WindowSpec *s = arg;
    /* The app id, the launcher's (tauri.conf.json's identifier): Wayland's app_id and X11's
       WM_CLASS, by which desktops group the window and find its icon (from n0buholic, PR #13).
       Windows: the icon (port/res/dhd.rc) on SDL's window class, small and big (window_icon). */
    SDL_SetHint(SDL_HINT_APP_ID, "app.skuza.downhillrevived");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "gs: SDL video did not start (%s)\n", SDL_GetError());
        return;
    }
    monitor_size(s);
    if (s->borderless) {  /* the whole primary monitor: SDL knows its bounds only once its video runs */
        SDL_Rect full, usable;
        display_of(NULL, &full, &usable);
        s->x = full.x, s->y = full.y, s->w = full.w, s->h = full.h;
    }
    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | (s->borderless ? SDL_WINDOW_BORDERLESS : SDL_WINDOW_RESIZABLE);
    /* A minimised start: SDL applies its window state when the window shows, so it must know
       (show_window); otherwise it restores the window then. */
    if (start_minimised()) flags |= SDL_WINDOW_MINIMIZED;
    if (FIXED_SCREEN) flags = SDL_WINDOW_FULLSCREEN;
    flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY | WINDOW_FLAGS;
    sdl_window = SDL_CreateWindow("Downhill Revived", s->w, s->h, flags);  /* s->w x s->h: the client area */
    if (!sdl_window) {
        fprintf(stderr, "gs: SDL window (%s)\n", SDL_GetError());
        return;
    }
    if (s->borderless) SDL_SetWindowPosition(sdl_window, s->x, s->y);
    window_icon();
    window_measure();
    SDL_AddEventWatch(window_watch, NULL);
    window_target(NULL);
    hle_log("gs: SDL window %dx%d (video %s)\n", s->w, s->h, SDL_GetCurrentVideoDriver());
}

/* A display's whole area and its usable part (without the taskbar), in pixels (host_dpi_aware). */
static void display_of(SDL_Window *w, SDL_Rect *full, SDL_Rect *usable)
{
    SDL_DisplayID id = w ? SDL_GetDisplayForWindow(w) : SDL_GetPrimaryDisplay();
    if (!SDL_GetDisplayBounds(id, full)) {
        full->x = full->y = 0; full->w = 896; full->h = 672;
    }
    if (!SDL_GetDisplayUsableBounds(id, usable)) *usable = *full;
}

/* Main thread: the game's window appears, in front, or minimised and without the focus when the
   game was started minimised (start_minimised). */
static void show_window(void *unused)
{
    (void)unused;
    int minimised = start_minimised();
    hle_log("gs: window shown%s\n", minimised ? " minimised (a minimised start)" : "");
    if (minimised) show_minimised();
    else { SDL_ShowWindow(sdl_window); SDL_RaiseWindow(sdl_window); }
}

/* The native handle, waited for up to tries x 10 ms (a phone makes its surface a little after the
   window, and again after the background). 1 when target holds it. */
static int wait_target(int tries)
{
    for (int i = 0; i < tries; i++) {
        sdl_host_run(window_target, NULL);
        if (target_ready) return 1;
        SDL_Delay(10);
    }
    return 0;
}

/* ---- the app in the background (phones: SDL sends these events on Android and iOS only) ---- */
static volatile int app_background; /* the app is in the background (lifecycle_watch): the game waits */
static volatile int surface_dropped; /* lifecycle_watch dropped the surface: app_background_wait makes it again */

/* SDL's app events, from the thread that pumps SDL's events (sdl_host.c), synchronously: SDL blocks
   that thread right after the background events (SDL_HINT_ANDROID_BLOCK_ON_PAUSE), so the queued
   copies arrive only after the app is back. Here the render thread stops presenting before the OS
   takes the window (on its own thread, soon after); the game thread waits at its next field. */
static bool SDLCALL lifecycle_watch(void *unused, SDL_Event *e)
{
    (void)unused;
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND && !app_background) {
        app_background = 1;
        if (surface) gs_gpu_surface_reset(&gs, NULL);
        surface_dropped = 1;
    } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        app_background = 0;
    }
    return true;
}

/* The app went to the background (game thread, at a field): the surface is dropped before the OS
   takes the window, and the game waits (SDL pauses the sound) until the app is back, then the
   surface is made on the new window. The close of the app ends the game as the close button does. */
static void app_background_wait(void)
{
    hle_log("gs: in the background; the game waits\n");
    gs_gpu_surface_reset(&gs, NULL);
    target_ready = 0;
    while (app_background && !window_closed) SDL_Delay(20);
    surface_dropped = 0;
    gs_pump_messages();
    if (!wait_target(300) || !gs_gpu_surface_reset(&gs, &target)) {
        hle_log("gs: no surface on the window after the background\n");
        target_ready = 0;
        return;
    }
    resize_pending = 1;
    hle_log("gs: back in the foreground\n");
}

/* The window (SDL's), then the renderer (gpu_rust.c: wgpu on Direct3D 12 or Vulkan, Metal on
   Apple; WARP when Windows has no graphics driver) and its surface on the window, which is shown
   once both run. */
static int init(void)
{
    host_dpi_aware();  /* real pixels: a borderless window at the monitor's own resolution (before SDL's video) */
    /* DHD_WINDOW: windowed (default; DHD_WINDOW_WIDTH x DHD_WINDOW_HEIGHT inside, 896x672),
       borderless or fullscreen (both the whole monitor: fill_monitor). A phone: the whole screen. */
    const char *mode = getenv("DHD_WINDOW"), *width = getenv("DHD_WINDOW_WIDTH"), *height = getenv("DHD_WINDOW_HEIGHT");
    int fullscreen = !FIXED_SCREEN && mode && !strcmp(mode, "fullscreen");
    int borderless = FIXED_SCREEN || (mode && !strcmp(mode, "borderless")) || fullscreen;
    WindowSpec spec = {0, 0, width && atoi(width) > 0 ? atoi(width) : 896, height && atoi(height) > 0 ? atoi(height) : 672,
                       borderless};
    sdl_host_run(make_window, &spec);  /* on the main thread: the window's messages are its queue's */
    if (!sdl_window) return 0;
    if (!target_ready && !wait_target(200)) { hle_log("gs: the window has no native handle\n"); return 0; }
    if (!gs_gpu_init(&gs)) return 0;
    if (!gs_gpu_surface(&gs, &target)) {
        hle_log("gs: the renderer has no surface on the window\n");
        gs_gpu_destroy(&gs);
        return 0;
    }
    surface = 1;
    SDL_AddEventWatch(lifecycle_watch, NULL);
    hle_log("gs: WebGPU presentation (%s, window kind %u)\n", gs_gpu_backend_name(&gs), target.kind);
    sdl_host_run(show_window, NULL);
    if (fullscreen) gs_window_apply();  /* the whole monitor */
    return 1;
}

/* Main thread: DHD_WINDOW, DHD_WINDOW_WIDTH and DHD_WINDOW_HEIGHT as init sets them up. */
static void window_apply(void *unused)
{
    (void)unused;
    const char *mode = getenv("DHD_WINDOW"), *width = getenv("DHD_WINDOW_WIDTH"), *height = getenv("DHD_WINDOW_HEIGHT");
    int whole = mode && (!strcmp(mode, "borderless") || !strcmp(mode, "fullscreen"));
    int minimised = (SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_MINIMIZED) != 0;  /* a minimised start stays so */
    if (whole) fill_monitor();
    else {
        int w = width && atoi(width) > 0 ? atoi(width) : 896, h = height && atoi(height) > 0 ? atoi(height) : 672;
        if (SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_FULLSCREEN) {  /* fill_monitor's, off Windows */
            SDL_SetWindowFullscreen(sdl_window, false);
            SDL_SyncWindow(sdl_window);
        }
        SDL_Rect monitor, usable;
        display_of(sdl_window, &monitor, &usable);
        SDL_SetWindowBordered(sdl_window, true);
        SDL_SetWindowSize(sdl_window, w, h);
        int top = 0, left = 0, bottom = 0, right = 0;
        SDL_GetWindowBordersSize(sdl_window, &top, &left, &bottom, &right);
        int outer_w = w + left + right, outer_h = h + top + bottom;  /* centred on the usable area */
        int x = usable.x + (usable.w - outer_w) / 2, y = usable.y + (usable.h - outer_h) / 2;
        SDL_SetWindowPosition(sdl_window, x > usable.x ? x : usable.x, y > usable.y ? y : usable.y);
    }
    if (!minimised) SDL_ShowWindow(sdl_window);
}

/* The PC settings page changed DHD_WINDOW, DHD_WINDOW_WIDTH or DHD_WINDOW_HEIGHT: the window
   mode and size now, on the main thread (the game thread waits). The renderer's surface follows
   at the next present. A phone's window is always the whole screen. */
void gs_window_apply(void)
{
    if (!sdl_window || !surface || FIXED_SCREEN) return;
    sdl_host_run(window_apply, NULL);
    resize_pending = 1;
}

/* The picture's filter and shape changed (DHD_PRESENT_FILTER, DHD_ASPECT): native renderer only. */
void gs_present_options_apply(void)
{
    if (sdl_window && gs_gpu_native(&gs)) gs_native_present_options(&gs);
}

/* The picture to save: the native renderer's own (upscaled with DHD_SCALE, so its size), else
   pixels[] (read back from the exact renderer when gpu_frame). */
static int grab(int gpu_frame, const uint32_t **image, unsigned *width, unsigned *height)
{
    if (gs_gpu_native(&gs)) return (*image = gs_native_picture(&gs, width, height)) != NULL;
    *image = pixels;
    return !gpu_frame || gs_gpu_capture(&gs, pixels);
}

static char published_capture_name[1024];
static int published_capture_ready;
void host_capture_name(uint32_t address, int ready) {
    published_capture_ready=ready;
    if (ready) snprintf(published_capture_name,sizeof published_capture_name,"%s",(const char *)GUEST(address));
}

static void capture(unsigned width, unsigned height, int gpu_frame)
{
    const uint32_t *image = pixels;
    static unsigned saved;
    /* Every branch below needs DHD_SCREENSHOT: without it, return before the getenvs (each scans
       the whole environment, six per field; finding 090 SP14). Nothing sets it at run time. */
    static int screenshot = -1;
    if (screenshot < 0) screenshot = getenv("DHD_SCREENSHOT") != NULL;
    if (!screenshot) return;
    const char *path = getenv("DHD_SCREENSHOT"), *at = getenv("DHD_CAPTURE_AT");
    const char *every = getenv("DHD_CAPTURE_INTERVAL");
    const char *frame_at = getenv("DHD_CAPTURE_FRAME");
    extern uint64_t kernel_vsync_count;
    double capture_time = frame_at ? (double)kernel_vsync_count : host_seconds();
    if (frame_at) {
        at = frame_at;
        every = getenv("DHD_CAPTURE_FRAME_INTERVAL");
    }
    double interval = every ? atof(every) : 0;
    char numbered[1024];
    /* DHD_CAPTURE_FIELDS=first-last[,first-last...]: every field of those windows, PATH-fNNNNNN.bmp. */
    const char *fields = getenv("DHD_CAPTURE_FIELDS");
    if (path && fields) {
        unsigned long long first, last;
        int hit = 0;
        for (const char *s = fields; s && sscanf(s, "%llu-%llu", &first, &last) == 2; s = strchr(s, ','), s = s ? s + 1 : s)
            hit |= kernel_vsync_count >= first && kernel_vsync_count <= last;
        if (!hit || !grab(gpu_frame, &image, &width, &height)) return;
        snprintf(numbered, sizeof numbered, "%s-f%06llu.bmp", path, (unsigned long long)kernel_vsync_count);
        path = numbered;
    } else if (getenv("DHD_RACE_SYNC")) {
        if (!path) return;
        if (published_capture_ready) snprintf(numbered,sizeof numbered,"%s",published_capture_name);
        if (!published_capture_ready) {
            /* Survey-only periodic pictures alongside the named race windows.
               DHD_CAPTURE_FRAME is otherwise intentionally ignored in race
               mode. Named consecutive captures always have priority. */
            const char *period = getenv("DHD_RACE_CAPTURE_INTERVAL");
            if (!period || strtoul(period, NULL, 10) == 0) return;
            static unsigned long long next;
            unsigned long long step = strtoul(period, NULL, 10);
            const char *first = getenv("DHD_RACE_CAPTURE_FROM");
            if (!next) next = first ? strtoull(first, NULL, 10) : step;
            if (kernel_vsync_count < next) return;
            next = (kernel_vsync_count / step + 1) * step;
            snprintf(numbered, sizeof numbered, "%s-f%06llu.bmp", path, (unsigned long long)kernel_vsync_count);
        }
        if (!grab(gpu_frame, &image, &width, &height)) return;
        path = numbered;
    } else {
        if (!path || (saved && interval <= 0) ||
            capture_time < (at ? atof(at) : 10.0) + saved * interval) return;
        if (!grab(gpu_frame, &image, &width, &height)) return;
        if (interval > 0) {
            snprintf(numbered, sizeof numbered, "%s-%03u.bmp", path, saved);
            path = numbered;
        }
        saved++;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "gs: cannot save screenshot %s\n", path); return; }
    /* The BMP headers, packed as the format requires. */
    struct __attribute__((packed)) {
        uint16_t bfType;
        uint32_t bfSize;
        uint16_t bfReserved1, bfReserved2;
        uint32_t bfOffBits;
    } file = {0};
    struct __attribute__((packed)) {
        uint32_t biSize;
        int32_t biWidth, biHeight;
        uint16_t biPlanes, biBitCount;
        uint32_t biCompression, biSizeImage;
        int32_t biXPelsPerMeter, biYPelsPerMeter;
        uint32_t biClrUsed, biClrImportant;
    } info = {0};
    _Static_assert(sizeof file == 14, "BMP file header is 14 bytes");
    _Static_assert(sizeof info == 40, "BMP info header is 40 bytes");
    file.bfType = 0x4D42; file.bfOffBits = sizeof file + sizeof info;
    file.bfSize = file.bfOffBits + width * height * 4;
    info.biSize = sizeof info; info.biWidth = width; info.biHeight = -(int32_t)height;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = 0;  /* BI_RGB */
    fwrite(&file, sizeof file, 1, f); fwrite(&info, sizeof info, 1, f);
    for (unsigned i = 0; i < width * height; i++) {
        uint32_t v = image[i], bgra = (v & 0xFF00FF00) | (v & 255) << 16 | (v >> 16 & 255);
        fwrite(&bgra, 4, 1, f);
    }
    fclose(f);
    fprintf(stderr, "compare: capture %s field=%llu host=%.6f game=%.6f\n", path,
            (unsigned long long)kernel_vsync_count, host_wall_seconds(), host_seconds());
    hle_log("gs: saved %s (%llu PATH1 / %llu PATH2 / %llu PATH3 packets, %llu kicks, %llu uploads)\n",
            path, gs.packets[0], gs.packets[1], gs.packets[2], gs.kicks, gs.uploads);
    hle_log("gs: PMODE=%llX SMODE2=%llX DISPFB1=%llX DISPLAY1=%llX DISPFB2=%llX DISPLAY2=%llX\n",
            gs_priv_read(&gs, GS_PMODE), gs_priv_read(&gs, GS_SMODE2),
            gs_priv_read(&gs, GS_DISPFB1), gs_priv_read(&gs, GS_DISPLAY1),
            gs_priv_read(&gs, GS_DISPFB2), gs_priv_read(&gs, GS_DISPLAY2));
}

volatile uint32_t gs_presented_frames;

volatile int gs_native_view;
void host_service_gs_native_view(int view) { gs_native_view = view; }

/* The title (online races: the lobby), set by the main thread's next pass (gs_window_tick): the
   game thread never waits for it, not even while the window is being dragged. */
static struct { SDL_SpinLock lock; int changed; char text[256]; } title;

void gs_window_title(const char *utf8)
{
    SDL_LockSpinlock(&title.lock);
    snprintf(title.text, sizeof title.text, "%s", utf8);
    title.changed = 1;
    SDL_UnlockSpinlock(&title.lock);
}

/* The window's events, from the SDL thread (sdl_host.c): the size changed (WM_SIZE: the back buffer
   follows at the next present), or the window was asked to close (WM_CLOSE: the game ends at the
   next gs_pump_messages). */
void gs_window_event(unsigned type)
{
    if (type == SDL_EVENT_WINDOW_RESIZED) resize_pending = 1;
    else if (type == SDL_EVENT_WINDOW_CLOSE_REQUESTED || type == SDL_EVENT_QUIT) {
        if (sdl_window) SDL_HideWindow(sdl_window);
        window_closed = 1;
    }
}

/* EXIT GAME (port/hle/exit_menu.c): the window closes as when its close button is clicked, and
   the game ends at the next gs_pump_messages. */
void gs_window_close(void)
{
    if (sdl_window) {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        e.window.windowID = SDL_GetWindowID(sdl_window);
        SDL_PushEvent(&e);  /* the SDL thread sees it: gs_window_event */
        return;
    }
    exit(0);
}

/* The window's events are the SDL thread's (sdl_host.c); here the game ends once the window closed. */
void gs_pump_messages(void)
{
    if (window_closed) exit(0);
}

/* The window's drawable size in pixels (the renderer's GsWindow.size, any thread): 0 x 0 without a
   window or while it is minimised. */
void gs_window_pixels(unsigned *width, unsigned *height)
{
    unsigned size = (unsigned)SDL_GetAtomicInt(&drawable);
    *width = size >> 16;
    *height = size & 0xFFFF;
}

/* ---- the mouse (finding 092) ----
   The SDL thread passes the window's mouse events (touch arrives as SDL's mouse events too); the
   module reads HostMouse (host_service_mouse_state) once a field. Positions are fractions of the
   picture where the renderer last showed it (gs_picture_rect: the bars beside it are outside), so
   the window's size, the picture's shape and DHD_SCALE don't matter to the menus. The OS cursor
   shows while the mouse moves, and hides after a few idle seconds and while a race is ridden. */
#define MOUSE_LOCK() SDL_LockSpinlock(&mouse.lock)
#define MOUSE_UNLOCK() SDL_UnlockSpinlock(&mouse.lock)
static struct {
    SDL_SpinLock lock;
    int left, top, right, bottom, w, h;  /* the picture in the window, pixels (0 x 0: the whole window) */
    float px, py;                        /* the pointer, window pixels */
    float before_x, before_y;            /* test desktops: where it was before the last motion, */
    float os_x, os_y;                    /* and where SDL puts the OS cursor (learnt at the first leave) */
    int dropped;                         /* the last motion went there and was dropped */
    HostMouse state;
    float wheel;                         /* wheel motion not yet a whole step */
    double moved;                        /* wall time of the last move or press */
} mouse;

void gs_picture_rect(int left, int top, int right, int bottom, int w, int h)
{
    if (right <= left || bottom <= top) return;
    MOUSE_LOCK();
    mouse.left = left, mouse.top = top, mouse.right = right, mouse.bottom = bottom, mouse.w = w, mouse.h = h;
    MOUSE_UNLOCK();
}

/* The picture's rect in the window (pixels) and the window's pixel size, as gs_picture_rect last
   set them: touch.c (Android) maps its finger positions into the picture with this, so its layout
   holds whatever the bars beside the picture or the scale. 0 when the renderer hasn't reported one
   yet. */
int gs_picture_rect_get(int *left, int *top, int *right, int *bottom, int *w, int *h)
{
    MOUSE_LOCK();
    int ok = mouse.right > mouse.left && mouse.bottom > mouse.top && mouse.w > 0 && mouse.h > 0;
    *left = mouse.left, *top = mouse.top, *right = mouse.right, *bottom = mouse.bottom;
    *w = mouse.w, *h = mouse.h;
    MOUSE_UNLOCK();
    return ok;
}

/* The pointer as fractions of the picture (65536 its width / height); lock held. */
static void mouse_place(void)
{
    int left = mouse.left, top = mouse.top, right = mouse.right, bottom = mouse.bottom;
    float px = mouse.px, py = mouse.py;
    int ww = 0, wh = 0;
    if (sdl_window && SDL_GetWindowSizeInPixels(sdl_window, &ww, &wh) && ww > 0 && wh > 0) {
        if (right <= left || mouse.w <= 0) left = top = 0, right = ww, bottom = wh;
        else px *= (float)mouse.w / ww, py *= (float)mouse.h / wh;  /* a resize the picture hasn't followed yet */
    }
    if (right <= left || bottom <= top) return;
    mouse.state.x = (int32_t)((px - left) * 65536.0f / (right - left));
    mouse.state.y = (int32_t)((py - top) * 65536.0f / (bottom - top));
}

/* DHD_TEST_INPUT (test runs on a desktop that is never shown, driven by posted messages): the
   OS cursor is never over the window there, so Windows reports a leave after each posted move:
   SDL first moves its pointer to where the OS cursor is (taken back at the leave event, which
   also tells where that is), and before the next press it resyncs the pointer there (dropped).
   Presses keep the posted move's point (SDL gives them its own idea of the pointer). So a test's
   moves and clicks act as a real mouse's would. */
void gs_mouse_event(const void *event)
{
    static int test_input = -1;
    const SDL_Event *e = event;
    if (test_input < 0) test_input = getenv("DHD_TEST_INPUT") != NULL;
    float density = sdl_window ? SDL_GetWindowPixelDensity(sdl_window) : 1.0f;
    if (density <= 0) density = 1.0f;
    MOUSE_LOCK();
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (test_input && e->motion.x == mouse.os_x && e->motion.y == mouse.os_y) {  /* to the OS cursor */
            mouse.dropped = 1;
            break;
        }
        mouse.dropped = 0;
        mouse.before_x = mouse.px, mouse.before_y = mouse.py;
        mouse.px = e->motion.x * density, mouse.py = e->motion.y * density;
        mouse_place();
        mouse.state.moves++;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int index = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1
                  : e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (!test_input) {  /* a test's press is where its posted move put the pointer (above) */
            mouse.px = e->button.x * density, mouse.py = e->button.y * density;
            mouse_place();
        }
        if (index < 0) break;
        if (e->button.down) mouse.state.buttons |= 1u << index, mouse.state.presses[index]++;
        else mouse.state.buttons &= ~(1u << index);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        mouse.wheel += e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
        int steps = (int)mouse.wheel;
        mouse.state.wheel += steps;
        mouse.wheel -= (float)steps;
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:  /* test desktops: the motion before it is taken back */
        if (test_input && !mouse.dropped) {  /* its motion got through: learn the point, take it back */
            mouse.os_x = mouse.px / density, mouse.os_y = mouse.py / density;
            mouse.px = mouse.before_x, mouse.py = mouse.before_y;
            mouse_place();
        }
        MOUSE_UNLOCK();
        return;

    default:
        MOUSE_UNLOCK();
        return;
    }
    mouse.state.seen = 1;
    mouse.moved = host_wall_seconds();
    MOUSE_UNLOCK();
}

/* SDL thread, every pass of its loop: a new title (gs_window_title); text input while a text
   field is open; the cursor shows for 3 s after the mouse was used, never while a race is ridden
   (the game's controls in riding mode, D_0029E290). */
void gs_window_tick(void)
{
    static int shown = -1;
    if (!sdl_window) return;
    if (title.changed) {
        char text[sizeof title.text];
        SDL_LockSpinlock(&title.lock);
        memcpy(text, title.text, sizeof text);
        title.changed = 0;
        SDL_UnlockSpinlock(&title.lock);
        SDL_SetWindowTitle(sdl_window, text);
    }
    /* SDL3 sends typed text (SDL_EVENT_TEXT_INPUT, sdl_host.c) only to a window that takes it:
       while one of the game's text fields is open (text_input_begin/end, the game thread: the
       rider name, the join code), from here, as SDL wants its video calls on this thread. */
    static int typing;
    if (text_input_active() != typing) {
        typing = !typing;
        if (typing) SDL_StartTextInput(sdl_window);
        else SDL_StopTextInput(sdl_window);
        fprintf(stderr, "text input: %s (SDL %s)\n", typing ? "on" : "off",
                SDL_TextInputActive(sdl_window) ? "sends typed text" : "sends no text");
    }
    int riding = *(volatile int32_t *)GUEST(0x0829E290) != 0;
    MOUSE_LOCK();
    int want = mouse.state.seen && !riding && host_wall_seconds() - mouse.moved < 3.0;
    MOUSE_UNLOCK();
    if (want == shown) return;
    shown = want;
    if (want) SDL_ShowCursor();
    else SDL_HideCursor();
}

void host_service_mouse_state(uint32_t out)
{
    MOUSE_LOCK();
    *(HostMouse *)GUEST(out) = mouse.state;
    MOUSE_UNLOCK();
}

/* DHD_BENCH=first-last times the wall clock between consecutive fields of that virtual-field
   window (run with DHD_FRAME_CLOCK=1 for an unpaced measurement) and prints one summary line
   at the last field; DHD_BENCH_EXIT=1 then ends the process. */
static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

void snd_host_bench(int end);  /* port/hle/sound.c: audio output counters */
void graphics_timeline(int phase, unsigned field, double wall_seconds);  /* DHD_TIMELINE (graphics_queue.c) */

/* CPU time of each named port thread over the window, in ms per field, and of the whole process. */
static void bench_threads(int end, unsigned fields, double wall)
{
    enum { MAX = 16 };
    static const char *names[MAX];
    static double before[MAX], process_before;
    static unsigned count;
    double now[MAX];
    const char *now_names[MAX];
    double host_process_cpu(void);  /* port/hle/host.c */
    double process = host_process_cpu();
    unsigned n = host_thread_cpu(now_names, now, MAX);
    if (!end) {
        count = n; process_before = process;
        for (unsigned i = 0; i < n; i++) { names[i] = now_names[i]; before[i] = now[i]; }
        return;
    }
    fprintf(stderr, "bench threads: cpu_ms_per_field");
    for (unsigned i = 0; i < n; i++) {
        double start = 0;
        for (unsigned j = 0; j < count; j++) if (names[j] == now_names[i]) start = before[j];
        fprintf(stderr, " %s=%.3f", now_names[i], (now[i] - start) * 1e3 / fields);
    }
    fprintf(stderr, " process=%.3f busy_cores=%.2f\n", (process - process_before) * 1e3 / fields,
            (process - process_before) / wall);
}

static void bench_vsync(void)
{
    static int state;  /* 0 unread, 1 armed, 2 done */
    static unsigned long long first, last;
    static double start, previous, *frames;
    extern uint64_t kernel_vsync_count;
    if (state == 2) return;
    if (!state) {
        const char *window = getenv("DHD_BENCH");
        if (!window || sscanf(window, "%llu-%llu", &first, &last) != 2 || last <= first) { state = 2; return; }
        frames = calloc(last - first, sizeof *frames);
        if (!frames) { state = 2; return; }
        state = 1;
    }
    unsigned long long field = kernel_vsync_count;
    if (field < first) return;
    double now = host_wall_seconds();
    if (field == first) {
        if (graphics_deferred()) graphics_wait();  /* gs_native_bench reads the GS worker's renderer */
        start = previous = now; gs_native_bench(&gs, 0); snd_host_bench(0); bench_threads(0, 0, 0);
        graphics_timeline(0, (unsigned)field, 0);
        return;
    }
    if (field > last) { state = 2; return; }  /* started inside the window: no clean measurement */
    frames[field - first - 1] = now - previous;
    graphics_timeline(field < last ? 1 : 2, (unsigned)field, now - previous);
    previous = now;
    if (field < last) return;
    if (graphics_deferred()) graphics_wait();
    gs_native_bench(&gs, 1);
    snd_host_bench(1);
    unsigned n = (unsigned)(last - first), worst = n / 100 ? n / 100 : 1;
    bench_threads(1, n, now - start);
    qsort(frames, n, sizeof *frames, compare_double);
    double tail = 0;
    for (unsigned i = n - worst; i < n; i++) tail += frames[i];
    fprintf(stderr, "bench: fields=%u-%llu fields_per_s=%.3f p50_ms=%.3f p99_ms=%.3f worst1pct_ms=%.3f max_ms=%.3f\n",
            (unsigned)first, last, n / (now - start), frames[n / 2] * 1e3, frames[n * 99 / 100] * 1e3,
            tail / worst * 1e3, frames[n - 1] * 1e3);
    fflush(stderr);
    free(frames);
    state = 2;
    const char *quit = getenv("DHD_BENCH_EXIT");
    if (quit && !strcmp(quit, "1")) host_exit(0);
}

const uint32_t *movie_picture(unsigned *width, unsigned *height);  /* port/hle/movie.c */
int host_widescreen_flat_limit(void);  /* port/hle/host.c */

/* The graphics didn't start. The game would go on with no window (heard, never seen): instead a
   message says why and the game ends. Tools that want no window set DHD_HEADLESS and never get here. */
static void graphics_failed(void)
{
    hle_log("gs: the graphics didn't start\n");
    const char *message = "Downhill Revived couldn't start its graphics on this computer.\n\n"
                          "Its renderer needs Direct3D 12 or Vulkan (Windows 10 or 11), Metal (macOS) or Vulkan "
                          "(Linux, Android), with a current graphics driver. On a Windows laptop with two graphics "
                          "chips, open Windows Settings > System > Display > Graphics, add the game and choose High "
                          "performance.\n\nThe details are in dhd.log, next to the game.";
    host_error_box("Downhill Revived", message);
    exit(1);
}

/* Plain MMIO pages are the authoritative privileged register storage: the GS takes them at each
   picture. Then counts changed display buffers, excluding repeated interlaced fields (the game's
   menu counter does not cover every track-intro render path). */
static void present_registers(const uint64_t *mmio)
{
    static unsigned previous_bp = ~0u;
    for (unsigned addr = 0; addr <= 0xE0; addr += 16)
        gs_priv_write(&gs, addr, mmio[addr / 16]);
    uint64_t pmode = gs_priv_read(&gs, GS_PMODE);
    unsigned bp = (unsigned)gs_priv_read(&gs, pmode & 1 ? GS_DISPFB1 : GS_DISPFB2) & 511;
    if ((pmode & 3) && bp != previous_bp) { gs_presented_frames++; previous_bp = bp; }
}

/* A native picture handed on in order with the drawing (deferred vsync, graphics_queue.c): the
   register page as the VBLANK handlers left it, and what the game thread knew at its vsync. */
typedef struct {
    uint64_t mmio[15];
    double due, began;  /* pacing_handed */
    int32_t resize, flat_limit;
} PresentMark;

/* The GS owner, once the drawing queued before the picture has run. */
static void present_marked(const void *payload)
{
    PresentMark m;
    unsigned width, height;
    memcpy(&m, payload, sizeof m);
    present_registers(m.mmio);
    gs_native_movie(&gs, NULL, 0, 0);
    gs_native_vsync_handed(&gs, &width, &height, m.resize, m.due, m.began, m.flat_limit);
}

void gs_vsync(void)
{
    static int profile;
    static int cpu_scanout;
    static double profile_start;
    static unsigned fields, previous_frames, previous_presented;
    if (!initialized) {
        if (graphics_deferred()) graphics_wait();  /* the renderer starts with the GS worker idle */
        initialized = 1;
        disabled = getenv("DHD_HEADLESS") != NULL;
        if (!disabled && !init()) graphics_failed();
        profile = getenv("DHD_PROFILE") != NULL;
        cpu_scanout = getenv("DHD_GS_CPU_SCANOUT") != NULL;
        profile_start = host_wall_seconds();
    }
    bench_vsync();
    gs_pump_messages();  /* first: the close button ends the game even with no renderer */
    if (disabled) return;
    if (app_background || surface_dropped) app_background_wait();
#ifdef __ANDROID__
    { void android_perf_field(void); android_perf_field(); }  /* port/host/android_perf.c: CPU clocks */
#endif
    uint64_t mmio[15];
    for (unsigned addr = 0; addr <= 0xE0; addr += 16) mmio[addr / 16] = *(volatile uint64_t *)GUEST(0x12000000 + addr);
    /* While a movie plays (port/hle/movie.c, finding 055) its picture is shown instead. */
    unsigned movie_width, movie_height;
    const uint32_t *movie = movie_picture(&movie_width, &movie_height);
    /* Deferred vsync: the GS worker hands the native picture on after the drawing queued before it;
       the game thread goes on. A movie or the exact renderer waits for the drawing here instead.
       (Capture and DHD_PROFILE turn deferral off: they read the GS below.) */
    if (graphics_deferred()) {
        if (!movie && gs_gpu_native(&gs)) {
            PresentMark m;
            memcpy(m.mmio, mmio, sizeof m.mmio);
            pacing_handed(&m.due, &m.began);
            m.resize = resize_pending, m.flat_limit = host_widescreen_flat_limit();
            resize_pending = 0;
            graphics_present_marked = present_marked;
            graphics_present(&m, sizeof m);
            return;
        }
        graphics_wait();
    }
    present_registers(mmio);
    unsigned width = 640, height = 448;
    int gpu_frame = 1;
    if (gs_gpu_native(&gs)) {
        /* The native renderer's thread draws and presents; capture waits for its picture. */
        gs_native_movie(&gs, movie, movie_width, movie_height);
        gs_native_vsync(&gs, &width, &height, resize_pending);
        resize_pending = 0;
    } else {
        gpu_frame = !cpu_scanout && gs_gpu_display(&gs, &width, &height);
        if (movie) {
            gpu_frame = 0;
            width = movie_width, height = movie_height;
            memcpy(pixels, movie, (size_t)width * height * 4);
        } else if (!gpu_frame) {
            if (!gs_display(&gs, pixels, &width, &height)) memset(pixels, 0, width * height * 4);
        }
        /* the renderer shows the picture itself, stretched over the client area */
        unsigned client_width, client_height;
        gs_window_pixels(&client_width, &client_height);
        double due, began;
        pacing_handed(&due, &began);
        pacing_present(due, began);
        if (!gs_gpu_present(&gs, gpu_frame ? NULL : pixels, width, height, client_width, client_height)) {
            fprintf(stderr, "gs: WebGPU presentation failed\n");
            disabled = 1;
        }
    }
    capture(width, height, gpu_frame);
    gs_capture_vsync(&gs);
    if (profile) {
        fields++;
        double now = host_wall_seconds(), elapsed = now - profile_start;
        if (elapsed >= 5) {
            unsigned frames = *(volatile unsigned *)GUEST(0x0829E10C);
            if (frames >= previous_frames)
                fprintf(stderr, "profile: %.2f fields/s, %.2f menu frames/s, %.2f display-buffer changes/s\n", fields / elapsed,
                        (frames - previous_frames) / elapsed, (gs_presented_frames - previous_presented) / elapsed);
            else fprintf(stderr, "profile: %.2f fields/s, frame counter reset\n", fields / elapsed);
            gs_gpu_profile(&gs, elapsed);
            previous_frames = frames; fields = 0; profile_start = now;
            previous_presented = gs_presented_frames;
        }
    }
}

/* Guest-address-only module boundary; native device code stays above. */
#include "../host/host_api.h"
#include "../include/guest.h"
void host_service_gs_vsync(void) { gs_vsync(); }
void host_service_gs_pump_messages(void) { gs_pump_messages(); }
void host_service_gs_window_title(uint32_t a0) { gs_window_title((const char *)GUEST(a0)); }
void host_service_gs_window_apply(void) { gs_window_apply(); }
void host_service_gs_present_options_apply(void) { gs_present_options_apply(); }
void host_service_gs_window_close(void) { gs_window_close(); }
