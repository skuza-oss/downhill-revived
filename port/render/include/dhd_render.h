/* The Rust GS renderer (port/render): its C ABI, version 1.

   Natively these are symbols of dhd_render.lib (x86_64-pc-windows-msvc). In the browser they are
   imports of module "dhd_render", satisfied by the Rust module's raw exports wasm-to-wasm (no JS
   frame in between). The i686 build does not use this library (it stays on the C renderer); the
   header still compiles there, and its layouts are checked there too.

   Rules every function keeps:
   - Rust reads no environment. C reads every DHD_* variable and passes flags.
   - Every export saves MXCSR, forces IEEE (the 0x1F80 class: round to nearest, no FTZ/DAZ, every
     exception masked) and restores the caller's MXCSR on return. On wasm there is nothing to save.
   - No callbacks into C except dhd_host_log, no function pointers, no environment, no ID3D11 type.
   - A panic or an uncaptured device error is logged through dhd_host_log, then the process aborts.
   - Each DhdrExact, DhdrNative and DhdrSurface is used by one thread at a time; a second entry
     while one runs aborts and names both exports. DhdrTexture functions are safe from any thread.

   Every struct below is #[repr(C)] in src/abi.rs. Layouts are frozen: tests/abi_layout.rs checks
   them in Rust, and port/tools/test_dhdr_abi.py compiles port/gs/test_dhdr_abi.c (this header) and
   src/abi.rs for i686, x86_64 and wasm32 and requires every size, alignment, offset and constant
   to agree. No struct has implicit padding on any of those targets: the pad fields are explicit
   and C sets them to zero. Pointer members come last, so the bytes before them are the same on
   every target. Any change after the freeze bumps DHDR_ABI_VERSION. */
#ifndef DHD_RENDER_H
#define DHD_RENDER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DHDR_ABI_VERSION 1u

#if defined(__wasm__)
#define DHDR_FN(name) __attribute__((import_module("dhd_render"), import_name(#name)))
#else
#define DHDR_FN(name)
#endif

#if !defined(__wasm__)
/* Provided BY C (gpu_rust.c): one UTF-8 line of `bytes` bytes, not NUL-terminated, no newline.
   hle_log + stderr, dhd.log flushed. Any thread. Never called on wasm (Rust logs through its
   wasm-bindgen console bridge there). */
void dhd_host_log(const char *line, size_t bytes);
#endif

/* ---------------- device: one per process, never released ---------------- */

enum { DHDR_DEV_VULKAN = 1, DHDR_DEV_FXC = 2, DHDR_DEV_DEBUG = 4, DHDR_DEV_VALIDATION = 8,
       DHDR_DEV_FALLBACK = 16, DHDR_DEV_NO_DUAL_SOURCE = 32 };
enum { DHDR_LOG_ERROR = 1, DHDR_LOG_WARN = 2, DHDR_LOG_INFO = 3 };
enum { DHDR_FEAT_DUAL_SOURCE = 1, DHDR_FEAT_RENDER_PIPELINES = 2 };
typedef struct DhdrDeviceDesc {
    uint32_t size;                   /* sizeof(DhdrDeviceDesc) */
    uint32_t abi;                    /* DHDR_ABI_VERSION; init fails (logged) on another size or abi */
    uint32_t flags;                  /* DHDR_DEV_*: DHD_WGPU_BACKEND=vulkan, DHD_WGPU_COMPILER=fxc,
                                        DHD_WGPU_DEBUG (DEBUG | VALIDATION), WARP adapter (FALLBACK),
                                        DHD_WGPU_DUAL_SOURCE=0 (NO_DUAL_SOURCE) */
    uint32_t vendor_id, device_id;   /* PCI ids of the presentation adapter; 0, 0: high performance */
    uint32_t log_level;              /* DHDR_LOG_*: the most verbose wgpu/naga level forwarded */
    const char *dxc_path;            /* UTF-8 path of dxcompiler.dll beside the exe, or NULL (FXC) */
} DhdrDeviceDesc;
/* 1: the device exists (later calls return 1 at once, whatever d holds); 0: failed (logged), and
   every later call returns 0 too. Native: blocks until the device exists. Browser: the device is
   made by the async dhdr_web_init (a wasm-bindgen export JS calls), and this returns whether it
   succeeded. */
