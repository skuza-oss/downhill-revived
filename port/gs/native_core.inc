/* Native renderer (DHD_RENDER=native), the part every graphics API shares: recording, the exact
   Z probe, texture decoding, frame hand-off, DHD_INTERP and the reports. GS primitives become
   ordinary triangles drawn into real render targets with a real depth buffer, hardware blending
   and decoded, cached textures. Pictures are close to the GS, not bit-exact.

   Included by gpu_rust_shared.inc for each backend's renderer: gpu_rust_native.inc (in
   port/gs/gpu_rust.c) and the browser's port/web/gpu_rust_web_native.inc. Before the include the
   backend defines
     NativeExact            its exact renderer's state (gpu_rust.c's Rust), with the
                            members native, clip_on and clip[4], and the static functions draw,
                            stage_pages and sync_range;
     NativeView             a sampled texture view (a texture pack's or the sharp font's);
     NATIVE_TEXTURE_GPU     the members of a decoded texture: texture, view and sharp at least;
     NATIVE_SURFACE_GPU     the members of a surface: texture and srv at least;
     NATIVE_GPU             the renderer's own members of Native;
   and after it the functions declared under "backend" below.

   Game state stays exact. The only GS data the EE consumes in a race is the lens-flare Z probe
   (func_00228328 reads a 4x4 PSMZ32 patch every frame). The exact backend keeps running on its
   own device, but only for Z-writing primitives inside a watch rectangle around the last probe,
   clipped to it, in order. A probe outside the watch rectangle (a camera cut) is answered by
   replaying the logged Z-writing primitives since the last full overwrite of the probed pixels.
   A probe at the Z buffer's bottom edge reads one row past it, into the next buffer (sky colours
   in a frame buffer, darkened by the rider shadow texture, which comes from a downsample of the
   frame). So while the probe is near the bottom edge the exact backend draws whole frames too:
   every render target an exactly drawn primitive samples joins the exact set, and the rows a
   probe can reach past the Z buffer (the band) are drawn exactly. The flare moves smoothly, so
   these exact frames start several fields before the probe reaches the edge.

   Each field's draws are recorded on the GS worker as a command list; at vsync the list goes
   to a render thread that owns the presentation device's context, so presentation no longer
   holds up the EE.

   DHD_INTERP=1 adds frames between game fields for 120/144 Hz displays. The simulation stays at
   59.94 Hz and runs no extra game code: the render thread keeps the last two recorded fields and
   on every display refresh re-runs the newer field's command list with its vertices moved
   towards the older field's. A draw command is paired with the older field's next command of
   the same state and vertex count (the camera, the riders and rigid objects); anything unpaired
   (particles, the flare, changing geometry) and anything that jumps is held at the newer field.
   The picture lags the game by one field. DHD_INTERP_HZ=N paces presentation to N Hz with a
   timer instead of the display's vsync. */

#include <math.h>
#include <limits.h>
#include "native_thread.h"  /* locks, the render thread, sleeps (SDL3; none in the browser) */
#include "../host/host_end.h"  /* the render thread stops at the end of the process (finding 210) */
#include "texpack.h"
#include "font_sharp.h"
#include "native_frontend.h"
#include "gs_mesh.h"         /* translated VU1 mesh batches (native_mesh, finding 164) */
#include "gs_strip.h"        /* the game's PACKED strips, read from the packet (native_strip, finding 197) */
#include "perf_log.h"       /* the perf: lines' waits (finding 129; empty in the browser) */

double host_wall_seconds(void); /* port/hle/host.c */

enum { NATIVE_SURFACES = 48, NATIVE_TEXTURES = 4096, NATIVE_WATCH = 128, NATIVE_CONVERTS = 64 };
enum { NATIVE_PALETTES = 512, NATIVE_PALETTE_PROBES = 8 };  /* a race goes round some 200 palettes a field */
/* NATIVE_SCALE_MAX, the largest resolution scale (DHD_SCALE) the backend draws, is set by the
   backend (gpu_rust_native.inc, gpu_rust_web_native.inc) before it includes this file. */

typedef struct {
    float x, y, z, s, t, q, fog;
    uint32_t rgba;
} NativeVertex;
_Static_assert(sizeof(NativeVertex) == 32, "NativeVertex is eight 4-byte words (native_same_vertex)");

typedef struct {
    float target[2]; uint32_t flags, atst;
    uint32_t aref; float alpha_scale, texa_alpha;
    uint32_t scale;                    /* set when drawn: target, texture, sampled scale (native.wgsl) */
    float fog[4];
    int32_t tex_offset[2]; uint32_t tex_size[2];
    uint32_t wrap[4];
    uint32_t region[4];
    /* A mesh draw (flag 4096, native_mesh): the draw offset (GS 12.4 units), the largest Z and the
       Z scale for native.wgsl's native_mesh_vertex; each object's own values (matrix, guard band,
       colour scale, fog) are in its record (native_mesh_record). Zero for every other draw. */
    float mesh_offset[4];
} NativeConstants;
enum { NATIVE_MESH = 4096, NATIVE_MESH_TWO_SIDED = 8192 };   /* constants.flags: a mesh draw (render.rs picks its
                                                              pipeline), drawn without back-face culling */

typedef struct {
    uint64_t key;
    NATIVE_TEXTURE_GPU    /* texture and view; sharp: the game's font made sharp, or with other button icons (font_sharp.c), or none */
    uint64_t used;        /* last field that drew with it */
    TexPackEntry *pack;   /* its replacement (texpack.c), once looked up (pack_checked) */
    int pack_checked;
} NativeTexture;

/* A render target or depth buffer: GS memory from page `page`, `bw` pages wide. */
typedef struct {
    unsigned page, bw, wide, depth;    /* wide: 16-bit pixel layout (64x64 pages) */
    unsigned zpsm;                     /* depth: the Z format it was last drawn with */
    volatile long extent[2];           /* pixels claimed so far (GS worker; only grows) */
    unsigned scale, size[2];           /* render thread: its resolution scale; texture size (scaled pixels) */
    NATIVE_SURFACE_GPU                 /* texture; srv: the view draws sample */
} NativeSurface;

typedef struct { int left, top, right, bottom; } NativeRect;  /* pixels [left, right) x [top, bottom) */

enum { NATIVE_DRAW, NATIVE_COPY, NATIVE_IMPORT, NATIVE_ALPHA_UPLOAD };
typedef struct {
    uint8_t kind, depth_key, color_mask, pad;
    int16_t target, depth, texture_surface;
    int16_t convert;                   /* the frame's texture conversion this draw samples, or -1 */
    uint32_t blend_key, blend_fix;
    NativeTexture *texture;
    NativeView replacement;            /* texpack.c: drawn instead of texture (flag 2048) */
    NativeRect scissor;
    NativeConstants constants;
    uint32_t first, count;             /* vertices, or indices in an indexed field (Native indexed) */
    /* NATIVE_COPY: source surface rectangle to (x, y) in target; NATIVE_IMPORT: pixels. */
    int16_t source;
    int x, y, w, h, sx, sy;
    uint32_t *pixels;
} NativeCommand;

/* DHD_INTERP pairs vertices by segment: one GIF tag's primitives (a VU1 batch, a piece of a
   mesh), under one draw state. Draw commands merge many of them, and culling or clipping in
   any one changes the command's vertex count. */
typedef struct {
    uint32_t first, count, key;      /* key: a hash of the command's draw state */
} NativeSegment;

/* A texture the GPU rebuilds from a surface just before the draw that samples it (native.wgsl's
   native_convert); the constants match its Convert struct. */
typedef struct {
    uint32_t tex[4], size[4];
    uint32_t slot[4][4];               /* per source surface: page, bw, 1 for depth or 2 for 16-bit colour, Z units (float bits) */
    uint8_t pages[512];                /* per GS page: 0 the CPU copy (the decoded base), 1-4 a source slot */
    uint32_t palette[256];
} NativeConvertConstants;
typedef struct {
    int16_t surface[4];
    NativeTexture *base;               /* the texture decoded from the CPU copy, or NULL */
    NativeConvertConstants constants;
} NativeConvert;

/* A field's display_surface when PMODE has no read circuit on (after a GS reset, finding 160): the
   picture is black, as the PS2 shows it (DhdrPicture.display_surface). -1: nothing to scan out,
   the last picture stays. */
#define NATIVE_DISPLAY_OFF (-2)

typedef struct {
    NativeVertex *vertices;
    unsigned vertex_count, vertex_capacity;
    uint32_t *indices;               /* an indexed field (Native indexed): the draws' vertices */
    unsigned index_count, index_capacity;
    NativeCommand *commands;
    unsigned command_count, command_capacity;
    NativeConvert *converts;
    unsigned convert_count, convert_capacity;
    int display_surface;             /* the displayed surface, -1 or NATIVE_DISPLAY_OFF */
    uint32_t display[12];
    uint32_t *movie;                 /* a movie's picture shown instead (gs_native_movie), when movie_on */
    unsigned movie_width, movie_height, movie_capacity;
    unsigned movie_rows;             /* the rows of the movie's picture the movie fills, in its middle */
    int movie_on;
    uint64_t field;
    double time;                     /* host wall seconds at vsync */
    double due, began;               /* when to present it (0: at once), when its pad was read (kernel.c) */
    NativeSegment *segments;         /* DHD_INTERP: the vertices of each GIF tag */
    unsigned segment_count, segment_capacity;
    int32_t *pair;                   /* DHD_INTERP: each segment's partner in the older field, or -1 */
    unsigned pair_capacity;
    unsigned path1;                  /* primitives VU1 drew (PATH1): 0 in a field of 2D only */
    double output_aspect;             /* aspect of the game picture at handoff */
    unsigned flat_limit;             /* loading state at handoff, never read from the render thread */
    int card_2d;                     /* the game drew its 2D unmapped, for a 4:3 card (host_widescreen_card_drawn) */
} NativeFrame;

/* Primitives that write Z buffer memory, for the exact probe answer: Z writes (zbp) and colour
   writes into a Z buffer's pages (fbp; the game clears part of its Z buffer that way). */
enum { NATIVE_NO_PAGE = 0xFFFF };
typedef struct {
    unsigned type, count, state, zbp, fbp;
    int box[4];
    GsVertex v[3];
    uint8_t unconditional;
} NativeLogDraw;
typedef struct { uint64_t reg[128]; uint16_t clut[1024]; } NativeLogState;
typedef struct {
    NativeLogDraw *draws; unsigned draw_count, draw_capacity;
    NativeLogState *states; unsigned state_count, state_capacity;
    uint64_t state_version;
} NativeLog;

typedef struct {
    uint64_t draws, primitives, commands, decoded, decoded_texels, cache_hits, imports, copies;
    uint64_t texture_from_surface, texture_unsupported, blend_approximate, date_ignored;
    uint64_t probe_hits, probe_replays, probe_uncovered, probe_replayed_draws, exact_draws;
    uint64_t color_reads, copy_fallbacks, display_missing, frames, presented;
    uint64_t unsupported_why[6]; /* depth, format, width class, block offset, buffer width, layout */
    uint64_t interp_frames, paired_vertices, held_vertices, jumped_vertices, z_alias_draws;
    uint64_t band_draws, band_rt_textures, probe_band_hits, band_moves, target_draws, exact_targets, full_fields;
    uint64_t converted, conversions_run, blend_dest_alpha;
} NativeStats;

enum { NATIVE_FRAMES = 6, NATIVE_INTERVALS = 8192 };

/* DHD_NATIVE_SURFACES=1: how each surface is used, printed at exit. Sprites that sample a surface
   are binned by texels per pixel (<0.75, about 1, about 2, more) and, for the 1:1 ones, by the
   fraction of a texel between U and the pixel's X (quarters): which sampling the upscaled
   surfaces have to keep aligned. */
typedef struct {
    uint64_t draws, depth_draws, sampled, copied_in, copied_out, converted, displayed;
    int depth_with;
    uint64_t pair[NATIVE_SURFACES], ratio[NATIVE_SURFACES][4], offset[NATIVE_SURFACES][4];
} NativeUsage;

typedef struct Native {
    NativeExactCache exact_state;
    NativeExact *exact;
    NativeMutex context_lock;          /* the render thread's device work, against the EE thread's */
    /* recording (GS worker) */
    NativeFrame frames[NATIVE_FRAMES];
    NativeFrame *recording;
    NativeCommand current;
    int current_valid;
    uint64_t state_version, written;
    unsigned state_ctxt, state_prim;
    float zscale; uint32_t zmax;
    float uv_scale[2];                /* FST: reciprocal GS texture dimensions, including 1/16 */
    float vertex_fog[256];            /* same rounded fog/255 values, shared by all vertices */
    int vertex_fog_ready, field_trace_on;
    /* Two of the last triangle's vertices as native_set converted them (each with its own
       colour): the ones a next strip (its second and third) or fan (its first and third) triangle
       shares (finding 146). Good while conv_ok == conv_serial (native_state moves conv_serial)
       and for the same offset and FST. */
    GsVertex conv_gs[2];
    /* native_palette: the 256 entries gs_palette converts for the last CPSM/CSA/TEXA, valid while
       the CLUT's entries stay (clut_writes); digest16/256 mix the first 16 or all of them. */
    struct { int valid, full; uint64_t writes, clut_key, texa, digest16, digest256; uint32_t entry[256]; } palette_memo;
    /* native_palette_digest (finding 197): the digests of palettes met before, each found again by
       the CLUT words it reads (palette_tags: a slot's tag, 0 when empty), and the last answer,
       good while the CLUT's entries stay. */
    uint32_t palette_tags[NATIVE_PALETTES];
    struct { uint64_t texa, digest; uint32_t used; uint16_t count, cpsm; uint32_t words[256]; } palettes[NATIVE_PALETTES];
    struct { int valid, full; uint64_t writes, clut_key, texa, digest; } palette_last;
    NativeVertex conv[2];
    int conv_fst;
    uint64_t conv_offset;
    unsigned conv_serial, conv_ok;
    /* Draws are indexed (dhdr_native_picture_indexed): a vertex a strip or fan shares with the
       previous triangle, and a quad's shared corners, are stored once. Off with DHD_INTERP (its
       mix pairs and moves vertices by their recorded order) and with DHD_NATIVE_INDEXED=0. */
    int indexed;
    int skip;                        /* the current state draws nothing */
    int mesh_open;                   /* the open draw is a mesh draw (native_mesh): no GS primitive joins it */
    NativeFrame *mesh_record_frame;  /* where native_mesh_record wrote its last record, and at which vertex */
    unsigned mesh_record_at;
    int box[4];                      /* pixels the open draw has touched */
    unsigned target_psm, depth_psm;
    int passes;                      /* AFAIL second pass needed */
    NativeCommand second;
    NativeSurface surfaces[NATIVE_SURFACES];
    volatile long surface_count;
    int16_t owner[512];
    uint32_t owner_writes[512];
    uint64_t upload_epoch, alpha_epoch[512];
    unsigned alpha_command[512];
    uint16_t color_pixel[2048];       /* GS word within a page -> row-major colour pixel */
    int16_t top_owner[512];          /* the colour surface that last drew a page, for a Z24 buffer's top byte */
    uint32_t top_writes[512];
    uint64_t page_hash[512];
    uint32_t page_hashed[512];
    uint8_t page_hash_valid[512];
    NativeTexture textures[NATIVE_TEXTURES];
    unsigned texture_count;
    int texpack;                       /* a texture pack is open (texpack.c) */
    int replace_on;                    /* replacements can be drawn (their samplers exist) */
    uint64_t replaced_draws;
    uint64_t field;
    /* exact Z probe */
    NativeLog logs[2];               /* previous field, this field */
    struct { int on, valid; unsigned zbp; int rect[4]; uint8_t covered[NATIVE_WATCH * NATIVE_WATCH]; unsigned missing; } watch;
    uint8_t zbw[512];                /* buffer width each Z buffer was last drawn with */
    uint8_t zbase[512];              /* pages used as a Z buffer base */
    uint8_t band[512];               /* pages kept exact all the time */
    uint16_t band_pages[64];
    uint8_t exact_target[512];       /* render target bases drawn exactly, whole */
    uint64_t full_until;             /* exact frames run through this field */
    unsigned full_hold;              /* fields they run on after the probe leaves the edge */
    unsigned band_count, band_key;   /* the Z buffer (base, width, rows) the band follows */
    int trace_probes;
    /* The Z probe from native depth (finding 097), unless DHD_EXACT_PROBE=1 keeps the exact path
       above (the state-trace tools: game state as the PS2's). The EE asks for the probe's
       neighbourhood (zread_want); the render thread copies it from the Z surface after each
       picture and hands back the newest finished copy (zread); the EE's read gets those values,
       a field or two late, written over the CPU image in native_fetch_pages (zread_pending). */
    int exact_probe;
    struct { int on; unsigned zbp; int surface; int rect[4]; } zread_want;                        /* queue_lock */
    struct { int valid; unsigned zbp; uint32_t rect[4]; uint32_t bits[64 * 64]; } zread;          /* queue_lock */
    uint64_t zread_tag;              /* render thread */
    uint32_t zread_bits[64 * 64];    /* render thread: dhdr_native_zread_take's scratch */
    struct { unsigned psm, bp, bw, x, y; uint32_t value, mask; } zread_pending[1024];  /* mask: the Z bits */
    unsigned zread_pending_count;
    FILE *texlog;                    /* DHD_NATIVE_TEXLOG: textures drawn without their pixels */
    NativeUsage *usage;              /* DHD_NATIVE_SURFACES */
    uint64_t replay_version;
    /* render thread */
    int thread;                      /* started */
    NativeMutex queue_lock;
    NativeCond queue_changed;
    NativeFrame *pending[2], *free_frames[NATIVE_FRAMES];
    unsigned pending_count, free_count;
    uint64_t handed, rendered;
    int quit, resize;
    int thread_exited;               /* the render thread has left its loop for good (queue_lock; native_stop) */
    NativeWindow window;
    float aspect;  /* the picture's shape in the window (DHD_ASPECT): width/height, 0 stretched, -1 its pixels */
    float wide;    /* widescreen's shape, or 0: fields of 2D only (loading screens) are shown 4:3 */
    unsigned flat_fields;            /* full-screen 2D fields in a row, up to the one shown */
    int card_shown;                  /* the last picture is a card: shown 4:3, the sides filled */
    unsigned movie_rows;             /* the shown picture is a movie in these rows of its middle; 0: not a movie */
    /* DHD_SCALE: wide surfaces (frame and Z buffers) are drawn this many times larger; 0 until
       "match window" has seen the window. Narrow ones (the shadow and its blur) stay at 1x. */
    unsigned scale_setting, scale;
    NativeConvertConstants convert_scratch;
    uint32_t *picture;                 /* DHD_SCREENSHOT of an upscaled picture */
    size_t picture_size;
    unsigned display_width, display_height;  /* the displayed picture (native_scanout) */
    NativeStats stats;
    /* DHD_INTERP */
    int interp;
    uint64_t segment_tags;           /* GIF tags seen when the open segment started */
    uint32_t segment_key;
    double interp_hz;
    double present_due, present_began;  /* the picture being drawn (NativeFrame due, began) */
    NativeFrame *older, *newer;      /* held by the render thread */
    NativeVertex *mixed;
    unsigned mixed_capacity;
    int32_t *same;                   /* native_mixed: segments by vertex positions (open addressing) */
    unsigned same_capacity;
    uint8_t *paired;
    unsigned paired_capacity;
    double last_present, first_present;
    float intervals[NATIVE_INTERVALS];   /* ms between presents, a ring */
    uint64_t interval_count;
    uint64_t bench_presented, bench_interp;  /* counters at the DHD_BENCH window's first field */
    /* DHD_NATIVE_TRACE=prefix: prefix-fields.csv, a line per recorded field (GS worker at vsync;
       vertices are the stored ones, indices 0 unless the field is indexed),
       and prefix-pictures.csv, a line per picture (render thread) with its GPU time. */
    FILE *trace_fields, *trace_pictures;
    NativeStats trace_last;
    struct { uint64_t self_copies, self_copy_px, copies, copy_px, imports, import_px, draws; } drawn;
    NATIVE_GPU                       /* the backend's device objects (render thread) */
} Native;

/* ---- backend: defined by the file that includes this one ---- */

/* A decoded texture's GPU copy from w x h RGBA pixels, and its sharp font when sharp (pack: the
   pack's replacement of it, font_sharp_view's); 0 when it failed (then nothing is left to
   release). GS worker. */
static int native_texture_create(Native *n, NativeTexture *t, const uint32_t *pixels, unsigned w, unsigned h,
                                 int sharp, uint64_t key, TexPackEntry *pack);
static void native_texture_release(NativeTexture *t);  /* all of it, any part may be missing */
/* What a draw with this texture shows instead: its sharp font when it has one (with a pack's
   replacement it has one only for the button icons, made from the pack's picture), else its
   texture pack replacement. */
static NativeView native_replacement_view(Native *n, NativeTexture *t);
/* The surface's texture at its scale and at least want[] GS pixels, keeping what it holds. */
static int native_surface_resources(Native *n, NativeSurface *s, const unsigned want[2]);
/* One picture: the field's commands with these vertices, scanned out and presented. */
static void native_picture(Native *n, NativeFrame *f, const NativeVertex *vertices, float t);
static void native_present(Native *n);  /* the last scanned-out picture again */
/* The displayed picture (display_width x display_height) into pixels; 0 when there is none. */
static int native_read_display(Native *n, uint32_t *pixels);
static void native_present_options(Native *n);  /* the present filter; then native_aspect_options */

/* ---- DHD_RSTAT: the render thread's per-picture budget (finding 108) ----
   The render thread's frame with the parts a 120 field/s budget (8.33 ms) is spent in: waiting for
   the next field, the context lock, native_prepare_surfaces, the snapshot, the Rust draw call
   (dhdr_native_picture, split further by port/render/src/native/render.rs's own rstat line), the
   present (whose Rust acquire/record/submit the same line splits) and the Z copies after the
   picture. A part costs two host_wall_seconds() calls while DHD_RSTAT is set and nothing without
   it (the start is 0 then, RSTAT_ADD adds nothing). One line every 240 pictures, so a run's tail
   holds the warmed means; the wait is the field clock's or the vsync's idle time, not work. */
enum {
    RSTAT_FRAME, RSTAT_WAIT, RSTAT_LOCK, RSTAT_PREPARE, RSTAT_SNAPSHOT, RSTAT_DRAW, RSTAT_PRESENT,
    RSTAT_PACE, RSTAT_SHOW, RSTAT_ZREAD, RSTAT_PARTS
};
static double native_rstat[RSTAT_PARTS];
static unsigned native_rstat_pictures;

static inline int native_rstat_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("DHD_RSTAT") != NULL;
    return on;
}

/* The clock, or 0 while off (RSTAT_ADD then adds nothing and reads no clock). */
static inline double native_rstat_start(void)
{
    return native_rstat_on() ? host_wall_seconds() : 0;
}

static inline void native_rstat_add(int part, double start)
{
    if (start) native_rstat[part] += host_wall_seconds() - start;
}

/* One picture accounted: every 240 a line of means, then a fresh sum. */
static inline void native_rstat_end(void)
{
    if (!native_rstat_on() || ++native_rstat_pictures % 240) return;
    fprintf(stderr, "rstat: pictures=240 mean_ms frame=%.3f wait=%.3f lock=%.3f prepare=%.3f "
            "snapshot=%.3f draw=%.3f present=%.3f pace=%.3f show=%.3f zread=%.3f\n",
            native_rstat[RSTAT_FRAME] * 1e3 / 240, native_rstat[RSTAT_WAIT] * 1e3 / 240,
            native_rstat[RSTAT_LOCK] * 1e3 / 240, native_rstat[RSTAT_PREPARE] * 1e3 / 240,
            native_rstat[RSTAT_SNAPSHOT] * 1e3 / 240, native_rstat[RSTAT_DRAW] * 1e3 / 240,
            native_rstat[RSTAT_PRESENT] * 1e3 / 240, native_rstat[RSTAT_PACE] * 1e3 / 240,
            native_rstat[RSTAT_SHOW] * 1e3 / 240, native_rstat[RSTAT_ZREAD] * 1e3 / 240);
    memset(native_rstat, 0, sizeof native_rstat);
}

static int psm_wide(unsigned psm) { return (psm & 7) == 2; }
static int psm_color(unsigned psm) { return psm == 0 || psm == 1 || psm == 2 || psm == 10; }

/* ---- surfaces and page ownership (GS worker; descriptors are append-only) ---- */

static int native_surface(Native *n, unsigned page, unsigned bw, unsigned psm, unsigned depth)
{
    unsigned wide = psm_wide(psm), count = (unsigned)n->surface_count;
    if (!bw) bw = 1;
    for (unsigned i = 0; i < count; i++) {
        NativeSurface *s = &n->surfaces[i];
        if (s->page == page && s->bw == bw && s->wide == wide && s->depth == depth) return (int)i;
    }
    if (count == NATIVE_SURFACES) return -1;
    NativeSurface *s = &n->surfaces[count];
    s->page = page; s->bw = bw; s->wide = wide; s->depth = depth;
    native_atomic_store(&n->surface_count, (long)count + 1);  /* the render thread creates its textures */
    return (int)count;
}

static int native_owner(Native *n, const Gs *g, unsigned page)
{
    page &= 511;
    return n->owner_writes[page] == g->page_writes[page] ? n->owner[page] : -1;
}

/* Pixel origin of a GS page inside a surface, or 0 when the page is outside its layout. */
static int native_page_origin(const NativeSurface *s, unsigned page, int *x, int *y)
{
    unsigned rel = (page - s->page) & 511, ph = s->wide ? 64 : 32;
    if (rel / s->bw * ph >= 2048) return 0;
    *x = (int)(rel % s->bw) * 64; *y = (int)(rel / s->bw * ph);
    return 1;
}

