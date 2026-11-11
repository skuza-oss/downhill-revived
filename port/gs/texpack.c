/* Texture replacement packs (upscale mods) in PCSX2's format, for the native renderer
   (DHD_RENDER=native; finding 057). Existing PCSX2 packs for this disc work unchanged: a pack is
   a folder of PNGs named by PCSX2's texture hash, textures\SCUS-97177\replacements in PCSX2.

   Names (PCSX2 pcsx2/GS/Renderers/HW/GSTextureReplacements.cpp and GSTextureCache.cpp,
   GPL-3.0-or-later, PCSX2 Dev Team; the scheme is reimplemented here):
     <TEX0 hash>-<CLUT hash>-<bits>.png   paletted formats (PSMT8, T4, T8H, T4HL, T4HH)
     <TEX0 hash>-<bits>.png               the others
   - TEX0 hash: XXH3-64 of the texture's GS memory. Where the format fills its 32-bit words
     (PSMCT32, PSMT8, PSMT4, PSMZ32) and the texture is at least a block, the raw 256-byte blocks
     it covers, row by row of blocks. Otherwise its texels expanded, row by row: an index byte
     each for the paletted formats, a 32-bit colour (TEXA applied) for the direct ones. A draw
     that uses mipmaps (MXL > 0, a mipmapped MMIN) also hashes levels 1..MXL, from MIPTBP1/2.
   - CLUT hash: XXH3-64 of the palette as 32-bit colours (16 or 256), CSA applied.
   - bits: PSM | TW << 6 | TH << 10, plus TEXA's TA0 << 15 | AEM << 23 | TA1 << 24 for the
     16- and 24-bit direct formats. Bit 14 was TCC in older PCSX2 versions and is ignored.
   Other names (regions "-rWxH-", mip levels "-mipN") are not ones PCSX2 loads; they are skipped.

   PNG alpha is GS alpha, as in PCSX2: 128 is opaque (a PNG without alpha gets 128). The
   replacement is sampled over the same normalised texture space as the original (native.wgsl's
   replacement flag), with mipmaps made here, so a 4x pack draws 4x the detail.

   Loading: the folder's file names are indexed at start. A texture's replacement is read (the
   host's files, host_api.h), decoded (the Rust library's PNG decoder, dhdr_png_*: the same on
   every platform) and uploaded on loader threads the first time it is drawn; until then the
   original is drawn. Uploaded replacements count against a memory budget (DHD_TEXTURE_
   CACHE_MB, dhd.ini texture_cache_mb; default half the graphics card's memory, 512-2048 MB);
   over it, those no field drew lately are released and loaded again when needed. The renderer
   that draws them makes the textures (replace_texture.h).

   Outside a race the first draw waits for its replacement (texpack_view, up to PACK_WAIT_MS of
   waiting a field; finding 086, F1). Some screens draw their picture once into each of the two
   frame buffers and then only flip between them: the loading picture before the title draws it
   for 4 frames and then just a fading overlay. A replacement that became ready between the last
   draw into one buffer and the last into the other left the original in one of them, and the
   screen flickered between the two pictures every field until the next screen (seen at 120 fps,
   where those draws are 8 ms apart; possible at 60 on a slow disk). In a live race (the
   countdown and the race, screen 9 phases 1-4) nothing waits, so a race never stalls on a
   texture: its replacement takes over a few fields later, as before. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "texpack.h"
#include "memory.h"
#include "../host/host_api.h"
#include "../include/guest.h"
#include "../render/include/dhd_render.h"  /* dhdr_png_size, dhdr_png_decode */
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#define XXH_INLINE_ALL
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"   /* xxHash 0.8.3, BSD-2-Clause (THIRD-PARTY-NOTICES.txt) */

void host_thread_name(const char *name);  /* port/hle/host.c */

enum { PACK_IDLE, PACK_QUEUED, PACK_READY, PACK_FAILED };
enum { PACK_LOADERS = 2, PACK_MAX_SIDE = 4096, PACK_WAIT_MS = 200, PACK_PATH = 4096 };
#define PACK_MAX_FILE (256u << 20)     /* a 4096 x 4096 PNG of 16-bit RGBA samples, stored, fits */

