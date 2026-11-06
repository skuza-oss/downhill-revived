/* Native VU1 world meshes (finding 164), included by vif.c: DHD_NATIVE_VU1=0170 hands overlay B's
   prelit textured world mesh loop (VI13 0x11, loop 0170, MSCAL 0078: 37-49 % of VU1 time, finding
   162) to the native renderer instead of running it.

   The object's setup (MSCAL 0078: 1AE0, the matrices) still runs on the VU, so the registers stay
   as the program leaves them; each batch after it (MSCNT at 1CB0 or 1040, VI13 0x11) is read from
   VU1 data memory as the VIF left it and goes to the GS queue as one mesh item (gs_mesh.h): the
   pool positions, colours and texture coordinates of its strip vertices, the triangles the strip
   draws (the input's ADC joins left out, each in the winding the program's back-face test keeps),
   the A+D texture writes and PRIM its packet would make, and the object's constants. The batch's
   microprogram and its XGKICK do not run; what the next run reads is left as the batch leaves it
   (the strip parity in vf6.x and at 1014, the rotating buffer in VI14/VI2, VI4, TPC 1040).

   The batch, as the microcode does it (vudis.py, 0x88-0x268 and 0FA8): header qword at VI2 (the
   strip's GIF tag: NLOOP n with EOP, PRE, PRIM, PACKED ST RGBAQ XYZF2; w: texture triplet offset
   t in bits 0-14, bit 15 the strip's parity), then per vertex three qwords: the pool index (S-8),
   the colour (V4-5), the texture coordinates (V2-16 as 12-bit fixed point; z the Q factor, w
   2048.0 at a strip join, which the fog's FTOI4 turns into ADC). A vertex k is clip = C (x y z 1)
   with C the setup's vf1..vf4; q = 1 / clip.w; XYZ = FTOI4(clip.xyz q); fog = clamp(vf19.w +
   vf18.x + vf18.w q, 0, vf18.y); colour = min(c vf31, 255); STQ = (u, v, uz) q with vf19.xy added
   to u, v past vertex 0. ADC also when back-facing: with P = 1 when the header's bit 15 is set,
   else -1 (the prologue sets vf6.x to 1, then negates it), triangle (k-2, k-1, k) is culled when
   P (-1)^k times its screen area (y down) is positive. CLIP against vf13 (data 996) sends a
   triangle to the clipper (1948) when any vertex is off the guard band in x or y or below the near
   bound. The clipper's fans do not come out as a GPU clips (their projection differs over the near
   ground in fast race frames), so a batch with such a vertex runs on the VU: the same test on the
   CPU, in homogeneous form, before the batch is translated (about a quarter of the batches).
   DHD_NATIVE_VU1=0170:gpuclip draws those too and leaves them to the GPU's clipping (a measurement).

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
#else
enum { MESH_OFF = 1, MESH_ON = 2, MESH_SKIP = 1, MESH_SHADOW = 2 };
enum { MESH_WHY_EOP, MESH_WHY_COUNT, MESH_WHY_TAG, MESH_WHY_PRIM, MESH_WHY_TEXTURE, MESH_WHY_COLOUR, MESH_WHY_JOIN,
       MESH_WHY_CLIP, MESH_WHYS };
static const char *const mesh_why_names[MESH_WHYS] = {"no EOP", "count", "tag", "prim", "texture tag", "colour",
                                                     "join value", "a vertex for the clipper"};
static struct {
    int state;                 /* 0 unread, MESH_OFF, MESH_ON */
    int mode;                  /* MESH_SKIP or MESH_SHADOW */
    int object;                /* the open MSCAL object is a 0078 one: its batches are translated */
    int gpu_clip;              /* :gpuclip: batches the VU would clip are drawn too (the GPU clips them) */
    uint64_t code_version; int overlay_b;
    float matrix[16], clip[4], color[4], fog[4], uv[2];
    uint64_t objects, batches, fallbacks, vertices, triangles, culled_joins, why[MESH_WHYS];
    /* the queue item: header, vertices, indices */
    uint32_t packet[(sizeof(GsMeshHeader) + GS_MESH_MAX_VERTICES * sizeof(GsMeshVertex) +
                     GS_MESH_MAX_VERTICES * 3 * 2 + 4) / 4];
    unsigned words;
    /* DHD_NATIVE_VU1_CHECK=1 (shadow mode): each item against what the program wrote (mesh_check) */
    int check;
    unsigned check_base; float check_parity; uint8_t join[GS_MESH_MAX_VERTICES];
    uint64_t checked, inside, bad_xy, bad_z, bad_fog, bad_colour, bad_stq, triangles_checked, clipped, bad_adc,
             backfaces;
} mesh;