DHDR_FN(dhdr_device_init)     int dhdr_device_init(const DhdrDeviceDesc *d);
/* The adapter's name, NUL-terminated, valid for the process ("" before init). */
DHDR_FN(dhdr_adapter_name)    const char *dhdr_adapter_name(void);
/* DHDR_FEAT_*: DUAL_SOURCE = dual-source blending offered and not refused by the flags;
   RENDER_PIPELINES = a probe render pipeline was built. 0 before init. */
DHDR_FN(dhdr_device_features) uint32_t dhdr_device_features(void);
/* "dhd_render 1.0 wgpu 29.0.3 core 29.0.3 naga 29.0.3", NUL-terminated, static. */
DHDR_FN(dhdr_version)         const char *dhdr_version(void);

/* ---------------- exact: raster.wgsl compute ----------------
   One DhdrExact per Gs, used by whichever thread runs gs_feed, one at a time. The calls are the
   device ops of port/gs/gpu_exact.inc's op_* seam, in its order (format: port/render/OPTRACE.md). */
typedef struct DhdrExact DhdrExact;
enum { DHDR_B_VRAM, DHDR_B_SNAPSHOT, DHDR_B_STATES, DHDR_B_PRIMITIVES, DHDR_B_BINS, DHDR_B_DISPLAY,
       DHDR_B_COPY, DHDR_B_READBACK, DHDR_B_STAGED, DHDR_B_BATCHES, DHDR_B_UPLOAD, DHDR_B_COUNT };
enum { DHDR_P_LOCAL_COPY = 1, DHDR_P_SCANOUT = 2 };
enum { DHDR_RASTER_EARLY_DEPTH = 1, DHDR_RASTER_ADDRESS_SHIFT = 2, DHDR_RASTER_EXCLUSIVE_WORD_STORE = 4 };
typedef struct DhdrExactStats {
    uint64_t submits, reads, read_ticks, dispatches, bytes_written, copies;
} DhdrExactStats;
/* Creates the 11 buffers (zeroed) and ALL exact pipelines eagerly, inside the MXCSR guard.
   NULL on failure (logged). */
DHDR_FN(dhdr_exact_create)  DhdrExact *dhdr_exact_create(uint32_t raster_flags);
/* Submits pending work first. NULL is ignored. */
DHDR_FN(dhdr_exact_destroy) void dhdr_exact_destroy(DhdrExact *x);
/* Queue write of bytes into buffer at offset: lands before the next submitted command buffer. */
DHDR_FN(dhdr_exact_write)   void dhdr_exact_write(DhdrExact *x, uint32_t buffer, uint32_t offset,
                                                  const void *data, uint32_t bytes);
/* Ends the open compute pass, then records a buffer-to-buffer copy. */
DHDR_FN(dhdr_exact_copy)    void dhdr_exact_copy(DhdrExact *x, uint32_t from, uint32_t from_offset,
                                                 uint32_t to, uint32_t to_offset, uint32_t bytes);
/* main over one batch record at dynamic_offset (a multiple of 256), in the open pass (opened if
   needed). */
DHDR_FN(dhdr_exact_batch)   void dhdr_exact_batch(DhdrExact *x, uint32_t dynamic_offset,
                                                  uint32_t groups_x, uint32_t groups_y);
/* Ends the open pass and dispatches pipeline (DHDR_P_*) in a pass of its own. SCANOUT:
   x_word = groups | width << 16, y_word = groups | height << 16 (they size the display texture). */
