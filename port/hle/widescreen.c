/* Widescreen (finding 043 for the field of view, finding 056 for the HUD and any aspect).

   The 3D view is Hor+: every X field of view times s = (4/3) / aspect, every Y factor (relative to
   X) divided by s, so the vertical view stays and the sides widen. The map of what to change is
   the PCSX2 widescreen patch for this disc, SCUS-97177_5AE01D98.pnach by Aced14 in
   github.com/PCSX2/pcsx2_patches (finding 043); the values are worked out here for any aspect,
   no pnach is read. Constants in code: port/patches.py (port_fov_x is s).

   The picture is still 640 (or 512) pixels wide, shown wider. What the game draws in 2D would be
   stretched by 1/s, so (port/patches_widescreen.py) the 2D draw code maps its X through here:
   - menus, popups and every other 2D: a 4:3 picture in the middle (x about the view's centre,
     times s); a rectangle reaching a side of the view (fades, backgrounds) keeps its sides;
   - the race HUD: each element keeps its shape and is anchored to the side it sits on (left,
     middle or right third of the view): its distance to that side is kept, in the height's units.
     The HUD's 3D pieces (the speedometer, the track bar, the place) are scene objects placed in
     front of the camera (func_00176930); they get the same move, in camera space. Its text is
     drawn by the HUD's own code (0x1751C8-0x17C2A0), which calls the port_hud_text_* wrappers.

   DHD_WIDESCREEN (dhd.ini widescreen): 0 off; 1 the display's shape (the monitor's, or the
   window size's when windowed), unless DHD_ASPECT names a ratio, which is then used; or W:H.
   This changes EE memory and what the game culls, so the game-state gates run without it. */
#include "hle.h"
#include "widescreen_flat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

float port_fov_x = 1.0f;  /* s: 1, or (4/3) / aspect */

static int widescreen;
static double aspect = 4.0 / 3.0;
/* Card mode (finding 140): the renderer shows the full-screen 2D pictures (loading cards, the
   memory card warning) at 4:3, so the game's 2D in them is drawn as at 4:3, unmapped. The host says
   at each vsync (host_frame_video) whether the next field is drawn for a card. */
static int card_2d;
static int card_setup;  /* the loading card is being drawn (port_card_setup) */

void widescreen_card_mode(int on) { card_2d = on; }

/* The loading card's setup (func_00182D78, port/patches_widescreen.py): it draws the card and its
   text ("Single Event", the track; "Bike Shop") for eight frames, and the loading thread then sends
   the same packets again until the level is loaded. The card is shown 4:3, so its 2D is drawn as
   at 4:3, unmapped; card mode, which follows the picture, would come too late for it. */
void port_card_setup(int on) { card_setup = on; }

/* The 2D draw code's X is mapped: widescreen, and not a card. */
static int map_2d(void) { return widescreen && !card_2d && !card_setup; }

/* The game's views (func_0023D650, func_0023D8D8): an entry per view, its View first the focal
   length in pixels (x0, the X field of view, already times s) and the size in pixels. */
typedef struct { float x0; unsigned short width, height; } WsView;
typedef struct { void *camera; WsView *view; int mode; char pad[8]; } WsEntry;
extern WsEntry D_0075A9D0[32];
void func_0023D618(int view, int *x, int *y);   /* the view's centre in GS pixels */
void func_0023D650(int view, int *width, int *height);
void func_0020F9F0(int font, int view, int x, int y, const char *text);  /* centred text */
void func_0020F9A8(int font, int view, int x, int y, const char *text);  /* left-aligned text */

/* Pixels, not the 96-dpi units Windows gives a program that doesn't say it knows them: the
   monitor's real size here, and the window and its back buffer at the monitor's own resolution
   (a borderless window on a scaled display was drawn smaller and enlarged by Windows). */
void host_display_aware(void)
{
    static int done;
    if (done++) return;
    host_dpi_aware();
}

/* "W:H" or "WxH" as W / H; 0 if it isn't one. */
static double ratio(const char *v)
{
    double w, h;
    char sep;
    if (!v || sscanf(v, "%lf%c%lf", &w, &sep, &h) != 3 || (sep != ':' && sep != 'x') || w <= 0 || h <= 0) return 0;
    return w / h;
}

/* The shape the game is shown in: what DHD_WIDESCREEN names, else DHD_ASPECT's ratio, else the
   window size's (windowed) or the monitor's. 0: widescreen off. */
