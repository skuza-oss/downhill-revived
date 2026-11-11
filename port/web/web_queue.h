/* The browser's graphics queue (build.py --threads): the game core in one Web Worker, the graphics
   reactor (VIF1/VU1, the GS front end, the Rust renderer) in another, on one shared memory, as the
   native build runs VIF1/VU1 and the GS on their own threads (port/hle/graphics_queue.c,
   gs_queue.inc).

   The core's side (web_queue.c built with WEB_QUEUE_PRODUCER into build/web/gqueue.wasm, which the
   core's graphics imports link to) copies every graphics call into a ring, in program order:
   - VIF1/GIF data (host_service_graphics_feed, host_service_vif_feed): copied, not waited for, as
     natively;
   - the GS calls that return a value or write guest memory (register reads, readbacks, snapshots,
     reset): queued, then the core waits until the reactor has run everything before them and them;
   - the GS calls that do neither (VBLANK, a privileged register write, the trace start): queued;
   - host_service_graphics_wait (every place the native EE waits for its workers, the mapped VU1,
     VIF1, GIF and GS registers among them): waits until the ring is empty;
   - per field: the field number (web_frame), the vsync (web_display, with the GS register MMIO
     pages as they were when it was queued) and environment updates.
   The reactor's side (web_queue.c in graphics.wasm) runs the packets in order, so the graphics
   state sees exactly the call sequence of the one-worker build.

   The control block sits at a fixed address in the web host's scratch region next to the core's
   FP control word (0x07800000): both modules know it without a link. The ring itself is the
   consumer's (its bss); the consumer publishes its address here.

   Two stages since SP4 (finding 090), as natively VIF1/VU1 and the GS have a thread each
   (graphics_queue.c's vu thread feeding gs_queue.inc's GS thread):
   - the VU worker (www/vu.js, build/web/vu.wasm: vif.c, vu0_interp.c and web_queue.c built with
     WEB_QUEUE_VU) consumes this ring: it runs the VIF1/VU1 packets and the VU1 calls itself, and
     passes everything else on, in order, through a second ring (WEB_Q_BLOCK2) to the GS worker
     (www/graphics.js, graphics.wasm: the GS front end and the Rust renderer):
     - each graphics_gif_feed (XGKICK, DIRECT) as a WQ_GIF packet: a copy of its words, one per
       call, so the GS sees the gs_feed calls of the one-worker build;
     - the GS's packets of this ring (GS calls, PATH3, the field, the vsync, the environment) as
       WQ_REF packets: their address in this ring, not a copy;
     - WQ_MARK packets: this ring's position up to which the VU worker is done; the GS worker
       stores it as this ring's consumed once it has run everything before the mark.
   - So consumed here is what both stages have finished: the core waits on it as before (its side
     is unchanged), and a referenced packet stays in this ring until the GS has run it. The VU
     worker keeps its own read position, and marks after each waited call, whenever it has caught
     up with written, and at least every quarter ring. */
#ifndef DHD_WEB_QUEUE_H
#define DHD_WEB_QUEUE_H
#include <stdint.h>

#define WEB_Q_BLOCK 0x07A00000u
#define WEB_Q_BLOCK2 0x07A00800u      /* the VU worker's ring to the GS worker (the same layout) */
#define WEB_Q_RING_BYTES (8u << 20)   /* a power of two; as graphics_queue.c's ring */
/* The reactor's own FP control word (float.c/double.c's helpers and host_fp_* in the reactor):
   the core's stays at 0x07800000; the threads must not share one. */
#define WEB_Q_REACTOR_CSR 0x07800040u
#define WEB_Q_VU_CSR 0x07800080u      /* the VU worker's */
/* The reactor's float/memory helpers' private stack top (postlink.py; the core's is 0x07920000). */
#define WEB_Q_REACTOR_HELPER_STACK 0x07960000u
#define WEB_Q_VU_HELPER_STACK 0x079A0000u

