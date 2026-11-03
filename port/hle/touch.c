/* On-screen touch controls (finding 141; the design is the reviewed spec it describes): the
   platform-neutral part, see touch.h. Everything here is the player's touches and time; the game
   is read only through TouchGame (the air flag and the race HUD hook, plus the clocks).

   The rules, in the order the code applies them:
   - Lift to change: a touch keeps its job until it lifts (Classic's ✕ may slide onto ◻ / ○ and
     the + spots, adding that press).
   - Touch areas are 20% larger than drawn; a touch in two goes to the nearer control.
   - Nothing dragged (the steering area, the trick stick's air area) starts in a gesture strip.
   - PEDAL's ✕ and the game's double-tap (sprint, 30 race fields, finding 049): a second press
     inside the window is held back up to 120 ms (sent before the window closes) unless it
     flicks; it waits for the window to close when the overlay itself let ✕ up, or HOP, SLIDE or
     the trick stick was opened and released since ✕ went up, or the race just resumed.
   - Camera (SELECT): a second press inside the game's SELECT double-press window (bail) waits.
   - The air layout comes air_wait_ms after the air flag; the trick stick's zones, latches and
     its one sending rule (settle, past the edge once the gate is open, landing).
   - Paused: race touches send nothing; the pause rows are ↓ / ↑ steps then ✕, sent as presses
     the game sees (whole fields of its VBLANKs). */
#include "touch.h"
#include "hle.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265f
#define FIELD_MS (1000.0 / 60.0)

enum {
    DOUBLE_TAP_FIELDS = 30,   /* the pad module's ✕ double-tap window (finding 049) */
    SELECT_FIELDS = 30,       /* SELECT twice = bail (check 3 measures it; the same counter) */
    PRESS_FIELDS = 3,         /* a tap the game must see: 50 ms */
    HOLD_BACK_MS = 120,
    SETTLE_MS = 70, GATE_MS = 60, TRIANGLE_DELAY_MS = 33,
    LEAVE_ARM_MS = 3000,
    HISTORY = 24
};
static const float SETTLE_MM = 1.0f, FIRST_LATCH_MM = 4.0f, SHIFT_PREVIEW_MM = 1.5f, SHIFT_MAX_MM = 5.0f;
static const float HYSTERESIS_DEG = 12.0f;
static const float FLICK_PAST_RIM_MM = 4.0f, FLICK_REACH_MM = 45.0f, FLICK_LANE_DEG = 30.0f, FLICK_SPEED = 150.0f;
static const float GAP_MM = 3.0f, AREA_GROW = 1.2f;
static const float REF_W = 154.5f, REF_H = 69.5f;  /* the reference phone (Mi 11i), mm */

/* ---------------------------------------------------------------- layout */

enum { AT_BR, AT_BL, AT_TL };   /* where a control is measured from */
enum { SHAPE_ROUND, SHAPE_BOX };
typedef struct { int anchor, shape; float px, py, w, h; } Place;  /* % of the reference screen; w, h % of its width / height */

static const char *const control_names[TC_COUNT] = {
    "pedal", "hop", "brake", "slide", "square", "circle", "camera", "pause",
    "cross", "triangle", "csquare", "ccircle", "plusleft", "plusright", "lbar", "rbar", "l3", "r3", "stick", "trick"
};
static const char *const control_labels[TC_COUNT] = {
    "PEDAL", "HOP", "BRAKE", "SLIDE", "", "", "", "", "", "", "", "", "", "", "", "", "L3", "R3", "STICK", "TRICK"
};

typedef struct { int on; float x, y, r, hw, hh; int box; } Ctl;  /* r: round; hw, hh: box half sizes */

/* The places in the spec, as % of the reference screen (centre x, y; width as % of its width, a
   box's height as % of its height). */
static int place_of(int preset, int control, int attacks, int hud_moved, Place *p)
{
    #define P(a, s, x, y, w, h) (*p = (Place){a, s, x, y, w, h}, 1)
    if (control == TC_STICK) return preset == TOUCH_CLASSIC ? P(AT_BL, SHAPE_ROUND, 17, 68, 15, 0) : P(AT_BL, SHAPE_ROUND, 21, 66, 15, 0);
    if (preset == TOUCH_CLASSIC) {
        switch (control) {
        case TC_CAMERA: return P(AT_TL, SHAPE_BOX, 23, 8, 7, 11.5f);        /* SELECT */
        case TC_PAUSE: return P(AT_TL, SHAPE_BOX, 32, 8, 7, 11.5f);         /* START */
        case TC_LBAR: return P(AT_BL, SHAPE_BOX, 22, 37, 20, 12.5f);
        case TC_RBAR: return P(AT_BR, SHAPE_BOX, 80, 37, 20, 12.5f);
        case TC_L3: return P(AT_BL, SHAPE_ROUND, 29.5f, 85, 5.6f, 0);
        case TC_R3: return P(AT_BR, SHAPE_ROUND, 71, 87, 5.6f, 0);
        case TC_TRIANGLE: return P(AT_BR, SHAPE_ROUND, 85, 56, 7.5f, 0);
        case TC_CSQUARE: return P(AT_BR, SHAPE_ROUND, 77, 69, 7.5f, 0);
        case TC_CCIRCLE: return P(AT_BR, SHAPE_ROUND, 93, 69, 7.5f, 0);
        case TC_CROSS: return P(AT_BR, SHAPE_ROUND, 85, 83, 8.5f, 0);
        case TC_PLUS_LEFT: return P(AT_BR, SHAPE_ROUND, 80.6f, 76.5f, 2.85f, 0);   /* drawn 4.4 mm */
        case TC_PLUS_RIGHT: return P(AT_BR, SHAPE_ROUND, 89.4f, 76.5f, 2.85f, 0);
        default: return 0;
        }
    }
    /* The top buttons end left of 36 %: the game centres its messages at the top ("YOU BAILED!",
       "FAILED TO LAND TRICK", the trick names), from about 36 % across on a 20:9 screen. */
    switch (control) {
    case TC_CAMERA: return P(AT_TL, SHAPE_BOX, 22.5f, 8, 5.2f, 11.5f);
    case TC_PAUSE: return P(AT_TL, SHAPE_BOX, 30, 8, 5.2f, 11.5f);
    case TC_TRICK: return P(AT_BR, SHAPE_ROUND, 70, 50, 20.1f, 0);         /* 31 mm */
    default: break;
    }
    if (!hud_moved) {   /* the fallback: the HUD stays where the game puts it */
        switch (control) {
        case TC_PEDAL: return P(AT_BR, SHAPE_ROUND, 80, 62, 15, 0);
        case TC_HOP: return P(AT_BR, SHAPE_ROUND, 77, 28, 11, 0);
        case TC_BRAKE: return P(AT_BR, SHAPE_ROUND, 65, 28, 9, 0);
        case TC_SLIDE: return P(AT_BR, SHAPE_ROUND, 88, 39, 7.1f, 0);
        case TC_SQUARE: return attacks == TOUCH_ATTACK_BUTTONS ? P(AT_BR, SHAPE_ROUND, 66, 66, 7.7f, 0) : 0;
        case TC_CIRCLE: return attacks == TOUCH_ATTACK_BUTTONS ? P(AT_BR, SHAPE_ROUND, 94, 60, 7.7f, 0) : 0;
        default: return 0;
        }
    }
    if (attacks == TOUCH_ATTACK_BUTTONS) {
        switch (control) {
        case TC_PEDAL: return P(AT_BR, SHAPE_ROUND, 82, 72, 15, 0);
        case TC_SQUARE: return P(AT_BR, SHAPE_ROUND, 68, 74, 7.7f, 0);
        case TC_CIRCLE: return P(AT_BR, SHAPE_ROUND, 92.8f, 53, 7.7f, 0);
        case TC_HOP: return P(AT_BR, SHAPE_ROUND, 79, 36, 11, 0);
        case TC_BRAKE: return P(AT_BR, SHAPE_ROUND, 67, 38, 9, 0);
        case TC_SLIDE: return P(AT_BR, SHAPE_ROUND, 91, 31, 8, 0);
        default: return 0;
        }
    }
    switch (control) {
    case TC_PEDAL: return P(AT_BR, SHAPE_ROUND, 85, 71, 15, 0);
    case TC_HOP: return P(AT_BR, SHAPE_ROUND, 81, 35, 11, 0);
    case TC_BRAKE: return P(AT_BR, SHAPE_ROUND, 69, 36, 9, 0);
    case TC_SLIDE: return P(AT_BR, SHAPE_ROUND, 92, 42, 8, 0);
    default: return 0;
    }
    #undef P
}

/* s before the Size setting: the default cluster needs 68 mm (HOP's top 54 mm up, the score
   panel 14 mm), Classic 62 mm. Not capped: the Size setting may go up to it. */
float touch_height_scale(float screen_height, int preset)
{
    return (screen_height - 14.0f) / (preset == TOUCH_CLASSIC ? 48.0f : 54.0f);
}

/* Buttons shrink less than positions, so every gap stays at least 3 mm (87% at s = 89%). */
float touch_button_scale(float s) { return 1.2f * s - 0.2f; }

/* ---------------------------------------------------------------- the state */

enum { JOB_NONE, JOB_STICK, JOB_BUTTON, JOB_PEDAL, JOB_TRICK, JOB_AIR_WAIT, JOB_PANEL, JOB_EDIT, JOB_IGNORED };
enum { PAUSE_NONE, PAUSE_ASKED, PAUSE_OPEN, PAUSE_CONTROLS, PAUSE_CONFIRM };
enum { ROW_RESUME, ROW_RESTART, ROW_QUIT, ROW_CONTROLS, ROW_BAIL, PAUSE_ROWS, PAUSE_BUTTON = 100 };
enum { PANEL_CLOSE, PANEL_BAIL, PANEL_LEAVE, PANEL_BUTTONS };
enum { CONFIRM_NO, CONFIRM_YES };
enum { EDIT_GROUND, EDIT_AIR, EDIT_MINUS, EDIT_PLUS, EDIT_RESET, EDIT_DONE, EDIT_BUTTONS };

typedef struct {
    int used;
    long long id;
    int job, control;
    uint16_t buttons;          /* a button's press(es) */
    int sent;                  /* the press is in the pad output (a held-back or guarded one: not yet) */
    double due_now, due_race;  /* when a held-back press goes out: wall ms, race ms (0: no limit) */
    int flicked, flick_armed;
    float x0, y0, x, y;
    double t0;
    float hx[HISTORY], hy[HISTORY];
    double ht[HISTORY];
    int hn;
    float grab_dx, grab_dy;    /* editor: finger to control centre */
    int target;                /* panel / editor: the button under the touch-down */
    double started_race;       /* race ms at touch-down */
    int fields;                /* fields the finger was down for (0: down and up between two) */
} Finger;

typedef struct {
    int on, finger;
    float cx, cy, ox, oy, nx, ny;   /* drawn centre, touch-down point, closest point to the centre */
    int shifted, first_done, back_inside, outside, landed, crossed, preview;
    double opened, last_latch, crossed_at, triangle_due;
    uint16_t held, sent;
    int triangle;              /* △ sent */
    uint8_t sectors;
} Trick;

typedef struct { uint16_t buttons; int fields; } Step;

struct Touch {
    TouchSettings set;
    TouchScreen screen;
    Ctl ctl[TC_COUNT];
    float s, bs;               /* the cluster's scale and its buttons' */
    float steer_x0, steer_x1, steer_y0, steer_y1;
    float air_x0, air_x1;
    Finger f[TOUCH_FINGERS];
    Trick trick;
    TouchGame game;
    int have_game;
    double now;
    /* visibility */
    double hud_seen;           /* wall ms the race HUD was last drawn */
    int hidden_by_pad, editor, editor_tab, edit_selected;
    TouchEdit edit_before;
    /* air */
    int air_layout;
    double air_since;          /* race ms the air flag came */
    int air_was;
    /* ✕ and SELECT windows (race ms) */
    double cross_released, select_released;
    int cross_was_down, cross_released_by_overlay, controls_since_release;
    double cross_release_wall;
    double hold_cross_until;   /* after a flick lifts: ✕ held until then (race ms; 0 none) */
    int select_was_down;
    /* timed presses */
    uint16_t pulse_buttons[16];
    uint32_t pulse_until[16];
    Step seq[48];
    int seq_len, seq_at;
    uint32_t seq_step_until;
    int seq_started;
    /* steering */
    int stick_finger;
    float stick_cx, stick_cy, stick_dx, stick_dy;   /* deflection -1..1 */
    float tilt_deg, tilt_mix;                         /* tilt steering, and its blend in (0..1) */
    int tilt_known;                                   /* a reading has come (none: the stick steers) */
    double tilt_mix_at;
    /* pause, panel */
    int pause, menu_cursor, confirm_row, pause_from_moving;
    double pause_asked_at;
    int panel_open;
    double leave_armed_until;
    int ask;
    int buzz;
    int saved;
    /* trick labels and hints */
    char label[TOUCH_TEXT];
    double label_until;
    int trick_counts[16];
    int tricks_sent;
    int hint;                  /* TOUCH_HINT_* on screen now */
    double hint_until, hint_race_start, last_hop_race, slide_far_since;
    int freeze, trick_hint_repeat;
    uint32_t freeze_at;        /* the hold starts at this VBLANK, once pictures with the tip are shown (0: none) */
    double freeze_since;
    int race_new;
    int race_was_on;           /* the race was on screen at the last update */
};

static const uint16_t SECTOR_BUTTONS[8] = {
    PAD_R1 | PAD_R2,   /* → Can Can */
    PAD_R1,            /* ↗ One Legger */
    PAD_L1 | PAD_R1,   /* ↑ Superman */
    PAD_L1,            /* ↖ One Hander */
    PAD_L1 | PAD_L2,   /* ← Table Top */
    PAD_L2,            /* ↙ Saran Wrap */
    PAD_L2 | PAD_R2,   /* ↓ Bar Kneel */
    PAD_R2,            /* ↘ R2 alone */
};

uint16_t touch_sector_buttons(int sector) { return SECTOR_BUTTONS[sector & 7]; }

const char *touch_trick_name(uint16_t b)
{
    b &= PAD_L1 | PAD_L2 | PAD_R1 | PAD_R2;
    switch (b) {
    case PAD_L1: return "ONE HANDER";
    case PAD_L1 | PAD_R1: return "SUPERMAN";
    case PAD_R1: return "ONE LEGGER";
    case PAD_R1 | PAD_R2: return "CAN CAN";
    case PAD_L2: return "SARAN WRAP";
    case PAD_L2 | PAD_R2: return "BAR KNEEL";
    case PAD_L1 | PAD_L2: return "TABLE TOP";
    case PAD_L1 | PAD_R2: return "NAC NAC";
    case PAD_L2 | PAD_R1: return "CANDY BAR";
    case PAD_L1 | PAD_L2 | PAD_R1: return "CLIFF HANGER";
    case PAD_L1 | PAD_L2 | PAD_R2: return "SCISSORS";
    case PAD_L1 | PAD_L2 | PAD_R1 | PAD_R2: return "SIGNATURE";
    default: return NULL;
    }
}

