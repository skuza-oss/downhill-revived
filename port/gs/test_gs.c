#include "gs.h"
#include "present.h"
#include <stdio.h>
#include <string.h>

static unsigned failures, draws;
#define CHECK(c) do { if (!(c)) { printf("FAIL GS line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void swizzles(void)
{
    /* Golden hashes of bit addresses over four pages, bp=31, stride=3 pages.
       Computed independently from PCSX2 GSTables.cpp (2026-10-13). */
    static const struct { unsigned psm, width, height, hash; } cases[] = {
        {0,64,32,0xA63F9DC5u}, {1,64,32,0xA63F9DC5u}, {2,64,64,0x024F9DC5u},
        {10,64,64,0x27CD9DC5u}, {19,128,64,0x621B9DC5u}, {20,128,128,0xF3F89DC5u},
        {27,64,32,0xDE001DC5u}, {36,64,32,0xDE001DC5u}, {44,64,32,0xC0D09DC5u},
        {48,64,32,0x62799DC5u}, {49,64,32,0x62799DC5u}, {50,64,64,0x35CD9DC5u},
        {58,64,64,0x96199DC5u}
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        unsigned hash = 2166136261u, psm = cases[i].psm, bw = cases[i].width * 3 / 64;
        unsigned bits = gs_bits_per_pixel(psm), mask = bits == 32 ? ~0u : (1u << bits) - 1;
        for (unsigned y = 0; y < cases[i].height * 2; y++) for (unsigned x = 0; x < cases[i].width * 2; x++) {
            unsigned a = gs_pixel_address(psm, 31, bw, x, y);
            hash = (hash ^ a) * 16777619u;
            gs_write_pixel(&gs, psm, 31, bw, x, y, x * 17 + y * 12345);
        }
        CHECK(hash == cases[i].hash);
        for (unsigned y = 0; y < cases[i].height * 2; y++) for (unsigned x = 0; x < cases[i].width * 2; x++)
            CHECK(gs_read_pixel(&gs, psm, 31, bw, x, y) == ((x * 17 + y * 12345) & mask));
        CHECK(gs_pixel_address(psm, 0x4000, bw, 0, 0) == gs_pixel_address(psm, 0, bw, 0, 0));
        /* Coverage must contain every accessed byte, including arbitrary base
           alignment, zero/odd stride and wrap at the end of GS memory. */
        unsigned random = 0x19354u;
        for (unsigned trial = 0; trial < 128; trial++) {
            uint32_t pages[16] = {0};
            random = random * 1664525u + 1013904223u;
            unsigned bp = random & 16383, stride = random >> 16 & 63;
            unsigned x0 = random >> 5 & 1023, y0 = random >> 15 & 1023;
            unsigned x1 = x0 + 255, y1 = y0 + 255;
            gs_mark_pages(pages, psm, bp, stride, x0,y0,x1,y1);
            for (unsigned y = y0; y <= y1; y += 17) for (unsigned x = x0; x <= x1; x += 17) {
                unsigned page = gs_pixel_address(psm, bp, stride, x,y) / (8192 * 8);
                CHECK(pages[page / 32] & (1u << (page & 31)));
            }
        }
    }
    gs_write_pixel(&gs, GS_PSMCT32, 0, 1, 0, 0, 0xAABBCCDD);
    gs_write_pixel(&gs, GS_PSMCT24, 0, 1, 0, 0, 0x123456);
    gs_write_pixel(&gs, GS_PSMT4HL, 0, 1, 0, 0, 5);
    CHECK(gs_read_pixel(&gs, GS_PSMCT32, 0, 1, 0, 0) == 0xA5123456);
}

static void begin_image(unsigned psm, unsigned x, unsigned y, unsigned w, unsigned h)
{
    gs_write(&gs, GS_BITBLTBUF, (uint64_t)psm << 56 | UINT64_C(1) << 48);
    gs_write(&gs, GS_TRXPOS, (uint64_t)x << 32 | (uint64_t)y << 48);
    gs_write(&gs, GS_TRXREG, w | (uint64_t)h << 32);
    gs_write(&gs, GS_TRXDIR, 0);
}