/* Leaves at once: the exit report would wait on the render thread. */
static void native_oom(const char *what)
{
    fprintf(stderr, "gs native: out of memory (%s)\n", what);
    fflush(stderr);
    native_exit_now(1);
}

static NativeCommand *native_command(Native *n)
{
    NativeFrame *f = n->recording;
    if (f->command_count == f->command_capacity) {
        unsigned capacity = f->command_capacity ? f->command_capacity * 2 : 4096;
        NativeCommand *grown = realloc(f->commands, capacity * sizeof *grown);
        if (!grown) native_oom("commands");
        f->commands = grown; f->command_capacity = capacity;
    }
    NativeCommand *c = &f->commands[f->command_count++];
    memset(c, 0, sizeof *c);
    c->target = c->depth = c->texture_surface = c->source = c->convert = -1;
    return c;
}

static void native_reserve(NativeFrame *f, unsigned more);
static void native_reserve_indices(NativeFrame *f, unsigned more);
static void native_quad_indexed(NativeFrame *f, const NativeVertex q[4]);
static unsigned native_next(const Native *n, const NativeFrame *f);

/* A page another colour surface drew in the other pixel layout: the rider shadow's 64x64 buffer
   is drawn as 32-bit, then through a 16-bit view of the same memory (moving bits into the 32-bit
   alpha), then as 32-bit again (finding 096). The GS memory holds what the last draw left; the CPU
   copy holds none of it (native draws never reach it). So the GPU rebuilds the page from the
   surface that drew it, in this surface's layout (native.wgsl's native_convert), and a sprite draws
   it in. 0 when the field has no conversion left. */
static int native_import_converted(Native *n, int surface, unsigned page, int previous, int x, int y, unsigned w,
                                   unsigned h)
{
    NativeFrame *f = n->recording;
    if (f->convert_count == NATIVE_CONVERTS) return 0;
    if (!f->converts && !(f->converts = malloc(NATIVE_CONVERTS * sizeof *f->converts))) native_oom("conversions");
    const NativeSurface *s = &n->surfaces[surface], *from = &n->surfaces[previous];
    NativeConvert *v = &f->converts[f->convert_count];
    memset(v, 0, sizeof *v);
    for (unsigned i = 0; i < 4; i++) v->surface[i] = -1;
    v->surface[0] = (int16_t)previous;
    if (n->usage) n->usage[previous].converted++;
    v->constants.slot[0][0] = from->page; v->constants.slot[0][1] = from->bw; v->constants.slot[0][2] = from->wide ? 2 : 0;
    v->constants.pages[page] = 1;
    v->constants.tex[0] = s->wide ? GS_PSMCT16 : GS_PSMCT32; v->constants.tex[1] = page * 32; v->constants.tex[2] = 1;
    v->constants.size[0] = w; v->constants.size[1] = h;
    v->constants.size[3] = 0x80;  /* TEXA TA0 0, TA1 0x80: a 16-bit pixel's alpha bit as 16-bit draws leave it */
    NativeCommand *c = native_command(n);
    c->kind = NATIVE_DRAW; c->target = (int16_t)surface; c->convert = (int16_t)f->convert_count++;
    c->color_mask = 15;
    c->scissor = (NativeRect){x, y, x + (int)w, y + (int)h};
    NativeConstants *k = &c->constants;
    k->flags = 1 | 2 | 1 << 2 | 1024;  /* textured, RGBA as the texture has it (TCC, DECAL), a sprite */
    k->alpha_scale = 255;
    k->tex_size[0] = w; k->tex_size[1] = h;
    k->wrap[0] = k->wrap[1] = 1;
    k->target[0] = k->target[1] = 2048;
    NativeVertex q[4];  /* corners 0 1 2 3, at GS pixel centres as native_record places them */
    for (unsigned i = 0; i < 4; i++)
        q[i] = (NativeVertex){(float)x + (float)(i & 1 ? w : 0) + 0.5f, (float)y + (float)(i & 2 ? h : 0) + 0.5f, 0,
                              (float)(i & 1), (float)(i >> 1), 1, 1, 0x80808080u};
    c->first = native_next(n, f); c->count = 6;
    native_reserve(f, 6);
    if (n->indexed) {
        native_reserve_indices(f, 6);
        native_quad_indexed(f, q);
    } else {
        NativeVertex *o = &f->vertices[f->vertex_count];
        o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[2]; o[4] = q[1]; o[5] = q[3];
        f->vertex_count += 6;
    }
    n->stats.converted++;
    return 1;
}

/* The surface's starting content for a page: what another surface of the same layout drew there,
   or that surface's page converted to this layout, else CPU memory (uploads, or nothing yet). */
static void native_import_page(Native *n, Gs *g, int surface, unsigned page)
{
    NativeSurface *s = &n->surfaces[surface];
    int x, y;
    if (s->depth || !native_page_origin(s, page, &x, &y)) return;
    unsigned w = 64, h = s->wide ? 64 : 32, psm = s->wide ? GS_PSMCT16 : GS_PSMCT32;
    int previous = native_owner(n, g, page), sx, sy;
    if (previous >= 0 && previous != surface && !n->surfaces[previous].depth) {
        if (n->surfaces[previous].wide == s->wide && native_page_origin(&n->surfaces[previous], page, &sx, &sy)) {
            NativeCommand *c = native_command(n);
            c->kind = NATIVE_COPY; c->source = (int16_t)previous; c->target = (int16_t)surface;
            c->x = x; c->y = y; c->w = (int)w; c->h = (int)h; c->sx = sx; c->sy = sy;
            n->stats.copies++;
            if (n->usage) n->usage[previous].copied_out++, n->usage[surface].copied_in++;
            return;
        }
        if (native_import_converted(n, surface, page, previous, x, y, w, h)) return;
    }
    NativeCommand *c = native_command(n);
    c->target = (int16_t)surface; c->x = x; c->y = y; c->w = (int)w; c->h = (int)h;
    c->kind = NATIVE_IMPORT;
    c->pixels = malloc(w * h * 4);
    if (!c->pixels) native_oom("import");
    for (unsigned j = 0; j < h; j++)
        for (unsigned i = 0; i < w; i++) {
            uint32_t v = gs_image_pixel(g->vram, psm, s->page * 32, s->bw, (unsigned)x + i, (unsigned)y + j);
            if (s->wide) v = gs_color(v, GS_PSMCT16, 0x80ull << 32 | 0x80);
            unsigned a = v >> 24;
            c->pixels[j * w + i] = (v & 0xFFFFFF) | a << 24;
        }
    n->stats.imports++;
}

/* Makes the surface own every page of a rectangle, importing the colour pages it did not own. */
static void native_claim(Native *n, Gs *g, int surface, unsigned psm, int x0, int y0, int x1, int y1)
{
    NativeSurface *s = &n->surfaces[surface];
    uint32_t pages[16] = {0};
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 2047) x1 = 2047;
    if (y1 > 2047) y1 = 2047;
    if (x1 < x0 || y1 < y0) return;
    if (x1 + 1 > s->extent[0]) native_atomic_store(&s->extent[0], x1 + 1);
    if (y1 + 1 > s->extent[1]) native_atomic_store(&s->extent[1], y1 + 1);
    gs_mark_pages(pages, psm, s->page * 32, s->bw, (unsigned)x0, (unsigned)y0, (unsigned)x1, (unsigned)y1);
    for (unsigned word = 0; word < 16; word++)
        for (uint32_t bits = pages[word]; bits; bits &= bits - 1) {
            unsigned page = word * 32 + native_bit_first(bits);
            if (native_owner(n, g, page) == surface) continue;
            if (!s->depth) native_import_page(n, g, surface, page);
            n->owner[page] = (int16_t)surface;
            n->owner_writes[page] = g->page_writes[page];
            if (!s->depth && !s->wide) { n->top_owner[page] = (int16_t)surface; n->top_writes[page] = g->page_writes[page]; }
        }
}

/* ---- textures ---- */

/* Font uploads share the framebuffer's high byte. Keep the RGB owner and queue
   only the written alpha bits; importing the CPU's whole page would erase RGB
   drawn exclusively on the native GPU. The mask also preserves the other nibble
   and untouched pixels of partial uploads. Called before page_writes increments. */
static void native_write_high(Gs *g, unsigned address, uint32_t value, uint32_t mask)
{
    Native *n = ((NativeExact *)g->backend)->native;
    unsigned page = address / (8192 * 8);
    int owner = native_owner(n, g, page), x, y;
    if (owner < 0) return;
    NativeSurface *s = &n->surfaces[owner];
    if (s->depth || s->wide || !native_page_origin(s, page, &x, &y) || x + 64 > 2048 || y + 32 > 2048) return;
    NativeFrame *f = n->recording;
    NativeCommand *c;
    if (n->alpha_command[page] && n->alpha_epoch[page] == n->upload_epoch) {
        c = &f->commands[n->alpha_command[page] - 1];
    } else {
        c = native_command(n);
        c->kind = NATIVE_ALPHA_UPLOAD; c->target = (int16_t)owner;
        c->x = x; c->y = y; c->w = 64; c->h = 32;
        c->pixels = calloc(2048, 4);
        if (!c->pixels) native_oom("alpha upload");
        n->alpha_command[page] = f->command_count;
        n->alpha_epoch[page] = n->upload_epoch;
        if (x + 64 > s->extent[0]) native_atomic_store(&s->extent[0], x + 64);
        if (y + 32 > s->extent[1]) native_atomic_store(&s->extent[1], y + 32);
    }
    unsigned shift = (address & 31) - 24;
    mask <<= shift; value = (value << shift) & mask;
    unsigned pixel = n->color_pixel[(address >> 5) & 2047];
    uint32_t old = c->pixels[pixel];
    c->pixels[pixel] = ((old & 255) & ~mask) | value | ((old >> 8 | mask) & 255) << 8;
    n->owner_writes[page] = g->page_writes[page] + 1;
    n->top_owner[page] = (int16_t)owner; n->top_writes[page] = g->page_writes[page] + 1;
}

static uint64_t native_mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

static uint64_t native_page_hash(Native *n, const Gs *g, unsigned page)
{
    if (!n->page_hash_valid[page] || n->page_hashed[page] != g->page_writes[page]) {
        const uint64_t *words = (const uint64_t *)((const uint8_t *)g->vram + page * 8192);
        uint64_t h = page;
        for (unsigned i = 0; i < 1024; i += 4)
            h = native_mix(h, words[i] ^ words[i + 1] * 3 ^ words[i + 2] * 5 ^ words[i + 3] * 7);
        n->page_hash[page] = h; n->page_hashed[page] = g->page_writes[page]; n->page_hash_valid[page] = 1;
    }
    return n->page_hash[page];
}

/* The palette of tex0 as gs_palette gives it, entry by entry (finding 148): the converted entries
   depend on the CLUT's entries, CPSM, CSA and TEXA only, so they are kept until one changes. */
static const uint32_t *native_palette(Native *n, const Gs *g, uint64_t tex0, int full)
{
    uint64_t clut_key = (tex0 >> 51 & 15) | (tex0 >> 56 & 31) << 4, texa = g->reg[GS_TEXA];   /* CPSM, CSA */
    if (!n->palette_memo.valid || n->palette_memo.writes != g->clut_writes || n->palette_memo.clut_key != clut_key ||
        n->palette_memo.texa != texa) {
        uint64_t digest = 16;
        for (unsigned i = 0; i < 16; i++) digest = native_mix(digest, n->palette_memo.entry[i] = gs_palette(g, tex0, i));
        n->palette_memo.digest16 = digest;
        n->palette_memo.valid = 1; n->palette_memo.full = 0; n->palette_memo.writes = g->clut_writes;
        n->palette_memo.clut_key = clut_key; n->palette_memo.texa = texa;
    }
    if (full && !n->palette_memo.full) {  /* the other 240, for 8-bit textures (full: all 256 needed) */
        uint64_t digest = 256;
        for (unsigned i = 0; i < 16; i++) digest = native_mix(digest, n->palette_memo.entry[i]);
        for (unsigned i = 16; i < 256; i++) digest = native_mix(digest, n->palette_memo.entry[i] = gs_palette(g, tex0, i));
        n->palette_memo.digest256 = digest;
        n->palette_memo.full = 1;
    }
    return n->palette_memo.entry;
}

/* The CLUT words gs_palette reads for the first `count` entries of tex0's palette, as it reads
   them (before gs_color). */
static void native_palette_words(const Gs *g, uint64_t tex0, unsigned count, uint32_t *words)
{
    unsigned cpsm = (unsigned)(tex0 >> 51) & 15, csa = (unsigned)(tex0 >> 56) & 31;
    if (cpsm & 2) {
        for (unsigned i = 0; i < count; i++) words[i] = g->clut[(csa * 16 + i) & 511];
        return;
    }
    for (unsigned row = 0; row < count; row += 16) {
        unsigned group = (csa & 15) * 16 + row;
        const uint16_t *low = &g->clut[group > 240 ? 240 : group], *high = low + 256;
        for (unsigned i = 0; i < 16; i++) words[row + i] = low[i] | (uint32_t)high[i] << 16;
    }
}

/* The digest native_palette gives the 16 or 256 converted entries of tex0's palette: the texture
   key's part. The converted entries follow from the CLUT words read, CPSM and TEXA, so a palette
   met before has its digest kept beside those words and is not converted again. (The entries'
   memo above holds one palette; the game alternates between textures, each with its own CLUT, and
   comes back to every one of them the next field.) A palette not met before is converted by
   native_palette as before. */
static uint64_t native_palette_digest(Native *n, const Gs *g, uint64_t tex0, int full)
{
    uint64_t clut_key = (tex0 >> 51 & 15) | (tex0 >> 56 & 31) << 4, texa = g->reg[GS_TEXA];   /* CPSM, CSA */
    if (n->palette_last.valid && n->palette_last.writes == g->clut_writes && n->palette_last.clut_key == clut_key &&
        n->palette_last.texa == texa && n->palette_last.full == full)
        return n->palette_last.digest;
    unsigned count = full ? 256 : 16, cpsm = (unsigned)clut_key & 15;
    uint32_t words[256] PS2_NO_AUTO_INIT, tag = 0;  /* written for every entry read */
    native_palette_words(g, tex0, count, words);
    for (unsigned i = 0; i < count; i++) tag += words[i] * (2 * i + 1);
    tag = ((tag ^ (uint32_t)texa ^ (uint32_t)(texa >> 32) * 31 ^ count ^ cpsm << 12) * 0x9E3779B1u) | 1;
    unsigned first = (tag >> 16) % NATIVE_PALETTES, slot = NATIVE_PALETTES;
    for (unsigned probe = 0; probe < NATIVE_PALETTE_PROBES && slot == NATIVE_PALETTES; probe++) {
        unsigned at = (first + probe) % NATIVE_PALETTES;
        if (n->palette_tags[at] == tag && n->palettes[at].count == count && n->palettes[at].cpsm == cpsm &&
            n->palettes[at].texa == texa && !memcmp(n->palettes[at].words, words, count * 4))
            slot = at;
    }
    if (slot == NATIVE_PALETTES) {
        /* not met, or pushed out: converted, and kept in an empty slot or the one used longest ago */
        slot = first;
        for (unsigned probe = 0; probe < NATIVE_PALETTE_PROBES && n->palette_tags[slot]; probe++) {
            unsigned at = (first + probe) % NATIVE_PALETTES;
            if (!n->palette_tags[at] || (int32_t)(n->palettes[at].used - n->palettes[slot].used) < 0) slot = at;
        }
        native_palette(n, g, tex0, full);
        n->palette_tags[slot] = tag;
        n->palettes[slot].texa = texa; n->palettes[slot].count = (uint16_t)count; n->palettes[slot].cpsm = (uint16_t)cpsm;
        n->palettes[slot].digest = full ? n->palette_memo.digest256 : n->palette_memo.digest16;
        memcpy(n->palettes[slot].words, words, count * 4);
    }
    n->palettes[slot].used = (uint32_t)n->field;
    n->palette_last.valid = 1; n->palette_last.full = full; n->palette_last.writes = g->clut_writes;
    n->palette_last.clut_key = clut_key; n->palette_last.texa = texa; n->palette_last.digest = n->palettes[slot].digest;
    return n->palette_last.digest;
}

static NativeTexture *native_decode(Native *n, Gs *g, unsigned ctxt, uint64_t tex0, unsigned w, unsigned h, uint64_t key)
{
    unsigned slot = (unsigned)(key % NATIVE_TEXTURES);
    for (unsigned probe = 0; probe < 16; probe++) {
        NativeTexture *t = &n->textures[(slot + probe) % NATIVE_TEXTURES];
        if (t->texture && t->key == key) { t->used = n->field; n->stats.cache_hits++; return t; }
    }
    /* Free the oldest of the probed slots unless a queued or rendering field may use it. */
    NativeTexture *victim = NULL;
    for (unsigned probe = 0; probe < 16; probe++) {
        NativeTexture *t = &n->textures[(slot + probe) % NATIVE_TEXTURES];
        if (!t->texture) { victim = t; break; }
        if (t->used + 8 < n->field && (!victim || t->used < victim->used)) victim = t;
    }
    if (!victim) return NULL;
    native_texture_release(victim);
    unsigned psm = (unsigned)(tex0 >> 20) & 63, tbp = (unsigned)tex0 & 0x3FFF, tbw = (unsigned)(tex0 >> 14) & 63;
    uint32_t *pixels = malloc((size_t)w * h * 4), palette[256];
    if (!pixels) return NULL;
    int indexed = psm == GS_PSMT8 || psm == GS_PSMT4 || psm == GS_PSMT8H || psm == GS_PSMT4HL || psm == GS_PSMT4HH;
    if (indexed) memcpy(palette, native_palette(n, g, tex0, 1), sizeof palette);
    if (!tbw) tbw = 1;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            uint32_t v = gs_image_pixel(g->vram, psm, tbp, tbw, x, y);
            pixels[y * w + x] = indexed ? palette[v & 255] : gs_color(v, psm, g->reg[GS_TEXA]);
        }
    /* The text engine's font, made sharp for high resolutions (dhd.ini font, finding 058), or with
       other button icons: over a pack's replacement of it when there is one (finding 200), so its
       replacement is looked up here. */
    int sharp = indexed && n->replace_on && font_sharp_active() && font_sharp_match(tex0);
    TexPackEntry *pack = sharp && n->texpack ? texpack_find(g, ctxt) : NULL;
    int made = native_texture_create(n, victim, pixels, w, h, sharp, key, pack);
    free(pixels);
    if (!made) return NULL;
    victim->key = key; victim->used = n->field;
    victim->pack = pack; victim->pack_checked = sharp && n->texpack;
    n->stats.decoded++; n->stats.decoded_texels += (uint64_t)w * h;
    return victim;
}

/* Where a page of a texture lives when the native renderer drew some of its pages: 0 for the
   CPU copy of GS memory, else 1 + the surface whose pixels hold the page. A Z24 buffer keeps the
   top byte (8H, 4HL and 4HH textures): what a colour draw into its memory left there, or the CPU
   copy (uploads; the logo). */
static int native_page_source(Native *n, Gs *g, unsigned page, unsigned psm)
{
    int o = native_owner(n, g, page);
    if (o < 0) return 0;
    const NativeSurface *s = &n->surfaces[o];
    if (s->depth && s->zpsm == GS_PSMZ24 && (psm == GS_PSMT8H || psm == GS_PSMT4HL || psm == GS_PSMT4HH)) {
        int top = n->top_owner[page];
        return top >= 0 && n->top_writes[page] == g->page_writes[page] ? 1 + top : 0;
    }
    if (s->wide || (s->depth && (s->zpsm & 7) == 2)) return -1;
    return 1 + o;
}

/* A texture over GS memory some of which native surfaces drew, in a layout the draw can't sample
   from a surface directly: a 32-bit target read as 16-bit (the shadow), the frame buffers' alpha
   read as a 4-bit texture (the haze), part of a texture page drawn over (the loading screen). The
   GPU rebuilds it just before the draw: the CPU copy decoded as usual, with the drawn pages read
   from their surfaces (native.wgsl's native_convert). */
static int native_convert_texture(Native *n, Gs *g, unsigned ctxt, uint64_t tex0, unsigned w, unsigned h, const uint32_t pages[16],
                                  uint64_t key, NativeCommand *c)
{
    unsigned psm = (unsigned)(tex0 >> 20) & 63, tbp = (unsigned)tex0 & 0x3FFF, tbw = (unsigned)(tex0 >> 14) & 63;
    if (!tbw) tbw = 1;
    if ((psm & 0x30) == 0x30) return 0;
    NativeFrame *f = n->recording;
    if (f->convert_count == NATIVE_CONVERTS) return 0;
    if (!f->converts && !(f->converts = malloc(NATIVE_CONVERTS * sizeof *f->converts))) native_oom("conversions");
    NativeConvert *v = &f->converts[f->convert_count];
    memset(v, 0, sizeof *v);
    for (unsigned i = 0; i < 4; i++) v->surface[i] = -1;
    unsigned slots = 0;
    for (unsigned p = 0; p < 512; p++) {
        if (!(pages[p / 32] & (1u << (p & 31)))) continue;
        int src = native_page_source(n, g, p, psm);
        if (src < 0) return 0;
        if (!src) continue;
        unsigned slot = 0;
        while (slot < slots && v->surface[slot] != src - 1) slot++;
        if (slot == slots) {
            if (slots == 4) return 0;
            const NativeSurface *s = &n->surfaces[src - 1];
            float unit = s->zpsm == GS_PSMZ32 ? 4294967296.0f : s->zpsm == GS_PSMZ24 ? 16777216.0f : 65536.0f;
            v->surface[slot] = (int16_t)(src - 1);
            if (n->usage) n->usage[src - 1].converted++;
            v->constants.slot[slot][0] = s->page; v->constants.slot[slot][1] = s->bw; v->constants.slot[slot][2] = s->depth;
            memcpy(&v->constants.slot[slot][3], &unit, 4);
            slots++;
        }
        v->constants.pages[p] = (uint8_t)(1 + slot);
    }
    if (!slots) return 0;
    v->base = native_decode(n, g, ctxt, tex0, w, h, key);
    uint64_t texa = g->reg[GS_TEXA];
    v->constants.tex[0] = psm; v->constants.tex[1] = tbp; v->constants.tex[2] = tbw;
    v->constants.size[0] = w; v->constants.size[1] = h;
    v->constants.size[2] = (uint32_t)texa; v->constants.size[3] = (uint32_t)(texa >> 32);
    if (gs_bits_per_pixel(psm) <= 8)
        memcpy(v->constants.palette, native_palette(n, g, tex0, 1), sizeof v->constants.palette);
    if (n->texlog)
        for (unsigned i = 0; i < slots; i++) {
            const NativeSurface *s = &n->surfaces[v->surface[i]];
            fprintf(n->texlog, "%llu,convert,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", (unsigned long long)n->field, s->zpsm, psm,
                    tbp, tbw, w, h, s->page, s->bw, s->wide, s->depth);
        }
    c->convert = (int16_t)f->convert_count++;
    c->texture = NULL;
    n->stats.converted++;
    return 1;
}

/* A texture pack's replacement for a decoded texture (texpack.c): looked up once per decode,
   drawn once its loader has it. Or the sharp font (font_sharp.c): a pack's own font wins, unless
   other button icons are chosen, when the copy is the pack's font with them (finding 200).
   Region repeat (an address mask) stays the original's. */
static void native_replace(Native *n, Gs *g, unsigned ctxt, NativeCommand *c)
{
    NativeTexture *t = c->texture;
    if (!t->pack_checked) { t->pack = n->texpack ? texpack_find(g, ctxt) : NULL; t->pack_checked = 1; }
    if ((!t->pack && !t->sharp) || c->constants.wrap[0] == 3 || c->constants.wrap[1] == 3) return;
    NativeView view = native_replacement_view(n, t);
    if (!view) return;
    c->replacement = view;
    c->constants.flags |= 2048;
    n->replaced_draws++;
}