struct TexPackEntry {
    uint64_t tex0_hash, clut_hash;
    uint32_t bits;
    char *path;                        /* below the pack's folder: UTF-8, '/' between names */
    atomic_int state;
    void *view;                        /* the renderer's (ReplaceDevice); loader thread, before state becomes READY */
    uint32_t bytes;
    uint64_t used;                     /* GS worker: the last field that drew it */
};

static struct {
    ReplaceDevice device;
    TexPackEntry *entries;
    unsigned count, capacity;
    int32_t *table;                    /* open addressing, entry index or -1 */
    unsigned mask;
    char root[PACK_PATH];              /* UTF-8 */
    unsigned skipped_names;
    /* loading */
    SDL_Mutex *lock;
    SDL_Condition *wake;
    SDL_Condition *done;               /* a loader finished an entry (READY or FAILED) */
    int64_t wait_us;                   /* waiting left this field (set at vsync, GS worker spends it) */
    uint64_t waits, waited_us, wait_misses;
    TexPackEntry **queue;              /* a ring one longer than the index: each entry is queued once at most */
    unsigned head, tail;
    _Atomic int64_t bytes;
    uint64_t budget, peak;
    atomic_long loaded, failed;
    _Atomic int64_t load_us;
    uint64_t evicted, last_evict;
    /* GS worker */
    uint8_t *scratch;
    size_t scratch_size;
    /* counts: distinct names seen, and found */
    uint64_t *seen;                    /* open addressing, 0 empty */
    unsigned seen_mask, seen_count, seen_hits, seen_mips;
    unsigned psm_seen[64], psm_hits[64];
    FILE *log;
} pack;

/* ---- the index ---- */

static uint64_t pack_key(uint64_t tex0_hash, uint64_t clut_hash, uint32_t bits)
{
    uint64_t h = tex0_hash ^ (clut_hash * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)bits * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return h;
}

static TexPackEntry *pack_lookup(uint64_t tex0_hash, uint64_t clut_hash, uint32_t bits)
{
    for (unsigned i = (unsigned)pack_key(tex0_hash, clut_hash, bits) & pack.mask;; i = (i + 1) & pack.mask) {
        int32_t at = pack.table[i];
        if (at < 0) return NULL;
        TexPackEntry *e = &pack.entries[at];
        if (e->tex0_hash == tex0_hash && e->clut_hash == clut_hash && e->bits == bits) return e;
    }
}

static int hex_field(const char *s, size_t length, uint64_t *out)
{
    uint64_t v = 0;
    if (!length || length > 16) return 0;
    for (size_t i = 0; i < length; i++) {
        char c = s[i];
        unsigned d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                     c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (d == 16) return 0;
        v = v << 4 | d;
    }
    *out = v;
    return 1;
}

/* "<hash>-<clut>-<bits>.png" or "<hash>-<bits>.png" (PCSX2's ParseReplacementName, without
   the region forms). */
static int pack_parse(const char *name, uint64_t *tex0_hash, uint64_t *clut_hash, uint32_t *bits)
{
    size_t length = strlen(name);
    if (length < 5 || SDL_strcasecmp(name + length - 4, ".png")) return 0;
    length -= 4;
    const char *part[3];
    size_t size[3], parts = 0, start = 0;
    for (size_t i = 0; i <= length; i++)
        if (i == length || name[i] == '-') {
            if (parts == 3) return 0;
            part[parts] = name + start; size[parts++] = i - start;
            start = i + 1;
        }
    uint64_t v[3];
    for (size_t i = 0; i < parts; i++)
        if (!hex_field(part[i], size[i], &v[i])) return 0;
    if (parts < 2 || size[parts - 1] > 8) return 0;
    *tex0_hash = v[0];
    *clut_hash = parts == 3 ? v[1] : 0;
    *bits = (uint32_t)v[parts - 1] & ~(1u << 14);
    return 1;
}

static void pack_add(const char *relative, const char *name)
{
    uint64_t tex0_hash, clut_hash;
    uint32_t bits;
    if (!pack_parse(name, &tex0_hash, &clut_hash, &bits)) { pack.skipped_names++; return; }
    if (pack.count == pack.capacity) {
        unsigned capacity = pack.capacity ? pack.capacity * 2 : 4096;
        TexPackEntry *grown = realloc(pack.entries, capacity * sizeof *grown);
        if (!grown) return;
        pack.entries = grown; pack.capacity = capacity;
    }
    char *path = SDL_strdup(relative);
    if (!path) return;
    TexPackEntry *e = &pack.entries[pack.count++];
    memset(e, 0, sizeof *e);
    e->tex0_hash = tex0_hash; e->clut_hash = clut_hash; e->bits = bits; e->path = path;
}