static void draw(Gs *g, unsigned prim, const GsVertex *v, unsigned count)
{
    (void)g;
    draws++;
    CHECK(prim == 6 && count == 2);
    CHECK(v[0].x == 16 && v[1].x == 64);
    CHECK(v[1].rgba == 0x80402010 && v[1].q == 2.0f);
}

static void gif(void)
{
    uint32_t packet[] = {
        1, (1u << 28) | (1u << 14) | (6u << 15), 0xE, 0,
        0x12345678, 0, GS_FOGCOL, 0,
        2 | 0x8000, 3u << 28, 0x512, 0,  /* STQ, RGBA, XYZ2, two sprite vertices */
        0,0,0x40000000,0, 0x10,0x20,0x40,0x80, 16,32,9,0,
        0,0,0x40000000,0, 0x10,0x20,0x40,0x80, 64,80,9,0,
        1 | 0x8000, (1u << 26) | (3u << 28), 0xA31,0, /* odd REGLIST */
        0x11223344,0x3F800000, 0x00120034,0, 0,0xAA000000, 0xFFFFFFFF,0xFFFFFFFF,
        1 | 0x8000, 1u << 28, 0xE,0, 0x88776655,0,GS_FOGCOL,0
    };
    for (unsigned chunk = 1; chunk <= 11; chunk++) {
        gs_reset(&gs); gs.draw = draw; draws = 0;
        for (unsigned i = 0; i < sizeof packet / 4; i += chunk) {
            unsigned n = sizeof packet / 4 - i;
            gs_feed(&gs, 3, packet + i, n < chunk ? n : chunk);
        }
        CHECK(draws == 1 && gs.packets[2] == 4 && gs.path[2].left == 0 && gs.path[2].have == 0);
        CHECK(gs.vertex.u == 0x34 && gs.vertex.v == 0x12 && gs.vertex.fog == 0xAA);
        CHECK(gs.reg[GS_FOGCOL] == 0x88776655);
    }
    gs_reset(&gs);
    begin_image(GS_PSMCT24, 63, 31, 3, 2);
    uint32_t image[] = {2, 2u << 26, 0,0, 0x04030201,0x08070605,0x0C0B0A09,0x100F0E0D,
                        0x00001211,0,0,0};
    for (unsigned i = 0; i < sizeof image / 4; i++) gs_feed(&gs, 3, image + i, 1);
    CHECK(gs_read_pixel(&gs, 1, 0, 1, 63, 31) == 0x030201);
    CHECK(gs_read_pixel(&gs, 1, 0, 1, 65, 32) == 0x121110);
    CHECK(gs.transfer_count == 6);
    begin_image(GS_PSMT4, 31, 15, 3, 1);
    gs_write(&gs, GS_HWREG, 0x987);
    CHECK(gs_read_pixel(&gs, 20, 0, 1, 31, 15) == 7);
    CHECK(gs_read_pixel(&gs, 20, 0, 1, 33, 15) == 9);
    /* PATH1 wraps through the end of VU1 memory and stops at EOP. */
    uint32_t vu[4096] = {0};
    vu[4092] = 0x8001; vu[4093] = 1u << 28; vu[4094] = 14;
    vu[0] = 0x123; vu[2] = GS_FOGCOL;
    CHECK(gs_xgkick(&gs, vu, 1023) == 0 && gs.reg[GS_FOGCOL] == 0x123);
}

static const uint32_t staging_words[] = {0x8001, 1u << 28, GS_RGBAQ, 0, 0x11, 0x22, 0x33, 0x44};
static unsigned staging_qwords;
static void staging_transfer(unsigned path, const uint32_t *words)
{
    CHECK(path == 1 && words == gs.path[0].words && gs.path[0].have == 4 && gs.source_path == 1);
    if (staging_qwords < 2) CHECK(!memcmp(words, staging_words + staging_qwords * 4, 16));
    else CHECK(staging_qwords < 2);
    staging_qwords++;
}