/* ---------------------------------------------------------------- settings */

void touch_settings_default(TouchSettings *s)
{
    memset(s, 0, sizeof *s);
    s->preset = TOUCH_THUMB_ZONES;
    s->opacity = 60;
    s->stick_range = 12;
    s->trick_size = 31;
    s->labels = TOUCH_LABELS_AUTO;
    s->attacks = TOUCH_ATTACK_FLICK;
    s->vibration = 1;
    s->tilt_strength = 50;
    s->hud_moved = 1;
    s->air_wait_ms = 150;
    for (int p = 0; p < 2; p++)
        for (int c = 0; c < TC_COUNT; c++) s->edit[p][c].size = 1;
}

void touch_layout_parse(TouchSettings *s, const char *text)
{
    for (int p = 0; p < 2; p++)
        for (int c = 0; c < TC_COUNT; c++) s->edit[p][c] = (TouchEdit){0, 0, 1};
    while (text && *text) {
        char name[32];
        float dx, dy, size;
        int n = 0;
        if (sscanf(text, " %31[a-z0-9.]=%f,%f,%f%n", name, &dx, &dy, &size, &n) == 4 && n > 0) {   /* digits: l3, r3 */
            int p = !strncmp(name, "classic.", 8) ? 1 : !strncmp(name, "thumb.", 6) ? 0 : -1;
            const char *control = p == 1 ? name + 8 : name + 6;
            for (int c = 0; p >= 0 && c < TC_COUNT; c++)
                if (!strcmp(control, control_names[c]) && fabsf(dx) < 200 && fabsf(dy) < 200 && size >= 0.5f && size <= 2.0f)
                    s->edit[p][c] = (TouchEdit){dx, dy, size};
        }
        const char *next = strchr(text, ';');
        text = next ? next + 1 : NULL;
    }
}

void touch_layout_format(const TouchSettings *s, char *out, int size)
{
    int at = 0;
    out[0] = 0;
    for (int p = 0; p < 2; p++)
        for (int c = 0; c < TC_COUNT; c++) {
            const TouchEdit *e = &s->edit[p][c];
            if (e->dx == 0 && e->dy == 0 && e->size == 1) continue;
            int n = snprintf(out + at, (size_t)(size - at), "%s%s.%s=%.1f,%.1f,%.2f", at ? ";" : "",
                             p ? "classic" : "thumb", control_names[c], e->dx, e->dy, e->size);
            if (n < 0 || at + n >= size) { out[at] = 0; return; }
            at += n;
        }
}

/* ---------------------------------------------------------------- geometry */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float dist(float ax, float ay, float bx, float by) { return hypotf(ax - bx, ay - by); }
static int right_handed(const Touch *t) { return !t->set.left_handed; }
/* x as the right-handed layout has it (the left hand mirrors). */
static float mirror_x(const Touch *t, float x) { return right_handed(t) ? x : t->screen.width - x; }

/* The layout for the settings and the screen. */
static void lay_out(Touch *t, int hud_moved)
{
    const TouchSettings *st = &t->set;
    float W = t->screen.width, H = t->screen.height;
    float limit = touch_height_scale(H, st->preset);
    float s = st->size > 0 ? st->size / 100.0f : 1.0f;
    if (s > limit) s = limit;
    if (s < 0.5f) s = 0.5f;
    t->s = s;
    t->bs = touch_button_scale(s);
    for (int c = 0; c < TC_COUNT; c++) {
        Place p;
        Ctl *k = &t->ctl[c];
        memset(k, 0, sizeof *k);
        if (!place_of(st->preset, c, st->attacks, hud_moved, &p)) continue;
        float w = p.w * REF_W / 100, h = p.h * REF_H / 100;
        float scale = t->bs;
        float x, y;
        if (p.anchor == AT_BR) x = W - (100 - p.px) * REF_W / 100 * s, y = H - (100 - p.py) * REF_H / 100 * s;
        else if (p.anchor == AT_BL) x = p.px * REF_W / 100 * s, y = H - (100 - p.py) * REF_H / 100 * s;
        else x = p.px * REF_W / 100, y = p.py * REF_H / 100;
        const TouchEdit *e = &st->edit[st->preset == TOUCH_CLASSIC][c];
        x += e->dx, y += e->dy;
        scale *= e->size;
        if (c == TC_TRICK) scale = e->size * st->trick_size / 31.0f;   /* its own setting */
        if (c == TC_STICK) scale = 1;
        k->on = 1;
        k->x = mirror_x(t, x);
        k->y = y;
        k->box = p.shape == SHAPE_BOX;
        k->r = w / 2 * scale;
        k->hw = w / 2 * scale, k->hh = h / 2 * scale;
        if (c == TC_TRICK) k->r = st->trick_size / 2.0f * e->size;
    }
    /* the camera cutout (read at run time) is kept clear: a control over it moves inward */
    const TouchScreen *sc = &t->screen;
    if (sc->cut_x1 > sc->cut_x0 && sc->cut_y1 > sc->cut_y0)
        for (int c = 0; c < TC_COUNT; c++) {
            Ctl *k = &t->ctl[c];
            float rx = k->box ? k->hw : k->r, ry = k->box ? k->hh : k->r;
            if (!k->on || k->x + rx < sc->cut_x0 || k->x - rx > sc->cut_x1 || k->y + ry < sc->cut_y0 || k->y - ry > sc->cut_y1) continue;
            if (sc->cut_x1 < W / 2) k->x = sc->cut_x1 + rx + 1;
            else k->x = sc->cut_x0 - rx - 1;
        }
    /* the steering area: 60 mm from the left strip, 3 mm short of the midline and of the right
       cluster's touch areas; Classic's to 44% of the width, below the L bar */
    float left = t->screen.strip_left, x1 = left + 60, top = 0;
    if (st->preset == TOUCH_CLASSIC) {
        x1 = 0.44f * W;
        top = t->ctl[TC_LBAR].y + t->ctl[TC_LBAR].hh * AREA_GROW + GAP_MM;
    } else {
        if (x1 > W / 2 - GAP_MM) x1 = W / 2 - GAP_MM;
        for (int c = 0; c < TC_COUNT; c++) {
            const Ctl *k = &t->ctl[c];
            if (!k->on || c == TC_STICK || c == TC_TRICK || c == TC_CAMERA || c == TC_PAUSE) continue;
            float edge = mirror_x(t, k->x) - k->r * AREA_GROW - GAP_MM;
            if (edge < x1) x1 = edge;
        }
        top = t->ctl[TC_CAMERA].y + t->ctl[TC_CAMERA].hh * AREA_GROW + GAP_MM;
    }
    t->steer_x0 = left, t->steer_x1 = x1, t->steer_y0 = top, t->steer_y1 = H - t->screen.strip_bottom;
    if (!right_handed(t)) {   /* mirrored: the area measured from the right strip */
        float a = W - x1 - (t->screen.strip_right - left), b = W - t->screen.strip_right;
        t->steer_x0 = a, t->steer_x1 = b;
    }
    t->air_x0 = right_handed(t) ? W / 2 : t->screen.strip_left;
    t->air_x1 = right_handed(t) ? W - t->screen.strip_right : W / 2;
}

int touch_control(const Touch *t, int control, float *x, float *y, float *r)
{
    if (control < 0 || control >= TC_COUNT || !t->ctl[control].on) return 0;
    const Ctl *k = &t->ctl[control];
    if (x) *x = k->x;
    if (y) *y = k->y;
    if (r) *r = k->box ? fmaxf(k->hw, k->hh) : k->r;
    return 1;
}

/* How far outside its touch area the point is (negative inside), or a large number if not in it. */
static float hit(const Touch *t, int c, float x, float y, float grow)
{
    const Ctl *k = &t->ctl[c];
    if (!k->on) return 1e9f;
    if (k->box) {
        float dx = fabsf(x - k->x) - k->hw, dy = fabsf(y - k->y) - k->hh;
        float out = fmaxf(dx, dy);
        return fabsf(x - k->x) <= k->hw * grow && fabsf(y - k->y) <= k->hh * grow ? out : 1e9f;
    }
    float d = dist(x, y, k->x, k->y);
    return d <= k->r * grow ? d - k->r : 1e9f;
}

/* The control whose touch area (20% larger) holds the point, nearest first; -1 none. */
static int control_at(const Touch *t, float x, float y, const int *list, int n)
{
    int best = -1;
    float best_out = 1e8f;
    for (int i = 0; i < n; i++) {
        float out = hit(t, list[i], x, y, AREA_GROW);
        if (out < best_out) best_out = out, best = list[i];
    }
    return best;
}

static int in_rect(float x, float y, float x0, float y0, float x1, float y1) { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }

static int in_strip(const Touch *t, float x, float y)
{
    const TouchScreen *s = &t->screen;
    return x < s->strip_left || x > s->width - s->strip_right || y < s->strip_top || y > s->height - s->strip_bottom;
}

/* ---------------------------------------------------------------- output pieces */

static uint32_t press_vsyncs(const Touch *t, int fields)
{
    int fps = t->game.fps > 0 ? t->game.fps : 60;
    return (uint32_t)((fields * fps + 59) / 60);
}

static void buzz(Touch *t, int kind)
{
    if (t->set.vibration && kind > t->buzz) t->buzz = kind;
}

static void pulse(Touch *t, uint16_t buttons, int fields)
{
    for (int i = 0; i < 16; i++)
        if (!t->pulse_buttons[i]) {
            t->pulse_buttons[i] = buttons;
            t->pulse_until[i] = t->game.vsync + press_vsyncs(t, fields);
            return;
        }
}

/* A sequence of presses, each PRESS_FIELDS down and PRESS_FIELDS up (the menu steps). */
static void seq_reset(Touch *t) { t->seq_len = t->seq_at = 0; t->seq_started = 0; }
static void seq_add(Touch *t, uint16_t buttons)
{
    if (t->seq_at >= t->seq_len) seq_reset(t);   /* the last sequence is over: a new one */
    if (t->seq_len + 2 > (int)(sizeof t->seq / sizeof *t->seq)) return;
    t->seq[t->seq_len++] = (Step){buttons, PRESS_FIELDS};
    t->seq[t->seq_len++] = (Step){0, PRESS_FIELDS};
}
static int seq_busy(const Touch *t) { return t->seq_at < t->seq_len; }

static void seq_cursor_to(Touch *t, int row)
{
    for (; t->menu_cursor < row; t->menu_cursor++) seq_add(t, PAD_DOWN);
    for (; t->menu_cursor > row; t->menu_cursor--) seq_add(t, PAD_UP);
}

static void seq_advance(Touch *t)
{
    if (!seq_busy(t)) return;
    if (!t->seq_started) {
        t->seq_started = 1;
        t->seq_step_until = t->game.vsync + press_vsyncs(t, t->seq[t->seq_at].fields);
        return;
    }
    while (seq_busy(t) && (int32_t)(t->game.vsync - t->seq_step_until) >= 0) {
        if (++t->seq_at < t->seq_len) t->seq_step_until = t->game.vsync + press_vsyncs(t, t->seq[t->seq_at].fields);
    }
}

/* ---------------------------------------------------------------- the finger list */

static Finger *finger_of(Touch *t, long long id)
{
    for (int i = 0; i < TOUCH_FINGERS; i++)
        if (t->f[i].used && t->f[i].id == id) return &t->f[i];
    return NULL;
}

static void history_add(Finger *f, float x, float y, double now)
{
    if (f->hn == HISTORY) {
        memmove(f->hx, f->hx + 1, sizeof f->hx - sizeof *f->hx);
        memmove(f->hy, f->hy + 1, sizeof f->hy - sizeof *f->hy);
        memmove(f->ht, f->ht + 1, sizeof f->ht - sizeof *f->ht);
        f->hn--;
    }
    f->hx[f->hn] = x, f->hy[f->hn] = y, f->ht[f->hn] = now;
    f->hn++;
}

/* mm/s over the last 50 ms of the finger's path. */
static float speed(const Finger *f, double now)
{
    int i = f->hn - 1;
    while (i > 0 && now - f->ht[i - 1] <= 50) i--;
    if (i == f->hn - 1 && i > 0) i--;
    double dt = now - f->ht[i];
    if (i < 0 || dt <= 1) return 0;
    return (float)(dist(f->x, f->y, f->hx[i], f->hy[i]) * 1000.0 / dt);
}

/* The thumb moved less than SETTLE_MM over the last SETTLE_MS. */
static int settled(const Finger *f, double now)
{
    for (int i = f->hn - 1; i >= 0; i--) {
        if (dist(f->hx[i], f->hy[i], f->x, f->y) >= SETTLE_MM) return 0;
        if (now - f->ht[i] >= SETTLE_MS) return 1;
    }
    return now - f->t0 >= SETTLE_MS;
}

static double race_ms(const Touch *t) { return t->game.race_ms; }

/* ---------------------------------------------------------------- ✕ and its windows */

static int cross_window_open(const Touch *t)
{
    return t->cross_released > -1e8 && race_ms(t) - t->cross_released < DOUBLE_TAP_FIELDS * FIELD_MS;
}
static double cross_window_end(const Touch *t) { return t->cross_released + DOUBLE_TAP_FIELDS * FIELD_MS; }

/* A ✕ press (PEDAL, or Classic's ✕) begins: when it may go out. */
static void cross_press_begins(Touch *t, Finger *f, int classic)
{
    f->sent = 0, f->due_now = 0, f->due_race = 0;
    if (t->hold_cross_until > 0) {   /* the flick-lift's held ✕: this touch continues the same press */
        t->hold_cross_until = 0;
        f->sent = 1;
        return;
    }
    for (int i = 0; i < TOUCH_FINGERS; i++)   /* another finger holds ✕ already: the same press */
        if (&t->f[i] != f && t->f[i].used && (t->f[i].buttons & PAD_CROSS) && t->f[i].sent) { f->sent = 1; return; }
    if (!cross_window_open(t)) { f->sent = 1; return; }
    int guarded = t->cross_released_by_overlay || (!classic && t->controls_since_release);
    if (guarded) { f->due_race = cross_window_end(t); return; }   /* waits for the window to close */
    if (classic || t->set.attacks == TOUCH_ATTACK_BUTTONS) { f->sent = 1; return; }
    /* a real second tap: held back up to 120 ms (a flick then sends no ✕), always before the
       window closes */
    f->due_now = t->now + HOLD_BACK_MS;
    f->due_race = cross_window_end(t) - FIELD_MS;
}

