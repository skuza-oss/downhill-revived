/* The menus' font copy with the button icons over a texture pack's replacement of it (font_sharp.c,
   button_icons.c; findings 134 and 200), through font_sharp_view with a fake renderer device and
   synthetic pictures only: a 512 x 512 atlas whose eight icon cells hold a disc and the others a
   bar, and a pack picture that fills each cell with its own colour. Built and run by
   port/tools/test_texpack.py. Prints PASS/FAIL lines, then RESULT. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "button_icons.h"
#include "font_sharp.h"
#include "../hle/controls_table.h"

uintptr_t guest_base;  /* font_sharp_match's EE memory; not called here */
void dhd_host_log(const char *line, size_t bytes) { fwrite(line, 1, bytes, stderr); }  /* the Rust library's log */

static int failures;
#define CHECK(cond, ...) do { if (cond) { printf("PASS "); } else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- the fake device: keeps each made picture's first level ---- */
typedef struct { unsigned w, h; uint8_t *pixels; int refs; } View;
static int created, released;
static void *fake_create(void *context, const uint8_t *pixels, unsigned w, unsigned h, unsigned levels)
{
    (void)context, (void)levels;
    View *v = calloc(1, sizeof *v);
    v->w = w, v->h = h, v->refs = 1;
    v->pixels = malloc((size_t)w * h * 4);
    memcpy(v->pixels, pixels, (size_t)w * h * 4);
    created++;
    return v;
}
static void fake_retain(void *view) { ((View *)view)->refs++; }
static void fake_release(void *view)
{
    View *v = view;
    if (v && !--v->refs) { free(v->pixels); free(v); released++; }
}
static const ReplaceDevice device = {fake_create, fake_retain, fake_release, NULL};

/* ---- the pictures ---- */
static const uint8_t icon_cells[8] = {0xAD, 0xAB, 0xA0, 0xA7, 0xB2, 0xB3, 0xB9, 0xBA};
static int is_icon(unsigned cell)
{
    for (int i = 0; i < 8; i++) if (icon_cells[i] == cell) return 1;
    return 0;
}

static uint32_t atlas[512 * 512];
static void make_atlas(void)
{
    for (unsigned y = 0; y < 512; y++)
        for (unsigned x = 0; x < 512; x++) {
            unsigned cell = (y / 32) * 16 + x / 32, cx = x % 32, cy = y % 32;
            int on = is_icon(cell) ? (cx - 16) * (cx - 16) + (cy - 16) * (cy - 16) < 11 * 11
                                   : cx >= 10 && cx < 22 && cy >= 6 && cy < 26;
            atlas[y * 512 + x] = on ? 0x40FFFFFFu : 0;  /* the menus' font: solid is 0x40 */
        }
}

/* The pack's picture, side x side: each cell its own colour (red = the cell), alpha 0x40 inside a
   margin. */
static unsigned pack_side;
static int pack_loads;
static uint8_t *pack_load(void *context, unsigned *w, unsigned *h)
{
    (void)context;
    pack_loads++;
    if (!pack_side) return NULL;
    uint8_t *p = malloc((size_t)pack_side * pack_side * 4);
    unsigned cell_side = pack_side / 16 ? pack_side / 16 : 1;
    for (unsigned y = 0; y < pack_side; y++)
        for (unsigned x = 0; x < pack_side; x++) {
            unsigned cell = (y * 16 / pack_side) * 16 + x * 16 / pack_side, cx = x % cell_side, cy = y % cell_side;
            int inside = cx >= cell_side / 8 && cx < cell_side * 7 / 8 && cy >= cell_side / 8 && cy < cell_side * 7 / 8;
            uint8_t *o = p + ((size_t)y * pack_side + x) * 4;
            o[0] = (uint8_t)cell, o[1] = 77, o[2] = 200, o[3] = inside ? 0x40 : 0;
        }
    *w = *h = pack_side;
    return p;
}
static const FontSharpPack pack = {pack_load, NULL};

/* The cell's texels in a W x W copy equal (or not) the pack picture's, made again at that size. */
static int cell_equal(const View *v, unsigned cell, const uint8_t *picture, unsigned picture_cell)
{
    unsigned c = v->w / 16;
    for (unsigned y = 0; y < c; y++)
        if (memcmp(v->pixels + ((size_t)((cell >> 4) * c + y) * v->w + (cell & 15) * c) * 4,
                   picture + ((size_t)((picture_cell >> 4) * c + y) * v->w + (picture_cell & 15) * c) * 4, (size_t)c * 4))
            return 0;
    return 1;
}
static int has_colour(const View *v, unsigned cell, uint8_t red)
{
    unsigned c = v->w / 16;
    for (unsigned y = 0; y < c; y++)
        for (unsigned x = 0; x < c; x++) {
            const uint8_t *p = v->pixels + ((size_t)((cell >> 4) * c + y) * v->w + (cell & 15) * c + x) * 4;
            if (p[3] && p[0] == red && p[1] == 77 && p[2] == 200) return 1;
        }
    return 0;
}