static void gif_staging(void)
{
    /* A transfer hook sees complete staged qwords, including across a split. */
    for (unsigned split = 0; split <= 8; split++) {
        gs_reset(&gs); staging_qwords = 0; gs.record_transfer = staging_transfer;
        gs_feed(&gs, 1, staging_words, split);
        gs_feed(&gs, 1, staging_words + split, 8 - split);
        CHECK(staging_qwords == 2 && gs.path[0].have == 0 && gs.path[0].left == 0);
        CHECK(gs.reg[GS_RGBAQ] == (UINT64_C(0x3F800000) << 32 | 0x44332211));
    }
    /* The source may be the same path's staging storage. */
    gs_reset(&gs); staging_qwords = 0; gs.record_transfer = staging_transfer;
    memcpy(gs.path[0].words, staging_words, 16);
    gs_feed(&gs, 1, gs.path[0].words, 4);
    memcpy(gs.path[0].words, staging_words + 4, 16);
    gs_feed(&gs, 1, gs.path[0].words, 4);
    CHECK(staging_qwords == 2 && gs.path[0].have == 0 && gs.path[0].left == 0);
    CHECK(gs.reg[GS_RGBAQ] == (UINT64_C(0x3F800000) << 32 | 0x44332211));
    gs.record_transfer = NULL;
}

static void palette(void)
{
    gs_reset(&gs);
    uint64_t tex = (uint64_t)GS_PSMT8 << 20 | UINT64_C(32) << 37 | UINT64_C(1) << 61;
    /* A CSM1 palette uploaded as 16x16 CT32 has bits 3 and 4 of its index exchanged. */
    for (unsigned y = 0; y < 16; y++) for (unsigned x = 0; x < 16; x++)
        gs_write_pixel(&gs, 0, 32, 1, x, y, 0x80100000 + y * 16 + x);
    gs_write(&gs, GS_TEX0_1, tex);
    for (unsigned i = 0; i < 256; i++)
        CHECK(gs_palette(&gs, tex, i) == (0x80100000 | (i & 0xE7) | (i & 8) << 1 | (i & 16) >> 1));
    gs_write_pixel(&gs, 0, 32, 1, 0, 0, 0x12345678);
    CHECK(gs_palette(&gs, tex, 0) == 0x80100000);  /* palette is a snapshot */
    gs_write(&gs, GS_TEX0_1, (tex & ~(UINT64_C(7) << 61)) | UINT64_C(2) << 61);
    CHECK(gs_palette(&gs, tex, 0) == 0x12345678);
    gs_write_pixel(&gs, 0, 32, 1, 0, 0, 0xDEADBEEF);
    gs_write(&gs, GS_TEX0_1, (tex & ~(UINT64_C(7) << 61)) | UINT64_C(4) << 61);
    CHECK(gs_palette(&gs, tex, 0) == 0x12345678);  /* CBP0 matches, CLD=4 skips */
    tex = (uint64_t)GS_PSMT4 << 20 | UINT64_C(2) << 51 | UINT64_C(1) << 55 |
          UINT64_C(3) << 56 | UINT64_C(1) << 61;
    gs_write(&gs, GS_TEXCLUT, 1 | 2 << 6 | 70 << 12);
    gs_write(&gs, GS_TEXA, UINT64_C(0x80) << 32 | 0x40);
    for (unsigned i = 0; i < 16; i++) gs_write_pixel(&gs, 2, 0, 1, 32 + i, 70, 0x8000 | i);
    gs_write(&gs, GS_TEX0_2, tex);
    CHECK(gs_palette(&gs, tex, 9) == 0x80000048);
    gs_write(&gs, GS_SIGNAL, UINT64_C(0xFFFFFFFF) << 32 | 7);
    CHECK(gs_priv_read(&gs, GS_CSR) & 1);
    gs_priv_write(&gs, GS_CSR, 1);
    CHECK(!(gs_priv_read(&gs, GS_CSR) & 1) && gs_priv_read(&gs, GS_SIGLBLID) == 7);
    uint64_t csr = gs_priv_read(&gs, GS_CSR);
    gs_vblank(&gs);
    CHECK(((gs_priv_read(&gs, GS_CSR) ^ csr) & 0x2000) && (gs_priv_read(&gs, GS_CSR) & 8));
    gs_priv_write(&gs, GS_CSR, 8); /* VSINT ack must retain the read-only FIELD. */
    CHECK(!(gs_priv_read(&gs, GS_CSR) & 8) && ((gs_priv_read(&gs, GS_CSR) ^ csr) & 0x2000));
    gs_vblank(&gs);
    CHECK(!((gs_priv_read(&gs, GS_CSR) ^ csr) & 0x2000));
}

