/* Native VU1 meshes (findings 164, 167, 194 and 198), included by vif.c: REVIVED SETTINGS' Native VU
   (the players' default; dhd.ini native_vu, DHD_NATIVE_VU1) hands overlay B's batch loops to the native
   renderer instead of running them. "on" is every loop below; for tests DHD_NATIVE_VU1 also lists
   loops (by their address; "prelit", "lit", "shadow" or "all" for groups), then optional flags:
   DHD_NATIVE_VU1=0170,1708:skip (gs_mesh.h's gs_mesh_parse).

   | loop | VI13  | setup      | fog      | colour                      | ST                      | back faces | clipping |
   | 0170 | 0x011 | 0078       | computed | c vf31                      | UV + vf19 from vertex 1 | culled     | yes      |
   | 1708 | 0x2C9 | 1638       | computed | c vf31                      | UV + vf19               | drawn      | yes      |
   | 08B0 | 0x100 | 07F0       | computed | c vf31                      | UV + vf19               | drawn      | no       |
   | 0798 | 0x0E0 | 06F0       | maximum  | c                           | UV                      | drawn      | no       |
   | 18B0 | 0x2FE | 17E0       | maximum  | c                           | UV                      | drawn      | yes      |
   | 1110 | 0x20C | 1050 *     | computed | c d5 L + d20 L.w^2 + d1015  | UV + vf19               | drawn      | yes      |
   | 13A0 | 0x260 | 12F0 *     | maximum  | c L + d20 L.w^2 + d1015     | UV                      | drawn      | no       |
   | 1510 | 0x288 | 1430       | computed | c vf31 + d20 max(N.H, 0)^16 | UV + vf19               | drawn      | yes      |
   | 0AA0 | 0x154 | 0A90       | maximum  | c                           | environment map         | drawn      | no       |
   | 0C30 | 0x186 | 0C20       | computed | c vf31                      | environment map         | drawn      | yes      |
   | 2DE8 | 0x5BD | 2DC8       | maximum  | 128, alpha lit              | projected (d5-d8)       | drawn      | yes      |
   | 3140 | 0x616 | 30A0 *     | maximum  | d1013                       | (untextured)            | drawn      | no       |

   (* and the batches after the lit and skinned objects' passes, 0040, 0010 and 0020; c the batch
   vertex's colour, L its pool entry's lighting at data 373 + index, N its pool
   normal there (12-bit fixed point), H = (d6.w, d7.w, d8.w), dN data N; the lit colours clamped
   to 255 and truncated, their alpha c.a d5.a + d20.a L.w^2, d1015 adding to RGB only. UV is the
   batch's (u, v, uz), STQ = UV q. The environment map (0AA0, 0C30): S, T = E N + (x q + d997.zw)
   d998.zw with E the 2 x 3 matrix d997-d999.xy times the object's rotation d0-d2, and Q = 1:
   screen-linear, so the item carries (S w, T w, w), which the shader's divide by w turns back.
   2DE8 projects a second texture: STQ = (d5-d8) (x y z 1) q (setup 2FD8 stores d5-d8 = data
   1008-1011 times the object's matrix), RGB the float 1.0000153's low byte (128), alpha d1013.w's
   low byte where N.d1013 > 0 (its FSAND at 2F30 sees 2F10's MAC: vu1_native_2dc8.h), else 0.
   3140 draws the shadow silhouettes into the shadow targets (setup 30A0: the matrix data 1008-1011
   times the object's, no divide; translated while that leaves w at 1, which the vertex shader's
   divide then keeps). 2DE8 and 3140 make their own strip tag: the loop writes the I register's
   constant (LOI at 0x2E98: PRIM 05C; at 0x30C0: PRIM 00C) over the header's second word.)

   A batch loop runs after a program's setup has set VI13 and the registers it reads: the shared
   1AE0 (world objects: 0078, 1638, 07F0, 06F0, 17E0; the lit setups 1050, 12F0, 1430, 0A90, 0C20;
   2DC8 after its own 2FD8), 30A0's 3190, and for lit or skinned objects a pass over the object's
   pool (MSCAL 0040's lighting, 0010/0020's skinning and lighting) that writes positions at 181 +
   index and lighting at 373 + index. Those stay on the VU (the item's maths needs the pool's
   positions on the CPU).
   Each batch (MSCNT at 1CB0, 1040, 12E0, 0590, 06E0 or 3258, VI13 a translated loop's) is read from VU1 data
   memory and the registers as they are and goes to the GS queue as one mesh item (gs_mesh.h): the
   pool positions, colours and texture coordinates of its strip vertices, the triangles the strip
   draws (the input's ADC joins left out; with back-face culling each in the winding the program's
   test keeps), the A+D texture writes and PRIM its packet would make, and the constants. The
   batch's microprogram and its XGKICK do not run; what the next run reads is left as the batch
   leaves it (the rotating buffer in VI14/VI2, VI3, VI4, TPC 1040; 0170's strip parity in vf6.x and
   at 1014; the registers the loops load with constants and keep, mesh_skipped). 0590's delay slot,
   the skinning's last pool store, is made first.

   The batch, as the microcode does it (vudis.py; 0170: 0x88-0x268, the others alike, and 0FA8):
   header qword at VI2 (the strip's GIF tag: NLOOP n with EOP, PRE, PRIM, PACKED ST RGBAQ XYZF2; w:
   texture triplet offset t in bits 0-14, bit 15 the strip's parity), then per vertex three qwords:
   the pool index (S-8), the colour (V4-5), the texture coordinates (V2-16 as 12-bit fixed point; z
   the Q factor, w 2048.0 at a strip join, which the fog's FTOI4 turns into ADC). A vertex k is
   clip = C (x y z 1) with C = vf1..vf4; q = 1 / clip.w; XYZ = FTOI4(clip.xyz q); fog = clamp(vf19.w
   + vf18.x + vf18.w q, 0, vf18.y), or vf18.y itself; colour as the table says, clamped to 255 and
   truncated; STQ = (u, v, uz) q with vf19.xy added to u, v as the table says. 0170's ADC also
   marks back faces: with P = 1 when the header's bit 15 is set, else -1 (its prologue sets vf6.x
   to 1, then negates it), triangle (k-2, k-1, k) is culled when P (-1)^k times its screen area (y
   down) is positive. CLIP against vf13 (data 996) sends a triangle to the clipper (1948) when any
   vertex is off the guard band in x or y or below the near bound; the loops without clipping
   kick such a vertex as it comes. A GPU clips the sideways ones as the clipper does, but not those
   that cross the near bound (w 1: the clipper's fans differ over the near ground in fast race
   frames; finding 164): a batch with such a vertex has its flagged triangles clipped here as the
   clipper clips them and the item carries their fans (mesh_clip_triangle, finding 253). The GS wraps the unclipped loops' ones: a
   batch of such a loop with a vertex off the guard band runs on the VU.

   Modes: skip (the batch's program does not run), or shadow, the default while DHD_EXACT_PROBE
   answers the lens flare's Z probe from the GS stream: the program runs too and its packet is
   kicked for the exact path only (the native renderer draws the mesh item instead), so game
   state is the switch-off run's. :skip or :shadow forces one. DHD_NATIVE_VU1_CHECK=1 (shadow)
   compares each item, through the shader's maths, with the packet the program wrote (mesh_check).
   Only with the native renderer and indexed fields (gs_mesh_mode). The browser runs the same code
   (finding 199): its VU worker (vu.wasm) and GS worker (graphics.wasm) share gs_mesh_setting and
   gs_mesh_mode at a fixed place in their memory (gs_mesh.h), and its items reach the GS worker
   through the queue's WQ_GIF packets (port/web/web_queue.c). */
#include "../gs/gs_mesh.h"
#include "../host/host_api.h"

enum { MESH_OFF = 1, MESH_ON = 2, MESH_SKIP = 1, MESH_SHADOW = 2 };
enum { MESH_WHY_EOP, MESH_WHY_COUNT, MESH_WHY_TAG, MESH_WHY_PRIM, MESH_WHY_TEXTURE, MESH_WHY_COLOUR, MESH_WHY_JOIN,
       MESH_WHY_NEAR, MESH_WHY_CLIP, MESH_WHY_ORTHO, MESH_WHY_FAN_NEAR, MESH_WHY_FAN_FOG, MESH_WHY_FAN_MANY,
       MESH_WHYS };
static const char *const mesh_why_names[MESH_WHYS] = {"no EOP", "count", "tag", "prim", "texture tag", "colour",
                                                     "join value", "a vertex below the near bound",
                                                     "a vertex off the guard band in x or y",
                                                     "w other than 1 (no divide)",
                                                     "a vertex of the clipper's fan behind the eye",
                                                     "the clipper's fog is not the loop's",
                                                     "the clipper's polygon outgrows its buffer"};

/* The translated loops (the table above). colour: c vf31, c, lit with d5, lit, the highlight, d1013,
   2DE8's; st: UV, UV + vf19 from vertex 1, UV + vf19, the environment map, 2DE8's projection. */
enum { MESH_COLOUR_SCALED, MESH_COLOUR_RAW, MESH_COLOUR_LIT_D5, MESH_COLOUR_LIT, MESH_COLOUR_HIGHLIGHT,
       MESH_COLOUR_CONSTANT, MESH_COLOUR_LIGHT_ALPHA };