static void select_press_begins(Touch *t, Finger *f)
{
    f->sent = 1, f->due_now = 0, f->due_race = 0;
    if (t->select_released > -1e8 && race_ms(t) - t->select_released < SELECT_FIELDS * FIELD_MS) {
        f->sent = 0;
        f->due_race = t->select_released + SELECT_FIELDS * FIELD_MS;
    }
}

/* Held-back and guarded presses whose time came. */
static void due_presses(Touch *t)
{
    for (int i = 0; i < TOUCH_FINGERS; i++) {
        Finger *f = &t->f[i];
        if (!f->used || f->sent || !f->buttons) continue;
        if (f->job == JOB_PEDAL && f->flicked) continue;   /* a flick: no ✕ until it lifts */
        int now_due = f->due_now > 0 && t->now >= f->due_now;
        int race_due = f->due_race > 0 && race_ms(t) >= f->due_race;
        if (now_due || race_due || (!f->due_now && !f->due_race)) {
            f->sent = 1;
            buzz(t, TOUCH_BUZZ_TICK);
        }
    }
}

/* ---------------------------------------------------------------- the trick stick */

static float trick_scale(const Touch *t) { return t->set.trick_size / 31.0f; }

static int sector_of(float dx, float dy)
{
    float a = atan2f(-dy, dx);   /* y up */
    int s = (int)floorf((a + PI_F / 8) / (PI_F / 4));
    return ((s % 8) + 8) % 8;
}

static float angle_diff(float a, float b)
{
    float d = fmodf(a - b + 3 * PI_F, 2 * PI_F) - PI_F;
    return d;
}

static void trick_open(Touch *t, Finger *f)
{
    Trick *k = &t->trick;
    float k_r = trick_scale(t) * 15.5f;
    memset(k, 0, sizeof *k);
    k->on = 1;
    k->finger = (int)(f - t->f);
    k->ox = f->x0, k->oy = f->y0;
    /* near the edges the centre shifts inward by at most 5 mm */
    float x0 = t->screen.strip_left, x1 = t->screen.width - t->screen.strip_right;
    float y0 = t->screen.strip_top, y1 = t->screen.height - t->screen.strip_bottom;
    float cx = f->x0, cy = f->y0;
    if (cx + k_r > x1) cx -= fminf(cx + k_r - x1, SHIFT_MAX_MM);
    if (cx - k_r < x0) cx += fminf(x0 - (cx - k_r), SHIFT_MAX_MM);
    if (cy + k_r > y1) cy -= fminf(cy + k_r - y1, SHIFT_MAX_MM);
    if (cy - k_r < y0) cy += fminf(y0 - (cy - k_r), SHIFT_MAX_MM);
    k->cx = cx, k->cy = cy;
    k->shifted = dist(cx, cy, f->x0, f->y0) > 0.25f;
    k->nx = f->x0, k->ny = f->y0;
    k->opened = t->now;
    k->preview = -1;
    k->outside = 0;
    f->job = JOB_TRICK;
    buzz(t, TOUCH_BUZZ_TICK);
}

/* A shoulder set as 0-15 (the trick counts). */
static int trick_id(uint16_t b)
{
    return ((b & PAD_L1) ? 1 : 0) | ((b & PAD_L2) ? 2 : 0) | ((b & PAD_R1) ? 4 : 0) | ((b & PAD_R2) ? 8 : 0);
}

/* The held set goes to the game. */
static void trick_send(Touch *t)
{
    Trick *k = &t->trick;
    if (!k->held || (k->held & ~k->sent) == 0) return;
    k->sent |= k->held;
    const char *name = touch_trick_name(k->sent);
    /* Trick labels: Auto until that trick was sent 3 times, Always, Off */
    int show = t->set.labels == TOUCH_LABELS_ALWAYS || (t->set.labels == TOUCH_LABELS_AUTO && t->trick_counts[trick_id(k->sent)] < 3);
    if (name && show) {
        snprintf(t->label, sizeof t->label, "%s", name);
        t->label_until = t->now + 1500;
    }
}

static void trick_latch(Touch *t, int sector)
{
    Trick *k = &t->trick;
    if (k->sectors & (1u << sector)) return;
    uint16_t before = k->held;
    k->sectors |= (uint8_t)(1u << sector);
    k->held |= SECTOR_BUTTONS[sector];
    k->last_latch = t->now;
    k->first_done = 1;
    k->back_inside = 0;
    k->crossed = 0;   /* the gate counts from the latest latch */
    if (k->held != before) buzz(t, TOUCH_BUZZ_DOUBLE);
}

/* A neighbouring sector adds only once the thumb is 12° past the edge of every held sector it borders. */
static int past_held_edges(const Trick *k, int sector, float angle)
{
    float centre = sector * PI_F / 4;
    for (int side = -1; side <= 1; side += 2) {
        int h = (sector + side + 8) % 8;
        if (!(k->sectors & (1u << h))) continue;
        float edge = centre + side * PI_F / 8;
        float into = -side * angle_diff(angle, edge);   /* positive: inside `sector`, away from h */
        if (into < HYSTERESIS_DEG * PI_F / 180) return 0;
    }
    return 1;
}

/* The trick stick follows the thumb (a motion sample, or the update's clock). */
static void trick_step(Touch *t, Finger *f)
{
    Trick *k = &t->trick;
    float sc = trick_scale(t), r_in = 7 * sc, r_out = 14 * sc;
    float dx = f->x - k->cx, dy = f->y - k->cy, r = hypotf(dx, dy);
    if (k->landed) return;
    if (r < r_in) {
        if (k->outside && k->first_done) {
            k->back_inside = 1;
            k->nx = f->x, k->ny = f->y;
        }
        k->outside = 0;
        if (dist(f->x, f->y, k->cx, k->cy) < dist(k->nx, k->ny, k->cx, k->cy)) k->nx = f->x, k->ny = f->y;
        /* preview: by travel on a shifted stick (after 1.5 mm) and for a latch read by travel */
        int by_travel = k->shifted || !k->first_done || k->back_inside;
        float tx = f->x - k->nx, ty = f->y - k->ny;
        if (by_travel) k->preview = hypotf(tx, ty) >= SHIFT_PREVIEW_MM ? sector_of(tx, ty) : (k->shifted ? -1 : sector_of(dx, dy));
        else k->preview = r > 0.5f ? sector_of(dx, dy) : -1;
        if (r < 0.5f && !k->shifted && !k->first_done) k->preview = -1;
        k->crossed = 0;
        return;
    }
    /* 7 mm and past: latches */
    int was_outside = k->outside;
    k->outside = 1;
    if (!k->first_done) {
        if (dist(f->x, f->y, k->ox, k->oy) >= FIRST_LATCH_MM) trick_latch(t, sector_of(f->x - k->nx, f->y - k->ny));
    } else if (k->back_inside) {
        trick_latch(t, sector_of(f->x - k->nx, f->y - k->ny));
        k->back_inside = 0;
    } else {
        int s = sector_of(dx, dy);
        if (!(k->sectors & (1u << s)) && past_held_edges(k, s, atan2f(-dy, dx))) trick_latch(t, s);
    }
    (void)was_outside;
    k->preview = sector_of(dx, dy);
    /* past 14 mm: △, once the gate (60 ms after the latest latch) is open */
    if (r > r_out) {
        if (!k->crossed) k->crossed = 1, k->crossed_at = t->now;
    } else if (!k->triangle) {
        k->crossed = 0;            /* a fast overshoot before the gate can come back in */
        k->triangle_due = 0;
    }
}

/* The trick stick's timed rules: the gate and △, settling. */
static void trick_tick(Touch *t)
{
    Trick *k = &t->trick;
    if (!k->on || k->landed) return;
    Finger *f = &t->f[k->finger];
    double gate = k->last_latch + GATE_MS;
    if (k->crossed && k->held && t->now >= gate && !k->triangle && !k->triangle_due) {
        trick_send(t);   /* the chord at once */
        double a = k->crossed_at + TRIANGLE_DELAY_MS, b = gate + TRIANGLE_DELAY_MS;
        k->triangle_due = a > b ? a : b;
    }
    if (k->triangle_due && !k->triangle && t->now >= k->triangle_due) {
        k->triangle = 1;
        buzz(t, TOUCH_BUZZ_STRONG);
    }
    if (t->now - k->opened >= SETTLE_MS && settled(f, t->now)) trick_send(t);
}

static void trick_close(Touch *t)
{
    Trick *k = &t->trick;
    if (!k->on) return;
    if (touch_trick_name(k->sent)) {   /* counted from what was sent: no game read */
        t->trick_counts[trick_id(k->sent)]++;
        t->tricks_sent++;
    }
    memset(k, 0, sizeof *k);
}

/* ---------------------------------------------------------------- visibility, pause */

static int race_on_screen(const Touch *t) { return t->hud_seen > 0 && t->now - t->hud_seen < 250; }

int touch_visible(const Touch *t)
{
    if (t->editor) return 1;
    if (t->pause != PAUSE_NONE) return 1;
    return race_on_screen(t) && !t->hidden_by_pad;
}

/* The overlay's touches: while it shows, and in a race it hid for a controller (the touch that
   brings it back); in the menus a touch is the menus' (their mouse). */
int touch_owns_touches(const Touch *t) { return touch_visible(t) || (t->hidden_by_pad && race_on_screen(t)); }

int touch_hud_mode(const Touch *t)
{
    if (!t->set.hud_moved || t->editor || !race_on_screen(t) || t->hidden_by_pad) return 0;
    return right_handed(t) ? 1 : 2;
}

int touch_freeze(const Touch *t) { return t->freeze; }

static int paused(const Touch *t) { return t->pause != PAUSE_NONE || t->editor; }

static void end_finger_job(Touch *t, Finger *f);

/* Every race touch lets go (the overlay hid, or the touches were taken away). */
static void drop_race_touches(Touch *t)
{
    for (int i = 0; i < TOUCH_FINGERS; i++)
        if (t->f[i].used) {
            end_finger_job(t, &t->f[i]);
            t->f[i].job = JOB_IGNORED;
        }
}

/* The race goes on again (Resume, a closed menu): touches still down are new presses, with the
   resume guard: a ✕ waits for the double-tap window, which counts from now as a release the
   overlay made (the Resume row's own ✕). */
static void race_resumes(Touch *t)
{
    t->pause = PAUSE_NONE;
    t->cross_released = race_ms(t);
    t->cross_released_by_overlay = 1;
    t->select_released = race_ms(t);
    t->hold_cross_until = 0;
    for (int i = 0; i < TOUCH_FINGERS; i++) {
        Finger *f = &t->f[i];
        if (!f->used) continue;
        if (f->job == JOB_PEDAL || (f->job == JOB_BUTTON && (f->buttons & PAD_CROSS))) cross_press_begins(t, f, f->job != JOB_PEDAL);
        else if (f->job == JOB_BUTTON && f->control == TC_CAMERA) select_press_begins(t, f);
        else if (f->job == JOB_BUTTON) f->sent = 1;
        else if (f->job == JOB_TRICK || f->job == JOB_AIR_WAIT || f->job == JOB_PANEL) f->job = JOB_IGNORED;
    }
    if (t->trick.on) trick_close(t);
}

static void pause_open(Touch *t)
{
    t->pause = PAUSE_ASKED;
    t->pause_asked_at = t->now;
    t->pause_from_moving = t->game.clock_moving;
    t->menu_cursor = 0;
    seq_reset(t);
    seq_add(t, PAD_START);
}

/* Pause (the top button, Back, Classic's START): offline START and the pause rows; online the panel. */
static void pause_pressed(Touch *t)
{
    if (t->game.online) {
        t->panel_open = !t->panel_open;
        t->leave_armed_until = 0;
        return;
    }
    if (t->pause == PAUSE_NONE) pause_open(t);
}

/* ---------------------------------------------------------------- the panels' buttons */

typedef struct { float x, y, hw, hh; } Box;

static Box pause_row_box(const Touch *t, int row)
{
    float W = t->screen.width, H = t->screen.height;
    float rh = fmaxf(9.0f, H * 0.124f), top = H * 0.05f + H * 0.08f + 6;
    float y = top + rh * (row + 0.5f) + (row >= ROW_CONTROLS ? 2.0f : 0);
    return (Box){W / 2, y, W * 0.30f, rh / 2 - 0.4f};
}

static Box confirm_box(const Touch *t, int row)
{
    float W = t->screen.width, H = t->screen.height;
    return (Box){W / 2, H * 0.5f + (row == CONFIRM_NO ? -6.0f : 6.0f), W * 0.2f, 4.5f};
}

static Box panel_box(const Touch *t, int b)
{
    /* under the top buttons, over the race: 9 mm targets, 3 mm apart */
    float y = t->ctl[TC_CAMERA].y + t->ctl[TC_CAMERA].hh * AREA_GROW + GAP_MM + 4.5f + 2;
    float w[PANEL_BUTTONS] = {16, 20, 24};
    float x = mirror_x(t, t->ctl[TC_CAMERA].x) - t->ctl[TC_CAMERA].hw;
    for (int i = 0; i < b; i++) x += w[i] + GAP_MM + (i + 1 == PANEL_LEAVE ? 4 : 0);
    Box r = {x + w[b] / 2, y, w[b] / 2, 4.5f};
    r.x = mirror_x(t, r.x);
    return r;
}

/* The editor's buttons, in the top buttons' row (their default places: the editor moves them):
   GROUND and AIR on one side of them, - + RESET DONE on the wider side (both groups there, GROUND
   and AIR first, when the other side is too narrow); b = EDIT_BUTTONS is the
   size readout's place, under GROUND and AIR. A screen too narrow for that gets one row below. */
