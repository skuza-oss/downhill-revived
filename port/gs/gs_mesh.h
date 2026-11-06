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
    GS_MESH_MAX_VERTICES = 64
};

typedef struct {
    float x, y, z;                 /* object-space position (the batch's entry in the object's pool) */
    float u, v, uz;                /* texture coordinates (ITOF12, the object's UV offset added past vertex 0), Q factor */
    float unused;
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

#endif
