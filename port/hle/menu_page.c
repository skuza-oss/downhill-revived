/* Pages in the game's popup menu style (menu_page.h), built the way its menus are (build
   func_0015F0C8, update func_0015E750, vtable D_00284FB8; finding 050), laid out as its centred
   popups (the pause menu, func_0015E238):
   - a SharedMenu object (include/menu.h, 0x4FC bytes), constructed by the game (func_0018D9D0),
     here in host memory, with a copy of the base vtable D_00287218 whose build, update, accept,
     back and left/right slots call the page's functions;
   - rows added as strings (func_0018E408), which the game keeps by pointer, so a page rewrites
     a row's text in place as func_0015F4C8 rewrites the volumes;
   - run modally by the popup manager D_003F52D0 (func_00190108: its own frame loop until the
     page closes), which draws, moves the cursor with up/down and plays the sounds.
   Below them, the mouse for every popup the manager runs, the game's own as well (finding 092). */
#include "hle.h"
#include "menu_page.h"
#include "../include/fps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The game's popup menus (src/unsorted, finding 050) */
extern char D_003F52D0[];             /* the popup manager: +0x20 its view, +0x12D closing, +0x150 the menu */
extern uint32_t D_00287218[];         /* SharedMenu's base vtable */
extern char D_0029DE68[];             /* the text table the ids index */
extern char D_004F3570[];             /* the fonts: 0x20 bytes each (func_0020FB80, func_0020F860) */
extern int32_t D_0029E290;            /* the controls: 1 riding, 0 menus (func_00220550) */
void *func_0018D9D0(void *menu);      /* SharedMenu constructor */
void func_0018E408(void *menu, const char *text, int enabled);  /* add a row */
void func_0018DC20(void *menu);       /* base update: cursor, drawing */
void func_0018E468(void *menu);       /* close (back) */
void func_00190688(void *manager);    /* close (accept) */
void func_00190108(void *manager, void *menu);  /* run modally */
void func_001B5AF0(void *manager, int a, int b);
void func_0023D650(int view, int *width, int *height);
int func_0018D860(void *item);        /* an item's text: its string, else its id's */
void func_0020FB80(int font, int x, int y, int width, int height);  /* a font's glyph size and spacing */
int func_0020F860(int font, const char *text);  /* a line's width, 1/16 pixels */
void port_menu_sound(int next);       /* S_NEXT or S_PREVIOUS (patched into 001EB1D8.cpp) */
int func_00220230(int port);          /* the buttons held on a pad (0 when none is there) */
extern float port_fov_x;              /* widescreen.c: (4/3) / the picture's shape, 1 without */

/* A page opened by a press (the X that chose a row of the menu below, or the screen before)
   would take that same press as its own while the button is still down: a page acts on
   buttons only once they have all been released. */
static int buttons_held(void) { return (func_00220230(0) | func_00220230(1)) & 0xFFFF; }

enum { VTABLE_SLOTS = 17, ITEM_SIZE = 16 };
enum { X_CURSOR = 0x0, X_STYLE = 0xC, X_STYLE_ON = 0x10, X_ITEMS = 0x18, X_COUNT = 0x418, X_VISIBLE = 0x41C,
       X_START = 0x420, X_TITLE = 0x438, X_FOOTER = 0x448, X_ROW_Y = 0x460, X_ROW_X = 0x464, X_ROW_STEP = 0x468,
       X_46C = 0x46C, X_470 = 0x470, X_PREFIXED = 0x478, X_BACK_BUTTON = 0x4A8, X_LEFT_ALIGNED = 0x4BB,
       X_RIGHT_ALIGNED = 0x4BC, X_4C4 = 0x4C4, X_VTABLE = 0x4F8 };
enum { M_VIEW = 0x20, M_CLOSING = 0x12D, M_MENU = 0x150 };  /* the popup manager's */
enum { TEXT_PREV_NEXT = 0xA6 };       /* "<- PREV -> NEXT /\ BACK X SELECT" */

typedef struct {                      /* MenuItem (include/menu.h) */
    short x, y;
    short id;                         /* text id; 0: no text, unless string is set */
    char enabled, placed;             /* placed: x, y fixed, no automatic layout */
    const char *string;               /* drawn instead of the id's text when set */
    void *table;
} Item;

#define AT(menu, field, type) (*(type *)((char *)(menu) + (field)))
#define MENU(p, field, type) AT((p)->menu, field, type)

static void set_text(Item *item, short id, const char *string)
{
    item->id = id;
    item->string = string;
    item->table = D_0029DE68;
}

static void footer(MenuPage *p)
{
    if (p->footer) set_text(&MENU(p, X_FOOTER, Item), 1, p->footer);  /* any id: the string is drawn */
    else set_text(&MENU(p, X_FOOTER, Item), TEXT_PREV_NEXT, NULL);
}

/* The rows start at 0x54 under the title (0x40), 10 apart, and the footer sits at 0xCC. A long
   page (PC SETTINGS: 13 rows) would run its last rows into the footer, so the title and the rows
   move up together until the last row ends a gap above it.
   A page with hints (p->hints) draws them on a line of their own HINT_ABOVE the footer, so the
   legend under them stays; its last row ends a row above that line. Its title may go up to
   TITLE_MIN_HINTS, and a page that still doesn't fit (PC SETTINGS' 16 rows and Touch Controls)
   draws its rows closer together. */
enum { ROW_Y = 0x54, ROW_STEP = 10, TITLE_ABOVE = 0x14, FOOTER_Y = 0xCC, FOOTER_GAP = 16, TITLE_MIN = 0x10,
       HINT_ABOVE = 11, TITLE_MIN_HINTS = 0x0C };

static void rows(MenuPage *p)
{
    MENU(p, X_COUNT, int) = 0;
    p->build(p);
    int n = menu_page_rows(p), y = ROW_Y, step = ROW_STEP;
    int last = p->hints ? FOOTER_Y - HINT_ABOVE - ROW_STEP : FOOTER_Y - FOOTER_GAP;
    int top = (p->hints ? TITLE_MIN_HINTS : TITLE_MIN) + TITLE_ABOVE;
    if (n > 0 && y + (n - 1) * step > last) y = last - (n - 1) * step;
    if (y < top) {
        y = top;
        if (p->hints && n > 1 && y + (n - 1) * step > last) step = (last - y) / (n - 1);
    }
    MENU(p, X_ROW_STEP, int) = step;
    MENU(p, X_ROW_Y, int) = y;
    MENU(p, X_TITLE, Item).y = (short)(y - TITLE_ABOVE);
}

/* ---- the vtable's slots ---- */

static void slot_build(void *menu)  /* slot 1, as func_0015F0C8, centred as func_0015E238 */
{
    MenuPage *p = menu;
    int width = 0, height = 0;
    func_0023D650(*(int *)(D_003F52D0 + M_VIEW), &width, &height);
    int half = width >> 1;
    Item *title = &MENU(p, X_TITLE, Item), *foot = &MENU(p, X_FOOTER, Item);
    MENU(p, X_ROW_STEP, int) = ROW_STEP;
    MENU(p, X_ROW_X, int) = half;
    title->x = (short)half; title->placed = 1;  /* its y and the rows' (X_ROW_Y): rows() */
    foot->x = (short)half; foot->y = FOOTER_Y; foot->placed = 1;
    MENU(p, X_LEFT_ALIGNED, char) = 0;  /* centred rows, highlight and panel */
    MENU(p, X_46C, int) = 1;
    MENU(p, X_4C4, int) = 0;
    MENU(p, X_BACK_BUTTON, int) = 5;  /* Triangle: back (slot 8) */
    MENU(p, X_470, int) = 0x14;
    set_text(title, 1, p->title);
    footer(p);
    rows(p);
    func_001B5AF0(D_003F52D0, 1, 1);
}