static Box editor_box(const Touch *t, int b)
{
    static const float w[EDIT_BUTTONS] = {18, 14, 10, 10, 16, 16};
    float W = t->screen.width;
    Place cam, pau;
    place_of(t->set.preset, TC_CAMERA, t->set.attacks, 1, &cam);
    place_of(t->set.preset, TC_PAUSE, t->set.attacks, 1, &pau);
    float hw = cam.w * REF_W / 200, y = cam.py * REF_H / 100, hh = cam.h * REF_H / 200;
    float a0 = mirror_x(t, cam.px * REF_W / 100), a1 = mirror_x(t, pau.px * REF_W / 100);
    float span0 = fminf(a0, a1) - hw - 2, span1 = fmaxf(a0, a1) + hw + 2;   /* the top buttons, a 2 mm margin */
    float group_a = w[EDIT_GROUND] + 2 + w[EDIT_AIR], group_b = w[EDIT_MINUS] + w[EDIT_PLUS] + w[EDIT_RESET] + w[EDIT_DONE] + 6;
    float left_room = span0 - 2, right_room = W - 2 - span1;
    int b_right = right_room >= left_room;
    float wide = b_right ? right_room : left_room, narrow = b_right ? left_room : right_room;
    int split = wide >= group_b && narrow >= group_a;
    int together = !split && wide >= group_a + 2 + group_b;   /* both groups on the wide side */
    if (split || together) {
        float ax, bx;
        if (split) ax = b_right ? span0 - group_a : span1, bx = b_right ? span1 : span0 - group_b;
        else if (b_right) ax = span1, bx = span1 + group_a + 2;
        else bx = span0 - group_b, ax = bx - 2 - group_a;
        if (b == EDIT_BUTTONS) return (Box){ax + group_a / 2, y + hh + 3.0f, group_a / 2, 1.5f};
        if (b <= EDIT_AIR) return (Box){ax + (b == EDIT_GROUND ? w[0] / 2 : w[0] + 2 + w[1] / 2), y, w[b] / 2, hh};
        float x = bx;
        for (int i = EDIT_MINUS; i < b; i++) x += w[i] + 2;
        return (Box){x + w[b] / 2, y, w[b] / 2, hh};
    }
    float total = 0;
    for (int i = 0; i < EDIT_BUTTONS; i++) total += w[i] + 2;
    total += 28;   /* the size readout */
    float x = (W - total) / 2;
    if (b == EDIT_BUTTONS) b = EDIT_AIR;   /* the readout: the caller puts it beside AIR */
    for (int i = 0; i < b; i++) x += w[i] + 2 + (i == EDIT_AIR ? 28 : 0);
    return (Box){x + w[b] / 2, t->screen.height * 0.16f, w[b] / 2, 4.5f};
}

static int in_box(Box b, float x, float y) { return fabsf(x - b.x) <= b.hw && fabsf(y - b.y) <= b.hh; }

int touch_target(const Touch *t, int kind, int index, float *x, float *y)
{
    static const int counts[4] = {PAUSE_ROWS, 2, PANEL_BUTTONS, EDIT_BUTTONS};
    if (kind < 0 || kind > 3 || index < 0 || index >= counts[kind]) return 0;
    Box b = kind == TOUCH_TARGET_PAUSE ? pause_row_box(t, index) : kind == TOUCH_TARGET_CONFIRM ? confirm_box(t, index)
          : kind == TOUCH_TARGET_PANEL ? panel_box(t, index) : editor_box(t, index);
    *x = b.x, *y = b.y;
    return 1;
}

static int pause_row_at(const Touch *t, float x, float y)
{
    if (t->pause == PAUSE_CONFIRM) {
        for (int r = 0; r < 2; r++) if (in_box(confirm_box(t, r), x, y)) return r;
        return -1;
    }
    for (int r = 0; r < PAUSE_ROWS; r++) if (in_box(pause_row_box(t, r), x, y)) return r;
    return -1;
}

static int panel_button_at(const Touch *t, float x, float y)
{
    for (int b = 0; b < PANEL_BUTTONS; b++) if (in_box(panel_box(t, b), x, y)) return b;
    return -1;
}

static void pause_row_tapped(Touch *t, int row)
{
    if (seq_busy(t)) return;
    if (t->pause == PAUSE_CONTROLS) { t->pause = PAUSE_OPEN; return; }
    if (t->pause == PAUSE_CONFIRM) {
        if (row == CONFIRM_YES) {
            seq_add(t, PAD_DOWN);    /* the question opens on No (check 10) */
            seq_add(t, PAD_CROSS);
            t->pause = PAUSE_NONE;   /* the race restarts or ends */
            race_resumes(t);
        } else if (row == CONFIRM_NO) {
            seq_add(t, PAD_TRIANGLE);
            t->pause = PAUSE_OPEN;
            t->menu_cursor = 0;      /* the game rebuilds its menu with the cursor on RESUME (phone test) */
        }
        return;
    }
    if (t->pause != PAUSE_OPEN) return;
    switch (row) {
    case ROW_RESUME:
    case ROW_RESTART:
    case ROW_QUIT: {
        int game_row = t->game.pause_row[row];
        if (game_row < 0) return;
        seq_cursor_to(t, game_row);
        seq_add(t, PAD_CROSS);
        if (row == ROW_RESUME) race_resumes(t);
        else t->pause = PAUSE_CONFIRM;
        break;
    }
    case ROW_CONTROLS:
        t->pause = PAUSE_CONTROLS;
        break;
    case ROW_BAIL:   /* START (out of the menu), then SELECT twice */
        seq_add(t, PAD_START);
        seq_add(t, PAD_SELECT);
        seq_add(t, PAD_SELECT);
        race_resumes(t);
        t->select_released = -1e9;
        break;
    }
}

static void panel_tapped(Touch *t, int b)
{
    if (b != PANEL_LEAVE) t->leave_armed_until = 0;
    switch (b) {
    case PANEL_CLOSE: t->panel_open = 0; break;
    case PANEL_BAIL:
        if (!seq_busy(t)) { seq_add(t, PAD_SELECT); seq_add(t, PAD_SELECT); }
        t->panel_open = 0;
        break;
    case PANEL_LEAVE:
        if (t->leave_armed_until > t->now) {
            t->ask = TOUCH_ASK_LEAVE_RACE;
            t->panel_open = 0;
            t->leave_armed_until = 0;
        } else t->leave_armed_until = t->now + LEAVE_ARM_MS;
        break;
    }
}

/* ---------------------------------------------------------------- the editor */

static int editable(const Touch *t, int c)
{
    if (!t->ctl[c].on) return 0;
    if (t->editor_tab == EDIT_AIR) return c == TC_TRICK;
    return c != TC_TRICK && c != TC_PLUS_LEFT && c != TC_PLUS_RIGHT;
}

/* The Ground tab's rules: on screen, 3 mm gaps, nothing in an attack lane, nothing dragged out of a strip. */
static int layout_valid(const Touch *t)
{
    float W = t->screen.width, H = t->screen.height;
    for (int c = 0; c < TC_COUNT; c++) {
        const Ctl *k = &t->ctl[c];
        if (!k->on) continue;
        float rx = k->box ? k->hw : k->r, ry = k->box ? k->hh : k->r;
        if (c == TC_TRICK) rx = ry = 7;
        if (k->x - rx < 0 || k->x + rx > W || k->y - ry < 0 || k->y + ry > H) return 0;
        if ((c == TC_STICK || c == TC_TRICK) && in_strip(t, k->x, k->y)) return 0;
        if (c == TC_STICK || c == TC_TRICK || c == TC_PLUS_LEFT || c == TC_PLUS_RIGHT) continue;
        for (int d = c + 1; d < TC_COUNT; d++) {
            const Ctl *m = &t->ctl[d];
            if (!m->on || d == TC_STICK || d == TC_TRICK || d == TC_PLUS_LEFT || d == TC_PLUS_RIGHT) continue;
            float gap;
            if (!k->box && !m->box) gap = dist(k->x, k->y, m->x, m->y) - k->r - m->r;
            else {
                float ax = fabsf(k->x - m->x) - (k->box ? k->hw : k->r) - (m->box ? m->hw : m->r);
                float ay = fabsf(k->y - m->y) - (k->box ? k->hh : k->r) - (m->box ? m->hh : m->r);
                gap = fmaxf(ax, ay);
            }
            if (gap < GAP_MM) return 0;
        }
        /* the flick lanes: no other control's touch area enters them */
        if (t->set.preset == TOUCH_THUMB_ZONES && t->set.attacks == TOUCH_ATTACK_FLICK && c != TC_PEDAL && t->ctl[TC_PEDAL].on) {
            const Ctl *p = &t->ctl[TC_PEDAL];
            float r = (k->box ? fmaxf(k->hw, k->hh) : k->r) * AREA_GROW;
            for (int i = 0; i <= 16; i++) {
                float a = i * PI_F / 8, qx = k->x + (i < 16 ? r * cosf(a) : 0), qy = k->y + (i < 16 ? r * sinf(a) : 0);
                float dx = qx - p->x, dy = qy - p->y, d = hypotf(dx, dy);
                if (d <= p->r || d > FLICK_REACH_MM) continue;
                if (fabsf(dy) <= fabsf(dx) * tanf(FLICK_LANE_DEG * PI_F / 180)) return 0;
            }
        }
    }
    return 1;
}

void touch_open_editor(Touch *t)
{
    t->editor = 1;
    t->editor_tab = EDIT_GROUND;
    t->edit_selected = t->set.preset == TOUCH_CLASSIC ? TC_CROSS : TC_PEDAL;
    drop_race_touches(t);
}

static TouchEdit *edit_of(Touch *t, int c) { return &t->set.edit[t->set.preset == TOUCH_CLASSIC][c]; }

static void editor_button(Touch *t, int b)
{
    switch (b) {
    case EDIT_GROUND: t->editor_tab = EDIT_GROUND; t->edit_selected = t->set.preset == TOUCH_CLASSIC ? TC_CROSS : TC_PEDAL; break;
    case EDIT_AIR:
        if (t->set.preset == TOUCH_THUMB_ZONES) t->editor_tab = EDIT_AIR, t->edit_selected = TC_TRICK;
        break;
    case EDIT_MINUS: case EDIT_PLUS: {
        if (t->edit_selected < 0) break;
        TouchEdit *e = edit_of(t, t->edit_selected), before = *e;
        e->size = clampf(e->size + (b == EDIT_PLUS ? 0.05f : -0.05f), 0.6f, 1.6f);
        lay_out(t, t->set.hud_moved);
        if (!layout_valid(t)) { *e = before; lay_out(t, t->set.hud_moved); }
        break;
    }
    case EDIT_RESET:
        for (int c = 0; c < TC_COUNT; c++) *edit_of(t, c) = (TouchEdit){0, 0, 1};
        lay_out(t, t->set.hud_moved);
        break;
    case EDIT_DONE:
        t->editor = 0;
        t->saved = 1;
        break;
    }
}

static void editor_down(Touch *t, Finger *f)
{
    f->job = JOB_EDIT;
    f->control = -1;
    f->target = -1;
    for (int b = 0; b < EDIT_BUTTONS; b++)
        if (in_box(editor_box(t, b), f->x, f->y)) { f->target = b; return; }
    float best = 1e9f;
    for (int c = 0; c < TC_COUNT; c++) {
        if (!editable(t, c)) continue;
        float out = hit(t, c, f->x, f->y, c == TC_TRICK ? 1.0f : AREA_GROW);
        if (out < best) best = out, f->control = c;
    }
    if (f->control >= 0) {
        t->edit_selected = f->control;
        t->edit_before = *edit_of(t, f->control);
        f->grab_dx = t->ctl[f->control].x - f->x;
        f->grab_dy = t->ctl[f->control].y - f->y;
    }
}

static void editor_move(Touch *t, Finger *f)
{
    if (f->control < 0) return;
    TouchEdit *e = edit_of(t, f->control);
    float want_x = f->x + f->grab_dx, want_y = f->y + f->grab_dy;
    const Ctl *k = &t->ctl[f->control];
    float sx = right_handed(t) ? 1.0f : -1.0f;   /* edits are stored as the right hand has them */
    e->dx += (want_x - k->x) * sx;
    e->dy += want_y - k->y;
    lay_out(t, t->set.hud_moved);
}

static void editor_up(Touch *t, Finger *f)
{
    if (f->target >= 0) {
        if (in_box(editor_box(t, f->target), f->x, f->y)) editor_button(t, f->target);
        return;
    }
    if (f->control >= 0 && !layout_valid(t)) {   /* back to where it was */
        *edit_of(t, f->control) = t->edit_before;
        lay_out(t, t->set.hud_moved);
    }
}

/* ---------------------------------------------------------------- touches */

static void end_finger_job(Touch *t, Finger *f)
{
    switch (f->job) {
    case JOB_STICK:
        if (t->stick_finger == (int)(f - t->f)) t->stick_finger = -1, t->stick_dx = t->stick_dy = 0;
        break;
    case JOB_TRICK:
        if (t->trick.on && t->trick.finger == (int)(f - t->f)) {
            if (!t->cross_was_down && f->t0 >= t->cross_release_wall) t->controls_since_release = 1;
            trick_close(t);
        }
        break;
    case JOB_PEDAL:
        if (f->sent && f->flicked && !paused(t)) {
            /* after a flick that lifts, ✕ stays held until the double-tap window closes */
            int others = 0;
            for (int i = 0; i < TOUCH_FINGERS; i++)
                if (&t->f[i] != f && t->f[i].used && (t->f[i].buttons & PAD_CROSS) && t->f[i].sent) others = 1;
            if (!others) t->hold_cross_until = race_ms(t) + DOUBLE_TAP_FIELDS * FIELD_MS;
        }
        break;
    case JOB_BUTTON:
        if ((f->control == TC_HOP || f->control == TC_SLIDE) && !t->cross_was_down && f->t0 >= t->cross_release_wall)
            t->controls_since_release = 1;
        break;
    default:
        break;
    }
    f->buttons = 0;
    f->sent = 0;
}

/* Where a new touch goes, Thumb zones. */
static void route_thumb(Touch *t, Finger *f)
{
    float x = f->x0, y = f->y0;
    static const int top[] = {TC_CAMERA, TC_PAUSE};
    static const int always[] = {TC_PEDAL, TC_SQUARE, TC_CIRCLE};
    static const int ground[] = {TC_HOP, TC_BRAKE, TC_SLIDE};
    int c = control_at(t, x, y, top, 2);
    if (c < 0) c = control_at(t, x, y, always, t->set.attacks == TOUCH_ATTACK_BUTTONS ? 3 : 1);
    if (c == TC_PEDAL) {
        f->job = JOB_PEDAL;
        f->control = TC_PEDAL;
        f->buttons = PAD_CROSS;
        f->flick_armed = 1;
        cross_press_begins(t, f, 0);
        if (f->sent) buzz(t, TOUCH_BUZZ_TICK);
        return;
    }
    if (c >= 0) {
        f->job = JOB_BUTTON;
        f->control = c;
        if (c == TC_PAUSE) { f->buttons = 0; pause_pressed(t); return; }
        f->buttons = c == TC_CAMERA ? PAD_SELECT : c == TC_SQUARE ? PAD_SQUARE : PAD_CIRCLE;
        if (c == TC_CAMERA) select_press_begins(t, f);
        else f->sent = 1;
        buzz(t, TOUCH_BUZZ_TICK);
        return;
    }
    int in_air_area = x >= t->air_x0 && x <= t->air_x1 && !in_strip(t, x, y);
    if (t->game.air && in_air_area) {
        if (t->air_layout) {
            if (t->trick.on) { f->job = JOB_IGNORED; return; }   /* a second air-area touch is ignored */
            trick_open(t, f);
        } else f->job = JOB_AIR_WAIT;   /* the stick, or the ground button under it if the rider lands first */
        return;
    }
    c = control_at(t, x, y, ground, 3);
    if (c >= 0) {
        f->job = JOB_BUTTON;
        f->control = c;
        f->buttons = c == TC_HOP ? PAD_R2 : c == TC_BRAKE ? PAD_TRIANGLE : PAD_L3;
        f->sent = 1;
        if (c == TC_HOP) t->last_hop_race = race_ms(t);
        buzz(t, TOUCH_BUZZ_TICK);
        return;
    }
    if (in_rect(x, y, t->steer_x0, t->steer_y0, t->steer_x1, t->steer_y1) && !in_strip(t, x, y) && t->stick_finger < 0) {
        f->job = JOB_STICK;
        t->stick_finger = (int)(f - t->f);
        if (t->set.stick_fixed) t->stick_cx = t->ctl[TC_STICK].x, t->stick_cy = t->ctl[TC_STICK].y;
        else t->stick_cx = x, t->stick_cy = y;
        return;
    }
    f->job = JOB_IGNORED;
}

