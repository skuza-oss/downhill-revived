/* Native VU1 meshes (findings 164 and 167), included by vif.c: DHD_NATIVE_VU1 hands overlay B's
   batch loops to the native renderer instead of running them. The value lists the loops (by their
   address; "prelit", "lit" or "all" for groups), then optional flags: DHD_NATIVE_VU1=0170,1708:skip.

   | loop | VI13  | fog      | colour                         | UV offset     | back faces | clipping |
   | 0170 | 0x011 | computed | c vf31                         | from vertex 1 | culled     | yes      |
   | 1708 | 0x2C9 | computed | c vf31                         | every vertex  | drawn      | yes      |
   | 08B0 | 0x100 | computed | c vf31                         | every vertex  | drawn      | no       |
   | 0798 | 0x0E0 | maximum  | c                              | none          | drawn      | no       |
   | 18B0 | 0x2FE | maximum  | c                              | none          | drawn      | yes      |
   | 1110 | 0x20C | computed | c d5 L + d20 L.w^2 + d1015     | every vertex  | drawn      | yes      |
   | 13A0 | 0x260 | maximum  | c L + d20 L.w^2 + d1015        | none          | drawn      | no       |
   | 1510 | 0x288 | computed | c vf31 + d20 max(N.H, 0)^16    | every vertex  | drawn      | yes      |

   (c the batch vertex's colour, L its pool entry's lighting at data 373 + index, N its pool
   normal there (12-bit fixed point), H = (d6.w, d7.w, d8.w), dN data N; the lit colours clamped
   to 255 and truncated, their alpha c.a d5.a + d20.a L.w^2, d1015 adding to RGB only.)

   A batch loop runs after a program's setup has set VI13 and the registers it reads: the shared
   1AE0 (world objects: 0078, 1638, 07F0, 06F0, 17E0; the lit setups 1050 and 12F0), and for lit or
   skinned objects a pass over the object's pool (MSCAL 0040's lighting, 0010/0020's skinning and
   lighting) that writes positions at 181 + index and lighting at 373 + index. Those stay on the VU.
   Each batch (MSCNT at 1CB0, 1040, 12E0 or 0590, VI13 a translated loop's) is read from VU1 data
   memory and the registers as they are and goes to the GS queue as one mesh item (gs_mesh.h): the
   pool positions, colours and texture coordinates of its strip vertices, the triangles the strip
   draws (the input's ADC joins left out; with back-face culling each in the winding the program's
   test keeps), the A+D texture writes and PRIM its packet would make, and the constants. The
   batch's microprogram and its XGKICK do not run; what the next run reads is left as the batch
   leaves it (the rotating buffer in VI14/VI2, VI3, VI4, TPC 1040; 0170's strip parity in vf6.x and
   at 1014; the lit loops' registers vf31 or vf13 and vf14). 0590's delay slot, the skinning's last
   pool store, is made first.

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
   frames; finding 164), and the GS wraps the unclipped loops' ones: a batch with such a vertex
   runs on the VU, by the same test on the CPU, in homogeneous form, before it is translated.
   :gpuclip draws the near ones too and leaves them to the GPU's clipping (a measurement).

   Modes: skip (the batch's program does not run), or shadow, the default while DHD_EXACT_PROBE
   answers the lens flare's Z probe from the GS stream: the program runs too and its packet is
   kicked for the exact path only (the native renderer draws the mesh item instead), so game
   state is the switch-off run's. :skip or :shadow forces one. DHD_NATIVE_VU1_CHECK=1 (shadow)
   compares each item, through the shader's maths, with the packet the program wrote (mesh_check).
   Only with the native renderer and indexed fields (gs_mesh_mode); the browser builds leave it
   out. */
#include "../gs/gs_mesh.h"