static void native_texture(Native *n, Gs *g, unsigned ctxt, NativeCommand *c)
{
    uint64_t tex0 = g->reg[GS_TEX0_1 + ctxt], clamp = g->reg[GS_CLAMP_1 + ctxt], tex1 = g->reg[GS_TEX1_1 + ctxt];
    unsigned psm = (unsigned)(tex0 >> 20) & 63, tbp = (unsigned)tex0 & 0x3FFF, tbw = (unsigned)(tex0 >> 14) & 63;
    unsigned tw = (unsigned)(tex0 >> 26) & 15, th = (unsigned)(tex0 >> 30) & 15;
    if (tw > 10) tw = 10;
    if (th > 10) th = 10;
    unsigned w = 1u << tw, h = 1u << th;
    NativeConstants *k = &c->constants;
    k->flags |= 1 | (unsigned)(tex0 >> 34 & 1) << 1 | (unsigned)(tex0 >> 35 & 3) << 2;
    if (tex1 >> 5 & 1) k->flags |= 32;
    k->tex_size[0] = w; k->tex_size[1] = h;
    k->wrap[0] = (uint32_t)clamp & 3; k->wrap[1] = (uint32_t)(clamp >> 2) & 3;
    k->region[0] = (uint32_t)(clamp >> 4) & 1023; k->region[1] = (uint32_t)(clamp >> 14) & 1023;
    k->region[2] = (uint32_t)(clamp >> 24) & 1023; k->region[3] = (uint32_t)(clamp >> 34) & 1023;
    k->alpha_scale = 255;
    unsigned page = (tbp / 32) & 511;
    int owner = native_owner(n, g, page);
    if (owner >= 0) {
        NativeSurface *s = &n->surfaces[owner];
        int x, y;
        if (!s->depth && psm_color(psm) && psm_wide(psm) == (int)s->wide && tbp % 32 == 0 &&
            (tbw == s->bw || w <= 64) && native_page_origin(s, page, &x, &y)) {
            c->texture_surface = (int16_t)owner;
            k->tex_offset[0] = x; k->tex_offset[1] = y;
            k->alpha_scale = 255;
            if (psm == GS_PSMCT24) { k->flags |= 256; k->texa_alpha = (float)(g->reg[GS_TEXA] & 255); }
            n->stats.texture_from_surface++;
            return;
        }
    }
    uint32_t pages[16] = {0};
    gs_mark_pages(pages, psm, tbp, tbw ? tbw : 1, 0, 0, w - 1, h - 1);
    uint64_t key = native_mix(native_mix(tex0 & 0x7FFFFFFFFFFull, (uint64_t)w << 32 | h), psm == GS_PSMCT24 || psm_wide(psm) ? g->reg[GS_TEXA] : 0);
    /* the menus' font: its copy has the button icons chosen now (button_icons.c) */
    if (psm == GS_PSMT4HL) key = native_mix(key, (uint64_t)font_sharp_icon_style_latch());
    unsigned gpu_pages = 0, gpu_page = 0;
    /* Visit set pages in the same ascending order as the key's original scan.
       Most textures touch only a handful of the 512 GS pages. */
    for (unsigned word = 0; word < 16; word++)
        for (uint32_t bits = pages[word]; bits; bits &= bits - 1) {
            unsigned p = word * 32 + native_bit_first(bits);
            key = native_mix(key, native_page_hash(n, g, p));
            if (native_owner(n, g, p) >= 0 && !gpu_pages++) gpu_page = p;
        }
    if (psm == GS_PSMT8 || psm == GS_PSMT4 || psm == GS_PSMT8H || psm == GS_PSMT4HL || psm == GS_PSMT4HH) {
        /* the digest of the 16 or 256 converted entries (native_palette, finding 148) */
        key = native_mix(key, native_palette_digest(n, g, tex0, psm == GS_PSMT8 || psm == GS_PSMT8H));
    }
    if (gpu_pages) {
        /* The menus' font lives in a depth buffer's top bytes; the sharp one is drawn from the
           CPU copy, which holds the font as the game uploaded it. */
        if (n->replace_on && font_sharp_active() && font_sharp_match(tex0)) {
            c->texture = native_decode(n, g, ctxt, tex0, w, h, key);
            if (c->texture && (c->texture->sharp || c->texture->pack)) native_replace(n, g, ctxt, c);
            if (c->replacement) return;
            c->texture = NULL;
        }
        if (native_convert_texture(n, g, ctxt, tex0, w, h, pages, key, c)) return;
        /* Drawn over, but the CPU copy holds it (a Z24 buffer's top byte), or out of reach. */
        if (owner >= 0) {
            const NativeSurface *s = &n->surfaces[owner];
            unsigned why = s->depth ? 0 : !psm_color(psm) ? 1 : psm_wide(psm) != (int)s->wide ? 2 :
                           tbp % 32 ? 3 : !(tbw == s->bw || w <= 64) ? 4 : 5;
            n->stats.texture_unsupported++;
            if (n->texlog)
                fprintf(n->texlog, "%llu,unsupported,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", (unsigned long long)n->field, why,
                        psm, tbp, tbw, w, h, s->page, s->bw, s->wide, s->depth);
            if (++n->stats.unsupported_why[why] <= 12 && getenv("DHD_NATIVE_WHY"))
                fprintf(stderr, "native texture: psm=%u tbp=%u tbw=%u %ux%u on surface page=%u bw=%u wide=%u depth=%u\n",
                        psm, tbp, tbw, w, h, s->page, s->bw, s->wide, s->depth);
        } else if (n->texlog)
            fprintf(n->texlog, "%llu,gpu_pages,%u,%u,%u,%u,%u,%u,%u,,,\n", (unsigned long long)n->field, gpu_pages, psm, tbp,
                    tbw, w, h, gpu_page);
    }
    c->texture = native_decode(n, g, ctxt, tex0, w, h, key);
    if (c->texture && (n->texpack || c->texture->sharp)) native_replace(n, g, ctxt, c);
    if (!c->texture) {
        k->flags &= ~1u;
        if (n->texlog)
            fprintf(n->texlog, "%llu,decode_failed,,%u,%u,%u,%u,%u,,,,\n", (unsigned long long)n->field, psm, tbp, tbw, w, h);
    }
}

/* ---- GS state to draw state ---- */

enum { BLEND_ZERO = 1, BLEND_ONE = 2, BLEND_SA = 5, BLEND_ISA = 6, BLEND_DA = 7, BLEND_IDA = 8, BLEND_F = 14, BLEND_IF = 15 };

/* GS blending ((A - B) * C >> 7) + D per colour, as factors of Cs and Cd: m * C + k. */
static uint32_t native_blend(Native *n, uint64_t alpha)
{
    unsigned a = (unsigned)alpha & 3, b = (unsigned)(alpha >> 2) & 3, c = (unsigned)(alpha >> 4) & 3, d = (unsigned)(alpha >> 6) & 3;
    static const unsigned positive[3] = {BLEND_SA, BLEND_DA, BLEND_F}, inverse[3] = {BLEND_ISA, BLEND_IDA, BLEND_IF};
    unsigned factor[2], op = 1, negative[2] = {0, 0};
    if (c == 3) c = 2;
    if (c == 1 && a != b) n->stats.blend_dest_alpha++;   /* D3D gives Ad / 255, the GS Ad / 128 */
    for (unsigned side = 0; side < 2; side++) {
        int m = (a == side) - (b == side), k = d == side;
        if (a == b) m = 0;
        if (!m) factor[side] = k ? BLEND_ONE : BLEND_ZERO;
        else if (m > 0) factor[side] = k ? (n->stats.blend_approximate++, BLEND_ONE) : positive[c];
        else if (k) factor[side] = inverse[c];
        else factor[side] = positive[c], negative[side] = 1;
    }
    if (negative[0] && negative[1]) n->stats.blend_approximate++;
    else if (negative[0]) op = 3;   /* REV_SUBTRACT: Cd * f - Cs * f */
    else if (negative[1]) op = 2;   /* SUBTRACT */
    return 1 | factor[0] << 1 | factor[1] << 6 | op << 11;
}

static void native_state(Native *n, Gs *g, unsigned prim, unsigned ctxt)
{
    /* Initialize on the GS worker, in the same host FP mode as native_set.
       Creation can run on the EE thread with its different rounding mode. */
    if (!n->vertex_fog_ready) {
        for (unsigned i = 0; i < 256; i++) n->vertex_fog[i] = (float)i / 255.0f;
        n->vertex_fog_ready = 1;
    }
    n->conv_serial++;  /* zmax, zscale and uv_scale may change below */
    NativeCommand *c = &n->current;
    uint64_t frame = g->reg[GS_FRAME_1 + ctxt], zbuf = g->reg[GS_ZBUF_1 + ctxt], test = g->reg[GS_TEST_1 + ctxt];
    uint64_t scissor = g->reg[GS_SCISSOR_1 + ctxt];
    unsigned fbp = (unsigned)frame & 511, fbw = (unsigned)(frame >> 16) & 63, fpsm = (unsigned)(frame >> 24) & 63;
    uint32_t fbmsk = (uint32_t)(frame >> 32);
    memset(c, 0, sizeof *c);
    c->kind = NATIVE_DRAW; c->target = c->depth = c->texture_surface = c->source = c->convert = -1;
    n->skip = 0; n->passes = 0;
    if (!psm_color(fpsm)) { n->skip = 1; return; }
    c->target = (int16_t)native_surface(n, fbp, fbw, fpsm, 0);
    if (c->target < 0) { n->skip = 1; return; }
    c->scissor.left = (int)(scissor & 2047); c->scissor.right = (int)(scissor >> 16 & 2047) + 1;
    c->scissor.top = (int)(scissor >> 32 & 2047); c->scissor.bottom = (int)(scissor >> 48 & 2047) + 1;
    n->target_psm = fpsm;
    unsigned mask = 15;
    if (psm_wide(fpsm)) {
        if ((fbmsk & 0xF8) == 0xF8) mask &= ~1u;
        if ((fbmsk & 0xF800) == 0xF800) mask &= ~2u;
        if ((fbmsk & 0xF80000) == 0xF80000) mask &= ~4u;
        if (fbmsk & 0x80000000u) mask &= ~8u;
    } else {
        for (unsigned i = 0; i < 4; i++) if ((fbmsk >> (i * 8) & 255) == 255) mask &= ~(1u << i);
        if (fpsm == GS_PSMCT24) mask &= ~8u;
    }
    /* Z: ZTE off behaves as ALWAYS. GS Z is an integer of the buffer's width; depth is its fraction. */
    unsigned ztst = test >> 16 & 1 ? (unsigned)(test >> 17) & 3 : 1, zwrite = !(zbuf >> 32 & 1);
    unsigned zpsm = 48 | ((unsigned)(zbuf >> 24) & 15);
    if (ztst == 0) { n->skip = 1; return; }
    if (ztst != 1 || zwrite) {
        c->depth = (int16_t)native_surface(n, (unsigned)zbuf & 511, fbw, zpsm, 1);
        c->depth_key = c->depth >= 0 ? (uint8_t)(8 | ztst | zwrite << 2) : 0;
        if (c->depth >= 0 && zwrite) n->surfaces[c->depth].zpsm = zpsm;
        n->depth_psm = zpsm;
    }
    n->zmax = zpsm == GS_PSMZ32 ? UINT32_MAX : (zpsm & 7) == 1 ? 0xFFFFFF : 0xFFFF;
    n->zscale = zpsm == GS_PSMZ32 ? (float)(1.0 / 4294967296.0) : (zpsm & 7) == 1 ? 1.0f / 16777216 : 1.0f / 65536;
    NativeConstants *k = &c->constants;
    k->target[0] = k->target[1] = 2048;
    if ((prim & 7) == 6) k->flags |= 1024;   /* a sprite: snap only nearest-filtered draws when upscaled (finding 196) */
    if (prim & 16) native_texture(n, g, ctxt, c);
    /* GS dimensions are powers of two (native_texture); a missing texture uses 1.
       Scaling by their reciprocal is exact, including the fixed-point UV shift. */
    if (prim & 256) {
        n->uv_scale[0] = 1.0f / (16.0f * (k->tex_size[0] ? k->tex_size[0] : 1));
        n->uv_scale[1] = 1.0f / (16.0f * (k->tex_size[1] ? k->tex_size[1] : 1));
    }
    if (prim & 32) {
        uint64_t fog = g->reg[GS_FOGCOL];
        k->flags |= 16;
        k->fog[0] = (float)(fog & 255); k->fog[1] = (float)(fog >> 8 & 255); k->fog[2] = (float)(fog >> 16 & 255);
    }
    if (test >> 14 & 1) n->stats.date_ignored++;
    c->blend_key = prim & 64 ? native_blend(n, g->reg[GS_ALPHA_1 + ctxt]) : 0;
    c->blend_fix = (uint32_t)(g->reg[GS_ALPHA_1 + ctxt] >> 32) & 255;
    /* Cs * As + Cd (or + 0): As reaches 2 on the GS but a blend factor stops at 1, so the shader
       premultiplies and the blend takes Cs as it is (sun halos and glows). */
    unsigned source = c->blend_key >> 1 & 31, destination = c->blend_key >> 6 & 31;
    if (c->blend_key && source == BLEND_SA && (destination == BLEND_ZERO || destination == BLEND_ONE)) {
        c->blend_key = (c->blend_key & ~(31u << 1)) | BLEND_ONE << 1;
        k->flags |= 512;
    }
    c->color_mask = (uint8_t)mask;
    /* Alpha test. AFAIL decides what a failing pixel still writes: a second pass draws exactly
       the failing pixels with the remaining writes. */
    unsigned ate = test & 1, atst = (unsigned)(test >> 1) & 7, afail = (unsigned)(test >> 12) & 3;
    if (ate && atst != 1) {
        k->flags |= 64; k->atst = atst; k->aref = (uint32_t)(test >> 4) & 255;
        if (afail) {
            n->second = *c;
            n->second.constants.flags |= 128;
            if (afail == 1) n->second.depth_key &= ~4;                       /* FB_ONLY */
            if (afail == 2) n->second.color_mask = 0;                        /* ZB_ONLY */
            if (afail == 3) { n->second.depth_key &= ~4; n->second.color_mask &= 7; } /* RGB_ONLY */
            n->passes = 1;
        }
    }
    if (!c->color_mask && !(c->depth_key & 4) && !n->passes) n->skip = 1;
}

/* ---- recording ---- */

static void native_reserve(NativeFrame *f, unsigned more)
{
    if (f->vertex_count + more <= f->vertex_capacity) return;
    unsigned capacity = f->vertex_capacity ? f->vertex_capacity * 2 : 1 << 16;
    while (capacity < f->vertex_count + more) capacity *= 2;
    NativeVertex *grown = realloc(f->vertices, capacity * sizeof *grown);
    if (!grown) native_oom("vertices");
    f->vertices = grown; f->vertex_capacity = capacity;
}

static void native_reserve_indices(NativeFrame *f, unsigned more)
{
    if (f->index_count + more <= f->index_capacity) return;
    unsigned capacity = f->index_capacity ? f->index_capacity * 2 : 1 << 16;
    while (capacity < f->index_count + more) capacity *= 2;
    uint32_t *grown = realloc(f->indices, capacity * sizeof *grown);
    if (!grown) native_oom("indices");
    f->indices = grown; f->index_capacity = capacity;
}

/* Where the open draw's vertices (indices, in an indexed field) go next. */
static unsigned native_next(const Native *n, const NativeFrame *f)
{
    return n->indexed ? f->index_count : f->vertex_count;
}

static void native_open(Native *n)
{
    n->upload_epoch++;
    NativeFrame *f = n->recording;
    NativeCommand *c = native_command(n);
    *c = n->current;
    c->first = native_next(n, f); c->count = 0;
    if (n->passes) {
        NativeCommand *second = native_command(n);
        *second = n->second;
        second->first = native_next(n, f); second->count = 0;
    }
    n->current_valid = 1;
    n->box[0] = n->box[1] = 4096; n->box[2] = n->box[3] = -4096;
    n->stats.commands += 1 + n->passes;
    if (!n->interp) return;  /* only interpolation consumes the segment hash */
    /* Not the target: the game draws alternate fields into its two frame buffers. */
    uint32_t key = 2166136261u;
    /* the texture is a host pointer: a 64-bit host hashes its high half as well (none on 32-bit) */
    const uint32_t parts[] = {(uint32_t)c->depth, c->depth_key, c->color_mask, c->blend_key,
                              c->blend_fix, (uint32_t)(uintptr_t)c->texture,
#if UINTPTR_MAX > 0xFFFFFFFFu
                              (uint32_t)((uintptr_t)c->texture >> 32),
#endif
                              (uint32_t)c->texture_surface, c->constants.flags};
    for (unsigned i = 0; i < sizeof parts / sizeof *parts; i++) key = (key ^ parts[i]) * 16777619u;
    n->segment_key = key;
    n->segment_tags = UINT64_MAX;
}

/* DHD_NATIVE_SURFACES: a finished draw (the open state's primitive type). */
static void native_note_usage(Native *n, const NativeCommand *c)
{
    NativeUsage *u = &n->usage[c->target];
    u->draws++;
    if (c->depth >= 0 && (c->depth_key & 8)) u->depth_draws++, u->depth_with = c->depth;
    if (c->texture_surface < 0) return;
    unsigned s = (unsigned)c->texture_surface;
    n->usage[s].sampled++;
    u->pair[s]++;
    if ((n->state_prim & 7) != 6) return;
    const NativeFrame *f = n->recording;
    const NativeVertex *a = n->indexed ? &f->vertices[f->indices[c->first]] : &f->vertices[c->first];
    const NativeVertex *b = n->indexed ? &f->vertices[f->indices[c->first + 5]] : a + 5;
    float tw = (float)c->constants.tex_size[0];
    float ua = a->s / (a->q ? a->q : 1) * tw, ub = b->s / (b->q ? b->q : 1) * tw, dx = fabsf(b->x - a->x);
    float ratio = dx > 0 ? fabsf(ub - ua) / dx : 0;
    u->ratio[s][ratio < 0.75f ? 0 : ratio < 1.5f ? 1 : ratio < 2.5f ? 2 : 3]++;
    if (ratio >= 0.75f && ratio < 1.5f) {
        float c0 = ua - (a->x - 0.5f);
        u->offset[s][(int)((c0 - floorf(c0)) * 4) & 3]++;
    }
}

/* Ends the open draw. The pages it drew become its surfaces'; pages they did not own yet are
   imported first, so those commands go in front of the draw. */
static void native_close(Native *n, Gs *g)
{
    NativeFrame *f = n->recording;
    if (!n->current_valid) return;
    n->current_valid = 0;
    unsigned draws = 1 + n->passes, at = f->command_count - draws;
    NativeCommand saved[2];
    memcpy(saved, &f->commands[at], draws * sizeof *saved);
    unsigned count = native_next(n, f) - saved[0].first;
    if (!count) { f->command_count = at; return; }
    saved[0].count = saved[draws - 1].count = count;
    const NativeRect *s = &saved[0].scissor;
    int x0 = n->box[0] > s->left ? n->box[0] : s->left, y0 = n->box[1] > s->top ? n->box[1] : s->top;
    int x1 = n->box[2] < s->right - 1 ? n->box[2] : s->right - 1, y1 = n->box[3] < s->bottom - 1 ? n->box[3] : s->bottom - 1;
    f->command_count = at;
    native_claim(n, g, saved[0].target, n->target_psm, x0, y0, x1, y1);
    if (saved[0].depth >= 0) native_claim(n, g, saved[0].depth, n->depth_psm, x0, y0, x1, y1);
    NativeCommand *c = native_command(n);
    *c = saved[0];
    if (draws == 2) { c = native_command(n); *c = saved[1]; }
    if (n->usage) native_note_usage(n, &saved[0]);
}

static void native_set(NativeVertex *o, const GsVertex *v, const GsVertex *colour, const Native *n,
                       int fst, float x, float y)
{
    uint32_t z = v->z > n->zmax ? n->zmax : v->z;
    o->x = x; o->y = y;
    /* zscale is 2^-16, 2^-24 or 2^-32: integer rounding commutes with this
       exact exponent shift; all intermediate values are normal floats. */
    o->z = (float)z * n->zscale;
    if (fst) { o->s = (float)v->u * n->uv_scale[0]; o->t = (float)v->v * n->uv_scale[1]; o->q = 1; }
    else { o->s = v->s; o->t = v->t; o->q = v->q; }
    o->fog = n->vertex_fog[v->fog & 255];
    o->rgba = colour->rgba;
}

static int native_same_vertex(const NativeVertex *a, const NativeVertex *b)
{
    uint64_t p[4], q[4];
    memcpy(p, a, sizeof p); memcpy(q, b, sizeof q);
    return ((p[0] ^ q[0]) | (p[1] ^ q[1]) | (p[2] ^ q[2]) | (p[3] ^ q[3])) == 0;
}

/* The field's index of vertex t: `at` when that vertex has the same bytes, else t stored. */
static uint32_t native_index_of(NativeFrame *f, const NativeVertex *t, uint32_t at)
{
    if (at != UINT32_MAX && native_same_vertex(&f->vertices[at], t)) return at;
    f->vertices[f->vertex_count] = *t;
    return f->vertex_count++;
}

/* An indexed field's triangle of GS primitive type `type`. A strip's (4) first two vertices are
   the previous triangle's last two, a fan's (5) its first and last: each is drawn from that one
   when their bytes are the same; the others are stored. The previous triangle may be another
   primitive's (equal bytes draw the same either way), so the picture is the expanded one. */
static void native_triangle_indexed(NativeFrame *f, unsigned type, const NativeVertex t[3])
{
    uint32_t *o = f->indices + f->index_count, a = UINT32_MAX, b = UINT32_MAX;
    if ((type == 4 || type == 5) && f->index_count >= 3) a = o[type == 4 ? -2 : -3], b = o[-1];
    o[0] = native_index_of(f, &t[0], a);
    o[1] = native_index_of(f, &t[1], b);
    o[2] = native_index_of(f, &t[2], UINT32_MAX);
    f->index_count += 3;
}

/* An indexed field's quad: corners 0 1 2 3 (two triangles, 0 1 2 and 2 1 3, as expanded). */
static void native_quad_indexed(NativeFrame *f, const NativeVertex q[4])
{
    uint32_t base = f->vertex_count, *o = f->indices + f->index_count;
    memcpy(f->vertices + base, q, 4 * sizeof *q);
    f->vertex_count += 4;
    o[0] = base; o[1] = base + 1; o[2] = base + 2; o[3] = base + 2; o[4] = base + 1; o[5] = base + 3;
    f->index_count += 6;
}

/* Bounded field diagnostics: no EE/GS state is changed. Coordinates are logged before
   native conversion and sprite snapping. Captures and CSVs belong under ignored build/. */
static FILE *native_field_trace(const char *kind)
{
    static int initialized;
    static unsigned long long first, last;
    static FILE *draws, *registers;
    extern uint64_t kernel_vsync_count;
    if (!initialized) {
        initialized = 1;
        const char *prefix = getenv("DHD_FIELD_TRACE"), *range = getenv("DHD_FIELD_TRACE_FIELDS");
        if (prefix && range && sscanf(range, "%llu-%llu", &first, &last) == 2) {
            char path[1024];
            snprintf(path, sizeof path, "%s-draws.csv", prefix); draws = fopen(path, "w");
            snprintf(path, sizeof path, "%s-registers.csv", prefix); registers = fopen(path, "w");
            if (draws) fputs("field,path,type,context,frame,tex0,xyoffset,vertex,x,y,u,v,z\n", draws);
            if (registers) fputs("field,xyoffset1,xyoffset2,pmode,smode2,dispfb1,display1,dispfb2,display2,csr\n", registers);
        }
    }
    if (kernel_vsync_count < first || kernel_vsync_count > last) return NULL;
    return *kind == 'd' ? draws : registers;
}

/* The draw offset native_record converts with. The game alternates OFY by 8/16 of a framebuffer
   line in INT+FFMD mode. Scanout line-doubles to progressive output; retaining that field
   displacement makes the whole scene bob, and sprite rounding can turn it into a full line.
   Remove only the half-line component on screen-sized targets, before rounding. Small offscreen
   targets (shadows/blur) keep their own coordinates. This local offset never reaches the exact
   GS rasterizer, probe replay or EE readbacks. */
static uint64_t native_offset(const Gs *g, unsigned ctxt)
{
    uint64_t offset = g->reg[GS_XYOFFSET_1 + ctxt];
    if ((offset >> 32 & 15) == 8 && (g->privileged[GS_SMODE2 / 16] & 3) == 3 &&
        (g->reg[GS_FRAME_1 + ctxt] >> 16 & 63) >= 8)
        offset -= UINT64_C(8) << 32;
    return offset;
}