/* Classic: a touch on the face buttons, the + spots first (their 7 mm area wins). */
static uint16_t classic_face_at(const Touch *t, float x, float y, int *control)
{
    for (int c = TC_PLUS_LEFT; c <= TC_PLUS_RIGHT; c++)
        if (t->ctl[c].on && dist(x, y, t->ctl[c].x, t->ctl[c].y) <= 3.5f) {
            *control = c;
            return (uint16_t)(PAD_CROSS | (c == TC_PLUS_LEFT ? PAD_SQUARE : PAD_CIRCLE));
        }
    static const int faces[] = {TC_CROSS, TC_TRIANGLE, TC_CSQUARE, TC_CCIRCLE};
    int c = control_at(t, x, y, faces, 4);
    *control = c;
    switch (c) {
    case TC_CROSS: return PAD_CROSS;
    case TC_TRIANGLE: return PAD_TRIANGLE;
    case TC_CSQUARE: return PAD_SQUARE;
    case TC_CCIRCLE: return PAD_CIRCLE;
    default: return 0;
    }
}

static uint16_t bar_buttons(const Touch *t, int bar, float x)
{
    const Ctl *k = &t->ctl[bar];
    float u = (x - (k->x - k->hw)) / (2 * k->hw);   /* 0..1 across the bar, as drawn */
    if (!right_handed(t)) u = 1 - u;
    int part = u < 0.37f ? 0 : u > 0.63f ? 2 : 1;    /* the middle strip is 26% wide */
    if (bar == TC_LBAR) return part == 0 ? PAD_L2 : part == 2 ? PAD_L1 : (PAD_L1 | PAD_L2);
    return part == 0 ? PAD_R1 : part == 2 ? PAD_R2 : (PAD_R1 | PAD_R2);
}

static void route_classic(Touch *t, Finger *f)
{
    float x = f->x0, y = f->y0;
    static const int others[] = {TC_CAMERA, TC_PAUSE, TC_LBAR, TC_RBAR, TC_L3, TC_R3};
    int face;
    uint16_t b = classic_face_at(t, x, y, &face);
    int c = control_at(t, x, y, others, 6);
    if (b && (c < 0 || hit(t, face, x, y, AREA_GROW) <= hit(t, c, x, y, AREA_GROW) || face == TC_PLUS_LEFT || face == TC_PLUS_RIGHT)) {
        f->job = JOB_BUTTON;
        f->control = face;
        f->buttons = b;
        if (b & PAD_CROSS) cross_press_begins(t, f, 1);
        else f->sent = 1;
        buzz(t, TOUCH_BUZZ_TICK);
        return;
    }
    if (c >= 0) {
        f->job = JOB_BUTTON;
        f->control = c;
        if (c == TC_PAUSE) { f->buttons = 0; pause_pressed(t); return; }
        f->buttons = c == TC_CAMERA ? PAD_SELECT : c == TC_L3 ? PAD_L3 : c == TC_R3 ? PAD_R3 : bar_buttons(t, c, x);
        if (c == TC_CAMERA) select_press_begins(t, f);
        else f->sent = 1;
        buzz(t, TOUCH_BUZZ_TICK);
        return;
    }
    if (in_rect(x, y, t->steer_x0, t->steer_y0, t->steer_x1, t->steer_y1) && !in_strip(t, x, y) && t->stick_finger < 0) {
        f->job = JOB_STICK;
        t->stick_finger = (int)(f - t->f);
        if (t->set.stick_fixed) t->stick_cx = t->ctl[TC_STICK].x, t->stick_cy = t->ctl[TC_STICK].y;
        else t->stick_cx = x, t->stick_cy = y;
        return;
    }
    f->job = JOB_IGNORED;
}

/* Hints that end on a touch. */
static void hint_touch(Touch *t, Finger *f)
{
    if (t->freeze && (f->job == JOB_PEDAL)) t->freeze = 0;
}

void touch_down(Touch *t, long long id, float x, float y, double now)
{
    t->now = now;
    Finger *f = finger_of(t, id);
    if (f) end_finger_job(t, f);   /* its up never came: what it held (the stick, a press) ends first */
    else
        for (int i = 0; i < TOUCH_FINGERS && !f; i++)
            if (!t->f[i].used) f = &t->f[i];
    if (!f) return;
    memset(f, 0, sizeof *f);
    f->used = 1, f->id = id;
    f->x0 = f->x = x, f->y0 = f->y = y, f->t0 = now;
    f->control = -1;
    f->target = -1;
    f->started_race = race_ms(t);
    history_add(f, x, y, now);
    if (t->hidden_by_pad) {   /* the touch that brings the overlay back is never sent */
        t->hidden_by_pad = 0;
        f->job = JOB_IGNORED;
        if (race_on_screen(t)) return;
    }
    if (t->editor) { editor_down(t, f); return; }
    if (!touch_visible(t)) { f->job = JOB_IGNORED; return; }
    if (t->pause != PAUSE_NONE) {   /* only the rows (and Pause, Back) send input */
        static const int pause_button[] = {TC_PAUSE};
        f->job = JOB_PANEL;
        f->target = t->pause == PAUSE_CONTROLS ? 0 : pause_row_at(t, x, y);
        if (t->pause == PAUSE_OPEN && f->target < 0 && control_at(t, x, y, pause_button, 1) == TC_PAUSE) f->target = PAUSE_BUTTON;
        return;
    }
    if (t->panel_open) {   /* the online panel's buttons win over the race areas */
        int b = panel_button_at(t, x, y);
        if (b >= 0) { f->job = JOB_PANEL; f->target = b; return; }
        t->leave_armed_until = 0;   /* any other tap disarms Leave race */
    }
    if (t->set.preset == TOUCH_CLASSIC) route_classic(t, f);
    else route_thumb(t, f);
    hint_touch(t, f);
}

static void stick_follow(Touch *t, Finger *f)
{
    float range = (float)(t->set.stick_range > 0 ? t->set.stick_range : 12);
    float dx = f->x - t->stick_cx, dy = f->y - t->stick_cy, d = hypotf(dx, dy);
    if (d > range && !t->set.stick_fixed) {   /* past its range the centre follows */
        t->stick_cx = f->x - dx / d * range;
        t->stick_cy = f->y - dy / d * range;
        dx = f->x - t->stick_cx, dy = f->y - t->stick_cy, d = range;
    }
    if (d > range) dx *= range / d, dy *= range / d;
    t->stick_dx = dx / range, t->stick_dy = dy / range;
}

static void pedal_follow(Touch *t, Finger *f)
{
    const Ctl *p = &t->ctl[TC_PEDAL];
    float dx = f->x - p->x, dy = f->y - p->y, d = hypotf(dx, dy);
    if (d <= p->r) { f->flick_armed = 1; return; }   /* re-arms back inside the rim */
    if (t->set.attacks != TOUCH_ATTACK_FLICK || !f->flick_armed) return;
    if (d < p->r + FLICK_PAST_RIM_MM || d > FLICK_REACH_MM) return;
    if (fabsf(dy) > fabsf(dx) * tanf(FLICK_LANE_DEG * PI_F / 180)) return;
    if (speed(f, t->now) < FLICK_SPEED) return;
    pulse(t, dx < 0 ? PAD_SQUARE : PAD_CIRCLE, PRESS_FIELDS);   /* ◻ hits left, ○ right, whatever the hand */
    f->flick_armed = 0;
    f->flicked = 1;
    buzz(t, TOUCH_BUZZ_TICK);
}

void touch_move(Touch *t, long long id, float x, float y, double now)
{
    t->now = now;
    Finger *f = finger_of(t, id);
    if (!f) return;
    f->x = x, f->y = y;
    history_add(f, x, y, now);
    switch (f->job) {
    case JOB_STICK: stick_follow(t, f); break;
    case JOB_PEDAL: pedal_follow(t, f); break;
    case JOB_TRICK: if (t->trick.on && t->trick.finger == (int)(f - t->f)) trick_step(t, f); break;
    case JOB_EDIT: editor_move(t, f); break;
    case JOB_BUTTON:
        if (t->set.preset == TOUCH_CLASSIC && (f->buttons & PAD_CROSS) && !paused(t)) {
            /* Classic's ✕ may roll onto a + spot or ◻ / ○: that press is added, ✕ stays */
            int c;
            uint16_t b = classic_face_at(t, x, y, &c);
            uint16_t extra = (b & (PAD_SQUARE | PAD_CIRCLE));
            if (extra && !(f->buttons & extra)) buzz(t, TOUCH_BUZZ_TICK);
            f->buttons = (uint16_t)(PAD_CROSS | extra);
        }
        break;
    default:
        break;
    }
}

void touch_up(Touch *t, long long id, float x, float y, double now)
{
    t->now = now;
    Finger *f = finger_of(t, id);
    if (!f) return;
    f->x = x, f->y = y;
    if (f->job == JOB_PANEL) {
        if (t->pause != PAUSE_NONE) {   /* row taps fire on lift, on the row they started on */
            if (t->pause == PAUSE_CONTROLS) pause_row_tapped(t, 0);
            else if (f->target == PAUSE_BUTTON) pause_row_tapped(t, ROW_RESUME);   /* Pause again: Resume */
            else if (f->target >= 0 && pause_row_at(t, x, y) == f->target) pause_row_tapped(t, f->target);
        } else if (t->panel_open && f->target >= 0 && panel_button_at(t, x, y) == f->target) panel_tapped(t, f->target);
    } else if (f->job == JOB_EDIT) editor_up(t, f);
    else if (f->job == JOB_TRICK && t->freeze && !t->trick.held) t->freeze = 0;   /* a lift without a push */
    else if (f->job == JOB_AIR_WAIT && !paused(t)) {
        /* a tap that ended in the air layout's wait: the ground layout was still on screen, so it
           was that button's (the flag also comes over bumps); a short press */
        static const int ground[] = {TC_HOP, TC_BRAKE, TC_SLIDE};
        int c = control_at(t, f->x0, f->y0, ground, 3);
        if (c >= 0) pulse(t, c == TC_HOP ? PAD_R2 : c == TC_BRAKE ? PAD_TRIANGLE : PAD_L3, PRESS_FIELDS);
        if (c == TC_HOP) t->last_hop_race = race_ms(t);
    } else if (f->job == JOB_BUTTON && f->sent && !f->fields && !paused(t)) {
        /* a tap shorter than a field never reached the pad: the game still sees it (✕ and SELECT
           keep their windows' bookkeeping, so not those) */
        uint16_t b = (uint16_t)(f->buttons & ~(PAD_CROSS | PAD_SELECT));
        if (b) pulse(t, b, PRESS_FIELDS);
    }
    end_finger_job(t, f);
    f->used = 0;
}

void touch_cancel_all(Touch *t, double now)
{
    t->now = now;
    for (int i = 0; i < TOUCH_FINGERS; i++)
        if (t->f[i].used) { end_finger_job(t, &t->f[i]); t->f[i].used = 0; }
    t->hold_cross_until = 0;
}

void touch_back(Touch *t, double now)
{
    t->now = now;
    if (t->freeze) { t->freeze = 0; return; }
    if (t->editor) { t->editor = 0; t->saved = 1; return; }
    if (t->pause == PAUSE_CONTROLS) { t->pause = PAUSE_OPEN; return; }   /* the overlay's own page */
    /* the question's No; while the presses that opened it still go, nothing (as a tap): the game
       has not shown it yet, so the overlay stays on it and the next Back answers */
    if (t->pause == PAUSE_CONFIRM) { pause_row_tapped(t, CONFIRM_NO); return; }
    if (t->pause == PAUSE_OPEN) { pause_row_tapped(t, ROW_RESUME); return; }
    if (t->pause == PAUSE_ASKED) return;
    if (race_on_screen(t)) { pause_pressed(t); return; }
    if (!seq_busy(t)) seq_add(t, PAD_TRIANGLE);   /* in menus Back is △ */
}

void touch_pad_used(Touch *t, double now)
{
    t->now = now;
    if (t->set.show_with_pad || t->hidden_by_pad || t->editor) return;
    t->hidden_by_pad = 1;
    drop_race_touches(t);
    t->panel_open = 0;
}

void touch_pad_lost(Touch *t, double now)
{
    t->now = now;
    if (!race_on_screen(t)) return;
    t->hidden_by_pad = 0;
    if (!t->game.online && t->pause == PAUSE_NONE) pause_open(t);   /* the menu-open chain starts with its own START */
}

void touch_tilt(Touch *t, int known, float degrees) { t->tilt_known = known; t->tilt_deg = known ? degrees : 0; }

/* ---------------------------------------------------------------- the host's queue */

static int finger_event(int kind) { return kind == TOUCH_EV_DOWN || kind == TOUCH_EV_MOVE || kind == TOUCH_EV_UP; }

/* A finger's moves in a row (none of its other events, and no cancel, between them): only the
   last stays, so the finger still ends up where it was. */
static void queue_merge_moves(TouchQueue *q)
{
    int kept = 0;
    for (int i = 0; i < q->count; i++) {
        const TouchEvent *e = &q->ev[i];
        int later = 0;
        if (e->kind == TOUCH_EV_MOVE)
            for (int j = i + 1; j < q->count; j++) {
                const TouchEvent *n = &q->ev[j];
                if (n->kind == TOUCH_EV_CANCEL) break;
                if (n->id != e->id || !finger_event(n->kind)) continue;
                later = n->kind == TOUCH_EV_MOVE;
                break;
            }
        if (later) q->merged++;
        else q->ev[kept++] = *e;
    }
    q->count = kept;
}

