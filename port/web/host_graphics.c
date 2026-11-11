/* Headless device boundary. GS and VU1 are the existing C implementations.
   The threaded build (build_graphics.py --threads) builds this file into two modules, one per
   worker (web_queue.h): graphics.wasm with DHD_WEB_GS (the GS front end's side) and vu.wasm with
   DHD_WEB_VU (VIF1/VU1's side; its graphics_gif_feed is web_queue.c's, into the GS worker's ring).
   The one-worker build has both sides. */
#include "../hle/hle.h"
#include "../hle/gs_queue.h"
#include "../gs/gs.h"
#include <string.h>
GraphicsProvenance graphics_provenance;
volatile uint32_t gs_presented_frames;
uint64_t kernel_vsync_count;
int hle_verbose;
/* As kernel.c's natively (dhd.log): every line, to the page's log. The reactor's callers are VIF1's
   rare or diagnostic lines and Native VU's (vu1_mesh.inc: one line at each change of the setting). */
void hle_log(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap);
}
double host_seconds(void) { return host_wall_seconds(); }
#ifndef DHD_WEB_VU
void graphics_gif_feed(unsigned path,const uint32_t *words,unsigned count) {
    if(!count)return; /* Native FIFO's while(count) never emits an empty packet. */
    /* as gs_queue.inc's gs_execute: a shadow batch's kick (vif.c's flag; the threaded build's
       web_queue.c sets it from the packet) and a translated mesh batch (Native VU, vu1_mesh.inc) */
    gs_vu_shadow = graphics_provenance.shadow;
#define WEB_GS_EXECUTE() (path == GS_PATH_MESH ? gs_mesh(&gs,words,count) : gs_feed(&gs,path,words,count))
    /* The FP mode of native's GS worker (gs_queue.inc gs_consume sets (csr|0x1f80)&~(63|0xe040):
       round to nearest, no FTZ/DAZ). XGKICK calls in here from VU1 under the VU mode (toward
       zero, FTZ, DAZ), so without this the GS front end's float math (draw setup) ran in a mode
       native never uses, through the slow round-toward-zero helpers of float.c/double.c. */
#ifdef PS2_VU_IEEE  /* the reactor's float ops are plain IEEE ones (build_graphics.py): no mode */
    WEB_GS_EXECUTE();
#else
    unsigned csr=host_fp_csr();
    host_fp_set_csr((csr|0x1f80u)&~(63u|0xe040u));
    WEB_GS_EXECUTE();
    host_fp_set_csr(csr);
#endif
#undef WEB_GS_EXECUTE
}
#endif
#ifndef DHD_WEB_GS
int graphics_xgkick(uint32_t memory,unsigned qword) {
    /* Match gs_queue.inc's default worker packet boundaries: the state trace
       records each gs_feed call as well as its words. Execution stays serial. */
    const uint32_t *words=GUEST(memory);unsigned total=0;
    for (;;) {
        const uint32_t *tag=words+((qword+total)&1023)*4;
        unsigned loops=tag[0]&32767,mode=(tag[1]>>26)&3,regs=tag[1]>>28;
        if(!regs)regs=16;
        unsigned payload=mode==0?loops*regs:mode==1?(loops*regs+1)/2:loops;
        if(payload>=0x4000-total)return -1;
        total+=1+payload;if(tag[0]&0x8000)break;
        if(total==0x4000)return -1;
    }
    for(unsigned done=0;done<total;){
        unsigned first=(qword+done)&1023,take=1024-first;
        if(take>total-done)take=total-done;
        graphics_gif_feed(1,words+first*4,take*4);done+=take;
    }return 0;
}
#endif
void graphics_gs_wait(void) {}
#ifndef DHD_WEB_GS
void host_service_graphics_feed(int unit,uint32_t address,uint32_t words) {
    if(!words)return;
    if (unit==2) graphics_gif_feed(3,GUEST(address),words);
    else vif_feed(unit,GUEST(address),words);
}
#endif
void web_frame(uint64_t field) { kernel_vsync_count=field; }
/* The field web_frame set: the threaded build's graphics workers give it to host_wall_seconds
   (field / 60, as the core's worker does), the field of the call being run, not the core's now. */