enum { MESH_ST_UV, MESH_ST_OFFSET_AFTER_FIRST, MESH_ST_OFFSET, MESH_ST_ENVIRONMENT, MESH_ST_PROJECTED };
typedef struct {
    const char *name;
    uint16_t vi13;
    uint8_t fog_max, colour, st, backface, clip;
    uint8_t ortho;      /* 3140: no divide */
    uint16_t tag_loi;   /* the LOI whose I the loop stores as the tag's second word (2DE8, 3140), or 0 */
} MeshProgram;
static const MeshProgram mesh_programs[] = {
    {"0170", 0x011, 0, MESH_COLOUR_SCALED, MESH_ST_OFFSET_AFTER_FIRST, 1, 1},
    {"1708", 0x2C9, 0, MESH_COLOUR_SCALED, MESH_ST_OFFSET, 0, 1},
    {"08B0", 0x100, 0, MESH_COLOUR_SCALED, MESH_ST_OFFSET, 0, 0},
    {"0798", 0x0E0, 1, MESH_COLOUR_RAW, MESH_ST_UV, 0, 0},
    {"18B0", 0x2FE, 1, MESH_COLOUR_RAW, MESH_ST_UV, 0, 1},
    {"1110", 0x20C, 0, MESH_COLOUR_LIT_D5, MESH_ST_OFFSET, 0, 1},
    {"13A0", 0x260, 1, MESH_COLOUR_LIT, MESH_ST_UV, 0, 0},
    {"1510", 0x288, 0, MESH_COLOUR_HIGHLIGHT, MESH_ST_OFFSET, 0, 1},
    {"3140", 0x616, 1, MESH_COLOUR_CONSTANT, MESH_ST_UV, 0, 0, 1, 0x30C0},
    {"0AA0", 0x154, 1, MESH_COLOUR_RAW, MESH_ST_ENVIRONMENT, 0, 0},
    {"0C30", 0x186, 0, MESH_COLOUR_SCALED, MESH_ST_ENVIRONMENT, 0, 1},
    {"2DE8", 0x5BD, 1, MESH_COLOUR_LIGHT_ALPHA, MESH_ST_PROJECTED, 0, 1, 0, 0x2E98},
};
enum { MESH_PROGRAMS = sizeof mesh_programs / sizeof *mesh_programs };
_Static_assert(MESH_PROGRAMS == GS_MESH_LOOPS, "mesh_programs: one bit each in the setting, in GS_MESH_LOOP_NAMES' order");

typedef struct {
    uint64_t batches, fallbacks, vertices, triangles, joins, why[MESH_WHYS];
    uint64_t near_batches, clip_triangles, fans, fan_vertices;   /* the near bound's batches, clipped here (mesh_clip) */
    double timed_seconds; uint64_t timed_batches, timed_vertices;   /* DHD_VU1_PROFILE (mesh_timed) */
    /* DHD_NATIVE_VU1_CHECK=1 (shadow mode): the items against what the program wrote (mesh_check) */
    uint64_t checked, inside, bad_xy, bad_z, bad_fog, bad_colour, bad_stq, triangles_checked, clipped, bad_adc,
             backfaces;
    uint64_t fans_checked, fans_bad_count, fans_missing, fan_checked, fan_bad_xyzf, fan_near_xyzf, fan_bad_rgba, fan_bad_stq, fan_far_stq;
} MeshCounts;

static struct {
    int state;                 /* 0 unread, MESH_OFF, MESH_ON */
    int mode;                  /* MESH_SKIP or MESH_SHADOW */
    unsigned enabled;          /* the translated loops, a bit each (mesh_programs) */
    int object;                /* the batch's loop (mesh_programs), or -1 */
    uint64_t code_version; int overlay_b;
    MeshCounts count[MESH_PROGRAMS];
    /* the queue item: header, vertices, indices */
    uint32_t packet[(sizeof(GsMeshHeader) + GS_MESH_MAX_ITEM_VERTICES * sizeof(GsMeshVertex) +
                     GS_MESH_MAX_VERTICES * (GS_MESH_FAN_VERTICES - 1) * 3 * 2 + 4) / 4];
    unsigned words;
    unsigned strip;            /* the item's strip vertices (the fans' follow) */
    /* DHD_NATIVE_VU1_CHECK=1 (shadow mode): each item against what the program wrote (mesh_check) */
    int check;
    unsigned check_base; float check_parity; uint8_t join[GS_MESH_MAX_VERTICES];
    /* and each fan of the batch as the clipper kicks it (count, then per vertex STQ, RGBA, XYZF2), against
       the program's own kicks (mesh_kicked) */
    unsigned fans, fans_seen;
    /* the colour words as the loop stores them, for its clipper: 2DE8's (floats) and the lit loops'
       (1110, 13A0: integers, negative where the light takes more than the vertex has) */
    uint32_t words_stored[GS_MESH_MAX_VERTICES][4];
    struct { unsigned count; uint32_t q[GS_MESH_FAN_VERTICES][3][4]; } fan[GS_MESH_MAX_VERTICES];
} mesh = {.object = -1};

/* A skipped batch's time on the VU thread, from the run's start to the item queued (profiled runs). */
static void mesh_timed(double seconds)
{
    if (mesh.object < 0) return;
    MeshCounts *c = &mesh.count[mesh.object];
    c->timed_seconds += seconds;
    c->timed_batches++;
    c->timed_vertices += mesh.strip;
}

static void mesh_report(void)
{
    for (unsigned p = 0; p < MESH_PROGRAMS; p++) {
        const MeshCounts *c = &mesh.count[p];
        if (!(mesh.enabled >> p & 1)) continue;
        hle_log("native VU1 %s (%s): %llu batches translated, %llu run on the VU; %llu vertices, "
                "%llu triangles, %llu join vertices\n", mesh_programs[p].name, mesh.mode == MESH_SHADOW ? "shadow" : "skip",
                (unsigned long long)c->batches, (unsigned long long)c->fallbacks,
                (unsigned long long)c->vertices, (unsigned long long)c->triangles, (unsigned long long)c->joins);
        if (c->near_batches)
            hle_log("native VU1 %s: %llu batches cross the near bound: %llu triangles through the clipper, %llu fans, "
                    "%llu fan vertices\n", mesh_programs[p].name, (unsigned long long)c->near_batches,
                    (unsigned long long)c->clip_triangles, (unsigned long long)c->fans, (unsigned long long)c->fan_vertices);
        for (unsigned i = 0; i < MESH_WHYS; i++)
            if (c->why[i])
                hle_log("native VU1 %s: %llu batches on the VU: %s\n", mesh_programs[p].name, (unsigned long long)c->why[i],
                        mesh_why_names[i]);
        if (c->timed_batches)
            hle_log("native VU1 %s cost (DHD_VU1_PROFILE): %.2f us a translated batch, %.1f ns a vertex (%llu batches)\n",
                    mesh_programs[p].name, c->timed_seconds * 1e6 / (double)c->timed_batches,
                    c->timed_seconds * 1e9 / (double)(c->timed_vertices ? c->timed_vertices : 1),
                    (unsigned long long)c->timed_batches);
        if (mesh.check)
            hle_log("native VU1 %s check: %llu vertices (%llu inside the guard band): XY off by more than 1/16 px %llu, "
                    "Z by more than 1 %llu, fog %llu, colour %llu, STQ %llu; %llu triangles (%llu back faces), ADC "
                    "differs %llu; %llu triangles left to the clipper\n", mesh_programs[p].name,
                    (unsigned long long)c->checked, (unsigned long long)c->inside, (unsigned long long)c->bad_xy,
                    (unsigned long long)c->bad_z, (unsigned long long)c->bad_fog, (unsigned long long)c->bad_colour,
                    (unsigned long long)c->bad_stq, (unsigned long long)c->triangles_checked,
                    (unsigned long long)c->backfaces, (unsigned long long)c->bad_adc, (unsigned long long)c->clipped);
        if (mesh.check && (c->fans_checked || c->fans_missing))
            hle_log("native VU1 %s check: %llu fans the clipper kicked: %llu not made here or not kicked, %llu with "
                    "another count; %llu fan vertices: XYZF2 differs %llu (%llu by one step), RGBA %llu, STQ %llu (%llu by "
                    "more than 1e-4)\n",
                    mesh_programs[p].name, (unsigned long long)c->fans_checked, (unsigned long long)c->fans_missing,
                    (unsigned long long)c->fans_bad_count, (unsigned long long)c->fan_checked,
                    (unsigned long long)c->fan_bad_xyzf, (unsigned long long)c->fan_near_xyzf,
                    (unsigned long long)c->fan_bad_rgba, (unsigned long long)c->fan_bad_stq,
                    (unsigned long long)c->fan_far_stq);
    }
}

/* DHD_NATIVE_VU1_CHECK: after a shadow batch ran on the VU, its item's vertices through the
   shader's maths (native_mesh_vertex, in C) against the GIF packet the program wrote in place:
   per vertex STQ, RGBA, XYZF2 (qwords 0-2), and per triangle the ADC bit against the joins and
   (0170) the back-face rule. Vertices off the guard band and triangles the clipper took are
   counted apart. */