/* Every PNG below the folder (PCSX2 searches it recursively too); relative is "" or "a/b/". A
   folder is closed before its subfolders are read, so one folder is open at a time. */
static void pack_scan(const char *relative, int depth)
{
    char folder[PACK_PATH], path[PACK_PATH];
    char **below = NULL;
    unsigned folders = 0;
    HostDirEntry found;
    if (snprintf(folder, sizeof folder, "%s/%s", pack.root, relative) >= (int)sizeof folder) return;
    HostDir d = host_dir_open(folder, &found);
    if (d == HOST_DIR_NONE) return;
    do {
        if (found.name[0] == '.' || snprintf(path, sizeof path, "%s%s", relative, found.name) >= (int)sizeof path - 1)
            continue;
        if (!found.is_dir) pack_add(path, found.name);
        else if (depth < 8) {
            char **grown = realloc(below, (folders + 1) * sizeof *below);
            if (!grown) continue;
            below = grown;
            if ((below[folders] = SDL_strdup(path))) folders++;
        }
    } while (host_dir_next(d, &found));
    host_dir_close(d);
    for (unsigned i = 0; i < folders; i++) {
        snprintf(path, sizeof path, "%s/", below[i]);
        pack_scan(path, depth + 1);
        SDL_free(below[i]);
    }
    free(below);
}

/* The pack's folder: DHD_TEXTURE_PACK (the replacements folder, or a folder holding
   replacements/ or SCUS-97177/replacements/, as PCSX2's textures folder does), else
   textures/SCUS-97177/replacements in DHD_DATA_DIR (the launcher's folder for the player's files),
   else beside the game. Paths are UTF-8; either slash separates names. */
static int pack_find_root(char root[PACK_PATH], int quiet)
{
    static const char *const below[] = {"", "/replacements", "/SCUS-97177/replacements"};
    char base[PACK_PATH], tried[PACK_PATH + 32];
    const char *named = host_getenv("DHD_TEXTURE_PACK"), *data = host_getenv("DHD_DATA_DIR");
    if (named && *named) {
        snprintf(base, sizeof base, "%s", named);
        size_t n = strlen(base);
        while (n > 1 && (base[n - 1] == '\\' || base[n - 1] == '/') && base[n - 2] != ':') base[--n] = 0;
        for (int i = 2; i >= 0; i--) {
            snprintf(tried, sizeof tried, "%s%s", base, below[i]);
            if (strlen(tried) < PACK_PATH && host_path_is_dir(tried)) { strcpy(root, tried); return 1; }
        }
        if (!quiet) fprintf(stderr, "textures: pack folder %s not found (texture_pack in dhd.ini)\n", named);
        return 0;
    }
    if (data && *data) snprintf(base, sizeof base, "%s/", data);
    else {
        unsigned n = host_exe_path(base, sizeof base);
        while (n && base[n - 1] != '\\' && base[n - 1] != '/') n--;
        base[n] = 0;
    }
    snprintf(tried, sizeof tried, "%stextures/SCUS-97177/replacements", base);
    if (strlen(tried) >= PACK_PATH || !host_path_is_dir(tried)) return 0;
    strcpy(root, tried);
    return 1;
}

int texpack_available(void)
{
    char root[PACK_PATH];
    return pack_find_root(root, 1);
}

/* ---- loading (loader threads) ---- */

/* The next level from this one, a 2x2 box (an odd side keeps its last texel). */
static void pack_halve(const uint8_t *from, unsigned w, unsigned h, uint8_t *to)
{
    unsigned nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    for (unsigned y = 0; y < nh; y++) {
        const uint8_t *r0 = from + (size_t)(y * 2 < h ? y * 2 : h - 1) * w * 4;
        const uint8_t *r1 = from + (size_t)(y * 2 + 1 < h ? y * 2 + 1 : h - 1) * w * 4;
        for (unsigned x = 0; x < nw; x++) {
            unsigned x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1;
            for (unsigned c = 0; c < 4; c++)
                to[((size_t)y * nw + x) * 4 + c] =
                    (uint8_t)((r0[x0 * 4 + c] + r0[x1 * 4 + c] + r1[x0 * 4 + c] + r1[x1 * 4 + c] + 2) >> 2);
        }
    }
}