static void slot_update(void *menu)  /* slot 5, as func_0015E750: the page, then the base */
{
    MenuPage *p = menu;
    if (!p->armed && !buttons_held()) p->armed = 1;
    if (p->rebuild) {
        p->rebuild = 0;
        rows(p);
        menu_page_set_cursor(p, p->rebuild_row);
    }
    if (p->update) p->update(p, menu_page_cursor(p));
    if (p->title_y > 0) MENU(p, X_TITLE, Item).y = (short)p->title_y;
    set_text(&MENU(p, X_TITLE, Item), 1, p->title);
    footer(p);
    func_0018DC20(menu);
    if (p->hint && *p->hint) {  /* its own line above the legend, in the legend's style */
        MenuStyle s = menu_page_style(p, MENU_STYLE_OFF);
        menu_draw_text(MENU_CENTRE, MENU(p, X_FOOTER, Item).x, FOOTER_Y - HINT_ABOVE, p->hint, &s);
    }
    if (p->draw) p->draw(p);
}

static void slot_accept(void *menu, int index)  /* slot 7, Cross */
{
    MenuPage *p = menu;
    if (!p->armed) return;
    if (p->accept) p->accept(p, index);
    else menu_page_close(p);
}

static void slot_back(void *menu)  /* slot 8, Triangle */
{
    MenuPage *p = menu;
    if (!p->armed) return;
    if (p->back) p->back(p);
    else menu_page_cancel(p);
}

static void slot_left(void *menu, int index)  /* slot 9 */
{
    MenuPage *p = menu;
    if (p->armed && p->left) p->left(p, index);
}

static void slot_right(void *menu, int index)  /* slot 10 */
{
    MenuPage *p = menu;
    if (p->armed && p->right) p->right(p, index);
}

/* ---- the page's calls ---- */

void menu_page_run(MenuPage *p)
{
    p->armed = 0;
    if (!p->constructed) {
        memcpy(p->vtable, D_00287218, sizeof p->vtable);  /* {delta, function} pairs, slot 1 on */
        p->vtable[1 * 2 + 1] = (uint32_t)(uintptr_t)slot_build;
        p->vtable[5 * 2 + 1] = (uint32_t)(uintptr_t)slot_update;
        p->vtable[7 * 2 + 1] = (uint32_t)(uintptr_t)slot_accept;
        p->vtable[8 * 2 + 1] = (uint32_t)(uintptr_t)slot_back;
        p->vtable[9 * 2 + 1] = (uint32_t)(uintptr_t)slot_left;
        p->vtable[10 * 2 + 1] = (uint32_t)(uintptr_t)slot_right;
        func_0018D9D0(p->menu);
        MENU(p, X_VTABLE, uint32_t *) = p->vtable;
        p->constructed = 1;
    } else
        menu_page_rebuild(p);  /* the rows as they are now, should the game keep the old ones */
    func_00190108(D_003F52D0, p->menu);
}

void menu_page_close(MenuPage *p) { (void)p; func_00190688(D_003F52D0); }
void menu_page_cancel(MenuPage *p) { func_0018E468(p->menu); }
void menu_page_rebuild(MenuPage *p) { menu_page_rebuild_at(p, 0); }
void menu_page_rebuild_at(MenuPage *p, int row) { p->rebuild = 1, p->rebuild_row = row; }
int menu_page_cursor(const MenuPage *p) { return MENU(p, X_CURSOR, int); }
int menu_page_rows(const MenuPage *p) { return MENU(p, X_COUNT, int); }
void menu_page_sound(int next) { port_menu_sound(next); }

static int add(MenuPage *p, const char *text, int enabled, int kind)
{
    int row = menu_page_rows(p);
    if (row >= MENU_PAGE_ROWS) return -1;
    p->kind[row] = (unsigned char)kind;
    func_0018E408(p->menu, text, enabled);
    return row;
}

int menu_page_row(MenuPage *p, const char *text, int enabled) { return add(p, text, enabled, MENU_ROW_BUTTON); }
int menu_page_text(MenuPage *p, const char *text) { return add(p, text, 0, MENU_ROW_TEXT); }

/* "Label:  < Value >"; without the arrows when it can't be changed. A value ending in " *" (PC
   SETTINGS: it waits for the next start) keeps its star outside the arrows. */
static void value_text(char *out, const char *label, const char *value, int arrows)
{
    char shown[MENU_PAGE_TEXT];
    size_t n = strlen(value);
    int star = n >= 2 && !strcmp(value + n - 2, " *");
    snprintf(shown, sizeof shown, arrows == MENU_VALUE_ARROWS ? "< %.*s >%s" : "%.*s%s", (int)(star ? n - 2 : n), value, star ? " *" : "");
    if (label && *label) snprintf(out, MENU_PAGE_TEXT, "%s  %s", label, shown);
    else snprintf(out, MENU_PAGE_TEXT, "%s", shown);
}

int menu_page_value(MenuPage *p, const char *label, const char *value, int arrows)
{
    int row = menu_page_rows(p);
    if (row >= MENU_PAGE_ROWS) return -1;
    value_text(p->text[row], label, value, arrows);
    p->arrows[row] = (unsigned char)arrows;
    return add(p, p->text[row], 1, MENU_ROW_VALUE);
}

void menu_page_value_set(MenuPage *p, int row, const char *label, const char *value, int arrows)
{
    if (row < 0 || row >= menu_page_rows(p) || row >= MENU_PAGE_ROWS) return;
    value_text(p->text[row], label, value, arrows);
    p->arrows[row] = (unsigned char)arrows;
}

void menu_page_suffix(MenuPage *p, int row, const char *text)
{
    if (row < 0 || row >= menu_page_rows(p)) return;
    MenuStyle normal = menu_page_style(p, MENU_STYLE_ROW);
    MenuStyle dim = menu_page_style(p, MENU_STYLE_OFF);
    int x = MENU(p, X_ROW_X, int) + menu_text_width(p->text[row], &normal) / 2 + 24;  /* a clear gap after the row */
    int y = MENU(p, X_ROW_Y, int) + row * MENU(p, X_ROW_STEP, int);
    menu_draw_text(MENU_LEFT, x, y, text, &dim);
}

void menu_page_enable(MenuPage *p, int row, int enabled)
{
    if (row >= 0 && row < menu_page_rows(p)) ((Item *)((char *)p->menu + X_ITEMS))[row].enabled = (char)(enabled != 0);
}

/* On the row, else on the first enabled one after it (the game skips disabled rows too). */
void menu_page_set_cursor(MenuPage *p, int row)
{
    int n = menu_page_rows(p);
    if (n <= 0) { MENU(p, X_CURSOR, int) = 0; return; }
    if (row < 0) row = 0;
    if (row >= n) row = n - 1;
    for (int k = 0; k < n; k++) {
        int i = (row + k) % n;
        if (((Item *)((char *)p->menu + X_ITEMS))[i].enabled) { MENU(p, X_CURSOR, int) = i; return; }
    }
    MENU(p, X_CURSOR, int) = row;
}

/* ---- drawing, for a page that lays itself out (menu_page.h) ---- */