static void scanout_layouts(void)
{
    uint32_t pixels[80 * 66];
    const unsigned formats[] = {0,1,2,10};
    for (unsigned i = 0; i < GS_VRAM_SIZE / 4; i++) gs.vram[i] = i * 1234567u;
    for (unsigned f = 0; f < 4; f++) for (unsigned mode = 0; mode < 2; mode++)
        for (unsigned stride = 1; stride <= 4; stride++) {
            unsigned psm = formats[f], w = 0, h = 0;
            gs_priv_write(&gs, GS_PMODE, 2);
            gs_priv_write(&gs, GS_SMODE2, mode ? 3 : 0);
            gs_priv_write(&gs, GS_DISPFB2, 511 | (uint64_t)stride << 9 | (uint64_t)psm << 15 |
                          UINT64_C(2040) << 32 | UINT64_C(2040) << 43);
            gs_priv_write(&gs, GS_DISPLAY2, UINT64_C(79) << 32 | UINT64_C(65) << 44);
            CHECK(gs_display(&gs, pixels, &w, &h) && w == 80 && h == 66);
            for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
                uint32_t v = gs_read_pixel(&gs, psm, 511 * 32, stride, (2040+x)&2047, (2040+(y>>mode))&2047);
                CHECK(pixels[y*w+x] == (gs_color(v, psm, UINT64_C(0x80) << 32) | 0xFF000000));
            }
        }
}

static void readback(void)
{
    const unsigned formats[] = {0,1,2,10,19,20,27,36,44,48,49,50,58};
    uint8_t expected[256], actual[256];
    for (unsigned f = 0; f < sizeof formats / sizeof *formats; f++) {
        unsigned psm = formats[f], bits = gs_bits_per_pixel(psm);
        unsigned count = 35, bytes = (count * bits + 7) / 8;
        memset(expected, 0, sizeof expected);
        memset(actual, 0xCC, sizeof actual);
        gs_reset(&gs);
        for (unsigned i = 0; i < count; i++) {
            uint32_t value = 0x1234567u * (i + 1);
            gs_write_pixel(&gs, psm, 16380, 2, (2045 + i % 7) & 2047,
                           (2046 + i / 7) & 2047, value);
            for (unsigned b = 0; b < bits; b++)
                expected[(i * bits + b) / 8] |= ((value >> b) & 1) << ((i * bits + b) & 7);
        }
        gs_write(&gs, GS_BITBLTBUF, 16380 | UINT64_C(2) << 16 | (uint64_t)psm << 24);
        gs_write(&gs, GS_TRXPOS, 2045 | UINT64_C(2046) << 16);
        gs_write(&gs, GS_TRXREG, 7 | UINT64_C(5) << 32);
        gs_write(&gs, GS_TRXDIR, 1);
        /* A single byte splits a 24/32-bit pixel, then a FIFO-sized chunk. */
        CHECK(gs_read_image(&gs, actual, 1) == 1);
        CHECK(gs_read_image(&gs, actual + 1, 16) == 16);
        CHECK(gs_read_image(&gs, actual + 17, sizeof actual - 17) == bytes - 17);
        CHECK(!memcmp(actual, expected, bytes));
        CHECK(actual[bytes] == 0xCC && gs_read_image(&gs, actual, 1) == 0);
        gs_write(&gs, GS_TRXDIR, 0);
        CHECK(gs_read_image(&gs, actual, 1) == 0);
    }
}