/* The whole file (*size bytes), or NULL. */
static uint8_t *pack_read(const char *path, size_t *size)
{
    HostFile f = host_file_open(path, HOST_FILE_READ);
    if (f == HOST_FILE_NONE) return NULL;
    uint64_t length = host_file_size(f);
    uint8_t *data = length && length <= PACK_MAX_FILE ? malloc((size_t)length) : NULL;
    if (data && host_file_read(f, data, (size_t)length) != (int64_t)length) { free(data); data = NULL; }
    host_file_close(f);
    *size = (size_t)length;
    return data;
}

/* The entry's picture, decoded (w x h RGBA, alpha in GS units: 0x80 opaque), in a buffer with room
   for its mip levels after it when `levels` (their count in *levels then); NULL when the file can't
   be read or decoded, or is too large. */
static uint8_t *pack_decode(const TexPackEntry *e, uint32_t *w, uint32_t *h, unsigned *levels, size_t *total)
{
    char path[PACK_PATH * 2];
    size_t size = 0;
    uint8_t *file, *pixels = NULL;
    snprintf(path, sizeof path, "%s/%s", pack.root, e->path);
    if (!(file = pack_read(path, &size))) return NULL;
    *w = *h = 0;
    if (dhdr_png_size(file, size, w, h) && *w && *h && *w <= PACK_MAX_SIDE && *h <= PACK_MAX_SIDE) {
        /* All the levels in one buffer, down to 1x1. */
        unsigned count = 1;
        size_t bytes = (size_t)*w * *h * 4;
        if (levels)
            for (unsigned lw = *w, lh = *h; lw > 1 || lh > 1; count++) {
                lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
                bytes += (size_t)lw * lh * 4;
            }
        if ((pixels = malloc(bytes)) && !dhdr_png_decode(file, size, pixels, *w, *h, 0x80)) {  /* PCSX2: opaque in GS units */
            free(pixels);
            pixels = NULL;
        }
        if (levels) *levels = count;
        if (total) *total = bytes;
    }
    free(file);
    return pixels;
}

uint8_t *texpack_image(TexPackEntry *e, unsigned *w, unsigned *h)
{
    uint32_t pw, ph;
    uint8_t *pixels = pack_decode(e, &pw, &ph, NULL, NULL);
    *w = pw, *h = ph;
    return pixels;
}

static int pack_load(TexPackEntry *e)
{
    uint8_t *pixels;
    uint32_t w = 0, h = 0;
    unsigned levels = 1;
    size_t total = 0;
    int ok = 0;
    if (!(pixels = pack_decode(e, &w, &h, &levels, &total))) return 0;
    uint8_t *level = pixels;
    unsigned lw = w, lh = h;
    for (unsigned i = 0; i + 1 < levels; i++) {
        uint8_t *next = level + (size_t)lw * lh * 4;
        pack_halve(level, lw, lh, next);
        level = next;
        lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
    }
    if (!(e->view = pack.device.create(pack.device.context, pixels, w, h, levels))) goto done;
    e->bytes = (uint32_t)total;
    ok = 1;
done:
    free(pixels);
    return ok;
}

static int SDLCALL pack_loader(void *argument)
{
    (void)argument;
    host_thread_name("texpack");
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for (;;) {
        SDL_LockMutex(pack.lock);
        while (pack.head == pack.tail) SDL_WaitCondition(pack.wake, pack.lock);
        TexPackEntry *e = pack.queue[pack.head];
        pack.head = (pack.head + 1) % (pack.count + 1);
        SDL_UnlockMutex(pack.lock);
        uint64_t t0 = SDL_GetTicksNS();
        int ok = pack_load(e);
        atomic_fetch_add(&pack.load_us, (int64_t)((SDL_GetTicksNS() - t0) / 1000));
        if (ok) {
            atomic_fetch_add(&pack.bytes, e->bytes);
            atomic_fetch_add(&pack.loaded, 1);
        } else {
            atomic_fetch_add(&pack.failed, 1);
            fprintf(stderr, "textures: can't load %s\n", e->path);
        }
        atomic_exchange(&e->state, ok ? PACK_READY : PACK_FAILED);  /* a full barrier */
        SDL_LockMutex(pack.lock);  /* texpack_view may wait for it */
        SDL_BroadcastCondition(pack.done);
        SDL_UnlockMutex(pack.lock);
    }
}

