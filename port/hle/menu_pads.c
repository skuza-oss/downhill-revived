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