typedef unsigned int u128 __attribute__((mode(TI)));
void func_00190E48(void *popups, void *packet, int x, int y, int w, int h);  /* the panel's packet */
void func_00190DB0(void *popups, int level);   /* its shade: 90 the popups' own */
void func_002103D8(void *lists, int view, void *packet);  /* onto the view's display list, by reference */
void func_002105A0(void *lists, void *qword, int r, int g, int b, int a);  /* RGBAQ */
void func_00210578(void *lists, void *qword, int x, int y);  /* XYZ2 (widescreen.c maps x) */
void func_0018F9C0(int unused, int x, int y, int w, int h);  /* the cursor's bar */
void func_0023D618(int view, int *x, int *y);
void func_0020F9A8(int font, int view, int x, int y, const char *text);
void func_0020F9F0(int font, int view, int x, int y, const char *text);
void func_0020FA98(int font, int view, int x, int y, const char *text);
void func_0020F518(int font, int r, int g, int b, int a);
void func_001C57A8(void *bank, u128 name);     /* a sound of the bank by name */
extern char D_00513B00[];                      /* the views' display lists */
extern char D_004B2A90[];                      /* the front end's sound bank */

/* The display lists keep a reference to each packet until the frame is drawn, so every box and
   rectangle of a frame has its own; the game draws a frame's lists before the next update. The
   DMA reads them from EE RAM, which this unit's statics are not in: they come from the game's
   heap (func_001FD430, its operator new) the first time, and stay for the run.
   A packet's DMA tag counts its own qword (as func_00190E48 writes it), so the DMA sends one
   qword past the end: each packet is zeroed first, and that spare goes to the GIF as an empty
   tag (in the game's own buffers it is zero too). A box is 14 qwords, a rectangle 6. */
enum { DRAW_BOXES = 32, BOX_QWORDS = 16, DRAW_RECTS = 96, RECT_QWORDS = 8 };
void *func_001FD430(unsigned int size);
static uint64_t (*box_packets)[BOX_QWORDS * 2], (*rect_packets)[RECT_QWORDS * 2];
static int boxes_used, rects_used;

static int draw_view(void) { return *(int *)(D_003F52D0 + M_VIEW); }

MenuStyle menu_page_style(const MenuPage *p, int which)
{
    static const int fields[] = {X_STYLE, X_STYLE_ON, X_STYLE + 8};
    MenuStyle s;
    const int *style = MENU(p, fields[which], const int *);
    if (style) memcpy(&s, style, sizeof s);
    else memset(&s, 0, sizeof s);
    return s;
}

MenuStyle menu_style(MenuStyle s, int percent, int r, int g, int b)
{
    for (int i = 0; i < 4; i++) s.size[i] = s.size[i] * percent / 100;
    if (r >= 0) s.rgba[0] = r, s.rgba[1] = g, s.rgba[2] = b;
    return s;
}

void menu_draw_size(int *width, int *height) { func_0023D650(draw_view(), width, height); }

void menu_draw_begin(void)
{
    if (!box_packets) {
        size_t boxes = sizeof *box_packets * DRAW_BOXES, rects = sizeof *rect_packets * DRAW_RECTS;
        uintptr_t at = ((uintptr_t)func_001FD430((unsigned)(boxes + rects + 15)) + 15) & ~(uintptr_t)15;
        box_packets = (void *)at, rect_packets = (void *)(at + boxes);
    }
    boxes_used = rects_used = 0;
}

void menu_draw_box(int x, int y, int w, int h, int level)
{
    if (!box_packets || boxes_used >= DRAW_BOXES) return;
    void *packet = box_packets[boxes_used++];
    memset(packet, 0, sizeof *box_packets);
    func_00190E48(D_003F52D0, packet, x, y, w, h);
    func_00190DB0(D_003F52D0, level);
    func_002103D8(D_00513B00, draw_view(), packet);
}

/* As func_0018F9C0 draws the cursor's bar: one sprite, its colour at both corners. */
void menu_draw_rect(int x, int y, int w, int h, int r, int g, int b, int a)
{
    if (!rect_packets || rects_used >= DRAW_RECTS || w <= 0 || h <= 0) return;
    uint64_t *start = rect_packets[rects_used++], *q = start + 2;
    memset(start, 0, sizeof *rect_packets);
    int view = draw_view(), cx, cy, vw, vh;
    func_0023D618(view, &cx, &cy);
    func_0023D650(view, &vw, &vh);
    int left = cx - (vw >> 1) + x, top = cy - (vh >> 1) + y;
    /* GIF tag: one loop of RGBAQ, XYZ2, RGBAQ, XYZ2; PRIM a sprite, alpha-blended (0x46) when a < 128 */
    q[0] = a < 128 ? 0x4023400000008001ull : 0x4003400000008001ull, q[1] = 0x5151, q += 2;
    func_002105A0(D_00513B00, q, r, g, b, a), q += 2;
    func_00210578(D_00513B00, q, left, top), q += 2;
    func_002105A0(D_00513B00, q, r, g, b, a), q += 2;
    func_00210578(D_00513B00, q, left + w, top + h), q += 2;
    start[0] = (uint64_t)(((q - start) / 2) | 0x60000000), start[1] = 0;
    func_002103D8(D_00513B00, view, start);
}

/* The track picker's picture (finding 132): one bilinear sprite of a 256 x 160 PSMCT16 texture
   at GS blocks 0x3C00-0x3E00 (TBW 4; its 256-line footprint is 16 pages). A census of every TEX0,
   CLUT, FRAME, ZBUF and upload address the GS saw from boot through the main and online menus,
   the shop, the rider screen, the room and the picker found 0x38E0-0x3F80 never touched; the
   shell's texture bins can reach 0x3B9F (their end, word 0xEE7F0) and small render targets start
   at 0x3F80, so this keeps 3 pages below it and 12 above. The pixels go to the GS from the core's
   own memory (PATH3, host_service_graphics_feed) once per new picture, never through the game's
   heap; the draw takes three of the popups' rectangle packets. 16-bit because a 32-bit 256 x 256
   footprint (32 pages) would reach 0x3F80. */
enum { PIC_TBP = 0x3C00, PIC_HEADER = 6, PIC_BYTES = MENU_PICTURE_W * MENU_PICTURE_H * 2 };
static const uint16_t *uploaded;       /* the picture now in VRAM; NULL: upload before drawing */
static unsigned picture_idle;

uint16_t *menu_picture_new(void)
{
    unsigned char *raw = malloc(PIC_HEADER * 16 + PIC_BYTES + 15);
    if (!raw) return NULL;
    uint64_t *h = (uint64_t *)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
    h[0] = 4 | 1ull << 60, h[1] = 0xE;                                   /* A+D, 4 registers */
    h[2] = (uint64_t)PIC_TBP << 32 | 4ull << 48 | 2ull << 56, h[3] = 0x50;  /* BITBLTBUF: PSMCT16, DBW 4 */
    h[4] = 0, h[5] = 0x51;                                               /* TRXPOS 0, 0 */
    h[6] = MENU_PICTURE_W | (uint64_t)MENU_PICTURE_H << 32, h[7] = 0x52;  /* TRXREG */
    h[8] = 0, h[9] = 0x53;                                               /* TRXDIR: host to local */
    h[10] = (PIC_BYTES / 16) | 0x8000 | 2ull << 58, h[11] = 0;           /* IMAGE, EOP */
    return (uint16_t *)(h + 12);
}

/* Once a field: a picture not drawn in the last field is uploaded again next time (the picker
   reopened): its GS blocks are free only while the shell runs (finding 132's census), so a race or
   anything else in between may have used them. */
