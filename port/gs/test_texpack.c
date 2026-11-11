/* texpack.c through its interface (port/tools/test_texpack.py builds and runs this; finding 057).
   GS textures are made in a Gs's memory here, their PCSX2 names come from texpack_find (the
   DHD_TEXTURE_PACK_LOG it writes) and the pack's PNGs are the script's synthetic ones. The
   renderer is a fake ReplaceDevice that keeps every picture it is given and checks each view's
   lifetime. Modes:
     root               texpack_available() for the environment the script sets
     names              looks every case up once (field = case number), prints the model names
     load OUT           loads every case found; OUT/<case>.bin is what the device was given
     budget             eviction under DHD_TEXTURE_CACHE_MB=1 (cases B0..B4)
     stress FIELDS      loaders and evictions at once, as in a race (no waiting; cases S*)
   Output: "PASS x" / "FAIL x" lines and "RESULT PASS" or "RESULT FAIL n". */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <SDL3/SDL.h>

#include "gs.h"
#include "texpack.h"
#include "memory.h"
#include "../host/host_api.h"
#include "../include/guest.h"

#define XXH_INLINE_ALL
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

/* ---- what the game's other units give texpack.c and host_sdl.c ---- */
uintptr_t guest_base;
static uint8_t guest_ram[0x440000];   /* the two words texpack.c reads: the screen and race phase */
void host_thread_name(const char *name) { (void)name; }
void dhd_host_log(const char *line, size_t bytes) { fprintf(stderr, "%.*s\n", (int)bytes, line); }
int sdl_host_run(void (*fn)(void *), void *arg) { (void)fn; (void)arg; return 0; }
FILE *host_os_fopen(const char *path, const char *mode)   /* UTF-8, as host_win32.c's */
{
#ifdef _WIN32
    wchar_t wpath[4096], wmode[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 4096) || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8))
        return NULL;
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}
unsigned host_exe_path(char *out, unsigned size)  /* TEST_EXE_PATH stands for the game's path */
{
    const char *p = getenv("TEST_EXE_PATH");
    snprintf(out, size, "%s", p ? p : "");
    return (unsigned)strlen(out);
}

static int failures;
#define CHECK(cond, ...) do { if (cond) { printf("PASS "); } else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static void live_race(int on)
{
    *(int32_t *)(guest_ram + 0x29DED8) = on ? 9 : 0;
    *(int32_t *)(guest_ram + 0x43F348) = on ? 2 : 0;
}

/* ---- the fake renderer ---- */
typedef struct View {
    unsigned w, h, levels;
    size_t bytes;
    uint8_t *pixels;
    int refs, dead;
    int64_t returned;                  /* the last field texpack_view returned it */
} View;
static SDL_Mutex *device_lock;
static int created, released, live;
static int64_t field_now;