/* ---- opening ---- */

static uint64_t pack_default_budget(uint64_t video_memory)
{
    uint64_t budget = video_memory ? video_memory / 2 : 1024ull << 20;
    if (budget < (512ull << 20)) budget = 512ull << 20;
    if (budget > (2048ull << 20)) budget = 2048ull << 20;
    return budget;
}

static int pack_switch(const char *v)
{
    return !v || !*v || !(!SDL_strcasecmp(v, "off") || !SDL_strcasecmp(v, "0") || !SDL_strcasecmp(v, "false") ||
                          !SDL_strcasecmp(v, "no"));
}

int texpack_open(const ReplaceDevice *device, uint64_t video_memory)
{
    if (!pack_switch(host_getenv("DHD_TEXTURES"))) return 0;
    if (!pack_find_root(pack.root, 0)) return 0;
    uint64_t t0 = SDL_GetTicksNS();
    pack_scan("", 0);
    double index_ms = (double)(SDL_GetTicksNS() - t0) / 1e6;
    if (!pack.count) {
        fprintf(stderr, "textures: no replacement textures in %s\n", pack.root);
        return 0;
    }
    unsigned size = 1;
    while (size < pack.count * 2) size <<= 1;
    pack.table = malloc(size * sizeof *pack.table);
    pack.queue = malloc((pack.count + 1) * sizeof *pack.queue);
    pack.seen_mask = (1u << 16) - 1;
    pack.seen = calloc(pack.seen_mask + 1, sizeof *pack.seen);
    if (!pack.table || !pack.queue || !pack.seen) return 0;
    pack.mask = size - 1;
    memset(pack.table, 0xFF, size * sizeof *pack.table);
    unsigned duplicates = 0;
    for (unsigned i = 0; i < pack.count; i++) {
        TexPackEntry *e = &pack.entries[i];
        if (pack_lookup(e->tex0_hash, e->clut_hash, e->bits)) { duplicates++; continue; }
        unsigned at = (unsigned)pack_key(e->tex0_hash, e->clut_hash, e->bits) & pack.mask;
        while (pack.table[at] >= 0) at = (at + 1) & pack.mask;
        pack.table[at] = (int32_t)i;
    }
    pack.device = *device;
    const char *mb = host_getenv("DHD_TEXTURE_CACHE_MB");
    pack.budget = mb && atoi(mb) > 0 ? (uint64_t)atoi(mb) << 20 : pack_default_budget(video_memory);
    pack.lock = SDL_CreateMutex();
    pack.wake = SDL_CreateCondition();
    pack.done = SDL_CreateCondition();
    for (unsigned i = 0; i < PACK_LOADERS; i++) SDL_DetachThread(SDL_CreateThread(pack_loader, "texpack", NULL));
    const char *log = host_getenv("DHD_TEXTURE_PACK_LOG");
    if (log && *log && (pack.log = fopen(log, "w")))
        fputs("field,psm,width,height,name,found,with_mips\n", pack.log);
    fprintf(stderr, "textures: %u replacements in %s (%u other files skipped, %u duplicates), indexed in %.0f ms, "
            "cache %llu MB\n", pack.count - duplicates, pack.root, pack.skipped_names, duplicates,
            index_ms, (unsigned long long)(pack.budget >> 20));
    return 1;
}

/* ---- names (GS worker) ---- */

static unsigned pack_palette_size(unsigned psm)
{
    return psm == GS_PSMT8 || psm == GS_PSMT8H ? 256 : psm == GS_PSMT4 || psm == GS_PSMT4HL || psm == GS_PSMT4HH ? 16 : 0;
}

/* GSLocalMemory's block size (bs) and whether the format's bits fill each word (fmsk). */
static void pack_block(unsigned psm, unsigned *bw, unsigned *bh, int *whole)
{
    *bw = psm == GS_PSMT8 || (psm & 7) == 2 ? 16 : psm == GS_PSMT4 ? 32 : 8;
    *bh = psm == GS_PSMT8 || psm == GS_PSMT4 ? 16 : 8;
    *whole = psm == GS_PSMCT32 || psm == GS_PSMT8 || psm == GS_PSMT4 || psm == GS_PSMZ32;
}