#if defined(DHD_WEB_VU) || defined(DHD_WEB_HOST)
enum { MESH_OFF = 1 };
static struct { int state; } mesh = {MESH_OFF};
static int mesh_before(unsigned addr, int mscnt, uint32_t **packet) { (void)addr, (void)mscnt, (void)packet; return 0; }
static void mesh_send(uint32_t *packet) { (void)packet; }
static void mesh_skipped(void) {}
static void mesh_after(unsigned addr, int mscnt) { (void)addr, (void)mscnt; }
static void mesh_timed(double seconds) { (void)seconds; }
#else
enum { MESH_OFF = 1, MESH_ON = 2, MESH_SKIP = 1, MESH_SHADOW = 2 };
enum { MESH_WHY_EOP, MESH_WHY_COUNT, MESH_WHY_TAG, MESH_WHY_PRIM, MESH_WHY_TEXTURE, MESH_WHY_COLOUR, MESH_WHY_JOIN,
       MESH_WHY_NEAR, MESH_WHY_CLIP, MESH_WHYS };
static const char *const mesh_why_names[MESH_WHYS] = {"no EOP", "count", "tag", "prim", "texture tag", "colour",
                                                     "join value", "a vertex below the near bound",
                                                     "a vertex off the guard band in x or y"};

/* The translated loops (the table above). colour: c vf31, c, lit with d5, lit, the highlight. */
enum { MESH_COLOUR_SCALED, MESH_COLOUR_RAW, MESH_COLOUR_LIT_D5, MESH_COLOUR_LIT, MESH_COLOUR_HIGHLIGHT };
typedef struct {
    const char *name;
    uint16_t vi13;
    uint8_t fog_max, colour, uv_offset, backface, clip;   /* uv_offset: 0 none, 1 from vertex 1, 2 every vertex */
} MeshProgram;
static const MeshProgram mesh_programs[] = {
    {"0170", 0x011, 0, MESH_COLOUR_SCALED, 1, 1, 1},
    {"1708", 0x2C9, 0, MESH_COLOUR_SCALED, 2, 0, 1},
    {"08B0", 0x100, 0, MESH_COLOUR_SCALED, 2, 0, 0},
    {"0798", 0x0E0, 1, MESH_COLOUR_RAW, 0, 0, 0},
    {"18B0", 0x2FE, 1, MESH_COLOUR_RAW, 0, 0, 1},
    {"1110", 0x20C, 0, MESH_COLOUR_LIT_D5, 2, 0, 1},
    {"13A0", 0x260, 1, MESH_COLOUR_LIT, 0, 0, 0},
    {"1510", 0x288, 0, MESH_COLOUR_HIGHLIGHT, 2, 0, 1},
};
enum { MESH_PRELIT = 0x1F, MESH_LIT = 0xE0 };   /* the groups' bits */
enum { MESH_PROGRAMS = sizeof mesh_programs / sizeof *mesh_programs };

typedef struct {
    uint64_t batches, fallbacks, vertices, triangles, joins, why[MESH_WHYS];
    double timed_seconds; uint64_t timed_batches, timed_vertices;   /* DHD_VU1_PROFILE (mesh_timed) */
} MeshCounts;

static struct {
    int state;                 /* 0 unread, MESH_OFF, MESH_ON */
    int mode;                  /* MESH_SKIP or MESH_SHADOW */
    unsigned enabled;          /* the translated loops, a bit each (mesh_programs) */
    int object;                /* the batch's loop (mesh_programs), or -1 */
    int gpu_clip;              /* :gpuclip: batches the VU would clip are drawn too (the GPU clips them) */
    uint64_t code_version; int overlay_b;
    MeshCounts count[MESH_PROGRAMS];
    /* the queue item: header, vertices, indices */
    uint32_t packet[(sizeof(GsMeshHeader) + GS_MESH_MAX_VERTICES * sizeof(GsMeshVertex) +
                     GS_MESH_MAX_VERTICES * 3 * 2 + 4) / 4];
    unsigned words;
    /* DHD_NATIVE_VU1_CHECK=1 (shadow mode): each item against what the program wrote (mesh_check) */
    int check;
    unsigned check_base; float check_parity; uint8_t join[GS_MESH_MAX_VERTICES];
    uint64_t checked, inside, bad_xy, bad_z, bad_fog, bad_colour, bad_stq, triangles_checked, clipped, bad_adc,
             backfaces;
} mesh = {.object = -1};