static void mesh_check(void)
{
    const MeshProgram *prog = &mesh_programs[mesh.object];
    MeshCounts *count = &mesh.count[mesh.object];
    const GsMeshHeader *h = (const GsMeshHeader *)mesh.packet;
    const GsMeshVertex *v = (const GsMeshVertex *)(h + 1);
    const ps2_q128 *m = vu1.vu0_mem;
    unsigned n = mesh.strip;
    float sx[GS_MESH_MAX_VERTICES], sy[GS_MESH_MAX_VERTICES];
    int in[GS_MESH_MAX_VERTICES];
    for (unsigned k = 0; k < n; k++) {
        const ps2_q128 *o = &m[(mesh.check_base + 1 + 3 * k) & 1023];
        const float c[4] = {v[k].x, v[k].y, v[k].z, v[k].w};
        float q = 1.0f / c[3], x = c[0] * q, y = c[1] * q, z = c[2] * q;
        float fog = h->fog[0] + h->fog[1] * q;
        fog = fog > h->fog[2] ? h->fog[2] : fog;
        fog = fog < 0 ? 0 : fog;
        sx[k] = x; sy[k] = y;
        float bound = h->clip[3] < 0 ? -h->clip[3] : h->clip[3];
        in[k] = c[3] > 0 && x + h->clip[0] <= bound && x + h->clip[0] >= -bound && y + h->clip[1] <= bound &&
                y + h->clip[1] >= -bound && c[3] + h->clip[2] >= -bound;
        count->checked++;
        if (!in[k]) continue;
        count->inside++;
        int dx = (int16_t)(uint16_t)((o[2].w[0] & 0xFFFF) - ((uint32_t)(int32_t)(x * 16.0f) & 0xFFFF));
        int dy = (int16_t)(uint16_t)((o[2].w[1] & 0xFFFF) - ((uint32_t)(int32_t)(y * 16.0f) & 0xFFFF));
        if (dx > 1 || dx < -1 || dy > 1 || dy < -1) count->bad_xy++;
        float z16 = z * 16.0f;   /* FTOI4 saturates (3140's shadow pass leaves z unscaled) */
        int32_t zi = z16 >= 2147483648.0f ? INT32_MAX : z16 <= -2147483648.0f ? INT32_MIN : (int32_t)z16;
        int dz = (o[2].sw[2] >> 4) - (zi >> 4);
        if (dz > 1 || dz < -1) count->bad_z++;
        int df = (int)(o[2].w[3] >> 4 & 255) - (int)fog;
        if (df > 1 || df < -1) count->bad_fog++;
        const uint8_t *rgba = (const uint8_t *)&v[k].rgba;
        for (unsigned i = 0; i < 4; i++) {
            int ref = rgba[i];
            /* the GS takes the low byte: of 2DE8's floats, of a lit loop's negative word */
            const int low = prog->colour == MESH_COLOUR_LIGHT_ALPHA || o[1].sw[i] < 0;
            int got = low ? (int)(o[1].w[i] & 255) : o[1].sw[i];
            if (got - ref > 1 || ref - got > 1) { count->bad_colour++; break; }
        }
        const float st[3] = {v[k].u * q, v[k].v * q, v[k].uz * q};
        for (unsigned i = 0; i < 3 && (h->prim & 16); i++) {   /* untextured: ST is not written */
            float d = o[0].f[i] - st[i], scale = (st[i] < 0 ? -st[i] : st[i]) + 1e-3f;
            if (d > 1e-4f * scale || d < -1e-4f * scale) { count->bad_stq++; break; }
        }
    }
    for (unsigned k = 2; k < n; k++) {
        if (!in[k] || !in[k - 1] || !in[k - 2]) { count->clipped++; continue; }
        float area = (sx[k - 1] - sx[k - 2]) * (sy[k] - sy[k - 1]) - (sy[k - 1] - sy[k - 2]) * (sx[k] - sx[k - 1]);
        float s = k & 1 ? -mesh.check_parity : mesh.check_parity;
        int back = prog->backface && s * area > 0, adc = (m[(mesh.check_base + 3 + 3 * k) & 1023].w[3] & 0x8000) != 0;
        count->triangles_checked++;
        count->backfaces += back && !mesh.join[k];
        if (adc != (mesh.join[k] || back)) count->bad_adc++;
    }
}

/* DHD_NATIVE_VU1_CHECK: a kick of the program while a shadow batch runs. The clipper kicks each
   fan from data 0x6D (after the texture's A+D qwords when the batch has them): its count and its
   vertices' STQ, RGBA and XYZF2 words against the fan mesh_build made, bit for bit. */
static void mesh_kicked(unsigned qword)
{
    if (mesh.object < 0 || qword != 0x6D) return;
    MeshCounts *count = &mesh.count[mesh.object];
    const ps2_q128 *m = vu1.vu0_mem;
    unsigned at = qword;
    if (!(m[at].w[0] & 0x8000)) at += (m[at].w[0] & 0x7FFF) + 1;
    unsigned n = m[at].w[0] & 0x7FFF;
    count->fans_checked++;
    if (mesh.fans_seen >= mesh.fans) { mesh.fans_seen++; count->fans_missing++; return; }
    unsigned f = mesh.fans_seen++;
    if (mesh.fan[f].count != n || n > GS_MESH_FAN_VERTICES) { count->fans_bad_count++; return; }
    for (unsigned j = 0; j < n; j++) {
        const ps2_q128 *q = &m[(at + 1 + 3 * j) & 1023];
        uint32_t (*e)[4] = mesh.fan[f].q[j];
        count->fan_checked++;
        if (memcmp(q[0].w, e[0], 12)) {
            count->fan_bad_stq++;
            for (unsigned a = 0; a < 3; a++) {
                float mine, d, scale;
                memcpy(&mine, &e[0][a], 4);
                d = q[0].f[a] - mine;
                scale = 1e-4f * ((mine < 0 ? -mine : mine) + 1e-3f);
                if (!(d <= scale && d >= -scale)) { count->fan_far_stq++; break; }
            }
        }
        if (memcmp(q[1].w, e[1], 16)) count->fan_bad_rgba++;
        if (memcmp(q[2].w, e[2], 16)) {
            int nearly = 1;
            for (unsigned a = 0; a < 4; a++) {
                int64_t d = (int64_t)q[2].sw[a] - (int32_t)e[2][a];
                nearly &= d >= -1 && d <= 1;
            }
            count->fan_bad_xyzf++;
            count->fan_near_xyzf += (unsigned)nearly;
        }
    }
}

/* Whether this VU1 run's batch may be translated: the setting (Native VU, gs_mesh_setting: read on
   the game thread at the start and whenever REVIVED SETTINGS changes it, so it applies live) and
   the renderer (gs_mesh_mode: none, or one that refused the mesh draws, finding 194). Asked before
   every run; a change takes effect at the next batch. Batches translated and batches run on the VU
   mix freely (a skipped batch leaves the VU as its program would; the near-bound batches have
   always run on the VU between translated ones), so the switch can turn at any batch.
   The renderer is made at the first vsync (present.c), which can come after the first VU1 runs:
   until then the translation waits. A forced mode (":skip", ":shadow") runs without a mesh
   renderer too (the state trace's headless runs have none): the items are then dropped, and only
   what the translation does to game state is seen. One log line at each change. */
static int mesh_enabled(void)
{
    static int applied = -1, applied_mode = -1, reported, no_renderer;
    int setting = __atomic_load_n(&gs_mesh_setting, __ATOMIC_ACQUIRE);
    int mode = __atomic_load_n(&gs_mesh_mode, __ATOMIC_RELAXED);
    if (setting < 0) setting = 0;   /* not read yet (host_settings_load reads it before the game runs) */
    unsigned loops = (unsigned)setting & GS_MESH_ALL;
    int forced = setting & GS_MESH_FORCE_SKIP ? MESH_SKIP : setting & GS_MESH_FORCE_SHADOW ? MESH_SHADOW : 0;
    int on = loops && (mode || forced);
    if (setting == applied && (on ? mode : 0) == applied_mode) return mesh.state == MESH_ON;
    if (loops && !on && kernel_vsync_count < 600) return 0;   /* the renderer may still come */
    int was_on = mesh.state == MESH_ON;
    applied = setting;
    applied_mode = on ? mode : 0;
    if (!on) {
        mesh.state = MESH_OFF;
        if (loops && !no_renderer++)
            hle_log("native VU1: Native VU needs the native renderer with indexed fields (or the GPU refused its "
                    "draws); the VU runs every program\n");
        else if (was_on) hle_log("native VU1: off; the VU runs every program\n");
        return 0;
    }
    mesh.enabled = loops;
    mesh.mode = forced ? forced : mode == 2 ? MESH_SHADOW : MESH_SKIP;
    mesh.state = MESH_ON;
    mesh.check = mesh.mode == MESH_SHADOW && getenv("DHD_NATIVE_VU1_CHECK") != NULL;
    char names[5 * MESH_PROGRAMS + 1] = "";
    for (unsigned p = 0; p < MESH_PROGRAMS; p++)
        if (mesh.enabled >> p & 1) { strcat(names, " "); strcat(names, mesh_programs[p].name); }
    hle_log("native VU1: loops%s drawn by the native renderer, %s%s\n", names,
            mesh.mode == MESH_SHADOW ? "shadow: the VU runs them too, for the exact Z probe" : "the VU skips them",
            mode ? "" : "; no mesh renderer: the items are dropped");
    if (!reported++) host_end_add(HOST_END_REPORT, "native VU1 counters", mesh_report);
    return 1;
}

static int mesh_overlay_b(void)
{
    if (mesh.code_version != ps2_vu1_code_version || !mesh.code_version) {
        mesh.code_version = ps2_vu1_code_version;
        mesh.overlay_b = !memcmp(micro(1), GUEST(PS2_RAM_BASE + 0x269C60), 32);
    }
    return mesh.overlay_b;
}

static int mesh_fail(unsigned why)
{
    mesh.count[mesh.object].why[why]++;
    mesh.count[mesh.object].fallbacks++;
    return 0;
}

/* The environment map's 2 x 3 matrix E: d997-d999.xy times the object's rotation d0-d2 (0AA0's
   vf14-vf16.xy, 0C30's vf8, vf10, vf14 .xy). */
