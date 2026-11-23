/* menu_pads_held (menu_pads.c) on the game's own stick test (func_002209A8, compiled from src/):
   the d-pad, the left stick at the menus' edge on either pad, nothing short of it, a pad in its
   digital mode, and the field after an edge (issue #103, finding 250). And menu_is_message: a
   popup with no row to choose is a message a tap answers (issue #105, finding 266). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hle.h"
#include "menu_page.h"

unsigned char D_0029E2A8[8] = {1, 1};  /* both pads there */
int D_00660D20[8] = {7, 7};            /* their mode: 7 analog, 4 digital (func_0021FAD0) */
static int word[2], stick_x[2] = {128, 128}, stick_y[2] = {128, 128};

int func_00220230(int port) { return word[port]; }
int func_002208E8(int axis, int port) { return axis == 2 ? stick_x[port] : axis == 3 ? stick_y[port] : 128; }

static void pad(int port, int buttons, int x, int y) { word[port] = buttons, stick_x[port] = x, stick_y[port] = y; }

int main(void)
{
    assert(menu_pads_held() == 0);
    pad(0, PAD_DOWN | PAD_CROSS, 128, 128);
    assert(menu_pads_held() == (PAD_DOWN | PAD_CROSS));  /* the d-pad and the buttons as before */
    pad(0, 0, 0, 128);
    assert(menu_pads_held() == PAD_LEFT);                /* the stick at each edge */
    pad(0, 0, 255, 128);
    assert(menu_pads_held() == PAD_RIGHT);
    pad(0, 0, 128, 0);
    assert(menu_pads_held() == PAD_UP);
    pad(0, 0, 128, 255);
    assert(menu_pads_held() == PAD_DOWN);
    pad(0, 0, 7, 249);
    assert(menu_pads_held() == (PAD_LEFT | PAD_DOWN));   /* more than 120 from the centre */
    pad(0, 0, 8, 248);
    assert(menu_pads_held() == 0);                       /* 120 is not the edge, as in the game's menus */
    pad(0, 0, 40, 200);
    assert(menu_pads_held() == 0);                       /* a stick part of the way: nothing */
    pad(0, 0, 128, 128);
    pad(1, 0, 128, 0);
    assert(menu_pads_held() == PAD_UP);                  /* the second pad's stick too */
    pad(1, PAD_TRIANGLE, 128, 128);
    assert(menu_pads_held() == PAD_TRIANGLE);
    pad(1, 0, 128, 128);
    D_00660D20[0] = 4;
    pad(0, 0, 0, 0);
    assert(menu_pads_held() == 0);                       /* a pad in its digital mode: no stick */
    D_00660D20[0] = 7;
    pad(0, 0xFFFF, 0, 0);
    assert(menu_pads_held() == -1);                      /* the entry an edge cleared: that field counts nothing */
    pad(0, 0, 128, 128);
    pad(1, 0xFFFF, 0, 0);
    assert(menu_pads_held() == -1);
    printf("menu pads: the d-pad and the stick at the game's edge\n");

    /* rows as the popup keeps them: 16 bytes each, the enabled flag at +6 (menu_page.c Item) */
    static char rows[64 * 16];
    assert(menu_is_message(rows + 6, 4, 16) == 1);   /* the shop's "MOUNTAIN CROSS / ENTRY BIKES ACQUIRED /
                                                        PRESS X TO CONTINUE": four text rows, none enabled */
    assert(menu_is_message(rows + 6, 0, 16) == 1);   /* the memory card's notices: no rows */
    rows[1 * 16 + 6] = 1;
    assert(menu_is_message(rows + 6, 2, 16) == 0);   /* a question: its YES / NO rows take taps themselves */
    assert(menu_is_message(rows + 6, 1, 16) == 1);   /* the enabled row is past the count */
    assert(menu_is_message(rows + 6, 65, 16) == 0);  /* not a popup's count */
    assert(menu_is_message(rows + 6, -1, 16) == 0);
    printf("menu pads: a popup with no row to choose is a message\n");

    /* the legend's tap ranges: every character 10 pixels wide here */
    static const struct { const char *text; int buttons; uint16_t first, last; } legends[] = {
        {GLYPH_CROSS " RETRY         " GLYPH_TRIANGLE " CANCEL", 2, PAD_CROSS, PAD_TRIANGLE},  /* the card notice */
        {GLYPH_CROSS " SELECT  " GLYPH_TRIANGLE " BACK  " GLYPH_SQUARE " INFO", 3, PAD_CROSS, PAD_SQUARE},
        {GLYPH_CROSS " OK " GLYPH_TRIANGLE " BACK " GLYPH_CIRCLE " NO", 3, PAD_CROSS, PAD_CIRCLE},  /* gaps under 2 pads */
    };
    for (unsigned t = 0; t < sizeof legends / sizeof *legends; t++) {
        const char *text = legends[t].text;
        int length = (int)strlen(text), prefix[MENU_PAGE_TEXT + 1];
        for (int i = 0; i <= length; i++) prefix[i] = 10 * i;
        MenuLegendRange r[MENU_LEGEND_BUTTONS];
        int n = menu_legend_ranges(text, length, prefix, r, MENU_LEGEND_BUTTONS);
        assert(n == legends[t].buttons && r[0].button == legends[t].first && r[n - 1].button == legends[t].last);
        for (int k = 0; k < n; k++) {
            assert(r[k].left < r[k].glyph_right && r[k].glyph_right < r[k].right);
            assert(text[r[k].right / 10 - 1] != ' ');                            /* the word ends at its last letter */
            assert(menu_legend_hit(r, n, r[k].left) == r[k].button);             /* the glyph's left edge */
            assert(menu_legend_hit(r, n, (r[k].left + r[k].glyph_right) / 2) == r[k].button);  /* the glyph */
            assert(menu_legend_hit(r, n, (r[k].left + r[k].right) / 2) == r[k].button);        /* the middle */
            assert(menu_legend_hit(r, n, r[k].right - 1) == r[k].button);        /* the word's right edge */
            if (k + 1 < n) {
                assert(r[k].to <= r[k + 1].from);                                /* never overlapping */
                int gap = r[k + 1].left - r[k].right, middle = (r[k].right + r[k + 1].left) / 2;
                if (gap > 12) assert(menu_legend_hit(r, n, middle) == 0);        /* a wide gap's middle: nothing */
                else assert(r[k].to == middle && r[k + 1].from == middle);      /* a narrow one split in two */
            }
        }
        assert(menu_legend_hit(r, n, r[0].left - 6) == r[0].button);             /* padding outside the legend */
        assert(menu_legend_hit(r, n, r[0].left - 7) == 0);
        assert(menu_legend_hit(r, n, r[n - 1].right + 5) == r[n - 1].button);
        assert(menu_legend_hit(r, n, r[n - 1].right + 6) == 0);
    }
    /* the card notice: CANCEL's glyph and every pixel of its word are Triangle, RETRY's never */
    {
        const char *text = legends[0].text;
        int length = (int)strlen(text), prefix[MENU_PAGE_TEXT + 1];
        for (int i = 0; i <= length; i++) prefix[i] = 10 * i;
        MenuLegendRange r[MENU_LEGEND_BUTTONS];
        int n = menu_legend_ranges(text, length, prefix, r, MENU_LEGEND_BUTTONS);
        for (int x = r[1].left; x < r[1].right; x++) assert(menu_legend_hit(r, n, x) == PAD_TRIANGLE);
        for (int x = r[0].left; x < r[0].right; x++) assert(menu_legend_hit(r, n, x) == PAD_CROSS);
    }
    /* the playlist editor (finding 274): "○ ADD □ CHANGE R1 DELETE △ BACK ✕ SELECT", each word its own
       button, DELETE R1's (it ran into CHANGE's range before R1 was a glyph); OPTIONS' "◂ LESS ▸ MORE" */
    {
        static const char editor[] = GLYPH_CIRCLE " ADD " GLYPH_SQUARE " CHANGE " GLYPH_R1 " DELETE " GLYPH_TRIANGLE
                                     " BACK " GLYPH_CROSS " SELECT";
        static const uint16_t want[5] = {PAD_CIRCLE, PAD_SQUARE, PAD_R1, PAD_TRIANGLE, PAD_CROSS};
        static const char *const words[5] = {"ADD", "CHANGE", "DELETE", "BACK", "SELECT"};
        int length = (int)strlen(editor), prefix[MENU_PAGE_TEXT + 1];
        for (int i = 0; i <= length; i++) prefix[i] = 10 * i;
        MenuLegendRange r[MENU_LEGEND_BUTTONS];
        assert(menu_legend_ranges(editor, length, prefix, r, MENU_LEGEND_BUTTONS) == 5);
        for (int k = 0; k < 5; k++) {
            int at = (int)(strstr(editor, words[k]) - editor);
            assert(r[k].button == want[k]);
            for (int i = at; i < at + (int)strlen(words[k]); i++) assert(menu_legend_hit(r, 5, 10 * i + 5) == want[k]);
        }
        static const char options[] = GLYPH_LEFT " LESS " GLYPH_RIGHT " MORE " GLYPH_TRIANGLE " BACK " GLYPH_CROSS " SELECT";
        length = (int)strlen(options);
        for (int i = 0; i <= length; i++) prefix[i] = 10 * i;
        assert(menu_legend_ranges(options, length, prefix, r, MENU_LEGEND_BUTTONS) == 4);
        assert(r[0].button == PAD_LEFT && r[1].button == PAD_RIGHT && menu_legend_hit(r, 4, 35) == PAD_LEFT &&
               menu_legend_hit(r, 4, 95) == PAD_RIGHT);
    }
    /* the legend bar (finding 277): the main menu's "▲ PREV ▼ NEXT           ✕ SELECT  △ BACK" (retail
       D_0028EFA8), PREV Up and NEXT Down, the wide gap before SELECT nobody's; the rider screen's with
       □ INFO between NEXT and SELECT (D_0028EFD8) */
    {
        static const char bar[] = GLYPH_UP " PREV " GLYPH_DOWN " NEXT           " GLYPH_CROSS " SELECT  " GLYPH_TRIANGLE " BACK";
        static const char rider[] = GLYPH_LEFT " PREV " GLYPH_RIGHT " NEXT   " GLYPH_SQUARE " INFO  " GLYPH_CROSS " SELECT  " GLYPH_TRIANGLE " BACK";
        static const uint16_t want[4] = {PAD_UP, PAD_DOWN, PAD_CROSS, PAD_TRIANGLE};
        static const uint16_t want_rider[5] = {PAD_LEFT, PAD_RIGHT, PAD_SQUARE, PAD_CROSS, PAD_TRIANGLE};
        static const char *const words[5] = {"PREV", "NEXT", "INFO", "SELECT", "BACK"};
        int length = (int)strlen(bar), prefix[MENU_PAGE_TEXT + 1];
        for (int i = 0; i <= length; i++) prefix[i] = 10 * i;
        MenuLegendRange r[MENU_LEGEND_BUTTONS];
        assert(menu_legend_ranges(bar, length, prefix, r, MENU_LEGEND_BUTTONS) == 4);
        for (int k = 0; k < 4; k++) {
            int at = (int)(strstr(bar, words[k < 2 ? k : k + 1]) - bar);
            assert(r[k].button == want[k] && menu_legend_hit(r, 4, 10 * at - 15) == want[k]);   /* its glyph */
            for (int i = at; i < at + (int)strlen(words[k < 2 ? k : k + 1]); i++) assert(menu_legend_hit(r, 4, 10 * i + 5) == want[k]);
        }
        assert(menu_legend_hit(r, 4, (r[1].right + r[2].left) / 2) == 0);   /* the gap before SELECT */
        length = (int)strlen(rider);
        for (int i = 0; i <= length; i++) prefix[i] = 10 * i;
        assert(menu_legend_ranges(rider, length, prefix, r, MENU_LEGEND_BUTTONS) == 5);
        for (int k = 0; k < 5; k++) {
            int at = (int)(strstr(rider, words[k]) - rider);
            assert(r[k].button == want_rider[k] && menu_legend_hit(r, 5, 10 * at + 15) == want_rider[k]);
        }
    }
    assert(menu_legend_ranges("RETRY", 5, (const int[6]){0, 10, 20, 30, 40, 50}, NULL, 0) == 0);  /* no glyph: no buttons */
    printf("menu pads: a legend's buttons, each word its own, never overlapping\n");
    /* MUSIC PLAYLIST > CUSTOM's legend lines (finding 274): each line's words take its button, the
       track list beside them never (a 12th track's number at (112, 157): a tap at (115, 162) is no USE) */
    {
        const int widths[3] = {40, 46, 62}, none[3] = {0, 46, 0};
        assert(menu_custom_legend_hit(widths, 115, 162) == 0 && menu_custom_legend_hit(widths, 112, 160) == 0);
        assert(menu_custom_legend_hit(widths, 35, 164) == PAD_CROSS && menu_custom_legend_hit(widths, 69, 160) == PAD_CROSS);
        assert(menu_custom_legend_hit(widths, 76, 164) == 0);  /* past USE's word */
        assert(menu_custom_legend_hit(widths, 60, 174) == PAD_TRIANGLE && menu_custom_legend_hit(widths, 70, 185) == PAD_CIRCLE);
        assert(menu_custom_legend_hit(widths, 70, 189) == 0 && menu_custom_legend_hit(widths, 70, 158) == 0);
        assert(menu_custom_legend_hit(none, 35, 164) == 0 && menu_custom_legend_hit(none, 35, 185) == 0);  /* not drawn */
        assert(menu_custom_legend_hit(none, 35, 174) == PAD_TRIANGLE);
    }
    printf("menu pads: MUSIC PLAYLIST's CUSTOM legend lines by their words, not the tracks beside\n");
    /* beside a line (finding 268): left of its words Left, right of them Right, on them its own button;
       a heading's halves (the middle passed as both ends): one of the two, never none */
    assert(menu_side_button(99, 100, 200) == PAD_LEFT && menu_side_button(0, 100, 200) == PAD_LEFT);
    assert(menu_side_button(100, 100, 200) == 0 && menu_side_button(199, 100, 200) == 0);
    assert(menu_side_button(200, 100, 200) == PAD_RIGHT && menu_side_button(639, 100, 200) == PAD_RIGHT);
    for (int x = -10; x < 650; x++) assert(menu_side_button(x, 110, 110) == (x < 110 ? PAD_LEFT : PAD_RIGHT));
    printf("menu pads: a tap beside a line is its Left or Right, a heading's halves its two\n");
    return 0;
}