static void native_record(Native *n, Gs *g, unsigned type, const GsVertex *v, unsigned count, unsigned prim, unsigned ctxt)
{
    FILE *trace = n->field_trace_on ? native_field_trace("draws") : NULL;
    if (trace) {
        extern uint64_t kernel_vsync_count;
        for (unsigned i = 0; i < count; i++)
            fprintf(trace, "%llu,%u,%u,%u,%llX,%llX,%llX,%u,%u,%u,%u,%u,%u\n",
                    (unsigned long long)kernel_vsync_count, g->source_path, type, ctxt,
                    (unsigned long long)g->reg[GS_FRAME_1 + ctxt], (unsigned long long)g->reg[GS_TEX0_1 + ctxt],
                    (unsigned long long)g->reg[GS_XYOFFSET_1 + ctxt], i, v[i].x, v[i].y, v[i].u, v[i].v, v[i].z);
    }
    n->recording->path1 += g->source_path == 1;
    if (n->state_version != g->state_version || n->state_ctxt != ctxt || n->state_prim != prim ||
        ((prim & 16) && n->written != g->pixels_written) || !n->current_valid || n->mesh_open) {
        native_close(n, g);
        native_state(n, g, prim, ctxt);
        n->state_version = g->state_version; n->state_ctxt = ctxt; n->state_prim = prim;
        n->written = g->pixels_written;
        n->mesh_open = 0;
        if (n->skip) return;
        native_open(n);
    }
    if (n->skip) return;
    NativeFrame *f = n->recording;
    uint64_t offset = native_offset(g, ctxt);
    int fst = prim >> 8 & 1, flat = !(prim >> 3 & 1);
    float x[3], y[3];
    int px[3], py[3];
    int left = n->box[0], top = n->box[1], right = n->box[2], bottom = n->box[3];
    /* A triangle's vertices are converted here. A strip's first two are the last triangle's last
       two, a fan's its first and last: those take the last triangle's conversion when their
       registers are the same bytes (native_set is a function of them, the offset, FST and the
       state's scales), so most strip vertices are converted once, not three times. */
    int triangle = type >= 3 && type <= 5 && count == 3;
    NativeVertex t[3];
    unsigned reused = 0;
    if (triangle && (type == 4 || type == 5) && n->conv_ok == n->conv_serial && n->conv_offset == offset &&
        n->conv_fst == fst) {
        if (!memcmp(&v[0], &n->conv_gs[0], sizeof *v)) t[0] = n->conv[0], reused |= 1;
        if (!memcmp(&v[1], &n->conv_gs[1], sizeof *v)) t[1] = n->conv[1], reused |= 2;
    }
    /* * 0.0625f: the same value as / 16.0f (a power of two), without a divide a vertex (finding 146) */
    for (unsigned i = 0; i < count; i++) {
        if (!(reused >> i & 1)) {
            x[i] = (float)((int)v[i].x - (int)(offset & 65535)) * 0.0625f + 0.5f;
            y[i] = (float)((int)v[i].y - (int)(offset >> 32 & 65535)) * 0.0625f + 0.5f;
            px[i] = (int)x[i], py[i] = (int)y[i];
            if (triangle) native_set(&t[i], &v[i], &v[i], n, fst, x[i], y[i]);
        } else px[i] = (int)t[i].x, py[i] = (int)t[i].y;   /* native_set keeps x and y */
        if (px[i] - 1 < left) left = px[i] - 1;
        if (py[i] - 1 < top) top = py[i] - 1;
        if (px[i] + 1 > right) right = px[i] + 1;
        if (py[i] + 1 > bottom) bottom = py[i] + 1;
    }
    n->box[0] = left; n->box[1] = top; n->box[2] = right; n->box[3] = bottom;
    if (triangle) {
        unsigned shared = type == 5 ? 0 : 1;   /* a fan keeps its first vertex, a strip its second */
        n->conv_gs[0] = v[shared]; n->conv[0] = t[shared];
        n->conv_gs[1] = v[2]; n->conv[1] = t[2];
        n->conv_offset = offset; n->conv_fst = fst; n->conv_ok = n->conv_serial;
        if (flat) t[0].rgba = t[1].rgba = t[2].rgba;   /* flat: the last vertex's colour */
    }
    const GsVertex *last = &v[count - 1];
    native_reserve(f, 6);
    if (n->indexed) native_reserve_indices(f, 6);
    if (n->interp) {
        uint64_t tags = g->packets[0] + g->packets[1] + g->packets[2];
        if (tags != n->segment_tags || !f->segment_count) {
            n->segment_tags = tags;
            if (f->segment_count == f->segment_capacity) {
                unsigned capacity = f->segment_capacity ? f->segment_capacity * 2 : 4096;
                NativeSegment *grown = realloc(f->segments, capacity * sizeof *grown);
                if (!grown) native_oom("segments");
                f->segments = grown; f->segment_capacity = capacity;
            }
            f->segments[f->segment_count++] = (NativeSegment){f->vertex_count, 0, n->segment_key};
        }
    }
    NativeVertex *o = &f->vertices[f->vertex_count];
    if (triangle) {
        if (n->indexed) native_triangle_indexed(f, type, t);
        else {
            memcpy(o, t, sizeof t);
            f->vertex_count += 3;
        }
    } else if (type == 6 && count == 2) {
        /* Sprites: the second vertex supplies Z, fog and colour; texture coordinates span the corners. */
        NativeVertex a, b;
        native_set(&a, &v[0], last, n, fst, x[0], y[0]);
        native_set(&b, &v[1], last, n, fst, x[1], y[1]);
        a.z = b.z; a.fog = b.fog;
        if (!fst) a.q = b.q, a.s *= b.q / (v[0].q ? v[0].q : 1), a.t *= b.q / (v[0].q ? v[0].q : 1);
        NativeVertex c0 = a, c1 = b, c2 = a, c3 = b;
        c1.y = a.y; c1.t = a.t;   /* top right */
        c2.y = b.y; c2.t = b.t;   /* bottom left */
        if (n->indexed) {
            NativeVertex q[4] = {c0, c1, c2, c3};
            native_quad_indexed(f, q);
        } else {
            o[0] = c0; o[1] = c1; o[2] = c2; o[3] = c2; o[4] = c1; o[5] = c3;
            f->vertex_count += 6;
        }
    } else {
        /* Points and lines: one-pixel quads along the major axis. */
        NativeVertex a, b;
        native_set(&a, &v[0], flat ? last : &v[0], n, fst, x[0] - 0.5f, y[0] - 0.5f);
        b = a;
        if (count == 2) native_set(&b, &v[1], last, n, fst, x[1] - 0.5f, y[1] - 0.5f);
        int horizontal = fabsf(b.x - a.x) >= fabsf(b.y - a.y);
        NativeVertex q[4] = {a, a, b, b};
        if (count == 1) { q[1].x += 1; q[2].y += 1; q[3].x += 1; q[3].y += 1; }
        else if (horizontal) { q[1].y += 1; q[3].y += 1; }
        else { q[1].x += 1; q[3].x += 1; }
        for (unsigned i = 0; i < 4; i++) q[i].x += 0.5f, q[i].y += 0.5f;
        if (n->indexed) native_quad_indexed(f, q);
        else {
            o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[2]; o[4] = q[1]; o[5] = q[3];
            f->vertex_count += 6;
        }
    }
    if (n->interp) f->segments[f->segment_count - 1].count = f->vertex_count - f->segments[f->segment_count - 1].first;
    n->stats.primitives++;
}

/* ---- exact Z for the lens-flare probe ---- */

static int native_overlap(const int a[4], const int b[4])
{
    return a[0] <= b[2] && b[0] <= a[2] && a[1] <= b[3] && b[1] <= a[3];
}

/* Whether a primitive certainly covers pixel (px, py). Sprites use the rasterizer's exact rule,
   min <= p < max (raster.wgsl draw_pixel); triangles count only strict interior pixels, so
   their edge rules never matter. The box is already clipped to the scissor. */
static int native_covers(const NativeLogDraw *d, const uint64_t *reg, unsigned ctxt, int px, int py)
{
    int X = px * 16, Y = py * 16, x[3], y[3];
    unsigned scanmask = (unsigned)reg[GS_SCANMSK] & 3;
    if (px < d->box[0] || px > d->box[2] || py < d->box[1] || py > d->box[3]) return 0;
    if ((scanmask == 2 && !(py & 1)) || (scanmask == 3 && (py & 1))) return 0;
    uint64_t offset = reg[GS_XYOFFSET_1 + ctxt];
    for (unsigned i = 0; i < d->count; i++) {
        x[i] = (int)d->v[i].x - (int)(offset & 65535);
        y[i] = (int)d->v[i].y - (int)(offset >> 32 & 65535);
    }
    if (d->type == 6) {
        int x0 = x[0] < x[1] ? x[0] : x[1], x1 = x[0] < x[1] ? x[1] : x[0];
        int y0 = y[0] < y[1] ? y[0] : y[1], y1 = y[0] < y[1] ? y[1] : y[0];
        return X >= x0 && X < x1 && Y >= y0 && Y < y1;
    }
    if (d->count != 3) return 0;
    int64_t e[3];
    for (unsigned i = 0; i < 3; i++) {
        unsigned j = (i + 1) % 3;
        e[i] = (int64_t)(x[j] - x[i]) * (Y - y[i]) - (int64_t)(y[j] - y[i]) * (X - x[i]);
    }
    return (e[0] > 0 && e[1] > 0 && e[2] > 0) || (e[0] < 0 && e[1] < 0 && e[2] < 0);
}

/* The context a logged primitive drew with. */
static unsigned native_log_context(const uint64_t *reg)
{
    return (unsigned)((reg[GS_PRMODECONT] & 1 ? reg[GS_PRIM] : reg[GS_PRMODE]) >> 9 & 1);
}

static void native_log(Native *n, Gs *g, unsigned type, const GsVertex *v, unsigned count, unsigned zbp, unsigned fbp,
                       int unconditional, const int box[4])
{
    NativeLog *log = &n->logs[1];
    /* state_version covers every drawing register except these, which the replay also reads;
       vertex registers change per primitive and travel with the logged vertices. */
    const uint64_t *last = log->state_count ? log->states[log->state_count - 1].reg : NULL;
    if (!last || log->state_version != g->state_version || last[GS_PRIM] != g->reg[GS_PRIM] ||
        last[GS_PRMODE] != g->reg[GS_PRMODE] || last[GS_PRMODECONT] != g->reg[GS_PRMODECONT] ||
        last[GS_XYOFFSET_1] != g->reg[GS_XYOFFSET_1] || last[GS_XYOFFSET_2] != g->reg[GS_XYOFFSET_2]) {
        if (log->state_count == log->state_capacity) {
            unsigned capacity = log->state_capacity ? log->state_capacity * 2 : 1024;
            NativeLogState *grown = realloc(log->states, capacity * sizeof *grown);
            if (!grown) native_oom("log states");
            log->states = grown; log->state_capacity = capacity;
        }
        NativeLogState *s = &log->states[log->state_count++];
        memcpy(s->reg, g->reg, sizeof s->reg); memcpy(s->clut, g->clut, sizeof s->clut);
        log->state_version = g->state_version;
    }
    if (log->draw_count == log->draw_capacity) {
        unsigned capacity = log->draw_capacity ? log->draw_capacity * 2 : 16384;
        NativeLogDraw *grown = realloc(log->draws, capacity * sizeof *grown);
        if (!grown) native_oom("log draws");
        log->draws = grown; log->draw_capacity = capacity;
    }
    NativeLogDraw *d = &log->draws[log->draw_count++];
    d->type = type; d->count = count; d->state = log->state_count - 1;
    d->zbp = zbp; d->fbp = fbp; d->unconditional = (uint8_t)unconditional;
    memcpy(d->box, box, sizeof d->box);
    memcpy(d->v, v, count * sizeof *v);
}

static int native_writes(const NativeLogDraw *d, unsigned zbp) { return d->zbp == zbp || d->fbp == zbp; }

/* Runs logged primitives [first, end) through the exact backend, clipped to rect. */
/* The exact backend's clip: rect, or the watch rectangle (nothing while there is none). */
static void native_clip(Native *n, const int rect[4])
{
    static const int none[4] = {1, 1, 0, 0};
    memcpy(n->exact->clip, rect ? rect : n->watch.on ? n->watch.rect : none, sizeof n->exact->clip);
}

static void native_band_add(Native *n, unsigned page)
{
    if (page < 512 && !n->band[page] && n->band_count < 64) {
        n->band[page] = 1;
        n->band_pages[n->band_count++] = (uint16_t)page;
    }
}

/* The pixels of a 32-bit buffer (base page, width w) that lie in the band. A probe reads the
   first NATIVE_BAND_ROWS rows of a band page in the Z layout; a colour layout holds the same
   words at rows 16 on (the Z and colour block orders swap page halves: PCSX2 blockTable32Z). */
enum { NATIVE_BAND_ROWS = 4 };
static int native_band_rect(const Native *n, unsigned base, unsigned w, int zlayout, int rect[4])
{
    int found = 0;
    if (!w) return 0;
    for (unsigned i = 0; i < n->band_count; i++) {
        unsigned page = n->band_pages[i];
        if (page < base || (page - base) / w >= 64) continue;
        int x = (int)((page - base) % w) * 64, y = (int)((page - base) / w) * 32 + (zlayout ? 0 : 16);
        if (x < rect[0]) rect[0] = x;
        if (y < rect[1]) rect[1] = y;
        if (x + 63 > rect[2]) rect[2] = x + 63;
        if (y + NATIVE_BAND_ROWS - 1 > rect[3]) rect[3] = y + NATIVE_BAND_ROWS - 1;
        found = 1;
    }
    return found;
}

static void native_exact_replay(Native *n, Gs *g, const int rect[4], unsigned zbp)
{
    static uint64_t saved_reg[128];
    static uint16_t saved_clut[1024];
    /* Latest point after which every probed pixel is overwritten unconditionally. */
    unsigned total = n->logs[0].draw_count + n->logs[1].draw_count, start = 0, width = (unsigned)(rect[2] - rect[0] + 1);
    unsigned height = (unsigned)(rect[3] - rect[1] + 1), left = width * height;
    uint8_t found[NATIVE_WATCH * NATIVE_WATCH];
    if (width > NATIVE_WATCH || height > NATIVE_WATCH) { n->stats.probe_uncovered++; return; }
    memset(found, 0, width * height);
    for (unsigned i = total; i-- > 0 && left;) {
        NativeLog *log = i < n->logs[0].draw_count ? &n->logs[0] : &n->logs[1];
        NativeLogDraw *d = &log->draws[i < n->logs[0].draw_count ? i : i - n->logs[0].draw_count];
        if (!d->unconditional || !native_writes(d, zbp) || !native_overlap(d->box, rect)) continue;
        const uint64_t *reg = log->states[d->state].reg;
        unsigned ctxt = native_log_context(reg);
        for (unsigned y = 0; y < height; y++)
            for (unsigned x = 0; x < width; x++)
                if (!found[y * width + x] && native_covers(d, reg, ctxt, rect[0] + (int)x, rect[1] + (int)y))
                    found[y * width + x] = 1, left--;
        if (!left) start = i;
    }
    if (left) n->stats.probe_uncovered++;
    if (left && n->trace_probes) {
        unsigned first = 0;
        while (found[first]) first++;
        int px = rect[0] + (int)(first % width), py = rect[1] + (int)(first / width), shown = 0;
        for (unsigned i = total; i-- > 0 && shown < 40;) {
            NativeLog *log = i < n->logs[0].draw_count ? &n->logs[0] : &n->logs[1];
            NativeLogDraw *d = &log->draws[i < n->logs[0].draw_count ? i : i - n->logs[0].draw_count];
            if (px < d->box[0] || px > d->box[2] || py < d->box[1] || py > d->box[3]) continue;
            const uint64_t *reg = log->states[d->state].reg;
            unsigned ctxt = native_log_context(reg);
            uint64_t zbuf = reg[GS_ZBUF_1 + ctxt], frame = reg[GS_FRAME_1 + ctxt], test = reg[GS_TEST_1 + ctxt];
            fprintf(stderr, "native uncovered %d,%d: draw %u type %u zbp %u fbp %u uncond %u covers %d zpsm %u zmsk %u "
                    "test %llx frame psm %u fbw %u fbmsk %08x scissor %llx box %d,%d-%d,%d v %u,%u %u,%u %u,%u z %u\n",
                    px, py, i, d->type, d->zbp, d->fbp, d->unconditional, native_covers(d, reg, ctxt, px, py),
                    (unsigned)(zbuf >> 24) & 15, (unsigned)(zbuf >> 32) & 1, (unsigned long long)test,
                    (unsigned)(frame >> 24) & 63, (unsigned)(frame >> 16) & 63, (unsigned)(frame >> 32),
                    (unsigned long long)reg[GS_SCISSOR_1 + ctxt], d->box[0], d->box[1], d->box[2], d->box[3],
                    d->v[0].x, d->v[0].y, d->v[1].x, d->v[1].y, d->v[2].x, d->v[2].y, d->v[d->count - 1].z);
            shown++;
        }
    }
    if (n->trace_probes)
        fprintf(stderr, "native probe: field %llu replay %d,%d-%d,%d zbp %u uncovered %u from %u of %u (%u+%u)\n",
                (unsigned long long)n->field, rect[0], rect[1], rect[2], rect[3], zbp, left, start, total,
                n->logs[0].draw_count, n->logs[1].draw_count);
    memcpy(saved_reg, g->reg, sizeof saved_reg); memcpy(saved_clut, g->clut, sizeof saved_clut);
    uint64_t saved_version = g->state_version;
    unsigned restored = UINT_MAX;
    int restored_masked = -1;
    NativeLog *restored_log = NULL;
    native_clip(n, rect);
    for (unsigned i = start; i < total; i++) {
        NativeLog *log = i < n->logs[0].draw_count ? &n->logs[0] : &n->logs[1];
        NativeLogDraw *d = &log->draws[i < n->logs[0].draw_count ? i : i - n->logs[0].draw_count];
        if (!native_writes(d, zbp) || !native_overlap(d->box, rect)) continue;
        /* Out of order, so colour writes are masked unless they are the Z memory itself. */
        int masked = d->fbp != zbp;
        if (d->state != restored || log != restored_log || masked != restored_masked) {
            memcpy(g->reg, log->states[d->state].reg, sizeof g->reg);
            memcpy(g->clut, log->states[d->state].clut, sizeof g->clut);
            if (masked) g->reg[GS_FRAME_1] |= 0xFFFFFFFFull << 32, g->reg[GS_FRAME_2] |= 0xFFFFFFFFull << 32;
            g->state_version = (1ull << 63) | ++n->replay_version;
            restored = d->state; restored_log = log; restored_masked = masked;
        }
        draw(g, d->type, d->v, d->count);
        n->stats.probe_replayed_draws++;
    }
    memcpy(g->reg, saved_reg, sizeof saved_reg); memcpy(g->clut, saved_clut, sizeof saved_clut);
    g->state_version = saved_version;
    /* Back to the watch rectangle for in-order draws. */
    native_clip(n, NULL);
}

static void native_watch_cover(Native *n, const NativeLogDraw *d, const uint64_t *reg, unsigned ctxt)
{
    int x0 = d->box[0] > n->watch.rect[0] ? d->box[0] : n->watch.rect[0];
    int y0 = d->box[1] > n->watch.rect[1] ? d->box[1] : n->watch.rect[1];
    int x1 = d->box[2] < n->watch.rect[2] ? d->box[2] : n->watch.rect[2];
    int y1 = d->box[3] < n->watch.rect[3] ? d->box[3] : n->watch.rect[3];
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            uint8_t *c = &n->watch.covered[(y - n->watch.rect[1]) * NATIVE_WATCH + (x - n->watch.rect[0])];
            if (!*c && native_covers(d, reg, ctxt, x, y)) { *c = 1; n->watch.missing--; }
        }
    if (!n->watch.missing) n->watch.valid = 1;
}

static void native_exact(Native *n, Gs *g, unsigned type, const GsVertex *v, unsigned count, unsigned prim, unsigned ctxt)
{
    uint64_t zbuf = g->reg[GS_ZBUF_1 + ctxt], test = g->reg[GS_TEST_1 + ctxt], frame = g->reg[GS_FRAME_1 + ctxt];
    unsigned abe = (unsigned)((g->reg[GS_PRMODECONT] & 1 ? g->reg[GS_PRIM] : g->reg[GS_PRMODE]) >> 6 & 1);
    const NativeExactState *state = native_exact_state(&n->exact_state, zbuf, test, frame, abe);
    if (state->never) return;                                   /* ZTST NEVER: nothing is written */
    unsigned zbp = state->zbp, fbp = state->fbp, fbw = state->fbw;
    if (!n->exact_probe) {   /* the probe reads native depth: no exact draws (finding 097) */
        if (state->zwrite) n->zbw[zbp] = (uint8_t)fbw;
        return;
    }
    uint64_t offset = g->reg[GS_XYOFFSET_1 + ctxt], scissor = g->reg[GS_SCISSOR_1 + ctxt];
    unsigned zpsm = state->zpsm, fpsm = state->fpsm;
    int zwrite = state->zwrite, colour = state->colour;
    int z_full = state->z_full, colour_eligible = state->colour_full;
    if (zwrite) {
        n->zbase[zbp] = 1;
        /* The page row after the Z buffer's last scissored row. */
        unsigned rows = (unsigned)(((scissor >> 48 & 2047) + 32) / 32), key = zbp | fbw << 9 | rows << 15;
        if (!psm_wide(zpsm) && fbw && key != n->band_key) {
            for (unsigned i = 0; i < n->band_count; i++) n->band[n->band_pages[i]] = 0;
            n->band_count = 0; n->band_key = key;
            for (unsigned k = 0; k < fbw; k++) native_band_add(n, zbp + rows * fbw + k);
            n->stats.band_moves++;
        }
    }
    int alias = colour && n->zbase[fbp] && (!n->zbw[fbp] || n->zbw[fbp] == fbw);
    int band[4] = {INT_MAX, INT_MAX, INT_MIN, INT_MIN}, banded = 0, full = n->field <= n->full_until;
    if (full && n->band_count) {
        if (colour && !psm_wide(fpsm)) banded |= native_band_rect(n, fbp, fbw, (fpsm & 0x30) == 0x30, band);
        if (zwrite && !psm_wide(zpsm)) banded |= native_band_rect(n, zbp, fbw, 1, band);
    }
    int target = full && colour && n->exact_target[fbp];
    if (!zwrite && !alias && !banded && !target) return;
    int box[4] = {65536, 65536, -65536, -65536};
    for (unsigned i = 0; i < count; i++) {
        int x = (int)v[i].x - (int)(offset & 65535), y = (int)v[i].y - (int)(offset >> 32 & 65535);
        if (x < box[0]) box[0] = x;
        if (y < box[1]) box[1] = y;
        if (x > box[2]) box[2] = x;
        if (y > box[3]) box[3] = y;
    }
    box[0] = box[0] >> 4; box[1] = box[1] >> 4; box[2] = (box[2] + 15) >> 4; box[3] = (box[3] + 15) >> 4;
    if (box[0] < (int)(scissor & 2047)) box[0] = (int)(scissor & 2047);
    if (box[1] < (int)(scissor >> 32 & 2047)) box[1] = (int)(scissor >> 32 & 2047);
    if (box[2] > (int)(scissor >> 16 & 2047)) box[2] = (int)(scissor >> 16 & 2047);
    if (box[3] > (int)(scissor >> 48 & 2047)) box[3] = (int)(scissor >> 48 & 2047);
    if (box[0] > box[2] || box[1] > box[3]) return;
    banded = banded && native_overlap(box, band);
    if (banded) n->stats.band_draws++;
    if (target) n->stats.target_draws++;
    if (banded || target) {
        /* A render target the native renderer drew, sampled here: from now on it is drawn
           exactly too (the rider shadow texture under the band's sky, for one). */
        uint64_t tex0 = g->reg[GS_TEX0_1 + ctxt];
        int owner = prim >> 4 & 1 ? native_owner(n, g, (unsigned)(tex0 & 0x3FFF) / 32) : -1;
        if (owner >= 0 && !n->surfaces[owner].depth) {
            n->stats.band_rt_textures++;
            if (!n->exact_target[n->surfaces[owner].page]) {
                n->exact_target[n->surfaces[owner].page] = 1;
                n->stats.exact_targets++;
                if (n->trace_probes)
                    fprintf(stderr, "native exact target: field %llu page %u bw %u (sampled by fbp %u)\n",
                            (unsigned long long)n->field, n->surfaces[owner].page, n->surfaces[owner].bw, fbp);
            }
        }
    }
    int watched = 0;
    if (zwrite || alias) {
        /* Unconditional: the written value never depends on what memory held before. Z24
           writes leave the top byte, which nothing else changes in a Z buffer. */
        int colour_full = alias && colour_eligible;
        native_log(n, g, type, v, count, zwrite ? zbp : NATIVE_NO_PAGE, alias ? fbp : NATIVE_NO_PAGE,
                   zwrite ? z_full : colour_full, box);
        if (alias) n->stats.z_alias_draws++;
        if (zwrite) n->zbw[zbp] = (uint8_t)fbw;
        watched = n->watch.on && ((zwrite && zbp == n->watch.zbp) || (alias && fbp == n->watch.zbp)) &&
                  native_overlap(box, n->watch.rect);
        if (watched) {
            NativeLog *log = &n->logs[1];
            NativeLogDraw *d = &log->draws[log->draw_count - 1];
            if (!n->watch.valid && d->unconditional) native_watch_cover(n, d, log->states[d->state].reg, ctxt);
        }
    }
    /* Each pixel is drawn once: an exact target is drawn whole, overlapping clips merge into
       their bounding box. */
    if (target) {
        static const int whole[4] = {0, 0, 2047, 2047};
        native_clip(n, whole);
        draw(g, type, v, count);
        n->stats.exact_draws++;
    } else if (watched && banded && native_overlap(band, n->watch.rect)) {
        const int *w = n->watch.rect;
        int both[4] = {band[0] < w[0] ? band[0] : w[0], band[1] < w[1] ? band[1] : w[1],
                       band[2] > w[2] ? band[2] : w[2], band[3] > w[3] ? band[3] : w[3]};
        native_clip(n, both);
        draw(g, type, v, count);
        n->stats.exact_draws++;
    } else {
        if (watched) { native_clip(n, NULL); draw(g, type, v, count); n->stats.exact_draws++; }
        if (banded) { native_clip(n, band); draw(g, type, v, count); n->stats.exact_draws++; }
    }
    native_clip(n, NULL);
}

/* A Z probe as rectangles in draw coordinates. The read (width sbw) and the draws (width fbw)
   share GS pages, so each page-sized part of the read maps on its own; a read crossing the
   buffer's right edge continues on the next row of pages. Returns 0 when there are too many. */