DHDR_FN(dhdr_exact_record)  void dhdr_exact_record(DhdrExact *x, uint32_t pipeline, uint32_t x_word, uint32_t y_word);
/* Ends the pass and submits the open encoder (nothing recorded: nothing happens). */
DHDR_FN(dhdr_exact_submit)  void dhdr_exact_submit(DhdrExact *x);
/* A read is begin, [browser: C awaits its own Suspending import web_gpu_await()], finish.
   buffer is DHDR_B_READBACK or DHDR_B_STAGED. want is a page bitmap: page p (8192 bytes, 512
   pages) is bit p & 31 of want[p >> 5].
   begin: submits, then starts mapping buffer over the span from the first to the last set page.
          It only maps: it never copies. C has already put the pages there through
          dhdr_exact_copy (right before for READBACK, earlier for STAGED).
          Native: blocks until mapped. Browser: returns at once; JS fetches the promise through
          the wasm-bindgen export dhdr_web_pending(). 0 = could not start (logged).
   finish: copies every set page to image + page * 8192 and unmaps. 0 = the map failed (logged;
          C exits: gpu_rust.c's op_read). Pages not set in want are left as they are.
   Between begin and finish no other dhdr_exact_* call is made on x. */
DHDR_FN(dhdr_exact_read_begin)  int dhdr_exact_read_begin(DhdrExact *x, uint32_t buffer, const uint32_t want[16]);
DHDR_FN(dhdr_exact_read_finish) int dhdr_exact_read_finish(DhdrExact *x, uint32_t buffer, const uint32_t want[16],
                                                           void *image);
/* The display texture (the last scanout). begin: submits, starts mapping, writes its size and
   returns 1; 0 = there is none (w, h set to 0). finish writes w * h RGBA8 pixels, rows unpadded;
   0 = the map failed. Same await rule and same no-other-call rule as the read. */
DHDR_FN(dhdr_exact_capture_begin)  int dhdr_exact_capture_begin(DhdrExact *x, uint32_t *w, uint32_t *h);
DHDR_FN(dhdr_exact_capture_finish) int dhdr_exact_capture_finish(DhdrExact *x, uint32_t *pixels);
DHDR_FN(dhdr_exact_stats)   void dhdr_exact_stats(const DhdrExact *x, DhdrExactStats *out);

/* ---------------- surfaces and presentation ---------------- */

typedef struct DhdrSurface DhdrSurface;
enum { DHDR_WINDOW_WIN32 = 1, DHDR_WINDOW_WEB_CANVAS = 2, DHDR_WINDOW_XLIB = 3, DHDR_WINDOW_WAYLAND = 4,
       DHDR_WINDOW_ANDROID = 5, DHDR_WINDOW_METAL_LAYER = 6 };   /* 2: dhdr_web_surface only; each native build
                                                                   takes its platform's: Windows 1, Linux 3 and 4,
                                                                   Android 5, Apple 6 (others refused: NULL) */
typedef struct DhdrWindow {
    uint32_t kind;                   /* DHDR_WINDOW_* */
    uint32_t reserved;               /* 0 */
    uint64_t a, b;                   /* WIN32: a = HWND, b = HINSTANCE; XLIB: a = Display *, b = Window;
                                        WAYLAND: a = wl_display *, b = wl_surface *; ANDROID: a =
                                        ANativeWindow *; METAL_LAYER: a = CAMetalLayer * */
} DhdrWindow;
enum { DHDR_PRESENT_NO_TEARING = 1, DHDR_PRESENT_VSYNC = 2 };   /* DHD_TEARING=0; VSYNC forces Fifo (DHD_INTERP) */
enum { DHDR_SURFACE_COPY_SRC = 1 };
/* A surface on the window: RGBA8, else BGRA8 (whichever the surface offers first in that order),
   Immediate > Mailbox > Fifo (VSYNC: Fifo; NO_TEARING: no Immediate), frame latency 1, opaque.
   NULL: unsupported kind or no surface (logged). */
DHDR_FN(dhdr_surface_create)  DhdrSurface *dhdr_surface_create(const DhdrWindow *w, uint32_t present_flags);
DHDR_FN(dhdr_surface_destroy) void dhdr_surface_destroy(DhdrSurface *s);   /* NULL is ignored */
/* DHDR_SURFACE_COPY_SRC: the surface textures can be copied from (the test hook reads them). */
DHDR_FN(dhdr_surface_usage)   uint32_t dhdr_surface_usage(const DhdrSurface *s);
/* An exact frame into the surface, configured to window_w x window_h (reconfigured when that
   changes, or when the surface is outdated or lost): pixels (a movie or a CPU scanout, w * h
   RGBA8, w * h <= 1024 * 1024) or, when pixels is NULL, x's display texture.
   1 = acquired and drawn: C calls pacing_present, then dhdr_surface_present;
   0 = skipped (minimised, timeout, no display texture yet); nothing to present;
   -1 = failed (logged): C stops presenting through this surface.
   Never blocks on vsync and never suspends. */