static void mesh_environment(float env[3][2])
{
    const ps2_q128 *m = vu1.vu0_mem;
    for (unsigned j = 0; j < 3; j++)
        for (unsigned a = 0; a < 2; a++)
            env[j][a] = m[997].f[a] * m[j].f[0] + m[998].f[a] * m[j].f[1] + m[999].f[a] * m[j].f[2];
}

/* The loops' clipper (1948, 1CC0-2738), for the triangles that cross the near bound (finding 253).
   A loop with CLIP sends a triangle whose three vertices carry any flag (off the guard band in x or
   y, below the near bound) to 1948 and sets its ADC in the strip; the sideways ones are left to the
   GPU's clipping, which draws what the clipper draws, but not the near ones (finding 164): a batch
   with a vertex below the near bound has all its flagged triangles clipped here, by the program's
   own steps, and the item carries the fans.

   1948 (k >= 2, no join): the three pool positions through the clip matrix data 27-30 (the setup's
   d22-25 times the object's; vertex a summed from the fourth column, b and c toward it), CLIP
   against their own w, and nothing is drawn when all three are off one plane (near, then y, then
   x). The stored STQ go back to (u, v, 1) by a divide by their own Q. 1CC0 then clips the
   triangle (positions in object space, colours as floats, u and v) plane by plane, in a second
   clip space, data 176-179 (d13-16 times the object's): the near plane first, then +y, -y, +x, -x.
   A vertex is in or out by its CLIP flag (z < -|w| for the near plane), but where an edge crosses
   is taken from other distances, w - 1 for the near plane and w -+ y, w -+ x for the sides
   (t = d0 / (d0 - d1)): the near cut therefore lands on w = 1 of that space, not on the plane
   the flags test, and may lie outside the edge. That is the game's look over the near ground and
   why a GPU's clipping draws something else there.
   The polygon left is projected again through the loop's own matrix (vf1..vf4, summed toward the
   fourth column), fogged by data 4 and 21, its Z scaled by d4.z^4 and held under 2^24, and kicked
   as a fan (the strip's tag with PRIM's fan bit and its own count) before the strip.

   Each value is made in the VU's order and rounding (mesh_before sets the mode), so the fans are
   the program's own: DHD_NATIVE_VU1_CHECK compares them bit for bit with its kicks. */
typedef struct { float p[4], c[4], t[4]; } MeshClipVertex;   /* pool position, colour, (u, v, 1) or the normal */

static inline uint32_t mesh_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

/* DIV: a zero divisor gives the largest float with the operands' sign */
static inline float mesh_div(float s, float t)
{
    if (t != 0.0f) return s / t;
    uint32_t b = ((mesh_bits(s) ^ mesh_bits(t)) & 0x80000000u) | 0x7F7FFFFFu;
    float r;
    memcpy(&r, &b, 4);
    return r;
}

/* CLIP v.xyz against |v.w|, as VU1 compares them (by their bits): +x 1, -x 2, +y 4, -y 8, +z 16, -z 32 */
static inline unsigned mesh_clip_flags(const float v[3], float w)
{
    uint32_t bound = mesh_bits(w);
    bound = bound & 0x7F800000u ? bound & 0x7FFFFFFFu : 0x007FFFFFu;
    unsigned flags = 0;
    for (unsigned i = 0; i < 3; i++) {
        uint32_t value = mesh_bits(v[i]);
        if ((int32_t)value > (int32_t)bound) flags |= 1u << (i * 2);
        if ((int32_t)(value ^ 0x80000000u) > (int32_t)bound) flags |= 2u << (i * 2);
    }
    return flags;
}

/* FTOI0 / FTOI4: toward zero, saturated by the sign */
static inline int32_t mesh_ftoi(float f, float scale)
{
    float scaled = f * scale;
    uint32_t b = mesh_bits(scaled);
    if ((b & 0x7F800000u) > 0x4EFFFFFFu) return (int32_t)b < 0 ? INT32_MIN : INT32_MAX;
    return (int32_t)scaled;
}

/* A float as the VU reads or leaves it: exponent 255 (a PC's NaN or infinity) is the largest
   number of its sign. */
static inline float mesh_vu(float f)
{
    const uint32_t b = mesh_bits(f);
    if ((b & 0x7F800000u) != 0x7F800000u) return f;
    return (int32_t)b < 0 ? -3.4028234663852886e38f : 3.4028234663852886e38f;
}

/* ((c0 x + c1 y) + c2 z) + c3: MULAx, MADDAy, MADDAz, MADDw */
static inline void mesh_transform(float o[4], const ps2_q128 *c, const float *p)
{
    for (unsigned i = 0; i < 4; i++) o[i] = ((c[0].f[i] * p[0] + c[1].f[i] * p[1]) + c[2].f[i] * p[2]) + c[3].f[i];
}

/* One plane of 1CC0 (0 the near plane, 1 +y, 2 -y, 3 +x, 4 -x) over the closed polygon in[0..n):
   the vertices left, in out; -1 when they outgrow the program's buffer of ten. */
static int mesh_clip_plane(const MeshClipVertex *in, unsigned n, MeshClipVertex *out, const ps2_q128 *space, unsigned plane)
{
    static const unsigned flag[5] = {0x20, 0x04, 0x08, 0x01, 0x02};
    float cp[4], d0 = 0, d1 = 0;
    unsigned out0 = 0, out1 = 0, count = 0;
    for (unsigned i = 0; i <= n; i++, d0 = d1, out0 = out1) {
        const MeshClipVertex *now = &in[i == n ? 0 : i];
        mesh_transform(cp, space, now->p);
        d1 = plane == 0 ? cp[3] - 1.0f : plane == 1 ? cp[3] - cp[1] : plane == 2 ? cp[3] + cp[1] :
             plane == 3 ? cp[3] - cp[0] : cp[3] + cp[0];
        out1 = mesh_clip_flags(cp, cp[3]) & flag[plane];
        if (!i) continue;
        const MeshClipVertex *before = &in[i - 1];
        if (!out0) {
            if (count == GS_MESH_FAN_VERTICES) return -1;
            out[count++] = *before;
        }
        if (!out0 != !out1) {
            if (count == GS_MESH_FAN_VERTICES) return -1;
            const float q = mesh_div(d0, d0 - d1);
            MeshClipVertex *o = &out[count++];
            for (unsigned a = 0; a < 4; a++) {
                o->p[a] = (now->p[a] - before->p[a]) * q + before->p[a];
                o->c[a] = (now->c[a] - before->c[a]) * q + before->c[a];
                o->t[a] = (now->t[a] - before->t[a]) * q + before->t[a];
            }
        }
    }
    return (int)count;
}

/* The triangle of strip vertices a, b, c (the item's vertices out[], their pool entries' indices
   pool[], the colour words the loop stored words[][4] or NULL for the item's bytes): the clipper's
   polygon in poly; its vertices, 0 when nothing is drawn, -1 for the buffer. What it carries
   beside the position and the colour: (u, v, 1) from the stored STQ, or for the environment map
   (0C30, which has its own copy of 1948 at 0E18) the pool normal. */
static int mesh_clip_triangle(const MeshProgram *prog, const GsMeshVertex *out, const uint16_t *pool,
                              const uint32_t (*words)[4], unsigned a, unsigned b, unsigned c, MeshClipVertex *poly)
{
    const ps2_q128 *m = vu1.vu0_mem;
    const unsigned at[3] = {a, b, c};
    const float *pa = m[(181 + pool[a]) & 1023].f, *pb = m[(181 + pool[b]) & 1023].f, *pc = m[(181 + pool[c]) & 1023].f;
    const float *corner[3] = {pa, pb, pc};
    float cp[3][4];
    for (unsigned i = 0; i < 4; i++)   /* a: MULAw the fourth column first */
        cp[0][i] = ((m[30].f[i] + m[27].f[i] * pa[0]) + m[28].f[i] * pa[1]) + m[29].f[i] * pa[2];
    mesh_transform(cp[1], &m[27], pb);
    mesh_transform(cp[2], &m[27], pc);
    if (mesh_clip_flags(cp[0], cp[0][3]) & mesh_clip_flags(cp[1], cp[1][3]) & mesh_clip_flags(cp[2], cp[2][3]) & 0x2F)
        return 0;
    MeshClipVertex work[2][GS_MESH_FAN_VERTICES];
    for (unsigned i = 0; i < 3; i++) {
        const GsMeshVertex *v = &out[at[i]];
        MeshClipVertex *o = &work[0][i];
        memcpy(o->p, corner[i], 16);
        /* ITOF0 of the stored colour words (2DE8 stores floats: their bits as integers) */
        for (unsigned j = 0; j < 4; j++) o->c[j] = words ? (float)(int32_t)words[at[i]][j] : (float)(v->rgba >> (j * 8) & 255);
        if (prog->st == MESH_ST_ENVIRONMENT) {
            const int32_t *normal = m[(373 + pool[at[i]]) & 1023].sw;
            for (unsigned j = 0; j < 4; j++) o->t[j] = (float)normal[j] * (1.0f / 4096.0f);
        } else {
            /* the stored STQ, (u, v, uz) q, divided by its Q again (2DE8 projects the fan's positions instead) */
            const float q = mesh_div(1.0f, v->w), stq[3] = {v->u * q, v->v * q, v->uz * q}, back = mesh_div(1.0f, stq[2]);
            o->t[0] = stq[0] * back; o->t[1] = stq[1] * back; o->t[2] = 1.0f; o->t[3] = 0;
        }
    }
    int n = 3;
    for (unsigned plane = 0; plane < 5 && n > 0; plane++)
        n = mesh_clip_plane(work[plane & 1], (unsigned)n, work[~plane & 1], &m[176], plane);
    if (n > 0) memcpy(poly, work[1], (unsigned)n * sizeof *poly);   /* five planes: the second buffer */
    return n;
}

