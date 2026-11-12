/* Native renderer concurrency (--stress, port/tools/test_native_transfers.py): six fields are handed
   to the render thread without reading their pictures, while a second thread creates and releases
   replacement textures through the ReplaceDevice the renderer gives texpack_open. The final picture
   of every round must equal the picture of a synchronous run (each picture read before the next field
   is handed over). Synthetic colours only, no game data.
   Each round's churn thread has made a replacement before the first field is handed over, so it
   runs beside the queued fields; a replacement the device does not create fails the run.
   DHD_NATIVE_STRESS_ROUNDS=n: rounds of the queued run (default 8). Exit 1 on any difference.
   DHD_NATIVE_TEST_DUMP=dir: the synchronous run's six pictures and the last round's final picture,
   with the CPU's VRAM after each, as test_native_transfers.c writes them (NN.rgba, NN.vram, NN.name). */
#include <windows.h>
#include "gpu.h"
#include "present.h"
#include "texpack.h"
#include "font_sharp.h"
#include <windows.h>
#include <xmmintrin.h>
#include "../include/guest.h"
#if defined(__x86_64__) || defined(_WIN64) || defined(__aarch64__)
uintptr_t guest_base;  /* host_win32.c's; gs.c's host services name it, the test never calls them */
#endif
uint64_t kernel_vsync_count;
volatile int gs_native_view;
double host_seconds(void) { return GetTickCount64()/1000.0; }
double host_wall_seconds(void) { return host_seconds(); }
void host_thread_name(const char *n) {(void)n;}
void host_end_add(int stage, const char *name, void (*fn)(void)) { (void)stage; (void)name; atexit(fn); }  /* as atexit did */
void host_putenv_s(const char *name, const char *value) {_putenv_s(name, value);}
void pacing_handed(double *d,double *b) {*d=*b=0;}
void pacing_present(double d,double b) {(void)d;(void)b;}
int host_widescreen_flat_limit(void) {return 1000;}
int host_widescreen_card_drawn(void) {return 0;}
double host_widescreen_aspect(void) {return 16.0/9;}
/* The renderer's ReplaceDevice (native_init calls texpack_open with it): kept for the churn thread. */
static const ReplaceDevice *replace_device;
int texpack_open(const ReplaceDevice *d,uint64_t m) {(void)m; replace_device = d; return 0;}
int texpack_available(void) {return 0;}
TexPackEntry *texpack_find(const Gs *g,unsigned c) {(void)g; (void)c; return NULL;}
void *texpack_view(TexPackEntry *e,uint64_t f) {(void)e; (void)f; return NULL;}
uint8_t *texpack_image(TexPackEntry *e,unsigned *w,unsigned *h) {(void)e; *w = *h = 0; return NULL;}
void texpack_field(uint64_t f) {(void)f;}
void texpack_report(void) {}
int font_sharp_on(void) {return 0;}
int font_sharp_active(void) {return 0;}
int font_sharp_icon_style(void) {return 0;}
int font_sharp_icon_style_latch(void) {return 0;}
int font_sharp_match(uint64_t t) {(void)t; return 0;}
void *font_sharp_view(const ReplaceDevice *d,const uint32_t *p,unsigned w,unsigned h,uint64_t k,
                      const FontSharpPack *pk)
{(void)d; (void)p; (void)w; (void)h; (void)k; (void)pk; return NULL;}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 512, HEIGHT = 224, FIELDS = 6, PICTURE_PIXELS = 1024 * 448 };
static unsigned failures, pictures;
static volatile LONG churn_stop, churn_replacements, churn_refused;
static uint32_t reference[PICTURE_PIXELS], picture[PICTURE_PIXELS];

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

static void vertex(int x, int y, uint32_t rgba)
{
    gs_write(&gs, GS_RGBAQ, rgba | 0x3F80000000000000ULL);
    gs_write(&gs, GS_XYZ2, (uint64_t)(x * 16) | (uint64_t)(y * 16) << 16);
}

/* Field i: a full-screen clear to its colour (a sprite), then six Gouraud triangles of its colours.
   Page 0 is the frame; nothing depends on what was there before, so every round draws the same. */
static void field(unsigned index)
{
    uint32_t clear_colour = 0x80000000u | ((index + 1) * 0x0A3C5Bu & 0xFFFFFFu);
    gs_write(&gs, GS_FRAME_1, 8ULL << 16);
    gs_write(&gs, GS_ZBUF_1, 1ULL << 32);
    gs_write(&gs, GS_TEST_1, 0);
    gs_write(&gs, GS_SCISSOR_1, (WIDTH - 1ULL) << 16 | (HEIGHT - 1ULL) << 48);
    gs_write(&gs, GS_PRIM, 6);
    gs_write(&gs, GS_RGBAQ, clear_colour | 0x3F80000000000000ULL);
    gs_write(&gs, GS_XYZ2, 0);
    gs_write(&gs, GS_XYZ2, WIDTH * 16ULL | (HEIGHT * 16ULL) << 16);
    gs_write(&gs, GS_PRIM, 3 | 8);
    for (unsigned i = 0; i < 6; i++) {
        int x = 20 + (int)i * 70 + (int)index * 5, y = 10 + (int)(i % 3) * 50;
        uint32_t c = (index * 0x1F3u + i * 0x4B9u) * 0x9E3779B1u;
        vertex(x, y, c & 0xFFFFFFu);
        vertex(x + 150, y + 40, (c >> 3) & 0xFFFFFFu);
        vertex(x + 40, y + 140, (c >> 7) & 0xFFFFFFu);
    }
}

