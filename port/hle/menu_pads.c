/* The pads as the port's own menu screens read them where they read the pads themselves (the
   online room and its dialogs, menu_page.h). The pages the popup manager moves (menu_page.c's
   rows) get the stick from the game already: its reader (func_001909A8) takes a direction when
   the button is down or the left stick is at its edge (func_00220640, func_002209A8). A screen of
   ours that reads the button word (func_00220230) alone answers the d-pad only, and a pad whose
   d-pad comes as the stick (a generic USB pad in its digital mode) not at all (issue #103,
   finding 250). */
#include "hle.h"
#include "menu_page.h"

int func_00220230(int port);           /* the buttons held on a pad (0 when none is there) */
int func_002209A8(int dir, int port);  /* the left stick pushed to its edge, more than 120 from the centre */

static int pad_held(int port)
{
    static const int dirs[4] = {PAD_UP, PAD_RIGHT, PAD_DOWN, PAD_LEFT};
    int held = func_00220230(port) & 0xFFFF;
    if (held == 0xFFFF) return -1;     /* the entry an edge just cleared (func_0021F880): its sticks read 0 too */
    for (int i = 0; i < 4; i++)
        if (func_002209A8(dirs[i], port)) held |= dirs[i];
    return held;
}

int menu_pads_held(void)
{
    int a = pad_held(0), b = pad_held(1);
    return a < 0 || b < 0 ? -1 : a | b;
}

/* A popup of the game whose rows are all text, none to choose (or no rows at all), is a message
   that waits for one button: "MOUNTAIN CROSS / ENTRY BIKES ACQUIRED / PRESS X TO CONTINUE" in the
   shop, the memory card's notices (issue #105, finding 266). A tap anywhere answers it with Cross
   (menu_page.c popup_mouse). `enabled` is the first row's flag, the rows `stride` bytes apart;
   a count out of the popup's 64 rows is not a popup's. */
int menu_is_message(const char *enabled, int count, int stride)
{
    if (count < 0 || count > 64) return 0;
    for (int i = 0; i < count; i++)
        if (enabled[i * stride]) return 0;
    return 1;
}

/* Where each button of a legend can be tapped (finding 266): a button is its glyph and its word,
   up to the next glyph without the spaces before it; prefix[i] is the drawn width of the legend's
   first i characters (i = 0..length), from its left edge. Each range takes LEGEND_PAD pixels more
   on both sides, but never past the middle of the gap to its neighbour, so two ranges never
   overlap and the rest of a wide gap belongs to no button. Returns the number of buttons. */
enum { LEGEND_PAD = 6 };
int menu_legend_ranges(const char *text, int length, const int *prefix, MenuLegendRange *out, int max)
{
    static const struct { char glyph; uint16_t button; } glyphs[] = {
        {GLYPH_CROSS[0], PAD_CROSS}, {GLYPH_TRIANGLE[0], PAD_TRIANGLE}, {GLYPH_CIRCLE[0], PAD_CIRCLE}, {GLYPH_SQUARE[0], PAD_SQUARE}};
    int at[MENU_LEGEND_BUTTONS], n = 0;
    uint16_t button[MENU_LEGEND_BUTTONS];
    for (int i = 0; i < length && n < max && n < MENU_LEGEND_BUTTONS; i++)
        for (unsigned g = 0; g < sizeof glyphs / sizeof *glyphs; g++)
            if (text[i] == glyphs[g].glyph) at[n] = i, button[n++] = glyphs[g].button;
    for (int k = 0; k < n; k++) {
        int end = k + 1 < n ? at[k + 1] : length;
        while (end > at[k] + 1 && text[end - 1] == ' ') end--;
        out[k].button = button[k];
        out[k].left = prefix[at[k]], out[k].glyph_right = prefix[at[k] + 1], out[k].right = prefix[end];
    }
    for (int k = 0; k < n; k++) {
        int lo = out[k].left - LEGEND_PAD, hi = out[k].right + LEGEND_PAD;
        if (k > 0) {
            int middle = (out[k - 1].right + out[k].left) / 2;
            if (lo < middle) lo = middle;
        }
        if (k + 1 < n) {
            int middle = (out[k].right + out[k + 1].left) / 2;
            if (hi > middle) hi = middle;
        }
        out[k].from = lo, out[k].to = hi;
    }
    return n;
}

/* The button whose range [from, to) holds x, 0 none. */
uint16_t menu_legend_hit(const MenuLegendRange *ranges, int n, int x)
{
    for (int k = 0; k < n; k++)
        if (x >= ranges[k].from && x < ranges[k].to) return ranges[k].button;
    return 0;
}

/* What a tap beside a line of a menu presses (finding 268): left of its words [left, right) Left, right
   of them Right, on them 0 (the line's own button: Cross, as before). A line whose two sides are its
   only buttons (the shop's heading "◄ INVENTORY ►") passes its middle as both: its left half is Left
   and its right half Right. */
uint16_t menu_side_button(int x, int left, int right)
{
    return x < left ? PAD_LEFT : x >= right ? PAD_RIGHT : 0;
}
