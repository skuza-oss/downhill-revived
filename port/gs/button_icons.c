/* The button icons for the player's controller (dhd.ini prompts, DHD_PROMPTS; finding 134).

   The game names its buttons with icons in its menus' font: the scene's 512x512 atlas (text
   engine mode 1, findings 050 and 058), 16x16 cells of 32 texels, a byte >= 0x80 in a string
   picking a cell like any letter ("\xAD SELECT  \xA7 BACK"). The cells: 0xAD Cross, 0xAB Circle,
   0xA0 Square, 0xA7 Triangle (discs with the symbol), 0xB2 L1, 0xB3 L2, 0xB9 R1, 0xBA R2 (their
   letters); the d-pad's arrows (0x8D, 0x8F, 0x90, 0x9D) stay as they are.

   With Xbox or Nintendo icons chosen (or Auto with such a controller as player 1), the renderer's
   copy of that atlas (font_sharp.c: the replacement it draws in its place, in memory only, finding
   058) gets the project's own icons (port/res/buttons/*.png, 128x128, embedded at build time as
   button_icons.inc) painted over those cells: each fitted, aspect kept, into the box the game's own
   icon fills in its cell, so it has the game's size and place in every string, takes the text's
   colour as the game's icons do (the vertex colour modulates it), and is filtered like the rest of
   the copy (mipmaps; 64 texels a cell at the copy's 2x). By position, as the pads are read
   (gamepad.c): Cross = A on Xbox, B on Nintendo; Circle = B / A; Square = X / Y; Triangle = Y / X;
   L1, L2, R1, R2 = LB, LT, RB, RT / L, ZL, R, ZR. Nothing is written to GS memory: the game, its
   state and the exact renderer see the PS2's icons (the exact renderer draws them too).

   The style is chosen on the game thread (host.c, each field: the setting and player 1's pad) and
   mixed into the font texture's key (native_core.inc), so a change makes a new copy. */
#include "button_icons.h"
#include "../render/include/dhd_render.h"  /* dhdr_png_size, dhdr_png_decode */
#include "font_sharp.h"
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_stdinc.h>
#include <stdlib.h>
#include <string.h>

#include "button_icons.inc"  /* button_pngs[]: { name, PNG bytes, size }, port/res/buttons */

static SDL_AtomicInt style;

void button_icons_choose(const char *setting, int auto_style)
{
    static int installed;
    if (!installed++) font_sharp_icons(button_icons_style, button_icons_paint);
    int s = BUTTON_ICONS_GAME;
    if (setting && !SDL_strcasecmp(setting, "xbox")) s = BUTTON_ICONS_XBOX;
    else if (setting && !SDL_strcasecmp(setting, "nintendo")) s = BUTTON_ICONS_NINTENDO;
    else if (setting && !SDL_strcasecmp(setting, "auto")) s = auto_style;
    SDL_SetAtomicInt(&style, s);
}

int button_icons_style(void) { return SDL_GetAtomicInt(&style); }

/* The game's icon cells and the icon each style puts there. */
static const struct { uint8_t code; const char *xbox, *nintendo; } cells[] = {
    {0xAD, "A", "B"},   {0xAB, "B", "A"},   {0xA0, "X", "Y"},  {0xA7, "Y", "X"},    /* Cross Circle Square Triangle */
    {0xB2, "LB", "L"},  {0xB3, "LT", "ZL"}, {0xB9, "RB", "R"}, {0xBA, "RT", "ZR"},  /* L1 L2 R1 R2 */
};

typedef struct {
    uint8_t *rgba;          /* decoded, 4 bytes a pixel */
    unsigned w, h;
    unsigned x0, y0, x1, y1;  /* its drawn part (alpha > 0), x1/y1 past the end */
    float peak;               /* its brightest opaque grey (the letters), 0..1 */
} Icon;

static Icon icons[sizeof button_pngs / sizeof *button_pngs];

/* The named icon, decoded the first time (font_sharp.c calls under its cache lock). */
static const Icon *icon(const char *name)
{
    for (unsigned i = 0; i < sizeof button_pngs / sizeof *button_pngs; i++) {
        if (strcmp(button_pngs[i].name, name)) continue;
        Icon *c = &icons[i];
        if (!c->rgba) {
            uint32_t w, h;
            if (!dhdr_png_size(button_pngs[i].png, button_pngs[i].size, &w, &h)) return NULL;
            uint8_t *rgba = malloc((size_t)w * h * 4);
            if (!rgba || !dhdr_png_decode(button_pngs[i].png, button_pngs[i].size, rgba, w, h, 255)) {
                free(rgba);
                return NULL;
            }
            unsigned x0 = w, y0 = h, x1 = 0, y1 = 0, peak = 1;
            for (unsigned y = 0; y < h; y++)
                for (unsigned x = 0; x < w; x++)
                    if (rgba[((size_t)y * w + x) * 4 + 3]) {
                        const uint8_t *p = rgba + ((size_t)y * w + x) * 4;
                        if (p[3] > 200)
                            for (int ch = 0; ch < 3; ch++) if (p[ch] > peak) peak = p[ch];
                        if (x < x0) x0 = x;
                        if (y < y0) y0 = y;
                        if (x + 1 > x1) x1 = x + 1;
                        if (y + 1 > y1) y1 = y + 1;
                    }
            if (x1 <= x0) { free(rgba); return NULL; }
            c->w = w, c->h = h, c->x0 = x0, c->y0 = y0, c->x1 = x1, c->y1 = y1, c->peak = peak / 255.0f;
            c->rgba = rgba;
        }
        return c;
    }
    return NULL;
}