static void display(void)
{
    gs_priv_write(&gs, GS_PMODE, 2);
    gs_priv_write(&gs, GS_SMODE2, 0);
    gs_priv_write(&gs, GS_DISPFB2, 0 | 8ULL << 9);
    gs_priv_write(&gs, GS_DISPLAY2, (WIDTH - 1ULL) << 32 | (HEIGHT - 1ULL) << 44);
}

/* The field's hand-over to the render thread (EE side). */
static void hand_over(void)
{
    unsigned w = 0, h = 0;
    ++kernel_vsync_count;
    gs_native_vsync(&gs, &w, &h, 0);
}

/* The picture after the last hand-over, copied out (the renderer's buffer is reused). */
static int read_picture(uint32_t *out, unsigned *w, unsigned *h)
{
    uint32_t *pixels = gs_native_picture(&gs, w, h);
    if (!pixels || *w * *h != PICTURE_PIXELS) return 0;
    memcpy(out, pixels, (size_t)*w * *h * 4);
    return 1;
}

/* DHD_NATIVE_TEST_DUMP: a picture and the CPU's VRAM after it (test_native_transfers.c's format). */
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

/* Replacements the churn thread has asked for so far (created or refused). */
static LONG churned(void)
{
    return InterlockedCompareExchange(&churn_replacements, 0, 0) + InterlockedCompareExchange(&churn_refused, 0, 0);
}

static DWORD WINAPI churn(void *argument)
{
    static uint8_t pixels[64 * 64 * 4];
    const ReplaceDevice *d = argument;
    while (!InterlockedCompareExchange(&churn_stop, 0, 0)) {
        void *view = d->create(d->context, pixels, 64, 64, 1);
        if (!view) { InterlockedIncrement(&churn_refused); continue; }
        d->retain(view);
        d->release(view);
        d->release(view);
        InterlockedIncrement(&churn_replacements);
    }
    return 0;
}

int main(void)
{
    gs_reset(&gs);
    _putenv("DHD_RENDER=native");
    _putenv("DHD_SCALE=2");
    const char *env = getenv("DHD_NATIVE_STRESS_ROUNDS");
    unsigned rounds = env && atoi(env) > 0 ? (unsigned)atoi(env) : 8;
    if (!test_gpu_init(&gs) || !gs_gpu_native(&gs)) {
        printf("FAIL the native renderer did not start\n");
        return 2;
    }
    printf("backend: %s\n", gs_gpu_backend_name(&gs));
    if (!replace_device) {
        printf("FAIL the native renderer gave no ReplaceDevice to texpack_open\n");
        return 1;
    }

    /* The synchronous run: each field's picture is read before the next field is handed over. */
    unsigned w = 0, h = 0;
    for (unsigned i = 0; i < FIELDS; i++) {
        field(i);
        display();
        hand_over();
        if (!read_picture(reference, &w, &h)) {
            printf("FAIL the synchronous run's picture of field %u\n", i);
            return 1;
        }
        char name[64];
        snprintf(name, sizeof name, "stress synchronous field %u", i);
        dump(name, reference, w, h);
    }
    printf("synchronous: %u fields, %ux%u final picture\n", FIELDS, w, h);

    /* The queued runs: the six fields are handed over without reading any picture, while the churn
       thread creates and releases replacements; then the final picture, read once. */
    for (unsigned round = 0; round < rounds; round++) {
        InterlockedExchange(&churn_stop, 0);
        LONG before = churned();
        HANDLE thread = CreateThread(NULL, 0, churn, (void *)replace_device, 0, NULL);
        if (!thread) { printf("FAIL no churn thread\n"); return 1; }
        while (churned() == before) SwitchToThread();  /* the churn has begun */
        for (unsigned i = 0; i < FIELDS; i++) {
            field(i);
            display();
            hand_over();
        }
        unsigned pw = 0, ph = 0;
        int ok = read_picture(picture, &pw, &ph);
        InterlockedExchange(&churn_stop, 1);
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        if (!ok || pw != w || ph != h) {
            printf("stress round %u: FAIL no final picture (%ux%u)\n", round, pw, ph);
            failures++;
            continue;
        }
        if (round == rounds - 1) dump("stress queued final picture", picture, pw, ph);
        unsigned differ = 0;
        for (unsigned i = 0; i < PICTURE_PIXELS; i++) differ += (picture[i] ^ reference[i]) != 0;
        printf("stress round %u: %u pixels differ from the synchronous run\n", round, differ);
        failures += differ != 0;
    }
    if (churn_refused) {
        printf("FAIL the ReplaceDevice refused %ld of %ld replacements\n", (long)churn_refused,
               (long)(churn_refused + churn_replacements));
        failures++;
    }
    printf("stress: %u rounds, %ld replacements created and released\n", rounds, (long)churn_replacements);

    gs_gpu_destroy(&gs);
    return failures ? 1 : 0;
}