static uint8_t *pack_room(size_t used, size_t more)
{
    if (used + more > pack.scratch_size) {
        size_t size = pack.scratch_size ? pack.scratch_size : 1 << 20;
        while (size < used + more) size *= 2;
        uint8_t *grown = realloc(pack.scratch, size);
        if (!grown) return NULL;
        pack.scratch = grown; pack.scratch_size = size;
    }
    return pack.scratch + used;
}

/* What PCSX2 hashes for one level (HashTextureLevel, no region): appended at pack.scratch + used. */
static size_t pack_level(const Gs *g, unsigned psm, unsigned tbp, unsigned tbw, unsigned tw, unsigned th, uint64_t texa,
                         size_t used)
{
    unsigned w = 1u << tw, h = 1u << th, bw, bh;
    int whole;
    pack_block(psm, &bw, &bh, &whole);
    const uint8_t *vram = (const uint8_t *)g->vram;
    if (whole && w >= bw && h >= bh) {
        uint8_t *out = pack_room(used, (size_t)(w / bw) * (h / bh) * 256);
        if (!out) return 0;
        for (unsigned y = 0; y < h; y += bh)
            for (unsigned x = 0; x < w; x += bw, out += 256)
                memcpy(out, vram + ((gs_pixel_address(psm, tbp, tbw, x, y) >> 11) & 0x3FFF) * 256, 256);
        return (size_t)(w / bw) * (h / bh) * 256;
    }
    int indexed = pack_palette_size(psm) != 0;
    size_t bytes = (size_t)w * h * (indexed ? 1 : 4);
    uint8_t *out = pack_room(used, bytes);
    if (!out) return 0;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            uint32_t v = gs_image_pixel(g->vram, psm, tbp, tbw, x, y);
            if (indexed) *out++ = (uint8_t)v;
            else { v = gs_color(v, psm, texa); memcpy(out, &v, 4); out += 4; }
        }
    return bytes;
}

/* GSClut's 32-bit palette (Read32): CT32/CT24 entries as stored, CT16 expanded with TEXA. */
static uint64_t pack_clut_hash(const Gs *g, uint64_t tex0, unsigned entries)
{
    uint32_t palette[256];
    unsigned cpsm = (unsigned)(tex0 >> 51) & 15, csa = (unsigned)(tex0 >> 56) & 31;
    uint64_t texa = g->reg[GS_TEXA];
    for (unsigned i = 0; i < entries; i++) {
        if (cpsm & 2) {
            unsigned c = g->clut[(csa * 16 + i) & 511];
            unsigned alpha = c & 0x8000 ? (unsigned)(texa >> 32) & 255 : (unsigned)texa & 255;
            if ((texa & 0x8000) && !c) alpha = 0;
            palette[i] = (c & 31) << 3 | (c & 0x3E0) << 6 | (c & 0x7C00) << 9 | alpha << 24;
        } else {
            unsigned group = (entries == 16 ? (csa & 15) * 16 : (csa & 15) * 16 + (i & ~15u));
            unsigned at = (entries == 16 ? group : group > 240 ? 240 : group) + (i & 15);
            palette[i] = g->clut[at & 255] | (uint32_t)g->clut[(at & 255) + 256] << 16;
        }
    }
    return XXH3_64bits(palette, entries * 4);
}

static void pack_count(unsigned psm, unsigned tw, unsigned th, uint64_t tex0_hash, uint64_t clut_hash, uint32_t bits,
                       int found, int mips, uint64_t field)
{
    uint64_t key = pack_key(tex0_hash, clut_hash, bits) | 1;
    unsigned i = (unsigned)(key >> 32) & pack.seen_mask;
    while (pack.seen[i] && pack.seen[i] != key) i = (i + 1) & pack.seen_mask;
    if (pack.seen[i] || pack.seen_count >= pack.seen_mask / 2) return;
    pack.seen[i] = key;
    pack.seen_count++;
    pack.psm_seen[psm & 63]++;
    if (found) { pack.seen_hits++; pack.psm_hits[psm & 63]++; pack.seen_mips += mips; }
    if (pack.log) {
        if (pack_palette_size(psm))
            fprintf(pack.log, "%llu,%u,%u,%u,%llx-%llx-%08x,%d,%d\n", (unsigned long long)field, psm, 1u << tw,
                    1u << th, (unsigned long long)tex0_hash, (unsigned long long)clut_hash, bits, found, mips);
        else
            fprintf(pack.log, "%llu,%u,%u,%u,%llx-%08x,%d,%d\n", (unsigned long long)field, psm, 1u << tw, 1u << th,
                    (unsigned long long)tex0_hash, bits, found, mips);
    }
}

