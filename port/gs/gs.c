/* GIF paths, GS registers, image transfers and palette storage.
   Behaviour reference: PCSX2 GSState.cpp and GSClut.cpp (GPL-3.0+).
   Each path keeps its tag across DMA boundaries, including partial qwords. */
#include "gs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

Gs gs;
/* Optional exact readback transcript, keyed by virtual GS field rather than
   wall time. Includes deferred lens-flare Z reads and immediate colour reads. */
static unsigned readback_field;
static void trace_readback(uint64_t blt, uint64_t pos, uint64_t size, const void *data, size_t bytes)
{
    static int initialized;
    static FILE *file;
    if (!initialized) {
        initialized = 1;
        const char *path = getenv("DHD_GS_READBACK_TRACE");
        if (path) { file = fopen(path, "wb"); if (!file) { perror(path); abort(); } }
    }
    if (!file) return;
    uint32_t header[2] = {readback_field, (uint32_t)bytes};
    uint64_t regs[3] = {blt, pos, size};
    if (fwrite(header, sizeof header, 1, file) != 1 || fwrite(regs, sizeof regs, 1, file) != 1 ||
        fwrite(data, 1, bytes, file) != bytes || fflush(file)) { perror("readback trace"); abort(); }
}

static unsigned field(uint64_t v, unsigned shift, unsigned mask) { return (unsigned)(v >> shift) & mask; }
static void sync_memory(Gs *g) { if (g->sync) g->sync(g); }

void gs_reset(Gs *g)
{
    memset(g, 0, sizeof *g);
    readback_field = 0;
    g->reg[GS_PRMODECONT] = 1;
    g->reg[GS_TRXDIR] = 3;
    g->vertex.q = 1;
    g->privileged[GS_CSR / 16] = 0x551B4000;  /* GS revision, ID and empty FIFO */
    g->privileged[GS_IMR / 16] = 0x7F00;
}