/* The icon's colour at (x, y), bilinear, premultiplied by its alpha (0..1 each). */
static void sample(const Icon *c, float x, float y, float out[4])
{
    x -= 0.5f, y -= 0.5f;
    int ix = (int)SDL_floorf(x), iy = (int)SDL_floorf(y);
    float fx = x - ix, fy = y - iy;
    out[0] = out[1] = out[2] = out[3] = 0;
    for (int k = 0; k < 4; k++) {
        int px = ix + (k & 1), py = iy + (k >> 1);
        float wgt = (k & 1 ? fx : 1 - fx) * (k >> 1 ? fy : 1 - fy);
        if (px < 0 || py < 0 || px >= (int)c->w || py >= (int)c->h || wgt <= 0) continue;
        const uint8_t *p = c->rgba + ((size_t)py * c->w + px) * 4;
        float a = p[3] / 255.0f;
        for (int ch = 0; ch < 3; ch++) out[ch] += wgt * a * p[ch] / 255.0f;
        out[3] += wgt * a;
    }
}

void button_icons_paint(uint8_t *big, unsigned f, const uint32_t *pixels, unsigned w, unsigned h, unsigned solid, int st)
{
    if (st == BUTTON_ICONS_GAME || w != 512 || h != 512 || !f) return;
    const unsigned cell = 32, W = w * f;
    for (unsigned n = 0; n < sizeof cells / sizeof *cells; n++) {
        const Icon *c = icon(st == BUTTON_ICONS_XBOX ? cells[n].xbox : cells[n].nintendo);
        if (!c) continue;
        unsigned cx = (cells[n].code & 15) * cell, cy = (cells[n].code >> 4) * cell;
        /* the game's icon in its cell: its box, and its brightest solid colour (the symbol's) */
        unsigned x0 = cell, y0 = cell, x1 = 0, y1 = 0, white = 0;
        for (unsigned y = 0; y < cell; y++)
            for (unsigned x = 0; x < cell; x++) {
                uint32_t p = pixels[(size_t)(cy + y) * w + cx + x];
                if (!(p >> 24)) continue;
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x + 1 > x1) x1 = x + 1;
                if (y + 1 > y1) y1 = y + 1;
                if (p >> 24 >= solid)
                    for (int ch = 0; ch < 24; ch += 8) if ((p >> ch & 255) > white) white = p >> ch & 255;
            }
        if (x1 <= x0 || !white) continue;
        /* the target: that box in the copy, the icon fitted into it with its own shape, centred */
        float bw = (float)(x1 - x0) * f, bh = (float)(y1 - y0) * f;
        float iw = (float)(c->x1 - c->x0), ih = (float)(c->y1 - c->y0);
        float scale = bw / iw < bh / ih ? bw / iw : bh / ih;  /* copy texels per icon pixel */
        float ox = cx * f + x0 * f + (bw - iw * scale) / 2, oy = cy * f + y0 * f + (bh - ih * scale) / 2;
        /* the whole cell is the icon's: the game's one goes */
        for (unsigned y = 0; y < cell * f; y++) memset(big + ((size_t)(cy * f + y) * W + cx * f) * 4, 0, (size_t)cell * f * 4);
        for (unsigned Y = cy * f; Y < (cy + cell) * f; Y++)
            for (unsigned X = cx * f; X < (cx + cell) * f; X++) {
                /* 4 x 4 bilinear taps over the texel's footprint in the icon: an area average */
                float acc[4] = {0};
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        float u = c->x0 + (X + (sx + 0.5f) / 4 - ox) / scale, v = c->y0 + (Y + (sy + 0.5f) / 4 - oy) / scale;
                        if (u < c->x0 || v < c->y0 || u > c->x1 || v > c->y1) continue;
                        float t[4];
                        sample(c, u, v, t);
                        for (int ch = 0; ch < 4; ch++) acc[ch] += t[ch] / 16;
                    }
                if (acc[3] <= 0) continue;
                uint8_t *o = big + ((size_t)Y * W + X) * 4;
                for (int ch = 0; ch < 3; ch++) {  /* the letters' grey is the game's symbol colour */
                    float v = acc[ch] / acc[3] / c->peak * white;
                    o[ch] = (uint8_t)(v > 255 ? 255 : v + 0.5f);
                }
                o[3] = (uint8_t)(acc[3] * solid + 0.5f);
            }
    }
}