/* A skipped batch's time on the VU thread, from the run's start to the item queued (profiled runs). */
static void mesh_timed(double seconds)
{
    if (mesh.object < 0) return;
    MeshCounts *c = &mesh.count[mesh.object];
    c->timed_seconds += seconds;
    c->timed_batches++;
    c->timed_vertices += ((const GsMeshHeader *)mesh.packet)->vertices;
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
        for (unsigned i = 0; i < MESH_WHYS; i++)
            if (c->why[i])
                hle_log("native VU1 %s: %llu batches on the VU: %s\n", mesh_programs[p].name, (unsigned long long)c->why[i],
                        mesh_why_names[i]);
        if (c->timed_batches)
            hle_log("native VU1 %s cost (DHD_VU1_PROFILE): %.2f us a translated batch, %.1f ns a vertex (%llu batches)\n",
                    mesh_programs[p].name, c->timed_seconds * 1e6 / (double)c->timed_batches,
                    c->timed_seconds * 1e9 / (double)(c->timed_vertices ? c->timed_vertices : 1),
                    (unsigned long long)c->timed_batches);
    }
    if (mesh.check)
        hle_log("native VU1 check: %llu vertices (%llu inside the guard band): XY off by more than 1/16 px %llu, "
                "Z by more than 1 %llu, fog %llu, colour %llu, STQ %llu; %llu triangles (%llu back faces), ADC differs "
                "%llu; %llu triangles left to the clipper\n",
                (unsigned long long)mesh.checked, (unsigned long long)mesh.inside, (unsigned long long)mesh.bad_xy,
                (unsigned long long)mesh.bad_z, (unsigned long long)mesh.bad_fog, (unsigned long long)mesh.bad_colour,
                (unsigned long long)mesh.bad_stq, (unsigned long long)mesh.triangles_checked,
                (unsigned long long)mesh.backfaces, (unsigned long long)mesh.bad_adc, (unsigned long long)mesh.clipped);
}

/* DHD_NATIVE_VU1_CHECK: after a shadow batch ran on the VU, its item's vertices through the
   shader's maths (native_mesh_vertex, in C) against the GIF packet the program wrote in place:
   per vertex STQ, RGBA, XYZF2 (qwords 0-2), and per triangle the ADC bit against the joins and
   (0170) the back-face rule. Vertices off the guard band and triangles the clipper took are
   counted apart. */