DHDR_FN(dhdr_present_exact)   int dhdr_present_exact(DhdrSurface *s, DhdrExact *x, const uint32_t *pixels,
                                                     uint32_t w, uint32_t h, uint32_t window_w, uint32_t window_h);
/* Presents the texture the last call that returned 1 drew (SurfaceTexture::present). */
/* Live presenter overlay; ping < 0 hides the online line, seconds is monotonic wall time. */
DHDR_FN(dhdr_surface_counter) void dhdr_surface_counter(DhdrSurface *s, int enabled, int ping, double seconds);
DHDR_FN(dhdr_surface_present) void dhdr_surface_present(DhdrSurface *s);

/* ---------------- native renderer, GPU half ----------------
   C (native_core.inc) records each field's commands; the render thread hands each picture over
   as a DhdrPicture (built by port/gs/native_snapshot.inc).
   DhdrNative and its surfaces: the native render thread only (under native_core's context_lock).
   DhdrTexture create/retain/release: ANY thread (GS worker, texpack loaders, render thread).
   DhdrNative never uses a DhdrExact. */
typedef struct DhdrNative DhdrNative;
/* Reference counted; created with one reference. Immutable after creation. */
typedef struct DhdrTexture DhdrTexture;

typedef struct DhdrVertex {          /* == native_core.inc's NativeVertex */
    float x, y, z, s, t, q, fog;
    uint32_t rgba;
} DhdrVertex;
typedef struct DhdrNativeConstants { /* == NativeConstants; native.wgsl's uniform block */
    float target[2];
    uint32_t flags, atst, aref;
    float alpha_scale, texa_alpha;
    uint32_t scale;                  /* C passes it as recorded; Rust sets target, scale when drawn */
    float fog[4];
    int32_t tex_offset[2];
    uint32_t tex_size[2], wrap[4], region[4];
} DhdrNativeConstants;
typedef struct DhdrConvertConstants { /* == NativeConvertConstants; native_convert's uniform block */
    uint32_t tex[4], size[4];
    uint32_t slot[4][4];             /* per source surface: page, bw, 1 for depth, Z units (float bits) */
    uint8_t pages[512];              /* per GS page: 0 the decoded base, 1-4 a source slot */
    uint32_t palette[256];
} DhdrConvertConstants;