static void menu_preview_field(void)
{
    if (uploaded && ++picture_idle > 1) uploaded = NULL;
}

void menu_draw_picture(int x, int y, int w, int h, const uint16_t *picture)
{
    if (!picture || !rect_packets || rects_used + 3 > DRAW_RECTS || w <= 0 || h <= 0) return;
    picture_idle = 0;
    if (picture != uploaded) {
        static int logged;
        if (!logged++) hle_log("menu: track picture: %d bytes to GS blocks %X-%X from the core's memory, per new picture\n",
                               PIC_BYTES, PIC_TBP, PIC_TBP + 512);
        host_service_graphics_feed(2, GADDR((const uint64_t *)picture - PIC_HEADER * 2), (PIC_HEADER * 16 + PIC_BYTES) / 4);
        uploaded = picture;
    }
    int view = draw_view(), cx, cy, vw, vh;
    func_0023D618(view, &cx, &cy);
    func_0023D650(view, &vw, &vh);
    int left = cx - (vw >> 1) + x, top = cy - (vh >> 1) + y;
    uint64_t *a = rect_packets[rects_used++], *b = rect_packets[rects_used++], *c = rect_packets[rects_used++], *q;
    memset(a, 0, sizeof *rect_packets);
    memset(b, 0, sizeof *rect_packets);
    memset(c, 0, sizeof *rect_packets);
    q = a + 2;  /* TEXFLUSH; TEX0: the picture, DECAL; TEX1: bilinear (the text packets set their own) */
    q[0] = 3 | 1ull << 60, q[1] = 0xE, q += 2;
    q[0] = 0, q[1] = 0x3F, q += 2;
    q[0] = PIC_TBP | 4u << 14 | 2u << 20 | 8ull << 26 | 8ull << 30 | 1ull << 35, q[1] = 0x06, q += 2;
    q[0] = 0x60, q[1] = 0x14, q += 2;
    a[0] = (uint64_t)(((q - a) / 2) | 0x60000000), a[1] = 0;
    func_002103D8(D_00513B00, view, a);
    /* Two triangles (a strip), not a sprite: the native renderer draws upscaled sprites (flag 1024)
       sampling each GS pixel once, as at 1x, so a sprite's picture stays blocky at 2x-8x; a
       triangle's UV runs across every rendered pixel, so bilinear works at any scale. Same
       rectangle and texels at 1x. UV half a texel inside so bilinear stays in the 160 lines. */
    const uint64_t u0 = 8, v0 = 8, u1 = MENU_PICTURE_W * 16 - 8, v1 = MENU_PICTURE_H * 16 - 8;
    q = b + 2;  /* top left, top right */
    q[0] = 1 | 1ull << 46 | 0x114ull << 47 | 5ull << 60, q[1] = 0x53531, q += 2;
    func_002105A0(D_00513B00, q, 128, 128, 128, 128), q += 2;
    q[0] = u0 | v0 << 32, q += 2;
    func_00210578(D_00513B00, q, left, top), q += 2;
    q[0] = u1 | v0 << 32, q += 2;
    func_00210578(D_00513B00, q, left + w, top), q += 2;
    b[0] = (uint64_t)(((q - b) / 2) | 0x60000000), b[1] = 0;
    func_002103D8(D_00513B00, view, b);
    q = c + 2;  /* bottom left, bottom right: the strip goes on */
    q[0] = 2 | 0x8000 | 2ull << 60, q[1] = 0x53, q += 2;
    q[0] = u0 | v1 << 32, q += 2;
    func_00210578(D_00513B00, q, left, top + h), q += 2;
    q[0] = u1 | v1 << 32, q += 2;
    func_00210578(D_00513B00, q, left + w, top + h), q += 2;
    c[0] = (uint64_t)(((q - c) / 2) | 0x60000000), c[1] = 0;
    func_002103D8(D_00513B00, view, c);
}

void menu_draw_bar(int x, int y, int w, int h) { func_0018F9C0(0, x, y, w, h); }

void menu_draw_text(int align, int x, int y, const char *text, const MenuStyle *s)
{
    func_0020FB80(0, s->size[0], s->size[1], s->size[2], s->size[3]);
    func_0020F518(0, s->rgba[0], s->rgba[1], s->rgba[2], s->rgba[3]);
    int view = draw_view();
    if (align == MENU_LEFT) func_0020F9A8(0, view, x, y, text);
    else if (align == MENU_RIGHT) func_0020FA98(0, view, x, y, text);
    else func_0020F9F0(0, view, x, y, text);
}

/* Retail's width routine knows the capitals only (its menus write in capitals); lowercase draws
   as small capitals, so measuring the capitals gives an upper bound for mixed-case names. */
int menu_text_width(const char *text, const MenuStyle *s)
{
    char saved[0x20], upper[160];
    size_t n = 0;
    for (; text[n] && n < sizeof upper - 1; n++) upper[n] = text[n] >= 'a' && text[n] <= 'z' ? (char)(text[n] - 32) : text[n];
    upper[n] = 0;
    memcpy(saved, D_004F3570, sizeof saved);
    func_0020FB80(0, s->size[0], s->size[1], s->size[2], s->size[3]);
    *(int *)(D_004F3570 + 0x1C) = 0;
    int width = func_0020F860(0, upper) >> 4;
    memcpy(D_004F3570, saved, sizeof saved);
    return width;
}

void menu_play(const char *sound)
{
    unsigned char name[16] = {0};
    size_t n = strlen(sound);
    memcpy(name, sound, n < 16 ? n : 16);
    u128 v;
    memcpy(&v, name, sizeof v);  /* the bank's names are their bytes, little-endian */
    func_001C57A8(D_004B2A90, v);
}

/* ---- the mouse (finding 092) ----
   Once a field (pad_vsync) the host's mouse (host_service_mouse_state) is read and, while one of
   the game's popups runs (the popup manager's menu, ours or the game's own: the pause menu, the
   options pages), turned into what a pad would do there:
   - moving over an enabled row puts the cursor on it (the menu's cursor word, as up/down would);
   - a left click on a row puts the cursor there and presses Cross; on one of our value rows its
     left arrow presses Left, the rest of it Right;
   - a right click presses Triangle (back), anywhere;
   - the wheel presses Up/Down, or Left/Right on one of our value rows.
   Presses go to player 1's pad like a controller's (menu_mouse_merge), so the menus play their own
   sounds and do their own checks. Nothing happens while a race is ridden, without a mouse, or
   when the mouse isn't used: the game's state is then exactly as without this code. */

enum { PRESS_FIELDS = 3, QUEUE = 8 };
static HostMouse seen_mouse;
static int mouse_started;
static uint16_t queue[QUEUE];
static int queued, press_field;  /* press_field: fields into the first press (held, then let go) */

static void press(uint16_t button)
{
    if (queued < QUEUE) queue[queued++] = button;
}

/* The buttons the mouse holds on player 1's pad this field. */
static uint16_t pressed_now(void) { return queued && press_field < PRESS_FIELDS ? queue[0] : 0; }

void menu_mouse_merge(int player, HostPad *out)
{
    static const uint16_t order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                       PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
    uint16_t b = player == 0 ? pressed_now() : 0;
    if (!b) return;
    out->buttons |= b;
    for (int i = 0; i < 12; i++)
        if (b & order[i]) out->pressure[i] = 255;
}

/* The pointer in the view's coordinates (those the menus draw at): x across 0..width, y down
   0..height. Widescreen shows the 640-pixel picture wider and keeps menus as a 4:3 picture in its
   middle (widescreen.c port_ws_point): the inverse of that. 0 when it is off the picture. */
