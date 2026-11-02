/* The game's font drawn sharp at high resolutions (finding 058).

   The text engine (func_0020F058 .. func_00210250, finding 050) draws every glyph as a sprite
   from one small 4-bit texture: 16x16 cells, glyphs about 11 pixels high, a white fill inside a
   dark outline with soft edges. At 2x-4x the native renderer magnifies those few texels and text
   looks blurred. Here the texture the game uploaded from the player's disc becomes, in memory and
   only while the game runs, one 8 times larger whose edges are sharp at any size:
   - each channel is magnified with a cubic B-spline (smooth contours, no ringing);
   - the edges are then cut again, as a signed distance field's are when it is drawn: the
     coverage (alpha) and the fill (the position between the outline and the fill colour) each
     pass a narrow smoothstep around their middle, about one texel of the large texture wide.
   The renderer draws it in place of the original with mipmaps (as a texture pack's replacement;
   a pack that has the font wins). Nothing is written to disk. dhd.ini font = original keeps the
   game's texture. The renderer makes the texture (replace_texture.h). */
#include <SDL3/SDL_atomic.h>  /* SDL_SpinLock, SDL_strcasecmp; the web build swaps in its one-thread stub */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font_sharp.h"
#include "memory.h"
#include "../include/guest.h"

#define SHARP_SCALE 8        /* magnification of the font texture */
#define SHARP_MAX_SIDE 1024  /* largest side made: the 512x512 menu font, 32x32 cells, is doubled */
#define SHARP_EDGE 0.75f     /* half the edge ramp, in texels of the large texture */
#define SHARP_FILL 0.55f     /* fill edge: a little toward the fill keeps strokes as thin as the original's */
#define SHARP_CACHE 4

/* The text packets' TEX0 commands, one per view (func_00210068; func_00210250 sets a scene
   font's). */
#define TEXT_TEX0_COMMANDS 0x4F35B0u

int font_sharp_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("DHD_FONT");
        on = !(v && (!SDL_strcasecmp(v, "original") || !SDL_strcasecmp(v, "off") || !strcmp(v, "0")));
    }
    return on;
}

static int (*icons_style)(void);
static FontSharpPaint *icons_paint;

void font_sharp_icons(int (*style)(void), FontSharpPaint *paint) { icons_style = style, icons_paint = paint; }
int font_sharp_icon_style(void) { return icons_style ? icons_style() : 0; }
int font_sharp_active(void) { return font_sharp_on() || font_sharp_icon_style(); }

int font_sharp_match(uint64_t tex0)
{
    /* TBP, TBW, PSM, TW, TH, CBP, CPSM and CSM: where the texture and its palette are, not how
       it is drawn. */
    const uint64_t mask = 0x3FFFFFFFFull | (0x7FFFFull << 37);
    for (unsigned i = 0; i < 4; i++) {
        uint32_t command = *(volatile uint32_t *)GUEST(PS2_RAM_BASE + ((TEXT_TEX0_COMMANDS + 4 * i) & 0x01FFFFFFu));
        if (!command) continue;
        uint64_t font = *(volatile uint64_t *)GUEST(PS2_RAM_BASE + (command & 0x01FFFFFFu));
        if (((font ^ tex0) & mask) == 0) return 1;
    }
    return 0;
}

static struct { uint64_t key; void *view; unsigned long used; } cache[SHARP_CACHE];
static void (*cache_release)(void *view);  /* the device the cached views came from */
static unsigned long cache_clock;
static SDL_SpinLock cache_lock;

/* The cubic B-spline's four weights at fraction t. */
static void bspline(float t, float w[4])
{
    float t2 = t * t, t3 = t2 * t, u = 1 - t;
    w[0] = u * u * u / 6;
    w[1] = (3 * t3 - 6 * t2 + 4) / 6;
    w[2] = (-3 * t3 + 3 * t2 + 3 * t + 1) / 6;
    w[3] = t3 / 6;
}