enum { DHDR_CMD_DRAW, DHDR_CMD_COPY, DHDR_CMD_IMPORT, DHDR_CMD_ALPHA_UPLOAD };   /* == NATIVE_* */
enum { DHDR_NO_INDEX = -1 };         /* an int16 surface or convert index that is absent */
/* One recorded command: native_core.inc's NativeCommand, reordered so the pointers come last. */
typedef struct DhdrCommand {
    uint8_t kind, depth_key, color_mask, pad;   /* DHDR_CMD_*; pad 0 */
    int16_t target, depth, texture_surface;     /* surface indices, or DHDR_NO_INDEX */
    int16_t convert;                 /* the picture's conversion this draw samples, or DHDR_NO_INDEX */
    uint32_t blend_key, blend_fix;   /* blend_key = 1 | src << 1 | dst << 6 | op << 11, D3D11 numbering */
    int32_t scissor[4];              /* GS pixels: left, top, right, bottom ([left, right) x [top, bottom)) */
    DhdrNativeConstants constants;
    uint32_t first, count;           /* DRAW: vertices [first, first + count) of the picture */
    int16_t source, pad2;            /* COPY: source surface; pad2 0 */
    int32_t x, y, w, h, sx, sy;      /* COPY: source (sx, sy) to (x, y), w x h; IMPORT: w x h at (x, y);
                                        ALPHA_UPLOAD: the 64 x 32 page at (x, y) */
    DhdrTexture *texture;            /* DRAW: the decoded texture, or NULL */
    DhdrTexture *replacement;        /* DRAW with flag 2048: drawn instead of texture, or NULL */
    const uint32_t *pixels;          /* IMPORT: w * h RGBA8; ALPHA_UPLOAD: 64 * 32 RGBA8; else NULL */
} DhdrCommand;
typedef struct DhdrConvert {
    int16_t surface[4];              /* source surfaces of slots 1-4, or DHDR_NO_INDEX */
    DhdrConvertConstants constants;  /* tex[3] as recorded; Rust sets each slot's scale byte */
    DhdrTexture *base;               /* the texture decoded from the CPU copy, or NULL */
} DhdrConvert;
typedef struct DhdrSurfaceInfo {     /* native_core.inc's NativeSurface, as the picture sees it */
    uint32_t page, bw, wide, depth, zpsm;
    uint32_t scale;                  /* 0 until it has a texture */
    uint32_t size[2];                /* its texture (scaled pixels), 0 x 0 when it has none */
    uint32_t extent[2];              /* GS pixels claimed so far */
} DhdrSurfaceInfo;
typedef struct DhdrPicture {
    uint32_t size;                   /* sizeof(DhdrPicture) */
    uint32_t vertex_count, command_count, convert_count, surface_count;
    int32_t display_surface;         /* scanned-out surface, or -1 */
    uint32_t display[12];            /* scanout constants as recorded (native.wgsl native_scanout) */
    uint32_t movie_width, movie_height, movie_on;
    uint32_t scale;                  /* the renderer's resolution scale (DHD_SCALE or matched) */
    const DhdrVertex *vertices;      /* possibly DHD_INTERP-mixed */
    const DhdrCommand *commands;
    const DhdrConvert *converts;
    const DhdrSurfaceInfo *surfaces; /* surface_count entries, snapshotted for this picture */
    const uint32_t *movie;           /* movie_on: movie_width * movie_height RGBA8, shown instead */
} DhdrPicture;
typedef struct DhdrPictureResult {
    uint32_t shown;                  /* 1: a picture was scanned out (it can be presented) */
    uint32_t display_width, display_height, pad;
    uint64_t draws, self_copies, self_copy_px, copies, copy_px, imports, import_px, two_pass_draws;
} DhdrPictureResult;
typedef struct DhdrPresentRect {
    float rect[4];                   /* present.wgsl's rectangle, as native_present_rect makes it */
    int32_t cut[4];                  /* scissor in window pixels: left, top, right, bottom */
    uint32_t window_w, window_h;     /* the client area; 0 x 0 = minimised: nothing is drawn */
    uint32_t resize, pad;            /* resize: reconfigure even when the size is unchanged */
} DhdrPresentRect;
enum { DHDR_NATIVE_DUAL = 1, DHDR_NATIVE_POINT_FILTER = 2 };   /* create: DUAL = may use dual-source
                                                                  blending; options: POINT_FILTER */
DHDR_FN(dhdr_native_create)  DhdrNative *dhdr_native_create(uint32_t flags);   /* NULL on pipeline failure (logged) */
DHDR_FN(dhdr_native_destroy) void dhdr_native_destroy(DhdrNative *n);           /* NULL is ignored */
/* A decoded texture: w x h RGBA8 rows. NULL on failure (logged). */
DHDR_FN(dhdr_texture_create) DhdrTexture *dhdr_texture_create(const uint32_t *rgba, uint32_t w, uint32_t h);
/* A replacement (texture pack, sharp font): levels mip levels, each level's rows following the
   previous level's (sides halved, down to 1), RGBA8 (replace_texture.h's ReplaceDevice create).
   NULL on failure (logged). */
DHDR_FN(dhdr_replace_create) DhdrTexture *dhdr_replace_create(const uint8_t *rgba_mips, uint32_t w, uint32_t h,
                                                              uint32_t levels);
DHDR_FN(dhdr_texture_retain)  void dhdr_texture_retain(DhdrTexture *t);
DHDR_FN(dhdr_texture_release) void dhdr_texture_release(DhdrTexture *t);       /* NULL is ignored */
/* Creates surface index (< 48) at scale, or grows it to at least w x h scaled pixels keeping its
   content (new texels zero); a depth surface is depth32float, a colour one RGBA8. Writes the
   texture's size to size_out. 1 ok, 0 failed (logged; the old texture, if any, is kept). */