/* The batch at VI2 as a mesh item in mesh.packet; 0 (and nothing built) for a form the
   translation does not know, which then runs on the VU. */
static int mesh_build(void)
{
    const MeshProgram *prog = &mesh_programs[mesh.object];
    MeshCounts *count = &mesh.count[mesh.object];
    const ps2_q128 *m = vu1.vu0_mem;
    unsigned base = vu1.vi[2] & 1023;
    const uint32_t *hdr = m[base].w;
    unsigned n = hdr[0] & 0x7FFF;
    if (!(hdr[0] & 0x8000)) return mesh_fail(MESH_WHY_EOP);
    if (n < 1 || n > GS_MESH_MAX_VERTICES || base + 1 + 3 * n > 1024) return mesh_fail(MESH_WHY_COUNT);
    uint32_t tag1 = hdr[1];
    if (prog->tag_loi) memcpy(&tag1, micro(1) + prog->tag_loi, 4);   /* that LOI's I, stored over the header */
    /* PACKED, NREG 3: ST RGBAQ XYZF2, PRE */
    if ((tag1 >> 26 & 3) || tag1 >> 28 != 3 || (hdr[2] & 0xFFF) != 0x412 || !(tag1 >> 14 & 1))
        return mesh_fail(MESH_WHY_TAG);
    unsigned prim = tag1 >> 15 & 0x7FF;
    if ((prim & 7) != 4 || (prim & 0x110) == 0x110) return mesh_fail(MESH_WHY_PRIM);   /* a strip, not textured by UV */
    GsMeshHeader *h = (GsMeshHeader *)mesh.packet;
    memset(h, 0, sizeof *h);
    h->magic = GS_MESH_MAGIC;
    h->flags = (mesh.mode == MESH_SHADOW ? GS_MESH_SHADOW : 0) | (prog->backface ? 0 : GS_MESH_TWO_SIDED);
    h->prim = prim;
    unsigned t = prog->ortho ? 0 : hdr[3] & 0x7FFF;   /* 3140 sets no texture */
    if (t) {   /* 0FA8: an A+D tag (data 26), TEX0 TEX1 MIPTBP1 (149-151 + t) and TEXFLUSH (164) */
        const uint32_t *tag = m[26].w;
        if ((tag[0] & 0xFFFF) != 4 || (tag[1] >> 14 & 1) || (tag[1] >> 26 & 3) || tag[1] >> 28 != 1 ||
            (tag[2] & 15) != 0xE || 151 + t > 1023)
            return mesh_fail(MESH_WHY_TEXTURE);
        memcpy(h->ad[0], m[149 + t].w, 16);
        memcpy(h->ad[1], m[150 + t].w, 16);
        memcpy(h->ad[2], m[151 + t].w, 16);
        memcpy(h->ad[3], m[164].w, 16);
        h->flags |= GS_MESH_TEXTURE;
    }
    /* the registers the loop reads, as the setup (and any pass since) left them */
    memcpy(h->clip, prog->clip ? vu1.vf[13].f : m[996].f, sizeof h->clip);   /* the loops without CLIP: data 996 */
    const float *v18 = vu1.vf[18].f, *v19 = vu1.vf[19].f;   /* the setup's data 21 and 4 */
    const float *scale = vu1.vf[31].f;   /* MESH_COLOUR_SCALED: the colour scale (the setup's data 5) */
    if (prog->fog_max) { h->fog[0] = h->fog[2] = v18[1]; h->fog[1] = 0; }          /* clamp(max) = max */
    else { h->fog[0] = v19[3] + v18[0]; h->fog[1] = v18[3]; h->fog[2] = v18[1]; }
    const float uv[2] = {v19[0], v19[1]};
    const float *d5 = m[5].f, *d20 = m[20].f, *d1015 = m[1015].f;
    const float highlight[3] = {m[6].f[3], m[7].f[3], m[8].f[3]};
    uint32_t constant = 0;
    if (prog->colour == MESH_COLOUR_CONSTANT)   /* FTOI0(d1013), the same for every vertex */
        for (unsigned i = 0; i < 4; i++) {
            float f = m[1013].f[i];
            if (!(f > -1.0f && f < 256.0f)) return mesh_fail(MESH_WHY_COLOUR);
            constant |= (uint32_t)(int)f << (i * 8);
        }
    /* 2DE8: RGB 128 (the float 1 + 2^-16 its 2EB8 LOI puts in vf19.xyz), the light d1013 */
    const float *light = m[1013].f, nudge = 1.0000152587890625f;
    const uint32_t lit_alpha = m[1013].w[3] & 255;
    /* the environment map's E and screen terms */
    float env[3][2] = {{0}};
    if (prog->st == MESH_ST_ENVIRONMENT) mesh_environment(env);
    const float env_add[2] = {m[997].f[2], m[997].f[3]}, env_scale[2] = {m[998].f[2], m[998].f[3]};
    /* 0170's prologue sets vf6.x = 1 (0098) and negates it unless the header's bit 15 (0158) */
    float parity = hdr[3] & 0x8000 ? 1.0f : -1.0f;
    mesh.check_parity = parity; mesh.check_base = base;
    GsMeshVertex *out = (GsMeshVertex *)(h + 1);
    uint8_t join[GS_MESH_MAX_VERTICES];
    const float fixed = 1.0f / 4096.0f;   /* ITOF12: exact for these integers */
    /* The vertex's clip-space position, the loop's matrix (vf1..vf4: the object's times the view
       and projection, z scaled) times its pool entry, summed as the loop sums it (0170's 0088-00B8:
       MULAw the fourth column, MADDAx, MADDAy, MADDz), each step rounded (the build has no fused
       multiply-add): the item's position, which the shader divides (finding 249).
       And the guard band test of the loop's CLIP (vf13, data 996) on it: a vertex below the near
       bound sends its batch's flagged triangles through the clipper below (its fans there are not
       what a GPU's clipping draws); the other batches' sideways ones are drawn and the GPU clips
       them. In a loop
       without CLIP a vertex off the guard band sends the batch to the VU (the loop kicks it as it
       comes, which the GS wraps). */
    const __m128 c0 = _mm_loadu_ps(vu1.vf[1].f), c1 = _mm_loadu_ps(vu1.vf[2].f);
    const __m128 c2 = _mm_loadu_ps(vu1.vf[3].f), c3 = _mm_loadu_ps(vu1.vf[4].f);
    const float bound = h->clip[3] < 0 ? -h->clip[3] : h->clip[3];
    uint16_t pool[GS_MESH_MAX_VERTICES];
    uint8_t below[GS_MESH_MAX_VERTICES];   /* the loop's CLIP: below the near bound */
    uint8_t aside[GS_MESH_MAX_VERTICES];   /* off the guard band in x or y (near batches and the check) */
    const int lit = prog->colour == MESH_COLOUR_LIT_D5 || prog->colour == MESH_COLOUR_LIT;
    uint32_t (*words)[4] = prog->colour == MESH_COLOUR_LIGHT_ALPHA || lit ? mesh.words_stored : NULL;
    unsigned any_below = 0, any_aside = 0;
    for (unsigned k = 0; k < n; k++) {
        const ps2_q128 *r = &m[base + 1 + 3 * k];
        const float *p = m[(181 + (r[0].w[3] & 0xFFFF)) & 1023].f;
        const int32_t *normal = m[(373 + (r[0].w[3] & 0xFFFF)) & 1023].sw;
        float cp[4];
        _mm_storeu_ps(cp, _mm_add_ps(_mm_add_ps(_mm_add_ps(c3, _mm_mul_ps(c0, _mm_set1_ps(p[0]))),
                                                _mm_mul_ps(c1, _mm_set1_ps(p[1]))),
                                     _mm_mul_ps(c2, _mm_set1_ps(p[2]))));
        pool[k] = (uint16_t)r[0].w[3];
        below[k] = aside[k] = 0;
        if (prog->ortho && cp[3] != 1.0f) return mesh_fail(MESH_WHY_ORTHO);   /* the shader's divide by 1 */
        /* the loop's CLIP of w + vf13.z against |vf13.w|: the vertex's triangles go through the
           clipper here (mesh_clip_triangle) */
        below[k] = h->clip[2] + cp[3] < -bound;
        if (below[k] ? !prog->clip : !(cp[3] > 0)) return mesh_fail(MESH_WHY_NEAR);
        any_below |= below[k];
        if (!prog->clip) {   /* off the guard band sideways (in homogeneous form) */
            float x = cp[0] + h->clip[0] * cp[3], y = cp[1] + h->clip[1] * cp[3], b = bound * cp[3];
            if (!(x <= b && x >= -b && y <= b && y >= -b)) return mesh_fail(MESH_WHY_CLIP);
        }
        const uint32_t *c = r[1].w;   /* (3140 and 2DE8 overwrite it) */
        /* (the lit loops take any word: ITOF0, the light, then their own clamp) */
        if (prog->colour != MESH_COLOUR_CONSTANT && prog->colour != MESH_COLOUR_LIGHT_ALPHA && !lit &&
            (c[0] | c[1] | c[2] | c[3]) > 255)
            return mesh_fail(MESH_WHY_COLOUR);
        int32_t w = r[2].sw[3];
        if (w != 0 && w != 0x800000) return mesh_fail(MESH_WHY_JOIN);   /* 2048.0: ADC through the fog's FTOI4 */
        join[k] = mesh.join[k] = w != 0;
        GsMeshVertex *o = &out[k];
        o->x = cp[0]; o->y = cp[1]; o->z = cp[2]; o->w = cp[3];
        const float nx = (float)normal[0] * fixed, ny = (float)normal[1] * fixed, nz = (float)normal[2] * fixed;
        if (prog->st == MESH_ST_ENVIRONMENT) {   /* screen-linear S, T and Q = 1: times w (the shader divides) */
            float q = 1.0f / cp[3], st[2];
            for (unsigned a = 0; a < 2; a++)
                st[a] = env[0][a] * nx + env[1][a] * ny + env[2][a] * nz + (cp[a] * q + env_add[a]) * env_scale[a];
            o->u = st[0] * cp[3]; o->v = st[1] * cp[3]; o->uz = cp[3];
        } else if (prog->st == MESH_ST_PROJECTED) {   /* (d5-d8) (x y z 1), its z the texture's Q */
            float s[3];
            for (unsigned a = 0; a < 3; a++) s[a] = m[8].f[a] + m[5].f[a] * p[0] + m[6].f[a] * p[1] + m[7].f[a] * p[2];
            o->u = s[0]; o->v = s[1]; o->uz = s[2];
        } else {
            o->u = (float)r[2].sw[0] * fixed; o->v = (float)r[2].sw[1] * fixed; o->uz = (float)r[2].sw[2] * fixed;
            if (prog->st == MESH_ST_OFFSET || (prog->st == MESH_ST_OFFSET_AFTER_FIRST && k)) {   /* vf19.xy */
                o->u += uv[0]; o->v += uv[1];
            }
        }
        if (prog->colour == MESH_COLOUR_CONSTANT)
            o->rgba = constant;
        else if (prog->colour == MESH_COLOUR_LIGHT_ALPHA) {   /* 2DE8 (2F08, 2F10): (z + y) + nudge x */
            float d = (nz * light[2] + ny * light[1]) + nudge * (nx * light[0]);
            o->rgba = 0x808080u | (d > 0 ? lit_alpha : 0) << 24;
            /* as stored: the nudge's bits, and the light's w or 1.0 (2F48) */
            words[k][0] = words[k][1] = words[k][2] = mesh_bits(nudge);
            words[k][3] = d > 0 ? m[1013].w[3] : 0x3F800000u;
        } else if (prog->colour == MESH_COLOUR_HIGHLIGHT) {   /* 1510: the pool normal against d6-d8's w */
            float d = highlight[0] * nx + highlight[1] * ny + highlight[2] * nz;
            d = d > 0 ? d : 0;
            d *= d; d *= d; d *= d; d *= d;   /* four squarings: ^16 */
            const float *s31 = vu1.vf[31].f;
            uint32_t rgba = 0;
            for (unsigned i = 0; i < 4; i++) {
                float sum = d20[i] * d + (float)c[i] * s31[i];
                if (!(sum > -1.0f)) return mesh_fail(MESH_WHY_COLOUR);
                rgba |= (uint32_t)(int)(sum > 255 ? 255.0f : sum) << (i * 8);
            }
            o->rgba = rgba;
        } else if (lit) {   /* 1110 / 13A0: the lit pool entry */
            const float *L = m[(373 + (r[0].w[3] & 0xFFFF)) & 1023].f, l2 = L[3] * L[3];
            uint32_t rgba = 0;
            for (unsigned i = 0; i < 4; i++) {
                float f = (float)(int32_t)c[i] * (prog->colour == MESH_COLOUR_LIT_D5 ? d5[i] : 1.0f);
                float sum = d20[i] * l2 + (i < 3 ? d1015[i] : 0.0f) + f * (i < 3 ? L[i] : 1.0f);
                /* A pool entry the lighting pass left alone holds integers: as floats, tiny numbers
                   and, for a negative one, exponent 255, which the VU reads as a huge number (it has
                   no NaN and no infinity; a result past the range saturates). */
                if (!(sum - sum == 0))
                    sum = mesh_vu(mesh_vu(mesh_vu(d20[i] * mesh_vu(mesh_vu(L[3]) * mesh_vu(L[3]))) +
                                          (i < 3 ? d1015[i] : 0.0f)) +
                                  mesh_vu(f * (i < 3 ? mesh_vu(L[i]) : 1.0f)));
                if (!(sum - sum == 0)) return mesh_fail(MESH_WHY_COLOUR);
                /* FTOI0 of min(sum, 255): a sum of -1 or less is stored as a negative word, of which
                   the GS takes the low byte (and the clipper the word) */
                words[k][i] = (uint32_t)mesh_ftoi(sum > 255 ? 255.0f : sum, 1.0f);
                rgba |= (words[k][i] & 255) << (i * 8);
            }
            o->rgba = rgba;
        } else if (prog->colour == MESH_COLOUR_SCALED) {   /* min(c scale, 255), truncated */
            uint32_t rgba = 0;
            for (unsigned i = 0; i < 4; i++) {
                float f = (float)c[i] * scale[i];
                if (!(f > -1.0f)) return mesh_fail(MESH_WHY_COLOUR);
                rgba |= (uint32_t)(int)(f > 255 ? 255.0f : f) << (i * 8);
            }
            o->rgba = rgba;
        } else
            o->rgba = c[0] | c[1] << 8 | c[2] << 16 | c[3] << 24;
    }
    /* The program clips the triangles off the guard band sideways too. In a batch that crosses the
       near bound those are drawn as its fans as well: the batch then is what the VU kicked (the
       near ground, cut by the GPU beside a fan, shows a seam); in the others the GPU clips them
       (finding 164) and the check compares the fans. */
    if (prog->clip && (any_below || mesh.check))
        for (unsigned k = 0; k < n; k++) {
            const GsMeshVertex *o = &out[k];
            const float q = mesh_div(1.0f, o->w), at[3] = {o->x * q + h->clip[0], o->y * q + h->clip[1], 0};
            any_aside |= aside[k] = (mesh_clip_flags(at, h->clip[3]) & 15) != 0;
        }
    /* The triangles that cross the near bound, as the fans the loop's clipper kicks while the loop
       runs (so before the strip): their vertices after the strip's, in clip space through the loop's
       matrix as the clipper projects them, with the colour it interpolated and its texture
       coordinates: (u, v, 1), the environment map's from the interpolated normal and the screen
       position, or 2DE8's projection of the interpolated position. A fan is drawn from both sides:
       in a culled item (0170) each triangle is ordered by its own area. */
    uint16_t list[(GS_MESH_MAX_VERTICES - 2) * (GS_MESH_FAN_VERTICES - 1) * 3];
    unsigned total = n, triangles = 0, clipped = 0, fans = 0;
    mesh.fans = mesh.fans_seen = 0;
    if (any_below | any_aside) {
        const float fog_base = m[4].f[3] + m[21].f[0], fog_q = m[21].f[3], fog_max = m[21].f[1];
        float z_scale = m[4].f[2] * m[4].f[2];
        z_scale *= z_scale;
        for (unsigned k = 2; k < n; k++) {
            const int cut = below[k] | below[k - 1] | below[k - 2] | aside[k] | aside[k - 1] | aside[k - 2];
            const int draw = any_below && cut;   /* else (the check) one the GPU clips */
            if (join[k] || !cut) continue;
            MeshClipVertex poly[GS_MESH_FAN_VERTICES];
            int got = mesh_clip_triangle(prog, out, pool, (const uint32_t (*)[4])words, k - 2, k - 1, k, poly);
            clipped += (unsigned)draw;
            if (got < 0 && draw) return mesh_fail(MESH_WHY_FAN_MANY);
            if (got <= 0) continue;
            float sx[GS_MESH_FAN_VERTICES], sy[GS_MESH_FAN_VERTICES];
            for (unsigned j = 0; j < (unsigned)got; j++) {
                float cp[4], st[3], kicked[3];   /* st: the item's u, v, uz; kicked: the STQ the clipper stores */
                mesh_transform(cp, &vu1.vf[1], poly[j].p);
                const float q = mesh_div(1.0f, cp[3]);
                float fog = fog_base + fog_q * q, z = cp[2] * q * z_scale;
                fog = fog > 0 ? fog : 0;
                fog = fog < fog_max ? fog : fog_max;
                z = z < 16777215.0f ? z : 16777215.0f;
                sx[j] = cp[0] * q; sy[j] = cp[1] * q;
                if (prog->st == MESH_ST_ENVIRONMENT) {   /* 2338: E N + (X + d997.zw) d998.zw, Q = 1 */
                    const float *normal = poly[j].t, at[2] = {sx[j], sy[j]};
                    for (unsigned a = 0; a < 2; a++) {
                        kicked[a] = ((env[0][a] * normal[0] + env[1][a] * normal[1]) + env[2][a] * normal[2]) +
                                    (at[a] + env_add[a]) * env_scale[a];
                        st[a] = kicked[a] * cp[3];
                    }
                    kicked[2] = 1.0f; st[2] = cp[3];
                } else if (prog->st == MESH_ST_PROJECTED) {   /* 2388: (d5-d8) (x y z 1), times Q */
                    const float *at = poly[j].p;
                    for (unsigned a = 0; a < 3; a++) {
                        st[a] = ((m[8].f[a] + m[5].f[a] * at[0]) + m[6].f[a] * at[1]) + m[7].f[a] * at[2];
                        kicked[a] = st[a] * q;
                    }
                } else
                    for (unsigned a = 0; a < 3; a++) { st[a] = poly[j].t[a]; kicked[a] = st[a] * q; }
                if (mesh.check) {
                    uint32_t (*e)[4] = mesh.fan[mesh.fans].q[j];
                    for (unsigned a = 0; a < 3; a++) e[0][a] = mesh_bits(kicked[a]);
                    for (unsigned a = 0; a < 4; a++) e[1][a] = (uint32_t)mesh_ftoi(poly[j].c[a], 1.0f);
                    e[2][0] = (uint32_t)mesh_ftoi(sx[j], 16.0f); e[2][1] = (uint32_t)mesh_ftoi(sy[j], 16.0f);
                    e[2][2] = (uint32_t)mesh_ftoi(z, 16.0f); e[2][3] = (uint32_t)mesh_ftoi(fog, 16.0f);
                }
                if (got < 3 || !draw) continue;
                /* what the shader makes of the item's vertex must be the kicked one: in front of the
                   eye, and the fog of the item's constants. The clipper's near plane is its own
                   space's (data 176-179), not the loop's CLIP: a fan vertex the shader finds below
                   the loop's near bound goes out as the plain projection, with the Z the clipper
                   clamped. */
                float item_fog = h->fog[0] + h->fog[1] * q;
                item_fog = item_fog < h->fog[2] ? item_fog : h->fog[2];
                item_fog = item_fog > 0 ? item_fog : 0;
                if (!(cp[3] > 0)) return mesh_fail(MESH_WHY_FAN_NEAR);
                if ((int)item_fog != (int)fog) return mesh_fail(MESH_WHY_FAN_FOG);
                GsMeshVertex *o = &out[total + j];
                o->x = cp[0]; o->y = cp[1]; o->w = cp[3];
                o->z = cp[2] * z_scale;
                if (!(o->z < 16777215.0f * cp[3])) o->z = 16777215.0f * cp[3];
                o->u = st[0]; o->v = st[1]; o->uz = st[2];
                o->rgba = 0;
                for (unsigned a = 0; a < 4; a++) o->rgba |= ((uint32_t)mesh_ftoi(poly[j].c[a], 1.0f) & 255) << (a * 8);
            }
            if (mesh.check) mesh.fan[mesh.fans].count = (unsigned)got;
            mesh.fans++;
            if (got < 3 || !draw) continue;
            for (unsigned j = 1; j + 1 < (unsigned)got; j++) {
                float area = (sx[j] - sx[0]) * (sy[j + 1] - sy[j]) - (sy[j] - sy[0]) * (sx[j + 1] - sx[j]);
                int swap = prog->backface && area > 0;   /* the cull drops a positive area (y down) */
                list[triangles++] = (uint16_t)total;
                list[triangles++] = (uint16_t)(total + j + (swap ? 1 : 0));
                list[triangles++] = (uint16_t)(total + j + (swap ? 0 : 1));
            }
            total += (unsigned)got;
            fans++;
        }
    }
    /* the strip's triangles: k-2, k-1, k for each vertex k >= 2 without the join's ADC; with 0170's
       culling each ordered so that the cull (positive screen area, y down) drops what its back-face
       test would */
    unsigned joins = 0;
    for (unsigned k = 2; k < n; k++) {
        if (join[k]) { joins++; continue; }
        if (any_below && (below[k] | below[k - 1] | below[k - 2] | aside[k] | aside[k - 1] | aside[k - 2]))
            continue;   /* the clipper's, above */
        int keep_order = !prog->backface || (parity > 0) == !(k & 1);
        list[triangles++] = (uint16_t)(keep_order ? k - 2 : k - 1);
        list[triangles++] = (uint16_t)(keep_order ? k - 1 : k - 2);
        list[triangles++] = (uint16_t)k;
    }
    memcpy(out + total, list, triangles * sizeof *list);
    h->vertices = total; h->indices = triangles;
    mesh.strip = n;
    mesh.words = (unsigned)((sizeof *h + total * sizeof *out + triangles * 2 + 3) / 4);
    count->batches++; count->vertices += n; count->triangles += triangles / 3; count->joins += joins;
    count->near_batches += any_below != 0; count->clip_triangles += clipped; count->fans += fans;
    count->fan_vertices += total - n;
    /* the parity 0170 leaves: one flip a vertex */
    if (prog->backface) vu1.vf[6].f[0] = n & 1 ? -parity : parity;
    return 1;
}