/* The last resort, the queue full without a move to merge: every finger is let go (the player
   lifts and touches again); the Backs and controller losses stay, then the new event. */
static void queue_cancel_fingers(TouchQueue *q, const TouchEvent *e)
{
    int n = 0;
    TouchEvent cancel = {TOUCH_EV_CANCEL, -1, 0, 0, q->count ? q->ev[0].ms : e->ms};
    TouchEvent keep[TOUCH_QUEUE];
    for (int i = 0; i < q->count && n < TOUCH_QUEUE - 2; i++)
        if (q->ev[i].kind == TOUCH_EV_BACK || q->ev[i].kind == TOUCH_EV_PAD_LOST) keep[n++] = q->ev[i];
    q->ev[0] = cancel;
    memcpy(q->ev + 1, keep, sizeof *keep * (size_t)n);
    q->count = 1 + n;
    if (e->kind != TOUCH_EV_UP && e->kind != TOUCH_EV_CANCEL) q->ev[q->count++] = *e;   /* its finger's down is gone */
    q->cancelled++;
}

void touch_queue_push(TouchQueue *q, int kind, long long id, float x, float y, double ms)
{
    TouchEvent e = {kind, id, x, y, ms};
    if (kind == TOUCH_EV_PAD_USED)   /* still hidden by an earlier one (only a down or Back changes that): the same */
        for (int i = q->count - 1; i >= 0; i--) {
            int k = q->ev[i].kind;
            if (k == TOUCH_EV_PAD_USED) return;
            if (k != TOUCH_EV_MOVE && k != TOUCH_EV_UP) break;
        }
    int soft = kind == TOUCH_EV_MOVE || kind == TOUCH_EV_PAD_USED;
    int room = soft ? TOUCH_QUEUE - TOUCH_QUEUE_RESERVE : TOUCH_QUEUE;
    if (q->count >= room) queue_merge_moves(q);
    if (q->count < room) { q->ev[q->count++] = e; return; }
    if (kind == TOUCH_EV_MOVE) {   /* the finger's last queued move takes the new place */
        for (int i = q->count - 1; i >= 0; i--) {
            TouchEvent *o = &q->ev[i];
            if (o->kind == TOUCH_EV_CANCEL) break;
            if (o->id != id || !finger_event(o->kind)) continue;
            if (o->kind == TOUCH_EV_MOVE) { o->x = x, o->y = y, o->ms = ms; q->merged++; }
            break;
        }
        return;   /* else it goes: the finger's up says where it ended */
    }
    if (soft) return;   /* a controller's use: the next one hides */
    queue_cancel_fingers(q, &e);
}

void touch_event(Touch *t, const TouchEvent *e, float x, float y)
{
    switch (e->kind) {
    case TOUCH_EV_DOWN: touch_down(t, e->id, x, y, e->ms); break;
    case TOUCH_EV_MOVE: touch_move(t, e->id, x, y, e->ms); break;
    case TOUCH_EV_UP: touch_up(t, e->id, x, y, e->ms); break;
    case TOUCH_EV_CANCEL: touch_cancel_all(t, e->ms); break;
    case TOUCH_EV_BACK: touch_back(t, e->ms); break;
    case TOUCH_EV_PAD_USED: touch_pad_used(t, e->ms); break;
    case TOUCH_EV_PAD_LOST: touch_pad_lost(t, e->ms); break;
    }
}

/* ---------------------------------------------------------------- the field */

static void air_layout_step(Touch *t)
{
    const TouchGame *g = &t->game;
    if (g->air && !t->air_was) t->air_since = g->race_ms;
    if (g->race_ms < t->air_since) t->air_since = g->race_ms;   /* a restart */
    t->air_was = g->air;
    int wait = t->set.air_wait_ms > 0 ? t->set.air_wait_ms : 150;
    int layout = t->set.preset == TOUCH_THUMB_ZONES && g->air && g->race_ms - t->air_since >= wait - 0.5;
    if (layout && !t->air_layout) {
        /* air-area touches in the wait become the trick stick, their movement so far counting */
        for (int i = 0; i < TOUCH_FINGERS; i++) {
            Finger *f = &t->f[i];
            if (!f->used || f->job != JOB_AIR_WAIT) continue;
            if (t->trick.on) { f->job = JOB_IGNORED; continue; }
            trick_open(t, f);
            t->trick.opened = t->now;
            for (int h = 0; h < f->hn; h++) {
                float x = f->x, y = f->y;
                f->x = f->hx[h], f->y = f->hy[h];
                trick_step(t, f);
                f->x = x, f->y = y;
            }
            trick_step(t, f);
        }
    }
    if (!g->air) {
        /* landed: the held trick goes now (a pad's "held when you land"); waiting touches become
           the ground button under them */
        if (t->trick.on && !t->trick.landed) {
            trick_send(t);
            t->trick.landed = 1;
            if (!(t->set.hints_shown & TOUCH_HINT_TRICK) && t->trick_hint_repeat == 1) t->trick_hint_repeat = 2;
        }
        for (int i = 0; i < TOUCH_FINGERS; i++) {
            Finger *f = &t->f[i];
            if (!f->used || f->job != JOB_AIR_WAIT) continue;
            static const int ground[] = {TC_HOP, TC_BRAKE, TC_SLIDE};
            int c = control_at(t, f->x0, f->y0, ground, 3);
            if (c < 0) { f->job = JOB_IGNORED; continue; }
            f->job = JOB_BUTTON, f->control = c, f->sent = 1;
            f->buttons = c == TC_HOP ? PAD_R2 : c == TC_BRAKE ? PAD_TRIANGLE : PAD_L3;
        }
    }
    t->air_layout = layout;
}

/* ✕ in the output: bookkeeping of its releases for the double-tap rules. */
static int cross_output(const Touch *t)
{
    if (paused(t)) return 0;
    if (t->hold_cross_until > 0) return 1;
    for (int i = 0; i < TOUCH_FINGERS; i++) {
        const Finger *f = &t->f[i];
        if (f->used && f->sent && (f->buttons & PAD_CROSS) && (f->job == JOB_PEDAL || f->job == JOB_BUTTON)) return 1;
    }
    return 0;
}

static void cross_track(Touch *t)
{
    if (t->hold_cross_until > 0 && race_ms(t) >= t->hold_cross_until) {
        t->hold_cross_until = 0;
        int down = cross_output(t);
        if (!down && t->cross_was_down) {   /* the overlay let ✕ up */
            t->cross_was_down = 0;
            t->cross_released = race_ms(t);
            t->cross_released_by_overlay = 1;
            t->controls_since_release = 0;
            t->cross_release_wall = t->now;
        }
    }
    int down = cross_output(t);
    if (down && !t->cross_was_down) t->cross_released_by_overlay = 0;
    if (!down && t->cross_was_down && !paused(t)) {
        t->cross_released = race_ms(t);
        t->cross_released_by_overlay = 0;
        t->controls_since_release = 0;
        t->cross_release_wall = t->now;
    }
    t->cross_was_down = down;
    int select = 0;
    for (int i = 0; i < TOUCH_FINGERS; i++)
        if (t->f[i].used && t->f[i].sent && (t->f[i].buttons & PAD_SELECT)) select = 1;
    if (!select && t->select_was_down && !paused(t)) t->select_released = race_ms(t);
    t->select_was_down = select;
}

static void pause_step(Touch *t)
{
    const TouchGame *g = &t->game;
    if (t->pause == PAUSE_ASKED && !seq_busy(t)) {
        /* the menu is open once the race clock stops (its own START when it wasn't moving) */
        if (!t->pause_from_moving || !g->clock_moving) t->pause = PAUSE_OPEN;
        else if (t->now - t->pause_asked_at > 500) race_resumes(t);   /* START was ignored */
    } else if ((t->pause == PAUSE_OPEN || t->pause == PAUSE_CONTROLS) && g->clock_moving && !seq_busy(t)) {
        race_resumes(t);   /* closed some other way (a controller) */
    }
    if (g->online && t->pause != PAUSE_NONE) race_resumes(t);
    if (!g->online) t->panel_open = 0;
    if (t->leave_armed_until && t->now > t->leave_armed_until) t->leave_armed_until = 0;
}

static void tilt_step(Touch *t)
{
    /* tilt steers on the ground, once the sensor has answered (none, or not yet: the stick) */
    float want = t->set.tilt && t->tilt_known && !t->game.air ? 1.0f : 0.0f;
    double dt = t->tilt_mix_at > 0 ? t->now - t->tilt_mix_at : 0;
    t->tilt_mix_at = t->now;
    float step = (float)(dt / 100.0);   /* the hand-over blends over 100 ms */
    if (t->tilt_mix < want) t->tilt_mix = fminf(want, t->tilt_mix + step);
    else t->tilt_mix = fmaxf(want, t->tilt_mix - step);
}

static void hints_step(Touch *t)
{
    const TouchGame *g = &t->game;
    if (g->online || t->set.preset != TOUCH_THUMB_ZONES || !touch_visible(t) || t->editor) { t->hint = 0; return; }
    double race = g->race_ms;
    if (t->race_new) { t->last_hop_race = race; t->slide_far_since = 0; t->race_new = 0; }
    if (t->hint && t->hint != TOUCH_HINT_TRICK && t->now > t->hint_until) t->hint = 0;
    /* the trick-stick hint goes with its hold when no push ended it (5 s, the landing), and with
       the air layout; a push ends it in touch_update */
    if (t->hint == TOUCH_HINT_TRICK && !t->freeze && !t->freeze_at && (t->now > t->hint_until || !t->air_layout)) t->hint = 0;
    unsigned shown = t->set.hints_shown;
    #define SHOW(h, ms) do { t->hint = (h); t->hint_until = t->now + (ms); t->set.hints_shown |= (h); t->saved = 1; } while (0)
    if (t->hint) {
        if (t->hint == TOUCH_HINT_HOP && race - t->last_hop_race < 1000) t->hint = 0;
        return;
    }
    if (!(shown & TOUCH_HINT_PEDAL) && g->clock_moving && race < 4000) SHOW(TOUCH_HINT_PEDAL, 4000 - race);
    else if (!(shown & TOUCH_HINT_ATTACK) && race >= 20000 && race < 25000) SHOW(TOUCH_HINT_ATTACK, 4000);
    else if (!(shown & TOUCH_HINT_HOP) && g->clock_moving && race - t->last_hop_race >= 10000) SHOW(TOUCH_HINT_HOP, 6000);
    else if (!(shown & TOUCH_HINT_SPRINT) && race >= 60000 && race < 65000) SHOW(TOUCH_HINT_SPRINT, 4000);
    else if (!(shown & TOUCH_HINT_SLIDE) && t->slide_far_since > 0 && t->now - t->slide_far_since > 1000) SHOW(TOUCH_HINT_SLIDE, 4000);
    else if (!(shown & TOUCH_HINT_TRICK) && t->air_layout && g->air && race - t->air_since >= 400 && !t->trick.held) {
        SHOW(TOUCH_HINT_TRICK, 5000);
        /* the port stops advancing frames until the first push: a few VBLANKs later, so the pictures
           with the tip are on screen while it holds */
        t->freeze_at = g->vsync + 3;
        if (!t->freeze_at) t->freeze_at = 1;
        t->trick_hint_repeat = 1;
    } else if (t->trick_hint_repeat == 2 && t->trick.on) {   /* the thumb was still on the stick at touchdown: once more */
        t->trick_hint_repeat = 3;
        t->hint = TOUCH_HINT_TRICK, t->hint_until = t->now + 3000;
    }
    #undef SHOW
    float far = hypotf(t->stick_dx, t->stick_dy);
    if (t->stick_finger >= 0 && far > 0.8f) { if (!t->slide_far_since) t->slide_far_since = t->now; }
    else t->slide_far_since = 0;
}

/* The race clock went back (Restart race, a new race): every time kept in race ms moves with it,
   so the windows and the presses waiting on them keep what they had left (a press the resume
   guard holds back would otherwise wait for the old race's time). */
static void race_clock_back(Touch *t, double delta)
{
    #define SHIFT(v) do { if ((v) > -1e8) (v) += delta; } while (0)
    SHIFT(t->cross_released);
    SHIFT(t->select_released);
    SHIFT(t->last_hop_race);
    SHIFT(t->air_since);
    SHIFT(t->hint_race_start);
    if (t->hold_cross_until > 0) t->hold_cross_until = fmax(t->hold_cross_until + delta, 1);
    for (int i = 0; i < TOUCH_FINGERS; i++) {
        Finger *f = &t->f[i];
        if (!f->used) continue;
        SHIFT(f->started_race);
        if (f->due_race > 0) f->due_race = fmax(f->due_race + delta, 1);
    }
    #undef SHIFT
}

void touch_update(Touch *t, const TouchGame *game, double now)
{
    t->now = now;
    if (t->have_game && game->race_ms < t->game.race_ms) race_clock_back(t, game->race_ms - t->game.race_ms);
    int was_on = t->race_was_on;
    t->game = *game;
    t->have_game = 1;
    if (game->race_hud) {
        if (!was_on) t->race_new = 1;
        t->hud_seen = now;
    }
    t->race_was_on = race_on_screen(t);
    if (was_on && !t->race_was_on && t->pause == PAUSE_NONE) {   /* the race is gone */
        drop_race_touches(t);
        t->panel_open = 0;
        t->hold_cross_until = 0;
        t->hint = 0;
    }
    seq_advance(t);
    for (int i = 0; i < TOUCH_FINGERS; i++)
        if (t->f[i].used) t->f[i].fields++;
    for (int i = 0; i < 16; i++)
        if (t->pulse_buttons[i] && (int32_t)(game->vsync - t->pulse_until[i]) >= 0) t->pulse_buttons[i] = 0;
    if (t->freeze_at && (int32_t)(game->vsync - t->freeze_at) >= 0) {
        t->freeze_at = 0;
        if (t->hint == TOUCH_HINT_TRICK && game->air) t->freeze = 1, t->freeze_since = now;
        else if (t->hint == TOUCH_HINT_TRICK) {
            /* landed before the hold began (a hop just over 400 ms): the tip was barely on screen,
               so it waits for a longer air */
            t->hint = 0;
            t->set.hints_shown &= ~(unsigned)TOUCH_HINT_TRICK;
            t->saved = 1;
        }
    }
    if (t->freeze) {
        int pushed = t->trick.on && t->trick.held;
        if (pushed || now - t->freeze_since > 5000 || !game->air || !race_on_screen(t)) t->freeze = 0;
        if (pushed) t->hint = 0;
    }
    pause_step(t);
    air_layout_step(t);
    if (t->trick.on) {
        Finger *f = &t->f[t->trick.finger];
        trick_step(t, f);
        trick_tick(t);
    }
    due_presses(t);
    cross_track(t);
    tilt_step(t);
    hints_step(t);
    if (t->label[0] && now > t->label_until && t->set.labels != TOUCH_LABELS_ALWAYS) t->label[0] = 0;
}