static void load_clut(Gs *g, uint64_t tex)
{
    unsigned psm = field(tex, 20, 63), cpsm = field(tex, 51, 15), cbp = field(tex, 37, 0x3FFF);
    unsigned cld = field(tex, 61, 7), csa = field(tex, 56, 31), csm = field(tex, 55, 1);
    unsigned count, start, i;
    static const unsigned t32[16] = {0,1,4,5,8,9,12,13,2,3,6,7,10,11,14,15};
    static const unsigned t16[32] = {
        0,2,8,10,16,18,24,26,4,6,12,14,20,22,28,30,
        1,3,9,11,17,19,25,27,5,7,13,15,21,23,29,31
    };
    if (psm != GS_PSMT8 && psm != GS_PSMT4 && psm != GS_PSMT8H && psm != GS_PSMT4HL && psm != GS_PSMT4HH)
        return;
    if (!cld || cld >= 6) return;
    if (cld >= 4 && g->cbp[cld - 4] == cbp) return;
    if (cld >= 2) g->cbp[(cld - 2) & 1] = (uint16_t)cbp;
    if (cpsm != GS_PSMCT32 && cpsm != GS_PSMCT16 && cpsm != GS_PSMCT16S && !(csm && cpsm == GS_PSMCT24))
        return;
    count = psm == GS_PSMT8 || psm == GS_PSMT8H ? 256 : 16;
    if (!g->sync_range) sync_memory(g);
    else if (!csm) g->sync_range(g, cbp * 256, 1024);
    else {
        uint64_t cl = g->reg[GS_TEXCLUT];
        for (i = 0; i < count; i++)
            g->sync_range(g, gs_pixel_address(cpsm, cbp, field(cl, 0, 63),
                          field(cl, 6, 63) * 16 + i, field(cl, 12, 1023)) / 8, 4);
    }
    start = (csa & ((cpsm & 2) ? 31 : 15)) * 16;
    if (!csm) {
        /* CSM1: the entries are distinct, so only the number of changed ones matters for
           state_version; counted without a branch per entry. */
        uint16_t low[256], high[256];
        unsigned first = 0, changed = 0;
        if (cpsm & 2) {
            for (i = 0; i < count; i++) {
                unsigned a = cbp * 256 + ((i & ~31u) + t16[i & 31]) * 2;
                low[i] = (uint16_t)(g->vram[(a & (GS_VRAM_SIZE - 1)) / 4] >> ((a & 2) * 8));
            }
            for (i = 0; i < count; i++) {
                uint16_t *entry = &g->clut[(start + i) & 511];
                changed += *entry != low[i];
                *entry = low[i];
            }
        } else {
            if (count == 256) first = start, start = 0;
            for (i = first; i < count; i++) {
                unsigned src = count == 256 ? (i & 0x80) | (i & 0x10) << 2 | (i & 0x60) >> 1 | t32[i & 15] : t32[i];
                uint32_t value = g->vram[(cbp * 64 + src) & (GS_VRAM_SIZE / 4 - 1)];
                low[i] = (uint16_t)value; high[i] = (uint16_t)(value >> 16);
            }
            for (i = first; i < count; i++) {
                uint16_t *lo = &g->clut[start + i], *hi = &g->clut[start + i + 256];
                changed += (*lo != low[i]) + (*hi != high[i]);
                *lo = low[i]; *hi = high[i];
            }
        }
        g->state_version += changed;
        return;
    }
    for (i = 0; i < count; i++) {
        unsigned dst = start + i, value;
        if (csm) {
            uint64_t cl = g->reg[GS_TEXCLUT];
            value = gs_read_pixel(g, cpsm == GS_PSMCT24 ? GS_PSMCT32 : cpsm, cbp,
                                  field(cl, 0, 63), field(cl, 6, 63) * 16 + i, field(cl, 12, 1023));
        } else if (cpsm & 2) {
            unsigned a = cbp * 256 + ((i & ~31u) + t16[i & 31]) * 2;
            value = (g->vram[(a & (GS_VRAM_SIZE - 1)) / 4] >> ((a & 2) * 8)) & 0xFFFF;
        } else {
            unsigned src = i;
            if (count == 256) {
                dst = i;
                if (i < start) continue;
                src = (i & 0x80) | (i & 0x10) << 2 | (i & 0x60) >> 1 | t32[i & 15];
            } else src = t32[i];
            value = g->vram[(cbp * 64 + src) & (GS_VRAM_SIZE / 4 - 1)];
        }
        if (g->clut[dst & 511] != (uint16_t)value) g->state_version++;
        g->clut[dst & 511] = (uint16_t)value;
        if (!(cpsm & 2)) {
            if (g->clut[(dst + 256) & 511] != (uint16_t)(value >> 16)) g->state_version++;
            g->clut[(dst + 256) & 511] = (uint16_t)(value >> 16);
        }
    }
}

uint32_t gs_palette(const Gs *g, uint64_t tex, unsigned index)
{
    unsigned cpsm = field(tex, 51, 15), csa = field(tex, 56, 31), value;
    if (cpsm & 2)
        value = g->clut[(csa * 16 + index) & 511];
    else {
        unsigned group = (csa & 15) * 16 + (index & ~15u);
        unsigned at = (group > 240 ? 240 : group) + (index & 15);
        value = g->clut[at] | (uint32_t)g->clut[at + 256] << 16;
    }
    return gs_color(value, cpsm, g->reg[GS_TEXA]);
}

int image_reference;  /* tests: 1 sends every host-to-local byte through the bit accumulator */

static void image_data(Gs *g, const uint8_t *data, unsigned bytes)
{
    uint64_t blt = g->reg[GS_BITBLTBUF], pos = g->reg[GS_TRXPOS], size = g->reg[GS_TRXREG];
    unsigned psm = field(blt, 56, 63), bp = field(blt, 32, 0x3FFF), bw = field(blt, 48, 63);
    unsigned width = field(size, 0, 4095), height = field(size, 32, 4095), bits = gs_bits_per_pixel(psm);
    if ((g->reg[GS_TRXDIR] & 3) != 0 || !width || !height) return;
    if (g->flush_draw) g->flush_draw(g);
    if (!g->sync_range) sync_memory(g);
    unsigned total = width * height;
    if (!g->transfer_have && g->transfer_count < total && !image_reference) {
        /* Whole pixels from a byte boundary: the same writes as the loop below, without the bit
           accumulator. What is left (a 24-bit pixel cut by the end, an odd last nibble) goes
           through the loop, which starts from the same empty accumulator. */
        unsigned pixels = bits == 24 ? bytes / 3 : bytes * 8 / bits;
        if (pixels > total - g->transfer_count) pixels = total - g->transfer_count;
        if (bits == 4) pixels &= ~1u;
        gs_write_pixels(g, psm, bp, bw, field(pos, 32, 2047), field(pos, 48, 2047), width, g->transfer_count, pixels,
                        data);
        g->transfer_count += pixels;
        data += pixels * bits / 8;
        bytes -= pixels * bits / 8;
    }
    for (unsigned i = 0; i < bytes && g->transfer_count < width * height; i++) {
        g->transfer_bits |= (uint64_t)data[i] << g->transfer_have;
        g->transfer_have += 8;
        while (g->transfer_have >= bits && g->transfer_count < width * height) {
            unsigned x = field(pos, 32, 2047) + g->transfer_count % width;
            unsigned y = field(pos, 48, 2047) + g->transfer_count / width;
            gs_write_pixel(g, psm, bp, bw, x & 2047, y & 2047, (uint32_t)g->transfer_bits);
            g->transfer_bits >>= bits;
            g->transfer_have -= bits;
            g->transfer_count++;
        }
    }
}