/* Before a VU1 run: 1 when the run is a translated batch (skip mode: do not run it), 2 when it is
   one in shadow mode (run it, its kicks marked shadow, then mesh_send), else 0. *packet: the item. */
/* DHD_VU1_EMULATED=1 (with Native VU on): every VU1 run that still goes through the emulated VU,
   counted per kind (overlay, MSCAL program, entry, MSCNT, the dispatch register VI13 at the mesh
   continuations: DHD_VU1_LAYOUT's kinds) and listed every 3000 fields and at the end, the work list for the emulated
   path's deletion (finding 253). The translated loops' own batches are in it with their reasons
   in the counters above. */
enum { MESH_EMULATED_KINDS = 256 };
static struct { uint32_t used, overlay, program, entry, mscnt, vi13; uint64_t runs, kicks; } mesh_emulated_kinds[MESH_EMULATED_KINDS];
static uint64_t mesh_emulated_other;
static int mesh_emulated_on = -1;

static void mesh_emulated_report(void)
{
    uint64_t runs = mesh_emulated_other;
    for (unsigned i = 0; i < MESH_EMULATED_KINDS; i++) runs += mesh_emulated_kinds[i].runs;
    hle_log("emulated VU1: %llu runs with Native VU on (%llu of kinds past the table)\n",
            (unsigned long long)runs, (unsigned long long)mesh_emulated_other);
    for (unsigned i = 0; i < MESH_EMULATED_KINDS; i++)
        if (mesh_emulated_kinds[i].used)
            hle_log("emulated VU1: overlay %c program %04X entry %04X mscnt %u vi13 %03X: %llu runs, %llu kicks\n",
                    "?AB"[mesh_emulated_kinds[i].overlay], mesh_emulated_kinds[i].program, mesh_emulated_kinds[i].entry,
                    mesh_emulated_kinds[i].mscnt, mesh_emulated_kinds[i].vi13,
                    (unsigned long long)mesh_emulated_kinds[i].runs, (unsigned long long)mesh_emulated_kinds[i].kicks);
}

