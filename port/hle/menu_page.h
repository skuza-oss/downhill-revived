/* The port's own screens (finding 092): one small layer over the game's popup menus (finding 050).
   A page is a SharedMenu (include/menu.h) in host memory with a copy of the base vtable D_00287218
   whose build, update, accept, back and left/right slots call the page's functions below. The
   game draws the panel, the rows, the highlight, the title and the footer in its own font and
   colours, moves the cursor with up/down and plays its sounds; the page says what its rows are and
   what the buttons do. Every page is laid out as the game's centred popups are (the pause menu):
   the title, then the rows centred under it, then the footer.

   Rows, in order (each call returns the row's index):
   - menu_page_row: a list row or a button; Cross acts on it (accept);
   - menu_page_value: a value picker, "Label:  < Value >"; left/right change it (and Cross, as
     the page decides); arrows 0: shown without them (it can't be changed);
   - menu_page_text: a line to read, never selected.
   The footer (p->footer) says what to do; NULL shows the game's PREV / NEXT / BACK / SELECT legend
   (a tap or click on a word presses its button). A hint about the row under the cursor goes in
   p->hint, a line of its own above the footer, so the legend stays (on a phone it is the only
   SELECT and BACK); a page with hints sets p->hints, and its rows leave that line free.

   The mouse (menu_mouse_*, below; any of the game's popups gets the same): the row under the
   pointer is selected when the mouse moves, a left click is Cross (on a value picker: its arrows,
   else the next value; on an ACTION value: Cross), a right click is Triangle, the wheel moves the cursor or changes the value.
   PC SETTINGS (pc_settings.c), EXIT GAME (exit_menu.c) and the online pages (online_menu.c) are
   such pages. */
#ifndef MENU_PAGE_H
#define MENU_PAGE_H

#include <stdint.h>

/* The legend's button glyphs in the game's font (text id 0xA6 "\x8D PREV \x8F NEXT ...") */
#define GLYPH_LEFT "\x8D"
#define GLYPH_RIGHT "\x8F"
#define GLYPH_TRIANGLE "\xA7"
#define GLYPH_CROSS "\xAD"

enum { MENU_PAGE_ROWS = 24, MENU_PAGE_TEXT = 80 };
enum { MENU_ROW_BUTTON, MENU_ROW_VALUE, MENU_ROW_TEXT };
/* menu_page_value's arrows: FIXED can't be changed (shown without arrows), ARROWS left/right
   change it, ACTION is shown without arrows and Cross acts on it (a click is Cross, as on a row:
   the Controls page's bindings). */
enum { MENU_VALUE_FIXED, MENU_VALUE_ARROWS, MENU_VALUE_ACTION };

typedef struct MenuPage MenuPage;
struct MenuPage {
    uint32_t menu[0x600 / 4];  /* the SharedMenu; first, so the game's `this` is the page */
    uint32_t vtable[17 * 2];
    int constructed;
    const char *title;
    const char *footer;        /* NULL: the game's PREV / NEXT / BACK / SELECT legend */
    const char *hint;          /* NULL: none; else a line above the footer (set in update) */
    int hints;                 /* the page shows hints: its rows keep their line free */
    void (*build)(MenuPage *); /* add the rows (menu_page_row ...); also after menu_page_rebuild */
    void (*update)(MenuPage *, int cursor);  /* every frame, before the game's update */
    void (*accept)(MenuPage *, int row);     /* Cross */
    void (*back)(MenuPage *);                /* Triangle; NULL: close the page */
    void (*left)(MenuPage *, int row), (*right)(MenuPage *, int row);
    int rebuild;               /* set by menu_page_rebuild: rows again at the next update */
    int rebuild_row;           /* and the cursor's row then */
    int armed;                 /* the buttons were all released since the page opened */
    unsigned char kind[MENU_PAGE_ROWS];     /* MENU_ROW_* of each row */
    unsigned char arrows[MENU_PAGE_ROWS];   /* a value row's MENU_VALUE_* */
    char text[MENU_PAGE_ROWS][MENU_PAGE_TEXT];  /* the value rows' text */
    /* A page that lays itself out (the race room): it has no rows and draws in update with the
       menu_draw_* calls below. mouse: once a field when the mouse did something, the pointer in
       the view's coordinates; a right click still presses Triangle. */
    void (*mouse)(MenuPage *, int x, int y, int moved, int left, int wheel);
    void (*draw)(MenuPage *);  /* every frame after the game's update (which starts the popups'
                                  frame afresh): the menu_draw_* calls */
    int (*header_mouse)(MenuPage *, int x, int y);  /* consumes a click above the rows */
    int title_y;               /* above 0: the title's y, instead of above the rows */
} __attribute__((aligned(16)));  /* the game stores a u128 at +0x490 with aligned SSE moves */