static unsigned native_probe_pieces(const int rect[4], unsigned psm, unsigned sbw, unsigned fbw, int pieces[][4],
                                    unsigned max, int merge)
{
    int pw = 64, ph = (psm & 7) == 2 ? 64 : 32;
    unsigned count = 0;
    if (!sbw) sbw = 1;
    if (!fbw) fbw = sbw;
    for (int y = rect[1]; y <= rect[3]; y = (y / ph + 1) * ph)
        for (int x = rect[0]; x <= rect[2]; x = (x / pw + 1) * pw) {
            int x1 = (x / pw + 1) * pw - 1, y1 = (y / ph + 1) * ph - 1;
            unsigned page = (unsigned)(y / ph) * sbw + (unsigned)(x / pw);
            int dx = (int)(page % fbw) * pw - x / pw * pw, dy = (int)(page / fbw) * ph - y / ph * ph;
            if (count == max) return 0;
            int *piece = pieces[count++];
            piece[0] = x + dx; piece[1] = y + dy;
            piece[2] = (x1 < rect[2] ? x1 : rect[2]) + dx; piece[3] = (y1 < rect[3] ? y1 : rect[3]) + dy;
        }
    /* Pieces that tile their bounding box (the usual case, sbw == fbw) are one rectangle. */
    int box[4] = {INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    long area = 0;
    for (unsigned i = 0; i < count; i++) {
        const int *q = pieces[i];
        area += (long)(q[2] - q[0] + 1) * (q[3] - q[1] + 1);
        if (q[0] < box[0]) box[0] = q[0];
        if (q[1] < box[1]) box[1] = q[1];
        if (q[2] > box[2]) box[2] = q[2];
        if (q[3] > box[3]) box[3] = q[3];
    }
    if (merge && count > 1 && area == (long)(box[2] - box[0] + 1) * (box[3] - box[1] + 1)) {
        memcpy(pieces[0], box, sizeof box);
        count = 1;
    }
    return count;
}

/* The Z surface the scene draws at page zbp, bw pages wide in psm's layout, or -1 (other Z
   surfaces can start at the same page: a small one at 64 x 64 does). */
static int native_zsurface(const Native *n, unsigned zbp, unsigned bw, unsigned psm)
{
    unsigned count = (unsigned)n->surface_count;
    for (unsigned i = 0; i < count; i++) {
        const NativeSurface *s = &n->surfaces[i];
        if (s->depth && s->page == zbp && s->bw == bw && s->wide == (unsigned)psm_wide(psm)) return (int)i;
    }
    return -1;
}

/* A Z read (the lens flare's probe) from native depth (finding 097): asks the render thread for
   the read's neighbourhood, and answers each pixel the newest copy holds; the others keep the CPU
   image. The answered pixels' pages are staged: native_fetch_pages writes the values there. The
   depth is the Z surface's own format: the probe reads the Z24 buffer as PSMZ32, whose top byte
   (other data; Z24 draws leave it) stays the CPU image's. */
static void native_zread_stage(Native *n, unsigned psm, unsigned sbp, unsigned sbw, const int rect[4], uint32_t staged[16])
{
    enum { MARGIN = 16 };
    unsigned zbp = sbp / 32, rbw = sbw ? sbw : 1, fbw = n->zbw[zbp] ? n->zbw[zbp] : rbw;
    int pw = 64, ph = (psm & 7) == 2 ? 64 : 32, surface = native_zsurface(n, zbp, fbw, psm);
    unsigned zpsm = surface >= 0 && n->surfaces[surface].zpsm ? n->surfaces[surface].zpsm : psm;
    double unit = zpsm == GS_PSMZ32 ? 4294967296.0 : zpsm == GS_PSMZ24 ? 16777216.0 : 65536.0;
    uint32_t zmax = zpsm == GS_PSMZ32 ? UINT32_MAX : zpsm == GS_PSMZ24 ? 0xFFFFFF : 0xFFFF;
    uint32_t mask = zmax & (psm == GS_PSMZ32 ? UINT32_MAX : psm == GS_PSMZ24 ? 0xFFFFFF : 0xFFFF);
    int box[4] = {INT_MAX, INT_MAX, INT_MIN, INT_MIN}, answered = 0;
    memset(staged, 0, 16 * sizeof *staged);
    native_mutex_lock(&n->queue_lock);
    for (int y = rect[1]; y <= rect[3]; y++)
        for (int x = rect[0]; x <= rect[2]; x++) {
            /* The read's pixel in the Z buffer's own layout (fbw pages wide): the same GS page. */
            unsigned page = (unsigned)(y / ph) * rbw + (unsigned)(x / pw);
            int dx = (int)(page % fbw) * pw + x % pw, dy = (int)(page / fbw) * ph + y % ph;
            if (dx < box[0]) box[0] = dx;
            if (dy < box[1]) box[1] = dy;
            if (dx > box[2]) box[2] = dx;
            if (dy > box[3]) box[3] = dy;
            const uint32_t *r = n->zread.rect;
            if (!n->zread.valid || n->zread.zbp != zbp || dx < (int)r[0] || dy < (int)r[1] ||
                dx >= (int)(r[0] + r[2]) || dy >= (int)(r[1] + r[3]) || n->zread_pending_count == 1024) continue;
            float d;
            memcpy(&d, &n->zread.bits[(unsigned)(dy - (int)r[1]) * r[2] + (unsigned)(dx - (int)r[0])], 4);
            uint32_t value = d >= 1.0f ? zmax : d <= 0.0f ? 0 : (uint32_t)(d * unit);
            n->zread_pending[n->zread_pending_count].psm = psm;
            n->zread_pending[n->zread_pending_count].bp = sbp;
            n->zread_pending[n->zread_pending_count].bw = sbw;
            n->zread_pending[n->zread_pending_count].x = (unsigned)x;
            n->zread_pending[n->zread_pending_count].y = (unsigned)y;
            n->zread_pending[n->zread_pending_count].mask = mask;
            n->zread_pending[n->zread_pending_count++].value = value;
            unsigned at = gs_pixel_address(psm, sbp, sbw, (unsigned)x & 2047, (unsigned)y & 2047) / (8192 * 8);
            staged[at / 32 & 15] |= 1u << (at & 31);
            answered++;
        }
    n->zread_want.on = surface >= 0;
    n->zread_want.zbp = zbp;
    n->zread_want.surface = surface;
    n->zread_want.rect[0] = box[0] - MARGIN < 0 ? 0 : box[0] - MARGIN;
    n->zread_want.rect[1] = box[1] - MARGIN < 0 ? 0 : box[1] - MARGIN;
    n->zread_want.rect[2] = box[2] + MARGIN - n->zread_want.rect[0] + 1;
    n->zread_want.rect[3] = box[3] + MARGIN - n->zread_want.rect[1] + 1;
    native_mutex_unlock(&n->queue_lock);
    if (answered == (rect[2] - rect[0] + 1) * (rect[3] - rect[1] + 1)) n->stats.probe_hits++;
    else n->stats.probe_uncovered++;
    if (n->trace_probes)
        fprintf(stderr, "native zread: field %llu read psm %#x zbp %u sbw %u fbw %u %d,%d-%d,%d at %d,%d-%d,%d surface %d; "
                "copy %d zbp %u %u,%u %ux%u; answered %d first %#x\n", (unsigned long long)n->field, psm, zbp, sbw, fbw,
                rect[0], rect[1], rect[2], rect[3], box[0], box[1], box[2], box[3], surface, n->zread.valid, n->zread.zbp,
                n->zread.rect[0], n->zread.rect[1], n->zread.rect[2], n->zread.rect[3], answered,
                n->zread_pending_count ? n->zread_pending[0].value : 0);
}

/* gs_settle_reads: the staged pages from the CPU image with the answered Z pixels written over;
   with DHD_EXACT_PROBE the exact device's staged pages (=2: the native answers are compared with
   them, a "native zread: compare" line per read on stderr). */
static void native_fetch_pages(Gs *g, const uint32_t staged[16], uint32_t *image)
{
    Native *n = ((NativeExact *)g->backend)->native;
    if (n->exact_probe) {
        fetch_pages(g, staged, image);
        if (n->exact_probe == 2 && n->zread_pending_count) {
            unsigned exact_covered = 0, native_covered = 0, differ = 0;
            uint32_t worst = 0;
            for (unsigned i = 0; i < n->zread_pending_count; i++) {
                unsigned psm = n->zread_pending[i].psm;
                uint32_t at = gs_pixel_address(psm, n->zread_pending[i].bp, n->zread_pending[i].bw,
                                               n->zread_pending[i].x & 2047, n->zread_pending[i].y & 2047);
                uint32_t mask = n->zread_pending[i].mask;
                uint32_t exact = image[(at / 32) & (GS_VRAM_SIZE / 4 - 1)] >> (at & 31) & mask;
                uint32_t mine = n->zread_pending[i].value & mask, gap = exact > mine ? exact - mine : mine - exact;
                exact_covered += exact != 0;
                native_covered += mine != 0;
                differ += gap > (exact >> 6);  /* over 1/64 of the value apart */
                if (gap > worst) worst = gap;
            }
            fprintf(stderr, "native zread: compare field %llu pixels %u exact covered %u native covered %u differ %u worst %#x\n",
                    (unsigned long long)n->field, n->zread_pending_count, exact_covered, native_covered, differ, worst);
            if (exact_covered > native_covered + 7 || native_covered > exact_covered + 7) {
                fprintf(stderr, "native zread: at %u,%u exact/native", n->zread_pending[0].x, n->zread_pending[0].y);
                for (unsigned i = 0; i < n->zread_pending_count; i++) {
                    unsigned psm = n->zread_pending[i].psm;
                    uint32_t at = gs_pixel_address(psm, n->zread_pending[i].bp, n->zread_pending[i].bw,
                                                   n->zread_pending[i].x & 2047, n->zread_pending[i].y & 2047);
                    fprintf(stderr, " %x/%x", image[(at / 32) & (GS_VRAM_SIZE / 4 - 1)] >> (at & 31), n->zread_pending[i].value);
                }
                fputc('\n', stderr);
                /* The logged draws writing the first pixel's Z, newest first. */
                static int dumped;
                int px = (int)n->zread_pending[0].x, py = (int)n->zread_pending[0].y, shown = 0;
                unsigned zbp = n->zread_pending[0].bp / 32, total = n->logs[0].draw_count + n->logs[1].draw_count;
                for (unsigned i = total; dumped < 30 && i-- > 0 && shown < 12;) {
                    NativeLog *log = i < n->logs[0].draw_count ? &n->logs[0] : &n->logs[1];
                    NativeLogDraw *d = &log->draws[i < n->logs[0].draw_count ? i : i - n->logs[0].draw_count];
                    if (px < d->box[0] || px > d->box[2] || py < d->box[1] || py > d->box[3] || !native_writes(d, zbp)) continue;
                    const uint64_t *reg = log->states[d->state].reg;
                    unsigned ctxt = native_log_context(reg);
                    uint64_t zbuf = reg[GS_ZBUF_1 + ctxt], frame = reg[GS_FRAME_1 + ctxt], test = reg[GS_TEST_1 + ctxt];
                    fprintf(stderr, "native zread:   draw %u type %u zbp %u fbp %u covers %d zpsm %u zmsk %u test %llx fpsm %u "
                            "fbw %u fbmsk %08x box %d,%d-%d,%d z %u\n", i, d->type, d->zbp, d->fbp,
                            native_covers(d, reg, ctxt, px, py), (unsigned)(zbuf >> 24) & 15, (unsigned)(zbuf >> 32) & 1,
                            (unsigned long long)test, (unsigned)(frame >> 24) & 63, (unsigned)(frame >> 16) & 63,
                            (unsigned)(frame >> 32), d->box[0], d->box[1], d->box[2], d->box[3], d->v[d->count - 1].z);
                    shown++;
                }
                if (dumped < 30)
                    for (unsigned i = 0; i < (unsigned)n->surface_count; i++)
                        if (n->surfaces[i].depth && n->surfaces[i].page == zbp)
                            fprintf(stderr, "native zread:   z surface %u bw %u wide %u zpsm %u\n", i, n->surfaces[i].bw,
                                    n->surfaces[i].wide, n->surfaces[i].zpsm);
                dumped++;
            }
        }
        n->zread_pending_count = 0;
        return;
    }
    for (unsigned page = 0; page < 512; page++)
        if (staged[page / 32] & (1u << (page & 31)))
            memcpy((uint8_t *)image + page * 8192, (const uint8_t *)g->vram + page * 8192, 8192);
    for (unsigned i = 0; i < n->zread_pending_count; i++) {
        unsigned psm = n->zread_pending[i].psm;
        uint32_t at = gs_pixel_address(psm, n->zread_pending[i].bp, n->zread_pending[i].bw,
                                       n->zread_pending[i].x & 2047, n->zread_pending[i].y & 2047);
        uint32_t shift = at & 31, mask = n->zread_pending[i].mask;
        uint32_t *word = &image[(at / 32) & (GS_VRAM_SIZE / 4 - 1)];
        *word = (*word & ~(mask << shift)) | (n->zread_pending[i].value & mask) << shift;
    }
    n->zread_pending_count = 0;
}

/* Deferred reads: a Z probe gets exact pixels; colour reads keep the CPU image (counted). */
static void native_stage_pages(Gs *g, const uint32_t want[16], uint32_t staged[16])
{
    NativeExact *gpu = g->backend;
    Native *n = gpu->native;
    uint64_t blt = g->reg[GS_BITBLTBUF], pos = g->reg[GS_TRXPOS], size = g->reg[GS_TRXREG];
    unsigned psm = (unsigned)(blt >> 24) & 63, sbp = (unsigned)blt & 0x3FFF, sbw = (unsigned)(blt >> 16) & 63;
    int rect[4] = {(int)(pos & 2047), (int)(pos >> 16 & 2047), 0, 0};
    rect[2] = rect[0] + (int)(size & 4095) - 1; rect[3] = rect[1] + (int)(size >> 32 & 4095) - 1;
    if ((psm & 0x30) == 0x30 && sbp % 32 == 0 && n->exact_probe != 1) {
        uint32_t compare[16];
        native_zread_stage(n, psm, sbp, sbw, rect, n->exact_probe ? compare : staged);
        if (!n->exact_probe) return;
    }
    if ((psm & 0x30) == 0x30 && sbp % 32 == 0) {
        unsigned zbp = sbp / 32;
        if (n->band_count && (n->band_key & 511) == zbp && rect[3] + 32 >= (int)(n->band_key >> 15) * 32)
            n->full_until = n->field + n->full_hold;
        int pieces[16][4];
        unsigned count = native_probe_pieces(rect, psm, sbw, n->zbw[zbp], pieces, 16, !n->band_count);
        if (!count) n->stats.probe_uncovered++;
        for (unsigned i = 0; i < count; i++) {
            const int *q = pieces[i];
            unsigned fbw = n->zbw[zbp] ? n->zbw[zbp] : (sbw ? sbw : 1), in_band = 1;
            for (int y = q[1] / 32; y <= q[3] / 32; y++)
                for (int x = q[0] / 64; x <= q[2] / 64; x++)
                    in_band &= n->band[(zbp + (unsigned)y * fbw + (unsigned)x) & 511];
            if (q[3] % 32 >= NATIVE_BAND_ROWS || q[3] / 32 != q[1] / 32) in_band = 0;
            if (in_band) {
                n->stats.probe_band_hits++;
                if (n->trace_probes)
                    fprintf(stderr, "native probe: field %llu read %d,%d-%d,%d piece %u/%u %d,%d-%d,%d band\n",
                            (unsigned long long)n->field, rect[0], rect[1], rect[2], rect[3], i + 1, count,
                            q[0], q[1], q[2], q[3]);
                continue;
            }
            int inside = n->watch.on && n->watch.valid && n->watch.zbp == zbp && q[0] >= n->watch.rect[0] &&
                         q[1] >= n->watch.rect[1] && q[2] <= n->watch.rect[2] && q[3] <= n->watch.rect[3];
            if (inside) n->stats.probe_hits++;
            else { native_exact_replay(n, g, q, zbp); n->stats.probe_replays++; }
            if (n->trace_probes)
                fprintf(stderr, "native probe: field %llu read %d,%d-%d,%d sbw %u fbw %u piece %u/%u %d,%d-%d,%d %s\n",
                        (unsigned long long)n->field, rect[0], rect[1], rect[2], rect[3], sbw, n->zbw[zbp], i + 1, count,
                        q[0], q[1], q[2], q[3], inside ? "watched" : "replayed");
        }
        /* Keep the watch while the probe stays well inside it; otherwise move it to the probe. */
        const int *q = count ? pieces[0] : rect;
        int margin = 8;
        if (!n->watch.on || n->watch.zbp != zbp || q[0] < n->watch.rect[0] + margin || q[1] < n->watch.rect[1] + margin ||
            q[2] > n->watch.rect[2] - margin || q[3] > n->watch.rect[3] - margin) {
            int half = 32;
            n->watch.on = 1; n->watch.valid = 0; n->watch.zbp = zbp;
            n->watch.rect[0] = q[0] - half < 0 ? 0 : q[0] - half;
            n->watch.rect[1] = q[1] - half < 0 ? 0 : q[1] - half;
            n->watch.rect[2] = q[2] + half;
            n->watch.rect[3] = q[3] + half;
            if (n->watch.rect[2] - n->watch.rect[0] >= NATIVE_WATCH) n->watch.rect[2] = n->watch.rect[0] + NATIVE_WATCH - 1;
            if (n->watch.rect[3] - n->watch.rect[1] >= NATIVE_WATCH) n->watch.rect[3] = n->watch.rect[1] + NATIVE_WATCH - 1;
            memset(n->watch.covered, 0, sizeof n->watch.covered);
            n->watch.missing = (unsigned)((n->watch.rect[2] - n->watch.rect[0] + 1) * (n->watch.rect[3] - n->watch.rect[1] + 1));
            native_clip(n, NULL);
        }
    } else n->stats.color_reads++;
    stage_pages(g, want, staged);
}

/* ---- GS hooks ---- */

static void native_draw(Gs *g, unsigned type, const GsVertex *v, unsigned count)
{
    NativeExact *gpu = g->backend;
    Native *n = gpu->native;
    unsigned prim = (unsigned)((g->reg[GS_PRIM] & 7) |
                    ((g->reg[GS_PRMODECONT] & 1 ? g->reg[GS_PRIM] : g->reg[GS_PRMODE]) & 0x7F8));
    unsigned ctxt = prim >> 9 & 1;
    native_exact(n, g, type, v, count, prim, ctxt);
    if (gs_vu_shadow) return;   /* a shadow batch's packet: its mesh item draws it (native_mesh) */
    native_record(n, g, type, v, count, prim, ctxt);
    n->stats.draws++;
}

/* gs.c's strips (draw_strip, finding 146): the drawn triangles of list, as native_draw would draw
   each in turn. The first goes through native_draw (it may change the state). The state then
   holds for the rest (gs.c writes only vertex registers in a strip; native_exact without the
   exact probe only notes the Z buffer's width, the same for each, and with it runs for each
   triangle before its record, as in native_draw), so each later vertex is converted once and
   each triangle recorded as native_record records it: the same box, vertices, indices, counts
   and conversion cache. With a per-primitive diagnostic (the field trace, interpolation's
   segments) or a state that draws nothing, every triangle goes through native_draw. */
static void native_draw_strip(Gs *g, unsigned type, const GsVertex *list, unsigned count, const uint8_t *drawn)
{
    NativeExact *gpu = g->backend;
    Native *n = gpu->native;
    unsigned end = count - 2, i = 0;   /* triangle k is list[k..k+2], k < end */
    while (i < end && !drawn[i]) i++;
    if (i == end) return;
    if (gs_vu_shadow) {   /* a shadow batch's packet: the exact path only (native_draw) */
        for (; i < end; i++) if (drawn[i]) native_draw(g, type, &list[i], 3);
        return;
    }
    native_draw(g, type, &list[i], 3);
    unsigned prim = (unsigned)((g->reg[GS_PRIM] & 7) |
                    ((g->reg[GS_PRMODECONT] & 1 ? g->reg[GS_PRIM] : g->reg[GS_PRMODE]) & 0x7F8));
    unsigned ctxt = prim >> 9 & 1;
    if (n->interp || n->field_trace_on || n->skip || !n->current_valid ||
        n->state_version != g->state_version || n->state_ctxt != ctxt || n->state_prim != prim ||
        ((prim & 16) && n->written != g->pixels_written) || type != 4) {
        for (i++; i < end; i++) if (drawn[i]) native_draw(g, type, &list[i], 3);
        return;
    }
    NativeFrame *f = n->recording;
    uint64_t offset = native_offset(g, ctxt);
    int fst = prim >> 8 & 1, flat = !(prim >> 3 & 1);
    int left = n->box[0], top = n->box[1], right = n->box[2], bottom = n->box[3];
    native_reserve(f, 3 * (end - i));
    if (n->indexed) native_reserve_indices(f, 3 * (end - i));
    /* the window of converted vertices: c[k % 3] is list[k]'s, with its pixel */
    NativeVertex c[3];
    int cx[3], cy[3];
    for (unsigned k = i + 1; k < i + 3; k++) {
        float x = (float)((int)list[k].x - (int)(offset & 65535)) * 0.0625f + 0.5f;
        float y = (float)((int)list[k].y - (int)(offset >> 32 & 65535)) * 0.0625f + 0.5f;
        cx[k % 3] = (int)x, cy[k % 3] = (int)y;
        native_set(&c[k % 3], &list[k], &list[k], n, fst, x, y);
    }
    unsigned recorded = 0, last = UINT32_MAX;   /* last: the latest triangle recorded in this loop */
    for (unsigned k = i + 1; k < end; k++) {
        unsigned a = k % 3, b = (k + 1) % 3, d = (k + 2) % 3;
        float x = (float)((int)list[k + 2].x - (int)(offset & 65535)) * 0.0625f + 0.5f;
        float y = (float)((int)list[k + 2].y - (int)(offset >> 32 & 65535)) * 0.0625f + 0.5f;
        cx[d] = (int)x, cy[d] = (int)y;
        native_set(&c[d], &list[k + 2], &list[k + 2], n, fst, x, y);
        if (!drawn[k]) continue;
        if (n->exact_probe) native_exact(n, g, type, &list[k], 3, prim, ctxt);
        /* The box grows by each vertex's pixel +-1; after the triangle just before, its first two
           are in it already (the box only grows), so only the new one can move it. */
        const unsigned w[3] = {a, b, d};
        for (unsigned j = last == k - 1 ? 2 : 0; j < 3; j++) {
            if (cx[w[j]] - 1 < left) left = cx[w[j]] - 1;
            if (cy[w[j]] - 1 < top) top = cy[w[j]] - 1;
            if (cx[w[j]] + 1 > right) right = cx[w[j]] + 1;
            if (cy[w[j]] + 1 > bottom) bottom = cy[w[j]] + 1;
        }
        if (n->indexed && !flat && last == k - 1) {
            /* Its first two vertices are the bytes of the previous triangle's last two (the same
               converted c[], unchanged without flat colour): native_triangle_indexed would find
               them equal and reuse both indices, then store the third. */
            uint32_t *o = f->indices + f->index_count;
            o[0] = o[-2]; o[1] = o[-1]; o[2] = f->vertex_count;
            f->vertices[f->vertex_count++] = c[d];
            f->index_count += 3;
            last = k;
            recorded++;
            continue;
        }
        NativeVertex t[3] = {c[a], c[b], c[d]};
        if (flat) t[0].rgba = t[1].rgba = t[2].rgba;   /* flat: the last vertex's colour */
        last = k;
        if (n->indexed) native_triangle_indexed(f, type, t);
        else {
            memcpy(&f->vertices[f->vertex_count], t, sizeof t);
            f->vertex_count += 3;
        }
        recorded++;
    }
    /* native_record's conversion cache after the last triangle recorded: its second and third
       vertex (nothing in the loop reads it, so it is set once; c[] may have moved on since, and
       native_set gives the same bytes again) */
    if (last != UINT32_MAX) {
        for (unsigned j = 0; j < 2; j++) {
            const GsVertex *v = &list[last + 1 + j];
            float x = (float)((int)v->x - (int)(offset & 65535)) * 0.0625f + 0.5f;
            float y = (float)((int)v->y - (int)(offset >> 32 & 65535)) * 0.0625f + 0.5f;
            n->conv_gs[j] = *v;
            native_set(&n->conv[j], v, v, n, fst, x, y);
        }
    }
    n->box[0] = left; n->box[1] = top; n->box[2] = right; n->box[3] = bottom;
    if (g->source_path == 1) f->path1 += recorded;
    n->stats.primitives += recorded;
    n->stats.draws += recorded;
}

/* A strip vertex of the game's layout (gs_strip.h) as native_set converts the vertex those three
   qwords write (gs_strip_vertex) with its own colour, ST not FST; its pixel in *px, *py. */
static inline __attribute__((always_inline)) void native_strip_set(NativeVertex *o, const uint32_t *w, int ofx, int ofy,
                                                                   uint32_t zmax, float zscale, const float *fog,
                                                                   int *px, int *py)
{
    float x = (float)((int)(w[8] & 65535) - ofx) * 0.0625f + 0.5f;
    float y = (float)((int)(w[9] & 65535) - ofy) * 0.0625f + 0.5f;
    uint32_t z = w[10] >> 4 & 0xFFFFFF;
    if (z > zmax) z = zmax;
    *px = (int)x; *py = (int)y;
    o->x = x; o->y = y; o->z = (float)z * zscale;
    memcpy(&o->s, w, 12);   /* s, t, q */
    o->fog = fog[w[11] >> 4 & 255];
    o->rgba = (w[4] & 255) | (w[5] & 255) << 8 | (w[6] & 255) << 16 | (w[7] & 255) << 24;
}

/* native_strip's loop over triangles first..end-1 of the packet (triangle k is vertices k..k+2):
   each drawn one recorded as native_record records it. exact: the exact probe is on, and each
   takes native_exact first, as in native_draw. A constant in each of the two functions below, so
   the usual loop has no call in it, and a function of its own, so its values have the registers
   to themselves. Returns how many it recorded; *last is the latest, or UINT32_MAX. */
static inline __attribute__((always_inline)) unsigned native_strip_loop(
    Native *n, Gs *g, NativeFrame *f, const uint32_t *w, const GsVertex *before, unsigned first, unsigned end,
    unsigned prim, unsigned ctxt, int ofx, int ofy, const int exact, unsigned *last)
{
    const uint32_t zmax = n->zmax;
    const float zscale = n->zscale, *fog = n->vertex_fog;
    int left = n->box[0], top = n->box[1], right = n->box[2], bottom = n->box[3];
    NativeVertex *vertices = f->vertices;
    uint32_t *const indices = f->indices, *o = indices + f->index_count, vertex_count = f->vertex_count;
    unsigned recorded = 0, latest = UINT32_MAX;
    int follows = 0;   /* the triangle right before was recorded in this loop */
    int px, py;
#define NATIVE_STRIP_BOX() do { \
    if (px - 1 < left) left = px - 1; \
    if (py - 1 < top) top = py - 1; \
    if (px + 1 > right) right = px + 1; \
    if (py + 1 > bottom) bottom = py + 1; \
} while (0)
    for (unsigned k = first; k < end; k++) {
        const uint32_t *p = w + (k + 2) * GS_STRIP_WORDS;   /* the triangle's third vertex */
        if (gs_strip_adc(p)) { follows = 0; continue; }
        if (exact) {
            GsVertex tri[3];
            for (unsigned j = 0; j < 3; j++) gs_strip_vertex(p - (2 - j) * GS_STRIP_WORDS, before, &tri[j]);
            native_exact(n, g, 4, tri, 3, prim, ctxt);
        }
        if (follows) {
            /* Its first two vertices are the bytes of the previous triangle's last two: only the
               third is new, and only it can move the box (the box only grows). */
            native_strip_set(&vertices[vertex_count], p, ofx, ofy, zmax, zscale, fog, &px, &py);
            NATIVE_STRIP_BOX();
            o[0] = o[-2]; o[1] = o[-1]; o[2] = vertex_count++;
        } else {
            NativeVertex t[3];
            for (unsigned j = 0; j < 3; j++) {
                native_strip_set(&t[j], p - (2 - j) * GS_STRIP_WORDS, ofx, ofy, zmax, zscale, fog, &px, &py);
                NATIVE_STRIP_BOX();
            }
            /* native_triangle_indexed for a strip: each of the first two is drawn from the
               triangle before (whatever recorded it) when its bytes are the same */
            uint32_t a = UINT32_MAX, b = UINT32_MAX;
            if (o - indices >= 3) a = o[-2], b = o[-1];
            if (a != UINT32_MAX && native_same_vertex(&vertices[a], &t[0])) o[0] = a;
            else { vertices[vertex_count] = t[0]; o[0] = vertex_count++; }
            if (b != UINT32_MAX && native_same_vertex(&vertices[b], &t[1])) o[1] = b;
            else { vertices[vertex_count] = t[1]; o[1] = vertex_count++; }
            vertices[vertex_count] = t[2]; o[2] = vertex_count++;
        }
        o += 3;
        follows = 1; latest = k;
        recorded++;
    }
#undef NATIVE_STRIP_BOX
    f->vertex_count = vertex_count;
    f->index_count = (unsigned)(o - indices);
    n->box[0] = left; n->box[1] = top; n->box[2] = right; n->box[3] = bottom;
    *last = latest;
    return recorded;
}

