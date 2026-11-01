/* The on-screen touch controls for Android (finding 141). Android had no touch input at all: a
   gamepad or a keyboard was needed (finding 091). This file turns touches into player 1's pad,
   through the same path as a controller (host_pad_device, host.c): a left stick for steering and
   leaning, and buttons for the riding controls.

   It is compiled for Android only (build_host64.py's ANDROID_UNITS), so no desktop build carries
   it; the two call sites (sdl_host.c's finger events, host.c's touch_merge) are behind
   __ANDROID__ too. It is on by default (DHD_TOUCH=0 turns it off, dhd.ini's "touch" setting).

   Layout: a region per control, fractions of the game's picture (gs_picture_rect_get), so the
   window's size, the scale and the bars beside the picture don't matter. The stock layout is a
   stick at the bottom left and the riding buttons at the right; DHD_TOUCH_LAYOUT names a preset
   ("default", "left-handed") or gives exact rectangles, "id=x:y:w:h,..." (id is a pad control's id
   from controls.c, or "stick"). A finger's position in the stick region is the left stick's tilt;
   the buttons set their control's pad button.

   The buttons are not drawn yet: a touch is invisible but the regions are known and shown in the
   CONTROLS page's preview. Drawing the overlay during play needs the Rust renderer (or a hooked HUD
   draw), which is next work (finding 141, Open). */
#include "hle.h"
#include "controls.h"
#include "../gs/present.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TOUCH_FINGERS = 12, TOUCH_REGIONS = 24 };

typedef struct {
    const char *id;      /* "stick", or a pad control's id (controls.c) */
    float x, y, w, h;    /* the rectangle, fractions of the picture */
} TouchRegion;

/* The stock layout: a left stick and the riding buttons (menu navigation is the mouse, finding 092,
   so no d-pad is needed). */
static const TouchRegion stock[] = {
    {"stick", 0.02f, 0.50f, 0.34f, 0.48f},
    {"south", 0.80f, 0.68f, 0.19f, 0.30f},   /* accelerate */
    {"north", 0.80f, 0.48f, 0.19f, 0.18f},   /* brake */
    {"east",  0.61f, 0.72f, 0.17f, 0.22f},   /* kick */
    {"west",  0.61f, 0.48f, 0.17f, 0.22f},   /* punch */
    {"l1",    0.61f, 0.28f, 0.17f, 0.18f},
    {"r1",    0.80f, 0.28f, 0.19f, 0.18f},
    {"l2",    0.42f, 0.28f, 0.17f, 0.18f},
    {"r2",    0.42f, 0.48f, 0.17f, 0.18f},
    {"back",  0.10f, 0.06f, 0.16f, 0.14f},   /* camera / bail (SELECT) */
    {"start", 0.28f, 0.06f, 0.16f, 0.14f},   /* pause (START) */
};
enum { STOCK = sizeof stock / sizeof *stock };

static TouchRegion regions[TOUCH_REGIONS];
static int region_count;
static int enabled = -1;    /* DHD_TOUCH: 1 unless it is set to 0 */
static int laid_out;

static struct { SDL_FingerID id; float x, y; int active; } fingers[TOUCH_FINGERS];

/* DHD_TOUCH=0 (dhd.ini "touch") turns the on-screen controls off; off is also the desktop rule, but
   this file isn't compiled there. */
static int touch_on(void)
{
    if (enabled < 0) {
        const char *setting = getenv("DHD_TOUCH");
        enabled = !(setting && !atoi(setting));
    }
    return enabled;
}

/* The preset's regions, mirrored for a left-handed player. */
static void layout_build(int mirror)
{
    region_count = 0;
    for (int i = 0; i < STOCK && region_count < TOUCH_REGIONS; i++) {
        TouchRegion r = stock[i];
        if (mirror) r.x = 1.0f - r.x - r.w;
        regions[region_count++] = r;
    }
}

/* DHD_TOUCH_LAYOUT's exact rectangles: "id=x:y:w:h,id=..." (a pad control's id, or "stick"). */
static void layout_parse(const char *text)
{
    while (text && *text) {
        const char *end = strchr(text, ',');
        size_t n = end ? (size_t)(end - text) : strlen(text);
        char item[96];
        if (n < sizeof item) {
            char id[32];
            float x, y, w, h;
            snprintf(item, sizeof item, "%.*s", (int)n, text);
            if (sscanf(item, "%31[^=]=%f:%f:%f:%f", id, &x, &y, &w, &h) == 5 && w > 0 && h > 0) {
                int at = -1;
                for (int i = 0; i < region_count; i++)
                    if (!strcmp(regions[i].id, id)) at = i;
                if (at < 0 && region_count < TOUCH_REGIONS && (!strcmp(id, "stick") || control_pad_index(id) >= 0)) {
                    at = region_count++;
                    regions[at].id = id;  /* the caller's string lives in the environment */
                }
                if (at >= 0) regions[at].x = x, regions[at].y = y, regions[at].w = w, regions[at].h = h;
            }
        }
        text = end ? end + 1 : text + n;
    }
}