typedef struct { uint64_t bits; uint32_t count, have; } Transfer;

/* Local-to-host transfers pack pixels consecutively, including 24-bit and
   nibble formats. Keep partial pixels across FIFO/DMA boundaries. */
static size_t pack_pixels(const uint32_t *vram, uint64_t blt, uint64_t pos, uint64_t size, Transfer *t,
                          uint8_t *out, size_t bytes)
{
    unsigned psm = field(blt, 24, 63), bp = field(blt, 0, 0x3FFF), bw = field(blt, 16, 63);
    unsigned width = field(size, 0, 4095), height = field(size, 32, 4095), bits = gs_bits_per_pixel(psm);
    size_t written = 0;
    while (written < bytes) {
        while (t->have < 8 && t->count < width * height) {
            unsigned x = (field(pos, 0, 2047) + t->count % width) & 2047;
            unsigned y = (field(pos, 16, 2047) + t->count / width) & 2047;
            uint32_t pixel = gs_image_pixel(vram, psm, bp, bw, x, y);
            t->bits |= (uint64_t)pixel << t->have;
            t->have += bits;
            t->count++;
        }
        if (!t->have) break;
        out[written++] = (uint8_t)t->bits;
        t->bits >>= 8;
        t->have = t->have >= 8 ? t->have - 8 : 0;
    }
    return written;
}

size_t gs_read_image(Gs *g, void *destination, size_t bytes)
{
    uint64_t blt = g->reg[GS_BITBLTBUF], pos = g->reg[GS_TRXPOS], size = g->reg[GS_TRXREG];
    unsigned psm = field(blt, 24, 63), bp = field(blt, 0, 0x3FFF), bw = field(blt, 16, 63);
    unsigned width = field(size, 0, 4095), height = field(size, 32, 4095), bits = gs_bits_per_pixel(psm);
    if ((g->reg[GS_TRXDIR] & 3) != 1 || !width || !height || !bits) return 0;
    if (!g->sync_range) sync_memory(g);
    else if (!g->transfer_count && !g->transfer_have) {
        /* The pages the rectangle covers: GS pages are at least 64x32 pixels, so a 16-pixel grid
           plus the far edges touches each of them. */
        unsigned low = UINT32_MAX, high = 0;
        for (unsigned y = 0;; y = y + 16 < height - 1 ? y + 16 : height - 1) {
            for (unsigned x = 0;; x = x + 16 < width - 1 ? x + 16 : width - 1) {
                unsigned page = gs_pixel_address(psm, bp, bw, (field(pos, 0, 2047) + x) & 2047,
                                                 (field(pos, 16, 2047) + y) & 2047) / (8192 * 8);
                if (page < low) low = page;
                if (page > high) high = page;
                if (x == width - 1) break;
            }
            if (y == height - 1) break;
        }
        g->sync_range(g, low * 8192, (high - low + 1) * 8192);
    }
    Transfer t = {g->transfer_bits, g->transfer_count, g->transfer_have};
    size_t written = pack_pixels(g->vram, blt, pos, size, &t, destination, bytes);
    g->transfer_bits = t.bits; g->transfer_count = t.count; g->transfer_have = t.have;
    trace_readback(blt, pos, size, destination, written);
    return written;
}