static void mesh_check(void)
{
    const MeshProgram *prog = &mesh_programs[mesh.object];
    const GsMeshHeader *h = (const GsMeshHeader *)mesh.packet;
    const GsMeshVertex *v = (const GsMeshVertex *)(h + 1);
    const ps2_q128 *m = vu1.vu0_mem;
    const float *M = h->matrix;
    unsigned n = h->vertices;
    float sx[GS_MESH_MAX_VERTICES], sy[GS_MESH_MAX_VERTICES];
    int in[GS_MESH_MAX_VERTICES];
    for (unsigned k = 0; k < n; k++) {
        const ps2_q128 *o = &m[(mesh.check_base + 1 + 3 * k) & 1023];
        float c[4];
        for (unsigned r = 0; r < 4; r++) c[r] = M[r] * v[k].x + M[4 + r] * v[k].y + M[8 + r] * v[k].z + M[12 + r];
        float q = 1.0f / c[3], x = c[0] * q, y = c[1] * q, z = c[2] * q;
        float fog = h->fog[0] + h->fog[1] * q;
        fog = fog > h->fog[2] ? h->fog[2] : fog;
        fog = fog < 0 ? 0 : fog;
        sx[k] = x; sy[k] = y;
        float bound = h->clip[3] < 0 ? -h->clip[3] : h->clip[3];
        in[k] = c[3] > 0 && x + h->clip[0] <= bound && x + h->clip[0] >= -bound && y + h->clip[1] <= bound &&
                y + h->clip[1] >= -bound && c[3] + h->clip[2] >= -bound;
        mesh.checked++;
        if (!in[k]) continue;
        mesh.inside++;
        int dx = (int16_t)(uint16_t)((o[2].w[0] & 0xFFFF) - ((uint32_t)(int32_t)(x * 16.0f) & 0xFFFF));
        int dy = (int16_t)(uint16_t)((o[2].w[1] & 0xFFFF) - ((uint32_t)(int32_t)(y * 16.0f) & 0xFFFF));
        if (dx > 1 || dx < -1 || dy > 1 || dy < -1) mesh.bad_xy++;
        int dz = (o[2].sw[2] >> 4) - (int32_t)z;
        if (dz > 1 || dz < -1) mesh.bad_z++;
        int df = (int)(o[2].w[3] >> 4 & 255) - (int)fog;
        if (df > 1 || df < -1) mesh.bad_fog++;
        const uint8_t *rgba = (const uint8_t *)&v[k].rgba;
        for (unsigned i = 0; i < 4; i++) {
            float want = (float)rgba[i] * h->color[i];
            int ref = (int)(want > 255 ? 255 : want), got = o[1].sw[i];
            if (got - ref > 1 || ref - got > 1) { mesh.bad_colour++; break; }
        }
        const float st[3] = {v[k].u * q, v[k].v * q, v[k].uz * q};
        for (unsigned i = 0; i < 3 && (h->prim & 16); i++) {   /* untextured: ST is not written */
            float d = o[0].f[i] - st[i], scale = (st[i] < 0 ? -st[i] : st[i]) + 1e-3f;
            if (d > 1e-4f * scale || d < -1e-4f * scale) { mesh.bad_stq++; break; }
        }
    }
    for (unsigned k = 2; k < n; k++) {
        if (!in[k] || !in[k - 1] || !in[k - 2]) { mesh.clipped++; continue; }
        float area = (sx[k - 1] - sx[k - 2]) * (sy[k] - sy[k - 1]) - (sy[k - 1] - sy[k - 2]) * (sx[k] - sx[k - 1]);
        float s = k & 1 ? -mesh.check_parity : mesh.check_parity;
        int back = prog->backface && s * area > 0, adc = (m[(mesh.check_base + 3 + 3 * k) & 1023].w[3] & 0x8000) != 0;
        mesh.triangles_checked++;
        mesh.backfaces += back && !mesh.join[k];
        if (adc != (mesh.join[k] || back)) mesh.bad_adc++;
    }
}

