/* Native transfer ordering, using synthetic solid colours and no game data.
   The presenter, texture replacements and EE scheduler are absent.
   DHD_NATIVE_TEST_DUMP=dir: every picture (NN-name.rgba, then its size) and the CPU's VRAM
   after it (NN.vram) go there, so test_native_transfers.py can compare runs (the renderer with and
   without dual-source blending, finding 084 W7, and the stored goldens); the blend scenes at the end
   have no expected colour of their own and are checked only that way. Adapter 0 (DXGI's first). */
#include <windows.h>
#include "gpu.h"
#include "present.h"
#include "texpack.h"
#include "font_sharp.h"
#include <windows.h>
#include "../include/guest.h"
#if defined(__x86_64__) || defined(_WIN64) || defined(__aarch64__)
uintptr_t guest_base;  /* host_win32.c's; gs.c's host services name it, the test never calls them */
#endif
uint64_t kernel_vsync_count;
volatile int gs_native_view;
double host_seconds(void) { return GetTickCount64()/1000.0; }
double host_wall_seconds(void) { return host_seconds(); }
void host_thread_name(const char *n) {(void)n;}
void host_putenv_s(const char *name, const char *value) {_putenv_s(name, value);}
void pacing_handed(double *d,double *b) {*d=*b=0;}
void pacing_present(double d,double b) {(void)d;(void)b;}
int host_widescreen_flat_limit(void) {return 1000;}
int host_widescreen_card_drawn(void) {return 0;}
double host_widescreen_aspect(void) {return 16.0/9;}
int texpack_open(const ReplaceDevice *d,uint64_t m) {(void)d; (void)m; return 0;}
int texpack_available(void) {return 0;}
TexPackEntry *texpack_find(const Gs *g,unsigned c) {return NULL;}
void *texpack_view(TexPackEntry *e,uint64_t f) {(void)e; (void)f; return NULL;}
uint8_t *texpack_image(TexPackEntry *e,unsigned *w,unsigned *h) {(void)e; *w = *h = 0; return NULL;}
void texpack_field(uint64_t f) {}
void texpack_report(void) {}
int font_sharp_on(void) {return 0;}
int font_sharp_active(void) {return 0;}
int font_sharp_icon_style(void) {return 0;}
int font_sharp_icon_style_latch(void) {return 0;}
int font_sharp_match(uint64_t t) {return 0;}
void *font_sharp_view(const ReplaceDevice *d,const uint32_t *p,unsigned w,unsigned h,uint64_t k,
                      const FontSharpPack *pk) {(void)d; (void)p; (void)w; (void)h; (void)k; (void)pk; return NULL;}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

const uint32_t *gs_wgpu_presented(unsigned *width, unsigned *height);  /* gpu_rust_test.c */

enum { WIDTH = 512, HEIGHT = 224 };
enum { PRESENTED_W = 1280, PRESENTED_H = 720 };  /* DHD_NATIVE_TEST_PRESENTED's window: 4:3 with bars */
static unsigned failures, pictures;
static int presented_on;
static unsigned presented_checks;
static HWND presented_window_handle;

/* The presented window's client size (the renderer's GsWindow.size). */
static void presented_window_size(unsigned *width, unsigned *height)
{
    RECT client = {0, 0, 0, 0};
    GetClientRect(presented_window_handle, &client);
    *width = (unsigned)client.right; *height = (unsigned)client.bottom;
}

/* test_gpu_stubs.c's test_gpu_init (this program has its own stubs): gs_gpu_init under the EE mode
   when DHD_TEST_EE_MXCSR=1 (round toward zero, FTZ, DAZ, exceptions masked; host_win32.c). */
static int test_gpu_init(Gs *g)
{
    const char *ee = getenv("DHD_TEST_EE_MXCSR");
    unsigned csr = _mm_getcsr();
    if (ee && atoi(ee) == 1) _mm_setcsr((csr & 63u) | 0xFFC0u);
    int ok = gs_gpu_init(g);
    _mm_setcsr(csr);
    return ok;
}