static ControlsLayout layout(int swap_cross_circle)
{
    ControlsLayout l = control_defaults[CONTROL_PAD1];
    if (swap_cross_circle) {
        uint8_t t = l.input[0][0];  /* targets 0 and 1: cross and circle */
        l.input[0][0] = l.input[1][0], l.input[1][0] = t;
    }
    return l;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    make_atlas();
    ControlsLayout plain = layout(0), swapped = layout(1);
    uint64_t key = 1;

    /* The game's own icons with a pack: no copy, the pack's own is drawn. */
    button_icons_choose("playstation", BUTTON_ICONS_GAME, &plain);
    pack_side = 1024;
    View *v = font_sharp_view(&device, atlas, 512, 512, key++, &pack);
    CHECK(!v && !pack_loads, "PlayStation icons in their places: no copy over the pack (the pack's own is drawn), its picture not read");

    /* Xbox icons over a 2x pack: the pack's picture as it is, the icon cells painted. */
    button_icons_choose("xbox", BUTTON_ICONS_GAME, &plain);
    uint8_t *picture = pack_load(NULL, &(unsigned){0}, &(unsigned){0});
    v = font_sharp_view(&device, atlas, 512, 512, key++, &pack);
    CHECK(v && v->w == 1024 && v->h == 1024, "Xbox icons, 1024 x 1024 pack: a 1024 x 1024 copy (%u x %u)", v ? v->w : 0, v ? v->h : 0);
    if (v) {
        int letters = 1, icons = 1;
        for (unsigned cell = 0; cell < 256; cell++)
            if (!is_icon(cell)) letters &= cell_equal(v, cell, picture, cell);
            else icons &= !has_colour(v, cell, (uint8_t)cell);
        CHECK(letters, "Xbox icons: every other cell is the pack's, texel for texel");
        CHECK(icons, "Xbox icons: no icon cell keeps the pack's picture");
        unsigned c = 64, lit = 0;
        for (unsigned y = 0; y < c; y++)
            for (unsigned x = 0; x < c; x++)
                lit += v->pixels[((size_t)(0xA * c + y) * 1024 + 0xD * c + x) * 4 + 3] != 0;
        CHECK(lit > c * c / 8, "Xbox icons: the Cross cell holds the A icon (%u of %u texels drawn)", lit, c * c);
        fake_release(v);
    }

    /* Nintendo icons over a 4x pack and a pack of an odd size (resampled to 2x). */
    button_icons_choose("nintendo", BUTTON_ICONS_GAME, &plain);
    pack_side = 2048;
    v = font_sharp_view(&device, atlas, 512, 512, key++, &pack);
    CHECK(v && v->w == 2048, "Nintendo icons, 2048 x 2048 pack: a 2048 x 2048 copy (%u)", v ? v->w : 0);
    fake_release(v);
    pack_side = 1000;
    v = font_sharp_view(&device, atlas, 512, 512, key++, &pack);
    CHECK(v && v->w == 1024, "1000 x 1000 pack: resampled to the nearest whole scale, 1024 x 1024 (%u)", v ? v->w : 0);
    fake_release(v);

    /* The PlayStation icons with Cross and Circle swapped (finding 157): the pack's own icon
       cells, moved. */
    button_icons_choose("playstation", BUTTON_ICONS_GAME, &swapped);
    pack_side = 1024;
    free(picture);
    picture = pack_load(NULL, &(unsigned){0}, &(unsigned){0});
    v = font_sharp_view(&device, atlas, 512, 512, key++, &pack);
    CHECK(v && cell_equal(v, 0xAD, picture, 0xAB) && cell_equal(v, 0xAB, picture, 0xAD) && cell_equal(v, 0xA0, picture, 0xA0),
          "PlayStation icons, Cross and Circle swapped: the pack's Circle cell in Cross's place and back, Square's kept");
    fake_release(v);
    free(picture);

    /* A pack picture that can't be read: the copy from the game's font, with the icons. */
    button_icons_choose("xbox", BUTTON_ICONS_GAME, &plain);
    pack_side = 0;
    v = font_sharp_view(&device, atlas, 512, 512, key++, &pack);
    CHECK(v && v->w == 1024, "an unreadable pack picture: the copy is made from the game's font (%u)", v ? v->w : 0);
    fake_release(v);

    /* The font's 128 x 128 texture (no icons there) with a pack: the pack's own. */
    v = font_sharp_view(&device, atlas, 128, 128, key++, &pack);
    CHECK(!v, "another font texture with a pack: no copy");

    printf(failures ? "RESULT FAIL %d\n" : "RESULT PASS\n", failures);
    return failures != 0;
}