DHDR_FN(dhdr_native_surface) int dhdr_native_surface(DhdrNative *n, uint32_t index, uint32_t scale, uint32_t w, uint32_t h,
                                                     uint32_t depth, uint32_t size_out[2]);
/* Draws the picture's commands, scans out (or shows its movie) and submits. 1 ok (out filled);
   0 failed (logged). */
DHDR_FN(dhdr_native_picture) int dhdr_native_picture(DhdrNative *n, const DhdrPicture *p, DhdrPictureResult *out);
/* Presents the last scanned-out picture (also "present again"); return values as dhdr_present_exact. */
DHDR_FN(dhdr_native_present) int dhdr_native_present(DhdrNative *n, DhdrSurface *s, const DhdrPresentRect *r);
/* The displayed picture: begin writes its size (0 = none, returns 0), finish writes w * h RGBA8. */
DHDR_FN(dhdr_native_read_begin)  int dhdr_native_read_begin(DhdrNative *n, uint32_t *w, uint32_t *h);
DHDR_FN(dhdr_native_read_finish) int dhdr_native_read_finish(DhdrNative *n, uint32_t *pixels);
DHDR_FN(dhdr_native_options) void dhdr_native_options(DhdrNative *n, uint32_t flags);   /* DHDR_NATIVE_POINT_FILTER */
/* The Z probe from native depth (finding 097). start copies the centre texel of each GS pixel of
   rect (x, y, w, h; at most 64 x 64) of Z surface index and maps it without waiting: 1 started,
   0 not. take never waits: the newest finished copy's w * h depth32float bit patterns (row-major,
   at most cap) in out, its rect and tag; 1 a new copy, 0 none. */
DHDR_FN(dhdr_native_zread_start) int dhdr_native_zread_start(DhdrNative *n, uint32_t index, const uint32_t rect[4], uint64_t tag);
DHDR_FN(dhdr_native_zread_take) int dhdr_native_zread_take(DhdrNative *n, uint32_t *out, uint32_t cap, uint32_t rect_out[4],
                                                           uint64_t *tag_out);

#if !defined(__wasm__)
/* ---------------- the texture packs' PNG files (texpack.c), native only ---------------- */
/* The width and height in a PNG's header (size bytes at data). 1 ok, 0 not a PNG (logged). */
DHDR_FN(dhdr_png_size) int dhdr_png_size(const uint8_t *data, size_t size, uint32_t *w, uint32_t *h);
/* Decodes the PNG into rgba, w * h * 4 bytes (w x h as dhdr_png_size gave): RGBA8 rows from any
   colour type, bit depth and interlacing (16-bit samples cut to their high byte). A PNG with no
   alpha channel and no tRNS gets opaque (0-255) as its alpha. 1 ok, 0 failed (logged). Any thread. */
DHDR_FN(dhdr_png_decode) int dhdr_png_decode(const uint8_t *data, size_t size, uint8_t *rgba, uint32_t w, uint32_t h,
                                             uint32_t opaque);
#endif

#if defined(__wasm__)
/* The surface the async dhdr_web_init made on the OffscreenCanvas, or NULL. */
DHDR_FN(dhdr_web_surface) DhdrSurface *dhdr_web_surface(void);
#endif

#ifdef DHD_GS_TEST_BUILD
/* Test hooks (cargo feature test-hooks; native only). */
/* raster.wgsl's depth_probe over count cases: 80 bytes in, 8 bytes out per case. 1 ok. */
DHDR_FN(dhdr_test_depth_probe) int dhdr_test_depth_probe(const void *cases, uint32_t count, uint32_t *results);
/* The last presented surface texture (read before it was presented), w x h RGBA8; NULL = none. */
DHDR_FN(dhdr_test_presented)   const uint32_t *dhdr_test_presented(uint32_t *w, uint32_t *h);
#endif

#ifdef __cplusplus
}
#endif
#endif