/* DHD_NATIVE_TEST_PRESENTED: every picture is also presented to a window that is never
   shown. The surface texture, read back before the present (gs_wgpu_presented), must be the picture
   scaled to the window's 4:3 rectangle, centred, nearest (DHD_PRESENT_FILTER=point), with black
   bars: the same rule as test_present.c. */
static void check_presented(const char *name, const uint32_t *picture, unsigned w, unsigned h)
{
    if (!presented_on || !picture) return;
    const unsigned W = PRESENTED_W, H = PRESENTED_H;
    unsigned sw = 0, sh = 0, bad = 0;
    const uint32_t *shown = gs_wgpu_presented(&sw, &sh);
    presented_checks++;
    if (!shown || sw != W || sh != H) {
        printf("%s: FAIL no presented %ux%u surface read back\n", name, W, H);
        failures++;
        return;
    }
    unsigned vw = H * 4 / 3, vh = H, vx = (W - vw) / 2, vy = 0;
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++) {
            uint32_t want = 0xFF000000;
            if (x >= vx && x < vx + vw && y >= vy && y < vy + vh) {
                unsigned sx = (2 * (x - vx) + 1) * w / (2 * vw), sy = (2 * (y - vy) + 1) * h / (2 * vh);
                want = picture[sy * w + sx] | 0xFF000000;
            }
            uint32_t got = shown[y * W + x];
            if (got != want && bad++ < 4) printf("  presented (%u,%u): %08X, want %08X\n", x, y, got, want);
        }
    printf("%s: presented %u pixels differ\n", name, bad);
    failures += bad != 0;
}

/* DHD_NATIVE_TEST_DUMP: the picture and the VRAM the game would read, for the backend comparison. */
static void dump(const char *name, const uint32_t *pixels, unsigned w, unsigned h)
{
    const char *dir = getenv("DHD_NATIVE_TEST_DUMP");
    char path[512];
    unsigned index = pictures++;
    if (!dir) return;
    snprintf(path, sizeof path, "%s/%02u.vram", dir, index);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(gs.vram, 1, GS_VRAM_SIZE, f); fclose(f); }
    snprintf(path, sizeof path, "%s/%02u.rgba", dir, index);
    if ((f = fopen(path, "wb"))) {
        uint32_t size[2] = {w, h};
        if (pixels) fwrite(pixels, 4, (size_t)w * h, f);
        fwrite(size, 4, 2, f);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/%02u.name", dir, index);
    if ((f = fopen(path, "w"))) { fputs(name, f); fclose(f); }
}

static void clear(unsigned page, uint32_t color)
{
    gs_write(&gs, GS_FRAME_1, page | 8ULL << 16);
    gs_write(&gs, GS_ZBUF_1, 1ULL << 32);
    gs_write(&gs, GS_TEST_1, 0);
    gs_write(&gs, GS_SCISSOR_1, (WIDTH - 1ULL) << 16 | (HEIGHT - 1ULL) << 48);
    gs_write(&gs, GS_PRIM, 6);
    gs_write(&gs, GS_RGBAQ, color);
    gs_write(&gs, GS_XYZ2, 0);
    gs_write(&gs, GS_XYZ2, WIDTH * 16ULL | (HEIGHT * 16ULL) << 16);
}

static void show(unsigned page, uint32_t color, const char *name)
{
    gs_priv_write(&gs, GS_PMODE, 2);
    gs_priv_write(&gs, GS_SMODE2, 0);
    gs_priv_write(&gs, GS_DISPFB2, page | 8ULL << 9);
    gs_priv_write(&gs, GS_DISPLAY2, (WIDTH - 1ULL) << 32 | (HEIGHT - 1ULL) << 44);
    unsigned w = 0, h = 0;
    ++kernel_vsync_count;
    gs_native_vsync(&gs, &w, &h, 0);
    uint32_t *pixels = gs_native_picture(&gs, &w, &h);
    dump(name, pixels, w, h);
    check_presented(name, pixels, w, h);
    unsigned changed = 0;
    if (pixels)
        for (unsigned i = 0; i < w * h; i++)
            changed += (pixels[i] & 0xFFFFFF) != (color & 0xFFFFFF);
    int ok = pixels && w == WIDTH * 2 && h == HEIGHT * 2 && !changed;
    if (!ok) failures++;
    printf("%s: %s (%ux%u, %u incorrect pixels)\n", name, ok ? "PASS" : "FAIL", w, h, changed);
}