static uint64_t pack_field;

TexPackEntry *texpack_find(const Gs *g, unsigned ctxt)
{
    uint64_t tex0 = g->reg[GS_TEX0_1 + ctxt], tex1 = g->reg[GS_TEX1_1 + ctxt], texa = g->reg[GS_TEXA];
    unsigned psm = (unsigned)(tex0 >> 20) & 63, tbp = (unsigned)tex0 & 0x3FFF, tbw = (unsigned)(tex0 >> 14) & 63;
    unsigned tw = (unsigned)(tex0 >> 26) & 15, th = (unsigned)(tex0 >> 30) & 15;
    if (tw > 10 || th > 10) return NULL;
    unsigned entries = pack_palette_size(psm);
    uint32_t bits = psm | tw << 6 | th << 10, plain = bits;
    if (!entries && (psm & 7) != 0)  /* the 16- and 24-bit direct formats: TEXA is part of the name */
        bits |= ((uint32_t)texa & 255) << 15 | ((uint32_t)(texa >> 15) & 1) << 23 | ((uint32_t)(texa >> 32) & 255) << 24;
    uint64_t clut_hash = entries ? pack_clut_hash(g, tex0, entries) : 0;
    size_t used = pack_level(g, psm, tbp, tbw, tw, th, texa, 0);
    if (!used) return NULL;
    uint64_t base_hash = XXH3_64bits(pack.scratch, used);
    /* A mipmapped draw: PCSX2 (full mipmapping) hashes levels 1..MXL after the base. */
    unsigned mxl = (unsigned)(tex1 >> 2) & 7, mmin = (unsigned)(tex1 >> 6) & 7;
    uint64_t mip_hash = 0;
    int mipmapped = mxl > 0 && mmin >= 2 && mmin <= 5;
    if (mipmapped) {
        uint64_t m1 = g->reg[GS_MIPTBP1_1 + ctxt], m2 = g->reg[GS_MIPTBP2_1 + ctxt];
        if (mxl > 6) mxl = 6;
        size_t all = used;
        for (unsigned i = 1; i <= mxl; i++) {
            uint64_t m = i <= 3 ? m1 >> ((i - 1) * 20) : m2 >> ((i - 4) * 20);
            size_t more = pack_level(g, psm, (unsigned)m & 0x3FFF, (unsigned)(m >> 14) & 63, tw > i ? tw - i : 0,
                                     th > i ? th - i : 0, texa, all);
            if (!more) { mipmapped = 0; break; }
            all += more;
        }
        if (mipmapped) mip_hash = XXH3_64bits(pack.scratch, all);
    }
    TexPackEntry *e = NULL;
    int with_mips = 0;
    if (mipmapped && (e = pack_lookup(mip_hash, clut_hash, bits))) with_mips = 1;
    if (!e) e = pack_lookup(base_hash, clut_hash, bits);
    if (!e && plain != bits) {  /* packs dumped without TEXA in the name */
        if (mipmapped && (e = pack_lookup(mip_hash, clut_hash, plain))) with_mips = 1;
        if (!e) e = pack_lookup(base_hash, clut_hash, plain);
    }
    pack_count(psm, tw, th, mipmapped ? mip_hash : base_hash, clut_hash, bits, e != NULL, with_mips, pack_field);
    return e;
}

/* ---- use (GS worker) ---- */

static uint64_t pack_bytes(void) { return (uint64_t)atomic_load(&pack.bytes); }

static void pack_evict(uint64_t field)
{
    /* The least recently drawn first, while over the budget; never one a queued field may draw. */
    while (pack_bytes() > pack.budget) {
        TexPackEntry *oldest = NULL;
        for (unsigned i = 0; i < pack.count; i++) {
            TexPackEntry *e = &pack.entries[i];
            if (e->state == PACK_READY && e->used + 8 < field && (!oldest || e->used < oldest->used)) oldest = e;
        }
        if (!oldest) return;
        pack.device.release(oldest->view);
        oldest->view = NULL;
        atomic_fetch_sub(&pack.bytes, (int64_t)oldest->bytes);
        oldest->state = PACK_IDLE;
        pack.evicted++;
    }
}