/* Deferred reads. The game reads the Z buffer under its lens flares every frame
   (sceGsExecStoreImage from func_00228328) and uses the result in the next frame's
   func_002286F8; waiting for the GPU there put a frame's raster time after its CPU time. The
   pages a read needs are captured when it is issued: GPU-written ones by an ordered GPU copy,
   the rest from the CPU image. Settling decodes them with the same packing as gs_read_image. */
enum { MAX_DEFERRED = 256 };
static struct {
    struct { uint8_t *destination; size_t bytes; uint64_t blt, pos, size; } read[MAX_DEFERRED];
    unsigned count;
    uint32_t captured[16], staged[16];
    uint64_t epoch[512];     /* VRAM state each captured page was taken at */
    uint32_t image[GS_VRAM_SIZE / 4];
} deferred;

static uint64_t vram_state(const Gs *g) { return g->vram_epoch + g->pixels_written; }

int gs_reads_pending(const Gs *g) { (void)g; return deferred.count != 0; }

size_t gs_defer_read_image(Gs *g, void *destination, size_t bytes)
{
    uint64_t blt = g->reg[GS_BITBLTBUF], pos = g->reg[GS_TRXPOS], size = g->reg[GS_TRXREG];
    unsigned psm = field(blt, 24, 63), bp = field(blt, 0, 0x3FFF), bw = field(blt, 16, 63);
    unsigned width = field(size, 0, 4095), height = field(size, 32, 4095), bits = gs_bits_per_pixel(psm);
    uint32_t want[16] = {0}, staged[16];
    /* Only a whole fresh transfer of a small rectangle: it leaves no partial pixels behind. */
    if (!g->stage_pages || (g->reg[GS_TRXDIR] & 3) != 1 || !width || !height || !bits ||
        g->transfer_count || g->transfer_have || width * height > 4096 ||
        (uint64_t)bytes * 8 != (uint64_t)width * height * bits) return 0;
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++) {
            unsigned page = gs_pixel_address(psm, bp, bw, (field(pos, 0, 2047) + x) & 2047,
                                             (field(pos, 16, 2047) + y) & 2047) / (8192 * 8);
            want[page / 32] |= 1u << (page & 31);
        }
    uint64_t now = vram_state(g);
    int stale = deferred.count == MAX_DEFERRED;
    for (unsigned page = 0; page < 512; page++)
        if (want[page / 32] & deferred.captured[page / 32] & (1u << (page & 31)) && deferred.epoch[page] != now)
            stale = 1;
    if (stale) return 0;  /* the caller settles the earlier reads and tries again */
    for (unsigned i = 0; i < 16; i++) want[i] &= ~deferred.captured[i];
    g->stage_pages(g, want, staged);
    for (unsigned page = 0; page < 512; page++) {
        uint32_t bit = 1u << (page & 31);
        if (!(want[page / 32] & bit)) continue;
        if (!(staged[page / 32] & bit))
            memcpy((uint8_t *)deferred.image + page * 8192, (uint8_t *)g->vram + page * 8192, 8192);
        deferred.captured[page / 32] |= bit;
        deferred.staged[page / 32] |= staged[page / 32] & bit;
        deferred.epoch[page] = now;
    }
    deferred.read[deferred.count++] = (typeof(deferred.read[0])){destination, bytes, blt, pos, size};
    /* The state gs_read_image leaves after the whole transfer. */
    g->transfer_count = width * height;
    g->transfer_have = 0;
    g->transfer_bits = 0;
    return bytes;
}

void gs_settle_reads(Gs *g)
{
    int any = 0;
    if (!deferred.count) return;
    for (unsigned i = 0; i < 16; i++) any |= deferred.staged[i] != 0;
    if (any) g->fetch_pages(g, deferred.staged, deferred.image);
    for (unsigned i = 0; i < deferred.count; i++) {
        Transfer t = {0, 0, 0};
        size_t written = pack_pixels(deferred.image, deferred.read[i].blt, deferred.read[i].pos, deferred.read[i].size, &t,
                    deferred.read[i].destination, deferred.read[i].bytes);
        trace_readback(deferred.read[i].blt, deferred.read[i].pos, deferred.read[i].size,
                       deferred.read[i].destination, written);
    }
    deferred.count = 0;
    memset(deferred.captured, 0, sizeof deferred.captured);
    memset(deferred.staged, 0, sizeof deferred.staged);
}