static void mesh_emulated(unsigned addr, unsigned mscnt)
{
    if (mesh_emulated_on < 0 && (mesh_emulated_on = getenv("DHD_VU1_EMULATED") != NULL))
        host_end_add(HOST_END_REPORT, "emulated VU1 runs", mesh_emulated_report);
    mesh_emulated_kicks = NULL;
    if (!mesh_emulated_on) return;
    unsigned overlay = !memcmp(micro(1), GUEST(PS2_RAM_BASE + 0x265C10), 32) ? 1 :
                       !memcmp(micro(1), GUEST(PS2_RAM_BASE + 0x269C60), 32) ? 2 : 0;
    unsigned program = graphics_provenance.program;
    /* VI13 at every continuation that jumps through it: 1040, 1CB0, 12E0 and the first batches
       after a pool pass or 30A0's setup (0590, 06E0, 3258: JR vi13) */
    unsigned vi13 = (addr == 0x1040 || addr == 0x1CB0 || addr == 0x12E0 || addr == 0x0590 || addr == 0x06E0 ||
                     addr == 0x3258) ? vu1.vi[13] & 0xFFFF : 0;
    unsigned at = (program * 7 + addr * 13 + overlay * 5 + mscnt + vi13 * 3) % MESH_EMULATED_KINDS;
    for (unsigned n = 0; n < MESH_EMULATED_KINDS; n++, at = (at + 1) % MESH_EMULATED_KINDS) {
        if (!mesh_emulated_kinds[at].used) {
            mesh_emulated_kinds[at].used = 1; mesh_emulated_kinds[at].overlay = overlay;
            mesh_emulated_kinds[at].program = program; mesh_emulated_kinds[at].entry = addr;
            mesh_emulated_kinds[at].mscnt = mscnt; mesh_emulated_kinds[at].vi13 = vi13;
        }
        if (mesh_emulated_kinds[at].overlay == overlay && mesh_emulated_kinds[at].program == program &&
            mesh_emulated_kinds[at].entry == addr && mesh_emulated_kinds[at].mscnt == mscnt &&
            mesh_emulated_kinds[at].vi13 == vi13) {
            mesh_emulated_kinds[at].runs++;
            mesh_emulated_kicks = &mesh_emulated_kinds[at].kicks;
            return;
        }
    }
    mesh_emulated_other++;
}

static int mesh_before(unsigned addr, int mscnt, uint32_t **packet)
{
    if (!mesh_enabled()) return 0;
    mesh.object = -1;
    if (!mscnt || (addr != 0x1040 && addr != 0x1CB0 && addr != 0x12E0 && addr != 0x0590 && addr != 0x06E0 &&
                   addr != 0x3258))
        return 0;
    for (unsigned p = 0; p < MESH_PROGRAMS; p++)
        if (vu1.vi[13] == mesh_programs[p].vi13 && (mesh.enabled >> p & 1)) mesh.object = (int)p;
    if (mesh.object < 0 || !mesh_overlay_b()) return mesh.object = -1, 0;
    /* 0590 (after the skinning pass): its JR's delay slot stores the pass's last vertex (SQ vf20,
       -2(vi8)); the VU repeats it in shadow mode */
    if (addr == 0x0590) memcpy(vu1.vu0_mem[(vu1.vi[8] - 2) & 1023].f, vu1.vf[20].f, 16);
    /* 06E0 (after program 0000's pool pass, 05A0): its delay slot stores the pass's last entry (SQ vf25,
       190(vi8)); 3258 (after 30A0's setup, 3190) has none (finding 264) */
    if (addr == 0x06E0) memcpy(vu1.vu0_mem[(vu1.vi[8] + 190) & 1023].f, vu1.vf[25].f, 16);
    float parity = vu1.vf[6].f[0];
    /* The item's maths is the loop's, so it rounds as the VU does (toward zero, FTZ/DAZ; as
       ps2_vu1_run sets it): the positions it makes are then the VU's own bits (finding 249). */
    unsigned control = host_fp_csr();
    const int set_mode = (control & ~0x3Fu) != 0xFFC0u;
    if (set_mode) host_fp_set_csr((control & ~0x603Fu) | 0xFFC0u);
    int built = mesh_build();
    if (set_mode) host_fp_set_csr(control);
    if (!built) return 0;
    *packet = mesh.packet;
    if (mesh.mode == MESH_SHADOW) {
        mesh_kick_watch = mesh.check;
        vu1.vf[6].f[0] = parity;   /* the VU runs the batch itself (0170 flips it there) */
        return 2;
    }
    return 1;
}