static int mesh_enabled(void)
{
    static const char *s;
    static int read, forced;
    if (mesh.state) return mesh.state == MESH_ON;
    if (!read) {
        read = 1;
        s = getenv("DHD_NATIVE_VU1");
        /* the loops before the first ':' (comma separated; "prelit" for all of them), flags after */
        size_t list = s ? strcspn(s, ":") : 0;
        for (const char *t = s; t && t < s + list;) {
            size_t len = strcspn(t, ",:");
            if (len == 6 && !strncmp(t, "prelit", 6)) mesh.enabled |= MESH_PRELIT;
            if (len == 3 && !strncmp(t, "lit", 3)) mesh.enabled |= MESH_LIT;
            if (len == 3 && !strncmp(t, "all", 3)) mesh.enabled |= (1u << MESH_PROGRAMS) - 1;
            for (unsigned p = 0; p < MESH_PROGRAMS; p++)
                if (len == 4 && !strncmp(t, mesh_programs[p].name, 4)) mesh.enabled |= 1u << p;
            t += len + (t[len] == ',');
            if (!len) break;
        }
        if (!mesh.enabled) { mesh.state = MESH_OFF; return 0; }
        forced = strstr(s + list, ":skip") ? MESH_SKIP : strstr(s + list, ":shadow") ? MESH_SHADOW : 0;
        mesh.gpu_clip = strstr(s + list, ":gpuclip") != NULL;
    }
    /* The renderer is made at the first vsync (present.c), which can come after the first VU1 runs:
       until field 600 the translation waits for it. A forced mode runs without a mesh renderer too
       (the state trace's headless runs have none): the items are then dropped, and only what the
       translation does to game state is seen. */
    if (!gs_mesh_mode && !forced) {
        if (kernel_vsync_count < 600) return 0;
        hle_log("native VU1: DHD_NATIVE_VU1=%s needs the native renderer with indexed fields; off\n", s);
        mesh.state = MESH_OFF;
        return 0;
    }
    mesh.mode = forced ? forced : gs_mesh_mode == 2 ? MESH_SHADOW : MESH_SKIP;
    mesh.state = MESH_ON;
    mesh.check = mesh.mode == MESH_SHADOW && getenv("DHD_NATIVE_VU1_CHECK") != NULL;
    char names[64] = "";
    for (unsigned p = 0; p < MESH_PROGRAMS; p++)
        if (mesh.enabled >> p & 1) { strcat(names, " "); strcat(names, mesh_programs[p].name); }
    hle_log("native VU1: loops%s drawn by the native renderer, %s (DHD_NATIVE_VU1=%s)%s\n", names,
            mesh.mode == MESH_SHADOW ? "shadow: the VU runs them too, for the exact Z probe" : "the VU skips them", s,
            gs_mesh_mode ? "" : "; no mesh renderer: the items are dropped");
    if (mesh.gpu_clip) hle_log("native VU1: batches the VU would clip are drawn too, clipped by the GPU (:gpuclip)\n");
    atexit(mesh_report);
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
    unsigned t = hdr[3] & 0x7FFF;
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
    for (unsigned col = 0; col < 4; col++) memcpy(&h->matrix[col * 4], vu1.vf[1 + col].f, 16);
    memcpy(h->clip, prog->clip ? vu1.vf[13].f : m[996].f, sizeof h->clip);   /* the loops without CLIP: data 996 */
    const float *v18 = vu1.vf[18].f, *v19 = vu1.vf[19].f;   /* the setup's data 21 and 4 */
    if (prog->colour == MESH_COLOUR_SCALED) memcpy(h->color, vu1.vf[31].f, sizeof h->color);
    else h->color[0] = h->color[1] = h->color[2] = h->color[3] = 1;                 /* min(c, 255) = c */
    if (prog->fog_max) { h->fog[0] = h->fog[2] = v18[1]; h->fog[1] = 0; }          /* clamp(max) = max */
    else { h->fog[0] = v19[3] + v18[0]; h->fog[1] = v18[3]; h->fog[2] = v18[1]; }
    const float uv[2] = {v19[0], v19[1]};
    const float *d5 = m[5].f, *d20 = m[20].f, *d1015 = m[1015].f;
    const float highlight[3] = {m[6].f[3], m[7].f[3], m[8].f[3]};
    /* 0170's prologue sets vf6.x = 1 (0098) and negates it unless the header's bit 15 (0158) */
    float parity = hdr[3] & 0x8000 ? 1.0f : -1.0f;
    mesh.check_parity = parity; mesh.check_base = base;
    GsMeshVertex *out = (GsMeshVertex *)(h + 1);
    uint8_t join[GS_MESH_MAX_VERTICES];
    const float fixed = 1.0f / 4096.0f;   /* ITOF12: exact for these integers */
    /* The guard band test of the loop's CLIP (vf13, data 996) in homogeneous form: a batch with a
       vertex below the near bound goes to the VU (the clipper's fans there are not what a GPU's
       clipping draws), and so does one with a vertex off it in x or y in a loop without CLIP (the
       loop kicks it as it comes, which the GS wraps); the CLIP loops' sideways ones are drawn and
       the GPU clips them. */
    const __m128 c0 = _mm_loadu_ps(&h->matrix[0]), c1 = _mm_loadu_ps(&h->matrix[4]);
    const __m128 c2 = _mm_loadu_ps(&h->matrix[8]), c3 = _mm_loadu_ps(&h->matrix[12]);
    const float bound = h->clip[3] < 0 ? -h->clip[3] : h->clip[3], near = -bound - h->clip[2];
    for (unsigned k = 0; k < n; k++) {
        const ps2_q128 *r = &m[base + 1 + 3 * k];
        const float *p = m[(181 + (r[0].w[3] & 0xFFFF)) & 1023].f;
        if (!mesh.gpu_clip || !prog->clip) {
            float c[4];
            _mm_storeu_ps(c, _mm_add_ps(_mm_add_ps(_mm_mul_ps(c0, _mm_set1_ps(p[0])), _mm_mul_ps(c1, _mm_set1_ps(p[1]))),
                                        _mm_add_ps(_mm_mul_ps(c2, _mm_set1_ps(p[2])), c3)));
            float x = c[0] + h->clip[0] * c[3], y = c[1] + h->clip[1] * c[3], b = bound * c[3];
            if (!(c[3] > 0 && c[3] >= near)) return mesh_fail(MESH_WHY_NEAR);
            /* off the guard band sideways: the loops with clipping cut it as a GPU does (finding 164,
               :gpuside), those without kick it as it comes */
            if (!prog->clip && !(x <= b && x >= -b && y <= b && y >= -b)) return mesh_fail(MESH_WHY_CLIP);
        }
        const uint32_t *c = r[1].w;
        if ((c[0] | c[1] | c[2] | c[3]) > 255) return mesh_fail(MESH_WHY_COLOUR);
        int32_t w = r[2].sw[3];
        if (w != 0 && w != 0x800000) return mesh_fail(MESH_WHY_JOIN);   /* 2048.0: ADC through the fog's FTOI4 */
        join[k] = mesh.join[k] = w != 0;
        GsMeshVertex *o = &out[k];
        o->x = p[0]; o->y = p[1]; o->z = p[2];
        o->u = (float)r[2].sw[0] * fixed; o->v = (float)r[2].sw[1] * fixed; o->uz = (float)r[2].sw[2] * fixed;
        if (prog->uv_offset == 2 || (prog->uv_offset == 1 && k)) { o->u += uv[0]; o->v += uv[1]; }   /* vf19.xy */
        o->unused = 0;
        if (prog->colour == MESH_COLOUR_HIGHLIGHT) {   /* 1510: the pool normal against d6-d8's w */
            const int32_t *nw = m[(373 + (r[0].w[3] & 0xFFFF)) & 1023].sw;
            float d = highlight[0] * ((float)nw[0] * fixed) + highlight[1] * ((float)nw[1] * fixed) +
                      highlight[2] * ((float)nw[2] * fixed);
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
        } else if (prog->colour >= MESH_COLOUR_LIT_D5) {   /* 1110 / 13A0: the lit pool entry */
            const float *L = m[(373 + (r[0].w[3] & 0xFFFF)) & 1023].f, l2 = L[3] * L[3];
            uint32_t rgba = 0;
            for (unsigned i = 0; i < 4; i++) {
                float f = (float)c[i] * (prog->colour == MESH_COLOUR_LIT_D5 ? d5[i] : 1.0f);
                float sum = d20[i] * l2 + (i < 3 ? d1015[i] : 0.0f) + f * (i < 3 ? L[i] : 1.0f);
                if (!(sum > -1.0f)) return mesh_fail(MESH_WHY_COLOUR);   /* FTOI0 of a negative: the GS wraps it */
                rgba |= (uint32_t)(int)(sum > 255 ? 255.0f : sum) << (i * 8);   /* (-1, 0) truncates to 0 */
            }
            o->rgba = rgba;
        } else
            o->rgba = c[0] | c[1] << 8 | c[2] << 16 | c[3] << 24;
    }
    /* the strip's triangles: k-2, k-1, k for each vertex k >= 2 without the join's ADC; with 0170's
       culling each ordered so that the cull (positive screen area, y down) drops what its back-face
       test would */
    uint16_t *index = (uint16_t *)(out + n);
    unsigned triangles = 0;
    for (unsigned k = 2; k < n; k++) {
        if (join[k]) { count->joins++; continue; }
        int keep_order = !prog->backface || (parity > 0) == !(k & 1);
        index[triangles++] = (uint16_t)(keep_order ? k - 2 : k - 1);
        index[triangles++] = (uint16_t)(keep_order ? k - 1 : k - 2);
        index[triangles++] = (uint16_t)k;
    }
    h->vertices = n; h->indices = triangles;
    mesh.words = (unsigned)((sizeof *h + n * sizeof *out + triangles * 2 + 3) / 4);
    count->batches++; count->vertices += n; count->triangles += triangles / 3;
    /* the parity 0170 leaves: one flip a vertex */
    if (prog->backface) vu1.vf[6].f[0] = n & 1 ? -parity : parity;
    return 1;
}