/* Host-to-local transfers through the whole-pixel path (gs_write_pixels) against the bit
   accumulator alone (image_reference): every format, odd sizes, partial and overlong transfers,
   GIF IMAGE qwords split at random and HWREG doublewords. Memory, page counters and the
   transfer state must be identical. */
extern int image_reference;
static Gs upload_fast, upload_slow;
/* The pages each Gs asked its backend to sync, in order (write_pixel's sync_range). */
static unsigned sync_log[2][1 << 16], sync_count[2];
static void sync_fast(Gs *g, unsigned address, unsigned bytes)
{
    (void)g; (void)bytes;
    if (sync_count[0] < 1 << 16) sync_log[0][sync_count[0]] = address;
    sync_count[0]++;
}
static void sync_slow(Gs *g, unsigned address, unsigned bytes)
{
    (void)g; (void)bytes;
    if (sync_count[1] < 1 << 16) sync_log[1][sync_count[1]] = address;
    sync_count[1]++;
}
static void upload_fuzz(void)
{
    static const unsigned formats[] = {0, 1, 2, 10, 19, 20, 27, 36, 44, 48, 49, 50, 58};
    unsigned random = 0x2468ACEu, differing = 0;
    static uint32_t words[4 * 1200];
    gs_reset(&upload_fast); gs_reset(&upload_slow);
    upload_fast.sync_range = sync_fast; upload_slow.sync_range = sync_slow;
    for (unsigned trial = 0; trial < 6000; trial++) {
#define RND (random = random * 1664525u + 1013904223u, random >> 8)
        unsigned psm = formats[RND % 13], bp = RND & 16383, bw = RND % 12, x = RND & 2047, y = RND & 2047;
        /* Half of them page-aligned PSMCT32, the game's uploads (memory.c's write_ct32). */
        if (trial & 1) psm = 0, bp &= ~31u;
        /* Some pages stay dirty, so a transfer meets both kinds. */
        if (RND % 3 == 0) memset(upload_fast.host_dirty, 0, 64), memset(upload_slow.host_dirty, 0, 64);
        sync_count[0] = sync_count[1] = 0;
        unsigned w = 1 + RND % 300, h = 1 + RND % 40, qwords = 1 + RND % 1100, hwreg = RND % 4 == 0;
        for (unsigned i = 0; i < qwords * 4; i++) { unsigned low = RND; words[i] = low ^ RND << 16; }
        for (int pass = 0; pass < 2; pass++) {
            Gs *g = pass ? &upload_slow : &upload_fast;
            image_reference = pass;
            gs_write(g, GS_BITBLTBUF, (uint64_t)psm << 56 | (uint64_t)bw << 48 | (uint64_t)bp << 32);
            gs_write(g, GS_TRXPOS, (uint64_t)x << 32 | (uint64_t)y << 48);
            gs_write(g, GS_TRXREG, w | (uint64_t)h << 32);
            gs_write(g, GS_TRXDIR, 0);
            if (hwreg)
                for (unsigned i = 0; i < qwords * 2; i++)
                    gs_write(g, GS_HWREG, words[i * 2] | (uint64_t)words[i * 2 + 1] << 32);
            else {
                uint32_t tag[4] = {qwords | 0x8000, 2u << 26, 0, 0};
                gs_feed(g, 2, tag, 4);
                unsigned split = random;
                for (unsigned done = 0; done < qwords * 4;) {
                    unsigned take = 1 + (split = split * 1664525u + 1013904223u) % 97;
                    if (take > qwords * 4 - done) take = qwords * 4 - done;
                    gs_feed(g, 2, words + done, take);
                    done += take;
                }
            }
        }
        image_reference = 0;
        if (upload_fast.transfer_count != upload_slow.transfer_count ||
            upload_fast.transfer_have != upload_slow.transfer_have ||
            upload_fast.transfer_bits != upload_slow.transfer_bits ||
            upload_fast.pixels_written != upload_slow.pixels_written ||
            memcmp(upload_fast.page_writes, upload_slow.page_writes, sizeof upload_fast.page_writes) ||
            memcmp(upload_fast.host_dirty, upload_slow.host_dirty, sizeof upload_fast.host_dirty) ||
            memcmp(upload_fast.path, upload_slow.path, sizeof upload_fast.path) ||
            sync_count[0] != sync_count[1] ||
            memcmp(sync_log[0], sync_log[1], (sync_count[0] < 1 << 16 ? sync_count[0] : 1 << 16) * sizeof **sync_log))
            differing++;
#undef RND
    }
    CHECK(differing == 0);
    CHECK(!memcmp(upload_fast.vram, upload_slow.vram, sizeof upload_fast.vram));
    CHECK(upload_fast.pixels_written > 1000000);
    printf("upload fuzz: 6000 transfers, %llu pixels, %u differing\n", (unsigned long long)upload_fast.pixels_written,
           differing);
    upload_fast.sync_range = upload_slow.sync_range = NULL;
}