static int pointer(int view, int *x, int *y)
{
    int w = 0, h = 0;
    func_0023D650(view, &w, &h);
    if (w <= 0 || h <= 0 || seen_mouse.x < 0 || seen_mouse.y < 0 || seen_mouse.x > 65536 || seen_mouse.y > 65536)
        return 0;
    float gx = seen_mouse.x * (float)w / 65536.0f, gy = seen_mouse.y * (float)h / 65536.0f;
    if (port_fov_x > 0 && port_fov_x != 1.0f) gx = w * 0.5f + (gx - w * 0.5f) / port_fov_x;
    *x = (int)gx, *y = (int)gy;
    return 1;
}

/* A line's width in pixels as the menu draws it in its row style (the font's size is the
   style's; the font's state is put back after). */
static int measure(void *menu, int style_field, const char *text, int part)
{
    char saved[0x20], line[MENU_PAGE_TEXT * 2];
    const int *style = AT(menu, style_field, const int *);  /* UiStyle: x, y, width, height, ... */
    snprintf(line, sizeof line, "%.*s", part, text);
    memcpy(saved, D_004F3570, sizeof saved);
    if (style) func_0020FB80(0, style[0], style[1], style[2], style[3]);
    *(int *)(D_004F3570 + 0x1C) = AT(menu, X_LEFT_ALIGNED, char) ? 0 : AT(menu, X_RIGHT_ALIGNED, char) ? 2 : 1;
    int width = func_0020F860(0, line) >> 4;
    memcpy(D_004F3570, saved, sizeof saved);
    return width;
}

/* Is this popup one of our pages: its vtable is the page's own copy. */
static MenuPage *page_of(void *menu)
{
    MenuPage *p = menu;
    return AT(menu, X_VTABLE, uint32_t *) == p->vtable ? p : NULL;
}

/* The row of the open popup under the pointer, -1 none; *left_arrow: on a value row's "<". */
static int row_at(void *menu, int view, int *left_arrow)
{
    int mx, my;
    *left_arrow = 0;
    if (!pointer(view, &mx, &my)) return -1;
    int count = AT(menu, X_COUNT, int), visible = AT(menu, X_VISIBLE, int), start = AT(menu, X_START, int);
    int x = AT(menu, X_ROW_X, int), y = AT(menu, X_ROW_Y, int), step = AT(menu, X_ROW_STEP, int);
    if (count <= 0 || count > 64 || step <= 0) return -1;
    if (x < 0) {  /* not laid out yet: the view's middle */
        int w = 0, h = 0;
        func_0023D650(view, &w, &h);
        x = w / 2;
    }
    if (visible <= 0) visible = count;
    int end = start + visible < count ? start + visible : count;
    int widest = 0, row = -1;
    for (int i = start; i < end; i++) {
        const char *text = (const char *)(uintptr_t)(uint32_t)func_0018D860((char *)menu + X_ITEMS + i * ITEM_SIZE);
        int width = text ? measure(menu, i == AT(menu, X_CURSOR, int) ? X_STYLE_ON : X_STYLE, text, MENU_PAGE_TEXT) : 0;
        if (width > widest) widest = width;
        int top = y + (i - start) * step - 1;
        if (my >= top && my < top + step) row = i;
    }
    if (row < 0) return -1;
    /* across: the panel the menu draws behind its rows (func_0018F458), with its margin */
    int left = AT(menu, X_LEFT_ALIGNED, char) ? x - 6 : AT(menu, X_RIGHT_ALIGNED, char) ? x - widest - 6 : x - widest / 2 - 6;
    if (mx < left || mx >= left + widest + 12) return -1;
    MenuPage *p = page_of(menu);
    if (p && row < MENU_PAGE_ROWS && p->kind[row] == MENU_ROW_VALUE) {
        const char *text = p->text[row], *arrow = strchr(text, '<');
        if (arrow) {
            int width = measure(menu, X_STYLE_ON, text, MENU_PAGE_TEXT);
            int row_left = AT(menu, X_LEFT_ALIGNED, char) ? x : x - width / 2;
            int at = row_left + measure(menu, X_STYLE_ON, text, (int)(arrow - text));
            *left_arrow = mx >= at - 8 && mx < at + 10;  /* the "<" and a little round it */
        }
    }
    return row;
}

static int row_enabled(void *menu, int row)
{
    return ((Item *)((char *)menu + X_ITEMS))[row].enabled != 0;
}

/* The button of our page's legend ("<- PREV -> NEXT /\ BACK X SELECT", the footer when the page
   has no text of its own) under the pointer, 0 none: each word with the button symbol before it,
   measured in the footer's style as drawn (centred at its x). On a phone that is the only way to
   say SELECT, as nothing else on the screen is a button. PREV and NEXT follow their arrows, left
   and right: a value row's previous and next value (a row without a value ignores them). */
static uint16_t page_legend_button(void *menu, int view)
{
    static const struct { const char *word; uint16_t button; } words[] = {
        {"PREV", PAD_LEFT}, {"NEXT", PAD_RIGHT}, {"BACK", PAD_TRIANGLE}, {"SELECT", PAD_CROSS}};
    Item *foot = &AT(menu, X_FOOTER, Item);
    int mx, my;
    if (foot->id != TEXT_PREV_NEXT || foot->string || !pointer(view, &mx, &my)) return 0;
    if (my < foot->y - 6 || my > foot->y + 16) return 0;
    const char *text = (const char *)(uintptr_t)(uint32_t)func_0018D860(foot);
    if (!text) return 0;
    int length = (int)strnlen(text, MENU_PAGE_TEXT), width = measure(menu, X_STYLE + 8, text, length);
    int start = foot->x - width / 2;
    if (mx < start - 8 || mx > start + width + 8) return 0;
    uint16_t best = 0;
    int best_end = 1 << 30;
    for (unsigned i = 0; i < sizeof words / sizeof *words; i++) {  /* the first word ending right of the pointer */
        const char *at = strstr(text, words[i].word);
        if (!at) continue;
        int end = start + measure(menu, X_STYLE + 8, text, (int)(at - text) + (int)strlen(words[i].word));
        if (mx <= end + 4 && end < best_end) best = words[i].button, best_end = end;
    }
    return best;
}

/* The open popup and the mouse's news this field: hover, clicks, wheel. */
static void popup_mouse(void *menu, int view, int moved, int left, int right, int wheel)
{
    MenuPage *p = page_of(menu);
    if (p && p->header_mouse && left) {
        int mx, my;
        if (pointer(view, &mx, &my) && p->header_mouse(p, mx, my)) return;
    }
    if (p && p->mouse) {  /* a page that lays itself out hit-tests its own pieces */
        int mx, my;
        if (pointer(view, &mx, &my) && (moved || left || wheel)) p->mouse(p, mx, my, moved, left, wheel);
        if (right) press(PAD_TRIANGLE);
        return;
    }
    int left_arrow, row = row_at(menu, view, &left_arrow);
    int value_row = p && row >= 0 && row < MENU_PAGE_ROWS && p->kind[row] == MENU_ROW_VALUE && p->arrows[row] != MENU_VALUE_ACTION;
    if (moved && row >= 0 && row_enabled(menu, row) && AT(menu, X_CURSOR, int) != row)
        AT(menu, X_CURSOR, int) = row;
    if (left && row >= 0 && row_enabled(menu, row)) {
        AT(menu, X_CURSOR, int) = row;
        press(value_row ? (left_arrow ? PAD_LEFT : PAD_RIGHT) : PAD_CROSS);
    } else if (left && row < 0 && p) {  /* our pages' legend */
        uint16_t b = page_legend_button(menu, view);
        if (b) press(b);
    }
    if (right) press(PAD_TRIANGLE);
    int cursor = AT(menu, X_CURSOR, int);
    int on_value = p && cursor >= 0 && cursor < MENU_PAGE_ROWS && p->kind[cursor] == MENU_ROW_VALUE && p->arrows[cursor] != MENU_VALUE_ACTION;
    for (; wheel > 0; wheel--) press(on_value ? PAD_RIGHT : PAD_UP);
    for (; wheel < 0; wheel++) press(on_value ? PAD_LEFT : PAD_DOWN);
}