static void packet_report(void);

static void mesh_send(uint32_t *packet)
{
    static uint64_t report_at = 3000;
    if (mesh.check && mesh.mode == MESH_SHADOW) {
        mesh_kick_watch = 0;
        if (mesh.fans_seen < mesh.fans) mesh.count[mesh.object].fans_missing += mesh.fans - mesh.fans_seen;
        mesh_check();
    }
    if (kernel_vsync_count >= report_at) {   /* the counts so far, every 3000 fields (and at exit) */
        mesh_report();
        if (mesh_emulated_on > 0) mesh_emulated_report();
        packet_report();
        report_at = kernel_vsync_count + 3000;
    }
    graphics_gif_feed(GS_PATH_MESH, packet, mesh.words);
}

/* A skipped batch: the state its end leaves (0FF8, 1000-1030; 0170's parity store at 1014). */
static void mesh_skipped(void)
{
    const MeshProgram *prog = &mesh_programs[mesh.object];
    if (prog->backface) memcpy(vu1.vu0_mem[1014].f, vu1.vf[6].f, 16);
    /* the registers each loop loads with constants and keeps (its working registers, the last
       vertex's values and the fog base vf22.w, are set by every prologue before they are read) */
    ps2_q128 *vf = vu1.vf;
    const ps2_q128 *m = vu1.vu0_mem;
    if (prog->tag_loi) memcpy(&vu1.vu0_mem[mesh.check_base].w[1], micro(1) + prog->tag_loi, 4);   /* the tag word */
    switch (prog->vi13) {
    case 0x20C:   /* 1110's prologue: vf31 = d20, vf8 = d5, vf26 = d1015 */
        memcpy(vf[31].f, m[20].f, 16);
        memcpy(vf[8].f, m[5].f, 16);
        memcpy(vf[26].f, m[1015].f, 16);
        break;
    case 0x260:   /* 13A0's: vf13 = d1015, vf14 = d20 */
        memcpy(vf[13].f, m[1015].f, 16);
        memcpy(vf[14].f, m[20].f, 16);
        break;
    case 0x288:   /* 1510's: vf7 = d17, vf14 = d6, and from its last pass vf8 = d18, vf10 = d19 (15E8, 1608) */
        memcpy(vf[7].f, m[17].f, 16);
        memcpy(vf[14].f, m[6].f, 16);
        memcpy(vf[8].f, m[18].f, 16);
        memcpy(vf[10].f, m[19].f, 16);
        break;
    case 0x616:   /* 3140's: vf5.y = I (the tag word), vf5.w = vf18.y, vf10 = FTOI0(d1013) (0..255: mesh_build) */
        memcpy(&vf[5].f[1], micro(1) + prog->tag_loi, 4);
        vf[5].f[3] = vf[18].f[1];
        for (unsigned i = 0; i < 4; i++) vf[10].sw[i] = (int32_t)m[1013].f[i];
        break;
    case 0x154:
    case 0x186: {   /* 0AA0's: vf14-vf16.xy = E, vf13.xyz = (d997.z, d997.w, d997.w) (MR32 twice), vf17 =
                       (d998.z, d998.w, d998.w, d998.w), vf8.z = 1, vf5.w = vf18.y; 0C30's: vf8, vf10,
                       vf14 .xy = E, vf7 = (d997.zw, d998.zw), vf27.xy = d998.zw, vf17.z = 1 */
        float env[3][2];
        mesh_environment(env);
        const float *d997 = m[997].f, *d998 = m[998].f;
        static const unsigned at[2][3] = {{14, 15, 16}, {8, 10, 14}};
        const int c30 = prog->vi13 == 0x186;
        for (unsigned j = 0; j < 3; j++) memcpy(vf[at[c30][j]].f, env[j], 8);
        if (!c30) {
            vf[13].f[0] = d997[2]; vf[13].f[1] = d997[3]; vf[13].f[2] = d997[3];
            vf[17].f[0] = d998[2]; vf[17].f[1] = vf[17].f[2] = vf[17].f[3] = d998[3];
            vf[8].f[2] = 1.0f;
            vf[5].f[3] = vf[18].f[1];
        } else {
            vf[7].f[0] = d997[2]; vf[7].f[1] = d997[3]; vf[7].f[2] = d998[2]; vf[7].f[3] = d998[3];
            vf[27].f[0] = d998[2]; vf[27].f[1] = d998[3];
            vf[17].f[2] = 1.0f;
        }
        break;
    }
    case 0x5BD:   /* 2DE8's: vf6 = d1013, vf7, vf8, vf10, vf14 = d5-d8, vf19 = (nudge x3, d1013.w), vf23.y = I */
        memcpy(vf[6].f, m[1013].f, 16);
        memcpy(vf[7].f, m[5].f, 16);
        memcpy(vf[8].f, m[6].f, 16);
        memcpy(vf[10].f, m[7].f, 16);
        memcpy(vf[14].f, m[8].f, 16);
        vf[19].f[0] = vf[19].f[1] = vf[19].f[2] = 1.0000152587890625f;
        vf[19].f[3] = m[1013].f[3];
        memcpy(&vf[23].f[1], micro(1) + prog->tag_loi, 4);
        break;
    }
    uint16_t buffer = (uint16_t)(vu1.vi[14] + 0x90);
    if (buffer == 0x3E8) buffer = 0x238;
    vu1.vi[14] = vu1.vi[2] = buffer;
    vu1.vi[3] = 0x3E8;
    vu1.vi[4] = 0x7FFF;
    vu1.vu0_ctrl[10] = 0x1040;
}

/* After a VU1 run that was not skipped (the constants are read at each batch). */
static void mesh_after(unsigned addr, int mscnt) { (void)addr, (void)mscnt; }

/* Overlay B's programs 0030 and 0050 (0030 is B 0050) kick the GIF packet the EE wrote at data
   0x6C and end: 0050 IADDIU vi8, vi0, 0x6C; XGKICK vi8; NOP; NOP[E]; NOP. In C, for every
   renderer, as the VU leaves it (finding 264): the same kick, VI8, TPC 0078 and the flag registers
   written back as ps2_vu1_run writes them (status 12 bits, MAC 16, clip 24); nothing else changes.
   DHD_NATIVE_VU1_CHECK=1 also runs each one on the VU from the same state and compares the
   registers, the data memory and the kicked qword (packet_check). */
static struct { int check; uint64_t runs, checked, bad_state, bad_kick; } packet = {-1};
static unsigned packet_kicks, packet_kicked;

static int packet_kick_record(void *opaque, unsigned qword)
{
    (void)opaque;
    packet_kicks++; packet_kicked = qword;
    return 0;
}

static void packet_report(void)
{
    hle_log("native VU1 0030/0050: %llu runs in C; checked %llu: state differs %llu, kick differs %llu\n",
            (unsigned long long)packet.runs, (unsigned long long)packet.checked,
            (unsigned long long)packet.bad_state, (unsigned long long)packet.bad_kick);
}

static void packet_end(void)
{
    vu1.vi[8] = 0x6C;
    vu1.vu0_ctrl[0] &= 0xFFF; vu1.vu0_ctrl[1] &= 0xFFFF; vu1.vu0_ctrl[2] &= 0xFFFFFF;
    vu1.vu0_ctrl[10] = 0x78;
}

static void packet_check(unsigned addr, unsigned top, unsigned itop)
{
    static ps2_cpu before, after;
    static ps2_q128 memory[1024];
    before = vu1;
    memcpy(memory, vu1.vu0_mem, sizeof memory);
    packet_end();
    after = vu1;
    vu1 = before;
    packet_kicks = 0;
    int failed = ps2_vu1_run(&vu1, addr, top, itop, packet_kick_record, NULL);
    packet.checked++;
    if (failed || memcmp(&vu1, &after, sizeof vu1) || memcmp(memory, vu1.vu0_mem, sizeof memory)) packet.bad_state++;
    if (packet_kicks != 1 || packet_kicked != 0x6C) packet.bad_kick++;
    vu1 = after;
}

/* 1 when the run was one of these (kicked here), else 0. */
static int packet_program(unsigned addr, int mscnt, unsigned top, unsigned itop, int traced)
{
    if (mscnt || (addr != 0x30 && addr != 0x50) || !mesh_overlay_b()) return 0;
    if (packet.check < 0) {
        packet.check = getenv("DHD_NATIVE_VU1_CHECK") != NULL;
        host_end_add(HOST_END_REPORT, "native VU1 0030/0050", packet_report);
    }
    packet.runs++;
    if (packet.check) packet_check(addr, top, itop);
    else packet_end();
    if (traced) kick_trace(0x58, vu1.vu0_ctrl[2], 0x6C);
    if (kick(NULL, 0x6C)) hle_log("vif1: microprogram at 0x%X failed\n", addr);
    return 1;
}