static __attribute__((noinline)) unsigned native_strip_triangles(Native *n, NativeFrame *f, const uint32_t *w,
                                                                 unsigned first, unsigned end, int ofx, int ofy,
                                                                 unsigned *last)
{
    return native_strip_loop(n, NULL, f, w, NULL, first, end, 0, 0, ofx, ofy, 0, last);
}

static __attribute__((noinline)) unsigned native_strip_triangles_exact(
    Native *n, Gs *g, NativeFrame *f, const uint32_t *w, const GsVertex *before, unsigned first, unsigned end,
    unsigned prim, unsigned ctxt, int ofx, int ofy, unsigned *last)
{
    return native_strip_loop(n, g, f, w, before, first, end, prim, ctxt, ofx, ofy, 1, last);
}

/* gs.c's strips in the game's own layout (gs_strip_hook, finding 197): the drawn triangles of the
   packet's vertices, as native_draw would draw each in turn, decoded and recorded in one pass.
   The first goes through native_draw (it may change the state); the state then holds for the rest
   (native_draw_strip says why). A triangle right after a recorded one adds its third vertex,
   converted where the frame keeps it, and takes the other two indices from that triangle; one
   after a skipped triangle converts all three and goes through native_triangle_indexed's steps.
   A vertex no drawn triangle has is never converted. Box, vertices, indices, counts and the
   conversion cache end as native_record leaves them. What the game's strips never are (a field
   that is not indexed, flat shading, FST), a per-primitive diagnostic and a state that draws
   nothing go through native_draw, triangle by triangle. */
static void native_strip(Gs *g, const uint32_t *w, unsigned count)
{
    NativeExact *gpu = g->backend;
    Native *n = gpu->native;
    unsigned end = count - 2, i = 0;   /* triangle k is vertices k..k+2, k < end */
    while (i < end && gs_strip_adc(w + (i + 2) * GS_STRIP_WORDS)) i++;
    if (i == end) return;
    const GsVertex before = g->vertex;   /* its u and v: the strip writes neither */
    GsVertex tri[3];
#define NATIVE_STRIP_DRAW_REST(k) do { \
    for (; (k) < end; (k)++) \
        if (!gs_strip_adc(w + ((k) + 2) * GS_STRIP_WORDS)) { \
            for (unsigned j_ = 0; j_ < 3; j_++) gs_strip_vertex(w + ((k) + j_) * GS_STRIP_WORDS, &before, &tri[j_]); \
            native_draw(g, 4, tri, 3); \
        } \
} while (0)
    if (gs_vu_shadow) {   /* a shadow batch's packet: the exact path only (native_draw) */
        NATIVE_STRIP_DRAW_REST(i);
        return;
    }
    for (unsigned j = 0; j < 3; j++) gs_strip_vertex(w + (i + j) * GS_STRIP_WORDS, &before, &tri[j]);
    native_draw(g, 4, tri, 3);
    unsigned prim = (unsigned)((g->reg[GS_PRIM] & 7) |
                    ((g->reg[GS_PRMODECONT] & 1 ? g->reg[GS_PRIM] : g->reg[GS_PRMODE]) & 0x7F8));
    unsigned ctxt = prim >> 9 & 1;
    i++;
    if (n->interp || n->field_trace_on || n->skip || !n->current_valid || !n->indexed ||
        n->state_version != g->state_version || n->state_ctxt != ctxt || n->state_prim != prim ||
        ((prim & 16) && n->written != g->pixels_written) || (prim >> 8 & 1) || !(prim >> 3 & 1)) {
        NATIVE_STRIP_DRAW_REST(i);
        return;
    }
#undef NATIVE_STRIP_DRAW_REST
    NativeFrame *f = n->recording;
    uint64_t offset = native_offset(g, ctxt);
    const int ofx = (int)(offset & 65535), ofy = (int)(offset >> 32 & 65535);
    native_reserve(f, 3 * (end - i));
    native_reserve_indices(f, 3 * (end - i));
    unsigned last, recorded;
    if (n->exact_probe) recorded = native_strip_triangles_exact(n, g, f, w, &before, i, end, prim, ctxt, ofx, ofy, &last);
    else recorded = native_strip_triangles(n, f, w, i, end, ofx, ofy, &last);
    /* native_record's conversion cache after the last triangle recorded: its second and third
       vertex, each with its own colour */
    if (last != UINT32_MAX) {
        for (unsigned j = 0; j < 2; j++) {
            const uint32_t *p = w + (last + 1 + j) * GS_STRIP_WORDS;
            int px, py;
            gs_strip_vertex(p, &before, &n->conv_gs[j]);
            native_strip_set(&n->conv[j], p, ofx, ofy, n->zmax, n->zscale, n->vertex_fog, &px, &py);
        }
    }
    if (g->source_path == 1) f->path1 += recorded;
    n->stats.primitives += recorded;
    n->stats.draws += recorded;
}

/* A translated VU1 mesh batch (gs_mesh.h; gs_mesh made the register writes its packet would make):
   draws of their own (flag NATIVE_MESH) whose vertices are the batch's object-space ones and whose
   indices are the triangles its strip draws, for native.wgsl's native_mesh_vertex. The object's
   own values (matrix, guard band, colour scale, fog) are a record in the frame's vertices, which the
   shader reads from the vertex buffer (bound again as storage, group 2), and each vertex names its
   record; so consecutive batches under one draw state join one command whatever object they belong
   to (finding 168). Where its pixels land is not known here, so the draw claims its whole scissor.
   Nothing of it reaches the exact path: with DHD_EXACT_PROBE the batch runs on the VU too (shadow)
   and its packet goes there. */
_Static_assert(sizeof(GsMeshVertex) == sizeof(NativeVertex), "a mesh vertex is stored as a NativeVertex");
enum { NATIVE_MESH_RECORD = 4 };   /* vertices a record fills: 128 bytes, eight vec4s for the shader */

static void native_mesh_constants(NativeConstants *k, const GsMeshHeader *h, const float offset[2], const Native *n)
{
    k->flags |= NATIVE_MESH | (h->flags & GS_MESH_TWO_SIDED ? NATIVE_MESH_TWO_SIDED : 0);
    k->mesh_offset[0] = offset[0]; k->mesh_offset[1] = offset[1];
    k->mesh_offset[2] = (float)n->zmax; k->mesh_offset[3] = n->zscale;
}

/* The batch's object record in the frame's vertices (the matrix's columns, the guard band, the colour
   scale, then fog A, B and max; native_mesh_vertex reads them as eight vec4s): the frame's last one
   again when it holds the same values (the object's other batches), else a new one. Its first
   vertex's index, which the batch's vertices carry. */
static unsigned native_mesh_record(Native *n, NativeFrame *f, const GsMeshHeader *h)
{
    float r[NATIVE_MESH_RECORD * 8] = {0};
    memcpy(r, h->matrix, sizeof h->matrix);
    memcpy(r + 16, h->clip, sizeof h->clip);
    memcpy(r + 20, h->color, sizeof h->color);
    memcpy(r + 24, h->fog, sizeof h->fog);
    if (n->mesh_record_frame == f && n->mesh_record_at + NATIVE_MESH_RECORD <= f->vertex_count &&
        !memcmp(f->vertices + n->mesh_record_at, r, sizeof r))
        return n->mesh_record_at;
    native_reserve(f, NATIVE_MESH_RECORD);
    memcpy(f->vertices + f->vertex_count, r, sizeof r);
    n->mesh_record_frame = f;
    n->mesh_record_at = f->vertex_count;
    f->vertex_count += NATIVE_MESH_RECORD;
    return n->mesh_record_at;
}

static void native_mesh(Gs *g, const uint32_t *words, size_t count)
{
    NativeExact *gpu = g->backend;
    Native *n = gpu->native;
    const GsMeshHeader *h = (const GsMeshHeader *)words;
    if (!n->indexed || h->vertices > GS_MESH_MAX_VERTICES ||
        count * 4 < sizeof *h + h->vertices * sizeof(GsMeshVertex) + h->indices * 2) return;
    unsigned prim = (unsigned)((g->reg[GS_PRIM] & 7) |
                    ((g->reg[GS_PRMODECONT] & 1 ? g->reg[GS_PRIM] : g->reg[GS_PRMODE]) & 0x7F8));
    unsigned ctxt = prim >> 9 & 1;
    /* native_exact's bookkeeping of the Z buffer's width (and the band's place), with no vertices:
       nothing of a mesh is drawn or logged exactly */
    if (!(h->flags & GS_MESH_SHADOW)) native_exact(n, g, 4, NULL, 0, prim, ctxt);
    uint64_t xy = native_offset(g, ctxt);
    const float offset[2] = {(float)(xy & 65535), (float)(xy >> 32 & 65535)};
    const NativeConstants *k = &n->current.constants;
    int same = n->current_valid && n->mesh_open && n->state_version == g->state_version && n->state_ctxt == ctxt &&
               n->state_prim == prim && !((prim & 16) && n->written != g->pixels_written) &&
               k->mesh_offset[0] == offset[0] && k->mesh_offset[1] == offset[1] &&
               !(k->flags & NATIVE_MESH_TWO_SIDED) == !(h->flags & GS_MESH_TWO_SIDED);
    if (!same) {
        native_close(n, g);
        native_state(n, g, prim, ctxt);
        n->state_version = g->state_version; n->state_ctxt = ctxt; n->state_prim = prim;
        n->written = g->pixels_written;
        n->mesh_open = 1;
        if (n->skip) return;
        native_mesh_constants(&n->current.constants, h, offset, n);
        if (n->passes) native_mesh_constants(&n->second.constants, h, offset, n);
        native_open(n);
    }
    if (n->skip || !h->indices) return;
    NativeFrame *f = n->recording;
    n->box[0] = n->box[1] = -4096; n->box[2] = n->box[3] = 4096;   /* native_close clips it to the scissor */
    const float record = (float)native_mesh_record(n, f, h);   /* exact: vertex counts stay below 2^24 */
    native_reserve(f, h->vertices);
    native_reserve_indices(f, h->indices);
    const GsMeshVertex *v = (const GsMeshVertex *)(h + 1);
    const uint16_t *index = (const uint16_t *)(v + h->vertices);
    uint32_t base = f->vertex_count, *o = f->indices + f->index_count;
    memcpy(f->vertices + base, v, h->vertices * sizeof *v);
    for (unsigned i = 0; i < h->vertices; i++) f->vertices[base + i].fog = record;   /* GsMeshVertex.unused */
    f->vertex_count += h->vertices;
    for (unsigned i = 0; i < h->indices; i++) o[i] = base + index[i];
    f->index_count += h->indices;
    f->path1 += h->indices / 3;
    n->stats.primitives += h->indices / 3;
    n->stats.draws++;
}

/* A host upload must not become the starting content of an earlier pending draw. */
static void native_flush_draw(Gs *g)
{
    NativeExact *gpu = g->backend;
    native_close(gpu->native, g);
}

/* TRXDIR 2. Between rendered surfaces it is a GPU copy; otherwise the CPU copies GS memory
   (as gs.c would), so later texture decodes see it. */
static int native_local_copy(Gs *g)
{
    NativeExact *gpu = g->backend;
    Native *n = gpu->native;
    n->upload_epoch++;
    /* Claim the pending source draw before deciding where the copy reads. */
    native_close(n, g);
    uint64_t b = g->reg[GS_BITBLTBUF], p = g->reg[GS_TRXPOS], r = g->reg[GS_TRXREG];
    unsigned width = (unsigned)r & 4095, height = (unsigned)(r >> 32) & 4095;
    unsigned spsm = (unsigned)(b >> 24) & 63, sbp = (unsigned)b & 0x3FFF, sbw = (unsigned)(b >> 16) & 63;
    unsigned dpsm = (unsigned)(b >> 56) & 63, dbp = (unsigned)(b >> 32) & 0x3FFF, dbw = (unsigned)(b >> 48) & 63;
    unsigned sx = (unsigned)p & 2047, sy = (unsigned)(p >> 16) & 2047, dx = (unsigned)(p >> 32) & 2047, dy = (unsigned)(p >> 48) & 2047;
    if (!width || !height) return 1;
    int source = native_owner(n, g, sbp / 32);
    if (source >= 0 && !n->surfaces[source].depth && psm_color(spsm) && spsm == dpsm && sbp % 32 == 0 && dbp % 32 == 0 &&
        n->surfaces[source].bw == (sbw ? sbw : 1) && !(p >> 59 & 3)) {
        int ox, oy;
        if (native_page_origin(&n->surfaces[source], sbp / 32, &ox, &oy)) {
            int target = native_surface(n, dbp / 32, dbw, dpsm, 0);
            if (target >= 0) {
                native_claim(n, g, target, dpsm, (int)dx, (int)dy, (int)(dx + width - 1), (int)(dy + height - 1));
                NativeCommand *c = native_command(n);
                c->kind = NATIVE_COPY; c->source = (int16_t)source; c->target = (int16_t)target;
                c->sx = ox + (int)sx; c->sy = oy + (int)sy; c->x = (int)dx; c->y = (int)dy; c->w = (int)width; c->h = (int)height;
                n->stats.copies++;
                if (n->usage) n->usage[source].copied_out++, n->usage[target].copied_in++;
                return 1;
            }
        }
    }
    n->stats.copy_fallbacks++;
    sync_range(g, sbp * 256, 4 * 1024 * 1024);
    for (unsigned y = 0; y < height; y++) for (unsigned x = 0; x < width; x++) {
        unsigned ex = p >> 59 & 1 ? width - 1 - x : x, ey = p >> 60 & 1 ? height - 1 - y : y;
        uint32_t v = gs_read_pixel(g, spsm, sbp, sbw, (sx + ex) & 2047, (sy + ey) & 2047);
        gs_write_pixel(g, dpsm, dbp, dbw, (dx + ex) & 2047, (dy + ey) & 2047, v);
    }
    return 1;
}

/* ---- shared render-thread helpers ---- */

/* Surfaces at least this many pages wide are upscaled (the 512- and 640-pixel frame and Z
   buffers); narrower ones stay at 1x: the 64-pixel shadow, its blur and the copies between them,
   which sample each other by whole texels and would blur less at a finer grid (PCSX2 draws such
   effect targets at native resolution too). A Z buffer takes its frame's width, so a target and
   its depth always share a scale. */
enum { NATIVE_SCALED_BW = 4 };

/* The size a surface's texture needs, in GS pixels: what the GS worker has claimed, in 64-pixel
   steps, never less than the texture already has. */
static void native_wanted(const NativeSurface *s, unsigned want[2])
{
    for (unsigned i = 0; i < 2; i++) {
        unsigned e = (unsigned)s->extent[i];
        if (s->texture && s->size[i] / s->scale > e) e = s->size[i] / s->scale;
        want[i] = e < 64 ? 64 : e > 2048 ? 2048 : (e + 63) & ~63u;
    }
}

void pacing_handed(double *due, double *began);   /* port/hle/kernel.c, just-in-time frames */
void pacing_present(double due, double began);

/* Full-screen 2D cards keep their 4:3 shape after eight consecutive flat
   fields. The EE supplies a state-specific limit with each queued field: the
   goggles transition has more geometry than the later tiled card. Race, results
   and countdown views never use a primitive-count heuristic. */
#define NATIVE_FLAT_FIELDS 8
double host_widescreen_aspect(void);
int host_widescreen_flat_limit(void);  /* port/hle/widescreen.c */
int host_widescreen_card_drawn(void);  /* port/hle/host.c: the field's 2D was drawn for a card */

/* Widescreen shows a full-screen 2D picture (a card) at 4:3 and a movie at its own shape (finding
   140): the sides of the window are filled with the picture blurred and darkened (present.rs). */
static int native_wide_43(const Native *n)
{
    return n->wide && (n->card_shown || n->movie_rows);
}

/* Whether the game should draw its next field's 2D for a card: unmapped, as at 4:3 (widescreen.c's
   card mode, asked through host_frame_video each vsync). The render thread's answer after its last
   picture: NATIVE_FLAT_FIELDS fields of 2D only in a row. */
static volatile int native_card_now;
int gs_native_card(void) { return native_card_now; }

/* The picture's size in a window of w x h, keeping its shape: DHD_ASPECT's; 4:3 for a card in
   widescreen and for a movie when the shape is wider (a movie is a 4:3 picture, whatever shape the
   game has). */
static void native_viewport(const Native *n, unsigned w, unsigned h, float *vw, float *vh)
{
    float aspect = n->aspect < 0 ? (float)n->display_width / n->display_height : n->aspect;
    if (native_wide_43(n) && (aspect > 4.0f / 3 || aspect == 0)) aspect = 4.0f / 3;
    if (n->movie_rows && aspect > 4.0f / 3) aspect = 4.0f / 3;
    *vw = (float)w, *vh = (float)h;
    if (aspect > 0) {
        if (*vw > *vh * aspect) *vw = floorf(*vh * aspect + 0.5f);
        else *vh = floorf(*vw / aspect + 0.5f);
    }
}

/* Every surface's texture at the size the field's commands need: what each surface claimed; a
   target and the Z buffer drawn with it alike, as both APIs bind only views of equal size together. */
static void native_prepare_surfaces(Native *n, const NativeFrame *f)
{
    unsigned surfaces = (unsigned)n->surface_count;
    unsigned want[NATIVE_SURFACES][2];
    for (unsigned i = 0; i < surfaces; i++) native_wanted(&n->surfaces[i], want[i]);
    for (int changed = 1, pass = 0; changed && pass < 4; pass++) {
        changed = 0;
        for (unsigned i = 0; i < f->command_count; i++) {
            const NativeCommand *k = &f->commands[i];
            if (k->kind != NATIVE_DRAW || k->depth < 0 || !(k->depth_key & 8)) continue;
            unsigned *a = want[k->target], *b = want[k->depth];
            for (unsigned j = 0; j < 2; j++)
                if (a[j] != b[j]) { a[j] = b[j] = a[j] > b[j] ? a[j] : b[j]; changed = 1; }
        }
    }
    for (unsigned i = 0; i < surfaces; i++) native_surface_resources(n, &n->surfaces[i], want[i]);
}

/* After a picture's scanout: whether it is a field of 2D only. After NATIVE_FLAT_FIELDS of them in a
   row the game draws its 2D for a card (native_card_now, widescreen.c), and a field of 2D only drawn
   that way is a card: shown 4:3 with its sides filled, its text in its shape. So the picture and its
   text switch together, at the first field the game drew for the card, and back at the first field
   with 3D. The browser's core can't ask (its host_frame_video is a stub; its text stays mapped):
   there a card is shown 4:3 after NATIVE_FLAT_FIELDS, as before. Each switch is logged with the
   field's VU1 primitives and the limit of its menu state. */
static void native_note_flat(Native *n, const NativeFrame *f)
{
#ifdef DHD_IOS_RENDER
    /* A field with no new PATH1 packets can still show the previous 3D race picture. */
    int flat = f->flat_limit > 0 && f->path1 < f->flat_limit;
#else
    int flat = !f->path1 || (n->wide && f->path1 < f->flat_limit);
#endif
    n->flat_fields = flat ? n->flat_fields + 1 : 0;
    native_card_now = n->wide && n->flat_fields >= NATIVE_FLAT_FIELDS;
#ifdef NATIVE_NO_CARD_MODE
    int card = native_card_now;
#else
    int card = n->wide && flat && f->card_2d;
#endif
    if (card != n->card_shown)
        fprintf(stderr, "gs native: field %llu: %s (VU1 primitives %u, limit %u)\n", (unsigned long long)f->field,
                card ? "a card, shown 4:3 and filled" : "the full width again", f->path1, f->flat_limit);
    n->card_shown = card;
}

/* After a present: the counts and the time between presents. */
static void native_note_presented(Native *n)
{
    n->stats.presented++;
    double now = host_wall_seconds();
    if (n->last_present) n->intervals[n->interval_count++ % NATIVE_INTERVALS] = (float)((now - n->last_present) * 1000);
    else n->first_present = now;
    n->last_present = now;
}

/* The picture's position in a window of w x h: the viewport (x, y, width, height) that keeps its
   shape, and DHD_VIEW's quarter (present.h gs_native_view): one quarter of the 3-4 player split
   screen (presets 6-9) fills the picture: the whole is drawn twice as large, shifted and cut to
   the picture; every view is still drawn and the game is unchanged (finding 041). cut: the
   picture's rectangle in the window, [left, right) x [top, bottom).
   In widescreen a movie shows only its rows (the 640 x 368 movie in the middle of the 640 x 448
   picture, finding 055), at their own shape (about 1.62:1), as large as the window takes. Returns
   1 when the window's sides around cut are to be filled (present.rs): widescreen's 4:3 pictures. */
static int native_present_rect(const Native *n, unsigned w, unsigned h, float viewport[4], NativeRect *cut)
{
    float vw, vh;
    int view = gs_native_view - 1, quarter = view >= 0 && view < 4;
    native_viewport(n, w, h, &vw, &vh);
    float top, height;   /* the cut's rows: the picture's, or the movie's */
    if (n->wide && n->movie_rows && n->movie_rows < n->display_height && !quarter) {
        float part = (float)n->movie_rows / n->display_height, shape = 4.0f / 3 / part;  /* the movie's own */
        float mw = (float)w, mh = (float)h;
        if (mw > mh * shape) mw = floorf(mh * shape + 0.5f);
        else mh = floorf(mw / shape + 0.5f);
        vw = mw, vh = mh / part;
        top = floorf((h - mh) / 2), height = mh;
    } else {
        top = floorf((h - vh) / 2), height = vh;
    }
    viewport[0] = floorf((w - vw) / 2); viewport[1] = floorf((h - vh) / 2); viewport[2] = vw; viewport[3] = vh;
    cut->left = (int)viewport[0]; cut->top = (int)top;
    cut->right = (int)(viewport[0] + vw); cut->bottom = (int)(top + height);
    if (quarter) {
        viewport[0] -= (view & 1) * vw, viewport[1] -= (view >> 1) * vh;
        viewport[2] *= 2, viewport[3] *= 2;
    }
    return native_wide_43(n) && !quarter;
}

static void native_recycle(NativeFrame *f)
{
    for (unsigned i = 0; i < f->command_count; i++) free(f->commands[i].pixels);
    f->command_count = 0; f->vertex_count = 0; f->index_count = 0; f->segment_count = 0; f->convert_count = 0;
    f->path1 = 0;
}

/* DHD_INTERP: pairs each segment of the newer field with an unused segment of the older field with
   the same draw state and vertex count: of those in the next 256, the one whose first and last
   vertices are nearest. Taking simply the next one paired a tree or a ground piece with its
   neighbour whenever the game culled one or sorted them differently, and moved it halfway there
   in the in-between pictures. */