/* The strip fast path (gs_feed's packed_strip, finding 146) against the qword-at-a-time path:
   the same triangles in the same order and the same state after, for random PACKED strip tags
   (2 to 5 registers, ADC skips, batches over 256 vertices, any split of the input). upload_fast
   takes the fast path (draw_strip set), upload_slow the plain one. */
enum { STRIP_LOG = 8192 };
static GsVertex strip_log[2][STRIP_LOG * 3];
static unsigned strip_logged[2], strip_batches;

static void strip_note(Gs *g, const GsVertex *v)
{
    unsigned k = g == &upload_slow;
    if (strip_logged[k] < STRIP_LOG) memcpy(&strip_log[k][strip_logged[k] * 3], v, 3 * sizeof *v);
    strip_logged[k]++;
}

static void strip_draw(Gs *g, unsigned prim, const GsVertex *v, unsigned count)
{
    CHECK(prim == 4 && count == 3);
    strip_note(g, v);
}

static void strip_batch(Gs *g, unsigned prim, const GsVertex *list, unsigned count, const uint8_t *drawn)
{
    CHECK(prim == 4 && count >= 3 && count <= 258);
    strip_batches++;
    for (unsigned i = 0; i + 2 < count; i++)
        if (drawn[i]) strip_note(g, list + i);
}

