/* The exact GS front end: the CPU half of the WebGPU backends, once (finding 084; the Rust renderer
   plan, D1). Included by the two providers of the same device seam: port/gs/gpu_wgpu.c (wgpu-native,
   the native build) and port/web/gpu_js_web.c (the browser's WebGPU, behind port/web/www/gpu.js).

   Primitive setup, page tracking, W5 batching (one submit's arenas; a dispatch covers the tiles a
   batch touches), the scanout constants, and the rules for when work reaches the GPU live here. The
   including file includes gs.h, gpu.h, stdint.h, stdlib.h and string.h first, and defines these
   device ops as static functions. This file declares their prototypes and calls nothing else on the
   device:

     op_write(gpu, buffer, offset, data, bytes)            queue write of bytes into a buffer
     op_copy(gpu, from, from_offset, to, to_offset, bytes) copy in the command stream
     op_batch(gpu, offset, x, y)                           main over one batch record (its dynamic
                                                           offset), in the open compute pass
     op_record(gpu, pipeline, x, y) -> int                 one other dispatch: P_COPY, or P_SCANOUT
                                                           with the picture's size in the high halves
                                                           of x and y; 0 when the display can't be made
     op_submit(gpu)                                        end the open pass, submit the commands
     op_read(gpu, buffer, want, image)                     map the wanted pages of a buffer (their
                                                           copies were submitted) and copy them into
                                                           image; waits for the GPU

   and four host hooks that are not device calls:

     op_clock() -> uint64_t            the profile clock (0 where there is none)
     op_clip(gpu, box) -> int          the native renderer's probe window: 1 and box[4] when on
     op_draw_skipped(g) -> int         the DHD_DRAW_SKIP_* diagnostics (0 where there are none)

   Buffers and pipelines are numbered as the browser's gpu.js: B_VRAM ... B_UPLOAD, P_COPY, P_SCANOUT.

   DHD_GS_OPTRACE: a provider that defines EXACT_OPTRACE before including this file gets the device-op
   recorder (port/render/OPTRACE.md; below), defines one more host hook, op_adapter (the trace
   header's adapter and raster options), and calls exact_trace_capture after each capture (a no-op
   macro in a build without EXACT_OPTRACE, so the provider may call it unconditionally). */

enum { B_VRAM, B_SNAPSHOT, B_STATES, B_PRIMITIVES, B_BINS, B_DISPLAY, B_COPY, B_READBACK, B_STAGED,
       B_BATCHES, B_UPLOAD };
enum { P_COPY = 1, P_SCANOUT = 2 };

/* raster.wgsl's State: one GS state (registers, CLUT) that primitives point at. */
typedef struct {
    uint32_t reg[128][4], info[4], palette[256];
} Constants;

/* MAX_DRAWS primitives per dispatch (DHD_GS_BATCH lowers it); the rest bound one submit's arenas. */
enum { MAX_DRAWS = 256, MAX_PRIMITIVES = 16384, MAX_STATES = 1024, MAX_BATCHES = 4096,
       MAX_BIN_WORDS = 4 * 1024 * 1024, BATCH_STRIDE = 256 };
typedef struct {
    uint32_t position[3][4], uv[3][4];
    float stq[3][4];
    uint32_t info[4], bounds[4];
    uint32_t depth[8];
} Primitive;

/* The front end's state. A provider's state starts with one of these (g->backend points at it). */
typedef struct Exact {
    /* one submit's arenas and the open batch */
    Primitive *primitive_data;
    Constants *state_data;
    uint32_t *bin_data, *batch_data, *grid;
    unsigned primitive_count, state_count, bin_words, batch_count;
    uint32_t upload_used[16];   /* the upload buffer's pages this submit already copies from */
    unsigned batch_first, memberships, batch_x0, batch_y0, batch_x1, batch_y1;
    uint64_t batch_frame, batch_zbuf;
    uint32_t batch_pages[16];
    Constants state;
    unsigned state_index;       /* gpu->state's place in state_data, or ~0u if not there yet */
    uint32_t state_pages[16];
    uint64_t state_frame, state_zbuf;
    /* page tracking: the pages the GPU wrote (cpu_dirty) and the texture's pages */
    uint32_t cpu_dirty[16], texture_dirty[16], texture_pages[16];
    uint64_t texture_key, clamp_key, tex1_key, mip_key[2];
    int texture_valid, batch_safe;
    uint64_t state_version;
    unsigned state_context;
    uint64_t uploaded;
    /* The native renderer's Z probe device: CPU-written pages go up only when a draw reads or
       writes them (draw's upload_pages want); the others wait in g->host_dirty. */
    int upload_lazily;
    int dirty;                  /* the GPU has written pages the CPU has not read back */
    int recorded;               /* commands are waiting for a submit */
    int display_valid;          /* the scanout picture is in the display texture */
    unsigned dispatches, submits, batch_limit;
    int profile;
    uint64_t draw_ticks, sync_ticks, draws, snapshots, uploads, copies, reads;
    GsGpuRasterStats raster_stats;
#ifdef EXACT_OPTRACE
    struct ExactTrace *trace;   /* the DHD_GS_OPTRACE recording of this instance, or NULL */
#endif
} Exact;

static void op_write(Exact *gpu, int buffer, uint32_t offset, const void *data, uint32_t bytes);
static void op_copy(Exact *gpu, int from, uint32_t from_offset, int to, uint32_t to_offset, uint32_t bytes);
static void op_batch(Exact *gpu, uint32_t offset, uint32_t x, uint32_t y);
static int op_record(Exact *gpu, int pipeline, uint32_t x, uint32_t y);
static void op_submit(Exact *gpu);
static void op_read(Exact *gpu, int buffer, const uint32_t want[16], void *image);
static uint64_t op_clock(void);
static int op_clip(const Exact *gpu, int box[4]);
static int op_draw_skipped(const Gs *g);

/* ---- DHD_GS_OPTRACE: the device-op recorder (port/render/OPTRACE.md, version 1) ----

   Off unless DHD_GS_OPTRACE=<path> is set: then the first exact instance of the process writes <path>
   and instance N (counted from 0) writes <path>.N. Every op the front end makes through the seam is
   one record, in order. A READ record holds the pages op_read returned, copied after the op, so the
   program sees what it sees unrecorded. DHD_GS_OPTRACE_LABEL is the caller's label in the header's
   source (after the program's base name). The build may pass DHD_GS_OPTRACE_SHA, the git blob SHA-1
   of this file. A trace ends with END when its instance is destroyed (after DESTROY), or at process
   exit. Nothing in a trace depends on time, addresses, paths or uninitialised bytes, so one program
   run twice on one adapter records the same bytes. When off, each op costs one branch. */