static void sample_alpha(unsigned x, unsigned y, unsigned alpha, const char *name)
{
    clear(256, 0);
    /* CSM2 palette: index becomes the red channel, making stored alpha visible. */
    for (unsigned i = 0; i < 256; i++) gs_write_pixel(&gs, GS_PSMCT32, 14000, 4, i, 0, 0x80000000 | i);
    gs_write(&gs, GS_TEXCLUT, 4);
    gs_write(&gs, GS_TEX0_1, 4096ULL | 8ULL << 14 | (uint64_t)GS_PSMT8H << 20 |
             9ULL << 26 | 8ULL << 30 | 1ULL << 35 | 14000ULL << 37 | 1ULL << 55 | 1ULL << 61);
    gs_write(&gs, GS_PRIM, 6 | 16 | 256);
    gs_write(&gs, GS_RGBAQ, 0x3F80000080808080ULL);
    gs_write(&gs, GS_UV, (x * 16 + 8) | (uint64_t)(y * 16 + 8) << 16);
    gs_write(&gs, GS_XYZ2, 0);
    gs_write(&gs, GS_XYZ2, WIDTH * 16ULL | (HEIGHT * 16ULL) << 16);
    show(256, alpha, name);
}

/* Finding 196: an upscaled sprite interpolates its UVs smoothly across the whole sprite, not
   once per GS pixel: a 2-GS-pixel-wide sprite over a black-to-white 2-texel texture shows a
   four-step gradient at 2x, where GS-pixel sampling showed two flat pairs. */
static void sprite_gradient(void)
{
    clear(256, 0xFF000000);
    gs_write_pixel(&gs, GS_PSMCT32, 4096, 1, 0, 0, 0xFF000000);
    gs_write_pixel(&gs, GS_PSMCT32, 4096, 1, 1, 0, 0xFFFFFFFF);
    gs_write_pixel(&gs, GS_PSMCT32, 4096, 1, 0, 1, 0xFF000000);
    gs_write_pixel(&gs, GS_PSMCT32, 4096, 1, 1, 1, 0xFFFFFFFF);
    gs_write(&gs, GS_TEX0_1, 4096ULL | 1ULL << 14 | 1ULL << 26 | 1ULL << 30 | 1ULL << 34);
    gs_write(&gs, GS_TEX1_1, 1ULL << 5 | 1ULL << 6);
    gs_write(&gs, GS_PRIM, 6 | 16 | 256);
    gs_write(&gs, GS_RGBAQ, 0x3F80000080808080ULL);
    gs_write(&gs, GS_UV, 8 | 8ULL << 16);
    gs_write(&gs, GS_XYZ2, 16 * 16ULL | (16 * 16ULL) << 16);
    gs_write(&gs, GS_UV, 24 | 24ULL << 16);
    gs_write(&gs, GS_XYZ2, 18 * 16ULL | (18 * 16ULL) << 16);
    gs_priv_write(&gs, GS_PMODE, 2);
    gs_priv_write(&gs, GS_SMODE2, 0);
    gs_priv_write(&gs, GS_DISPFB2, 256 | 8ULL << 9);
    gs_priv_write(&gs, GS_DISPLAY2, (WIDTH - 1ULL) << 32 | (HEIGHT - 1ULL) << 44);
    unsigned w = 0, h = 0;
    ++kernel_vsync_count;
    gs_native_vsync(&gs, &w, &h, 0);
    uint32_t *pixels = gs_native_picture(&gs, &w, &h);
    dump("upscaled sprite interpolates smoothly", pixels, w, h);
    check_presented("upscaled sprite interpolates smoothly", pixels, w, h);
    unsigned r[4] = {0, 0, 0, 0};
    int ok = pixels && w == WIDTH * 2 && h == HEIGHT * 2;
    if (ok)
        for (unsigned i = 0; i < 4; i++) r[i] = pixels[34 * w + 32 + i] & 255;
    ok = ok && r[0] < r[1] && r[1] < r[2] && r[2] < r[3] && r[3] - r[0] > 100;
    if (!ok) failures++;
    printf("upscaled sprite interpolates smoothly: %s (%u %u %u %u)\n", ok ? "PASS" : "FAIL",
           r[0], r[1], r[2], r[3]);
}