static void local_copy(Gs *g)
{
    uint64_t b = g->reg[GS_BITBLTBUF], p = g->reg[GS_TRXPOS], r = g->reg[GS_TRXREG];
    unsigned width = field(r, 0, 4095), height = field(r, 32, 4095);
    g->vram_epoch++;
    if (g->local_copy && g->local_copy(g)) return;
    sync_memory(g);
    for (unsigned y = 0; y < height; y++) for (unsigned x = 0; x < width; x++) {
        unsigned dx = p >> 59 & 1 ? width - 1 - x : x, dy = p >> 60 & 1 ? height - 1 - y : y;
        uint32_t v = gs_read_pixel(g, field(b, 24, 63), field(b, 0, 0x3FFF), field(b, 16, 63),
                                  (field(p, 0, 2047) + dx) & 2047, (field(p, 16, 2047) + dy) & 2047);
        gs_write_pixel(g, field(b, 56, 63), field(b, 32, 0x3FFF), field(b, 48, 63),
                       (field(p, 32, 2047) + dx) & 2047, (field(p, 48, 2047) + dy) & 2047, v);
    }
}

static void vertex_kick(Gs *g, int skip)
{
    if (skip) g->adc_skips++;
    unsigned prim = (unsigned)g->reg[GS_PRIM] & 7, count;
    static const unsigned needed[8] = {1,2,2,3,3,3,2,1};
    g->vertices[g->vertex_count++] = g->vertex;
    if (g->vertex_count < needed[prim]) return;
    count = g->vertex_count;
    g->kicks++;
    if (!skip && prim != 7 && g->draw) g->vram_epoch++, g->draw(g, prim, g->vertices, count);
    if (prim == 2) { g->vertices[0] = g->vertices[1]; g->vertex_count = 1; }
    else if (prim == 4) {
        g->vertices[0] = g->vertices[1]; g->vertices[1] = g->vertices[2];
        g->vertex_count = 2; g->strip_parity ^= 1;
    } else if (prim == 5) { g->vertices[1] = g->vertices[2]; g->vertex_count = 2; }
    else g->vertex_count = 0;
}