static double wanted_aspect(void)
{
    const char *on = getenv("DHD_WIDESCREEN");
    double named = ratio(on);
    if (named) return named;
    if (!on || !atoi(on)) return 0;
    if ((named = ratio(getenv("DHD_ASPECT")))) return named;
    const char *mode = getenv("DHD_WINDOW");
    const char *w = getenv("DHD_WINDOW_WIDTH"), *h = getenv("DHD_WINDOW_HEIGHT");
    /* "monitor" (dhd.ini's default window size): the monitor's size, which present.c reads later */
    int monitor_sized = (w && !strcmp(w, "monitor")) || (h && !strcmp(h, "monitor"));
    if ((!mode || !strcmp(mode, "windowed")) && !monitor_sized) {
        int width = w && atoi(w) > 0 ? atoi(w) : 896, height = h && atoi(h) > 0 ? atoi(h) : 672;
        return (double)width / height;
    }
    return host_monitor_aspect();  /* the primary monitor (present.c puts the borderless and fullscreen window there) */
}

static uint32_t float_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bits_float(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

void host_widescreen_init(void)
{
    /* PS2 address, retail word, 1 for an X (times s) or 0 for a Y factor (divided by s). The view
       table D_0027ECC0 (0xC0 bytes a view: X at +0, Y at +0x20) for 1P, 2P both ways, 4P and
       paused; the menu's Y; three tree sprite widths. */
    static const uint32_t words[][3] = {
        {0x27ECC0, 0x438A8000, 1}, {0x27ECE0, 0x3F15551D, 0},  /* paused X, 1P Y */
        {0x27ED80, 0x438A8000, 1}, {0x27EDA0, 0x3F155550, 0},  /* 2P horizontal */
        {0x27EE40, 0x438A8000, 1}, {0x27EE60, 0x3F155550, 0},
        {0x27EF00, 0x435E0000, 1}, {0x27EF20, 0x3F15551D, 0},  /* 2P vertical */
        {0x27EFC0, 0x435E0000, 1}, {0x27EFE0, 0x3F15551D, 0},
        {0x27F0A0, 0x3EF0A3D7, 0},                             /* menu Y */
        {0x27F140, 0x43520000, 1}, {0x27F160, 0x3EF0A3D7, 0},  /* 4P */
        {0x27F200, 0x43520000, 1}, {0x27F220, 0x3EF0A3D7, 0},
        {0x27F2C0, 0x43520000, 1}, {0x27F2E0, 0x3EF0A3D7, 0},
        {0x27F380, 0x43520000, 1}, {0x27F3A0, 0x3EF0A3D7, 0},
        {0x26C5C0, 0x438A8000, 1}, {0x26C700, 0x438A8000, 1},  /* trees */
        {0x26C758, 0x438A8000, 1},
    };
    host_display_aware();
    double want = wanted_aspect();
    if (want < 4.0 / 3.0 + 0.01) return;  /* off, or a 4:3 (or narrower) display */
    if (want > 4.0) want = 4.0;
    widescreen = 1;
    aspect = want;
    port_fov_x = (float)(4.0 / 3.0 / aspect);
    /* The picture's shape follows (DHD_ASPECT "wide", the default with widescreen on). */
    char text[32];
    snprintf(text, sizeof text, "%d:1000", (int)(aspect * 1000 + 0.5));
    host_putenv_s("DHD_WIDESCREEN_ASPECT", text);
    const char *shape = getenv("DHD_ASPECT");
    if (!shape || !*shape) host_putenv_s("DHD_ASPECT", "wide");
    hle_env_changed();
    unsigned changed = 0;
    for (unsigned i = 0; i < sizeof words / sizeof *words; i++) {
        uint32_t *at = (uint32_t *)(uintptr_t)(PS2_RAM_BASE + words[i][0]);
        float v = bits_float(words[i][1]);
        if (*at == words[i][1]) { *at = float_bits(words[i][2] ? v * port_fov_x : v / port_fov_x); changed++; }
        else hle_log("widescreen: %06X holds %08X, not %08X; left\n", words[i][0], *at, words[i][1]);
    }
    hle_log("widescreen: aspect %.4f (s %.4f), %u of %u data words\n", aspect, port_fov_x, changed,
            (unsigned)(sizeof words / sizeof *words));
}

/* The bike shop builds its HUD and shadow at run time: when the shop's block is set up (the
   halfword at 0xA095D0 reads 2), widen the HUD bars and the shadow, as the patch's conditional
   lines do (for 16:9: 1.33, 1.33 and -111). Writing 0xA095D0 itself ends the condition. The game
   writes the bars back each field, as on PCSX2, whose patch is applied every field too.
   The shadow word is no width: 0xA60030 is a VIF UNPACK V3-32 of the dark panel behind the rider,
   and 0xA60038 its first vertex's second coordinate, the panel's far corner: -95.886 in retail
   (C2BFC5EA, logged in round 40), -111 in the 16:9 patch, which moves the corner past the wider
   view's left side. Up to 16:9 it takes the line through those two points in 1/s; wider shapes
   keep -111, which leaves the corner past the left side up to 5.9:1 (more than the 4:1 cap). Further
   along the line the vertex crosses the camera's plane (between -111 and -116) and the panel is
   drawn mirrored across the picture, over the rider, from about 2:1 on (finding 154). */
void host_widescreen_vsync(void)
{
    if (!widescreen || *(volatile uint16_t *)(uintptr_t)(PS2_RAM_BASE + 0xA095D0) != 2) return;
    volatile uint32_t *top = (volatile uint32_t *)(uintptr_t)(PS2_RAM_BASE + 0xA095D0);
    volatile uint32_t *bottom = (volatile uint32_t *)(uintptr_t)(PS2_RAM_BASE + 0xA09610);
    volatile uint32_t *shadow = (volatile uint32_t *)(uintptr_t)(PS2_RAM_BASE + 0xA60038);
    float wider = fminf(1.0f / port_fov_x, 4.0f / 3.0f);  /* how much wider than 4:3 the corner follows */
    *top = float_bits(1.0f / port_fov_x);     /* top HUD width */
    *bottom = float_bits(1.0f / port_fov_x);  /* bottom HUD width */
    *shadow = float_bits(-95.886f - (111.0f - 95.886f) * 3.0f * (wider - 1.0f));  /* the panel's corner */
}

/* ---- the 2D draw code's X (port/patches_widescreen.py) ---- */

static int view_size(int view, int *cx, int *w)
{
    int cy, h;
    if (view < 0 || view >= 32 || !D_0075A9D0[view].view) return 0;
    func_0023D618(view, cx, &cy);
    func_0023D650(view, w, &h);
    return *w > 0;
}

static int scaled(double v) { return (int)floor(v + 0.5); }

/* A point of something in the middle 4:3 (menus, popups, fades): x in GS pixels, about view 0's
   centre; a point at or past a side of the view (a full-width rectangle's corner) stays. */
int port_ws_point(int x)
{
    int cx, w;
    if (!map_2d() || !view_size(0, &cx, &w) || abs(x - cx) * 2 >= w) return x;
    return cx + scaled((x - cx) * port_fov_x);
}

/* A rectangle from x, w wide (GS pixels), as port_ws_point maps its sides. */
void port_ws_span(int *x, int *w)
{
    int left = port_ws_point(*x), right = port_ws_point(*x + *w);
    *x = left;
    *w = right - left;
}

/* A width (or height-free size) of a 2D thing, so it keeps its shape. */
int port_ws_size(int w) { return map_2d() ? scaled(w * port_fov_x) : w; }

/* The side a HUD element at x (GS pixels) belongs to, in GS pixels: the left or right side of
   the view, or its centre. */
static int anchor(int view, int x)
{
    int cx, w;
    if (!view_size(view, &cx, &w)) return x;
    int third = w / 6;  /* a third of the width either side of the centre: the middle third */
    return x < cx - third ? cx - w / 2 : x > cx + third ? cx + w / 2 : cx;
}

/* A point of a HUD element whose reference point is at ref (GS pixels): kept as far from its
   side as it was (anchor). */
int port_ws_hud_point(int view, int ref, int x)
{
    if (!map_2d()) return x;
    int a = anchor(view, ref);
    return a + scaled((x - a) * port_fov_x);
}

/* ---- the touch controls' HUD (finding 141, touch_field.c) ----
   The HUD's own calls are the hook: each says the race HUD is drawn (touch_hud_seen), and while
   the touch controls are on (touch_hud_place) the speedometer and the energy arrows above it, the
   cluster in the bottom-right corner where the right thumb works, move to the top corner: the
   left one for the right hand (the arrows beside the speedometer), the right one for the left
   hand, where the left side's pieces (the track bar, the place) also move across. Places are
   fractions of the 4:3 view (u across from the left, v down from the top); widescreen then
   anchors each piece to its new side as before. DHD_TOUCH_HUD_SHIFT="su,sv,au,av" overrides the
   right hand's shifts of the speedometer and the arrows (check 7 tunes them). */
static void touch_hud_move(float u, float v, float *du, float *dv)
{
    static HleEnv env;
    static const char *read;
    static float shift[4] = {-0.75f, -0.75f, -0.65f, -0.67f};
    const char *text = hle_env(&env, "DHD_TOUCH_HUD_SHIFT");
    if (text != read) {
        read = text;
        float s[4];
        if (text && sscanf(text, "%f,%f,%f,%f", &s[0], &s[1], &s[2], &s[3]) == 4) memcpy(shift, s, sizeof shift);
    }
    *du = *dv = 0;
    if (u > 2.0f / 3 && v > 0.62f) {   /* the bottom-right cluster: the arrows above 0.755, the speedometer below */
        int arrows = v < 0.755f;
        *dv = arrows ? shift[3] : shift[1];
        if (touch_hud_place == 1) *du = arrows ? shift[2] : shift[0];
        else *du = arrows ? -0.21f : 0;   /* left hand: stays right, the arrows to its left */
    } else if (touch_hud_place == 2 && u < 1.0f / 3) *du = 1 - 2 * u;   /* left hand: the left side mirrors */
}

/* The HUD's 3D pieces: local is the offset from the camera (x right, y forward, z up) that
   func_00176930 places a piece at. A piece in a side third moves out with that side: by the
   width the view gained at its depth. */
void port_ws_hud_offset(int view, float *local)
{
    touch_hud_seen = 1;
    if (view < 0 || view >= 32 || !D_0075A9D0[view].view) return;
    const WsView *v = D_0075A9D0[view].view;
    float depth = local[1], focal = v->x0, half = v->width * 0.5f;
    if (depth <= 0 || focal <= 0 || half <= 0) return;
    if (touch_hud_place) {   /* the piece's place in the 4:3 view; its height is three quarters of its width */
        float span = depth * half / (focal / port_fov_x), du, dv;
        touch_hud_move((local[0] / span + 1) * 0.5f, (1 - local[2] / (span * 0.75f)) * 0.5f, &du, &dv);
        local[0] += du * 2 * span;
        local[2] -= dv * 2 * span * 0.75f;
    }
    if (!widescreen) return;
    /* where it is across the 4:3 view, -1 left side .. 1 right side */
    float u = local[0] * (focal / port_fov_x) / (depth * half);
    if (fabsf(u) <= 1.0f / 3) return;
    float gained = (1.0f - port_fov_x) * depth * half / focal;  /* half the width gained there */
    local[0] += u > 0 ? gained : -gained;
}

/* ---- text (func_0020FBB0, func_0020FDB0) ---- */

static int hud_text;       /* the HUD's code is drawing (port_hud_text_*) */
static int text_open;      /* a string is being drawn: text_anchor holds its side */
static int text_anchor;    /* 1/16 pixels from the view's left side */

/* A string starts: ref is its line's x (left, centre or right, by its alignment), 1/16 pixels
   from the view's left side. Menus and popups keep it in the middle 4:3; the HUD's to its side. */
void port_ws_text_begin(int view, int ref)
{
    int cx, w;
    text_open = 0;
    if (!map_2d() || !view_size(view, &cx, &w)) return;
    int w16 = w * 16;
    text_anchor = w16 / 2;
    if (hud_text) text_anchor = ref < w16 / 3 ? 0 : ref > w16 * 2 / 3 ? w16 : w16 / 2;
    text_open = 1;
}

void port_ws_text_end(void) { text_open = 0; }

/* A glyph's left side (pen, 1/16 pixels from the view's left side) in a view w pixels wide. */
int port_ws_glyph_x(int w, int pen)
{
    if (!map_2d()) return pen;
    int a = text_open ? text_anchor : w * 8;
    return a + scaled((pen - a) * port_fov_x);
}

/* A glyph's width, 1/16 pixels. */
int port_ws_glyph_w(int width) { return port_ws_size(width); }

/* A HUD string's point (x, y in the view's pixels) as the touch controls' HUD has it. */
static void touch_hud_text(int view, int *x, int *y)
{
    touch_hud_seen = 1;
    int w, h;
    if (!touch_hud_place || view < 0 || view >= 32 || !D_0075A9D0[view].view) return;
    func_0023D650(view, &w, &h);
    if (w <= 0 || h <= 0) return;
    float du, dv;
    touch_hud_move((float)*x / w, (float)*y / h, &du, &dv);
    *x += scaled(du * w);
    *y += scaled(dv * h);
}

/* The HUD's text calls (port/patches_widescreen.py renames them in its units). */
void port_hud_text_c(int font, int view, int x, int y, const char *text)
{
    touch_hud_text(view, &x, &y);
    hud_text = 1;
    func_0020F9F0(font, view, x, y, text);
    hud_text = 0;
}

void port_hud_text_l(int font, int view, int x, int y, const char *text)
{
    touch_hud_text(view, &x, &y);
    hud_text = 1;
    func_0020F9A8(font, view, x, y, text);
    hud_text = 0;
}

/* ---- the picture (the native renderer, port/gs/native_core.inc) ---- */

/* The widescreen picture's shape (width / height), 0 when off. */
double host_widescreen_aspect(void) { return widescreen ? aspect : 0; }

/* Maximum PATH1 count of a full-screen loading picture in this menu state (widescreen_flat.h). */
int host_widescreen_flat_limit(void) { return widescreen_flat_limit(); }