/* Two of the game's own popups draw more than a list (finding 092, UI2):
   - the shop (its root page, vtable D_00284190) keeps its list (BIKES / JERSEYS / BIKE COLOR) at
     +0x500 and refreshes its pictures when that list's cursor moves by a press (func_0013C128), so
     the mouse moves it by Up/Down presses, never by its cursor word: hover drives the cursor to the
     row; a click on the row the cursor is on presses Left left of its text, Right right of it, Cross
     on it; the wheel there is Left/Right (the previous / next bike, jersey or colour);
   - the track chooser (D_004C7A30, vtable D_00288650) draws a map: a click on its left part is
     Left (previous track), on its right part Right, in the middle Cross; the wheel is Left/Right.
   Both have a legend at the bottom whose buttons a click presses. */
static int shell_trace = -1;           /* DHD_MOUSE_TRACE */
enum { SHOP_ROOT = 0x08284190u, SHOP_LIST = 0x500, TRACK_CHOOSER = 0x08288650u };
static void *list_target_menu;          /* the shop's list while the mouse drives its cursor */
static int list_target = -1, list_steps;

typedef struct { short left, right; uint16_t button; } LegendButton;
static uint16_t legend_at(const LegendButton *b, int n, int mx)
{
    for (int i = 0; i < n; i++)
        if (mx >= b[i].left && mx < b[i].right) return b[i].button;
    return 0;
}

static int game_popup_mouse(void *menu, int view, int moved, int left, int right, int wheel)
{
    uint32_t vtable = AT(menu, X_VTABLE, uint32_t);
    if (vtable != SHOP_ROOT && vtable != TRACK_CHOOSER) return 0;
    int w = 0, h = 0, mx = 0, my = 0;
    func_0023D650(view, &w, &h);
    int on = w > 0 && h > 0 && pointer(view, &mx, &my);
    int x640 = on ? mx * 640 / w : 0, bottom = on && my >= h * 90 / 100;
    if (right) press(PAD_TRIANGLE), list_target = -1;
    if (vtable == TRACK_CHOOSER) {
        static const LegendButton legend[] = {{27, 115, PAD_LEFT}, {115, 215, PAD_RIGHT}, {355, 480, PAD_CROSS}, {490, 600, PAD_TRIANGLE}};
        if (on && left) {
            uint16_t b = bottom ? legend_at(legend, 4, x640) : mx < w * 35 / 100 ? PAD_LEFT : mx >= w * 65 / 100 ? PAD_RIGHT : PAD_CROSS;
            if (b) press(b);
            if (shell_trace > 0) hle_log("shell mouse: track chooser click at %d,%d: %04x\n", mx, my, b);
        }
        for (; wheel > 0; wheel--) press(PAD_LEFT);
        for (; wheel < 0; wheel++) press(PAD_RIGHT);
        return 1;
    }
    uint32_t at = AT(menu, SHOP_LIST, uint32_t);
    void *list = at >= 0x08000000u && at < 0x0A000000u ? (void *)(uintptr_t)at : NULL;
    if (!list) return 1;
    static const LegendButton legend[] = {{20, 88, PAD_TRIANGLE}, {88, 180, PAD_CROSS}, {180, 265, PAD_START}};
    if (on && left && bottom) {
        uint16_t b = legend_at(legend, 3, x640);
        if (b) press(b), list_target = -1;
        if (shell_trace > 0) hle_log("shell mouse: shop legend click at %d,%d: %04x\n", mx, my, b);
        return 1;
    }
    int arrow, row = on ? row_at(list, view, &arrow) : -1, cursor = AT(list, X_CURSOR, int);
    if (shell_trace > 0 && (left || wheel)) hle_log("shell mouse: shop at %d,%d row %d cursor %d%s\n", mx, my, row, cursor, left ? " click" : "");
    if (row >= 0 && row_enabled(list, row) && (moved || left)) {
        if (row == cursor && left) {
            const char *text = (const char *)(uintptr_t)(uint32_t)func_0018D860((char *)list + X_ITEMS + row * ITEM_SIZE);
            int width = text ? measure(list, X_STYLE_ON, text, MENU_PAGE_TEXT) : 0, x = AT(list, X_ROW_X, int);
            press(mx < x - width / 2 ? PAD_LEFT : mx >= x + width / 2 ? PAD_RIGHT : PAD_CROSS);
            list_target = -1;
        } else if (row != cursor) {
            if (list_target != row) list_steps = 0;
            list_target = row, list_target_menu = list;
        }
    }
    if (wheel) list_target = -1;
    for (; wheel > 0; wheel--) press(cursor >= 0 ? PAD_LEFT : PAD_UP);
    for (; wheel < 0; wheel++) press(cursor >= 0 ? PAD_RIGHT : PAD_DOWN);
    return 1;
}

/* The shop's list toward the hovered row: one press at a time, the next once the cursor moved. */
static void list_drive(void *menu)
{
    if (list_target < 0) return;
    if (AT(menu, X_VTABLE, uint32_t) != SHOP_ROOT || (void *)(uintptr_t)AT(menu, SHOP_LIST, uint32_t) != list_target_menu) {
        list_target = -1;
        return;
    }
    if (queued) return;
    int cursor = AT(list_target_menu, X_CURSOR, int);
    if (cursor == list_target || ++list_steps > 16) list_target = -1;
    else press(list_target > cursor ? PAD_DOWN : PAD_UP);
}

/* ---- the front end's own menus (finding 092, UI2) ----
   The shell (func_001EB4D0: one function per screen id D_0029DED8, finding 050) draws its lists
   itself: a static menu struct per screen (first row +8, last +0xC, cursor +0x10, disabled flags
   +0x1C) and rows through three text helpers at y = first y + index * step. Each shell frame is
   marked (port_shell_frame) and the helpers report what they draw (port_shell_text): both only
   write host memory. The mouse acts there by pad presses only, as a controller would:
   - lists: moving over a row presses Up/Down until the cursor is on it (one press at a time,
     each once the last is done); a left click does the same, then Cross; the wheel is Up/Down;
   - the rider carousel: a click on the left part is Left (previous), on the right edge Right
     (next), in the middle (the rider) Cross; the wheel is Left/Right; a click on the legend at the
     bottom is the button it shows;
   - the title, the flight to the rider screen and the ride to the shop: a click is Cross;
   - a right click is Triangle on every screen. */

enum { SHELL_TEXTS = 48, SHELL_TEXT = 40 };
typedef struct { short align, style, x, y; char text[SHELL_TEXT]; } ShellText;
static ShellText shell_texts[2][SHELL_TEXTS];
static int shell_count[2], shell_fill;  /* shell_fill: the frame being drawn; the other is the last one */
static unsigned fields, shell_seen;     /* fields: menu_mouse_field's count; shell_seen: at the last shell frame */
static int shell_ran;                   /* a shell frame has run: the game's views exist (issue #11) */