#ifdef EXACT_OPTRACE
#include <stdio.h>
#include "../host/host_end.h"

/* The header's adapter fields (OPTRACE.md), from the provider's op_adapter. */
typedef struct {
    uint32_t raster_flags;      /* DHDR_RASTER_*: 1 early depth, 2 address shift, 4 exclusive word store */
    uint32_t api;               /* 1 DX12, 2 Vulkan, 3 browser WebGPU, 0 unknown */
    uint32_t vendor_id, device_id;
    uint32_t compiler;          /* 0 none / unknown, 1 FXC, 2 DXC */
    uint32_t fallback;          /* 1 on a CPU / fallback adapter (WARP) */
    uint64_t driver_version;    /* the DXGI UMD version, or 0 */
    char name[128];             /* UTF-8, NUL-terminated here (the trace has no NUL) */
} ExactAdapter;

static void op_adapter(const Exact *gpu, ExactAdapter *adapter);

enum { TRACE_WRITE = 1, TRACE_COPY, TRACE_BATCH, TRACE_RECORD, TRACE_SUBMIT, TRACE_READ, TRACE_CAPTURE,
       TRACE_DESTROY, TRACE_END = 15 };

typedef struct ExactTrace {
    FILE *file;
    uint64_t fnv, records;
    uint32_t counts[8];
    struct ExactTrace *next;    /* the traces still open, for the exit handler */
} ExactTrace;

static ExactTrace *trace_list;
static unsigned trace_instances;

static void trace_fail(const char *what)
{
    fprintf(stderr, "gs: DHD_GS_OPTRACE: %s\n", what);
    exit(2);
}

static void trace_put(FILE *f, const void *data, size_t bytes)
{
    if (bytes && fwrite(data, 1, bytes, f) != bytes) trace_fail("trace write failed");
}

