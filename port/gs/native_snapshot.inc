/* A recorded native field as the Rust renderer's DhdrPicture (port/render/include/dhd_render.h).

   Included by gpu_rust_shared.inc (for gpu_rust_native.inc and the browser's
   gpu_rust_web_native.inc): the picture dhdr_native_picture draws. It had a second includer, the
   retired C renderer's DHD_NATIVE_RTRACE recorder in gpu_wgpu_native.inc (last at render/rust 3882d5576), which
   wrote the same picture to an .ntrace (port/render/NTRACE.md), so a trace holds exactly what the
   Rust path is given.

   Included after native_core.inc (it needs Native, NativeFrame and native_oom) and after
   dhd_render.h. A texture becomes a DhdrTexture pointer through two macros the includer may
   define first:
     NATIVE_SNAPSHOT_TEXTURE(t)  t a NativeTexture * (never NULL); default (t)->gpu, the Rust
                                 path's NATIVE_TEXTURE_GPU member;
     NATIVE_SNAPSHOT_VIEW(v)     v a NativeView (never NULL); default (DhdrTexture *)(v), as the
                                 Rust path's NativeView is a DhdrTexture *.
   (The C path's recorder passed its own handles there and mapped them to texture contents itself.)

   Render thread, under the context lock, after native_prepare_surfaces: the surface snapshot
   holds each surface's scale and texture size as the picture's commands will see them. The
   picture points into the snapshot's arrays, the frame's vertices (the ones passed in, possibly
   DHD_INTERP-mixed) and the frame's pixel and movie buffers: it is valid until the next
   native_snapshot on the same NativeSnapshot or until the frame is recycled, whichever comes
   first. No GPU work, no allocation once the arrays have grown to the largest field. */

#ifndef NATIVE_SNAPSHOT_TEXTURE
#define NATIVE_SNAPSHOT_TEXTURE(t) ((t)->gpu)
#endif
#ifndef NATIVE_SNAPSHOT_VIEW
#define NATIVE_SNAPSHOT_VIEW(v) ((DhdrTexture *)(v))
#endif

/* The vertices go through as they are, and the uniform blocks are copied whole: their layouts
   must be the same. */
_Static_assert(sizeof(DhdrVertex) == sizeof(NativeVertex), "DhdrVertex");
_Static_assert(offsetof(DhdrVertex, x) == offsetof(NativeVertex, x) && offsetof(DhdrVertex, y) == offsetof(NativeVertex, y) &&
               offsetof(DhdrVertex, z) == offsetof(NativeVertex, z) && offsetof(DhdrVertex, s) == offsetof(NativeVertex, s) &&
               offsetof(DhdrVertex, t) == offsetof(NativeVertex, t) && offsetof(DhdrVertex, q) == offsetof(NativeVertex, q) &&
               offsetof(DhdrVertex, fog) == offsetof(NativeVertex, fog) &&
               offsetof(DhdrVertex, rgba) == offsetof(NativeVertex, rgba), "DhdrVertex members");
_Static_assert(sizeof(DhdrNativeConstants) == sizeof(NativeConstants), "DhdrNativeConstants");
_Static_assert(offsetof(DhdrNativeConstants, target) == offsetof(NativeConstants, target) &&
               offsetof(DhdrNativeConstants, flags) == offsetof(NativeConstants, flags) &&
               offsetof(DhdrNativeConstants, atst) == offsetof(NativeConstants, atst) &&
               offsetof(DhdrNativeConstants, aref) == offsetof(NativeConstants, aref) &&
               offsetof(DhdrNativeConstants, alpha_scale) == offsetof(NativeConstants, alpha_scale) &&
               offsetof(DhdrNativeConstants, texa_alpha) == offsetof(NativeConstants, texa_alpha) &&
               offsetof(DhdrNativeConstants, scale) == offsetof(NativeConstants, scale) &&
               offsetof(DhdrNativeConstants, fog) == offsetof(NativeConstants, fog) &&
               offsetof(DhdrNativeConstants, tex_offset) == offsetof(NativeConstants, tex_offset) &&
               offsetof(DhdrNativeConstants, tex_size) == offsetof(NativeConstants, tex_size) &&
               offsetof(DhdrNativeConstants, wrap) == offsetof(NativeConstants, wrap) &&
               offsetof(DhdrNativeConstants, region) == offsetof(NativeConstants, region) &&
               offsetof(DhdrNativeConstants, mesh_offset) == offsetof(NativeConstants, mesh_offset),
               "DhdrNativeConstants members");
_Static_assert(sizeof(DhdrConvertConstants) == sizeof(NativeConvertConstants), "DhdrConvertConstants");
_Static_assert(offsetof(DhdrConvertConstants, tex) == offsetof(NativeConvertConstants, tex) &&
               offsetof(DhdrConvertConstants, size) == offsetof(NativeConvertConstants, size) &&
               offsetof(DhdrConvertConstants, slot) == offsetof(NativeConvertConstants, slot) &&
               offsetof(DhdrConvertConstants, pages) == offsetof(NativeConvertConstants, pages) &&
               offsetof(DhdrConvertConstants, palette) == offsetof(NativeConvertConstants, palette),
               "DhdrConvertConstants members");