/* Before a VU1 run: 1 when the run is a translated batch (skip mode: do not run it), 2 when it is
   one in shadow mode (run it, its kicks marked shadow, then mesh_send), else 0. *packet: the item. */
static int mesh_before(unsigned addr, int mscnt, uint32_t **packet)
{
    if (!mesh_enabled()) return 0;
    mesh.object = -1;
    if (!mscnt || (addr != 0x1040 && addr != 0x1CB0 && addr != 0x12E0 && addr != 0x0590)) return 0;
    for (unsigned p = 0; p < MESH_PROGRAMS; p++)
        if (vu1.vi[13] == mesh_programs[p].vi13 && (mesh.enabled >> p & 1)) mesh.object = (int)p;
    if (mesh.object < 0 || !mesh_overlay_b()) return mesh.object = -1, 0;
    /* 0590 (after the skinning pass): its JR's delay slot stores the pass's last vertex (SQ vf20,
       -2(vi8)); the VU repeats it in shadow mode */
    if (addr == 0x0590) memcpy(vu1.vu0_mem[(vu1.vi[8] - 2) & 1023].f, vu1.vf[20].f, 16);
    float parity = vu1.vf[6].f[0];
    if (!mesh_build()) return 0;
    *packet = mesh.packet;
    if (mesh.mode == MESH_SHADOW) {
        vu1.vf[6].f[0] = parity;   /* the VU runs the batch itself (0170 flips it there) */
        return 2;
    }
    return 1;
}