void menu_page_run(MenuPage *p);       /* modally, until it closes */
void menu_page_close(MenuPage *p);     /* as the game closes a page on Cross */
void menu_page_cancel(MenuPage *p);    /* as it closes one on Triangle */
int menu_page_row(MenuPage *p, const char *text, int enabled);  /* text: kept, not copied */
int menu_page_value(MenuPage *p, const char *label, const char *value, int arrows);  /* copied */
int menu_page_text(MenuPage *p, const char *text);  /* kept, not copied */
/* A value row's label and value again (and whether it shows its arrows), in place. */
void menu_page_value_set(MenuPage *p, int row, const char *label, const char *value, int arrows);
void menu_page_rebuild(MenuPage *p);   /* the rows change: build again, cursor on the first */
void menu_page_rebuild_at(MenuPage *p, int row);  /* the same, the cursor on that row */
int menu_page_cursor(const MenuPage *p);
void menu_page_set_cursor(MenuPage *p, int row);
int menu_page_rows(const MenuPage *p);
void menu_page_suffix(MenuPage *p, int row, const char *text);  /* dim text after a row */
void menu_page_enable(MenuPage *p, int row, int enabled);  /* a row's flag, without building again */
void menu_page_sound(int next);        /* S_NEXT / S_PREVIOUS, as the value rows play them */

/* ---- drawing, for a page that lays itself out (in its draw) ----
   In the popup's view, x across 0..640 and y down 0..224 as the rows are (menu_draw_size),
   each call queued at once, in order: what is drawn later is on top, and text is on top of
   every box. The game's own pieces: the popups' translucent panel with its black top and bottom
   edges (func_00190E48), the cursor's bar (func_0018F9C0, one a frame), and its font in the
   rows' styles (the menu's UiStyles, below), sized and coloured. */
typedef struct { int size[4]; int rgba[4]; } MenuStyle;  /* a UiStyle: glyph size and spacing, colour */
enum { MENU_STYLE_ROW, MENU_STYLE_SELECTED, MENU_STYLE_OFF };
enum { MENU_LEFT, MENU_CENTRE, MENU_RIGHT };
#define GLYPH_SQUARE "\xA0"
#define GLYPH_CIRCLE "\xAB"
MenuStyle menu_page_style(const MenuPage *p, int which);
MenuStyle menu_style(MenuStyle s, int percent, int r, int g, int b);  /* sized; colour r < 0: kept */
void menu_draw_size(int *width, int *height);  /* the popup view's size */
void menu_draw_begin(void);            /* each frame, before the first box */
void menu_draw_box(int x, int y, int w, int h, int level);  /* level 90: the popups' own */
void menu_draw_rect(int x, int y, int w, int h, int r, int g, int b, int a);  /* a < 128: blended */
enum { MENU_PICTURE_W = 256, MENU_PICTURE_H = 160 };
/* A picture for menu_draw_picture: MENU_PICTURE_W x _H PSMCT16 pixels (5:5:5, bit 15 set) in the
   core's memory, its GS upload in front of it; NULL when out of memory. Kept, never freed. */
uint16_t *menu_picture_new(void);
void menu_draw_picture(int x, int y, int w, int h, const uint16_t *picture);  /* bilinear, one sprite */
void menu_draw_bar(int x, int y, int w, int h);  /* the cursor's bar */
void menu_draw_text(int align, int x, int y, const char *text, const MenuStyle *s);
int menu_text_width(const char *text, const MenuStyle *s);
void menu_play(const char *sound);     /* a front-end sound by name: "S_SELECT", "S_BACK"... */

#endif