_Static_assert(sizeof(NativeRect) == 4 * sizeof(int32_t), "NativeRect is the scissor's four int32");
_Static_assert((int)NATIVE_DRAW == (int)DHDR_CMD_DRAW && (int)NATIVE_COPY == (int)DHDR_CMD_COPY &&
               (int)NATIVE_IMPORT == (int)DHDR_CMD_IMPORT && (int)NATIVE_ALPHA_UPLOAD == (int)DHDR_CMD_ALPHA_UPLOAD,
               "command kinds");
_Static_assert(NATIVE_SURFACES == 48, "dhdr_native_surface takes indices below 48");
_Static_assert(sizeof(((NativeFrame *)0)->display) == sizeof(((DhdrPicture *)0)->display), "display constants");

typedef struct NativeSnapshot {
    DhdrCommand *commands;
    unsigned command_capacity;
    DhdrConvert *converts;
    unsigned convert_capacity;
    DhdrSurfaceInfo surfaces[NATIVE_SURFACES];
    DhdrPicture picture;
} NativeSnapshot;

static DhdrTexture *native_snapshot_texture(const NativeTexture *t)
{
    return t ? NATIVE_SNAPSHOT_TEXTURE(t) : NULL;
}

/* One command, field by field (the order differs and the pointers change type); pads zero. */
static void native_snapshot_command(DhdrCommand *d, const NativeCommand *k)
{
    memset(d, 0, sizeof *d);
    d->kind = k->kind; d->depth_key = k->depth_key; d->color_mask = k->color_mask;
    d->target = k->target; d->depth = k->depth; d->texture_surface = k->texture_surface; d->convert = k->convert;
    d->blend_key = k->blend_key; d->blend_fix = k->blend_fix;
    d->scissor[0] = k->scissor.left; d->scissor[1] = k->scissor.top;
    d->scissor[2] = k->scissor.right; d->scissor[3] = k->scissor.bottom;
    memcpy(&d->constants, &k->constants, sizeof d->constants);
    d->first = k->first; d->count = k->count;
    d->source = k->source;
    d->x = k->x; d->y = k->y; d->w = k->w; d->h = k->h; d->sx = k->sx; d->sy = k->sy;
    d->texture = native_snapshot_texture(k->texture);
    d->replacement = k->replacement ? NATIVE_SNAPSHOT_VIEW(k->replacement) : NULL;
    d->pixels = k->kind == NATIVE_IMPORT || k->kind == NATIVE_ALPHA_UPLOAD ? k->pixels : NULL;
}

/* The picture of frame f drawn with these vertices (f's own, or DHD_INTERP's mix of f->vertex_count). */
static const DhdrPicture *native_snapshot(NativeSnapshot *s, const Native *n, const NativeFrame *f,
                                          const NativeVertex *vertices)
{
    if (f->command_count > s->command_capacity) {
        DhdrCommand *grown = realloc(s->commands, (size_t)f->command_count * sizeof *grown);
        if (!grown) native_oom("picture commands");
        s->commands = grown; s->command_capacity = f->command_count;
    }
    if (f->convert_count > s->convert_capacity) {
        DhdrConvert *grown = realloc(s->converts, (size_t)f->convert_count * sizeof *grown);
        if (!grown) native_oom("picture conversions");
        s->converts = grown; s->convert_capacity = f->convert_count;
    }
    for (unsigned i = 0; i < f->command_count; i++) native_snapshot_command(&s->commands[i], &f->commands[i]);
    for (unsigned i = 0; i < f->convert_count; i++) {
        const NativeConvert *v = &f->converts[i];
        DhdrConvert *d = &s->converts[i];
        memset(d, 0, sizeof *d);
        memcpy(d->surface, v->surface, sizeof d->surface);
        memcpy(&d->constants, &v->constants, sizeof d->constants);
        d->base = native_snapshot_texture(v->base);
    }
    unsigned surfaces = (unsigned)n->surface_count;
    if (surfaces > NATIVE_SURFACES) surfaces = NATIVE_SURFACES;
    for (unsigned i = 0; i < surfaces; i++) {
        const NativeSurface *from = &n->surfaces[i];
        DhdrSurfaceInfo *d = &s->surfaces[i];
        d->page = from->page; d->bw = from->bw; d->wide = from->wide; d->depth = from->depth; d->zpsm = from->zpsm;
        d->scale = from->scale;
        d->size[0] = from->size[0]; d->size[1] = from->size[1];
        d->extent[0] = (uint32_t)from->extent[0]; d->extent[1] = (uint32_t)from->extent[1];
    }
    DhdrPicture *p = &s->picture;
    memset(p, 0, sizeof *p);
    p->size = sizeof *p;
    p->vertex_count = f->vertex_count;
    p->command_count = f->command_count;
    p->convert_count = f->convert_count;
    p->surface_count = surfaces;
    p->display_surface = f->display_surface;
    memcpy(p->display, f->display, sizeof p->display);
    p->movie_on = f->movie_on ? 1 : 0;
    p->movie_width = p->movie_on ? f->movie_width : 0;
    p->movie_height = p->movie_on ? f->movie_height : 0;
    p->scale = n->scale;
    p->vertices = (const DhdrVertex *)vertices;
    p->commands = s->commands;
    p->converts = s->converts;
    p->surfaces = s->surfaces;
    p->movie = p->movie_on ? f->movie : NULL;
    return p;
}

static void native_snapshot_free(NativeSnapshot *s)
{
    free(s->commands);
    free(s->converts);
    memset(s, 0, sizeof *s);
}