void port_shell_frame(void)
{
    port_loop_frame(0);   /* not in a race (fps.h, finding 159) */
    shell_fill ^= 1;
    shell_count[shell_fill] = 0;
    shell_seen = fields;
    shell_ran = 1;
}

/* The front end runs: a shell frame in the last 8 fields. Before the first one there are no views
   yet (func_0023D650 would read view 0's missing object): a mouse moved over the window as the
   game started crashed it there. */
static int shell_running(void) { return shell_ran && fields - shell_seen < 8; }

void port_shell_text(int align, int style, int x, int y, const void *text)
{
    int n = shell_count[shell_fill];
    if (n >= SHELL_TEXTS || !text) return;
    ShellText *t = &shell_texts[shell_fill][n];
    t->align = (short)align, t->style = (short)style, t->x = (short)x, t->y = (short)y;
    snprintf(t->text, sizeof t->text, "%s", (const char *)text);  /* some rows are the screen's stack buffer */
    shell_count[shell_fill] = n + 1;
}

typedef struct {                      /* the shell's menu struct (func_001F7420) */
    int32_t start, frames, first, last, cursor, back, next;
    uint8_t disabled[16];
} ShellMenu;
typedef struct {
    short screen;
    uint32_t menu;                    /* its address */
    short y, y_other, step;           /* row i at y + i * step (y_other: another layout of the screen) */
    char packed;                      /* disabled rows are not drawn and the rest close up (func_001EF0E0) */
} ShellList;
static const ShellList shell_lists[] = {
    {15, 0x4C6CA8, 95, 0, 15, 0},     /* main menu (func_001ED5B8) */
    {2, 0x4C6D98, 90, 0, 10, 0},      /* OPTIONS (func_001F0160) */
    {13, 0x4C6BE8, 25, 0, 9, 0},      /* controls (func_001F0A28) */
    {19, 0x4C6DC8, 95, 0, 15, 0},     /* difficulty (func_001F1168) */
    {10, 0x4C6D68, 95, 0, 15, 0},     /* number of players (func_001EED78) */
    {24, 0x4C6D08, 85, 0, 15, 0},     /* multiplayer game mode (func_001EDD28) */
    {16, 0x4C6D38, 85, 95, 15, 0},    /* new / continue (func_001F3B98) */
    {1, 0x4C6C78, 65, 0, 15, 0},      /* single-player modes (func_001ED900) */
    {11, 0x4C6DF8, 85, 0, 15, 1},     /* race type (func_001EF0E0) */
    {12, 0x4C6B88, 85, 0, 15, 0},     /* func_001EFA78 */
    {17, 0x4C6B88, 85, 0, 15, 0},     /* func_001EFE28 */
    {21, 0x4C6CD8, 95, 0, 15, 0},     /* func_001EF5D0 */
    {22, 0x4C6C18, 65, 0, 15, 0},     /* continue career (func_001EE2A0) */
    {25, 0x4C6C48, 95, 0, 15, 0},     /* multiplayer game (func_001EE8A8) */
    {30, 0x4C6E58, 95, 0, 15, 0},     /* func_001F4508 */
};
enum { SHELL_TITLE = 0, SHELL_FLIGHT = 32, SHELL_SELECTED = 33, SHELL_TO_SHOP = 34 };
static int carousel(int screen) { return screen >= 3 && screen <= 6; }  /* rider select (func_001F24B8) */
extern int32_t D_0029DED8;            /* the shell's screen */
extern char D_004C6560[];             /* the shell's text styles: 0x58 bytes each, +0x48 glyph sizes */

static ShellMenu *shell_menu(const ShellList *l) { return (ShellMenu *)(uintptr_t)(0x08000000u + l->menu); }

/* What the last shell frame drew at y: across [*left, *right); 0 when nothing was. */
static int shell_drawn(int y, int *left, int *right, int *style)
{
    const ShellText *t = shell_texts[shell_fill ^ 1];
    int n = shell_count[shell_fill ^ 1], found = 0;
    for (int i = 0; i < n; i++, t++) {
        if (t->y != y) continue;
        char saved[0x20];
        const int32_t *s = (const int32_t *)(D_004C6560 + t->style * 0x58 + 0x48);
        memcpy(saved, D_004F3570, sizeof saved);
        func_0020FB80(0, s[0], s[1], (int)((const float *)s)[2], (int)((const float *)s)[3]);
        int width = func_0020F860(0, t->text) >> 4;
        memcpy(D_004F3570, saved, sizeof saved);
        int l = t->align == 0 ? t->x : t->align == 1 ? t->x - width / 2 : t->x - width;
        if (!found || l < *left) *left = l;
        if (!found || l + width > *right) *right = l + width;
        *style = t->style;
        found = 1;
    }
    return found;
}

/* The row of a shell list under the pointer, -1 none. */
static int shell_row_at(const ShellList *l, int mx, int my)
{
    const ShellMenu *m = shell_menu(l);
    if (m->first < 0 || m->last < m->first || m->last > 15) return -1;
    for (int pass = 0; pass < 2; pass++) {
        int y0 = pass ? l->y_other : l->y;
        if (!y0) continue;
        int drawn = 0, place = 0;
        for (int i = m->first; i <= m->last; i++) {
            if (l->packed && m->disabled[i]) continue;
            int y = y0 + (l->packed ? place++ : i) * l->step, left = 0, right = 0, style = 0;
            if (!shell_drawn(y, &left, &right, &style)) continue;
            drawn = 1;
            /* the glyphs sit below y: their middle is about half their height (the style's
               +0x4C, 1/16 units) further down */
            int middle = y + (*(const int32_t *)(D_004C6560 + style * 0x58 + 0x4C) >> 4) * 9 / 16;
            int top = middle - l->step / 2;
            if (my >= top && my < top + l->step && mx >= left - 6 && mx < right + 6)
                return m->disabled[i] ? -1 : i;
        }
        if (drawn) break;  /* this layout is the one drawn */
    }
    return -1;
}

static int shell_target = -1, shell_click, shell_screen = -1, shell_steps;

static const ShellList *shell_list(int screen)
{
    for (unsigned i = 0; i < sizeof shell_lists / sizeof *shell_lists; i++)
        if (shell_lists[i].screen == screen) return &shell_lists[i];
    return NULL;
}

/* The rider screen's legend (func_001BAF10's bar, "< PREV > NEXT  [] INFO  X SELECT  /\ BACK"):
   its buttons across the 640-wide view. */
static uint16_t legend_button(int mx)
{
    static const struct { short left, right; uint16_t button; } legend[] = {
        {20, 118, PAD_LEFT}, {118, 220, PAD_RIGHT}, {240, 340, PAD_SQUARE}, {350, 480, PAD_CROSS}, {490, 600, PAD_TRIANGLE}};
    for (unsigned i = 0; i < sizeof legend / sizeof *legend; i++)
        if (mx >= legend[i].left && mx < legend[i].right) return legend[i].button;
    return 0;
}