void gs_write(Gs *g, unsigned reg, uint64_t value)
{
    reg &= 127;
    switch (reg) {
    case GS_TEX0_1: case GS_TEX0_2: case GS_CLAMP_1: case GS_CLAMP_2:
    case GS_TEX1_1: case GS_TEX1_2: case GS_TEXA: case GS_SCANMSK: case GS_FOGCOL:
    case GS_MIPTBP1_1: case GS_MIPTBP1_2: case GS_MIPTBP2_1: case GS_MIPTBP2_2:
    case GS_SCISSOR_1: case GS_SCISSOR_2: case GS_ALPHA_1: case GS_ALPHA_2:
    case GS_DIMX: case GS_DTHE: case GS_COLCLAMP: case GS_TEST_1: case GS_TEST_2:
    case GS_PABE: case GS_FBA_1: case GS_FBA_2: case GS_FRAME_1: case GS_FRAME_2:
    case GS_ZBUF_1: case GS_ZBUF_2:
        if (g->reg[reg] != value) g->state_version++;
        break;
    }
    g->reg[reg] = value;
    switch (reg) {
    case GS_PRIM: g->vertex_count = g->strip_parity = 0; break;
    case GS_RGBAQ:
        g->vertex.rgba = (uint32_t)value;
        { uint32_t q = (uint32_t)(value >> 32); memcpy(&g->vertex.q, &q, 4); }
        break;
    case GS_ST: memcpy(&g->vertex.s, &value, 8); break;
    case GS_UV: g->vertex.u = field(value, 0, 0x3FFF); g->vertex.v = field(value, 16, 0x3FFF); break;
    case GS_FOG: g->vertex.fog = field(value, 56, 255); break;
    case GS_XYZF2: case GS_XYZ2: case GS_XYZF3: case GS_XYZ3:
        g->vertex.x = field(value, 0, 65535); g->vertex.y = field(value, 16, 65535);
        g->vertex.z = (uint32_t)(value >> 32);
        if (reg == GS_XYZF2 || reg == GS_XYZF3) {
            g->vertex.z &= 0xFFFFFF; g->vertex.fog = (unsigned)(value >> 56);
        }
        vertex_kick(g, reg == GS_XYZF3 || reg == GS_XYZ3);
        break;
    case GS_TEX0_1: case GS_TEX0_2: {
        unsigned ctxt = reg - GS_TEX0_1, tw = field(value, 26, 15);
        unsigned psm = field(value, 20, 63), bits = gs_bits_per_pixel(psm);
        /* MTBA uses square textures derived from TW, ignoring TH and TBW.
           High-byte indexed formats occupy 32-bit storage. */
        if (psm == 1 || psm == 27 || psm == 36 || psm == 44) bits = 32;
        if ((g->reg[GS_TEX1_1 + ctxt] & 512) && tw >= 5 && tw <= (bits < 32 ? 10u : 9u) && (psm & 48) != 48) {
            unsigned bp = field(value, 0, 16383), bw = (1u << tw) >> 6;
            unsigned size = ((1u << tw) * (1u << tw) * bits) >> 11;
            uint64_t mip = 0;
            if (!bw) bw = 1;
            for (unsigned level = 0; level < 3; level++) {
                bp += size; bw >>= 1; if (!bw) bw = 1;
                mip |= (uint64_t)((bp & 16383) | (bw & 63) << 14) << (level * 20);
                size >>= 2; if (!size) size = 1;
            }
            gs_write(g, GS_MIPTBP1_1 + ctxt, mip);
        }
        load_clut(g, value); break;
    }
    case GS_TEX2_1: case GS_TEX2_2: {
        unsigned t = reg - GS_TEX2_1 + GS_TEX0_1;
        uint64_t mask = UINT64_C(0xFFFFFFE003F00000);
        if ((g->reg[t] & mask) != (value & mask)) g->state_version++;
        g->reg[t] = (g->reg[t] & ~mask) | (value & mask);
        load_clut(g, g->reg[t]);
        break;
    }
    case GS_TRXDIR:
        g->transfer_count = g->transfer_have = 0; g->transfer_bits = 0;
        if ((value & 3) == 0) g->uploads++;
        if ((value & 3) == 2) local_copy(g);
        break;
    case GS_HWREG: image_data(g, (const uint8_t *)&value, 8); break;
    case GS_SIGNAL: case GS_LABEL: {
        unsigned shift = reg == GS_LABEL ? 32 : 0;
        uint64_t mask = (uint64_t)(uint32_t)(value >> 32) << shift;
        uint64_t *id = &g->privileged[GS_SIGLBLID / 16];
        *id = (*id & ~mask) | ((uint64_t)(uint32_t)value << shift & mask);
        if (reg == GS_SIGNAL) g->privileged[GS_CSR / 16] |= 1;
        break;
    }
    case GS_FINISH: g->privileged[GS_CSR / 16] |= 2; break;
    }
}

void gs_priv_write(Gs *g, unsigned addr, uint64_t value)
{
    addr &= 0x1FFF;
    if (addr >= sizeof g->privileged * 2 || (addr & 15)) return;
    if (addr == GS_CSR) {
        if (value & 0x200) {  /* GS reset leaves local memory intact. */
            g->state_version++;
            memset(g->reg, 0, sizeof g->reg); memset(g->path, 0, sizeof g->path);
            g->reg[GS_PRMODECONT] = 1; g->reg[GS_TRXDIR] = 3;
            g->vertex_count = 0;
        }
        g->privileged[addr / 16] &= ~(value & 31);  /* interrupt bits are write-one-to-clear */
    } else g->privileged[addr / 16] = value;
}

uint64_t gs_priv_read(const Gs *g, unsigned addr)
{
    addr &= 0x1FFF;
    return addr < sizeof g->privileged * 2 && !(addr & 15) ? g->privileged[addr / 16] : 0;
}

void gs_vblank(Gs *g)
{
    readback_field++;
    g->privileged[GS_CSR / 16] |= 8;
    g->privileged[GS_CSR / 16] ^= 0x2000;
}

/* gs_write for the registers a PACKED vertex writes every time (RGBAQ, ST, UV, XYZF2/3, XYZ2/3):
   the same stores, inline and without gs_write's switches. None of them is a state register, so
   state_version does not move. On the throttled phone of finding 127 these writes were most of
   the GS thread's 27 % in gs_write. */