/* A blend scene's picture: no expected colour, compared across backends (dump). */
static void picture(unsigned page, const char *name)
{
    gs_priv_write(&gs, GS_PMODE, 2);
    gs_priv_write(&gs, GS_SMODE2, 0);
    gs_priv_write(&gs, GS_DISPFB2, page | 8ULL << 9);
    gs_priv_write(&gs, GS_DISPLAY2, (WIDTH - 1ULL) << 32 | (HEIGHT - 1ULL) << 44);
    unsigned w = 0, h = 0;
    ++kernel_vsync_count;
    gs_native_vsync(&gs, &w, &h, 0);
    uint32_t *pixels = gs_native_picture(&gs, &w, &h);
    dump(name, pixels, w, h);
    check_presented(name, pixels, w, h);
    if (!pixels) failures++;
    printf("%s: %s (%ux%u)\n", name, pixels ? "drawn" : "FAIL", w, h);
}

enum { SCENE_Z = 200 };   /* the scenes' Z buffer page (frame: page 0) */

static void vertex(int x, int y, uint32_t z, uint32_t rgba)
{
    gs_write(&gs, GS_RGBAQ, rgba | 0x3F80000000000000ULL);
    gs_write(&gs, GS_XYZ2, (uint64_t)(x * 16) | (uint64_t)(y * 16) << 16 | (uint64_t)z << 32);
}

/* A scene's start: a coloured background with a ramp of alpha (so DA blending varies), Z at 0. */
static void scene_background(void)
{
    gs_write(&gs, GS_FRAME_1, 8ULL << 16);
    gs_write(&gs, GS_SCISSOR_1, (WIDTH - 1ULL) << 16 | (HEIGHT - 1ULL) << 48);
    gs_write(&gs, GS_ALPHA_1, 0);
    gs_write(&gs, GS_ZBUF_1, SCENE_Z);
    gs_write(&gs, GS_TEST_1, 1ULL << 16 | 1ULL << 17);  /* ZTE, ALWAYS: Z written 0 */
    gs_write(&gs, GS_PRIM, 3 | 8);  /* Gouraud triangles */
    vertex(0, 0, 0, 0x10204060); vertex(WIDTH, 0, 0, 0x90A04020); vertex(0, HEIGHT, 0, 0x40308090);
    vertex(WIDTH, 0, 0, 0x90A04020); vertex(0, HEIGHT, 0, 0x40308090); vertex(WIDTH, HEIGHT, 0, 0xF0F0F0F0);
}

/* Overlapping triangles in one draw (one command): the later ones at the same Z as earlier ones
   in places, alpha above and below 0x80, so As / 128 and the Z test both matter. */
static void scene_triangles(uint32_t z)
{
    for (unsigned i = 0; i < 6; i++) {
        int x = 40 + (int)i * 60, y = 20 + (int)(i % 3) * 40;
        uint32_t zi = i & 1 ? z : z + 0x100000 * i;
        uint32_t a = (0x20 + i * 0x28) << 24;
        vertex(x, y, zi, a | 0x0000FF);
        vertex(x + 160, y + 30, zi, a | 0x00FF00);
        vertex(x + 40, y + 150, zi + 0x80000, a | 0xFF0000);
    }
}