static uint8_t axis(float v) { return (uint8_t)clampf(128.0f + v * 127.0f, 0, 255); }

void touch_pad(const Touch *t, uint16_t *buttons, uint8_t *lx, uint8_t *ly, uint8_t *rx, uint8_t *ry)
{
    uint16_t b = 0;
    float sx = 0, sy = 0;
    (void)rx; (void)ry;
    for (int i = 0; i < 16; i++) b |= t->pulse_buttons[i];
    if (seq_busy(t) && t->seq_started) b |= t->seq[t->seq_at].buttons;
    if (!paused(t) && touch_visible(t)) {
        for (int i = 0; i < TOUCH_FINGERS; i++) {
            const Finger *f = &t->f[i];
            if (f->used && f->sent && (f->job == JOB_PEDAL || f->job == JOB_BUTTON)) b |= f->buttons;
        }
        if (t->hold_cross_until > 0) b |= PAD_CROSS;
        if (t->trick.on) b |= (uint16_t)(t->trick.sent | (t->trick.triangle ? PAD_TRIANGLE : 0));
        if (t->stick_finger >= 0) sx = t->stick_dx, sy = t->stick_dy;
        if (t->set.tilt && t->tilt_mix > 0) {   /* tilt steers left/right on the ground; the stick's left/right is ignored there */
            float full = 40.0f - 30.0f * clampf((float)t->set.tilt_strength, 0, 100) / 100.0f;
            float tilt = clampf((t->tilt_deg - t->set.tilt_centre) / full, -1, 1);
            sx = sx * (1 - t->tilt_mix) + tilt * t->tilt_mix;
        }
    }
    *buttons |= b;
    if (sx != 0 || sy != 0) { *lx = axis(sx); *ly = axis(sy); }
}

int touch_take_buzz(Touch *t) { int b = t->buzz; t->buzz = 0; return b; }
int touch_take_ask(Touch *t) { int a = t->ask; t->ask = 0; return a; }
int touch_take_saved(Touch *t, TouchSettings *out)
{
    if (!t->saved) return 0;
    t->saved = 0;
    *out = t->set;
    return 1;
}

/* ---------------------------------------------------------------- set-up */

Touch *touch_new(void)
{
    Touch *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    touch_settings_default(&t->set);
    t->screen = (TouchScreen){REF_W, REF_H, 0, 0, 0, 0, 0, 0, 0, 0};
    t->stick_finger = -1;
    t->cross_released = t->select_released = -1e9;
    t->cross_release_wall = -1e9;
    t->edit_selected = -1;
    lay_out(t, 1);
    return t;
}

void touch_free(Touch *t) { free(t); }

void touch_configure(Touch *t, const TouchSettings *s, const TouchScreen *screen)
{
    int preset_changed = s->preset != t->set.preset;
    int hints = t->set.hints_shown;
    t->set = *s;
    if (t->saved) t->set.hints_shown |= hints;   /* a hint shown since the last save isn't lost */
    if (screen && screen->width > 10 && screen->height > 10) t->screen = *screen;
    if (preset_changed) drop_race_touches(t);
    lay_out(t, t->set.hud_moved);
}

const TouchSettings *touch_settings(const Touch *t) { return &t->set; }

/* ---------------------------------------------------------------- the overlay's shapes */

typedef struct { TouchShape *out; int n, max; float alpha; } Draw;

static uint32_t rgba(int r, int g, int b, float a)
{
    int A = (int)clampf(a * 255.0f + 0.5f, 0, 255);
    return (uint32_t)r << 24 | (uint32_t)g << 16 | (uint32_t)b << 8 | (uint32_t)A;
}

static void put(Draw *d, int kind, float x, float y, float a, float b, float c, float e, uint32_t fill, uint32_t line)
{
    if (d->n >= d->max) return;
    d->out[d->n++] = (TouchShape){kind, x, y, a, b, c, e, fill, line};
}

static void text(Draw *d, float x, float y, float h, int align, const char *s, uint32_t colour)
{
    float adv = h * 0.9f, w = adv * (float)strlen(s);
    float left = align < 0 ? x : align > 0 ? x - w : x - w / 2;
    for (int i = 0; s[i]; i++)
        if (s[i] != ' ') put(d, TS_CHAR, left + adv * (i + 0.5f), y, h, 0, (float)(unsigned char)s[i], 0, colour, 0);
}

/* A round control: the dimmed fill, the dark outer and white inner rim (readable on snow). */
static void button(Draw *d, const Ctl *k, int pressed, float dim, int symbol, const char *label)
{
    float a = d->alpha * dim;
    uint32_t fill = pressed ? rgba(166, 227, 154, 0.55f * a) : rgba(10, 14, 18, 0.45f * a);
    put(d, TS_DISC, k->x, k->y, k->r, 0, 0, 0, fill, rgba(255, 255, 255, 0.88f * a));
    float g = k->r * (label && *label ? 0.42f : 0.55f), gy = k->y - (label && *label ? k->r * 0.18f : 0);
    if (symbol) put(d, symbol, k->x, gy, g, 0, 0, 0, 0, rgba(255, 255, 255, 0.95f * a));
    if (label && *label) text(d, k->x, k->y + k->r * (symbol ? 0.45f : 0), fminf(k->r * 0.32f, 3.2f), 0, label, rgba(255, 255, 255, 0.95f * a));
}

static void box_button(Draw *d, float x, float y, float hw, float hh, int pressed, float alpha, const char *label, float th)
{
    uint32_t fill = pressed ? rgba(166, 227, 154, 0.55f * alpha) : rgba(10, 14, 18, 0.7f * alpha);
    put(d, TS_RECT, x, y, hw, hh, fminf(hw, hh) * 0.44f, 0, fill, rgba(255, 255, 255, 0.85f * alpha));
    if (label && *label) {
        float fit = (2 * hw - 1.6f) / (0.9f * (float)strlen(label));   /* inside the rim, with a margin */
        text(d, x, y, fminf(th, fit), 0, label, rgba(255, 255, 255, alpha));
    }
}

static int pressed_control(const Touch *t, int c)
{
    for (int i = 0; i < TOUCH_FINGERS; i++)
        if (t->f[i].used && t->f[i].control == c && (t->f[i].job == JOB_BUTTON || t->f[i].job == JOB_PEDAL)) return 1;
    return 0;
}

static void draw_top_buttons(const Touch *t, Draw *d, int classic)
{
    for (int c = TC_CAMERA; c <= TC_PAUSE; c++) {
        const Ctl *k = &t->ctl[c];
        if (!k->on) continue;
        box_button(d, k->x, k->y, k->hw, k->hh, pressed_control(t, c), d->alpha, classic ? (c == TC_CAMERA ? "SELECT" : "START") : "",
                   fminf(k->hh * 0.5f, 2.2f));
        if (classic) continue;
        uint32_t w = rgba(255, 255, 255, 0.95f * d->alpha);
        if (c == TC_PAUSE) {
            put(d, TS_RECT, k->x - k->hw * 0.25f, k->y, k->hw * 0.12f, k->hh * 0.42f, 0.2f, 0, w, 0);
            put(d, TS_RECT, k->x + k->hw * 0.25f, k->y, k->hw * 0.12f, k->hh * 0.42f, 0.2f, 0, w, 0);
        } else {   /* a camera: body and lens */
            put(d, TS_RECT, k->x, k->y + k->hh * 0.05f, k->hw * 0.55f, k->hh * 0.36f, 0.5f, 0, 0, w);
            put(d, TS_RING, k->x, k->y + k->hh * 0.05f, k->hh * 0.2f, 0.35f, 0, 0, 0, w);
        }
    }
}

static void draw_stick(const Touch *t, Draw *d)
{
    float range = (float)(t->set.stick_range > 0 ? t->set.stick_range : 12);
    float ring = fmaxf(range, t->ctl[TC_STICK].r * 0.8f);
    uint32_t line = rgba(255, 255, 255, 0.5f * d->alpha), knob = rgba(255, 255, 255, 0.3f * d->alpha);
    float cx = t->ctl[TC_STICK].x, cy = t->ctl[TC_STICK].y, kx = cx, ky = cy;
    if (t->stick_finger >= 0) {
        cx = t->stick_cx, cy = t->stick_cy;
        kx = cx + t->stick_dx * range, ky = cy + t->stick_dy * range;
        line = rgba(255, 255, 255, 0.8f * d->alpha);
    }
    put(d, TS_DISC, cx, cy, ring, 0, 0, 0, rgba(10, 14, 18, 0.16f * d->alpha), line);
    put(d, TS_DISC, kx, ky, ring * 0.44f, 0, 0, 0, knob, rgba(255, 255, 255, 0.85f * d->alpha));
}

static void draw_trick(const Touch *t, Draw *d)
{
    const Trick *k = &t->trick;
    float sc = trick_scale(t), r_in = 7 * sc, r_out = 14 * sc, r_ring = 15.5f * sc;
    float cx, cy, a = d->alpha;
    if (k->on) cx = k->cx, cy = k->cy;
    else {
        cx = t->ctl[TC_TRICK].x, cy = t->ctl[TC_TRICK].y;
        a *= 0.3f;   /* the faint ring: where it opens by default */
    }
    put(d, TS_DISC, cx, cy, r_ring, 0, 0, 0, rgba(10, 14, 18, 0.25f * a), rgba(255, 255, 255, 0.8f * a));
    put(d, TS_RING, cx, cy, r_in, 0.25f, 0, 0, 0, rgba(255, 255, 255, 0.45f * a));
    put(d, TS_RING, cx, cy, r_out, 0.25f, 0, 0, 0, rgba(255, 255, 255, 0.45f * a));
    for (int s = 0; s < 8; s++) {
        float start = s * PI_F / 4 - PI_F / 8, end = start + PI_F / 4;
        int latched = k->on && (k->sectors & (1u << s));
        int lit = k->on && k->preview == s;
        if (latched) put(d, TS_SECTOR, cx, cy, r_in, r_out, start, end, rgba(150, 220, 140, 0.62f * a), 0);
        else if (lit) put(d, TS_SECTOR, cx, cy, r_in, r_out, start, end, rgba(150, 220, 140, 0.28f * a), 0);
        if (k->on && k->triangle && latched) put(d, TS_SECTOR, cx, cy, r_out, r_ring, start, end, rgba(255, 209, 102, 0.7f * a), 0);
        /* the seams */
        put(d, TS_SECTOR, cx, cy, r_in, r_ring, start - 0.01f, start + 0.01f, rgba(255, 255, 255, 0.35f * a), 0);
    }
    /* the shoulders at the diagonals; Minimal after 20 tricks unless labels are Always */
    int minimal = t->tricks_sent >= 20 && t->set.labels != TOUCH_LABELS_ALWAYS;
    if (!minimal) {
        static const char *const names[4] = {"R1", "L1", "L2", "R2"};
        for (int i = 0; i < 4; i++) {
            float ang = PI_F / 4 + i * PI_F / 2, r = (r_in + r_out) / 2;
            text(d, cx + cosf(ang) * r, cy - sinf(ang) * r, 2.2f * sc, 0, names[i], rgba(255, 255, 255, 0.85f * a));
        }
    }
}

static void draw_thumb(const Touch *t, Draw *d)
{
    int air = t->air_layout;
    draw_stick(t, d);
    const Ctl *p = &t->ctl[TC_PEDAL];
    button(d, p, pressed_control(t, TC_PEDAL), air ? 0.45f : 1.0f, TS_CROSS, "PEDAL");
    if (!air && t->set.attacks == TOUCH_ATTACK_FLICK) {   /* the flick hints: not buttons */
        float off = 16.0f * t->bs, s = 2.6f;
        uint32_t w = rgba(255, 255, 255, 0.92f * d->alpha);
        put(d, TS_CHEVRON, p->x - off - s * 1.1f, p->y, s * 0.8f, 0, PI_F, 0, 0, w);
        put(d, TS_SQUARE, p->x - off + s * 0.4f, p->y, s, 0, 0, 0, 0, w);
        put(d, TS_CIRCLE, p->x + off - s * 0.4f, p->y, s, 0, 0, 0, 0, w);
        put(d, TS_CHEVRON, p->x + off + s * 1.1f, p->y, s * 0.8f, 0, 0, 0, 0, w);
    }
    if (t->set.attacks == TOUCH_ATTACK_BUTTONS) {
        button(d, &t->ctl[TC_SQUARE], pressed_control(t, TC_SQUARE), 1, TS_SQUARE, "");
        button(d, &t->ctl[TC_CIRCLE], pressed_control(t, TC_CIRCLE), 1, TS_CIRCLE, "");
    }
    if (!air) {
        button(d, &t->ctl[TC_HOP], pressed_control(t, TC_HOP), 1, 0, "HOP");
        button(d, &t->ctl[TC_BRAKE], pressed_control(t, TC_BRAKE), 1, TS_TRIANGLE, "BRAKE");
        button(d, &t->ctl[TC_SLIDE], pressed_control(t, TC_SLIDE), 1, 0, "SLIDE");
    } else draw_trick(t, d);
    draw_top_buttons(t, d, 0);
}

static void draw_bar(const Touch *t, Draw *d, int bar)
{
    const Ctl *k = &t->ctl[bar];
    uint32_t line = rgba(255, 255, 255, 0.85f * d->alpha);
    int pressed = pressed_control(t, bar);
    put(d, TS_RECT, k->x, k->y, k->hw, k->hh, k->hh * 0.44f, 0, pressed ? rgba(166, 227, 154, 0.5f * d->alpha) : rgba(10, 14, 18, 0.6f * d->alpha), line);
    put(d, TS_RECT, k->x, k->y, k->hw * 0.26f, k->hh, 0, 0, rgba(255, 255, 255, 0.14f * d->alpha), rgba(255, 255, 255, 0.5f * d->alpha));
    const char *a = bar == TC_LBAR ? "L2" : "R1", *m = bar == TC_LBAR ? "L1+L2" : "R1+R2", *b = bar == TC_LBAR ? "L1" : "R2";
    if (!right_handed(t)) { const char *x = a; a = b; b = x; }
    float th = fminf(k->hh * 0.55f, 3.0f);
    text(d, k->x - k->hw * 0.63f, k->y, th, 0, a, line);
    text(d, k->x, k->y, th * 0.6f, 0, m, line);
    text(d, k->x + k->hw * 0.63f, k->y, th, 0, b, line);
}