/* A queued entry's view once its loader has it, waiting while this field's waiting lasts (the file
   comment); NULL when it isn't there by then. */
static void *pack_wait(TexPackEntry *e, uint64_t field)
{
    if (pack.wait_us <= 0) return NULL;
    uint64_t t0 = SDL_GetTicksNS();
    int64_t spent = 0;
    SDL_LockMutex(pack.lock);
    while (e->state == PACK_QUEUED && spent < pack.wait_us) {
        SDL_WaitConditionTimeout(pack.done, pack.lock, (Sint32)((pack.wait_us - spent + 999) / 1000));
        spent = (int64_t)((SDL_GetTicksNS() - t0) / 1000);
    }
    SDL_UnlockMutex(pack.lock);
    pack.wait_us -= spent;
    pack.waits++, pack.waited_us += (uint64_t)spent;
    if (e->state != PACK_READY) { pack.wait_misses++; return NULL; }
    e->used = field;
    return e->view;
}

void *texpack_view(TexPackEntry *e, uint64_t field)
{
    int state = e->state;
    if (state == PACK_READY) {
        e->used = field;
        return e->view;
    }
    if (state == PACK_QUEUED) return pack_wait(e, field);
    if (state != PACK_IDLE) return NULL;
    if (pack_bytes() > pack.budget) {
        if (pack.last_evict == field) return NULL;
        pack.last_evict = field;
        pack_evict(field);
        if (pack_bytes() > pack.budget) return NULL;
    }
    e->used = field;
    SDL_LockMutex(pack.lock);
    e->state = PACK_QUEUED;
    pack.queue[pack.tail] = e;
    pack.tail = (pack.tail + 1) % (pack.count + 1);
    SDL_SignalCondition(pack.wake);
    SDL_UnlockMutex(pack.lock);
    return pack_wait(e, field);
}

/* Screen 9 phases 1-4: the race's countdown and the race (finding 024: D_0029DED8 is the screen,
   9 the race; D_0043F348 the race phase, 0 loading, 5 waiting for X, 1 countdown, 2-4 racing). */
static int pack_live_race(void)
{
    int screen = *(volatile int32_t *)GUEST(PS2_RAM_BASE + 0x29DED8);
    int phase = *(volatile int32_t *)GUEST(PS2_RAM_BASE + 0x43F348);
    return screen == 9 && phase >= 1 && phase <= 4;
}

void texpack_field(uint64_t field)
{
    pack_field = field;
    pack.wait_us = pack_live_race() ? 0 : PACK_WAIT_MS * 1000;
    uint64_t bytes = pack_bytes();
    if (bytes > pack.peak) pack.peak = bytes;
    if (bytes > pack.budget && field % 30 == 0 && pack.last_evict != field) {
        pack.last_evict = field;
        pack_evict(field);
    }
    if (pack.log && field % 600 == 0) fflush(pack.log);
}

void texpack_report(void)
{
    if (!pack.count) return;
    fprintf(stderr, "textures: %u distinct textures drawn, %u in the pack (%.1f%%; %u named with mip levels), "
            "%ld loaded (%.1f ms each), %ld failed, %llu released, peak %llu MB of %llu MB\n",
            pack.seen_count, pack.seen_hits, pack.seen_count ? 100.0 * pack.seen_hits / pack.seen_count : 0.0,
            pack.seen_mips, pack.loaded, pack.loaded ? (double)pack.load_us / 1000 / pack.loaded : 0.0, pack.failed,
            (unsigned long long)pack.evicted, (unsigned long long)(pack.peak >> 20),
            (unsigned long long)(pack.budget >> 20));
    fprintf(stderr, "textures: %llu draws waited for a replacement (%.1f ms in all), %llu of them in vain\n",
            (unsigned long long)pack.waits, (double)pack.waited_us / 1000, (unsigned long long)pack.wait_misses);
    for (unsigned psm = 0; psm < 64; psm++)
        if (pack.psm_seen[psm])
            fprintf(stderr, "textures: psm %u: %u drawn, %u in the pack\n", psm, pack.psm_seen[psm], pack.psm_hits[psm]);
    if (pack.log) fflush(pack.log);
}

/* Guest-address-only module services. */
int host_service_texpack_available(void) { return texpack_available(); }