static void shell_mouse(int news, int moved, int left, int right, int wheel)
{
    int screen = D_0029DED8;
    if (screen != shell_screen) shell_target = -1, shell_screen = screen;
    const ShellList *list = shell_list(screen);
    int w = 0, h = 0, mx = 0, my = 0;
    func_0023D650(0, &w, &h);
    int on = news && pointer(0, &mx, &my);
    if (on && list) {
        int row = shell_row_at(list, mx, my);
        if ((moved || left) && row >= 0) {
            if (shell_target != row || left) shell_steps = 0;
            shell_target = row;
            shell_click |= left;
        }
        if (shell_trace > 0) hle_log("shell mouse: screen %d at %d,%d row %d cursor %d%s\n", screen, mx, my, row,
                                     shell_menu(list)->cursor, left ? " click" : "");
    } else if (on && left && carousel(screen) && w > 0 && h > 0) {
        uint16_t b = my >= h * 86 / 100 ? legend_button(mx * 640 / w)
                   : mx < w * 35 / 100 ? PAD_LEFT : mx >= w * 85 / 100 ? PAD_RIGHT : PAD_CROSS;
        if (b) press(b);
        if (shell_trace > 0) hle_log("shell mouse: rider screen click at %d,%d: %04x\n", mx, my, b);
    } else if (on && left && (screen == SHELL_TITLE || screen == SHELL_FLIGHT || screen == SHELL_SELECTED || screen == SHELL_TO_SHOP))
        press(PAD_CROSS);
    if (right) press(PAD_TRIANGLE), shell_target = -1;
    if (wheel) shell_target = -1;
    int sideways = carousel(screen);
    for (; wheel > 0; wheel--) press(sideways ? PAD_LEFT : PAD_UP);
    for (; wheel < 0; wheel++) press(sideways ? PAD_RIGHT : PAD_DOWN);
    /* toward the row: one press at a time, the next once the cursor has moved */
    if (list && shell_target >= 0 && !queued) {
        const ShellMenu *m = shell_menu(list);
        if (m->cursor == shell_target) {
            if (shell_click) press(PAD_CROSS);
            shell_target = -1, shell_click = 0;
        } else if (++shell_steps > 2 * 16)  /* it doesn't get there (a row the menu skips): give up */
            shell_target = -1, shell_click = 0;
        else
            press(shell_target > m->cursor ? PAD_DOWN : PAD_UP);
    }
}

/* DHD_MOUSE_TRACE: each new shell screen (with what its last frame drew) and each popup page
   the popup manager shows (its address, vtable and row layout), for working out a screen. */
static void trace_rows(void *m)
{
    for (int i = 0; i < AT(m, X_COUNT, int) && i < 16; i++) {
        const Item *it = (const Item *)((char *)m + X_ITEMS) + i;
        const char *text = (const char *)(uintptr_t)(uint32_t)func_0018D860((char *)m + X_ITEMS + i * ITEM_SIZE);
        hle_log("  row %d at %d,%d%s%s: %s\n", i, it->x, it->y, it->enabled ? "" : " (off)", it->placed ? " placed" : "",
                text ? text : "");
    }
}

static void trace_screens(void)
{
    static int last_screen = -1;
    static uint32_t last_popup = 1;
    static int last_rows = -1, last_cursor = -1;
    uint32_t popup = D_003F52D0[M_CLOSING] ? 0 : (uint32_t)(uintptr_t)*(void **)(D_003F52D0 + M_MENU);
    void *open = (void *)(uintptr_t)popup;
    int rows = popup ? AT(open, X_COUNT, int) : -1, cursor = popup ? AT(open, X_CURSOR, int) : -1;
    if (popup && AT(open, X_VTABLE, uint32_t) == 0x08284190u && AT(open, 0x500, uint32_t))  /* the shop's list's cursor too */
        cursor = cursor * 64 + AT((void *)(uintptr_t)AT(open, 0x500, uint32_t), X_CURSOR, int);
    if (popup != last_popup || rows != last_rows || cursor != last_cursor) {
        last_popup = popup, last_rows = rows, last_cursor = cursor;
        if (popup) {
            void *m = (void *)(uintptr_t)popup;
            hle_log("mouse trace: popup %08x vtable %08x rows %d cursor %d at %d,%d step %d align %d/%d\n", popup,
                    (uint32_t)(uintptr_t)AT(m, X_VTABLE, void *), AT(m, X_COUNT, int), AT(m, X_CURSOR, int),
                    AT(m, X_ROW_X, int), AT(m, X_ROW_Y, int), AT(m, X_ROW_STEP, int), AT(m, X_LEFT_ALIGNED, char),
                    AT(m, X_RIGHT_ALIGNED, char));
            trace_rows(m);
            uint32_t child = AT(m, 0x500, uint32_t);  /* the shop's pages keep a list there */
            if (child >= 0x08000000u && child < 0x0A000000u && AT(m, X_VTABLE, uint32_t) == 0x08284190u) {
                void *c = (void *)(uintptr_t)child;
                hle_log("  list %08x vtable %08x rows %d cursor %d at %d,%d step %d\n", child, AT(c, X_VTABLE, uint32_t),
                        AT(c, X_COUNT, int), AT(c, X_CURSOR, int), AT(c, X_ROW_X, int), AT(c, X_ROW_Y, int), AT(c, X_ROW_STEP, int));
                trace_rows(c);
            }
        } else
            hle_log("mouse trace: popup closed\n");
    }
    if (shell_running() && D_0029DED8 != last_screen && shell_count[shell_fill ^ 1]) {
        last_screen = D_0029DED8;
        hle_log("mouse trace: screen %d\n", last_screen);
        const ShellText *t = shell_texts[shell_fill ^ 1];
        for (int i = 0; i < shell_count[shell_fill ^ 1]; i++, t++)
            hle_log("  text %d style %d at %d,%d: %s\n", t->align, t->style, t->x, t->y, t->text);
    }
}

void menu_mouse_field(void)
{
    fields++;
    menu_preview_field();  /* the track picture: uploaded again after a pause */
    if (shell_trace < 0) shell_trace = getenv("DHD_MOUSE_TRACE") != NULL;
    if (queued && ++press_field >= 2 * PRESS_FIELDS) {  /* this press is done: the next one */
        memmove(queue, queue + 1, (size_t)(queued - 1) * sizeof *queue);
        queued--;
        press_field = 0;
    }
    HostMouse now;
    memset(&now, 0, sizeof now);
    host_service_mouse_state(GADDR(&now));
    int moved = 0, left = 0, right = 0, wheel = 0;
    if (now.seen && !mouse_started) {  /* the first reading: only what comes after it counts */
        seen_mouse = now;
        mouse_started = 1;
    } else if (now.seen) {
        moved = now.moves != seen_mouse.moves;
        left = now.presses[0] != seen_mouse.presses[0], right = now.presses[1] != seen_mouse.presses[1];
        wheel = now.wheel - seen_mouse.wheel;
        seen_mouse = now;
    }
    if (shell_trace > 0) trace_screens();
    int news = moved || left || right || wheel;
    if (!news && shell_target < 0 && list_target < 0) return;  /* nothing from the mouse: nothing at all */
    if (D_0029E290) { queued = 0, shell_target = list_target = -1; return; }  /* riding: the mouse does nothing */
    void *menu = *(void **)(D_003F52D0 + M_MENU);
    if (menu && !D_003F52D0[M_CLOSING]) {  /* a popup runs (the shell waits for it); the manager keeps
                                              its last page after a run, closing set */
        shell_target = -1;
        int view = *(int *)(D_003F52D0 + M_VIEW);
        if (news && !game_popup_mouse(menu, view, moved, left, right, wheel)) popup_mouse(menu, view, moved, left, right, wheel);
        list_drive(menu);
        return;
    }
    list_target = -1;
    if (shell_running()) shell_mouse(news, moved, left, right, wheel);
    else shell_target = -1;
}