static void layout_load(void)
{
    const char *setting;
    if (laid_out) return;
    laid_out = 1;
    setting = getenv("DHD_TOUCH_LAYOUT");
    layout_build(setting && !strcmp(setting, "left-handed"));
    if (setting && *setting && strcmp(setting, "default") && strcmp(setting, "left-handed")) layout_parse(setting);
}

/* DHD_TOUCH or DHD_TOUCH_LAYOUT changed (the CONTROLS page saved them): read them again. */
void touch_reset(void)
{
    enabled = -1;
    laid_out = 0;
}

/* A window point (SDL's normalized 0..1) as fractions of the picture. */
static void picture_point(float wx, float wy, float *px, float *py)
{
    int left, top, right, bottom, w, h;
    *px = wx, *py = wy;
    if (!gs_picture_rect_get(&left, &top, &right, &bottom, &w, &h) || w <= 0 || h <= 0) return;
    *px = (wx * w - left) / (float)(right - left);
    *py = (wy * h - top) / (float)(bottom - top);
}

static const TouchRegion *region_at(float x, float y)
{
    for (int i = 0; i < region_count; i++) {
        const TouchRegion *r = &regions[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return r;
    }
    return NULL;
}

/* sdl_host.c: one SDL finger event (down, motion, up or canceled). */
void touch_event(const void *event)
{
    const SDL_Event *e = event;
    SDL_FingerID id;
    float x = 0, y = 0;
    int down = -1, slot = -1;
    if (!touch_on()) return;
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        id = e->tfinger.fingerID, x = e->tfinger.x, y = e->tfinger.y, down = 1;
    } else if (e->type == SDL_EVENT_FINGER_MOTION) {
        id = e->tfinger.fingerID, x = e->tfinger.x, y = e->tfinger.y;
    } else if (e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) {
        id = e->tfinger.fingerID, down = 0;
    } else
        return;
    for (int i = 0; i < TOUCH_FINGERS; i++)
        if (fingers[i].active && fingers[i].id == id) slot = i;
    if (down == 0) {
        if (slot >= 0) fingers[slot].active = 0;
        return;
    }
    if (slot < 0) {
        if (down != 1) return;  /* a motion for a finger we never saw go down */
        for (int i = 0; i < TOUCH_FINGERS && slot < 0; i++)
            if (!fingers[i].active) slot = i;
        if (slot < 0) return;   /* too many fingers: the extra ones are ignored */
    }
    fingers[slot].id = id;
    fingers[slot].x = x, fingers[slot].y = y;
    fingers[slot].active = 1;
}

/* The on-screen controls into player 1's pad, while riding (in the menus a touch is the mouse). */
void touch_merge(int riding, HostPad *out)
{
    static const uint16_t pressure_order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                                PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
    uint16_t buttons = 0;
    if (!riding || !touch_on()) return;
    layout_load();
    for (int i = 0; i < TOUCH_FINGERS; i++) {
        float x, y;
        const TouchRegion *r;
        if (!fingers[i].active) continue;
        picture_point(fingers[i].x, fingers[i].y, &x, &y);
        r = region_at(x, y);
        if (!r) continue;
        if (!strcmp(r->id, "stick")) {
            float dx = (x - (r->x + r->w * 0.5f)) / (r->w * 0.5f);
            float dy = (y - (r->y + r->h * 0.5f)) / (r->h * 0.5f);
            if (dx < -1) dx = -1; else if (dx > 1) dx = 1;
            if (dy < -1) dy = -1; else if (dy > 1) dy = 1;
            out->left_x = (uint8_t)(128 + (int)(dx * 127));
            out->left_y = (uint8_t)(128 + (int)(dy * 127));
        } else {
            int control = control_pad_index(r->id);
            if (control >= 0) buttons |= (uint16_t)control_pad_button(control);
        }
    }
    out->buttons |= buttons;
    for (int i = 0; i < 12; i++)
        if (out->buttons & pressure_order[i]) out->pressure[i] = 255;
}