__attribute__((export_name("web_field"))) double web_field(void) { return (double)kernel_vsync_count; }
#ifndef DHD_WEB_VU
/* The GS privileged registers' MMIO pages (0x12000000 + addr, addr = 0, 16, ... 0xE0), as the
   vsync's web_display reads them: live, or in the threaded build the copy the core took when it
   queued the vsync (web_queue.c; the core goes on writing the live pages meanwhile). */
const uint64_t *web_gs_mmio_snapshot;
uint64_t web_gs_mmio(unsigned addr)
{
    if (web_gs_mmio_snapshot) return web_gs_mmio_snapshot[addr / 16];
    return *(volatile uint64_t *)(uintptr_t)(0x12000000u + addr);
}
#endif
/* Every call from the core enters through build_graphics.py's web_x_* wrappers. */
void web_graphics_enter(void) {}
#ifndef DHD_WEB_GS
/* The VU1 blocks take their words from the data image (ps2_vu_aot_load, as memory.c's
   ps2_image_load does natively); the core has loaded it before the first VIF1 packet. */
void ps2_vu_aot_load(const void *vutext);
/* VU1 microprogram runs through the native kernels so far (the page's status line). */
extern unsigned long long ps2_vu1_native_runs[];
extern double web_vu_steps, web_vu_blocks;
__attribute__((export_name("web_vu_stat"))) double web_vu_stat(int which) { return which ? web_vu_blocks : web_vu_steps; }
__attribute__((export_name("web_vu_native"))) double web_vu_native(void) {
    double n = 0;
    for (unsigned i = 0; i < 12; i++) n += (double)ps2_vu1_native_runs[i];
    return n;
}
void web_vu_ready(void) {
    static int loaded;
    /* Not before the image is there: VIF1 packets can come before the core's ps2_image_load. */
    const volatile uint32_t *image = (const volatile uint32_t *)(uintptr_t)(0x08000000u + 0x263BC0u);
    /* The section starts with zero words: look for any data in its first 4 KB. */
    uint32_t any = 0;
    if (!loaded) for (unsigned i = 0; i < 1024 && !any; i++) any = image[i];
    if (!loaded && any) {
        loaded = 1;
        ps2_vu_aot_load((const void *)image);
        /* aot_find and the kernels cache their answer per code version: forget the misses
           recorded before the words were there. */
        extern uint64_t ps2_vu1_code_version;
        ps2_vu1_code_version++;
    }
}
#else
void web_vu_ready(void) {}
#endif
#ifdef DHD_WEB_VU
/* The VU worker's environment changed (www/vu.js updated its WASI copy): getenv reads it again. */
#include <wasi/libc-environ.h>
__attribute__((export_name("web_vu_env_changed"))) void web_vu_env_changed(void) { __wasilibc_deinitialize_environ(); }
#endif

/* The FP control word (MXCSR image) at 0x07800000 in the shared memory: the float helpers read
   it there, the page's fiber switches save and restore it there. Defined here rather than imported
   from JS, and exported as web_x_* so the core's imports link straight to them (no JS frame on
   the EE's frequent rounding-mode changes).
   The threaded build (build_graphics.py --threads, DHD_WEB_THREADS) runs this reactor's two
   modules in their own workers: each has its own word (WEB_Q_REACTOR_CSR, WEB_Q_VU_CSR; their
   float helpers are built for it), and the core's host_fp_* link to gqueue.wasm's, on 0x07800000. */
#ifdef DHD_WEB_THREADS
#include "web_queue.h"
#ifdef DHD_WEB_VU
#define WEB_CSR (*(volatile uint32_t *)(uintptr_t)WEB_Q_VU_CSR)
#else
#define WEB_CSR (*(volatile uint32_t *)(uintptr_t)WEB_Q_REACTOR_CSR)
#endif
#else
#define WEB_CSR (*(volatile uint32_t *)(uintptr_t)0x07800000u)
#endif
unsigned host_fp_csr(void) { return WEB_CSR; }
void host_fp_set_csr(unsigned csr) { WEB_CSR = csr; }
void host_fp_mode(int mode) { WEB_CSR = mode ? 0xffc0u : 0x1f80u; }
__attribute__((export_name("web_x_host_fp_csr"))) unsigned web_x_host_fp_csr(void) { return WEB_CSR; }
__attribute__((export_name("web_x_host_fp_set_csr"))) void web_x_host_fp_set_csr(unsigned csr) { WEB_CSR = csr; }
__attribute__((export_name("web_x_host_fp_mode"))) void web_x_host_fp_mode(int mode) { host_fp_mode(mode); }