static void blend_scene(const char *name, uint64_t alpha, uint32_t fbmsk, uint64_t test, int zwrite, unsigned prim)
{
    scene_background();
    gs_write(&gs, GS_FRAME_1, 8ULL << 16 | (uint64_t)fbmsk << 32);
    gs_write(&gs, GS_ALPHA_1, alpha);
    gs_write(&gs, GS_TEST_1, test);
    gs_write(&gs, GS_ZBUF_1, SCENE_Z | (zwrite ? 0 : 1ULL << 32));
    gs_write(&gs, GS_PRIM, prim);
    scene_triangles(0x40000000);
    scene_triangles(0x40000000);   /* again on itself: equal Z everywhere */
    picture(0, name);
}

static void blend_scenes(void)
{
    const uint64_t blend = 0 | 1 << 2 | 0 << 4 | 1 << 6;          /* (Cs - Cd) * As + Cd */
    const uint64_t add = 0 | 2 << 2 | 0 << 4 | 1 << 6;            /* Cs * As + Cd (premultiplied) */
    const uint64_t dest = 0 | 1 << 2 | 1 << 4 | 1 << 6;           /* (Cs - Cd) * Ad + Cd */
    const uint64_t fix = 0 | 1 << 2 | 2 << 4 | 1 << 6 | 0x50ULL << 32;   /* (Cs - Cd) * 0x50 + Cd */
    const uint64_t sub = 1 | 0 << 2 | 0 << 4 | 1 << 6;            /* (Cd - Cs) * As + Cd */
    const uint64_t inverse = 0 | 1 << 2 | 0 << 4 | 2 << 6;        /* (Cs - Cd) * As + 0 */
    const uint64_t keep = 0 | 0 << 2 | 0 << 4 | 1 << 6;           /* (Cs - Cs) * As + Cd: the colour kept */
    const uint64_t always = 1ULL << 16 | 1ULL << 17, gequal = 1ULL << 16 | 2ULL << 17, greater = 1ULL << 16 | 3ULL << 17;
    const unsigned tri = 3 | 8 | 64;
    blend_scene("blend As, alpha written, no Z", blend, 0, 0, 0, tri);
    blend_scene("blend As, alpha masked", blend, 0xFF000000u, 0, 0, tri);
    blend_scene("blend As, Z ALWAYS with writes", blend, 0, always, 1, tri);
    blend_scene("blend As, Z GEQUAL with writes", blend, 0, gequal, 1, tri);
    blend_scene("blend As, Z GREATER with writes", blend, 0, greater, 1, tri);
    blend_scene("blend As, Z GEQUAL read only", blend, 0, gequal, 0, tri);
    blend_scene("blend As, Z GREATER read only", blend, 0, greater, 0, tri);
    blend_scene("Cs * As + Cd", add, 0, gequal, 1, tri);
    blend_scene("blend Ad", dest, 0, gequal, 1, tri);
    blend_scene("blend FIX", fix, 0, greater, 1, tri);
    blend_scene("(Cd - Cs) * As + Cd", sub, 0, gequal, 1, tri);
    blend_scene("(Cs - Cd) * As", inverse, 0, greater, 1, tri);
    blend_scene("blend As, green and alpha masked", blend, 0xFF00FF00u, gequal, 1, tri);
    blend_scene("blend As, alpha test AFAIL FB_ONLY", blend, 0, greater | 1 | 5 << 1 | 0x60 << 4 | 1 << 12, 1, tri);
    blend_scene("blend As, alpha test AFAIL RGB_ONLY", blend, 0, gequal | 1 | 5 << 1 | 0x60 << 4 | 3 << 12, 1, tri);
    blend_scene("no blending, Z GREATER with writes", blend, 0, greater, 1, 3 | 8);
    /* The colour kept (blend Zero / One; finding 176): drawn without blending, colour masked. */
    blend_scene("Cd kept, alpha written, Z GEQUAL with writes", keep, 0, gequal, 1, tri);
    blend_scene("Cd kept, alpha masked, Z GREATER with writes", keep, 0xFF000000u, greater, 1, tri);
    blend_scene("Cd kept, alpha masked, Z GEQUAL read only", keep, 0xFF000000u, gequal, 0, tri);
    blend_scene("Cd kept, alpha test AFAIL FB_ONLY", keep, 0, greater | 1 | 5 << 1 | 0x60 << 4 | 1 << 12, 1, tri);
}