enum {
    WQ_PAD,        /* the ring's end: skip to its start */
    WQ_VIF,        /* host_service_vif_feed(unit a, words b): vif_feed */
    WQ_GRAPHICS,   /* host_service_graphics_feed(unit a, words b): GIF PATH3 (unit 2) or VIF */
    WQ_CALL,       /* a GS/VU1 call: id a, b = 1 when the core waits for it; 3 u64 arguments */
    WQ_FRAME,      /* web_frame: the field number (u64) */
    WQ_VSYNC,      /* web_display, with 15 u64 GS register pages (0x12000000 + 16 i) */
    WQ_ENV,        /* an environment update: a bytes of "NAME=value" */
    /* the second ring's (VU worker to GS worker) */
    WQ_GIF,        /* graphics_gif_feed(path a, words b), the words copied */
    WQ_REF,        /* the first ring's packet at address a, for the GS */
    WQ_MARK,       /* the first ring is done up to position a */
};
#define WQ_KIND 0xffu
#define WQ_VU_READY 0x100u   /* the VU1 image was in guest RAM when the core queued this */
#define WQ_VU_CALL 0x200u    /* a WQ_CALL the VU worker runs (host_vu1_*), not the GS */
#define WQ_SHADOW 0x400u     /* a WQ_GIF a shadow batch kicked (graphics_provenance.shadow; Native VU's :shadow) */

typedef struct {
    uint32_t bytes;   /* the packet's size (16-byte multiple), header included */
    uint32_t kind;    /* WQ_* | WQ_VU_READY */
    uint32_t a, b;
} WebPacket;

/* web_q_run's answers to its JS loop. */
enum { WQ_RUN_EMPTY, WQ_RUN_PRESENT, WQ_RUN_ENV };

typedef struct {
    uint32_t written;        /* producer: bytes published (wraps) */
    uint32_t pad0[15];
    uint32_t consumed;       /* consumer: bytes retired (the first ring: by both workers; the GS
                                worker stores it at each WQ_MARK) */
    uint32_t pad1[15];
    uint32_t sleeping;       /* consumer: waits on written (graphics.js: Atomics.waitAsync; the VU
                                worker: memory.atomic.wait32) */
    uint32_t pad2[15];
    uint32_t waiting;        /* producer: waits on consumed (memory.atomic.wait32) */
    uint32_t pad3[15];
    uint32_t ring;           /* the ring's address, 0 until the reactor's web_q_init */
    uint32_t failed;         /* the graphics worker stopped: a waiting core traps */
    uint32_t vu_seen;        /* producer: the VU1 image has been seen in guest RAM */
    uint32_t display;        /* the GS worker (in the second block): web_display's answer at the
                                last WQ_VSYNC */
    uint64_t result;         /* the worker that ran it (in the first block): the last waited-for
                                call's result */
    uint32_t env_text, env_bytes;   /* each worker in its block (the VU worker the first, the GS
                                       worker the second): the last WQ_ENV's copy, for its JS loop */
    uint32_t stats[16];      /* graphics.js: the status line's numbers, for worker.js (STAT there) */
    /* Native VU (in the first block only): gs.h's gs_mesh_mode and gs_mesh.h's gs_mesh_setting,
       the two words natively shared by the GS and VU threads. Here the GS worker (graphics.wasm:
       the renderer, the setting read) and the VU worker (vu.wasm: vu1_mesh.inc) are two modules,
       so the words live at this fixed place both know (gs_mesh.h's DHD_WEB_THREADS macros). */
    int32_t mesh_setting, mesh_mode;
} WebQueue;
/* The offsets www/graphics.js, www/worker.js and headless_graphics.mjs use (Int32Array indices). */
_Static_assert(__builtin_offsetof(WebQueue, written) == 0 && __builtin_offsetof(WebQueue, sleeping) == 128 &&
               __builtin_offsetof(WebQueue, failed) == 260 && __builtin_offsetof(WebQueue, display) == 268 &&
               __builtin_offsetof(WebQueue, env_text) == 280 && __builtin_offsetof(WebQueue, env_bytes) == 284 &&
               __builtin_offsetof(WebQueue, stats) == 288, "web queue layout (JS reads it by offset)");

#define WQ ((WebQueue *)(uintptr_t)WEB_Q_BLOCK)
#define WQ2 ((WebQueue *)(uintptr_t)WEB_Q_BLOCK2)
_Static_assert(sizeof(WebQueue) <= WEB_Q_BLOCK2 - WEB_Q_BLOCK, "the two control blocks");
#endif
