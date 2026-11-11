/* The browser's graphics queue (web_queue.h), both sides:
   - WEB_QUEUE_PRODUCER: the core's side, build/web/gqueue.wasm (build_graphics.py --threads, with
     its generated call wrappers gqueue_calls.c). The core's graphics imports link to its web_x_*
     exports through www/thunks.js, as they link to the reactor's in the one-worker build.
   - WEB_QUEUE_VU: the VU worker's side, linked into vu.wasm (build_graphics.py --threads): the
     first ring and web_vq_run, which www/vu.js (or headless_graphics.mjs) calls in a loop, and the
     second ring's producer (graphics_gif_feed, the WQ_REF and WQ_MARK packets).
   - otherwise: the GS worker's side, linked into graphics.wasm (build_graphics.py --threads): the
     second ring and web_q_run, which www/graphics.js (or headless_graphics.mjs) calls in a loop.
   Positions only grow (32-bit wrap); each side writes its own and reads the other's with
   sequentially consistent atomics. A side that sleeps says so first (sleeping, waiting) and reads
   the other's position again, so the other side's notify after its publication is never lost (the
   handshake of port/hle/graphics_queue.c's idle_wait, on wasm's wait and notify). */
#include "web_queue.h"

static inline uint32_t load(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void store(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static inline WebPacket *packet(uint32_t at)
{
    return (WebPacket *)(uintptr_t)(WQ->ring + (at & (WEB_Q_RING_BYTES - 1)));
}

#ifdef WEB_QUEUE_PRODUCER
/* ---- the core's side ---- */

/* The core's FP control word (the float helpers postlink.py put into the core read it here; the
   page's fiber switches save and restore it), as host_graphics.c has it in the one-worker build. */
#define WEB_CSR (*(volatile uint32_t *)(uintptr_t)0x07800000u)
__attribute__((export_name("web_x_host_fp_csr"))) unsigned web_x_host_fp_csr(void) { return WEB_CSR; }
__attribute__((export_name("web_x_host_fp_set_csr"))) void web_x_host_fp_set_csr(unsigned csr) { WEB_CSR = csr; }
__attribute__((export_name("web_x_host_fp_mode"))) void web_x_host_fp_mode(int mode) { WEB_CSR = mode ? 0xffc0u : 0x1f80u; }

/* build_graphics.py's guest address fold (the reactor's web_x_* wrappers fold the same way). */
static inline uint32_t web_fold(uint32_t a)
{
    uint32_t low = a & 0x1fffffffu;
    if (a >= 0x38000000u && a < 0x3a000000u) return a - 0x30000000u;
    if (a >= 0x30400000u && a < 0x32000000u) return a - 0x28000000u;
    if ((a & 0xe0000000u) == 0 || (a & 0xe0000000u) == 0x20000000u) {
        if (low >= 0x08000000u && low < 0x0a000000u) return low;
        if (low >= 0x00400000u && low < 0x02000000u) return low + 0x08000000u;
    }
    return a;
}

/* Until consumed moves away from seen: a short spin, then wasm's wait (a dedicated worker may
   block). The graphics worker sets failed when it stops; the core then traps instead of hanging. */
static void wait_consumed(uint32_t seen)
{
    WebQueue *q = WQ;
    for (unsigned spin = 0; spin < 4096; spin++)
        if (load(&q->consumed) != seen) return;
    __atomic_fetch_add(&q->waiting, 1, __ATOMIC_SEQ_CST);
    while (load(&q->consumed) == seen) {
        if (load(&q->failed)) __builtin_trap();
        __builtin_wasm_memory_atomic_wait32((int *)&q->consumed, (int)seen, 100000000LL);
    }
    __atomic_fetch_sub(&q->waiting, 1, __ATOMIC_SEQ_CST);
}

/* Until the reactor has retired everything up to end. */
static void wait_until(uint32_t end)
{
    for (;;) {
        uint32_t done = load(&WQ->consumed);
        if ((int32_t)(done - end) >= 0) return;
        wait_consumed(done);
    }
}

/* Room for one packet of bytes at the head (a pad packet first when it would cross the ring's
   end): its position. Only this side writes written. */
static uint32_t reserve(uint32_t bytes)
{
    WebQueue *q = WQ;
    if (!q->ring) __builtin_trap();   /* the reactor's web_q_init has not run */
    uint32_t at = q->written;
    uint32_t tail = WEB_Q_RING_BYTES - (at & (WEB_Q_RING_BYTES - 1)), wrap = bytes > tail ? tail : 0;
    for (;;) {
        uint32_t done = load(&q->consumed);
        if (WEB_Q_RING_BYTES - (at - done) >= bytes + wrap) break;
        wait_consumed(done);
    }
    if (wrap) {
        WebPacket *pad = packet(at);
        pad->bytes = wrap;
        pad->kind = WQ_PAD;
        at += wrap;
    }
    return at;
}

static void publish(uint32_t end)
{
    WebQueue *q = WQ;
    store(&q->written, end);
    if (load(&q->sleeping)) __builtin_wasm_memory_atomic_notify((int *)&q->written, 1);
}

/* host_graphics.c's web_vu_ready, decided where the one-worker build decides it: the VU1 image
   (the core loads it into guest RAM) is there when the call is made, not when the reactor runs it. */
static uint32_t vu_ready(void)
{
    WebQueue *q = WQ;
    if (!q->vu_seen) {
        const volatile uint32_t *image = (const volatile uint32_t *)(uintptr_t)(0x08000000u + 0x263BC0u);
        for (unsigned i = 0; i < 1024; i++)
            if (image[i]) { q->vu_seen = 1; break; }
    }
    return q->vu_seen ? WQ_VU_READY : 0;
}

/* VIF1/GIF data, copied now (the core may change it once this returns). A transfer larger than a
   quarter of the ring goes in parts, as graphics_queue.c's (a DMA block is at most 1 MiB). */
static void feed(uint32_t kind, int unit, uint32_t address, uint32_t words)
{
    const uint32_t *from = (const uint32_t *)(uintptr_t)web_fold(address);
    uint32_t flags = vu_ready();
    do {
        uint32_t take = words > WEB_Q_RING_BYTES / 16 ? WEB_Q_RING_BYTES / 16 : words;
        uint32_t bytes = (uint32_t)(sizeof(WebPacket) + take * 4 + 15) & ~15u;
        uint32_t at = reserve(bytes);
        WebPacket *p = packet(at);
        p->bytes = bytes;
        p->kind = kind | flags;
        p->a = (uint32_t)unit;
        p->b = take;
        __builtin_memcpy(p + 1, from, take * 4);
        publish(at + bytes);
        from += take;
        words -= take;
    } while (words);
}

__attribute__((export_name("web_x_host_service_graphics_feed")))
void web_x_host_service_graphics_feed(int unit, uint32_t address, uint32_t words) { feed(WQ_GRAPHICS, unit, address, words); }
__attribute__((export_name("web_x_host_service_vif_feed")))
void web_x_host_service_vif_feed(int unit, uint32_t address, uint32_t words) { feed(WQ_VIF, unit, address, words); }
/* Every place the native EE waits for its graphics workers. */
__attribute__((export_name("web_x_host_service_graphics_wait")))
void web_x_host_service_graphics_wait(void) { wait_until(WQ->written); }

/* A GS/VU1 call (gqueue_calls.c's wrappers): queued; with wait, the core waits for its result. */
uint64_t web_q_call(uint32_t id, uint32_t wait, uint32_t flags, uint64_t a0, uint64_t a1, uint64_t a2)
{
    uint32_t at = reserve(48);
    WebPacket *p = packet(at);
    p->bytes = 48;
    p->kind = WQ_CALL | flags;
    p->a = id;
    p->b = wait;
    uint64_t *args = (uint64_t *)(p + 1);
    args[0] = a0; args[1] = a1; args[2] = a2;
    publish(at + 48);
    if (!wait) return 0;
    wait_until(at + 48);
    return WQ->result;
}
uint32_t web_q_vu_ready(void) { return vu_ready(); }

/* worker.js's host_frame_context: the reactor's kernel_vsync_count (web_frame), in order. */
__attribute__((export_name("web_q_frame"))) void web_q_frame(uint64_t field)
{
    uint32_t at = reserve(32);
    WebPacket *p = packet(at);
    p->bytes = 32;
    p->kind = WQ_FRAME;
    p->a = p->b = 0;
    *(uint64_t *)(p + 1) = field;
    publish(at + 32);
}

/* worker.js's host_service_gs_vsync: web_display with the GS register pages as they are now. */
__attribute__((export_name("web_q_vsync"))) void web_q_vsync(void)
{
    uint32_t at = reserve(144);
    WebPacket *p = packet(at);
    p->bytes = 144;
    p->kind = WQ_VSYNC;
    p->a = p->b = 0;
    uint64_t *regs = (uint64_t *)(p + 1);
    for (unsigned i = 0; i < 15; i++) regs[i] = *(volatile uint64_t *)(uintptr_t)(0x12000000u + 16 * i);
    publish(at + 144);
}

/* worker.js's host_putenv: "NAME=value" (bytes long, at text) for the reactor's environment. */
__attribute__((export_name("web_q_env"))) void web_q_env(uint32_t text, uint32_t bytes)
{
    if (bytes > 4000) __builtin_trap();
    uint32_t size = (uint32_t)(sizeof(WebPacket) + bytes + 15) & ~15u;
    uint32_t at = reserve(size);
    WebPacket *p = packet(at);
    p->bytes = size;
    p->kind = WQ_ENV;
    p->a = bytes;
    p->b = 0;
    __builtin_memcpy(p + 1, (const void *)(uintptr_t)text, bytes);
    publish(at + size);
}

#else
/* ---- the graphics workers' side ---- */
#include "../hle/gs_queue.h"   /* graphics_gif_feed */
void web_graphics_enter(void);
void web_frame(uint64_t field);
void host_fp_set_csr(unsigned csr);

static inline WebPacket *packet_in(const WebQueue *q, uint32_t at)
{
    return (WebPacket *)(uintptr_t)(q->ring + (at & (WEB_Q_RING_BYTES - 1)));
}
/* consumed up to at, and the producer told when it waits for room or a result. */
static void retire(WebQueue *q, uint32_t at)
{
    store(&q->consumed, at);
    if (load(&q->waiting)) __builtin_wasm_memory_atomic_notify((int *)&q->consumed, 0xffffffffu);
}

static uint8_t web_ring[WEB_Q_RING_BYTES] __attribute__((aligned(64)));
static char env_copy[4096];

/* A WQ_ENV packet's line, copied for the JS loop (into the worker's own block). */
static int env_line(WebQueue *q, const WebPacket *p)
{
    if (p->a >= sizeof env_copy) __builtin_trap();
    __builtin_memcpy(env_copy, p + 1, p->a);
    env_copy[p->a] = 0;
    q->env_text = (uint32_t)(uintptr_t)env_copy;
    q->env_bytes = p->a;
    return WQ_RUN_ENV;
}

#ifdef WEB_QUEUE_VU
/* ---- the VU worker (vu.wasm, www/vu.js): the first ring's consumer, the second's producer ---- */
void vif_feed(int unit, const uint32_t *words, uint32_t count);
void web_vu_ready(void);
uint64_t web_q_vu_dispatch(uint32_t id, const uint64_t *args);   /* build_graphics.py's web_exports_vu.c */

static uint32_t read_at, marked;   /* the first ring: run up to, marked up to */

/* A worker stopped (its JS sets failed in both blocks): trap rather than wait for ever. */
static void check_failed(void)
{
    if (load(&WQ->failed) || load(&WQ2->failed)) __builtin_trap();
}

/* Room for one packet of bytes in the second ring (a pad packet first when it would cross the
   ring's end), as the core's reserve: a short spin, then wasm's wait (a worker may block). */
static uint32_t reserve2(uint32_t bytes)
{
    WebQueue *q = WQ2;
    uint32_t at = q->written;
    uint32_t tail = WEB_Q_RING_BYTES - (at & (WEB_Q_RING_BYTES - 1)), wrap = bytes > tail ? tail : 0;
    for (unsigned spin = 0;; spin++) {
        uint32_t done = load(&q->consumed);
        if (WEB_Q_RING_BYTES - (at - done) >= bytes + wrap) break;
        if (spin < 4096) continue;
        __atomic_fetch_add(&q->waiting, 1, __ATOMIC_SEQ_CST);
        if (load(&q->consumed) == done) {
            check_failed();
            __builtin_wasm_memory_atomic_wait32((int *)&q->consumed, (int)done, 100000000LL);
        }
        __atomic_fetch_sub(&q->waiting, 1, __ATOMIC_SEQ_CST);
    }
    if (wrap) {
        WebPacket *pad = packet_in(q, at);
        pad->bytes = wrap;
        pad->kind = WQ_PAD;
        at += wrap;
    }
    return at;
}

static void publish2(uint32_t end)
{
    WebQueue *q = WQ2;
    store(&q->written, end);
    if (load(&q->sleeping)) __builtin_wasm_memory_atomic_notify((int *)&q->written, 1);
}

static void put2(uint32_t kind, uint32_t a)
{
    uint32_t at = reserve2(16);
    WebPacket *p = packet_in(WQ2, at);
    p->bytes = 16;
    p->kind = kind;
    p->a = a;
    p->b = 0;
    publish2(at + 16);
}

static void mark(uint32_t at)
{
    put2(WQ_MARK, at);
    marked = at;
}

/* VU1's output for the GS (XGKICK through graphics_xgkick, VIF1 DIRECT): copied into the second
   ring, one packet per call, so the GS front end gets the gs_feed calls of the one-worker build. */
void graphics_gif_feed(unsigned path, const uint32_t *words, unsigned count)
{
    if (!count) return;   /* as host_graphics.c's: never an empty gs_feed */
    uint32_t bytes = (uint32_t)(sizeof(WebPacket) + count * 4 + 15) & ~15u;
    if (bytes > WEB_Q_RING_BYTES / 2) __builtin_trap();   /* DIRECT moves at most 1 MiB */
    uint32_t at = reserve2(bytes);
    WebPacket *p = packet_in(WQ2, at);
    p->bytes = bytes;
    p->kind = WQ_GIF | (graphics_provenance.shadow ? WQ_SHADOW : 0);   /* Native VU's shadow kicks */
    p->a = path;
    p->b = count;
    __builtin_memcpy(p + 1, words, count * 4);
    publish2(at + bytes);
}

/* Once, after the GS worker's web_q_init (the second ring must be there): the first ring's
   address for the core's side; 0 when the second ring is missing. The VU worker's own FP control
   word (WEB_Q_VU_CSR) is set to IEEE mode and stays in it between packets. */
__attribute__((export_name("web_vq_init"))) uint32_t web_vq_init(void)
{
    if (!load(&WQ2->ring)) return 0;
    host_fp_set_csr(0x1f80);
    store(&WQ->ring, (uint32_t)(uintptr_t)web_ring);
    return WEB_Q_BLOCK;
}

/* Runs the first ring's packets, waiting (memory.atomic.wait32) when it is empty, until an
   environment update (WQ_RUN_ENV: env_text, env_bytes in the first block), which its JS loop
   applies before it calls again, or 0.1 s without a packet (WQ_RUN_EMPTY: its JS loop takes its
   messages and calls again). VIF1 packets and VU1 calls run here; the rest goes to the GS. */
__attribute__((export_name("web_vq_run"))) int web_vq_run(void)
{
    WebQueue *q = WQ;
    for (;;) {
        uint32_t at = read_at;
        if (load(&q->written) == at) {
            if (marked != at) mark(at);   /* caught up: the core may be waiting for this */
            for (unsigned spin = 0; spin < 32768 && load(&q->written) == at; spin++) {}
            if (load(&q->written) == at) {
                store(&q->sleeping, 1);
                int idle = 0;
                while (load(&q->written) == at && !idle) {
                    check_failed();
                    idle = __builtin_wasm_memory_atomic_wait32((int *)&q->written, (int)at, 100000000LL) == 2;
                }
                store(&q->sleeping, 0);
                if (idle) return WQ_RUN_EMPTY;   /* 0.1 s without a packet: a turn of the JS loop */
            }
            continue;
        }
        WebPacket *p = packet_in(q, at);
        uint32_t kind = p->kind & WQ_KIND, end = at + p->bytes, waited = 0;
        int answer = -1;
        if (kind == WQ_CALL) {
            waited = p->b;
            if (p->kind & WQ_VU_CALL) {
                /* the web_x_* wrapper folds the addresses and checks the VU1 image */
                uint64_t r = web_q_vu_dispatch(p->a, (const uint64_t *)(p + 1));
                if (waited) q->result = r;
            } else put2(WQ_REF, (uint32_t)(uintptr_t)p);
        } else if (kind != WQ_PAD) {
            /* what build_graphics.py's web_x_* wrappers do around each call */
            web_graphics_enter();
            if (p->kind & WQ_VU_READY) web_vu_ready();
            const uint32_t *words = (const uint32_t *)(p + 1);
            switch (kind) {
            case WQ_VIF:
                vif_feed((int)p->a, words, p->b);
                break;
            case WQ_GRAPHICS:   /* host_graphics.c's host_service_graphics_feed */
                if (p->b) {
                    if (p->a == 2) put2(WQ_REF, (uint32_t)(uintptr_t)p);   /* PATH3: the GS's */
                    else vif_feed((int)p->a, words, p->b);
                }
                break;
            case WQ_FRAME:   /* both workers' kernel_vsync_count */
                web_frame(*(const uint64_t *)words);
                put2(WQ_REF, (uint32_t)(uintptr_t)p);
                break;
            case WQ_VSYNC:
                put2(WQ_REF, (uint32_t)(uintptr_t)p);
                break;
            case WQ_ENV:   /* both workers' environment */
                answer = env_line(q, p);
                put2(WQ_REF, (uint32_t)(uintptr_t)p);
                break;
            default:
                __builtin_trap();
            }
        }
        read_at = end;
        if (waited || end - marked >= WEB_Q_RING_BYTES / 4) mark(end);
        if (answer >= 0) return answer;
    }
}

#else
/* ---- the GS worker (graphics.wasm, www/graphics.js): the second ring's consumer ---- */
uint32_t web_display(void);
int web_present(uint32_t canvas_w, uint32_t canvas_h);
extern const uint64_t *web_gs_mmio_snapshot;
uint64_t web_q_dispatch(uint32_t id, const uint64_t *args);   /* build_graphics.py's web_exports.c */

/* Once, before the VU worker's web_vq_init: the second ring's address for the VU worker. The
   reactor's own FP control word (WEB_Q_REACTOR_CSR) is set to the GS front end's IEEE mode here
   and stays in it between calls (graphics_gif_feed under --exact-float switches it and back), so
   no packet switches it, unlike the one-worker build's web_x_* wrappers on the core's word. */
__attribute__((export_name("web_q_init"))) uint32_t web_q_init(void)
{
    host_fp_set_csr(0x1f80);
    store(&WQ2->ring, (uint32_t)(uintptr_t)web_ring);
    return WEB_Q_BLOCK2;
}

/* A first-ring packet the VU worker passed on (WQ_REF): a GS call, PATH3, the field, the vsync or
   the environment, run as the one-worker build runs them. */
static int run_ref(const WebPacket *p)
{
    uint32_t kind = p->kind & WQ_KIND;
    if (kind == WQ_CALL) {
        /* the web_x_* wrapper folds the addresses */
        uint64_t r = web_q_dispatch(p->a, (const uint64_t *)(p + 1));
        if (p->b) WQ->result = r;
        return -1;
    }
    web_graphics_enter();
    const uint64_t *args = (const uint64_t *)(p + 1);
    switch (kind) {
    case WQ_GRAPHICS:   /* PATH3 (unit 2): host_graphics.c's host_service_graphics_feed */
        graphics_gif_feed(3, (const uint32_t *)args, p->b);
        return -1;
    case WQ_FRAME:
        web_frame(args[0]);
        return -1;
    case WQ_VSYNC:
        web_gs_mmio_snapshot = args;
        WQ2->display = web_display();
        web_gs_mmio_snapshot = 0;
        return WQ_RUN_PRESENT;
    case WQ_ENV:
        return env_line(WQ2, p);
    }
    __builtin_trap();
}

/* Runs the second ring's packets until it is empty (WQ_RUN_EMPTY: the JS loop waits on its
   written, sleeping is set), a vsync was drawn (WQ_RUN_PRESENT: display is web_display's answer;
   the loop shows it and ends its task) or the environment changed (WQ_RUN_ENV: env_text,
   env_bytes), all in the second block. Called through JSPI's promising: a readback inside
   suspends it. */
__attribute__((export_name("web_q_run"))) int web_q_run(void)
{
    WebQueue *q = WQ2;
    store(&q->sleeping, 0);
    uint32_t at = load(&q->consumed);
    for (;;) {
        if (load(&q->written) == at) {
            for (unsigned spin = 0; spin < 32768 && load(&q->written) == at; spin++) {}
            if (load(&q->written) == at) {
                store(&q->sleeping, 1);
                if (load(&q->written) == at) return WQ_RUN_EMPTY;
                store(&q->sleeping, 0);
            }
            continue;
        }
        const WebPacket *p = packet_in(q, at);
        uint32_t bytes = p->bytes;
        int answer = -1;
        switch (p->kind & WQ_KIND) {
        case WQ_PAD:
            break;
        case WQ_GIF:   /* host_graphics.c's: gs_feed (or gs_mesh), with the kick's shadow flag */
            graphics_provenance.shadow = (p->kind & WQ_SHADOW) != 0;
            graphics_gif_feed(p->a, (const uint32_t *)(p + 1), p->b);
            graphics_provenance.shadow = 0;
            break;
        case WQ_REF:
            answer = run_ref((const WebPacket *)(uintptr_t)p->a);
            break;
        case WQ_MARK:   /* both workers are done with the first ring up to here */
            retire(WQ, p->a);
            break;
        default:
            __builtin_trap();
        }
        at += bytes;
        retire(q, at);
        if (answer >= 0) return answer;
    }
}

/* The vsync's picture onto the canvas (worker.js does this in the one-worker build, in IEEE mode,
   which the reactor's word is in at rest). */
__attribute__((export_name("web_q_present"))) int web_q_present(uint32_t canvas_w, uint32_t canvas_h)
{
    return web_present(canvas_w, canvas_h);
}
#endif
#endif