static void trace_bytes(ExactTrace *t, const void *data, size_t bytes)
{
    const uint8_t *p = data;
    uint64_t h = t->fnv;
    for (size_t i = 0; i < bytes; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    t->fnv = h;
    trace_put(t->file, data, bytes);
}

/* One record: tag, payload size, the head words, the tail bytes, zero padding to 4 bytes. */
static void trace_record(ExactTrace *t, uint32_t tag, const uint32_t *head, unsigned words,
                         const void *tail, size_t tail_bytes)
{
    static const uint8_t zero[4];
    if (tail_bytes > UINT32_MAX - 4 * words) trace_fail("record too large");
    uint32_t prefix[2] = {tag, (uint32_t)(words * 4 + tail_bytes)};
    trace_bytes(t, prefix, sizeof prefix);
    trace_bytes(t, head, words * 4);
    trace_bytes(t, tail, tail_bytes);
    trace_bytes(t, zero, (4 - tail_bytes % 4) % 4);
    t->records++;
    if (tag < 8) t->counts[tag]++;
}

/* END (the records before it, the FNV-1a 64 of every record byte before its tag, the counts per
   tag), then the file closes. */
static void trace_finish(ExactTrace *t)
{
    uint32_t end[2 + 2 + 2 + 8];
    end[0] = TRACE_END; end[1] = 8 + 8 + sizeof t->counts;
    memcpy(end + 2, &t->records, 8);
    memcpy(end + 4, &t->fnv, 8);
    memcpy(end + 6, t->counts, sizeof t->counts);
    trace_put(t->file, end, sizeof end);
    if (fclose(t->file)) trace_fail("trace close failed");
    for (ExactTrace **link = &trace_list; *link; link = &(*link)->next)
        if (*link == t) { *link = t->next; break; }
    free(t);
}

static void trace_exit(void)
{
    while (trace_list) trace_finish(trace_list);
}

static void trace_u32(uint8_t *at, uint32_t v) { memcpy(at, &v, 4); }

/* A new instance's trace and its header (OPTRACE.md), when DHD_GS_OPTRACE is set. */
static void trace_open(Exact *gpu)
{
    static int checked, exit_handler;
    static const char *path, *label;
    if (!checked) {
        checked = 1;
        path = getenv("DHD_GS_OPTRACE");
        label = getenv("DHD_GS_OPTRACE_LABEL");
        if (path && !*path) path = NULL;
    }
    if (!path || gpu->trace) return;
    char name[1024];
    unsigned instance = trace_instances++;
    if (instance) snprintf(name, sizeof name, "%s.%u", path, instance);
    else snprintf(name, sizeof name, "%s", path);
    ExactTrace *t = calloc(1, sizeof *t);
    if (!t || !(t->file = fopen(name, "wb"))) trace_fail("cannot write the trace");
    setvbuf(t->file, NULL, _IOFBF, 1 << 20);
    t->fnv = 0xcbf29ce484222325ull;
    if (!exit_handler) { exit_handler = 1; host_end_add(HOST_END_REPORT, "op trace", trace_exit); }
    t->next = trace_list; trace_list = t;

    ExactAdapter a;
    memset(&a, 0, sizeof a);
    op_adapter(gpu, &a);
    a.name[sizeof a.name - 1] = 0;
    /* source: the program's base name (no directory, no .exe), then the caller's label */
    char program[260] = "", source[512];
#ifdef _WIN32
    char module[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, module, sizeof module);
    if (length && length < sizeof module) {
        const char *base = module;
        for (const char *c = module; *c; c++) if (*c == '\\' || *c == '/') base = c + 1;
        snprintf(program, sizeof program, "%s", base);
        size_t n = strlen(program);
        if (n > 4 && !_stricmp(program + n - 4, ".exe")) program[n - 4] = 0;
    }
#endif
    snprintf(source, sizeof source, "%s%s%s", program, label && *label && *program ? " " : "", label ? label : "");
    /* recorder_sha: 40 hex digits (lower case), or 40 '0' when the build did not pass them */
    char sha[40];
    memset(sha, '0', sizeof sha);
#ifdef DHD_GS_OPTRACE_SHA
#define TRACE_TEXT_(x) #x
#define TRACE_TEXT(x) TRACE_TEXT_(x)
    {
        const char *given = TRACE_TEXT(DHD_GS_OPTRACE_SHA);
        char digits[40];
        unsigned n = 0;
        for (const char *c = given; *c && n <= 40; c++) {
            if (*c == '"') continue;
            int hex = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F');
            if (!hex) { n = 41; break; }
            if (n < 40) digits[n] = *c >= 'A' && *c <= 'F' ? (char)(*c - 'A' + 'a') : *c;
            n++;
        }
        if (n == 40) memcpy(sha, digits, sizeof sha);
    }
#endif
    uint32_t name_bytes = (uint32_t)strlen(a.name), source_bytes = (uint32_t)strlen(source);
    uint32_t header_bytes = (88 + 4 + name_bytes + 4 + source_bytes + 3) & ~3u;
    uint8_t header[88 + 4 + sizeof a.name + 4 + sizeof source + 4];
    memset(header, 0, sizeof header);
    memcpy(header, "DHDOPTR", 8);
    trace_u32(header + 8, 1);
    trace_u32(header + 12, header_bytes);
    trace_u32(header + 16, a.raster_flags);
    trace_u32(header + 20, a.api);
    trace_u32(header + 24, a.vendor_id);
    trace_u32(header + 28, a.device_id);
    trace_u32(header + 32, a.compiler);
    trace_u32(header + 36, a.fallback);
    memcpy(header + 40, &a.driver_version, 8);
    memcpy(header + 48, sha, sizeof sha);
    trace_u32(header + 88, name_bytes);
    memcpy(header + 92, a.name, name_bytes);
    trace_u32(header + 92 + name_bytes, source_bytes);
    memcpy(header + 96 + name_bytes, source, source_bytes);
    trace_put(t->file, header, header_bytes);   /* not part of END's fnv */
    gpu->trace = t;
}

/* The instance is going away (exact_free): DESTROY, then END. */
static void trace_destroy(Exact *gpu)
{
    if (!gpu->trace) return;
    trace_record(gpu->trace, TRACE_DESTROY, NULL, 0, NULL, 0);
    trace_finish(gpu->trace);
    gpu->trace = NULL;
}

/* The provider's capture of the display texture returned pixels (width x height RGBA8, rows
   unpadded); width = height = 0 when there was no display texture. */
static void exact_trace_capture(Exact *gpu, unsigned width, unsigned height, const uint32_t *pixels)
    __attribute__((unused));
static void exact_trace_capture(Exact *gpu, unsigned width, unsigned height, const uint32_t *pixels)
{
    if (!gpu || !gpu->trace) return;
    uint32_t head[2] = {width, height};
    trace_record(gpu->trace, TRACE_CAPTURE, head, 2, pixels, (size_t)width * height * 4);
}

/* The device ops, recorded. */
static void dev_write(Exact *gpu, int buffer, uint32_t offset, const void *data, uint32_t bytes)
{
    if (gpu->trace) {
        uint32_t head[3] = {(uint32_t)buffer, offset, bytes};
        trace_record(gpu->trace, TRACE_WRITE, head, 3, data, bytes);
    }
    op_write(gpu, buffer, offset, data, bytes);
}

static void dev_copy(Exact *gpu, int from, uint32_t from_offset, int to, uint32_t to_offset, uint32_t bytes)
{
    if (gpu->trace) {
        uint32_t head[5] = {(uint32_t)from, from_offset, (uint32_t)to, to_offset, bytes};
        trace_record(gpu->trace, TRACE_COPY, head, 5, NULL, 0);
    }
    op_copy(gpu, from, from_offset, to, to_offset, bytes);
}

static void dev_batch(Exact *gpu, uint32_t offset, uint32_t x, uint32_t y)
{
    if (gpu->trace) {
        uint32_t head[3] = {offset, x, y};
        trace_record(gpu->trace, TRACE_BATCH, head, 3, NULL, 0);
    }
    op_batch(gpu, offset, x, y);
}

static int dev_record(Exact *gpu, int pipeline, uint32_t x, uint32_t y)
{
    if (gpu->trace) {
        uint32_t head[3] = {(uint32_t)pipeline, x, y};
        trace_record(gpu->trace, TRACE_RECORD, head, 3, NULL, 0);
    }
    return op_record(gpu, pipeline, x, y);
}

static void dev_submit(Exact *gpu)
{
    if (gpu->trace) trace_record(gpu->trace, TRACE_SUBMIT, NULL, 0, NULL, 0);
    op_submit(gpu);
}

static void dev_read(Exact *gpu, int buffer, const uint32_t want[16], void *image)
{
    op_read(gpu, buffer, want, image);
    if (!gpu->trace) return;
    uint32_t head[18];
    unsigned pages = 0, n = 0;
    head[0] = (uint32_t)buffer;
    memcpy(head + 1, want, 64);
    head[17] = 1;   /* op_read does not fail */
    for (unsigned page = 0; page < 512; page++) pages += want[page / 32] >> (page & 31) & 1;
    uint8_t *bytes = malloc((size_t)pages * 8192 + 1);
    if (!bytes) trace_fail("out of memory");
    for (unsigned page = 0; page < 512; page++)
        if (want[page / 32] >> (page & 31) & 1)
            memcpy(bytes + (size_t)n++ * 8192, (const uint8_t *)image + (size_t)page * 8192, 8192);
    trace_record(gpu->trace, TRACE_READ, head, 18, bytes, (size_t)pages * 8192);
    free(bytes);
}
#else
#define dev_write op_write
#define dev_copy op_copy
#define dev_batch op_batch
#define dev_record op_record
#define dev_submit op_submit
#define dev_read op_read
#define trace_open(gpu) ((void)0)
#define trace_destroy(gpu) ((void)0)
#define exact_trace_capture(gpu, width, height, pixels) ((void)0)
#endif

static void close_batch(Exact *gpu);

/* The open batch closed and commands recorded: the next submit has work. */
static void exact_commands(Exact *gpu)
{
    close_batch(gpu);
    gpu->recorded = 1;
}

/* One dispatch of another pipeline, in the command stream. */
static int record(Exact *gpu, int pipeline, unsigned x, unsigned y)
{
    exact_commands(gpu);
    return dev_record(gpu, pipeline, x, y);
}

/* A flush point: everything recorded so far, with its arenas, goes to the GPU. */
static void submit_all(Exact *gpu)
{
    if (!gpu->recorded && gpu->primitive_count == gpu->batch_first) return;
    exact_commands(gpu);
    if (gpu->state_count) dev_write(gpu, B_STATES, 0, gpu->state_data, gpu->state_count * sizeof(Constants));
    if (gpu->primitive_count)
        dev_write(gpu, B_PRIMITIVES, 0, gpu->primitive_data, gpu->primitive_count * sizeof(Primitive));
    if (gpu->bin_words) dev_write(gpu, B_BINS, 0, gpu->bin_data, gpu->bin_words * 4);
    if (gpu->batch_count) dev_write(gpu, B_BATCHES, 0, gpu->batch_data, gpu->batch_count * BATCH_STRIDE);
    dev_submit(gpu);
    gpu->recorded = 0;
    gpu->primitive_count = gpu->state_count = gpu->bin_words = gpu->batch_count = gpu->batch_first = 0;
    gpu->state_index = ~0u;
    memset(gpu->upload_used, 0, sizeof gpu->upload_used);
    gpu->submits++;
}

/* Contiguous runs of the pages in want, as copies from VRAM into the same offsets of target. */
static void copy_pages(Exact *gpu, int target, const uint32_t want[16])
{
    exact_commands(gpu);
    for (unsigned page = 0; page < 512;) {
        if (!(want[page / 32] & (1u << (page & 31)))) { page++; continue; }
        unsigned end = page;
        while (end < 512 && want[end / 32] & (1u << (end & 31))) end++;
        dev_copy(gpu, B_VRAM, page * 8192, target, page * 8192, (end - page) * 8192);
        page = end;
    }
}

/* ---- gpu.c's CPU half, over the ops ---- */

static void sync_vram(Gs *g)
{
    Exact *gpu = g->backend;
    if (!gpu || !gpu->dirty) return;
    uint64_t start = gpu->profile ? op_clock() : 0;
    /* Only the GPU-written pages come back: unuploaded CPU changes on the others survive. */
    copy_pages(gpu, B_READBACK, gpu->cpu_dirty);
    submit_all(gpu);
    dev_read(gpu, B_READBACK, gpu->cpu_dirty, g->vram);
    gpu->reads++;
    gpu->dirty = 0;
    memset(gpu->cpu_dirty, 0, sizeof gpu->cpu_dirty);
    if (gpu->profile) gpu->sync_ticks += op_clock() - start;
}

/* Reads back only the GPU-written pages in the range (gpu.c's sync_range). */
static void sync_range(Gs *g, unsigned address, unsigned bytes)
{
    Exact *gpu = g->backend;
    uint32_t want[16] = {0};
    int any = 0, left = 0;
    if (!gpu || !gpu->dirty || !bytes) return;
    for (unsigned p = address / 8192; p <= (address + bytes - 1) / 8192 && p < address / 8192 + 512; p++) {
        unsigned page = p & 511;
        if (gpu->cpu_dirty[page / 32] & (1u << (page & 31))) want[page / 32] |= 1u << (page & 31), any = 1;
    }
    if (!any) return;
    uint64_t start = gpu->profile ? op_clock() : 0;
    copy_pages(gpu, B_READBACK, want);
    submit_all(gpu);
    dev_read(gpu, B_READBACK, want, g->vram);
    gpu->reads++;
    for (unsigned i = 0; i < 16; i++) left |= (gpu->cpu_dirty[i] &= ~want[i]) != 0;
    gpu->dirty = left;
    if (gpu->profile) gpu->sync_ticks += op_clock() - start;
}

/* Deferred reads (gs_defer_read_image): the copies go in order with the draws now; fetch_pages
   submits them if they are still waiting and reads the staged pages when the data is needed. */
static void stage_pages(Gs *g, const uint32_t want[16], uint32_t staged[16])
{
    Exact *gpu = g->backend;
    int any = 0;
    for (unsigned i = 0; i < 16; i++) any |= (staged[i] = gpu->dirty ? want[i] & gpu->cpu_dirty[i] : 0) != 0;
    if (!any) return;
    copy_pages(gpu, B_STAGED, staged);
}

static void fetch_pages(Gs *g, const uint32_t staged[16], uint32_t *image)
{
    Exact *gpu = g->backend;
    uint64_t start = gpu->profile ? op_clock() : 0;
    submit_all(gpu);
    dev_read(gpu, B_STAGED, staged, image);
    gpu->reads++;
    if (gpu->profile) gpu->sync_ticks += op_clock() - start;
}

/* CPU-written pages (g->host_dirty) to the GPU's VRAM: all of them, or with upload_lazily only
   those in want (NULL: all). A page the GPU reads or writes is always uploaded first, so a page
   still waiting is never GPU-written (cpu_dirty) and the readbacks never need it. */
static void upload_pages(Gs *g, const uint32_t *want)
{
    Exact *gpu = g->backend;
    if (gpu->uploaded == g->pixels_written) return;
    unsigned any = 0;
    uint32_t pages[16];
    for (unsigned i = 0; i < 16; i++) any |= g->host_dirty[i];
    /* Initial contents and explicit whole-image writes have no page range. */
    if (gpu->uploaded == UINT64_MAX || !any) memset(g->host_dirty, 255, sizeof g->host_dirty), want = NULL;
    if (!gpu->upload_lazily) want = NULL;
    any = 0;
    for (unsigned i = 0; i < 16; i++) any |= pages[i] = want ? g->host_dirty[i] & want[i] : g->host_dirty[i];
    if (!any) return;
    /* The queue writes land before the submit: a page of the upload buffer serves one copy each. */
    for (unsigned i = 0; i < 16; i++) if (pages[i] & gpu->upload_used[i]) { submit_all(gpu); break; }
    exact_commands(gpu);
    for (unsigned page = 0; page < 512;) {
        if (!(pages[page / 32] & (1u << (page & 31)))) { page++; continue; }
        unsigned first = page++;
        while (page < 512 && (pages[page / 32] & (1u << (page & 31)))) page++;
        dev_write(gpu, B_UPLOAD, first * 8192, (uint8_t *)g->vram + first * 8192, (page - first) * 8192);
        dev_copy(gpu, B_UPLOAD, first * 8192, B_VRAM, first * 8192, (page - first) * 8192);
    }
    any = 0;
    for (unsigned i = 0; i < 16; i++) {
        gpu->texture_dirty[i] |= pages[i]; gpu->upload_used[i] |= pages[i];
        any |= g->host_dirty[i] &= ~pages[i];
    }
    if (!any) gpu->uploaded = g->pixels_written;
    gpu->uploads++;
}

static int readback_all(Gs *g, uint32_t *image) __attribute__((unused));
static int readback_all(Gs *g, uint32_t *image)
{
    Exact *gpu = g ? g->backend : NULL;
    uint32_t all[16];
    if (!gpu || !image) return 0;
    upload_pages(g, NULL);
    memset(all, 255, sizeof all);
    copy_pages(gpu, B_READBACK, all);
    submit_all(gpu);
    dev_read(gpu, B_READBACK, all, image);
    return 1;
}

static void texture_pages(Exact *gpu, Gs *g, unsigned ctxt)
{
    uint64_t tex = g->reg[GS_TEX0_1 + ctxt], clamp = g->reg[GS_CLAMP_1 + ctxt];
    uint64_t tex1 = g->reg[GS_TEX1_1 + ctxt];
    uint64_t mip[2] = {g->reg[GS_MIPTBP1_1 + ctxt], g->reg[GS_MIPTBP2_1 + ctxt]};
    if (gpu->texture_valid && gpu->texture_key == tex && gpu->clamp_key == clamp &&
        gpu->tex1_key == tex1 && gpu->mip_key[0] == mip[0] && gpu->mip_key[1] == mip[1]) return;
    unsigned mmin = (unsigned)(tex1 >> 6) & 7, levels = (unsigned)(tex1 >> 2) & 7;
    if (levels > 6) levels = 6;
    if (mmin < 2 || mmin > 5) levels = 0;
    memset(gpu->texture_pages, 0, sizeof gpu->texture_pages);
    for (unsigned level = 0; level <= levels; level++) {
        unsigned lo[2] = {0,0}, hi[2];
        for (unsigned axis = 0; axis < 2; axis++) {
            unsigned mode = (unsigned)(clamp >> (axis * 2)) & 3;
            unsigned minimum = ((unsigned)(clamp >> (4 + axis * 20)) & 1023) >> level;
            unsigned maximum = ((unsigned)(clamp >> (14 + axis * 20)) & 1023) >> level;
            unsigned size = (unsigned)(tex >> (26 + axis * 4)) & 15;
            size = size > level ? size - level : 0;
            hi[axis] = size >= 11 ? 2047 : (1u << size) - 1;
            if (mode == 2) { lo[axis] = minimum; hi[axis] = maximum; }
            if (mode == 3) { lo[axis] = maximum; hi[axis] = maximum | minimum; }
            if (lo[axis] > hi[axis]) { lo[axis] = 0; hi[axis] = 2047; }
        }
        uint64_t location = level ? mip[(level - 1) / 3] >> (((level - 1) % 3) * 20) : tex;
        gs_mark_pages(gpu->texture_pages, (unsigned)(tex >> 20) & 63, (unsigned)location & 16383,
                      (unsigned)(location >> 14) & 63, lo[0], lo[1], hi[0], hi[1]);
    }
    gpu->texture_key = tex; gpu->clamp_key = clamp; gpu->texture_valid = 1;
    gpu->tex1_key = tex1; gpu->mip_key[0] = mip[0]; gpu->mip_key[1] = mip[1];
}

static int minimum(int a, int b) { return a < b ? a : b; }
static int maximum(int a, int b) { return a > b ? a : b; }

static int independent_pixels(Gs *g, unsigned ctxt)
{
    uint64_t frame = g->reg[GS_FRAME_1 + ctxt], z = g->reg[GS_ZBUF_1 + ctxt];
    uint64_t scissor = g->reg[GS_SCISSOR_1 + ctxt];
    unsigned bw = (unsigned)(frame >> 16) & 63;
    unsigned x = (unsigned)(scissor >> 16) & 2047, y = (unsigned)(scissor >> 48) & 2047;
    /* Aliased rows or overlapping color/Z surfaces can communicate across
       pixels. Keep a dispatch boundary between such primitives. */
    if (!bw || x >= bw * 64 || (y / 32 + 1) * bw > 512) return 0;
    uint64_t test = g->reg[GS_TEST_1 + ctxt];
    if ((z >> 32 & 1) && (!(test >> 16 & 1) || (test >> 17 & 3) <= 1)) return 1;
    uint32_t color[16] = {0}, depth[16] = {0};
    gs_mark_pages(color, (unsigned)(frame >> 24) & 63, ((unsigned)frame & 511)*32, bw, 0,0,x,y);
    gs_mark_pages(depth, 48 | ((unsigned)(z >> 24) & 15), ((unsigned)z & 511)*32, bw, 0,0,x,y);
    for (unsigned i = 0; i < 16; i++) if (color[i] & depth[i]) return 0;
    return 1;
}

/* The open batch as one dispatch over the tiles its primitives touch: each touched 8x8 tile gets
   an entry (position, first word, count) and its primitives' indices in submission order. */
static void close_batch(Exact *gpu)
{
    unsigned first = gpu->batch_first, last = gpu->primitive_count;
    if (first == last) return;
    unsigned x0 = gpu->batch_x0, y0 = gpu->batch_y0;
    unsigned width = gpu->batch_x1 - x0 + 1, tiles = width * (gpu->batch_y1 - y0 + 1);
    uint32_t *grid = gpu->grid, *bins = gpu->bin_data;
    memset(grid, 0, tiles * 4);
    for (unsigned i = first; i < last; i++) {
        const uint32_t *b = gpu->primitive_data[i].bounds;
        unsigned right = (b[0] + b[2] - 1) / 8, bottom = (b[1] + b[3] - 1) / 8;
        for (unsigned y = b[1] / 8; y <= bottom; y++)
            for (unsigned x = b[0] / 8; x <= right; x++) grid[(y - y0) * width + x - x0]++;
    }
    unsigned base = gpu->bin_words, touched = 0;
    for (unsigned t = 0; t < tiles; t++) touched += grid[t] != 0;
    unsigned entry = base, list = base + touched * 3;
    for (unsigned t = 0; t < tiles; t++) {
        if (!grid[t]) continue;
        bins[entry] = (x0 + t % width) | (y0 + t / width) << 16;
        bins[entry + 1] = list; bins[entry + 2] = 0;
        list += grid[t];
        grid[t] = entry; /* from here on: the tile's entry */
        entry += 3;
    }
    for (unsigned i = first; i < last; i++) {
        const uint32_t *b = gpu->primitive_data[i].bounds;
        unsigned right = (b[0] + b[2] - 1) / 8, bottom = (b[1] + b[3] - 1) / 8;
        for (unsigned y = b[1] / 8; y <= bottom; y++)
            for (unsigned x = b[0] / 8; x <= right; x++) {
                uint32_t *e = bins + grid[(y - y0) * width + x - x0];
                bins[e[1] + e[2]++] = i;
            }
    }
    gpu->bin_words = list;
    uint32_t *header = gpu->batch_data + gpu->batch_count * (BATCH_STRIDE / 4);
    header[0] = touched; header[1] = base; header[2] = header[3] = 0; /* [3] stays 0: keep() */
    unsigned columns = touched < 32768 ? touched : 32768;
    dev_batch(gpu, gpu->batch_count * BATCH_STRIDE, columns, (touched + columns - 1) / columns);
    gpu->recorded = 1;
    gpu->raster_stats.reference_batches++;
    gpu->raster_stats.reference_primitives += last - first;
    gpu->raster_stats.rejected[GS_GPU_HW_UNAVAILABLE]++;
    gpu->batch_count++;
    gpu->batch_first = last;
    gpu->memberships = 0;
    gpu->dispatches++;
}

/* The pages a state's primitives can touch (its frame, and its Z buffer with the Z test on) and
   their layout: primitives of states with the same layout, or with no page in common, can share
   a batch, since then no pixel's word is reached from another tile's pixel. */
static void state_layout(Exact *gpu, Gs *g, unsigned ctxt)
{
    uint64_t frame = g->reg[GS_FRAME_1 + ctxt], z = g->reg[GS_ZBUF_1 + ctxt];
    uint64_t scissor = g->reg[GS_SCISSOR_1 + ctxt];
    unsigned bw = (unsigned)(frame >> 16) & 63;
    memset(gpu->state_pages, 0, sizeof gpu->state_pages);
    gs_mark_pages(gpu->state_pages, (unsigned)(frame >> 24) & 63, ((unsigned)frame & 511) * 32, bw,
                  (unsigned)scissor & 2047, (unsigned)(scissor >> 32) & 2047,
                  (unsigned)(scissor >> 16) & 2047, (unsigned)(scissor >> 48) & 2047);
    if (g->reg[GS_TEST_1 + ctxt] >> 16 & 1)
        gs_mark_pages(gpu->state_pages, 48 | ((unsigned)(z >> 24) & 15), ((unsigned)z & 511) * 32, bw,
                      (unsigned)scissor & 2047, (unsigned)(scissor >> 32) & 2047,
                      (unsigned)(scissor >> 16) & 2047, (unsigned)(scissor >> 48) & 2047);
    gpu->state_frame = frame & 0x3F3F01FFu;
    gpu->state_zbuf = z & 0x0F0001FFu;
}

static int batch_compatible(const Exact *gpu)
{
    if (gpu->state_frame == gpu->batch_frame && gpu->state_zbuf == gpu->batch_zbuf) return 1;
    for (unsigned i = 0; i < 16; i++) if (gpu->state_pages[i] & gpu->batch_pages[i]) return 0;
    return 1;
}

static void draw(Gs *g, unsigned type, const GsVertex *v, unsigned count)
{
    if (op_draw_skipped(g)) return;
    Exact *gpu = g->backend;
    uint64_t start = gpu->profile ? op_clock() : 0;
    Primitive c = {0};
    unsigned prim = (unsigned)((g->reg[GS_PRIM] & 7) |
                    ((g->reg[GS_PRMODECONT] & 1 ? g->reg[GS_PRIM] : g->reg[GS_PRMODE]) & 0x7F8));
    unsigned ctxt = prim >> 9 & 1;
    /* Keep integer Z exact through plane setup and interpolation. GS software
       rendering uses double precision for depth, independently of ST/RGBA. */
    double depth[4] = {v[0].z, 0, 0, 0};
    if (type == 1 || type == 2) {
        int dx = (int)v[1].x - (int)v[0].x, dy = (int)v[1].y - (int)v[0].y;
        unsigned axis = abs(dx) >= abs(dy) ? 1 : 2;
        int distance = axis == 1 ? dx : dy;
        if (distance) depth[axis] = ((double)v[1].z - v[0].z) / distance;
    } else if (count == 3) {
        double dx1 = (int)v[1].x - (int)v[0].x, dy1 = (int)v[1].y - (int)v[0].y;
        double dx2 = (int)v[2].x - (int)v[0].x, dy2 = (int)v[2].y - (int)v[0].y;
        double dz1 = (double)v[1].z - v[0].z, dz2 = (double)v[2].z - v[0].z;
        double area = dx1 * dy2 - dy1 * dx2;
        if (area) { depth[1] = (dz1 * dy2 - dy1 * dz2) / area; depth[2] = (dx1 * dz2 - dz1 * dx2) / area; }
    }
    memcpy(c.depth, depth, sizeof depth);
    uint64_t offset = g->reg[GS_XYOFFSET_1 + ctxt], scissor = g->reg[GS_SCISSOR_1 + ctxt];
    int minx = 65536, miny = 65536, maxx = -65536, maxy = -65536;
    for (unsigned i = 0; i < count; i++) {
        int x = (int)v[i].x - (int)(offset & 65535), y = (int)v[i].y - (int)(offset >> 32 & 65535);
        minx = minimum(minx, x); miny = minimum(miny, y); maxx = maximum(maxx, x); maxy = maximum(maxy, y);
        c.position[i][0] = (uint32_t)x; c.position[i][1] = (uint32_t)y;
        c.position[i][2] = v[i].z; c.position[i][3] = v[i].rgba;
        c.uv[i][0] = v[i].u; c.uv[i][1] = v[i].v; c.uv[i][2] = v[i].fog;
        c.stq[i][0] = v[i].s; c.stq[i][1] = v[i].t; c.stq[i][2] = v[i].q;
    }
    minx = maximum(minx >> 4, (int)(scissor & 2047));
    miny = maximum(miny >> 4, (int)(scissor >> 32 & 2047));
    maxx = minimum((maxx + 15) >> 4, (int)(scissor >> 16 & 2047));
    maxy = minimum((maxy + 15) >> 4, (int)(scissor >> 48 & 2047));
    int clip[4];
    if (op_clip(gpu, clip)) {
        minx = maximum(minx, clip[0]); miny = maximum(miny, clip[1]);
        maxx = minimum(maxx, clip[2]); maxy = minimum(maxy, clip[3]);
    }
    if (minx > maxx || miny > maxy) return;
    if (gpu->state_version != g->state_version || gpu->state_context != ctxt) {
        static const unsigned registers[] = {6,7,8,9,20,21,34,52,53,54,55,59,61,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79};
        for (unsigned r = 0; r < sizeof registers / sizeof *registers; r++) {
            unsigned i = registers[r];
            gpu->state.reg[i][0] = (uint32_t)g->reg[i]; gpu->state.reg[i][1] = (uint32_t)(g->reg[i] >> 32);
        }
        for (unsigned i = 0; i < 256; i++) gpu->state.palette[i] = gs_palette(g, g->reg[GS_TEX0_1 + ctxt], i);
        gpu->state_version = g->state_version; gpu->state_context = ctxt;
        gpu->batch_safe = independent_pixels(g, ctxt);
        gpu->state.info[1] = gpu->batch_safe ? 1u : 0u; /* independent pixels; packed pixels can share words */
        gpu->state_index = ~0u;
        state_layout(gpu, g, ctxt);
    }
    c.bounds[0] = (uint32_t)minx; c.bounds[1] = (uint32_t)miny;
    c.bounds[2] = (uint32_t)(maxx - minx + 1); c.bounds[3] = (uint32_t)(maxy - miny + 1);
    c.info[0] = type; c.info[1] = prim; c.info[2] = count;
    unsigned memberships = ((unsigned)maxx / 8 - (unsigned)minx / 8 + 1) *
                           ((unsigned)maxy / 8 - (unsigned)miny / 8 + 1);
    /* The pages it writes, and those it reads: the frame and Z inside its box, and its texture's. */
    uint32_t written[16] = {0}, want[16] = {0};
    uint64_t frame = g->reg[GS_FRAME_1 + ctxt], zbuf = g->reg[GS_ZBUF_1 + ctxt];
    unsigned bw = (unsigned)(frame >> 16) & 63;
    gs_mark_pages(written, (unsigned)(frame >> 24) & 63, ((unsigned)frame & 511) * 32, bw, minx,miny,maxx,maxy);
    gs_mark_pages(want, 48 | ((unsigned)(zbuf >> 24) & 15), ((unsigned)zbuf & 511) * 32, bw, minx,miny,maxx,maxy);
    if (!(zbuf >> 32 & 1)) for (unsigned i = 0; i < 16; i++) written[i] |= want[i];
    if (prim & 16) texture_pages(gpu, g, ctxt);
    for (unsigned i = 0; i < 16; i++) want[i] |= written[i] | (prim & 16 ? gpu->texture_pages[i] : 0);
    upload_pages(g, want);
    if (prim & 16) {
        for (unsigned i = 0; i < 16; i++) if (gpu->texture_pages[i] & gpu->texture_dirty[i]) {
            exact_commands(gpu);
            dev_copy(gpu, B_VRAM, 0, B_SNAPSHOT, 0, GS_VRAM_SIZE);
            gpu->snapshots++;
            memset(gpu->texture_dirty, 0, sizeof gpu->texture_dirty);
            break;
        }
    }
    /* A batch ends at its size limit, before a primitive whose pixels aren't independent (those
       run alone), and before a state whose pages could meet the batch's in another tile. A
       submit's arenas end it too (a batch's tile list takes at most 4 words per membership). */
    if (gpu->primitive_count != gpu->batch_first &&
        (gpu->primitive_count - gpu->batch_first == gpu->batch_limit || !gpu->batch_safe ||
         !batch_compatible(gpu) || 4 * (gpu->memberships + memberships) > MAX_BIN_WORDS - gpu->bin_words))
        close_batch(gpu);
    if (gpu->primitive_count == MAX_PRIMITIVES || gpu->batch_count == MAX_BATCHES ||
        (gpu->state_index == ~0u && gpu->state_count == MAX_STATES) ||
        4 * memberships > MAX_BIN_WORDS - gpu->bin_words)
        submit_all(gpu);
    if (gpu->state_index == ~0u) {
        gpu->state_index = gpu->state_count++;
        gpu->state_data[gpu->state_index] = gpu->state;
    }
    c.info[3] = gpu->state_index;
    unsigned tx0 = (unsigned)minx / 8, ty0 = (unsigned)miny / 8, tx1 = (unsigned)maxx / 8, ty1 = (unsigned)maxy / 8;
    if (gpu->primitive_count == gpu->batch_first) {
        gpu->batch_x0 = tx0; gpu->batch_y0 = ty0; gpu->batch_x1 = tx1; gpu->batch_y1 = ty1;
        gpu->batch_frame = gpu->state_frame; gpu->batch_zbuf = gpu->state_zbuf;
        memcpy(gpu->batch_pages, gpu->state_pages, sizeof gpu->batch_pages);
    } else {
        gpu->batch_x0 = minimum(gpu->batch_x0, tx0); gpu->batch_y0 = minimum(gpu->batch_y0, ty0);
        gpu->batch_x1 = maximum(gpu->batch_x1, tx1); gpu->batch_y1 = maximum(gpu->batch_y1, ty1);
        for (unsigned i = 0; i < 16; i++) gpu->batch_pages[i] |= gpu->state_pages[i];
    }
    gpu->primitive_data[gpu->primitive_count++] = c;
    gpu->memberships += memberships;
    gpu->dirty = 1;
    for (unsigned i = 0; i < 16; i++) { gpu->cpu_dirty[i] |= written[i]; gpu->texture_dirty[i] |= written[i]; }
    gpu->draws++;
    if (!gpu->batch_safe) close_batch(gpu);
    if (gpu->profile) gpu->draw_ticks += op_clock() - start;
}

/* TRXDIR 2 on the GPU (gpu.c's local_copy). */
static int local_copy(Gs *g)
{
    Exact *gpu = g->backend;
    uint64_t b = g->reg[GS_BITBLTBUF], p = g->reg[GS_TRXPOS], r = g->reg[GS_TRXREG];
    uint32_t c[12] = {0}, from[16] = {0}, to[16] = {0};
    unsigned width = (unsigned)r & 4095, height = (unsigned)(r >> 32) & 4095;
    c[0] = (unsigned)(b >> 24) & 63; c[1] = (unsigned)b & 0x3FFF; c[2] = (unsigned)(b >> 16) & 63;
    c[3] = (unsigned)p & 2047; c[4] = (unsigned)(p >> 16) & 2047;
    c[5] = (unsigned)(b >> 56) & 63; c[6] = (unsigned)(b >> 32) & 0x3FFF; c[7] = (unsigned)(b >> 48) & 63;
    c[8] = width; c[9] = height; c[10] = (unsigned)(p >> 32) & 2047; c[11] = (unsigned)(p >> 48) & 2047;
    if (!gpu || !width || !height || c[3] + width > 2048 || c[4] + height > 2048 ||
        c[10] + width > 2048 || c[11] + height > 2048)
        return 0;
    gs_mark_pages(from, c[0], c[1], c[2], c[3], c[4], c[3] + width - 1, c[4] + height - 1);
    gs_mark_pages(to, c[5], c[6], c[7], c[10], c[11], c[10] + width - 1, c[11] + height - 1);
    for (unsigned i = 0; i < 16; i++) if (from[i] & to[i]) return 0;
    /* Its constants' queue write lands before the next submit: earlier work goes first, and
       until that submit no other write reaches them (each local copy starts with one). */
    upload_pages(g, NULL);
    submit_all(gpu);
    dev_write(gpu, B_COPY, 0, c, sizeof c);
    record(gpu, P_COPY, (width + 7) / 8, (height + 7) / 8);
    for (unsigned i = 0; i < 16; i++) { gpu->cpu_dirty[i] |= to[i]; gpu->texture_dirty[i] |= to[i]; }
    gpu->dirty = 1;
    gpu->copies++;
    return 1;
}

/* gpu.c's and gpu_wgpu.c's display: the scanout into the display texture, submitted. Returns 1 and
   the picture's size, or 0 when nothing is displayed. */
static int exact_display(Gs *g, unsigned *width, unsigned *height)
{
    Exact *gpu = g->backend;
    uint32_t c[12] = {0};
    uint64_t pmode = gs_priv_read(g, GS_PMODE);
    unsigned circuit = pmode & 1 ? 0 : 1;
    uint64_t fb = gs_priv_read(g, circuit ? GS_DISPFB2 : GS_DISPFB1);
    uint64_t display = gs_priv_read(g, circuit ? GS_DISPLAY2 : GS_DISPLAY1);
    c[0] = (unsigned)(fb >> 15) & 31; c[1] = ((unsigned)fb & 511) * 32;
    c[2] = (unsigned)(fb >> 9) & 63; c[3] = (unsigned)(fb >> 32) & 2047; c[4] = (unsigned)(fb >> 43) & 2047;
    c[5] = ((unsigned)(display >> 32) & 4095) + 1; c[5] /= ((unsigned)(display >> 23) & 15) + 1;
    c[6] = ((unsigned)(display >> 44) & 2047) + 1; c[6] /= ((unsigned)(display >> 27) & 3) + 1;
    c[7] = (gs_priv_read(g, GS_SMODE2) & 3) == 3;
    if (!gpu || !(pmode & 3) || !c[2] || !c[5] || !c[6] || c[5] > 1024 || c[6] > 1024) return 0;
    upload_pages(g, NULL);
    submit_all(gpu);
    dev_write(gpu, B_DISPLAY, 0, c, 48);
    if (!record(gpu, P_SCANOUT, (c[5] + 7) / 8 | c[5] << 16, (c[6] + 7) / 8 | c[6] << 16)) return 0;
    submit_all(gpu);
    gpu->display_valid = 1;
    *width = c[5]; *height = c[6];
    return 1;
}

/* A provider's init: the front end's arenas, and its defaults (the batch limit is the provider's). */
static int exact_alloc(Exact *gpu)
{
    gpu->primitive_data = malloc(MAX_PRIMITIVES * sizeof *gpu->primitive_data);
    gpu->state_data = malloc(MAX_STATES * sizeof *gpu->state_data);
    gpu->bin_data = malloc(MAX_BIN_WORDS * sizeof *gpu->bin_data);
    gpu->batch_data = calloc(MAX_BATCHES, BATCH_STRIDE);
    gpu->grid = malloc(256 * 256 * sizeof *gpu->grid); /* a batch's box: 2048x2048 pixels at most */
    gpu->uploaded = UINT64_MAX;
    gpu->state_version = UINT64_MAX;
    gpu->state_index = ~0u;
    gpu->batch_limit = MAX_DRAWS;
    return gpu->primitive_data && gpu->state_data && gpu->bin_data && gpu->batch_data && gpu->grid;
}

static void exact_free(Exact *gpu) __attribute__((unused));
static void exact_free(Exact *gpu)
{
    trace_destroy(gpu);
    free(gpu->primitive_data); free(gpu->state_data); free(gpu->bin_data); free(gpu->batch_data); free(gpu->grid);
    gpu->primitive_data = NULL; gpu->state_data = NULL; gpu->bin_data = gpu->batch_data = gpu->grid = NULL;
}

/* The provider's settings (it reads them, from DHD_PROFILE and DHD_GS_BATCH where it has them):
   limit 0 or above MAX_DRAWS is MAX_DRAWS. */
static void exact_config(Exact *gpu, int profile, unsigned long limit) __attribute__((unused));
static void exact_config(Exact *gpu, int profile, unsigned long limit)
{
    gpu->profile = profile;
    gpu->batch_limit = limit && limit <= MAX_DRAWS ? (unsigned)limit : MAX_DRAWS;
}

/* Installs the front end as g's GS hooks, on gpu. */
static void exact_hooks(Gs *g, Exact *gpu)
{
    trace_open(gpu);
    g->backend = gpu;
    g->draw = draw; g->sync = sync_vram; g->sync_range = sync_range;
    g->local_copy = local_copy;
    g->stage_pages = stage_pages; g->fetch_pages = fetch_pages;
}

/* Installs the hooks again on a Gs whose VRAM was cleared (gs_reset clears the whole Gs): what was
   recorded goes to the GPU, then the whole (zeroed) image goes up again and no page is GPU-only. */
static void exact_attach(Gs *g, Exact *gpu) __attribute__((unused));
static void exact_attach(Gs *g, Exact *gpu)
{
    submit_all(gpu);
    gpu->uploaded = UINT64_MAX;
    gpu->state_version = UINT64_MAX;
    gpu->state_index = ~0u;
    gpu->texture_valid = 0;
    gpu->dirty = 0;
    memset(gpu->cpu_dirty, 0, sizeof gpu->cpu_dirty);
    memset(gpu->texture_dirty, 255, sizeof gpu->texture_dirty);
    exact_hooks(g, gpu);
}