/* A full picture from CPU memory alone (an upload: no draw). */
static void upload(unsigned page, uint32_t color)
{
    gs_write(&gs, GS_BITBLTBUF, ((uint64_t)page * 32) << 32 | 8ULL << 48);
    gs_write(&gs, GS_TRXPOS, 0);
    gs_write(&gs, GS_TRXREG, WIDTH | (uint64_t)HEIGHT << 32);
    gs_write(&gs, GS_TRXDIR, 0);
    for (unsigned i = 0; i < WIDTH * HEIGHT / 2; i++) gs_write(&gs, GS_HWREG, color | (uint64_t)color << 32);
}

int main(void)
{
    gs_reset(&gs);
    _putenv("DHD_RENDER=native");
    _putenv("DHD_SCALE=2");
    const char *presented = getenv("DHD_NATIVE_TEST_PRESENTED");
    presented_on = presented && *presented && atoi(presented) == 1;
    if (presented_on) _putenv("DHD_PRESENT_FILTER=point");
    if (!test_gpu_init(&gs) || !gs_gpu_native(&gs)) return 2;
    printf("backend: %s\n", gs_gpu_backend_name(&gs));
    if (presented_on) {
        /* WS_POPUP without WS_VISIBLE: the client area is the size asked for, and nothing shows */
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "dhd_native_transfers";
        RegisterClassA(&wc);
        presented_window_handle = CreateWindowExA(0, wc.lpszClassName, "test_native_transfers", WS_POPUP, 0, 0,
                                                  PRESENTED_W, PRESENTED_H, NULL, NULL, wc.hInstance, NULL);
        GsWindow target = {GS_WINDOW_WIN32, (uintptr_t)presented_window_handle, (uintptr_t)GetModuleHandleA(NULL),
                           presented_window_size};
        if (!presented_window_handle || !gs_gpu_surface(&gs, &target)) {
            printf("FAIL DHD_NATIVE_TEST_PRESENTED: no WebGPU surface\n");
            return 1;
        }
    }

    /* The upload is later than the clear, even though the native draw batch is open. */
    clear(0, 0x80000000);
    gs_write(&gs, GS_BITBLTBUF, 8ULL << 48);
    gs_write(&gs, GS_TRXPOS, 0);
    gs_write(&gs, GS_TRXREG, WIDTH | (uint64_t)HEIGHT << 32);
    gs_write(&gs, GS_TRXDIR, 0);
    for (unsigned i = 0; i < WIDTH * HEIGHT / 2; i++)
        gs_write(&gs, GS_HWREG, 0x800000FF800000FFULL);
    show(0, 0x800000FF, "clear then full-page CPU upload");

    /* A newly drawn source has no older surface owner for the copy to find. */
    clear(128, 0x8000FF00);
    gs_write(&gs, GS_BITBLTBUF, 4096ULL | 8ULL << 16 | 8192ULL << 32 | 8ULL << 48);
    gs_write(&gs, GS_TRXPOS, 0);
    gs_write(&gs, GS_TRXREG, WIDTH | (uint64_t)HEIGHT << 32);
    gs_write(&gs, GS_TRXDIR, 2);
    show(256, 0x8000FF00, "new draw target then local copy");

    clear(128, 0xA500FF00);
    show(128, 0xA500FF00, "colour before high-byte uploads");
    gs_write_pixel(&gs, GS_PSMT4HL, 4096, 8, 7, 9, 3);
    gs_write_pixel(&gs, GS_PSMT4HH, 4096, 8, 8, 9, 4);
    gs_write_pixel(&gs, GS_PSMT8H, 4096, 8, 9, 9, 0x7E);
    gs_write_pixel(&gs, GS_PSMT4HL, 4096, 8, 10, 9, 0);
    show(128, 0xA500FF00, "high-byte uploads preserve every RGB pixel");
    sample_alpha(7, 9, 0xA3, "4HL preserves the high nibble");
    sample_alpha(8, 9, 0x45, "4HH preserves the low nibble");
    sample_alpha(9, 9, 0x7E, "8H replaces the byte");
    sample_alpha(10, 9, 0xA0, "zero nibble is still written");
    sample_alpha(0, 0, 0xA5, "partial uploads preserve other pixels");
    sprite_gradient();

    clear(56, 0x8000FF00);
    show(56, 0x8000FF00, "first loading buffer before font upload");
    clear(112, 0x800000FF);
    show(112, 0x800000FF, "second loading buffer before font upload");
    gs_write(&gs, GS_BITBLTBUF, 128ULL << 32 | 8ULL << 48 | (uint64_t)GS_PSMT4HL << 56);
    gs_write(&gs, GS_TRXPOS, 0);
    gs_write(&gs, GS_TRXREG, 512ULL | 512ULL << 32);
    gs_write(&gs, GS_TRXDIR, 0);
    for (unsigned i = 0; i < 512 * 512 / 16; i++) gs_write(&gs, GS_HWREG, 0xFFFFFFFFFFFFFFFFULL);
    show(56, 0x8000FF00, "font alpha preserves the whole first loading buffer");
    show(112, 0x800000FF, "font alpha preserves the second loading buffer's corner");

    /* No draw in these four pictures: a CPU upload, a local copy onto a CPU page, a font-alpha
       upload over a CPU page (the CPU's memory holds it, so the frame imports the page), and a
       font-alpha upload over the copied surface. The native frames then hold IMPORT, COPY and
       ALPHA_UPLOAD commands and the scanout only (native_replay --only no-draw). Each picture is 56
       pages (512x224 CT32); the three areas (pages 344, 400 and 456) do not overlap, and nothing
       before uses them except sample_alpha's CLUT (block 14000), which no later picture reads. */
    upload(344, 0x80336699);
    show(344, 0x80336699, "no draw: CPU upload");
    upload(400, 0x80112233);
    gs_write(&gs, GS_BITBLTBUF, (uint64_t)(344 * 32) | 8ULL << 16 | (uint64_t)(400 * 32) << 32 | 8ULL << 48);
    gs_write(&gs, GS_TRXPOS, 0);
    gs_write(&gs, GS_TRXREG, WIDTH | (uint64_t)HEIGHT << 32);
    gs_write(&gs, GS_TRXDIR, 2);
    show(400, 0x80336699, "no draw: local copy over a CPU page");
    upload(456, 0x80445566);
    gs_write(&gs, GS_BITBLTBUF, (uint64_t)(456 * 32) << 32 | 8ULL << 48 | (uint64_t)GS_PSMT4HL << 56);
    gs_write(&gs, GS_TRXPOS, 0);
    gs_write(&gs, GS_TRXREG, WIDTH | (uint64_t)HEIGHT << 32);
    gs_write(&gs, GS_TRXDIR, 0);
    for (unsigned i = 0; i < WIDTH * HEIGHT / 16; i++) gs_write(&gs, GS_HWREG, 0xFFFFFFFFFFFFFFFFULL);
    show(456, 0x80445566, "no draw: font alpha over a CPU page");
    /* page 400 is still the copy's surface: the alpha goes to it as ALPHA_UPLOAD, the RGB stays */
    gs_write(&gs, GS_BITBLTBUF, (uint64_t)(400 * 32) << 32 | 8ULL << 48 | (uint64_t)GS_PSMT4HL << 56);
    gs_write(&gs, GS_TRXPOS, 0);
    gs_write(&gs, GS_TRXREG, WIDTH | (uint64_t)HEIGHT << 32);
    gs_write(&gs, GS_TRXDIR, 0);
    for (unsigned i = 0; i < WIDTH * HEIGHT / 16; i++) gs_write(&gs, GS_HWREG, 0x5A5A5A5A5A5A5A5AULL);
    show(400, 0x80336699, "no draw: font alpha over a copied surface");
    blend_scenes();

    if (presented_on) {
        printf("presented: %u pictures checked against the window\n", presented_checks);
        DestroyWindow(presented_window_handle);
    }
    gs_gpu_destroy(&gs);
    return failures ? 1 : 0;
}