static void mesh_report(void)
{
    hle_log("native VU1 0170 (%s): %llu objects, %llu batches translated, %llu run on the VU; %llu vertices, "
            "%llu triangles, %llu join vertices\n", mesh.mode == MESH_SHADOW ? "shadow" : "skip",
            (unsigned long long)mesh.objects, (unsigned long long)mesh.batches, (unsigned long long)mesh.fallbacks,
            (unsigned long long)mesh.vertices, (unsigned long long)mesh.triangles, (unsigned long long)mesh.culled_joins);
    for (unsigned i = 0; i < MESH_WHYS; i++)
        if (mesh.why[i]) hle_log("native VU1 0170: %llu batches on the VU: %s\n", (unsigned long long)mesh.why[i], mesh_why_names[i]);
    if (mesh.check)
        hle_log("native VU1 0170 check: %llu vertices (%llu inside the guard band): XY off by more than 1/16 px %llu, "
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
   the back-face rule. Vertices off the guard band and triangles the clipper took are counted apart. */
static void mesh_check(void)
{
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
        for (unsigned i = 0; i < 3; i++) {
            float d = o[0].f[i] - st[i], scale = (st[i] < 0 ? -st[i] : st[i]) + 1e-3f;
            if (d > 1e-4f * scale || d < -1e-4f * scale) { mesh.bad_stq++; break; }
        }
    }
    for (unsigned k = 2; k < n; k++) {
        if (!in[k] || !in[k - 1] || !in[k - 2]) { mesh.clipped++; continue; }
        float area = (sx[k - 1] - sx[k - 2]) * (sy[k] - sy[k - 1]) - (sy[k - 1] - sy[k - 2]) * (sx[k] - sx[k - 1]);
        float s = k & 1 ? -mesh.check_parity : mesh.check_parity;
        int back = s * area > 0, adc = (m[(mesh.check_base + 3 + 3 * k) & 1023].w[3] & 0x8000) != 0;
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
        if (!s || strncmp(s, "0170", 4)) { mesh.state = MESH_OFF; return 0; }
        forced = strstr(s + 4, ":skip") ? MESH_SKIP : strstr(s + 4, ":shadow") ? MESH_SHADOW : 0;
        mesh.gpu_clip = strstr(s + 4, ":gpuclip") != NULL;
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
    hle_log("native VU1: loop 0170 (MSCAL 0078) drawn by the native renderer, %s (DHD_NATIVE_VU1=%s)%s\n",
            mesh.mode == MESH_SHADOW ? "shadow: the VU runs it too, for the exact Z probe" : "the VU skips it", s,
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

static int mesh_fail(unsigned why) { mesh.why[why]++; mesh.fallbacks++; return 0; }

/* The batch at VI2 as a mesh item in mesh.packet; 0 (and nothing built) for a form the
   translation does not know, which then runs on the VU. */
static int mesh_build(void)
{
    const ps2_q128 *m = vu1.vu0_mem;
    unsigned base = vu1.vi[2] & 1023;
    const uint32_t *hdr = m[base].w;
    unsigned n = hdr[0] & 0x7FFF;
    if (!(hdr[0] & 0x8000)) return mesh_fail(MESH_WHY_EOP);
    if (n < 1 || n > GS_MESH_MAX_VERTICES || base + 1 + 3 * n > 1024) return mesh_fail(MESH_WHY_COUNT);
    /* PACKED, NREG 3: ST RGBAQ XYZF2, PRE */
    if ((hdr[1] >> 26 & 3) || hdr[1] >> 28 != 3 || (hdr[2] & 0xFFF) != 0x412 || !(hdr[1] >> 14 & 1))
        return mesh_fail(MESH_WHY_TAG);
    unsigned prim = (unsigned)((hdr[0] | (uint64_t)hdr[1] << 32) >> 47 & 0x7FF);
    if ((prim & 0x107) != 4) return mesh_fail(MESH_WHY_PRIM);   /* a triangle strip with ST */
    GsMeshHeader *h = (GsMeshHeader *)mesh.packet;
    memset(h, 0, sizeof *h);
    h->magic = GS_MESH_MAGIC;
    h->flags = mesh.mode == MESH_SHADOW ? GS_MESH_SHADOW : 0;
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
    memcpy(h->matrix, mesh.matrix, sizeof h->matrix);
    memcpy(h->clip, mesh.clip, sizeof h->clip);
    memcpy(h->color, mesh.color, sizeof h->color);
    memcpy(h->fog, mesh.fog, sizeof h->fog);
    /* the prologue sets vf6.x = 1 (0098) and negates it unless the header's bit 15 (0158) */
    float parity = hdr[3] & 0x8000 ? 1.0f : -1.0f;
    mesh.check_parity = parity; mesh.check_base = base;
    GsMeshVertex *out = (GsMeshVertex *)(h + 1);
    uint8_t join[GS_MESH_MAX_VERTICES];
    const float fixed = 1.0f / 4096.0f;   /* ITOF12: exact for these integers */
    /* The guard band test of the loop's CLIP (vf13, data 996) in homogeneous form: a batch with a
       vertex off it in x or y or below the near bound goes to the VU, whose clipper the GPU's
       clipping does not reproduce (the speed blur over the near ground differs; finding 164). */
    const __m128 c0 = _mm_loadu_ps(&mesh.matrix[0]), c1 = _mm_loadu_ps(&mesh.matrix[4]);
    const __m128 c2 = _mm_loadu_ps(&mesh.matrix[8]), c3 = _mm_loadu_ps(&mesh.matrix[12]);
    const float bound = mesh.clip[3] < 0 ? -mesh.clip[3] : mesh.clip[3], near = -bound - mesh.clip[2];
    for (unsigned k = 0; k < n; k++) {
        const ps2_q128 *r = &m[base + 1 + 3 * k];
        const float *p = m[(181 + (r[0].w[3] & 0xFFFF)) & 1023].f;
        if (!mesh.gpu_clip) {
            float c[4];
            _mm_storeu_ps(c, _mm_add_ps(_mm_add_ps(_mm_mul_ps(c0, _mm_set1_ps(p[0])), _mm_mul_ps(c1, _mm_set1_ps(p[1]))),
                                        _mm_add_ps(_mm_mul_ps(c2, _mm_set1_ps(p[2])), c3)));
            float x = c[0] + mesh.clip[0] * c[3], y = c[1] + mesh.clip[1] * c[3], b = bound * c[3];
            if (!(c[3] > 0 && c[3] >= near && x <= b && x >= -b && y <= b && y >= -b)) return mesh_fail(MESH_WHY_CLIP);
        }
        const uint32_t *c = r[1].w;
        if ((c[0] | c[1] | c[2] | c[3]) > 255) return mesh_fail(MESH_WHY_COLOUR);
        int32_t w = r[2].sw[3];
        if (w != 0 && w != 0x800000) return mesh_fail(MESH_WHY_JOIN);   /* 2048.0: ADC through the fog's FTOI4 */
        join[k] = mesh.join[k] = w != 0;
        GsMeshVertex *o = &out[k];
        o->x = p[0]; o->y = p[1]; o->z = p[2];
        o->u = (float)r[2].sw[0] * fixed; o->v = (float)r[2].sw[1] * fixed; o->uz = (float)r[2].sw[2] * fixed;
        if (k) { o->u += mesh.uv[0]; o->v += mesh.uv[1]; }   /* vf19.xy, added from vertex 1 on (0x1A0) */
        o->unused = 0;
        o->rgba = c[0] | c[1] << 8 | c[2] << 16 | c[3] << 24;
    }
    /* the strip's triangles: k-2, k-1, k for each vertex k >= 2 without the join's ADC, ordered so
       that the cull (positive screen area, y down) drops what the back-face test would */
    uint16_t *index = (uint16_t *)(out + n);
    unsigned count = 0;
    for (unsigned k = 2; k < n; k++) {
        if (join[k]) { mesh.culled_joins++; continue; }
        int keep_order = (parity > 0) == !(k & 1);
        index[count++] = (uint16_t)(keep_order ? k - 2 : k - 1);
        index[count++] = (uint16_t)(keep_order ? k - 1 : k - 2);
        index[count++] = (uint16_t)k;
    }
    h->vertices = n; h->indices = count;
    mesh.words = (unsigned)((sizeof *h + n * sizeof *out + count * 2 + 3) / 4);
    mesh.batches++; mesh.vertices += n; mesh.triangles += count / 3;
    /* the parity the batch leaves: one flip a vertex */
    vu1.vf[6].f[0] = n & 1 ? -parity : parity;
    return 1;
}

/* Before a VU1 run: 1 when the run is a translated batch (skip mode: do not run it), 2 when it is
   one in shadow mode (run it, its kicks marked shadow, then mesh_send), else 0. *packet: the item. */
static int mesh_before(unsigned addr, int mscnt, uint32_t **packet)
{
    if (!mesh_enabled()) return 0;
    if (!mscnt) {   /* a new object: 0078 setups are translated from their batches on */
        mesh.object = addr == 0x78 && mesh_overlay_b();
        return 0;
    }
    if (!mesh.object || (addr != 0x1CB0 && addr != 0x1040) || vu1.vi[13] != 0x11) return 0;
    float parity = vu1.vf[6].f[0];
    if (!mesh_build()) return 0;
    *packet = mesh.packet;
    if (mesh.mode == MESH_SHADOW) {
        vu1.vf[6].f[0] = parity;   /* the VU runs the batch itself and flips it there */
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

/* A skipped batch: the state its end leaves (1000-1030, the parity store at 1014). */
static void mesh_skipped(void)
{
    memcpy(vu1.vu0_mem[1014].f, vu1.vf[6].f, 16);
    uint16_t buffer = (uint16_t)(vu1.vi[14] + 0x90);
    if (buffer == 0x3E8) buffer = 0x238;
    vu1.vi[14] = vu1.vi[2] = buffer;
    vu1.vi[3] = 0x3E8;
    vu1.vi[4] = 0x7FFF;
    vu1.vu0_ctrl[10] = 0x1040;
}

/* After a VU1 run that was not skipped: an 0078 setup's constants for its batches. */
static void mesh_after(unsigned addr, int mscnt)
{
    if (mscnt || !mesh.object || addr != 0x78) return;
    mesh.objects++;
    for (unsigned col = 0; col < 4; col++) memcpy(&mesh.matrix[col * 4], vu1.vf[1 + col].f, 16);
    memcpy(mesh.clip, vu1.vf[13].f, 16);
    memcpy(mesh.color, vu1.vf[31].f, 16);
    const float *v18 = vu1.vf[18].f, *v19 = vu1.vf[19].f;   /* data 21 and 4 */
    mesh.fog[0] = v19[3] + v18[0]; mesh.fog[1] = v18[3]; mesh.fog[2] = v18[1]; mesh.fog[3] = 0;
    mesh.uv[0] = v19[0]; mesh.uv[1] = v19[1];
}
#endif