static inline void vertex_write(Gs *g, unsigned reg, uint64_t value)
{
    g->reg[reg] = value;
    switch (reg) {
    case GS_RGBAQ:
        g->vertex.rgba = (uint32_t)value;
        { uint32_t q = (uint32_t)(value >> 32); memcpy(&g->vertex.q, &q, 4); }
        break;
    case GS_ST: memcpy(&g->vertex.s, &value, 8); break;
    case GS_UV: g->vertex.u = field(value, 0, 0x3FFF); g->vertex.v = field(value, 16, 0x3FFF); break;
    default:  /* XYZF2, XYZ2, XYZF3, XYZ3 */
        g->vertex.x = field(value, 0, 65535); g->vertex.y = field(value, 16, 65535);
        g->vertex.z = (uint32_t)(value >> 32);
        if (reg == GS_XYZF2 || reg == GS_XYZF3) {
            g->vertex.z &= 0xFFFFFF; g->vertex.fog = (unsigned)(value >> 56);
        }
        vertex_kick(g, reg == GS_XYZF3 || reg == GS_XYZ3);
        break;
    }
}

static void packed(Gs *g, GsPath *p, unsigned reg, const uint32_t *w)
{
    uint64_t value = w[0] | (uint64_t)w[1] << 32;
    switch (reg) {
    case 1: value = (w[0] & 255) | (w[1] & 255) << 8 | (w[2] & 255) << 16 |
                   (w[3] & 255) << 24 | (uint64_t)p->q << 32; break;
    case 2: p->q = w[2]; break;
    case 3: value = (w[0] & 0x3FFF) | (uint64_t)(w[1] & 0x3FFF) << 16; break;
    case 4: case 12:
        value = (w[0] & 65535) | (uint64_t)(w[1] & 65535) << 16 |
                (uint64_t)(w[2] >> 4 & 0xFFFFFF) << 32 | (uint64_t)(w[3] >> 4 & 255) << 56;
        if (w[3] & 0x8000) reg = 12;
        break;
    case 5: case 13:
        value = (w[0] & 65535) | (uint64_t)(w[1] & 65535) << 16 | (uint64_t)w[2] << 32;
        if (w[3] & 0x8000) reg = 13;
        break;
    case 10: value = (uint64_t)(w[3] >> 4 & 255) << 56; break;
    case 14: reg = w[2] & 127; break;
    case 11: case 15: return;
    }
    if ((reg >= GS_RGBAQ && reg <= GS_XYZ2) || reg == GS_XYZF3 || reg == GS_XYZ3) vertex_write(g, reg, value);
    else gs_write(g, reg, value);
}

static void gif_qword(Gs *g, unsigned path, GsPath *p)
{
    uint32_t *w = p->words;
    if (!p->left) {
        uint64_t tag = w[0] | (uint64_t)w[1] << 32;
        p->left = w[0] & 0x7FFF; p->eop = w[0] >> 15 & 1;
        p->mode = field(tag, 58, 3); p->nreg = field(tag, 60, 15);
        if (!p->nreg) p->nreg = 16;
        p->regs = w[2] | (uint64_t)w[3] << 32; p->reg = 0; p->q = 0x3F800000;
        if (p->left && p->mode == 0 && (tag >> 46 & 1)) gs_write(g, GS_PRIM, field(tag, 47, 2047));
        g->packets[path - 1]++;
    } else if (p->mode >= 2) {
        image_data(g, (const uint8_t *)w, 16); p->left--;
    } else {
        unsigned n = p->mode == 0 ? 1 : 2;
        for (unsigned i = 0; i < n && p->left; i++) {
            unsigned reg = (unsigned)(p->regs >> (p->reg * 4)) & 15;
            if (p->mode == 0) packed(g, p, reg, w);
            else if (reg != 14 && reg != 15 && reg != 11)
                gs_write(g, reg, w[i * 2] | (uint64_t)w[i * 2 + 1] << 32);
            if (++p->reg == p->nreg) { p->reg = 0; p->left--; }
        }
    }
}

