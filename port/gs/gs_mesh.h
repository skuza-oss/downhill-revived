/* A translated VU1 mesh batch (finding 164): what the VU thread hands the GS front end instead of
   the GIF packet a VU1 world-mesh batch (overlay B's loop 0170, MSCAL 0078) would kick. The
   native renderer draws it with a vertex shader doing the loop's maths (native.wgsl's
   native_mesh_vertex): transform, perspective, fog, colour scale, texture coordinates times Q;
   GPU culling and clipping replace the VU's back-face flags and its clipper.

   One item of the GS queue (path GS_PATH_MESH): the header, `vertices` GsMeshVertex, then
   `indices` 16-bit indices into them (the triangles the strip draws, each in the winding the
   back-face cull wants), padded to a whole word. The vertex is NativeVertex-shaped (32 bytes), so
   the GS thread copies it as it is into the field's vertex array. */
#ifndef PORT_GS_MESH_H
#define PORT_GS_MESH_H
#include <stdint.h>

enum {
    GS_MESH_MAGIC = 0x3170564Du,   /* "MVp1" */
    GS_MESH_SHADOW = 1,            /* the VU program ran too and kicked its packet (exact Z probe): no register writes */
    GS_MESH_TEXTURE = 2,           /* ad[] holds the batch's A+D texture qwords (TEX0, TEX1, MIPTBP1, TEXFLUSH) */
    GS_MESH_TWO_SIDED = 4,         /* no back-face culling (the loop draws both sides) */
    GS_MESH_MAX_VERTICES = 64
};

typedef struct {
    float x, y, z;                 /* object-space position (the batch's entry in the object's pool) */
    float u, v, uz;                /* texture coordinates (ITOF12, the object's UV offset added past vertex 0), Q factor */
    float unused;                  /* the GS thread writes the object's record there (native_mesh, finding 168) */
    uint32_t rgba;                 /* the V4-5 colour as unpacked: r << 3, g << 3, b << 3, a << 7 in bytes */
} GsMeshVertex;

typedef struct {
    uint32_t magic, flags, vertices, indices;
    uint64_t prim;                 /* PRIM, as the strip's GIF tag writes it */
    uint32_t pad[2];
    uint32_t ad[4][4];             /* GS_MESH_TEXTURE: the four A+D qwords the batch kicks first */
    float matrix[16];              /* columns: object x view-projection, z scaled (VU vf1..vf4) */
    float clip[4];                 /* the guard band: x, y and w offsets, bound (VU data 996) */
    float color[4];                /* colour scale (VU data 5) */
    float fog[4];                  /* fog = clamp(fog[0] + fog[1] / w, 0, fog[2]) */
} GsMeshHeader;

/* The setting (REVIVED SETTINGS' Native VU, dhd.ini native_vu, DHD_NATIVE_VU1; finding 194) as one
   word: the translated loops, a bit each in vu1_mesh.inc's mesh_programs order (GS_MESH_LOOP_NAMES),
   and its test flags. "on" (or 1, true, yes, all) is every loop; "off" (0, false, no, empty) none;
   otherwise the loops by address or group (prelit, lit, shadow, all), comma separated, then flags:
   ":skip" or ":shadow" force a mode, ":gpuclip" draws the batches the VU would clip too. */
#define GS_MESH_LOOP_NAMES "0170", "1708", "08B0", "0798", "18B0", "1110", "13A0", "1510", "3140", "0AA0", "0C30", "2DE8"
enum {
    GS_MESH_LOOPS = 12,
    GS_MESH_PRELIT = 0x1F, GS_MESH_LIT = 0x6E0, GS_MESH_SHADOW_LOOPS = 0x100, GS_MESH_ALL = (1 << GS_MESH_LOOPS) - 1,
    GS_MESH_FORCE_SKIP = 1 << 16, GS_MESH_FORCE_SHADOW = 1 << 17, GS_MESH_GPU_CLIP = 1 << 18
};
unsigned gs_mesh_parse(const char *value);
/* The setting as the game thread last read it (gs_mesh_setting_read: at the native renderer's start
   and when REVIVED SETTINGS changes a picture option, gs_present_options_apply; in the browser the
   GS worker reads it, gpu_rust_web.c), for the VU thread, which reads no environment; -1 (or 0)
   until it was read. Atomic loads and stores (any thread).
   The threaded browser build (DHD_WEB_THREADS) runs the GS and the VU in two wasm modules on one
   shared memory, so this word and gs.h's gs_mesh_mode are at a fixed place both modules know
   (port/web/web_queue.h's first control block) instead of a variable of one of them. */
#if defined(DHD_WEB_THREADS)
#include "web_queue.h"   /* -Iport/web */
#define gs_mesh_setting (WQ->mesh_setting)
#define gs_mesh_mode (WQ->mesh_mode)
#else
extern int gs_mesh_setting;
#endif
void gs_mesh_setting_read(void);

#endif
