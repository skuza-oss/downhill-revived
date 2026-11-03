/* EXIT GAME: the main menu's fifth row (port/patches_exit.py patches func_001ED5B8, after ONLINE's
   row), which asks first in the game's own popup (menu_page.c, as PC SETTINGS): "Quit Downhill
   Revived?" with No, Return (where the cursor starts) and Yes, Quit, the words of the game's own
   "Are You Sure?" popup. Yes closes the window as its close button does (gs_window_close): the
   game ends at the next field, both dhd.exe go, and nothing is saved that wasn't already.
   Triangle or No goes back to the main menu, the cursor on EXIT GAME. */
#include "hle.h"
#include "menu_page.h"

#include <stdint.h>

void gs_window_close(void);  /* port/gs/present.c */
int port_main_label(const int *table, int index);  /* online_menu.c: rows 0-3 */
int port_main_choice(int shown);                   /* online_menu.c: the row a shown row is */

static MenuPage page;
static int quit;

static void page_build(MenuPage *p)
{
    menu_page_row(p, "No, Return", 1);
    menu_page_row(p, "Yes, Quit", 1);
}

static void page_accept(MenuPage *p, int row)
{
    quit = row == 1;
    menu_page_close(p);
}

/* Main menu row 4 (patched func_001ED5B8): ask, and quit on Yes. */
void port_exit_menu(void)
{
    page.title = "QUIT DOWNHILL REVIVED?";
    page.build = page_build;
    page.accept = page_accept;
    page.back = NULL;  /* Triangle: back to the main menu */
    quit = 0;
    menu_page_run(&page);
    if (quit) {
        hle_log("exit game: chosen in the main menu\n");
        host_service_gs_window_close();
    }
}

/* The main menu's row labels: the game's three, ONLINE (online_menu.c, when offered), EXIT GAME. */
int port_exit_label(const int *table, int index)
{
    index = port_main_choice(index);
    return index == 4 ? (int)(uintptr_t)"EXIT GAME" : port_main_label(table, index);
}
