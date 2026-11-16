/* menu_pads_held (menu_pads.c) on the game's own stick test (func_002209A8, compiled from src/):
   the d-pad, the left stick at the menus' edge on either pad, nothing short of it, a pad in its
   digital mode, and the field after an edge (issue #103, finding 250). And menu_is_message: a
   popup with no row to choose is a message a tap answers (issue #105, finding 266). */
#include <assert.h>
#include <stdio.h>
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
    return 0;
}