static size_t mip_bytes(unsigned w, unsigned h, unsigned levels)
{
    size_t total = 0;
    for (unsigned i = 0; i < levels; i++) {
        total += (size_t)w * h * 4;
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    return total;
}

static void *fake_create(void *context, const uint8_t *pixels, unsigned w, unsigned h, unsigned levels)
{
    (void)context;
    View *v = calloc(1, sizeof *v);
    v->w = w, v->h = h, v->levels = levels, v->bytes = mip_bytes(w, h, levels);
    v->pixels = malloc(v->bytes);
    memcpy(v->pixels, pixels, v->bytes);
    v->refs = 1, v->returned = -1;
    SDL_LockMutex(device_lock);
    created++, live++;
    SDL_UnlockMutex(device_lock);
    return v;
}

static void fake_retain(void *view)
{
    SDL_LockMutex(device_lock);
    ((View *)view)->refs++;
    SDL_UnlockMutex(device_lock);
}

static int early_releases, double_releases;
static void fake_release(void *view)
{
    View *v = view;
    SDL_LockMutex(device_lock);
    if (v->dead) double_releases++;
    /* texpack.h: a view stays valid for at least 8 fields after the last call that returned it. */
    if (v->returned >= 0 && field_now <= v->returned + 8) early_releases++;
    if (--v->refs == 0) { v->dead = 1; released++; live--; memset(v->pixels, 0xEE, v->bytes); }
    SDL_UnlockMutex(device_lock);
}

static const ReplaceDevice device = {fake_create, fake_retain, fake_release, NULL};

static int dead_returns;
static View *view_at(TexPackEntry *e, int64_t field)
{
    View *v = texpack_view(e, (uint64_t)field);
    if (v) {
        SDL_LockMutex(device_lock);
        if (v->dead) dead_returns++;
        v->returned = field;
        SDL_UnlockMutex(device_lock);
    }
    return v;
}

static void next_field(int64_t field)
{
    field_now = field;
    texpack_field((uint64_t)field);
}

/* ---- GS textures ---- */
static Gs g;

typedef struct Case {
    const char *id;
    unsigned psm, tw, th, cpsm, csa, mxl, mmin;
    uint64_t texa;
    unsigned seed;
} Case;

/* T8/T4 cases are paletted (CLUT at csa); M* have a model name below; the rest are CT32 64x64. */
static const Case cases[] = {
    {"M1", GS_PSMCT32, 2, 2, 0, 0, 0, 0, 0, 11},                       /* below a block: texels hashed */
    {"M2", GS_PSMT8, 3, 3, GS_PSMCT32, 0, 0, 0, 0, 12},                /* below a block: index bytes */
    {"M3", GS_PSMT4, 3, 3, GS_PSMCT16, 3, 0, 0, 0x40ull << 32 | 1u << 15 | 0x30, 13},  /* CT16 CLUT + TEXA */
    {"M4", GS_PSMCT16, 2, 2, 0, 0, 0, 0, 0x40ull << 32 | 1u << 15 | 0x80, 14},          /* TEXA in the name */
    {"M5", GS_PSMCT32, 2, 2, 0, 0, 1, 4, 0, 15},                       /* mipmapped: levels 0..1 hashed */
    {"M6", GS_PSMCT32, 2, 2, 0, 0, 1, 5, 0, 16},                       /* mipmapped; the pack has the base name */
    {"G1", GS_PSMCT32, 6, 6, 0, 0, 0, 0, 0, 21},                       /* the block hashes */
    {"G2", GS_PSMT8, 5, 5, GS_PSMCT32, 0, 0, 0, 0, 22},
    {"G3", GS_PSMT4, 6, 6, GS_PSMCT32, 5, 0, 0, 0, 23},
    {"G4", GS_PSMCT24, 4, 4, 0, 0, 0, 0, 1u << 15 | 0x7F, 24},
    {"G5", GS_PSMCT32, 6, 6, 0, 0, 2, 5, 0, 25},                       /* mipmapped, the pack has its base name only */
    {"G6", GS_PSMT8H, 4, 4, GS_PSMCT32, 0, 0, 0, 0, 26},
};
#define FIXED_CASES (sizeof cases / sizeof cases[0])
enum { PLAIN_CASES = 24, BUDGET_CASES = 5, STRESS_CASES = 40 };
#define CASES (FIXED_CASES + PLAIN_CASES + BUDGET_CASES + STRESS_CASES)

static Case case_at(unsigned i, char *id)
{
    if (i < FIXED_CASES) return cases[i];
    Case c = {id, GS_PSMCT32, 6, 6, 0, 0, 0, 0, 0, 100 + i};
    i -= FIXED_CASES;
    if (i < PLAIN_CASES) sprintf(id, "P%u", i);
    else if ((i -= PLAIN_CASES) < BUDGET_CASES) sprintf(id, "B%u", i);
    else sprintf(id, "S%u", i - BUDGET_CASES);
    return c;
}

static uint32_t rng(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static unsigned case_tbp(unsigned i) { return i * 128; }  /* 32 KB apart: two pages, mips in the second */

/* Fills case i's texture (and mip level 1) and its CLUT, sets context 1's registers. */
static void case_setup(unsigned i, uint32_t *texels, uint32_t *level1, uint32_t *palette)
{
    char id[8];
    Case c = case_at(i, id);
    unsigned tbp = case_tbp(i), w = 1u << c.tw, h = 1u << c.th;
    uint32_t s = c.seed * 2654435761u;
    unsigned bits = gs_bits_per_pixel(c.psm);
    uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            uint32_t v = rng(&s) & mask;
            if (c.psm == GS_PSMCT24) v &= 0xFFFFFF;
            if (c.psm == GS_PSMCT16 && (x + y) % 5 == 0) v = 0;  /* AEM: a zero colour */
            gs_write_pixel(&g, c.psm, tbp, 1, x, y, v);
            if (texels) texels[y * w + x] = v;
        }
    if (c.mxl)
        for (unsigned y = 0; y < h / 2; y++)
            for (unsigned x = 0; x < w / 2; x++) {
                uint32_t v = rng(&s);
                gs_write_pixel(&g, c.psm, tbp + 64, 1, x, y, v);
                if (level1) level1[y * (w / 2) + x] = v;
            }
    unsigned entries = c.psm == GS_PSMT8 || c.psm == GS_PSMT8H ? 256 : c.cpsm || c.psm == GS_PSMT4 ? 16 : 0;
    for (unsigned k = 0; k < entries; k++) {
        uint32_t v = rng(&s);
        if (c.cpsm == GS_PSMCT16) {
            uint16_t c16 = (uint16_t)(k == 2 ? 0 : v);
            g.clut[(c.csa * 16 + k) & 511] = c16;
            if (palette) palette[k] = c16;
        } else {
            unsigned at = entries == 16 ? c.csa * 16 + k : k;  /* T8: CSA 0 */
            g.clut[at & 255] = (uint16_t)v;
            g.clut[(at & 255) + 256] = (uint16_t)(v >> 16);
            if (palette) palette[k] = v;
        }
    }
    g.reg[GS_TEX0_1] = (uint64_t)tbp | 1ull << 14 | (uint64_t)c.psm << 20 | (uint64_t)c.tw << 26 |
                       (uint64_t)c.th << 30 | (uint64_t)c.cpsm << 51 | (uint64_t)c.csa << 56;
    g.reg[GS_TEX1_1] = (uint64_t)c.mxl << 2 | (uint64_t)c.mmin << 6;
    g.reg[GS_MIPTBP1_1] = (uint64_t)(tbp + 64) | 1ull << 14;
    g.reg[GS_MIPTBP2_1] = 0;
    g.reg[GS_TEXA] = c.texa;
}

/* PCSX2's names, from the textures as written (finding 057), for the M cases. */
static void model_name(unsigned i, char *out)
{
    static uint32_t texels[4096], level1[1024], palette[256];
    char id[8];
    Case c = case_at(i, id);
    case_setup(i, texels, level1, palette);
    unsigned w = 1u << c.tw, h = 1u << c.th, n = w * h;
    uint32_t bits = c.psm | c.tw << 6 | c.th << 10;
    uint64_t tex0, clut = 0;
    if (c.psm == GS_PSMT8 || c.psm == GS_PSMT4) {
        uint8_t index[4096];
        for (unsigned k = 0; k < n; k++) index[k] = (uint8_t)texels[k];
        tex0 = XXH3_64bits(index, n);
        unsigned entries = c.psm == GS_PSMT8 ? 256 : 16;
        uint32_t expanded[256];
        for (unsigned k = 0; k < entries; k++) {
            uint32_t v = palette[k];
            if (c.cpsm == GS_PSMCT16) {  /* GSClut's Expand16 */
                unsigned alpha = v & 0x8000 ? (unsigned)(c.texa >> 32) & 255 : (unsigned)c.texa & 255;
                if (!(v & 0x8000) && (c.texa & 0x8000) && !(v & 0x7FFF)) alpha = 0;
                v = (v & 31) << 3 | (v >> 5 & 31) << 11 | (v >> 10 & 31) << 19 | alpha << 24;
            }
            expanded[k] = v;
        }
        clut = XXH3_64bits(expanded, entries * 4);
        sprintf(out, "%llx-%llx-%08x", (unsigned long long)tex0, (unsigned long long)clut, bits);
        return;
    }
    uint32_t colours[4096 + 1024];
    for (unsigned k = 0; k < n; k++) {
        uint32_t v = texels[k];
        if (c.psm == GS_PSMCT16) {  /* RGB5A1 with TEXA: TA1 or TA0 by the top bit, 0 for black with AEM */
            unsigned alpha = v & 0x8000 ? (unsigned)(c.texa >> 32) & 255 : (unsigned)c.texa & 255;
            if (!(v & 0x8000) && (c.texa & 0x8000) && !(v & 0x7FFF)) alpha = 0;
            v = (v & 31) << 3 | (v >> 5 & 31) << 11 | (v >> 10 & 31) << 19 | alpha << 24;
        }
        colours[k] = v;
    }
    size_t count = n;
    if (c.mxl && strcmp(c.id, "M6")) {  /* M6: the base level's name */
        for (unsigned k = 0; k < n / 4; k++) colours[n + k] = level1[k];
        count += n / 4;
    }
    if (c.psm == GS_PSMCT16)
        bits |= ((uint32_t)c.texa & 255) << 15 | ((uint32_t)(c.texa >> 15) & 1) << 23 | ((uint32_t)(c.texa >> 32) & 255) << 24;
    tex0 = XXH3_64bits(colours, count * 4);
    sprintf(out, "%llx-%08x", (unsigned long long)tex0, bits);
}

/* ---- modes ---- */

static TexPackEntry *found[CASES];

static int open_pack(void)
{
    device_lock = SDL_CreateMutex();
    for (unsigned i = 0; i < CASES; i++) case_setup(i, NULL, NULL, NULL);
    return texpack_open(&device, 0);
}

static void find_all(void)
{
    for (unsigned i = 0; i < CASES; i++) {
        case_setup(i, NULL, NULL, NULL);
        next_field(i);
        found[i] = texpack_find(&g, 0);
    }
}

static int mode_names(void)
{
    for (unsigned i = 0; i < FIXED_CASES; i++)
        if (cases[i].id[0] == 'M') {
            char name[64];
            model_name(i, name);
            printf("model %u %s %s\n", i, cases[i].id, name);
        }
    if (!open_pack()) { printf("FAIL texpack_open\n"); return 1; }
    find_all();
    for (unsigned i = 0; i < CASES; i++) {
        char id[8];
        printf("case %u %s found=%d\n", i, case_at(i, id).id, found[i] != NULL);
    }
    texpack_report();
    return 0;
}

static int mode_load(const char *out)
{
    if (!open_pack()) { printf("FAIL texpack_open\n"); return 1; }
    find_all();
    View *got[CASES] = {0};
    uint64_t t0 = SDL_GetTicks();
    for (int64_t field = 1000; SDL_GetTicks() - t0 < 5000; field++) {
        next_field(field);
        int waiting = 0;
        for (unsigned i = 0; i < CASES; i++)
            if (found[i] && !got[i] && !(got[i] = view_at(found[i], field))) waiting++;
        if (!waiting) break;
        SDL_Delay(1);
    }
    for (unsigned i = 0; i < CASES; i++) {
        char id[8], path[4096];
        const char *name = case_at(i, id).id;
        if (!found[i]) { printf("case %s absent\n", name); continue; }
        /* texpack_image (the button icons over a pack's font, finding 200): the view's first level */
        unsigned iw = 0, ih = 0;
        uint8_t *image = texpack_image(found[i], &iw, &ih);
        if (!got[i]) CHECK(!image, "image: %s gives no picture, as it loads none", name);
        else CHECK(image && iw == got[i]->w && ih == got[i]->h && !memcmp(image, got[i]->pixels, (size_t)iw * ih * 4),
                   "image: %s %ux%u, the loaded view's first level", name, iw, ih);
        free(image);
        if (!got[i]) { printf("case %s failed\n", name); continue; }
        snprintf(path, sizeof path, "%s/%s.bin", out, name);
        FILE *f = fopen(path, "wb");
        fprintf(f, "%u %u %u\n", got[i]->w, got[i]->h, got[i]->levels);
        fwrite(got[i]->pixels, 1, got[i]->bytes, f);
        fclose(f);
        printf("case %s loaded %ux%u levels %u\n", name, got[i]->w, got[i]->h, got[i]->levels);
    }
    texpack_report();
    return 0;
}

static unsigned case_index(const char *want)
{
    for (unsigned i = 0; i < CASES; i++) {
        char id[8];
        if (!strcmp(case_at(i, id).id, want)) return i;
    }
    return 0;
}

/* B0..B4 are 256 x 256 (349,524 bytes with mips): three fit DHD_TEXTURE_CACHE_MB=1, four don't. */
static int mode_budget(void)
{
    if (!open_pack()) { printf("FAIL texpack_open\n"); return 1; }
    find_all();
    TexPackEntry *b[BUDGET_CASES];
    View *v[BUDGET_CASES] = {0};
    for (unsigned i = 0; i < BUDGET_CASES; i++) {
        char id[8];
        sprintf(id, "B%u", i);
        b[i] = found[case_index(id)];
        if (!b[i]) { printf("FAIL B%u not in the pack\n", i); return 1; }
    }
    /* B0..B3 drawn at fields 2000..2003 (each waits for its load: no race), so the oldest is plain. */
    int64_t f = 2000;
    for (unsigned i = 0; i < 4; i++, f++) {
        next_field(f);
        v[i] = view_at(b[i], f);
    }
    CHECK(v[0] && v[1] && v[2] && v[3], "budget: four loaded (the budget is checked before a load, not after)");
    CHECK(created == 4 && released == 0, "budget: 4 made, none released (%d, %d)", created, released);
    for (; f < 2012; f++) {              /* B3 drawn every field; B0..B2 not */
        next_field(f);
        v[3] = view_at(b[3], f);
    }
    next_field(f);
    View *e = view_at(b[4], f);          /* over budget: the least recently drawn goes, down to the budget */
    CHECK(released == 1 && v[0]->dead && !v[1]->dead && !v[2]->dead && !v[3]->dead,
          "budget: B4 released B0, the least recently drawn, and only it (released %d)", released);
    CHECK(e != NULL, "budget: B4 loaded after the release");
    next_field(++f);
    View *again = view_at(b[0], f);      /* B0 loads again when drawn again; B1 goes for it */
    CHECK(again && again != v[0] && created == 6 && v[1]->dead && !v[2]->dead,
          "budget: B0 loaded again, B1 released for it (%d made, %d released)", created, released);
    for (int k = 0; k < 40; k++) {       /* texpack_field releases on its own (every 30 fields) */
        next_field(++f);
        view_at(b[0], f), view_at(b[3], f), view_at(b[4], f);
    }
    CHECK(v[2]->dead && !v[3]->dead && released == 3, "budget: texpack_field released the stale B2 (%d released)",
          released);
    CHECK(early_releases == 0 && double_releases == 0 && dead_returns == 0,
          "budget: no view released within 8 fields of its use, none twice, none returned dead (%d, %d, %d)",
          early_releases, double_releases, dead_returns);
    texpack_report();
    return 0;
}

/* A race: no waiting, two loaders creating while this thread draws and evicts. */
static int mode_stress(int64_t fields)
{
    if (!open_pack()) { printf("FAIL texpack_open\n"); return 1; }
    find_all();
    live_race(1);
    TexPackEntry *s[STRESS_CASES];
    for (unsigned i = 0; i < STRESS_CASES; i++) {
        char id[8];
        sprintf(id, "S%u", i);
        if (!(s[i] = found[case_index(id)])) { printf("FAIL S%u not in the pack\n", i); return 1; }
    }
    uint32_t seed = 7;
    unsigned drawn = 0, ready = 0;
    for (int64_t f = 5000; f < 5000 + fields; f++) {
        next_field(f);
        unsigned window = (unsigned)((f / 50) % STRESS_CASES);  /* a working set that moves on */
        for (unsigned k = 0; k < 10; k++) {
            unsigned at = (window + rng(&seed) % 12) % STRESS_CASES;
            drawn++;
            if (view_at(s[at], f)) ready++;
        }
        if (f % 97 == 0) SDL_Delay(2);  /* let the loaders get ahead now and then */
    }
    SDL_Delay(500);
    CHECK(ready > drawn / 4, "stress: %u of %u draws had their replacement", ready, drawn);
    CHECK(released > 0, "stress: evictions ran (%d made, %d released)", created, released);
    CHECK(created - released == live, "stress: made - released = live (%d - %d = %d)", created, released, live);
    CHECK(early_releases == 0 && double_releases == 0 && dead_returns == 0,
          "stress: no view released within 8 fields of its use, none twice, none returned dead (%d, %d, %d)",
          early_releases, double_releases, dead_returns);
    texpack_report();
    return 0;
}

int main(int argc, char **argv)
{
    guest_base = (uintptr_t)guest_ram - PS2_RAM_BASE;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) return 2;
    int r = 2;
    if (!strcmp(argv[1], "root")) { printf("available=%d\n", texpack_available()); return 0; }
    if (!strcmp(argv[1], "names")) r = mode_names();
    else if (!strcmp(argv[1], "load") && argc > 2) r = mode_load(argv[2]);
    else if (!strcmp(argv[1], "budget")) r = mode_budget();
    else if (!strcmp(argv[1], "stress") && argc > 2) r = mode_stress(atoll(argv[2]));
    if (r) return r;
    printf(failures ? "RESULT FAIL %d\n" : "RESULT PASS\n", failures);
    fflush(stdout);
    _Exit(failures != 0);  /* the loaders never end */
}
