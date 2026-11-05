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

   The icons follow player 1's controller layout (dhd.ini pad1, finding 157): a PS2 button's cell
   shows the icon of the controller button chosen for it (its main one), one of the eight above
   by its place; with the PlayStation icons the game's own icon of that place (Cross on Circle's
   place shows Circle's). A button on a place without an icon (the d-pad, Start, a stick pressed)
   keeps the PS2's icon. Without a controller (the keyboard) the game's icons stay as they are.

   The style and the places are chosen on the game thread (host.c, each field: the setting, player
   1's pad and layout) as one key (style + 3 x the places, a base-9 digit a cell: 0 its own, else
   the cell whose place it shows, plus 1), mixed into the font texture's key (native_core.inc), so
   a change makes a new copy; 0 is the game's own icons, untouched. */
#include "button_icons.h"
#include "../render/include/dhd_render.h"  /* dhdr_png_size, dhdr_png_decode */
#include "../hle/controls_table.h"           /* the layout's targets and controller inputs */
#include "font_sharp.h"
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_stdinc.h>
#include <stdlib.h>
#include <string.h>

#include "button_icons.inc"  /* button_pngs[]: { name, PNG bytes, size }, port/res/buttons */

static SDL_AtomicInt style;

/* The game's icon cells, the PS2 button each is (controls_table.h's name), the controller input
   in its place (the default layout's) and the icon each style puts there. */
enum { CELLS = 8 };
static const struct { uint8_t code; const char *target, *place, *xbox, *nintendo; } cells[CELLS] = {
    {0xAD, "cross", "south", "A", "B"}, {0xAB, "circle", "east", "B", "A"},     /* Cross Circle */
    {0xA0, "square", "west", "X", "Y"}, {0xA7, "triangle", "north", "Y", "X"},  /* Square Triangle */
    {0xB2, "l1", "lb", "LB", "L"},      {0xB3, "l2", "lt", "LT", "ZL"},         /* L1 L2 */
    {0xB9, "r1", "rb", "RB", "R"},      {0xBA, "r2", "rt", "RT", "ZR"},         /* R1 R2 */
};

static int target_of(const char *name)
{
    for (int t = 0; t < CONTROL_BUTTONS; t++)
        if (!strcmp(control_targets[t].name, name)) return t;
    return -1;
}

static const char *input_name(int id)
{
    for (int i = 0; i < CONTROL_PAD_INPUTS; i++)
        if (control_pad_inputs[i].id == id) return control_pad_inputs[i].name;
    return "";
}

/* The places as the key's digits: each cell's 0 (its own) or 1 + the cell whose place its PS2
   button's main input has. */
static int places(const struct ControlsLayout *pad)
{
    int key = 0;
    for (int n = CELLS - 1; n >= 0; n--) {
        int t = target_of(cells[n].target), digit = 0;
        const char *place = t >= 0 ? input_name(pad->input[t][0]) : "";
        for (int m = 0; m < CELLS; m++)
            if (m != n && !strcmp(cells[m].place, place)) digit = m + 1;
        key = key * 9 + digit;
    }
    return key;
}

void button_icons_choose(const char *setting, int auto_style, const struct ControlsLayout *pad)
{
    static int installed;
    if (!installed++) font_sharp_icons(button_icons_style, button_icons_paint);
    int s = BUTTON_ICONS_GAME;
    if (setting && !SDL_strcasecmp(setting, "xbox")) s = BUTTON_ICONS_XBOX;
    else if (setting && !SDL_strcasecmp(setting, "nintendo")) s = BUTTON_ICONS_NINTENDO;
    else if (setting && !SDL_strcasecmp(setting, "auto")) s = auto_style;
    SDL_SetAtomicInt(&style, s + 3 * (pad ? places(pad) : 0));
}

int button_icons_style(void) { return SDL_GetAtomicInt(&style); }

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

/* The game's own icons in other cells (the PlayStation icons on a layout that moves them): each
   cell's copy as it was, then in the cells that show its place. */
static void move_game_icons(uint8_t *big, unsigned f, unsigned W, const int source[CELLS])
{
    const unsigned cell = 32 * f;
    uint8_t *was = malloc((size_t)CELLS * cell * cell * 4);
    if (!was) return;
    for (int n = 0; n < CELLS; n++)
        for (unsigned y = 0; y < cell; y++)
            memcpy(was + ((size_t)n * cell + y) * cell * 4,
                   big + ((size_t)((cells[n].code >> 4) * cell + y) * W + (cells[n].code & 15) * cell) * 4, (size_t)cell * 4);
    for (int n = 0; n < CELLS; n++)
        if (source[n] != n)
            for (unsigned y = 0; y < cell; y++)
                memcpy(big + ((size_t)((cells[n].code >> 4) * cell + y) * W + (cells[n].code & 15) * cell) * 4,
                       was + ((size_t)source[n] * cell + y) * cell * 4, (size_t)cell * 4);
    free(was);
}

void button_icons_paint(uint8_t *big, unsigned f, const uint32_t *pixels, unsigned w, unsigned h, unsigned solid, int key)
{
    int st = key % 3, source[CELLS];
    for (int n = 0, rest = key / 3; n < CELLS; n++, rest /= 9) source[n] = rest % 9 ? rest % 9 - 1 : n;
    if (!key || w != 512 || h != 512 || !f) return;
    const unsigned cell = 32, W = w * f;
    if (st == BUTTON_ICONS_GAME) {
        move_game_icons(big, f, W, source);
        return;
    }
    for (unsigned n = 0; n < CELLS; n++) {
        const Icon *c = icon(st == BUTTON_ICONS_XBOX ? cells[source[n]].xbox : cells[source[n]].nintendo);
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
