/* The host symbols the renderer's native half (gpu_rust_native.inc) links against, for GS test
   programs that don't use it (test_gpu, test_depth, test_present, replay): the same stubs as
   test_native_transfers.c. */
#include "gpu.h"
#include "present.h"
#include "texpack.h"
#include "font_sharp.h"
#include <windows.h>
#include <xmmintrin.h>
#include <stdlib.h>
#include "../include/guest.h"
#if defined(__x86_64__) || defined(_WIN64) || defined(__aarch64__)
uintptr_t guest_base;  /* host_win32.c's; gs.c's host services name it, the tests never call them */
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

/* DHD_TEST_EE_MXCSR=1 (forwarded by test_gs_wgpu.py): the EE thread's MXCSR for the calls between
   test_ee_mxcsr_enter and test_ee_mxcsr_leave: round toward zero, FTZ and DAZ, exceptions masked
   (host_win32.c's host_fp_mode: 0xFFC0 with the flags kept), as the game runs the GS start and the
   shader compiles. FXC folds constants with the caller's mode (finding 084, 244df66db), so this is the
   run that sees a shader compiled in the wrong mode. Unset, or any other value: the caller's mode. */
unsigned test_ee_mxcsr_enter(void)
{
    const char *ee = getenv("DHD_TEST_EE_MXCSR");
    unsigned csr = _mm_getcsr();
    if (ee && atoi(ee) == 1) _mm_setcsr((csr & 63u) | 0xFFC0u);
    return csr;
}
void test_ee_mxcsr_leave(unsigned csr) { _mm_setcsr(csr); }

/* Every test program starts the GS through this (gs_gpu_init under the EE mode). The device:
   WARP, or the GPU with DHD_GS_TEST_HARDWARE=1 (DHD_WGPU_ADAPTER=fallback asks gpu_rust.c for WARP). */
int test_gpu_hardware(void)
{
    const char *hardware = getenv("DHD_GS_TEST_HARDWARE");
    return hardware && atoi(hardware);
}
int test_gpu_init(Gs *g)
{
    if (!test_gpu_hardware()) _putenv_s("DHD_WGPU_ADAPTER", "fallback");
    unsigned csr = test_ee_mxcsr_enter();
    int ok = gs_gpu_init(g);
    test_ee_mxcsr_leave(csr);
    return ok;
}