void gs_feed(Gs *g, unsigned path, const uint32_t *words, size_t count)
{
    if (path < 1 || path > 3) return;
    if (g->record_packet) g->record_packet(g, path, words, count);
    g->source_path = path;
    GsPath *p = &g->path[path - 1];
    /* Keep the word-at-a-time semantics if input aliases the path state. */
    uintptr_t input = (uintptr_t)words, state = (uintptr_t)p;
    int bulk = input < state ? count <= (state - input) / sizeof *words
                             : input - state >= sizeof *p;
    for (size_t i = 0; i < count;) {
        if (bulk && !p->have && count - i >= 4 && p->left && p->mode >= 2 && !g->record_transfer && !image_reference) {
            /* IMAGE data: the whole run of qwords at once, the same pixels as one image_data per
               qword. The path keeps the last qword, as it would. */
            size_t run = (count - i) / 4 < p->left ? (count - i) / 4 : p->left;
            image_data(g, (const uint8_t *)(words + i), (unsigned)run * 16);
            memcpy(p->words, words + i + (run - 1) * 4, sizeof p->words);
            p->left -= (uint32_t)run;
            i += run * 4;
        } else if (bulk && !p->have && count - i >= 4) {
            memcpy(p->words, words + i, sizeof p->words);
            p->have = 4;  /* the transfer callback observes a completed qword */
            i += 4;
        } else {
            p->words[p->have++] = words[i++];
        }
        if (p->have == 4) {
            if (g->record_transfer) g->record_transfer(path, p->words);
            p->have = 0; gif_qword(g, path, p);
        }
    }
}

int gs_xgkick(Gs *g, const uint32_t *memory, unsigned qword)
{
    GsPath *p = &g->path[0];
    for (unsigned n = 0; n < 0x4000; n++) {
        gs_feed(g, 1, memory + ((qword + n) & 1023) * 4, 4);
        if (!p->left && p->eop) return 0;
    }
    return -1;  /* malformed packet: don't let a missing EOP hang the host */
}

/* The GS instance and all its native callbacks belong to the host. */
#include "../host/host_api.h"
#include "../include/guest.h"
unsigned host_service_gs_bits_per_pixel(unsigned a0) { return gs_bits_per_pixel(a0); }
int host_service_gs_reads_pending(void) { return gs_reads_pending(&gs); }
void host_service_gs_settle_reads(void) { gs_settle_reads(&gs); }
void host_service_gs_reset(void) { gs_reset(&gs); }
void host_service_gs_vblank(void) { gs_vblank(&gs); }
uint64_t host_service_gs_priv_read(unsigned a0) { return gs_priv_read(&gs, a0); }
void host_service_gs_priv_write(unsigned a0, uint64_t a1) { gs_priv_write(&gs, a0, a1); }
uint32_t host_service_gs_read_image(uint32_t a0, uint32_t a1) { return gs_read_image(&gs, GUEST(a0), a1); }
uint32_t host_service_gs_defer_read_image(uint32_t a0, uint32_t a1) { return gs_defer_read_image(&gs, GUEST(a0), a1); }
uint64_t host_service_gs_register(unsigned reg) { return reg < 128 ? gs.reg[reg] : 0; }

/* Trace packets are copied while their producer owns the bytes. The module
   consumes them after graphics_wait, never from a VU/GS OS thread. */
typedef struct TracePacket { struct TracePacket *next; uint32_t path, count; uint32_t words[]; } TracePacket;
static TracePacket *trace_first, *trace_last;
static void trace_record(Gs *g, unsigned path, const uint32_t *words, size_t count) {
    (void)g;
    if (count>UINT32_MAX/4) abort();
    TracePacket *p=malloc(sizeof *p+count*4);
    if (!p) abort();
    p->next=NULL; p->path=path; p->count=(uint32_t)count;
    memcpy(p->words,words,count*4);
    if (trace_last) trace_last->next=p; else trace_first=p;
    trace_last=p;
}
void host_service_gs_trace_begin(void) { gs.record_packet=trace_record; }
uint32_t host_service_gs_trace_size(void) { return trace_first ? trace_first->count*4+8 : 0; }
void host_service_gs_trace_pop(uint32_t address) {
    TracePacket *p=trace_first;
    if (!p) abort();
    uint32_t *out=GUEST(address); out[0]=p->path; out[1]=p->count;
    memcpy(out+2,p->words,p->count*4);
    trace_first=p->next; if (!trace_first) trace_last=NULL;
    free(p);
}
void host_service_gs_snapshot(uint32_t address, uint32_t bytes) {
    /* The prefix before native callbacks contains only fixed-width PS2 state. */
    if (bytes!=offsetof(Gs,sync)) abort();
    memcpy(GUEST(address),&gs,bytes);
}