static void draw_classic(const Touch *t, Draw *d)
{
    draw_stick(t, d);
    draw_bar(t, d, TC_LBAR);
    draw_bar(t, d, TC_RBAR);
    button(d, &t->ctl[TC_L3], pressed_control(t, TC_L3), 1, 0, "L3");
    button(d, &t->ctl[TC_R3], pressed_control(t, TC_R3), 1, 0, "R3");
    button(d, &t->ctl[TC_TRIANGLE], pressed_control(t, TC_TRIANGLE), 1, TS_TRIANGLE, "");
    button(d, &t->ctl[TC_CSQUARE], pressed_control(t, TC_CSQUARE), 1, TS_SQUARE, "");
    button(d, &t->ctl[TC_CCIRCLE], pressed_control(t, TC_CCIRCLE), 1, TS_CIRCLE, "");
    button(d, &t->ctl[TC_CROSS], pressed_control(t, TC_CROSS), 1, TS_CROSS, "");
    for (int c = TC_PLUS_LEFT; c <= TC_PLUS_RIGHT; c++) {
        const Ctl *k = &t->ctl[c];
        put(d, TS_DISC, k->x, k->y, k->r, 0, 0, 0, rgba(10, 14, 18, 0.65f * d->alpha), rgba(255, 255, 255, 0.8f * d->alpha));
        text(d, k->x, k->y, k->r * 1.1f, 0, "+", rgba(255, 255, 255, d->alpha));
    }
    draw_top_buttons(t, d, 1);
}

static void draw_panel_rows(const Touch *t, Draw *d)
{
    float W = t->screen.width, H = t->screen.height;
    put(d, TS_RECT, W / 2, H / 2, W, H, 0, 0, rgba(6, 8, 12, 0.58f), 0);
    put(d, TS_RECT, W / 2, H / 2, W * 0.33f, H * 0.45f, 2.0f, 0, rgba(14, 22, 29, 0.95f), rgba(166, 227, 154, 0.55f));
    uint32_t green = rgba(166, 227, 154, 1), white = rgba(255, 255, 255, 1), off = rgba(255, 255, 255, 0.35f);
    if (t->pause == PAUSE_CONTROLS) {
        static const char *const lines[] = {
            "CONTROLS", "",
            "LEFT THUMB: STEER AND LEAN, SPIN AND FLIP",
            "PEDAL: HOLD TO PEDAL, DOUBLE-TAP AND HOLD: SPRINT",
            "FLICK PEDAL LEFT / RIGHT: ATTACK",
            "HOP: HOLD TO CROUCH, LET GO TO JUMP",
            "IN THE AIR: PUSH THE TRICK STICK, ROLL TO COMBINE",
            "PAST THE EDGE: STYLED TRICK",
            "SIGNATURE: LEFT, THEN RIGHT THROUGH THE MIDDLE, OUT",
            "(NEEDS A FULL TRICK METER)", "", "TAP TO GO BACK"};
        float fit = W * 0.66f - 6.0f;   /* the panel's width less its margins */
        for (unsigned i = 0; i < sizeof lines / sizeof *lines; i++) {
            size_t n = strlen(lines[i]);
            float h = i ? 2.2f : 3.6f;
            if (n && 0.9f * h * (float)n > fit) h = fit / (0.9f * (float)n);   /* a long line shrinks to fit */
            text(d, W / 2, H * 0.12f + i * 4.4f, h, 0, lines[i], i ? white : green);
        }
        return;
    }
    if (t->pause == PAUSE_CONFIRM) {
        text(d, W / 2, H * 0.3f, 3.6f, 0, "ARE YOU SURE?", green);
        static const char *const rows[2] = {"NO", "YES"};
        for (int r = 0; r < 2; r++) {
            Box b = confirm_box(t, r);
            put(d, TS_RECT, b.x, b.y, b.hw, b.hh, 1.0f, 0, rgba(255, 255, 255, 0.06f), 0);
            text(d, b.x, b.y, 3.0f, 0, rows[r], white);
        }
        return;
    }
    text(d, W / 2, H * 0.05f + 5, 3.9f, 0, "PAUSED", green);
    static const char *const rows[PAUSE_ROWS] = {"RESUME", "RESTART RACE", "QUIT RACE", "CONTROLS", "BAIL OUT"};
    for (int r = 0; r < PAUSE_ROWS; r++) {
        Box b = pause_row_box(t, r);
        int enabled = r >= ROW_CONTROLS || t->game.pause_row[r] >= 0;
        int pressed = 0;
        for (int i = 0; i < TOUCH_FINGERS; i++)
            if (t->f[i].used && t->f[i].job == JOB_PANEL && t->f[i].target == r && pause_row_at(t, t->f[i].x, t->f[i].y) == r) pressed = 1;
        if (pressed) put(d, TS_RECT, b.x, b.y, b.hw, b.hh, 1.0f, 0, rgba(166, 227, 154, 0.17f), 0);
        if (r == ROW_CONTROLS) put(d, TS_RECT, b.x, b.y - b.hh - 1.0f, b.hw, 0.15f, 0, 0, rgba(166, 227, 154, 0.45f), 0);
        text(d, b.x, b.y, 3.0f, 0, rows[r], enabled ? white : off);
    }
}

static void draw_online_panel(const Touch *t, Draw *d)
{
    static const char *const names[PANEL_BUTTONS] = {"CLOSE", "BAIL OUT", "LEAVE RACE"};
    for (int b = 0; b < PANEL_BUTTONS; b++) {
        Box x = panel_box(t, b);
        const char *label = names[b];
        uint32_t fill = rgba(10, 14, 18, 0.9f), line = b == PANEL_LEAVE ? rgba(255, 122, 122, 1) : rgba(255, 255, 255, 0.4f);
        put(d, TS_RECT, x.x, x.y, x.hw, x.hh, 1.4f, 0, fill, line);
        text(d, x.x, x.y, 2.2f, 0, label, b == PANEL_LEAVE ? rgba(255, 179, 179, 1) : rgba(255, 255, 255, 1));
    }
    if (t->leave_armed_until > t->now) {
        Box x = panel_box(t, PANEL_LEAVE);
        text(d, x.x, x.y + x.hh + 3, 2.2f, 0, "TAP AGAIN TO LEAVE", rgba(255, 179, 179, 1));
    }
}

static void draw_editor(const Touch *t, Draw *d)
{
    float W = t->screen.width, H = t->screen.height;
    const TouchScreen *s = &t->screen;
    put(d, TS_RECT, W / 2, H / 2, W, H, 0, 0, rgba(6, 8, 12, 0.38f), 0);
    if (s->strip_left > 0) put(d, TS_STRIPES, s->strip_left / 2, H / 2, s->strip_left / 2, H / 2, 0, 0, rgba(255, 90, 90, 0.28f), 0);
    if (s->strip_right > 0) put(d, TS_STRIPES, W - s->strip_right / 2, H / 2, s->strip_right / 2, H / 2, 0, 0, rgba(255, 90, 90, 0.28f), 0);
    if (s->strip_bottom > 0) put(d, TS_STRIPES, W / 2, H - s->strip_bottom / 2, W / 2, s->strip_bottom / 2, 0, 0, rgba(255, 90, 90, 0.28f), 0);
    Draw full = *d;
    full.alpha = 1;
    if (t->editor_tab == EDIT_AIR) {
        draw_trick(t, &full);
        button(&full, &t->ctl[TC_PEDAL], 0, 0.45f, TS_CROSS, "PEDAL");
    } else if (t->set.preset == TOUCH_CLASSIC) draw_classic(t, &full);
    else {
        int air = t->air_layout;
        Touch *mt = (Touch *)t;
        mt->air_layout = 0;
        draw_thumb(t, &full);
        mt->air_layout = air;
    }
    d->n = full.n;
    if (t->edit_selected >= 0 && t->ctl[t->edit_selected].on) {
        const Ctl *k = &t->ctl[t->edit_selected];
        float r = k->box ? fmaxf(k->hw, k->hh) : k->r;
        put(d, TS_RING, k->x, k->y, r + 1.2f, 0.5f, 0, 0, 0, rgba(255, 209, 102, 1));
    }
    static const char *const names[EDIT_BUTTONS] = {"GROUND", "AIR", "-", "+", "RESET", "DONE"};
    for (int b = 0; b < EDIT_BUTTONS; b++) {
        Box x = editor_box(t, b);
        int on = (b == EDIT_GROUND && t->editor_tab == EDIT_GROUND) || (b == EDIT_AIR && t->editor_tab == EDIT_AIR);
        uint32_t fill = b == EDIT_DONE ? rgba(166, 227, 154, 1) : on ? rgba(255, 255, 255, 0.18f) : rgba(10, 14, 18, 0.9f);
        put(d, TS_RECT, x.x, x.y, x.hw, x.hh, 1.4f, 0, fill, rgba(255, 255, 255, 0.28f));
        text(d, x.x, x.y, 2.4f, 0, names[b], b == EDIT_DONE ? rgba(16, 32, 16, 1) : rgba(255, 255, 255, 1));
    }
    if (t->edit_selected >= 0 && t->ctl[t->edit_selected].on) {
        const Ctl *k = &t->ctl[t->edit_selected];
        char info[48];
        float size = (k->box ? fmaxf(k->hw, k->hh) : k->r) * 2;
        snprintf(info, sizeof info, "%s %.0f MM", control_labels[t->edit_selected], size);
        Box a = editor_box(t, EDIT_BUTTONS);
        if (a.hh > 2) a = editor_box(t, EDIT_AIR), a.x += a.hw + 15;   /* one row: beside AIR */
        text(d, a.x, a.y, 2.2f, 0, info, rgba(255, 209, 102, 1));
    }
}

static void draw_hint(const Touch *t, Draw *d)
{
    const char *line1 = NULL, *line2 = NULL;
    int ring = -1;
    switch (t->hint) {
    case TOUCH_HINT_PEDAL: line1 = "HOLD PEDAL TO PEDAL"; line2 = "THE LEFT THUMB STEERS"; ring = TC_PEDAL; break;
    case TOUCH_HINT_ATTACK:
        line1 = t->set.attacks == TOUCH_ATTACK_FLICK ? "FLICK PEDAL SIDEWAYS TO ATTACK" : "TAP SQUARE OR CIRCLE TO ATTACK";
        line2 = t->set.attacks == TOUCH_ATTACK_FLICK ? "YOU KEEP PEDALLING" : "LEFT OR RIGHT RIDER";
        ring = t->set.attacks == TOUCH_ATTACK_FLICK ? TC_PEDAL : TC_SQUARE;
        break;
    case TOUCH_HINT_HOP: line1 = "LIFT, THEN HOLD HOP TO CROUCH"; line2 = "LET GO TO JUMP"; ring = TC_HOP; break;
    case TOUCH_HINT_SPRINT: line1 = "DOUBLE-TAP AND HOLD PEDAL"; line2 = "TO SPRINT"; ring = TC_PEDAL; break;
    case TOUCH_HINT_SLIDE: line1 = "HOLD SLIDE FOR A POWERSLIDE"; line2 = "WHILE THE LEFT THUMB STEERS"; ring = TC_SLIDE; break;
    case TOUCH_HINT_TRICK: line1 = "PUSH A DIRECTION FOR A TRICK"; line2 = t->freeze || t->freeze_at ? "LET GO BEFORE YOU LAND - THE GAME WAITS FOR YOU" : "LET GO BEFORE YOU LAND"; ring = TC_TRICK; break;
    default: return;
    }
    float W = t->screen.width, h1 = 3.0f, h2 = 2.2f;
    float text_w = fmaxf(strlen(line1) * 0.9f * h1, strlen(line2) * 0.9f * h2);
    float w = text_w / 2 + 4;
    float x = W * 0.42f, y = t->screen.height * 0.3f;
    float rx = 0, ry = 0, rr = 0;
    int ringed = ring >= 0 && t->ctl[ring].on;
    if (ringed) {
        const Ctl *k = &t->ctl[ring];
        rx = k->x, ry = k->y, rr = (ring == TC_TRICK ? 15.5f * trick_scale(t) : k->r) + 1.5f;
        if (fabsf(x - rx) < w + rr && fabsf(y - ry) < 6.5f + rr) {
            /* the box would cover the ring it points at (the trick stick's, on the Mi 11i): it goes
               beside the ring, toward the middle, its text shrunk to the room there */
            float room = rx > W / 2 ? rx - rr - 4 : W - (rx + rr) - 4;
            if (2 * w > room && text_w > 0) {
                float f = fmaxf(room - 8, 10) / text_w;
                h1 *= f, h2 *= f, w = room / 2;
            }
            x = rx > W / 2 ? rx - rr - 2 - w : rx + rr + 2 + w;
        }
    }
    put(d, TS_RECT, x, y, w, 6.5f, 2.0f, 0, rgba(10, 14, 18, 0.94f), rgba(255, 255, 255, 0.35f));
    text(d, x, y - 2.2f, h1, 0, line1, rgba(255, 255, 255, 1));
    text(d, x, y + 2.6f, h2, 0, line2, rgba(255, 255, 255, 0.7f));
    /* under the box: the top's camera and Pause buttons stay clear */
    if (t->freeze || t->freeze_at) text(d, x, y + 6.5f + 2.8f, 2.2f, 0, "PAUSED FOR THE HINT", rgba(255, 255, 255, 0.8f));
    if (ringed) put(d, TS_RING, rx, ry, rr, 0.9f, 0, 0, 0, rgba(166, 227, 154, 0.9f));
}

int touch_shapes(const Touch *t, TouchShape *out, int max)
{
    Draw d = {out, 0, max, clampf(t->set.opacity / 100.0f, 0.1f, 1.0f)};
    if (!touch_visible(t)) return 0;
    if (t->editor) { draw_editor(t, &d); return d.n; }
    if (t->set.preset == TOUCH_CLASSIC) draw_classic(t, &d);
    else draw_thumb(t, &d);
    if (t->label[0] && t->set.labels != TOUCH_LABELS_OFF) {
        char shown[TOUCH_TEXT + 4];
        snprintf(shown, sizeof shown, "%s%s", t->label, t->trick.triangle ? " *" : "");
        text(&d, t->screen.width / 2, t->screen.height * 0.26f, 3.6f, 0, shown, rgba(255, 255, 255, 0.95f));
    }
    if (t->panel_open) draw_online_panel(t, &d);
    if (t->hint) draw_hint(t, &d);
    if (t->pause == PAUSE_OPEN || t->pause == PAUSE_CONTROLS || t->pause == PAUSE_CONFIRM) draw_panel_rows(t, &d);
    return d.n;
}