static void native_pair(Native *n, const NativeFrame *a, NativeFrame *b)
{
    if (b->pair_capacity < b->segment_count) {
        int32_t *grown = realloc(b->pair, b->segment_count * sizeof *grown);
        if (!grown) native_oom("pairs");
        b->pair = grown; b->pair_capacity = b->segment_count;
    }
    if (n->paired_capacity < a->segment_count) {
        uint8_t *grown = realloc(n->paired, a->segment_count);
        if (!grown) native_oom("pairs");
        n->paired = grown; n->paired_capacity = a->segment_count;
    }
    memset(n->paired, 0, a->segment_count);
    unsigned cursor = 0;
    for (unsigned j = 0; j < b->segment_count; j++) {
        const NativeSegment *sb = &b->segments[j];
        b->pair[j] = -1;
        if (!sb->count) continue;
        /* Look a little way ahead only: segments mostly keep their order from field to field. */
        const NativeVertex *fb = b->vertices + sb->first, *lb = fb + sb->count - 1;
        float best_distance = 1e30f;
        int best = -1;
        for (unsigned step = 0; step < a->segment_count && step < 256; step++) {
            unsigned i = (cursor + step) % a->segment_count;
            const NativeSegment *sa = &a->segments[i];
            if (n->paired[i] || sa->count != sb->count || sa->key != sb->key) continue;
            const NativeVertex *fa = a->vertices + sa->first, *la = fa + sa->count - 1;
            float d = fabsf(fa->x - fb->x) + fabsf(fa->y - fb->y) + fabsf(la->x - lb->x) + fabsf(la->y - lb->y);
            if (d < best_distance) best_distance = d, best = (int)i;
            if (d < 0.5f) break;
        }
        if (best >= 0) {
            n->paired[best] = 1; b->pair[j] = best;
            while (cursor < a->segment_count && n->paired[cursor]) cursor++;  /* the first unused */
        }
    }
    static int trace = -1; static unsigned calls;
    if (trace < 0) trace = getenv("DHD_INTERP_TRACE") != NULL;
    if (trace && calls++ % 120 == 0) {
        unsigned pairs = 0;
        for (unsigned j = 0; j < b->segment_count; j++) pairs += b->pair[j] >= 0;
        fprintf(stderr, "interp pair: field %llu segments %u/%u commands %u/%u vertices %u/%u pairs %u\n",
                (unsigned long long)b->field, a->segment_count, b->segment_count, a->command_count, b->command_count,
                a->vertex_count, b->vertex_count, pairs);
        for (unsigned j = 0; j < 6 && j < a->segment_count && j < b->segment_count; j++)
            fprintf(stderr, "  seg %u: %u+%u %08X | %u+%u %08X\n", j, a->segments[j].first, a->segments[j].count,
                    a->segments[j].key, b->segments[j].first, b->segments[j].count, b->segments[j].key);
    }
}

static uint32_t native_positions_hash(const NativeVertex *v, unsigned count)
{
    uint32_t h = 2166136261u ^ count;
    for (unsigned i = 0; i < count; i++) {
        uint32_t w[3];
        memcpy(w, &v[i].x, sizeof w);
        h = (h ^ w[0]) * 16777619u; h = (h ^ w[1]) * 16777619u; h = (h ^ w[2]) * 16777619u;
    }
    return h;
}

static int native_same_positions(const NativeVertex *a, const NativeVertex *b, unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return 0;
    return 1;
}

/* The newer field's vertices moved back towards the older field's: at t = 0 the older field,
   at t = 1 the newer. A segment drawn again over the same vertices (the ground's detail and
   lighting passes, a tree's layers, depth tested equal) moves exactly as the first one did,
   paired or not; else the passes fought in the in-between pictures. An unpaired segment moves
   as the last paired one of its draw state did on average (mostly the camera's motion), so a
   piece the game clipped differently this field stays with its neighbours. */
static const NativeVertex *native_mixed(Native *n, const NativeFrame *a, const NativeFrame *b, float t)
{
    if (n->mixed_capacity < b->vertex_count) {
        NativeVertex *grown = realloc(n->mixed, b->vertex_count * sizeof *grown);
        if (!grown) native_oom("interpolated vertices");
        n->mixed = grown; n->mixed_capacity = b->vertex_count;
    }
    memcpy(n->mixed, b->vertices, b->vertex_count * sizeof *n->mixed);
    unsigned size = 1;
    while (size < 2 * b->segment_count) size *= 2;
    if (n->same_capacity < size) {
        int32_t *grown = realloc(n->same, size * sizeof *grown);
        if (!grown) native_oom("interpolated segments");
        n->same = grown; n->same_capacity = size;
    }
    memset(n->same, 0xFF, size * sizeof *n->same);
    static struct { uint32_t key; int set; float x, y, z; } moves[256];  /* by draw state; render thread only */
    memset(moves, 0, sizeof moves);
    float back = 1 - t;
    uint64_t paired = 0, jumped = 0;
    for (unsigned j = 0; j < b->segment_count; j++) {
        const NativeSegment *sb = &b->segments[j];
        const NativeVertex *vb = b->vertices + sb->first;
        NativeVertex *o = n->mixed + sb->first;
        if (!sb->count) continue;
        unsigned slot = native_positions_hash(vb, sb->count) & (size - 1);
        int same = -1;
        for (; n->same[slot] >= 0; slot = (slot + 1) & (size - 1)) {
            const NativeSegment *s = &b->segments[n->same[slot]];
            if (s->count == sb->count && native_same_positions(b->vertices + s->first, vb, sb->count)) {
                same = n->same[slot];
                break;
            }
        }
        if (same >= 0) {
            const NativeVertex *m = n->mixed + b->segments[same].first;
            for (unsigned v = 0; v < sb->count; v++) o[v].x = m[v].x, o[v].y = m[v].y, o[v].z = m[v].z;
            continue;
        }
        n->same[slot] = (int32_t)j;
        unsigned move = (sb->key * 2654435761u) >> 24;
        if (b->pair[j] < 0) {
            if (!moves[move].set || moves[move].key != sb->key) continue;  /* held */
            for (unsigned v = 0; v < sb->count; v++) {
                o[v].x = vb[v].x + moves[move].x * back;
                o[v].y = vb[v].y + moves[move].y * back;
                o[v].z = vb[v].z + moves[move].z * back;
            }
            continue;
        }
        const NativeSegment *sa = &a->segments[b->pair[j]];
        const NativeVertex *va = a->vertices + sa->first;
        /* A large jump is a different object that happens to share the state: hold it. */
        float jump = 0, mx = 0, my = 0, mz = 0;
        for (unsigned v = 0; v < sb->count; v++) {
            float dx = fabsf(vb[v].x - va[v].x), dy = fabsf(vb[v].y - va[v].y);
            if (dx > jump) jump = dx;
            if (dy > jump) jump = dy;
        }
        if (jump > 96) { jumped += sb->count; continue; }
        for (unsigned v = 0; v < sb->count; v++) {
            float dx = va[v].x - vb[v].x, dy = va[v].y - vb[v].y, dz = va[v].z - vb[v].z;
            o[v].x = vb[v].x + dx * back;
            o[v].y = vb[v].y + dy * back;
            o[v].z = vb[v].z + dz * back;
            mx += dx, my += dy, mz += dz;
        }
        moves[move].key = sb->key, moves[move].set = 1;
        moves[move].x = mx / sb->count, moves[move].y = my / sb->count, moves[move].z = mz / sb->count;
        paired += sb->count;
    }
    n->stats.paired_vertices += paired; n->stats.jumped_vertices += jumped;
    n->stats.held_vertices += b->vertex_count - paired - jumped;
    return n->mixed;
}

static void native_sleep_until(double when)
{
    double left = when - host_wall_seconds();
    if (left > 0.001) native_sleep_ns((uint64_t)((left - 0.0007) * 1e9));
    while (host_wall_seconds() < when) native_cpu_relax();
}

/* DHD_INTERP_DUMP=prefix with DHD_INTERP_DUMP_FIELDS=first-last: every picture shown while the
   newer field is in that window, as prefix-fNNNNNN-KK-tTTT.bmp (K counts the pictures of that
   field, T is t in thousandths). Kept in memory (at most 400) and written after the window, so
   the pictures keep their real timing. A diagnostic for the in-between pictures. Context lock
   held. */
static void native_dump_picture(Native *n, const NativeFrame *b, float t)
{
    enum { MAX = 400 };
    static int init, written;
    static const char *prefix;
    static unsigned long long first, last, field;
    static unsigned picture, count, w, h;
    static struct { unsigned long long field; unsigned picture; int t; uint32_t *pixels; } kept[MAX];
    if (!init++) {
        const char *range = getenv("DHD_INTERP_DUMP_FIELDS");
        prefix = getenv("DHD_INTERP_DUMP");
        if (!range || sscanf(range, "%llu-%llu", &first, &last) != 2) prefix = NULL;
    }
    if (!prefix || written || b->field < first || !n->display_width) return;
    if (b->field > last || count == MAX) {
        for (unsigned i = 0; i < count; i++) {
            char path[1024];
            snprintf(path, sizeof path, "%s-f%06llu-%02u-t%04d.bmp", prefix, kept[i].field, kept[i].picture, kept[i].t);
            FILE *f = fopen(path, "wb");
            if (!f) continue;
            /* BITMAPFILEHEADER (14 bytes) and BITMAPINFOHEADER (40), little-endian: a 32-bit
               top-down BI_RGB picture */
            uint8_t header[54] = {'B', 'M'};
            uint32_t fields[][2] = {{2, 54 + w * h * 4}, {10, 54}, {14, 40}, {18, w}, {22, 0u - h}, {26, 1 | 32 << 16}};
            for (unsigned k = 0; k < sizeof fields / sizeof fields[0]; k++)
                for (unsigned b = 0; b < 4; b++) header[fields[k][0] + b] = (uint8_t)(fields[k][1] >> b * 8);
            fwrite(header, sizeof header, 1, f);
            for (unsigned j = 0; j < w * h; j++) {
                uint32_t v = kept[i].pixels[j];
                kept[i].pixels[j] = (v & 0xFF00FF00) | (v & 255) << 16 | (v >> 16 & 255);
            }
            fwrite(kept[i].pixels, 4, (size_t)w * h, f);
            fclose(f);
            free(kept[i].pixels);
        }
        written = 1;
        return;
    }
    picture = field == b->field ? picture + 1 : 0;
    field = b->field;
    w = n->display_width, h = n->display_height;
    uint32_t *pixels = malloc((size_t)w * h * 4);
    if (!pixels) return;
    if (!native_read_display(n, pixels)) {
        free(pixels);
        return;
    }
    kept[count].field = field, kept[count].picture = picture, kept[count].t = (int)(t * 1000), kept[count].pixels = pixels;
    count++;
}

/* The frame buffer a field drew into last: the display circuit still shows the one the field
   before finished (double buffering), so an in-between picture drawn from this field shows only
   if scanned out from here. -1 when the field drew into no other surface like the displayed one. */
static int native_drawn_surface(Native *n, const NativeFrame *f)
{
    if (f->display_surface < 0) return -1;
    const NativeSurface *shown = &n->surfaces[f->display_surface];
    for (unsigned i = f->command_count; i-- > 0;) {
        const NativeCommand *c = &f->commands[i];
        if (c->kind != NATIVE_DRAW || c->target < 0 || c->target == f->display_surface) continue;
        const NativeSurface *s = &n->surfaces[c->target];
        if (!s->depth && s->bw == shown->bw && s->wide == shown->wide && s->srv) return c->target;
    }
    return -1;
}

/* A phone's app away from the screen (finding 227; the rules: render_gate.h): every piece of GPU
   work this thread records, submits or presents passes native_gate, under queue_lock. */
#include "render_gate.h"
static RenderGate native_gate;      /* queue_lock (render_gate.h) */
static Native *native_reported;     /* (a tentative definition: it is set where the renderer starts) */

/* Android: the exact renderer's submits run on the render thread (dhdr_exact_set_submitter,
   port/render/src/exact/encoder.rs). On the Adreno 660 (driver 512) exact compute work recorded
   and submitted from the GS worker while this thread recorded its passes crashed this thread
   inside the driver (vkCmdBindPipeline, vkEndCommandBuffer) a picture or so after the first Z
   probe; a context lock around both sides did not help, one recording thread does (finding 091,
   AND3). The GS worker posts the submit and waits; this thread runs it between pictures. */
#if defined(__ANDROID__) && !defined(NATIVE_INLINE)
#define NATIVE_EXACT_ON_RENDER 1
void dhdr_exact_set_submitter(void (*run)(void (*call)(void *), void *arg, void *context), void *context);
static struct { void (*call)(void *); void *arg; int posted, done, live; } native_call;  /* queue_lock */
static _Thread_local int native_on_render;

static void native_exact_run(void (*call)(void *), void *arg, void *context)
{
    Native *n = context;
    if (native_on_render) { call(arg); return; }
    native_mutex_lock(&n->queue_lock);
    if (!native_call.live) { native_mutex_unlock(&n->queue_lock); call(arg); return; }
    while (native_call.posted) native_cond_wait(&n->queue_changed, &n->queue_lock);
    native_call.call = call, native_call.arg = arg, native_call.posted = 1, native_call.done = 0;
    native_cond_broadcast(&n->queue_changed);
    while (!native_call.done) native_cond_wait(&n->queue_changed, &n->queue_lock);
    native_call.posted = 0;
    native_cond_broadcast(&n->queue_changed);
    native_mutex_unlock(&n->queue_lock);
}

/* The render thread, queue_lock held: runs a posted call (the lock released meanwhile). */
static void native_serve_call(Native *n)
{
    if (!native_call.posted || native_call.done) return;
    /* GPU work: deferred while the app is away (the poster waits; served at the return, or at the
       quit so no poster hangs) */
    if (!render_gate_may_start(&native_gate, n->quit)) return;
    render_gate_begin(&native_gate);
    native_mutex_unlock(&n->queue_lock);
    native_call.call(native_call.arg);
    native_mutex_lock(&n->queue_lock);
    render_gate_end(&native_gate);
    native_call.done = 1;
    native_cond_broadcast(&n->queue_changed);
}
#define NATIVE_WAIT(n, idle) \
    for (native_serve_call(n); (idle); native_serve_call(n)) native_cond_wait(&(n)->queue_changed, &(n)->queue_lock)
#else
#define NATIVE_WAIT(n, idle) while (idle) native_cond_wait(&(n)->queue_changed, &(n)->queue_lock)
#endif

/* DHD_INTERP: one picture per display refresh (or DHD_INTERP_HZ tick) from the two held fields. */
static void native_interp_thread(Native *n)
{
    double next = 0;
    for (;;) {
        native_mutex_lock(&n->queue_lock);
        NATIVE_WAIT(n, !n->pending_count && !n->newer && !n->quit);
        if (n->quit) { native_mutex_unlock(&n->queue_lock); return; }
        /* One new field per picture: every field's uploads and copies reach the surfaces. */
        int fresh = 0;
        if (n->pending_count) {
            NativeFrame *f = n->pending[0];
            n->pending[0] = n->pending[1];
            n->pending_count--;
            if (n->older) { native_recycle(n->older); n->free_frames[n->free_count++] = n->older; }
            n->older = n->newer; n->newer = f; fresh = 1;
        }
        native_cond_broadcast(&n->queue_changed);
        native_mutex_unlock(&n->queue_lock);
        NativeFrame *a = n->older, *b = n->newer;
        double now = host_wall_seconds(), period = a ? b->time - a->time : 0;
        /* No interpolation across pauses, loading or a missing older field. */
        int mix = a && period > 0.004 && period < 0.1;
        if (fresh && mix) native_pair(n, a, b);
        float t = mix ? (float)((now - b->time) / period) : 1;
        if (t < 0) t = 0;
        native_mutex_lock(&n->context_lock);
        /* Experiments (DHD_INTERP_MODE): once = each field drawn once, at the first picture;
           drawn = each picture scanned out from the frame buffer the field drew into. */
        static int mode = -1;
        if (mode < 0) {
            const char *m = getenv("DHD_INTERP_MODE");
            mode = m && !strcmp(m, "once") ? 1 : m && !strcmp(m, "drawn") ? 2 : 0;
        }
        int shown = b->display_surface, drawn = mode == 2 ? native_drawn_surface(n, b) : -1;
        if (drawn >= 0) b->display_surface = drawn;
        if (mode == 1 && !fresh) native_present(n);
        else if (t < 1) { native_picture(n, b, native_mixed(n, a, b, t), t); n->stats.interp_frames++; }
        else native_picture(n, b, b->vertices, 1);
        b->display_surface = shown;
        native_dump_picture(n, b, t < 1 ? t : 1);
        native_mutex_unlock(&n->context_lock);
        if (fresh) {
            native_mutex_lock(&n->queue_lock);
            n->rendered = b->field;
            native_cond_broadcast(&n->queue_changed);
            native_mutex_unlock(&n->queue_lock);
        }
        if (n->interp_hz) {
            now = host_wall_seconds();
            next = next && next > now - 0.05 ? next + 1 / n->interp_hz : now + 1 / n->interp_hz;
            native_sleep_until(next);
        }
    }
}

void host_thread_name(const char *name);  /* port/host/host_threads.c */

/* DHD_ZREAD_FOLD and DHD_GPU_OVERLAP (finding 111): off unless set; 0 turns one off, any other
   value on. They save Adreno CPU time but cost about 10% at 2x on Metal, and their probe timing is
   not proven exact, so no platform has them on by default yet. zread.rs reads them the same way. */
static int native_gpu_switch(const char *name)
{
    const char *s = getenv(name);
    return s && strcmp(s, "0") != 0;
}

/* The experiment has no interpolation/browser publication hook. */
static int native_fold_probe(const Native *n)
{
#ifdef NATIVE_INLINE
    (void)n;
    return 0;
#else
    static int on = -1;
    if (on < 0) on = native_gpu_switch("DHD_ZREAD_FOLD");
    return on && !n->interp;
#endif
}

/* Experimental same-picture probe: request it before encoding, take at the old publication
   point, and exclude the current tag in Rust. Disabled until the native-read exactness gate. */
static void native_zread_start_picture(Native *n)
{
    if (n->exact_probe == 1) return;
    native_mutex_lock(&n->queue_lock);
    int on = n->zread_want.on, surface = n->zread_want.surface;
    unsigned zbp = n->zread_want.zbp;
    uint32_t want[4] = {(uint32_t)n->zread_want.rect[0], (uint32_t)n->zread_want.rect[1],
                        (uint32_t)n->zread_want.rect[2], (uint32_t)n->zread_want.rect[3]};
    native_mutex_unlock(&n->queue_lock);
    if (want[2] > 64) want[2] = 64;
    if (want[3] > 64) want[3] = 64;
    int started = on ? dhdr_native_zread_start(n->r, (uint32_t)surface, want, (uint64_t)zbp << 32 | (n->zread_tag++ & 0xFFFFFFFFu)) : 0;
    if (n->trace_probes && on)
        fprintf(stderr, "native zread: picture surface %d zbp %u want %u,%u %ux%u started %d\n", surface, zbp, want[0], want[1],
                want[2], want[3], started);
}

/* Render thread, after a picture (context lock held): the newest finished Z copy to the EE, and a
   copy of what the EE's probe asks for now, from the Z surface just drawn (finding 097). */
static void native_zread_after(Native *n)
{
    /* Even with the exact probe, a skipped batched present must flush the picture. */
    if (n->exact_probe == 1 && !(native_fold_probe(n) && native_gpu_switch("DHD_GPU_OVERLAP"))) return;
    uint32_t rect[4];
    uint64_t tag;
    if (dhdr_native_zread_take(n->r, n->zread_bits, 64 * 64, rect, &tag)) {
        native_mutex_lock(&n->queue_lock);
        n->zread.valid = 1;
        n->zread.zbp = (unsigned)(tag >> 32);
        memcpy(n->zread.rect, rect, sizeof rect);
        memcpy(n->zread.bits, n->zread_bits, (size_t)rect[2] * rect[3] * 4);
        native_mutex_unlock(&n->queue_lock);
        if (n->trace_probes) {
            float lo = 2, hi = -1, d;
            for (uint32_t i = 0; i < rect[2] * rect[3]; i++) {
                memcpy(&d, &n->zread_bits[i], 4);
                if (d < lo) lo = d;
                if (d > hi) hi = d;
            }
            fprintf(stderr, "native zread: took tag %llu zbp %u %u,%u %ux%u depth %g..%g\n", (unsigned long long)(tag & 0xFFFFFFFFu),
                    (unsigned)(tag >> 32), rect[0], rect[1], rect[2], rect[3], lo, hi);
        }
    }
    if (!native_fold_probe(n)) native_zread_start_picture(n);

}

/* The next handed field drawn and its frame recycled; 0 once told to quit. The render thread's
   loop, or with NATIVE_INLINE the field's own vsync (the field is already pending, so no wait). */
/* The platform's callback and the game's return (render_gate.h, finding 227): the render thread
   finishes the GPU work under way, starts none while the app is away, and leaves the fields queued
   for the return (dropping them would drop their uploads). What that guarantees: no new GPU work or
   present from this thread after gs_render_quiet says so; work submitted before may still run on
   the GPU (iOS lets that finish; it forbids new work from the background). Not covered: the GS
   worker's own exact Z-probe submits on iOS (they don't come through this thread; the worker blocks
   on the held queue within two fields). Desktops never set the hold: only a phone's background
   event (present.c's lifecycle watch) and the iOS app's callbacks do. */
static void gate_lock(Native *n, int on)
{
    if (!n) return;
    if (on) native_mutex_lock(&n->queue_lock);
    else native_mutex_unlock(&n->queue_lock);
}

unsigned gs_render_leave(void)
{
    Native *n = native_reported;
    gate_lock(n, 1);
    unsigned generation = render_gate_leave(&native_gate);
    gate_lock(n, 0);
    return generation;
}

unsigned gs_render_generation(void)
{
    Native *n = native_reported;
    gate_lock(n, 1);
    unsigned generation = native_gate.generation;
    native_gate.hold = 1;   /* the game's own wait holds too (its callback may still come) */
    gate_lock(n, 0);
    return generation;
}

unsigned gs_render_current(void)
{
    Native *n = native_reported;
    gate_lock(n, 1);
    unsigned generation = native_gate.generation;
    gate_lock(n, 0);
    return generation;
}

int gs_render_release(unsigned generation)
{
    Native *n = native_reported;
    gate_lock(n, 1);
    int released = render_gate_release(&native_gate, generation), held = native_gate.held;
    if (released) {
        native_gate.held = 0;
        if (n) native_cond_broadcast(&n->queue_changed);   /* a deferred call or a held field goes now */
    }
    gate_lock(n, 0);
    if (released && held) fprintf(stderr, "gs: the renderer held %d fields while the app was away; it draws them now\n", held);
    return released;
}

int gs_render_quiet(void)
{
    Native *n = native_reported;
    gate_lock(n, 1);
    int quiet = render_gate_quiet(&native_gate);
    gate_lock(n, 0);
    return quiet;
}

static int native_render_next(Native *n)
{
    double rs_wait = native_rstat_start(), waited = perf_log_on() ? host_wall_seconds() : 0;
    native_mutex_lock(&n->queue_lock);
    for (;;) {
        NATIVE_WAIT(n, !n->pending_count && !n->quit);
        if (n->quit) { native_mutex_unlock(&n->queue_lock); return 0; }
        if (render_gate_may_start(&native_gate, n->quit)) break;
        native_gate.held = n->pending_count;   /* held: the field stays queued */
        native_mutex_unlock(&n->queue_lock);
        native_sleep_ns(5000000);
        native_mutex_lock(&n->queue_lock);
    }
    render_gate_begin(&native_gate);   /* the picture: GPU work under way (gs_render_quiet) */
    NativeFrame *f = n->pending[0];
    n->pending[0] = n->pending[1];
    n->pending_count--;
    native_cond_broadcast(&n->queue_changed);
    native_mutex_unlock(&n->queue_lock);
    native_rstat_add(RSTAT_WAIT, rs_wait);
    if (waited) perf_wait_self(host_wall_seconds() - waited);  /* waiting for the next field: not work */
    /* The frame from here (the field handed over) to the last Z copy: the render thread's work. */
    double rs_frame = native_rstat_start();
    static int spikes = -1;
    if (spikes < 0) spikes = getenv("DHD_SPIKES") != NULL;
    double asked = spikes ? host_wall_seconds() : 0;
    double rs_lock = native_rstat_start();
    native_mutex_lock(&n->context_lock);
    native_rstat_add(RSTAT_LOCK, rs_lock);
    if (spikes && host_wall_seconds() - asked >= 0.002)   /* another thread held the context */
        fprintf(stderr, "spike_lock: field=%llu start=%.9f ms=%.3f\n", (unsigned long long)f->field, asked,
                (host_wall_seconds() - asked) * 1e3);
    n->present_due = f->due, n->present_began = f->began;
    native_picture(n, f, f->vertices, 1);
    double rs_zread = native_rstat_start();
    native_zread_after(n);
    native_rstat_add(RSTAT_ZREAD, rs_zread);
    native_mutex_unlock(&n->context_lock);
    native_rstat_add(RSTAT_FRAME, rs_frame);
    native_rstat_end();
    native_recycle(f);
    native_mutex_lock(&n->queue_lock);
    render_gate_end(&native_gate);
    n->free_frames[n->free_count++] = f;
    n->rendered = f->field;
    native_cond_broadcast(&n->queue_changed);
    native_mutex_unlock(&n->queue_lock);
    return 1;
}

#ifndef NATIVE_INLINE
static int SDLCALL native_thread(void *argument)
{
    Native *n = argument;
    host_thread_name("native");
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#ifdef NATIVE_EXACT_ON_RENDER
    native_on_render = 1;
    native_mutex_lock(&n->queue_lock);
    native_call.live = 1;
    native_mutex_unlock(&n->queue_lock);
    dhdr_exact_set_submitter(native_exact_run, n);
    fprintf(stderr, "gs: exact submits run on the render thread\n");
#endif
    if (n->interp) native_interp_thread(n);
    else while (native_render_next(n)) {}
    /* Left for good (native_stop): from here this thread touches nothing of the renderer's. A call
       the exact renderer posted meanwhile is run by its poster from now on (native_call.live). */
    native_mutex_lock(&n->queue_lock);
#ifdef NATIVE_EXACT_ON_RENDER
    native_call.live = 0;
    native_serve_call(n);
#endif
    n->thread_exited = 1;
    native_cond_broadcast(&n->queue_changed);
    native_mutex_unlock(&n->queue_lock);
    return 0;
}

/* The end of the process (host_end.h, the render stage): the render thread finishes the picture it
   is drawing and leaves; waits for it 1 s at most (a present stuck in the driver), then goes on
   without it. Finding 210: exit() with this thread alive let the GPU driver's static destructors
   destroy a mutex it then locked (bionic aborts on that). */