static void mesh_send(uint32_t *packet)
{
    static uint64_t report_at = 3000;
    if (mesh.check && mesh.mode == MESH_SHADOW) mesh_check();
    if (kernel_vsync_count >= report_at) {   /* the counts so far, every 3000 fields (and at exit) */
        mesh_report();
        report_at = kernel_vsync_count + 3000;
    }
    graphics_gif_feed(GS_PATH_MESH, packet, mesh.words);
}

/* A skipped batch: the state its end leaves (0FF8, 1000-1030; 0170's parity store at 1014). */
static void mesh_skipped(void)
{
    const MeshProgram *prog = &mesh_programs[mesh.object];
    if (prog->backface) memcpy(vu1.vu0_mem[1014].f, vu1.vf[6].f, 16);
    if (prog->colour == MESH_COLOUR_LIT_D5) {   /* 1110's prologue: vf31 = d20, vf8 = d5, vf26 = d1015 */
        memcpy(vu1.vf[31].f, vu1.vu0_mem[20].f, 16);
        memcpy(vu1.vf[8].f, vu1.vu0_mem[5].f, 16);
        memcpy(vu1.vf[26].f, vu1.vu0_mem[1015].f, 16);
    } else if (prog->colour == MESH_COLOUR_LIT) {   /* 13A0's: vf13 = d1015, vf14 = d20 */
        memcpy(vu1.vf[13].f, vu1.vu0_mem[1015].f, 16);
        memcpy(vu1.vf[14].f, vu1.vu0_mem[20].f, 16);
    } else if (prog->colour == MESH_COLOUR_HIGHLIGHT) {   /* 1510's: vf7 = d17, vf14 = d6, and from its last
                                                              pass vf8 = d18, vf10 = d19 (15E8, 1608) */
        memcpy(vu1.vf[7].f, vu1.vu0_mem[17].f, 16);
        memcpy(vu1.vf[14].f, vu1.vu0_mem[6].f, 16);
        memcpy(vu1.vf[8].f, vu1.vu0_mem[18].f, 16);
        memcpy(vu1.vf[10].f, vu1.vu0_mem[19].f, 16);
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
#endif