static void strips(void)
{
    static const struct { uint32_t regs; unsigned nreg; } lists[] = {
        {0x512, 3},     /* ST, RGBAQ, XYZ2 */
        {0xC1, 2},      /* RGBAQ, XYZF2 */
        {0xD213, 4},    /* UV, RGBAQ, ST, XYZF3 */
        {0x51321, 5},   /* RGBAQ, ST, UV, RGBAQ, XYZ2: too long for the fast path */
    };
    static uint32_t words[(1 + 700 * 5) * 4];
    unsigned random = 0x5EED, differing = 0;
#define RND (random = random * 1664525u + 1013904223u, random >> 8)
    for (unsigned trial = 0; trial < 64; trial++) {
        unsigned which = trial % 4, nreg = lists[which].nreg, loops = 1 + RND % 700, n = 4;
        words[0] = loops | 0x8000; words[1] = nreg << 28 | 1u << 14 | 4u << 15;  /* PRE, PRIM = triangle strip */
        words[2] = lists[which].regs; words[3] = 0;
        for (unsigned l = 0; l < loops; l++)
            for (unsigned r = 0; r < nreg; r++, n += 4) {
                for (unsigned i = 0; i < 4; i++) { unsigned low = RND; words[n + i] = low ^ RND << 16; }
                if (r == nreg - 1) words[n + 3] = (words[n + 3] & ~0x8000u) | (RND % 8 == 0 ? 0x8000u : 0);
            }
        strip_logged[0] = strip_logged[1] = 0;
        for (int pass = 0; pass < 2; pass++) {
            Gs *g = pass ? &upload_slow : &upload_fast;
            gs_reset(g);
            g->draw = strip_draw; g->draw_strip = pass ? NULL : strip_batch;
            unsigned split = random;
            for (unsigned done = 0; done < n;) {
                unsigned take = trial & 1 ? n : 1 + (split = split * 1664525u + 1013904223u) % 4000;
                if (take > n - done) take = n - done;
                gs_feed(g, 2, words + done, take);
                done += take;
            }
        }
        Gs *a = &upload_fast, *b = &upload_slow;
        unsigned logged = strip_logged[0] < STRIP_LOG ? strip_logged[0] : STRIP_LOG;
        if (strip_logged[0] != strip_logged[1] || memcmp(strip_log[0], strip_log[1], logged * 3 * sizeof (GsVertex)) ||
            memcmp(&a->vertex, &b->vertex, sizeof a->vertex) || a->vertex_count != b->vertex_count ||
            memcmp(a->vertices, b->vertices, a->vertex_count * sizeof *a->vertices) ||
            a->strip_parity != b->strip_parity || a->kicks != b->kicks || a->adc_skips != b->adc_skips ||
            a->vram_epoch != b->vram_epoch || memcmp(a->reg, b->reg, sizeof a->reg) ||
            memcmp(a->path, b->path, sizeof a->path))
            differing++;
    }
#undef RND
    CHECK(differing == 0 && strip_batches > 64);
    upload_fast.draw = upload_slow.draw = NULL;
    upload_fast.draw_strip = NULL;
    printf("strip fast path: 64 tags, %u batches, %u differing\n", strip_batches, differing);
}

int main(void)
{
    swizzles(); gif(); gif_staging(); palette(); upload_fuzz(); strips();
    uint32_t pixels[16]; unsigned w = 0, h = 0;
    gs_priv_write(&gs, GS_PMODE, 2);
    gs_priv_write(&gs, GS_DISPFB2, 1 | 1 << 9 | UINT64_C(63) << 32 | UINT64_C(31) << 43);
    gs_priv_write(&gs, GS_DISPLAY2, UINT64_C(7) << 32 | UINT64_C(1) << 23 | UINT64_C(1) << 44);
    gs_write_pixel(&gs, 0, 32, 1, 64, 32, 0x80123456);
    CHECK(gs_display(&gs, pixels, &w, &h) && w == 4 && h == 2 && pixels[5] == 0xFF123456);
    gs_priv_write(&gs, GS_SMODE2, 3);
    gs_write_pixel(&gs, 0, 32, 1, 64, 31, 0x80123456);
    CHECK(gs_display(&gs, pixels, &w, &h) && pixels[1] == 0xFF123456 && pixels[5] == pixels[1]);
    /* A different game frame replaces both parities, irrespective of FIELD. */
    for (unsigned field = 0; field < 2; field++) {
        gs_vblank(&gs);
        gs_write_pixel(&gs, 0, 32, 1, 64, 31, 0x80654320 | field);
        CHECK(gs_display(&gs, pixels, &w, &h) && pixels[1] == (0xFF654320 | field) && pixels[5] == pixels[1]);
    }
    scanout_layouts(); readback();
    printf("GS memory, GIF, transfer and CLUT tests: %u failures\n", failures);
    return failures != 0;
}