static void native_stop(void)
{
    Native *n = native_reported;
    if (!n) return;
    native_mutex_lock(&n->queue_lock);
    if (!n->thread) { native_mutex_unlock(&n->queue_lock); return; }
    n->quit = 1;
    native_cond_broadcast(&n->queue_changed);
    for (int waited = 0; !n->thread_exited && waited < 1000; waited++) {
        native_mutex_unlock(&n->queue_lock);
        native_sleep_ns(1000000);
        native_mutex_lock(&n->queue_lock);
    }
    int exited = n->thread_exited;
    native_mutex_unlock(&n->queue_lock);
    if (!exited) fprintf(stderr, "gs native: the render thread did not stop within 1 s; the process leaves without it\n");
}
#endif

/* ---- presentation interface (EE thread, GS worker idle) ---- */

/* The PC settings page (EE thread): the context lock around a swap chain change, and the
   present options read again. */
void gs_native_lock(Gs *g, int lock)
{
    Native *n = ((NativeExact *)g->backend)->native;
    if (lock) native_mutex_lock(&n->context_lock);
    else native_mutex_unlock(&n->context_lock);
}

void gs_native_present_options(Gs *g)
{
    Native *n = ((NativeExact *)g->backend)->native;
    native_mutex_lock(&n->context_lock);
    native_present_options(n);
    native_mutex_unlock(&n->context_lock);
}

int gs_gpu_native(const Gs *g)
{
    return g && g->backend && ((const NativeExact *)g->backend)->native != NULL;
}

void gs_native_vsync(Gs *g, unsigned *width, unsigned *height, int resize)
{
    double due, began;
    pacing_handed(&due, &began);
    gs_native_vsync_handed(g, width, height, resize, due, began, host_widescreen_flat_limit(), host_widescreen_card_drawn(),
                           host_widescreen_aspect());
}

void gs_native_vsync_handed(Gs *g, unsigned *width, unsigned *height, int resize, double due, double began,
                            int flat_limit, int card_2d, double output_aspect)
{
    FILE *trace = native_field_trace("registers");
    if (trace) {
        extern uint64_t kernel_vsync_count;
        fprintf(trace, "%llu,%llX,%llX,%llX,%llX,%llX,%llX,%llX,%llX,%llX\n",
                (unsigned long long)kernel_vsync_count,
                (unsigned long long)g->reg[GS_XYOFFSET_1], (unsigned long long)g->reg[GS_XYOFFSET_2],
                (unsigned long long)gs_priv_read(g, GS_PMODE), (unsigned long long)gs_priv_read(g, GS_SMODE2),
                (unsigned long long)gs_priv_read(g, GS_DISPFB1), (unsigned long long)gs_priv_read(g, GS_DISPLAY1),
                (unsigned long long)gs_priv_read(g, GS_DISPFB2), (unsigned long long)gs_priv_read(g, GS_DISPLAY2),
                (unsigned long long)gs_priv_read(g, GS_CSR));
        fflush(trace);
        FILE *draws = native_field_trace("draws");
        if (draws) fflush(draws);
    }
    NativeExact *gpu = g->backend;
    Native *n = gpu->native;
    n->upload_epoch++;
    NativeFrame *f = n->recording;
    native_close(n, g);
    /* The display circuit, as gs_gpu_display reads it. */
    uint64_t pmode = gs_priv_read(g, GS_PMODE);
    unsigned circuit = pmode & 1 ? 0 : 1;
    uint64_t fb = gs_priv_read(g, circuit ? GS_DISPFB2 : GS_DISPFB1);
    uint64_t display = gs_priv_read(g, circuit ? GS_DISPLAY2 : GS_DISPLAY1);
    uint32_t *d = f->display;
    memset(d, 0, sizeof f->display);
    d[0] = (unsigned)(fb >> 15) & 31; d[1] = ((unsigned)fb & 511) * 32;
    d[2] = (unsigned)(fb >> 9) & 63; d[3] = (unsigned)(fb >> 32) & 2047; d[4] = (unsigned)(fb >> 43) & 2047;
    d[5] = ((unsigned)(display >> 32) & 4095) + 1; d[5] /= ((unsigned)(display >> 23) & 15) + 1;
    d[6] = ((unsigned)(display >> 44) & 2047) + 1; d[6] /= ((unsigned)(display >> 27) & 3) + 1;
    d[7] = (gs_priv_read(g, GS_SMODE2) & 3) == 3;
    /* scanout reads display_state[0] = d[0..3] and [1] = d[4..7] with b.x = DBY etc. */
    uint32_t packed[12] = {d[0], d[1] / 32, d[2], d[3], d[4], d[5], d[6], d[7]};
    f->display_surface = -1;
    if (!(pmode & 3)) f->display_surface = NATIVE_DISPLAY_OFF;  /* no read circuit on: a black screen */
    else if (d[2] && d[5] && d[6] && d[5] <= 1024 && d[6] <= 1024 && psm_color(d[0])) {
        f->display_surface = native_surface(n, (unsigned)fb & 511, d[2], d[0], 0);
        if (f->display_surface >= 0) {
            native_claim(n, g, f->display_surface, d[0], (int)d[3], (int)d[4], (int)(d[3] + d[5] - 1),
                         (int)(d[4] + (d[6] >> d[7]) - 1));
            if (n->usage) n->usage[f->display_surface].displayed++;
        }
        *width = d[5]; *height = d[6];
    } else n->stats.display_missing++;
    memcpy(f->display, packed, sizeof packed);
    f->flat_limit = (unsigned)flat_limit;
    f->output_aspect = output_aspect;
    f->card_2d = card_2d;
    f->field = ++n->field;
    if (n->texpack) texpack_field(n->field);
    f->time = host_wall_seconds();
    f->due = due, f->began = began;
    n->stats.frames++;
    if (n->field <= n->full_until) n->stats.full_fields++;
    if (n->trace_fields) {
        extern uint64_t kernel_vsync_count;
        const NativeStats *s = &n->stats, *l = &n->trace_last;
        fprintf(n->trace_fields, "%llu,%llu,%.6f,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d,%u\n",
                (unsigned long long)f->field, (unsigned long long)kernel_vsync_count, f->time, f->command_count,
                f->vertex_count, s->primitives - l->primitives, s->decoded - l->decoded, s->decoded_texels - l->decoded_texels,
                s->texture_from_surface - l->texture_from_surface, s->texture_unsupported - l->texture_unsupported,
                s->imports - l->imports, s->copies - l->copies, s->copy_fallbacks - l->copy_fallbacks,
                s->exact_draws - l->exact_draws, s->color_reads - l->color_reads, n->field <= n->full_until,
                f->index_count);
        n->trace_last = *s;
    }
    /* Hand the field to the render thread; at most one waits. */
    native_mutex_lock(&n->queue_lock);
    n->resize |= resize;
#ifndef NATIVE_INLINE
    if (!n->thread) n->thread = native_thread_start(native_thread, n);
#endif
    /* One field waits for the render thread; with DHD_INTERP two, so the EE never waits for a
       display refresh. */
    /* The GS worker (or the game thread) waiting here waits for the render thread: perf_log's
       busy time leaves it out (two clock reads, only when it waits). */
    double handoff = (n->pending_count >= (n->interp ? 2u : 1u) || !n->free_count) && perf_log_on() ? host_wall_seconds() : 0;
    while (n->pending_count >= (n->interp ? 2u : 1u)) native_cond_wait(&n->queue_changed, &n->queue_lock);
    while (!n->free_count) native_cond_wait(&n->queue_changed, &n->queue_lock);
    if (handoff) perf_wait_self(host_wall_seconds() - handoff);
    n->pending[n->pending_count++] = f;
    n->handed = f->field;
    n->recording = n->free_frames[--n->free_count];
    native_cond_broadcast(&n->queue_changed);
    native_mutex_unlock(&n->queue_lock);
#ifdef NATIVE_INLINE
    native_render_next(n);  /* no render thread: the field is drawn now */
#endif
    /* The probe log keeps this field and the previous one. */
    NativeLog spare = n->logs[0];
    n->logs[0] = n->logs[1];
    n->logs[1] = spare;
    n->logs[1].draw_count = n->logs[1].state_count = 0;
    n->logs[1].state_version = UINT64_MAX;
}

/* The last handed field's picture, once the render thread has drawn it, in n->picture (its size
   in width and height; upscaled with DHD_SCALE). */
static int native_picture_read(Native *n, unsigned *width, unsigned *height)
{
    native_mutex_lock(&n->queue_lock);
    while (n->rendered < n->handed) native_cond_wait(&n->queue_changed, &n->queue_lock);
    native_mutex_unlock(&n->queue_lock);
    native_mutex_lock(&n->context_lock);
    size_t size = (size_t)n->display_width * n->display_height;
    int ok = size != 0;
    if (ok && size > n->picture_size) {
        uint32_t *grown = realloc(n->picture, size * 4);
        if (grown) n->picture = grown, n->picture_size = size;
        else ok = 0;
    }
    if (ok) ok = native_read_display(n, n->picture);
    if (ok) { *width = n->display_width; *height = n->display_height; }
    native_mutex_unlock(&n->context_lock);
    return ok;
}

/* gs_gpu_capture: the picture into a 1x-sized buffer (pixels[] in present.c), so only when it fits. */
static int native_capture(Native *n, uint32_t *pixels)
{
    unsigned w, h;
    if (!native_picture_read(n, &w, &h) || (size_t)w * h > 1024 * 1024) return 0;
    memcpy(pixels, n->picture, (size_t)w * h * 4);
    return 1;
}

/* DHD_SCREENSHOT_WINDOW=WxH: screenshots as a W x H window shows the picture (its shape, the
   bars beside it; nearest pixels), to check the presentation (widescreen, 2D fields at 4:3). */
static uint32_t *native_window_picture(Native *n, unsigned *width, unsigned *height)
{
    static uint32_t *out;
    static size_t out_size;
    unsigned w, h;
    const char *size = getenv("DHD_SCREENSHOT_WINDOW");
    if (!size || sscanf(size, "%ux%u", &w, &h) != 2 || !w || !h || w > 8192 || h > 8192) return n->picture;
    if ((size_t)w * h > out_size) {
        uint32_t *grown = realloc(out, (size_t)w * h * 4);
        if (!grown) return n->picture;
        out = grown, out_size = (size_t)w * h;
    }
    float rect[4];
    NativeRect cut;
    native_present_rect(n, w, h, rect, &cut);   /* widescreen's fill beside a 4:3 picture is left black here */
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            int px = (int)x - (int)rect[0], py = (int)y - (int)rect[1];
            uint32_t c = 0xFF000000u;
            if ((int)x >= cut.left && (int)y >= cut.top && (int)x < cut.right && (int)y < cut.bottom)
                c = n->picture[(unsigned)(py * (float)*height / rect[3]) * *width + (unsigned)(px * (float)*width / rect[2])];
            out[y * w + x] = c;
        }
    *width = w, *height = h;
    return out;
}

void gs_native_movie(Gs *g, const uint32_t *rgba, unsigned width, unsigned height, unsigned rows)
{
    NativeFrame *f = ((NativeExact *)g->backend)->native->recording;
    f->movie_on = 0;
    if (!rgba || !width || !height) return;
    if ((size_t)width * height > f->movie_capacity) {
        uint32_t *grown = realloc(f->movie, (size_t)width * height * 4);
        if (!grown) return;
        f->movie = grown, f->movie_capacity = width * height;
    }
    memcpy(f->movie, rgba, (size_t)width * height * 4);
    f->movie_width = width, f->movie_height = height, f->movie_rows = rows, f->movie_on = 1;
}

uint32_t *gs_native_picture(Gs *g, unsigned *width, unsigned *height)
{
    Native *n = ((NativeExact *)g->backend)->native;
    return native_picture_read(n, width, height) ? native_window_picture(n, width, height) : NULL;
}

/* Displayed pictures per second and the spread of the time between them (the worst 1% is the mean
   of the longest hundredth). Context lock held, or the renderer stopped. */
static void native_intervals(Native *n, const char *label, uint64_t presented, uint64_t interpolated)
{
    static float sorted[NATIVE_INTERVALS];
    unsigned count = n->interval_count < NATIVE_INTERVALS ? (unsigned)n->interval_count : NATIVE_INTERVALS;
    if (!count) return;
    memcpy(sorted, n->intervals, count * sizeof *sorted);
    for (unsigned i = 1; i < count; i++) {   /* insertion sort: a report, not a hot path */
        float v = sorted[i];
        unsigned j = i;
        while (j && sorted[j - 1] > v) { sorted[j] = sorted[j - 1]; j--; }
        sorted[j] = v;
    }
    unsigned worst = count / 100 ? count / 100 : 1;
    double tail = 0;
    for (unsigned i = count - worst; i < count; i++) tail += sorted[i];
    double seconds = n->last_present - n->first_present;
    const NativeStats *s = &n->stats;
    uint64_t total = s->paired_vertices + s->held_vertices + s->jumped_vertices;
    fprintf(stderr, "%s: displayed_fps=%.2f present_ms p50=%.2f p99=%.2f worst1pct=%.2f max=%.2f interpolated=%llu "
            "paired=%.1f%% held=%.1f%% jumped=%.1f%%\n", label,
            seconds > 0 && presented ? (double)(presented - 1) / seconds : 0, sorted[count / 2], sorted[count * 99 / 100],
            tail / worst, sorted[count - 1], (unsigned long long)interpolated, total ? 100.0 * s->paired_vertices / total : 0,
            total ? 100.0 * s->held_vertices / total : 0, total ? 100.0 * s->jumped_vertices / total : 0);
    fflush(stderr);
}

/* DHD_BENCH (present.c): presents are counted from the window's first field to its last. */
void gs_native_bench(Gs *g, int end)
{
    if (!gs_gpu_native(g)) return;
    Native *n = ((NativeExact *)g->backend)->native;
    native_mutex_lock(&n->context_lock);
    if (!end) {
        n->interval_count = 0; n->last_present = 0;
        n->bench_presented = n->stats.presented; n->bench_interp = n->stats.interp_frames;
        n->stats.paired_vertices = n->stats.held_vertices = n->stats.jumped_vertices = 0;
    } else {
        native_intervals(n, "bench interp", n->stats.presented - n->bench_presented, n->stats.interp_frames - n->bench_interp);
    }
    native_mutex_unlock(&n->context_lock);
}

static void native_profile(Native *n)
{
    const NativeStats *s = &n->stats;
    fprintf(stderr, "profile native: frames=%llu presented=%llu draws=%llu primitives=%llu commands=%llu surfaces=%ld "
            "decoded=%llu texels=%llu cache_hits=%llu from_surface=%llu unsupported_textures=%llu imports=%llu copies=%llu "
            "copy_fallbacks=%llu blend_approx=%llu date=%llu probe_hits=%llu probe_replays=%llu probe_uncovered=%llu "
            "replayed=%llu exact_draws=%llu color_reads=%llu display_missing=%llu why=%llu,%llu,%llu,%llu,%llu,%llu "
            "z_alias_draws=%llu band_pages=%u band_draws=%llu band_rt_textures=%llu probe_band_hits=%llu band_moves=%llu "
            "exact_targets=%llu target_draws=%llu full_fields=%llu converted=%llu conversions_run=%llu blend_dest_alpha=%llu\n",
            s->frames, s->presented, s->draws, s->primitives, s->commands, n->surface_count, s->decoded, s->decoded_texels,
            s->cache_hits, s->texture_from_surface, s->texture_unsupported, s->imports, s->copies, s->copy_fallbacks,
            s->blend_approximate, s->date_ignored, s->probe_hits, s->probe_replays, s->probe_uncovered,
            s->probe_replayed_draws, s->exact_draws, s->color_reads, s->display_missing,
            s->unsupported_why[0], s->unsupported_why[1], s->unsupported_why[2], s->unsupported_why[3],
            s->unsupported_why[4], s->unsupported_why[5], s->z_alias_draws, n->band_count, s->band_draws,
            s->band_rt_textures, s->probe_band_hits, s->band_moves, s->exact_targets, s->target_draws, s->full_fields,
            s->converted, s->conversions_run, s->blend_dest_alpha);
    if (n->interp) native_intervals(n, "profile interp", s->presented, s->interp_frames);
    if (n->texpack) {
        fprintf(stderr, "textures: %llu draw states used a replacement\n", (unsigned long long)n->replaced_draws);
        texpack_report();
    }
    for (unsigned i = 0; n->usage && i < (unsigned)n->surface_count; i++) {
        const NativeSurface *f = &n->surfaces[i];
        const NativeUsage *u = &n->usage[i];
        fprintf(stderr, "surface %u: page=%u bw=%u wide=%u depth=%u extent=%ldx%ld scale=%u size=%ux%u draws=%llu "
                "depth_draws=%llu (z %d) sampled=%llu copied_in=%llu copied_out=%llu converted=%llu displayed=%llu\n", i,
                f->page, f->bw, f->wide, f->depth, f->extent[0], f->extent[1], f->scale, f->size[0], f->size[1], u->draws, u->depth_draws, u->depth_draws ? u->depth_with : -1,
                u->sampled, u->copied_in, u->copied_out, u->converted, u->displayed);
        for (unsigned j = 0; j < NATIVE_SURFACES; j++)
            if (u->pair[j])
                fprintf(stderr, "  samples %u: %llu draws; sprites by texels/pixel <0.75 %llu, 1 %llu, 2 %llu, more %llu; "
                        "1:1 offset quarters %llu %llu %llu %llu\n", j, u->pair[j], u->ratio[j][0], u->ratio[j][1],
                        u->ratio[j][2], u->ratio[j][3], u->offset[j][0], u->offset[j][1], u->offset[j][2], u->offset[j][3]);
    }
}

static void native_exit_report(void);
#ifdef NATIVE_INLINE
static Native *native_reported;
#endif
static void native_exit_report(void) { if (native_reported) native_profile(native_reported); }

/* DHD_ASPECT: 4:3 (a television's picture, the default), 16:9 or any W:H, stretch, pixels, or
   wide: widescreen's shape (port/hle/widescreen.c; 4:3 when widescreen is off). Read again
   whenever the PC settings page changes it (gs_native_present_options). */
static void native_aspect_options(Native *n)
{
    const char *aspect = getenv("DHD_ASPECT");
    unsigned aw = 4, ah = 3;
    double host_widescreen_aspect(void);
    n->wide = (float)host_widescreen_aspect();
    /* iOS's game-thread widescreen update follows the active output, including hotplug. */
    if (aspect && !strcmp(aspect, "wide")) n->aspect = n->wide ? n->wide : 4.0f / 3;
    else if (aspect && !strcmp(aspect, "stretch")) n->aspect = 0;
    else if (aspect && !strcmp(aspect, "pixels")) n->aspect = -1;
    else {
        if (aspect && (sscanf(aspect, "%u:%u", &aw, &ah) != 2 || !aw || !ah)) aw = 4, ah = 3;
        n->aspect = (float)aw / ah;
    }
}

/* A new renderer's shared state, before the backend creates its device objects; NULL without memory. */
static Native *native_create(NativeExact *exact)
{
    Native *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->exact = exact;
    n->field_trace_on = getenv("DHD_FIELD_TRACE") && getenv("DHD_FIELD_TRACE_FIELDS");
    native_mutex_init(&n->context_lock);
    native_mutex_init(&n->queue_lock);
    native_cond_init(&n->queue_changed);
    n->recording = &n->frames[0];
    for (unsigned i = 1; i < NATIVE_FRAMES; i++) n->free_frames[n->free_count++] = &n->frames[i];
    const char *interp = getenv("DHD_INTERP"), *hz = getenv("DHD_INTERP_HZ");
    n->interp = interp && atoi(interp) != 0;
#if defined(__ANDROID__) || defined(DHD_IOS_RENDER)
    /* its own picture loop doesn't pass the render gate a phone's background needs (finding 227) */
    if (n->interp) fprintf(stderr, "gs: DHD_INTERP is not offered on a phone; ignored\n");
    n->interp = 0;
#endif
#ifdef NATIVE_INLINE
    n->interp = 0;  /* its pictures come between fields, from a thread of their own */
#endif
    n->interp_hz = n->interp && hz ? atof(hz) : 0;
    const char *indexed = getenv("DHD_NATIVE_INDEXED");
    n->indexed = !n->interp && !(indexed && atoi(indexed) == 0);
    n->logs[1].state_version = UINT64_MAX;
    for (unsigned i = 0; i < 512; i++) n->owner[i] = n->top_owner[i] = -1;
    /* DHD_SCALE (dhd.ini "scale"): 1 to NATIVE_SCALE_MAX (default 2), or "window" for the smallest
       scale whose picture is as tall as the window. Upscaling draws only the pictures; the exact
       backend answering the game's reads stays at 1x. */
    const char *scale = getenv("DHD_SCALE");
    n->scale_setting = 2;
    if (scale && (!native_stricmp(scale, "window") || !native_stricmp(scale, "match"))) n->scale_setting = 0;
    else if (scale && atoi(scale) >= 1) n->scale_setting = (unsigned)atoi(scale);
    if (n->scale_setting > NATIVE_SCALE_MAX) {
        fprintf(stderr, "gs: resolution scale %u is more than this renderer draws; %u (DHD_SCALE)\n",
                n->scale_setting, NATIVE_SCALE_MAX);
        n->scale_setting = NATIVE_SCALE_MAX;
    }
    return n;
}

/* Word in a page to the pixel holding it, for the colour (PSMCT32) and Z (PSMZ32) layouts: x | y << 8,
   colour then Z (native_convert's native_inverse); and n->color_pixel from it. */
static void native_inverse_table(Native *n, uint32_t inverse[4096])
{
    for (unsigned layout = 0; layout < 2; layout++)
        for (unsigned y = 0; y < 32; y++)
            for (unsigned x = 0; x < 64; x++)
                inverse[layout * 2048 + (gs_pixel_address(layout ? GS_PSMZ32 : GS_PSMCT32, 0, 1, x, y) >> 5 & 2047)] = x | y << 8;
    for (unsigned i = 0; i < 2048; i++) n->color_pixel[i] = (uint16_t)((inverse[i] & 255) + (inverse[i] >> 8) * 64);
}

/* The backend's objects exist: the GS hooks go to the native renderer, the exact backend only
   answers the Z probe (nothing until a probe), and the diagnostics open. */
static void native_install(Gs *g, NativeExact *exact, Native *n, const char *api)
{
    exact->native = n;
    exact->clip[0] = exact->clip[1] = 1; exact->clip[2] = exact->clip[3] = 0;  /* nothing until a probe */
    exact->clip_on = 1;
    exact->e.upload_lazily = 1;  /* the probe's draws need only the uploaded pages they touch */
    g->draw = native_draw;
    g->draw_strip = native_draw_strip;
    gs_strip_hook = native_strip;
    g->local_copy = native_local_copy;
    g->flush_draw = native_flush_draw;
    g->write_high = native_write_high;
    g->stage_pages = native_stage_pages;
    g->fetch_pages = native_fetch_pages;
    const char *exact_probe = getenv("DHD_EXACT_PROBE");
    n->exact_probe = exact_probe ? atoi(exact_probe) : 0;
    gs_mesh_hook = native_mesh;   /* DHD_NATIVE_VU1 (port/hle/vu1_mesh.inc, finding 164) */
    /* none where the GPU can't draw meshes (the renderer said why; Native VU, finding 194) */
    __atomic_store_n(&gs_mesh_mode, !n->indexed || !dhdr_native_meshes() ? 0 : n->exact_probe ? 2 : 1, __ATOMIC_RELAXED);
    n->trace_probes = getenv("DHD_NATIVE_PROBES") != NULL;
    if (getenv("DHD_NATIVE_SURFACES") && !(n->usage = calloc(NATIVE_SURFACES, sizeof *n->usage))) native_oom("usage");
    const char *texlog = getenv("DHD_NATIVE_TEXLOG");
    if (texlog && *texlog && (n->texlog = fopen(texlog, "w")))
        fputs("field,event,why,psm,tbp,tbw,w,h,surface_page,surface_bw,surface_wide,surface_depth\n", n->texlog);
    const char *hold = getenv("DHD_NATIVE_FULL_HOLD");
    n->full_hold = hold && *hold ? (unsigned)atoi(hold) : 20;
    const char *trace = getenv("DHD_NATIVE_TRACE");
    if (trace && *trace) {
        char path[NATIVE_PATH_MAX];
        snprintf(path, sizeof path, "%s-fields.csv", trace);
        if ((n->trace_fields = fopen(path, "w")))
            fputs("field,vsync,time,commands,vertices,primitives,decoded,decoded_texels,from_surface,unsupported,"
                  "imports,copies,copy_fallbacks,exact_draws,color_reads,full,indices\n", n->trace_fields);
        snprintf(path, sizeof path, "%s-pictures.csv", trace);
        if ((n->trace_pictures = fopen(path, "w")))
            fputs("field,t,cpu_ms,gpu_ms,draws,self_copies,self_copy_px,copies,copy_px,imports,import_px\n", n->trace_pictures);
    }
    native_reported = n;
#ifndef NATIVE_INLINE
    host_end_add(HOST_END_RENDER, "render thread", native_stop);
#endif
    host_end_add(HOST_END_REPORT, "native report", native_exit_report);
    fprintf(stderr, "gs: native renderer (DHD_RENDER=native); exact GS only for the Z probe\n");
    fprintf(stderr, "gs: native renderer on %s\n", api);
    /* what was asked (the scale in use: "gs native: resolution scale" at the first picture) */
    if (n->scale_asked) fprintf(stderr, "gs: resolution scale %u asked (DHD_SCALE)\n", n->scale_asked);
    else fprintf(stderr, "gs: resolution scale matching the window asked (DHD_SCALE)\n");
    if (n->interp)
        fprintf(stderr, "gs: interpolated frames (DHD_INTERP=1), paced by %s\n", n->interp_hz ? "DHD_INTERP_HZ" : "vsync");
}