static float smooth(float edge, float half, float x)
{
    float t = (x - (edge - half)) / (2 * half);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

static float luma(const float *c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }

/* Magnifies src (w x h, 4 floats a texel, alpha 1 where the font is solid) by f in both
   directions into dst (8-bit RGBA, alpha 1 as solid) and cuts its edges. */
static void sharpen(const float *src, unsigned w, unsigned h, unsigned f, unsigned solid, uint8_t *dst)
{
    unsigned W = w * f, H = h * f;
    float *rows = malloc((size_t)W * h * 4 * sizeof *rows);
    if (!rows) return;
    /* The outline and fill colours: the brightest opaque texels and the soft edge's. */
    float fill[3] = {0}, outline[3] = {0}, top = 0;
    unsigned nf = 0, no = 0;
    for (unsigned i = 0; i < w * h; i++)
        if (src[i * 4 + 3] >= 0.99f && luma(src + i * 4) > top) top = luma(src + i * 4);
    for (unsigned i = 0; i < w * h; i++) {
        const float *c = src + i * 4;
        if (c[3] >= 0.99f && luma(c) >= top - 0.02f) { for (int k = 0; k < 3; k++) fill[k] += c[k]; nf++; }
        else if (c[3] > 0 && c[3] < 0.99f) { for (int k = 0; k < 3; k++) outline[k] += c[k]; no++; }
    }
    float axis[3], len2 = 0;
    for (int k = 0; k < 3; k++) {
        fill[k] /= nf ? nf : 1; outline[k] /= no ? no : 1;
        axis[k] = fill[k] - outline[k]; len2 += axis[k] * axis[k];
    }
    int two_tone = nf && no && fabsf(luma(fill) - luma(outline)) > 0.25f;
    /* Across, then down. */
    for (unsigned y = 0; y < h; y++)
        for (unsigned X = 0; X < W; X++) {
            float p = (X + 0.5f) / f - 0.5f, wt[4], acc[4] = {0};
            int x0 = (int)floorf(p);
            bspline(p - x0, wt);
            for (int k = 0; k < 4; k++) {
                int x = x0 - 1 + k;
                x = x < 0 ? 0 : x >= (int)w ? (int)w - 1 : x;
                const float *c = src + ((size_t)y * w + x) * 4;
                for (int ch = 0; ch < 4; ch++) acc[ch] += wt[k] * c[ch];
            }
            memcpy(rows + ((size_t)y * W + X) * 4, acc, sizeof acc);
        }
    float half = SHARP_EDGE / f;
    for (unsigned Y = 0; Y < H; Y++) {
        float p = (Y + 0.5f) / f - 0.5f, wt[4];
        int y0 = (int)floorf(p);
        bspline(p - y0, wt);
        const float *r[4];
        for (int k = 0; k < 4; k++) {
            int y = y0 - 1 + k;
            y = y < 0 ? 0 : y >= (int)h ? (int)h - 1 : y;
            r[k] = rows + (size_t)y * W * 4;
        }
        for (unsigned X = 0; X < W; X++) {
            float c[4];
            for (int ch = 0; ch < 4; ch++)
                c[ch] = wt[0] * r[0][X * 4 + ch] + wt[1] * r[1][X * 4 + ch] + wt[2] * r[2][X * 4 + ch] + wt[3] * r[3][X * 4 + ch];
            if (two_tone) {
                float t = 0;
                for (int k = 0; k < 3; k++) t += (c[k] - outline[k]) * axis[k];
                t /= len2;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                float d = smooth(SHARP_FILL, half, t) - t;
                for (int k = 0; k < 3; k++) c[k] += d * axis[k];
            }
            c[3] = smooth(0.5f, half, c[3]);
            uint8_t *o = dst + ((size_t)Y * W + X) * 4;
            for (int ch = 0; ch < 3; ch++) {
                float v = c[ch] < 0 ? 0 : c[ch] > 1 ? 1 : c[ch];
                o[ch] = (uint8_t)(v * 255 + 0.5f);
            }
            o[3] = (uint8_t)(c[3] * solid + 0.5f);
        }
    }
    free(rows);
}

static void halve(const uint8_t *src, unsigned w, unsigned h, uint8_t *dst)
{
    unsigned hw = w > 1 ? w / 2 : 1, hh = h > 1 ? h / 2 : 1;
    for (unsigned y = 0; y < hh; y++)
        for (unsigned x = 0; x < hw; x++)
            for (unsigned ch = 0; ch < 4; ch++) {
                unsigned x1 = w > 1 ? 2 * x + 1 : x, y1 = h > 1 ? 2 * y + 1 : y;
                unsigned s = src[((size_t)2 * y * w + 2 * x) * 4 + ch] + src[((size_t)2 * y * w + x1) * 4 + ch] +
                             src[((size_t)y1 * w + 2 * x) * 4 + ch] + src[((size_t)y1 * w + x1) * 4 + ch];
                dst[((size_t)y * hw + x) * 4 + ch] = (uint8_t)((s + 2) / 4);
            }
}

static void *make(const ReplaceDevice *device, const uint32_t *pixels, unsigned w, unsigned h)
{
    unsigned f = SHARP_SCALE;
    while (f > 1 && (w * f > SHARP_MAX_SIDE || h * f > SHARP_MAX_SIDE)) f /= 2;
    if (f < 2) return NULL;
    /* Solid is the highest alpha: 0x80 in the executable's font, 0x40 in the menus' (whose
       colour comes from the vertices). */
    unsigned solid = 0;
    for (unsigned i = 0; i < w * h; i++)
        if (pixels[i] >> 24 > solid) solid = pixels[i] >> 24;
    if (!solid || solid > 0x80) return NULL;  /* not a font's alpha */
    float *src = malloc((size_t)w * h * 4 * sizeof *src);
    if (!src) return NULL;
    for (unsigned i = 0; i < w * h; i++) {
        uint32_t p = pixels[i];
        src[i * 4 + 0] = (p & 255) / 255.0f;
        src[i * 4 + 1] = (p >> 8 & 255) / 255.0f;
        src[i * 4 + 2] = (p >> 16 & 255) / 255.0f;
        src[i * 4 + 3] = (float)(p >> 24) / solid;
    }
    unsigned W = w * f, H = h * f, levels = 1;
    size_t total = (size_t)W * H * 4;
    for (unsigned lw = W, lh = H; lw > 1 || lh > 1; levels++) {
        lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
        total += (size_t)lw * lh * 4;
    }
    uint8_t *big = calloc(1, total);
    if (!big) { free(src); return NULL; }
    if (font_sharp_on()) sharpen(src, w, h, f, solid, big);
    else  /* only for the button icons: the font as it is, each texel f x f */
        for (unsigned Y = 0; Y < H; Y++)
            for (unsigned X = 0; X < W; X++) {
                uint32_t p = pixels[(Y / f) * w + X / f];
                uint8_t *o = big + ((size_t)Y * W + X) * 4;
                o[0] = p & 255, o[1] = p >> 8 & 255, o[2] = p >> 16 & 255, o[3] = p >> 24 & 255;
            }
    free(src);
    int style = font_sharp_icon_style();
    if (style && icons_paint) icons_paint(big, f, pixels, w, h, solid, style);
    uint8_t *level = big;
    unsigned lw = W, lh = H;
    for (unsigned i = 0; i + 1 < levels; i++) {
        uint8_t *next = level + (size_t)lw * lh * 4;
        halve(level, lw, lh, next);
        level = next;
        lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
    }
    void *view = device->create(device->context, big, W, H, levels);
    free(big);
    return view;
}

void *font_sharp_view(const ReplaceDevice *device, const uint32_t *pixels, unsigned w, unsigned h, uint64_t key)
{
    void *view = NULL;
    SDL_LockSpinlock(&cache_lock);
    unsigned oldest = 0;
    for (unsigned i = 0; i < SHARP_CACHE; i++) {
        if (cache[i].view && cache[i].key == key) {
            cache[i].used = ++cache_clock;
            view = cache[i].view;
            device->retain(view);
            break;
        }
        if (!cache[i].view || (cache[oldest].view && cache[i].used < cache[oldest].used)) oldest = i;
    }
    if (!view && (view = make(device, pixels, w, h))) {
        if (cache[oldest].view) cache_release(cache[oldest].view);
        cache[oldest].key = key; cache[oldest].view = view; cache[oldest].used = ++cache_clock;
        cache_release = device->release;
        device->retain(view);
    }
    SDL_UnlockSpinlock(&cache_lock);
    return view;
}
