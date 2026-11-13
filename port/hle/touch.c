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
   - A press the game hasn't read yet when its finger lifts (down and up between two pad reads, a
     stall, a held-back or waiting press) still goes, for PRESS_FIELDS; a later press of the same
     buttons waits for its release, so a double tap stays two presses (the windows above apply).
   - The air layout comes air_wait_ms after the air flag; the trick stick's zones, latches and
     its one sending rule (settle, past the edge once the gate is open, landing).
   - Paused: race touches send nothing; the pause rows are ↓ / ↑ steps then ✕, sent as presses
     the game sees (whole fields of its VBLANKs). A controller used on the pause menu takes it
     over (PAUSE_PAD); the race clock moving again ends any pause state.
   - The D-pad (finding 214, Steer With: D-pad): four arms around a rest point, where the stick
     sat; the game gets the pad's own d-pad buttons (full lock, lean, powerslide on a double tap:
     what a PS2 d-pad does in a race). A thumb on the pad presses the directions its angle from the
     rest point is in, so a thumb between two arms presses both, never opposite ones, and a slide
     from arm to arm needs no lift.
   - The game's codes (finding 189): the pause rows' CODES page has the d-pad and the four face
     buttons the codes are made of; ENTER resumes the race and then sends the typed presses as a
     pad player types them (each a short press, well inside the game's code-reading windows), with
     the race touches let go while they go. */
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
    HISTORY = 24,
    /* codes (finding 189): at most 8 presses (the game's code table, D_00272448); before the
       first, a quiet time longer than the game's 20 frames of quiet that end a code
       (func_001E6F58, D_0029D7D4), so the Resume row's ✕ is not taken as the code's start */
    CODE_PRESSES = 8, CODE_LEAD_FIELDS = 30
};
static const float SETTLE_MM = 1.0f, FIRST_LATCH_MM = 4.0f, SHIFT_PREVIEW_MM = 1.5f, SHIFT_MAX_MM = 5.0f;
static const float HYSTERESIS_DEG = 12.0f;
static const float FLICK_PAST_RIM_MM = 4.0f, FLICK_REACH_MM = 45.0f, FLICK_LANE_DEG = 30.0f, FLICK_SPEED = 150.0f;
static const float GAP_MM = 3.0f, AREA_GROW = 1.2f;
/* The D-pad (finding 214), at button scale 1: arms 9 mm square, GAP_MM apart (the 9 mm targets of
   the panels), the rest point's dead zone 60 % of the way to the arms' inner edge. A thumb's angle
   from the rest point presses a direction within 67.5° of its axis (so a diagonal thumb presses two
   neighbours, 22.5°-wide bands either side of an axis press one); a band's edge is 4° either way
   sticky, so a thumb resting on it doesn't flap. */
static const float DPAD_HALF_MM = 4.5f, DPAD_DEAD = 0.6f, DPAD_REACH_DEG = 67.5f, DPAD_STICKY_DEG = 4.0f;
static const float REF_W = 154.5f, REF_H = 69.5f;  /* the reference phone (Mi 11i), mm */

/* ---------------------------------------------------------------- layout */

enum { AT_BR, AT_BL, AT_TL };   /* where a control is measured from */
enum { SHAPE_ROUND, SHAPE_BOX };
typedef struct { int anchor, shape; float px, py, w, h; } Place;  /* % of the reference screen; w, h % of its width / height */

static const char *const control_names[TC_COUNT] = {
    "pedal", "hop", "brake", "slide", "square", "circle", "camera", "pause",
    "cross", "triangle", "csquare", "ccircle", "plusleft", "plusright", "lbar", "rbar", "l3", "r3", "stick", "trick"
};

typedef struct {
    int on;
    float x, y, r, hw, hh;   /* r: round; hw, hh: box half sizes */
    int box;
    float x0, y0, r0, hh0;   /* its place and size before the player's edits (the steering area and the
                                D-pad's room are measured from these: moving a button never moves them) */
} Ctl;

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

enum { JOB_NONE, JOB_STICK, JOB_BUTTON, JOB_PEDAL, JOB_TRICK, JOB_AIR_WAIT, JOB_PANEL, JOB_EDIT, JOB_IGNORED, JOB_SKIP, JOB_DPAD };
/* the D-pad's arms, in the order of the CODES page's d-pad (and TOUCH_TARGET_DPAD's indices) */
enum { DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT, DPAD_ARMS };
static const uint16_t DPAD_PAD[DPAD_ARMS] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT};
static const float DPAD_AXIS_DEG[DPAD_ARMS] = {90.0f, -90.0f, 180.0f, 0.0f};   /* y up */
/* PAUSE_PAD: a controller took over the game's pause menu (or its question) from the rows: the
   overlay no longer knows where the game's menu is, so it hides and leaves the menu to the
   controller until the race goes on. */
enum { PAUSE_NONE, PAUSE_ASKED, PAUSE_OPEN, PAUSE_CONTROLS, PAUSE_CONFIRM, PAUSE_PAD, PAUSE_CODES };
/* ROW_CODES shares the CONTROLS row (its right half: the panel has room for five rows) */
enum { ROW_RESUME, ROW_RESTART, ROW_QUIT, ROW_CONTROLS, ROW_BAIL, ROW_CODES, PAUSE_ROWS, PAUSE_BUTTON = 100 };
/* the CODES page: the d-pad and the face buttons (as the codes name them), then CLEAR, BACK, ENTER */
enum { CODE_UP, CODE_DOWN, CODE_LEFT, CODE_RIGHT, CODE_TRIANGLE, CODE_CIRCLE, CODE_CROSS, CODE_SQUARE,
       CODE_CLEAR, CODE_BACK, CODE_ENTER, CODE_BUTTONS };
static const uint16_t CODE_PAD[CODE_CLEAR] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
                                              PAD_TRIANGLE, PAD_CIRCLE, PAD_CROSS, PAD_SQUARE};
enum { PANEL_CLOSE, PANEL_BAIL, PANEL_LEAVE, PANEL_BUTTONS };
enum { CONFIRM_NO, CONFIRM_YES };
enum { EDIT_GROUND, EDIT_AIR, EDIT_MINUS, EDIT_PLUS, EDIT_UNDO, EDIT_REDO, EDIT_RESET, EDIT_DONE, EDIT_BUTTONS };
/* the editor's history (finding 226): the preset's edits after each gesture, -/+ run, RESET */
enum { UNDO_STEPS = 32, REPEAT_FIRST_MS = 400, REPEAT_MS = 90 };
typedef struct { TouchEdit e[TC_COUNT]; int legacy; } EditStep;

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
    unsigned order;            /* touch-downs so far at its own: older fingers have smaller ones */
    int shown;                 /* its press was in the pad output of a field (the game read it) */
    int lifted;                /* lifted before the game read its press: kept until it has (finger_of skips it) */
    uint32_t lift_until;       /* lifted and shown: the VBLANK its press ends */
    int after_lift;            /* waits for an older lifted press of the same buttons to be let up first */
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
    float dpad_fit;            /* the D-pad's arms times this to fit between the HUD and L3 (finding 218) */
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
    int editor_alone;          /* the editor alone, over a plain background (touch_open_editor_alone) */
    TouchEdit edit_before, edit_ok;   /* a drag's control before it, and at the last place it fitted */
    EditStep undo[UNDO_STEPS];
    int undo_count, undo_at;   /* steps kept; the one shown */
    int pinch_finger;          /* a second finger resizing the held control (-1 none) */
    float pinch_d0, pinch_size0;
    int repeat_finger;         /* a finger held on - or +: the step repeats (-1 none) */
    double repeat_at;
    int repeated;
    int tablet;                /* laid out with the tablet defaults (lay_out) */
    /* air */
    int air_layout;
    double air_since;          /* race ms the air flag came */
    int air_was;
    /* the action pictures' ground or air (finding 229): the flag as last seen and since when (race
       ms), the state shown, and each control's own (kept while it is held) */
    int pic_flag, pic_air;
    double pic_flag_since;
    unsigned char pic_air_ctl[TC_COUNT];
    /* ✕ and SELECT windows (race ms) */
    double cross_released, select_released;
    int cross_was_down, cross_released_by_overlay, controls_since_release;
    double cross_release_wall;
    double hold_cross_until;   /* after a flick lifts: ✕ held until then (race ms; 0 none) */
    int select_was_down;
    /* timed presses */
    uint16_t pulse_buttons[16];
    uint32_t pulse_until[16];  /* the VBLANK it ends, once it went out */
    int pulse_fields[16];      /* its length, until it went out (then 0) */
    Step seq[48];
    int seq_len, seq_at;
    uint32_t seq_step_until;
    int seq_started;
    /* the CODES page: the presses typed, and whether the sequence now going is a code's */
    uint8_t code[CODE_PRESSES];
    int code_len, code_sending;
    /* steering */
    int stick_finger;
    float stick_cx, stick_cy, stick_dx, stick_dy;   /* deflection -1..1 */
    int dpad_finger;                                  /* the thumb on the D-pad (-1 none) */
    uint16_t dpad_buttons;                            /* the d-pad buttons it presses now */
    /* what the game gets (finding 218, dpad_field): each new direction at least PRESS_FIELDS from the
       first field that sends it, and a field up before a new press of a direction still showing,
       so a quick tap and a double tap (the powerslide) reach the game as a pad's presses would */
    uint16_t dpad_latched, dpad_gap, dpad_out;
    uint32_t dpad_until[4];
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
    int fly_was;               /* the flight over the track played at the last update (finding 209) */
    int replay_ran;            /* this replay's clock has moved (its load and countdown are over; finding 218) */
    int skip_pulse;            /* a SKIP tap's START press is going out */
    unsigned downs;            /* touch-downs so far (Finger.order) */
    char note[160];            /* touch_take_note's line */
    int note_ready, rescue_noted;   /* a line waits; the steering rescue was logged (once a run) */
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
    s->style = TOUCH_STYLE_ACTIONS;
    s->vibration = 1;
    s->tilt_strength = 50;
    s->hud_moved = 1;
    s->air_wait_ms = 150;
    for (int p = 0; p < 2; p++)
        for (int c = 0; c < TC_COUNT; c++) s->edit[p][c].size = 1;
}

void touch_layout_parse(TouchSettings *s, const char *text)
{
    int edited[2] = {0, 0}, current[2] = {0, 0};
    for (int p = 0; p < 2; p++)
        for (int c = 0; c < TC_COUNT; c++) s->edit[p][c] = (TouchEdit){0, 0, 1};
    while (text && *text) {
        char name[32];
        float dx, dy, size;
        int n = 0;
        int version = 0;
        if (sscanf(text, " %31[a-z0-9.]=%f,%f,%f%n", name, &dx, &dy, &size, &n) == 4 && n > 0) {   /* digits: l3, r3 */
            int p = !strncmp(name, "classic.", 8) ? 1 : !strncmp(name, "thumb.", 6) ? 0 : -1;
            const char *control = p == 1 ? name + 8 : name + 6;
            /* 0.8.0 wrote sizes 0.6-1.6 and read 0.5-2.0; a 13-inch tablet needs moves past 200 mm */
            for (int c = 0; p >= 0 && c < TC_COUNT; c++)
                if (!strcmp(control, control_names[c]) && fabsf(dx) < 1000 && fabsf(dy) < 1000 &&
                    size >= TOUCH_SIZE_MIN - 0.001f && size <= TOUCH_SIZE_MAX + 0.001f) {
                    s->edit[p][c] = (TouchEdit){dx, dy, size};
                    edited[p] = 1;
                }
        } else if (sscanf(text, " %31[a-z.]=%d", name, &version) == 2 && version >= 2) {   /* thumb.layout=2 */
            if (!strcmp(name, "thumb.layout")) current[0] = 1;
            if (!strcmp(name, "classic.layout")) current[1] = 1;
        }
        const char *next = strchr(text, ';');
        text = next ? next + 1 : NULL;
    }
    /* edits without the mark were made on 0.8.0's places: they keep them (touch.h) */
    for (int p = 0; p < 2; p++) s->legacy[p] = edited[p] && !current[p];
}

void touch_layout_format(const TouchSettings *s, char *out, int size)
{
    int at = 0;
    out[0] = 0;
    for (int p = 0; p < 2; p++) {
        int edited = 0;
        for (int c = 0; c < TC_COUNT; c++) {
            const TouchEdit *e = &s->edit[p][c];
            if (e->dx != 0 || e->dy != 0 || e->size != 1) edited = 1;
        }
        if (edited && !s->legacy[p]) {   /* laid out on the current places (touch_layout_parse) */
            int n = snprintf(out + at, (size_t)(size - at), "%s%s.layout=2", at ? ";" : "", p ? "classic" : "thumb");
            if (n < 0 || at + n >= size) { out[at] = 0; return; }
            at += n;
        }
        for (int c = 0; c < TC_COUNT; c++) {
            const TouchEdit *e = &s->edit[p][c];
            if (e->dx == 0 && e->dy == 0 && e->size == 1) continue;
            int n = snprintf(out + at, (size_t)(size - at), "%s%s.%s=%.1f,%.1f,%.2f", at ? ";" : "",
                             p ? "classic" : "thumb", control_names[c], e->dx, e->dy, e->size);
            if (n < 0 || at + n >= size) { out[at] = 0; return; }
            at += n;
        }
    }
}

/* ---------------------------------------------------------------- geometry */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float dist(float ax, float ay, float bx, float by) { return hypotf(ax - bx, ay - by); }
static int right_handed(const Touch *t) { return !t->set.left_handed; }
/* x as the right-handed layout has it (the left hand mirrors). */
static float mirror_x(const Touch *t, float x) { return right_handed(t) ? x : t->screen.width - x; }

/* The D-pad's arms (finding 214): their half size, the distance of an arm's centre from the rest
   point (the arms are GAP_MM apart, like the other buttons) and the pad's half extent. The size
   follows the buttons' (touch_button_scale), kept to the room below the Classic bar (or the top
   buttons) so the pad fits at the Size setting's low end, times the layout editor's size for the
   stick's home. */
/* The room below the Classic bar (or the top buttons) as the preset places them: from their places
   before the player's edits, so a bar dragged to the bottom never squeezes the pad to nothing (the
   review of 2026-11-13; finding 229). */
static const float DPAD_HALF_MIN_MM = 2.0f;
static float dpad_room(const Touch *t)
{
    const Ctl *above = t->set.preset == TOUCH_CLASSIC ? &t->ctl[TC_LBAR] : &t->ctl[TC_CAMERA];
    return t->screen.height - t->screen.strip_bottom - 1.0f - (above->y0 + above->hh0 * AREA_GROW + GAP_MM);
}
static float dpad_half(const Touch *t, const TouchEdit *e)
{
    float fit = (dpad_room(t) / 2 - GAP_MM) / 3;   /* the pad's half extent, 3 half + GAP_MM, is half the room */
    if (fit < DPAD_HALF_MIN_MM) fit = DPAD_HALF_MIN_MM;   /* never zero or less, whatever the screen */
    float half = DPAD_HALF_MM * t->bs;
    return (half < fit ? half : fit) * e->size * (t->dpad_fit > 0 ? t->dpad_fit : 1.0f);
}
static float dpad_off(const Touch *t, const TouchEdit *e) { return 2 * dpad_half(t, e) + GAP_MM; }
static float dpad_extent(const Touch *t, const TouchEdit *e) { return dpad_off(t, e) + dpad_half(t, e); }

/* The game's own HUD on the steering side (finding 218): the track bar (the riders' places down the
   track) and the place number under it sit at the screen's edge from about half its height down, the
   bar leaning outward. Measured on the Mi 11i's 2400 x 1080 picture of a race (the HUD scales with the
   screen's height): the bar starts 36.3 mm down, 3.2 mm in from the edge, and ends at the bottom
   12.4 mm in; the place number is inside that line. Hand Left mirrors it, as the overlay. */
static const float HUD_BAR_TOP = 0.522f, HUD_BAR_IN = 0.046f, HUD_BAR_LEAN = 0.297f;
/* How far in from the steering side's edge the HUD reaches at height y (mm; 0 above the bar). */
static float hud_reach(const Touch *t, float y)
{
    float H = t->screen.height, v = (y < H ? y : H) / H;
    return v < HUD_BAR_TOP ? 0 : H * (HUD_BAR_IN + (v - HUD_BAR_TOP) * HUD_BAR_LEAN);
}

/* The D-pad's four arms at rest point x, y (right-handed) are clear of the HUD and of L3 by GAP_MM. */
static int dpad_clear(const Touch *t, const TouchEdit *e, float x, float y)
{
    float half = dpad_half(t, e), off = dpad_off(t, e);
    const Ctl *l3 = &t->ctl[TC_L3];
    float lx = mirror_x(t, l3->x);
    for (int a = 0; a < DPAD_ARMS; a++) {
        float ax = x + (a == DPAD_LEFT ? -off : a == DPAD_RIGHT ? off : 0), ay = y + (a == DPAD_UP ? -off : a == DPAD_DOWN ? off : 0);
        float bottom = ay + half + GAP_MM;
        if (bottom >= HUD_BAR_TOP * t->screen.height && ax - half < hud_reach(t, bottom) + GAP_MM) return 0;
        if (t->set.preset == TOUCH_CLASSIC && l3->on &&
            hypotf(fmaxf(fabsf(ax - lx) - half, 0), fmaxf(fabsf(ay - l3->y) - half, 0)) - l3->r < GAP_MM) return 0;
    }
    return 1;
}

/* Where the D-pad sits before the player's edits: the nearest place to the stick's home that keeps it
   below the Classic bar (or the top buttons), above the screen's bottom, clear of the left gesture
   strip, of the game's track bar and place number (hud_reach) and, in Classic, of L3 by the buttons'
   gap. Where no place is clear at full size (Classic between the track bar and L3) the arms shrink
   until one is, the pad's centre kept on the edge side of L3 (a pad's d-pad is beside its stick) while
   the arms stay DPAD_ARM_MIN_MM or more; at small Sizes (the cluster shrinks into the corner, the HUD
   doesn't) the pad may sit past L3. Past DPAD_FIT_MIN the pad keeps its size and only L3's gap holds,
   as before finding 218. Right-handed x. */
static const float DPAD_FIT_MIN = 0.6f, DPAD_ARM_MIN_MM = 5.0f, DPAD_STEP_MM = 0.5f;
static void dpad_home(Touch *t, const TouchEdit *e, float *x, float *y)
{
    const Ctl *above = t->set.preset == TOUCH_CLASSIC ? &t->ctl[TC_LBAR] : &t->ctl[TC_CAMERA];
    const Ctl *l3 = &t->ctl[TC_L3];
    float x0 = *x, y0 = *y;
    t->dpad_fit = 1.0f;
    float arm = 2 * dpad_half(t, e);
    /* pass 0: on the edge side of L3 with arms of DPAD_ARM_MIN_MM at least; pass 1 (small Sizes, where
       the cluster shrinks into the corner and the HUD doesn't): anywhere on the steering half */
    for (int pass = 0; pass < 2; pass++)
        for (float fit = 1.0f; fit >= DPAD_FIT_MIN - 0.001f; fit -= 0.02f) {
            if (pass == 0 && fit * arm < DPAD_ARM_MIN_MM - 0.001f && fit < 1.0f) break;
            t->dpad_fit = fit;
            float span = dpad_extent(t, e);
            float top = above->y0 + above->hh0 * AREA_GROW + GAP_MM + span;
            float bottom = t->screen.height - t->screen.strip_bottom - 1.0f - span;
            float side = (right_handed(t) ? t->screen.strip_left : t->screen.strip_right) + 1.0f + span;
            float far = t->screen.width / 2 - span;
            if (pass == 0 && t->set.preset == TOUCH_CLASSIC && l3->on && mirror_x(t, l3->x) < far) far = mirror_x(t, l3->x);
            if (bottom < top) top = bottom;
            float best = 1e9f, bx = 0, by = 0;
            for (float py = top; py <= bottom + 0.001f; py += DPAD_STEP_MM)
                for (float px = side; px <= far; px += DPAD_STEP_MM) {
                    float d = hypotf(px - x0, py - y0);
                    if (d < best && dpad_clear(t, e, px, py)) best = d, bx = px, by = py;
                }
            if (best < 1e9f) { *x = bx, *y = by; return; }
        }
    /* nothing clear: full size, the old rule (L3's gap only) */
    t->dpad_fit = 1.0f;
    float half = dpad_half(t, e), off = dpad_off(t, e), span = dpad_extent(t, e);
    float top = above->y0 + above->hh0 * AREA_GROW + GAP_MM + span;
    float bottom = t->screen.height - t->screen.strip_bottom - 1.0f - span;
    float side = (right_handed(t) ? t->screen.strip_left : t->screen.strip_right) + 1.0f + span;
    *y = bottom < top ? bottom : clampf(*y, top, bottom);
    if (*x < side) *x = side;
    if (t->set.preset != TOUCH_CLASSIC || !l3->on) return;
    float lx = mirror_x(t, l3->x);
    for (; *x > side; *x -= 0.5f) {
        int clear = 1;
        for (int a = 0; a < DPAD_ARMS; a++) {
            float ax = *x + (a == DPAD_LEFT ? -off : a == DPAD_RIGHT ? off : 0), ay = *y + (a == DPAD_UP ? -off : a == DPAD_DOWN ? off : 0);
            if (hypotf(fmaxf(fabsf(ax - lx) - half, 0), fmaxf(fabsf(ay - l3->y) - half, 0)) - l3->r < GAP_MM) clear = 0;
        }
        if (clear) break;
    }
}

/* A tablet's screen (TOUCH_TABLET_MM on its short side), whatever the layout's places. */
static int big_screen(const Touch *t) { return fminf(t->screen.width, t->screen.height) >= TOUCH_TABLET_MM; }

/* The tablet defaults (finding 226). Two hands hold a tablet by its sides, the thumbs some way up
   from the lower corners: the bottom clusters move up by a quarter of the height past 80 mm (at
   most 28 mm: a 13-inch iPad) and in by a tenth of the width past 160 mm (at most 12 mm). */
static float tablet_lift(const Touch *t) { return t->tablet ? clampf(0.25f * (t->screen.height - 80.0f), 0, 28.0f) : 0; }
static float tablet_inward(const Touch *t) { return t->tablet ? clampf(0.1f * (t->screen.width - 160.0f), 0, 12.0f) : 0; }

/* A place's centre before the player's edits (right-handed). */
static void place_xy(const Touch *t, const Place *p, float *x, float *y)
{
    float W = t->screen.width, H = t->screen.height, s = t->s, lift = tablet_lift(t), in = tablet_inward(t);
    if (p->anchor == AT_BR) *x = W - (100 - p->px) * REF_W / 100 * s - in, *y = H - (100 - p->py) * REF_H / 100 * s - lift;
    else if (p->anchor == AT_BL) *x = p->px * REF_W / 100 * s + in, *y = H - (100 - p->py) * REF_H / 100 * s - lift;
    else *x = p->px * REF_W / 100, *y = p->py * REF_H / 100;
}

/* Where the action side's usable screen ends (right-handed x): the edge, less its gesture strip
   and the camera cutout's band when the cutout is on that side. */
static float action_edge(const Touch *t)
{
    const TouchScreen *s = &t->screen;
    float W = s->width;
    int cut = s->cut_x1 > s->cut_x0 && s->cut_y1 > s->cut_y0;
    if (right_handed(t)) {
        float e = W - s->strip_right;
        return cut && s->cut_x0 > W / 2 ? fminf(e, s->cut_x0) : e;
    }
    float e = W - s->strip_left;
    return cut && s->cut_x1 < W / 2 ? fminf(e, W - s->cut_x1) : e;
}

/* The top buttons (Pause / START and camera / SELECT; finding 226). 0.8.0 put them in the top-left
   corner, on phones as on tablets, where the race HUD the touch controls move (the speedometer and
   the PUNCH meter beside it, finding 141's HUD hook) reaches about 0.9 of the screen's height
   across: the buttons covered the meter's end and its label (the iOS Simulator's run, finding
   224), and on a tablet the corner is out of a thumb's reach. Now they sit in a row just above the
   action cluster's touch areas (its top before the player's edits), Pause at the action side's
   edge, clear of the cutout and the gesture strip; the left hand mirrors it (its HUD goes to the
   top right, the buttons to the top left). The game's centred messages stay clear (they start
   about 36 % across). */
static void top_place(const Touch *t, int c, int hud_moved, float *x, float *y)
{
    const TouchSettings *st = &t->set;
    float top = t->screen.height;
    for (int o = 0; o < TC_COUNT; o++) {
        Place p;
        if (o == TC_TRICK || !place_of(st->preset, o, st->attacks, hud_moved, &p) || p.anchor != AT_BR) continue;
        float ox, oy;
        place_xy(t, &p, &ox, &oy);
        float half = (p.shape == SHAPE_BOX ? p.h * REF_H / 200 : p.w * REF_W / 200) * t->bs;
        top = fminf(top, oy - half * AREA_GROW);
    }
    Place me;
    place_of(st->preset, TC_PAUSE, st->attacks, hud_moved, &me);
    float hw = me.w * REF_W / 200 * t->bs, hh = me.h * REF_H / 200 * t->bs;
    float pause_x = action_edge(t) - 2.0f - hw * AREA_GROW;
    *y = fmaxf(top - GAP_MM - hh * AREA_GROW, t->screen.strip_top + 1.0f + hh);
    *x = c == TC_PAUSE ? pause_x : pause_x - 2 * hw * AREA_GROW - GAP_MM;
}

/* The camera cutout and the default layout (finding 226): how far each half's controls move away
   from a cutout on its side so none of them is over it. 0.8.0 moved only the control that was
   over it, into its neighbours (the Mi 11i's Classic ○, the iPhone's Thumb zones SLIDE onto HOP
   with Hand Left), which also locked 0.8.0's editor on such a screen; the whole half now moves
   together, so its gaps stay. From the places before the player's edits, so a drag never moves
   the others. */
static void cutout_shift(const Touch *t, int hud_moved, int current, float *left, float *right)
{
    const TouchScreen *sc = &t->screen;
    const TouchSettings *st = &t->set;
    float W = sc->width;
    *left = *right = 0;
    if (!(sc->cut_x1 > sc->cut_x0 && sc->cut_y1 > sc->cut_y0)) return;
    for (int c = 0; c < TC_COUNT; c++) {
        Place p;
        if (c == TC_TRICK || !place_of(st->preset, c, st->attacks, hud_moved, &p)) continue;
        float x, y;
        if (current && (c == TC_CAMERA || c == TC_PAUSE)) top_place(t, c, hud_moved, &x, &y);
        else place_xy(t, &p, &x, &y);
        x = mirror_x(t, x);
        float rx = p.w * REF_W / 200 * (c == TC_STICK ? 1 : t->bs), ry = p.shape == SHAPE_BOX ? p.h * REF_H / 200 * t->bs : rx;
        if (x + rx < sc->cut_x0 || x - rx > sc->cut_x1 || y + ry < sc->cut_y0 || y - ry > sc->cut_y1) continue;
        if (sc->cut_x1 < W / 2) *left = fmaxf(*left, sc->cut_x1 + 1 - (x - rx));
        else *right = fmaxf(*right, x + rx + 1 - sc->cut_x0);
    }
}

/* The layout for the settings and the screen. */
static void lay_out(Touch *t, int hud_moved)
{
    const TouchSettings *st = &t->set;
    float W = t->screen.width, H = t->screen.height;
    float limit = touch_height_scale(H, st->preset);
    /* a tablet (unless its preset keeps 0.8.0's places) starts from a larger base size; the Size
       setting is a share of that base, so its 70-105 % mean the same on every screen */
    t->tablet = big_screen(t) && !st->legacy[st->preset == TOUCH_CLASSIC];
    float base = t->tablet ? TOUCH_TABLET_SCALE : 1.0f;
    float s = (st->size > 0 ? st->size / 100.0f : 1.0f) * base;
    if (s > limit) s = limit;
    if (s < 0.5f) s = 0.5f;
    t->s = s;
    t->bs = touch_button_scale(s / base) * base;   /* a phone: touch_button_scale(s), as before */
    /* a preset with 0.8.0's edits keeps 0.8.0's places whole (the top buttons, the cutout's push) */
    int current = !st->legacy[st->preset == TOUCH_CLASSIC];
    float shift_left = 0, shift_right = 0;
    if (current) cutout_shift(t, hud_moved, current, &shift_left, &shift_right);
    for (int c = 0; c < TC_COUNT; c++) {
        Place p;
        Ctl *k = &t->ctl[c];
        memset(k, 0, sizeof *k);
        if (!place_of(st->preset, c, st->attacks, hud_moved, &p)) continue;
        float w = p.w * REF_W / 100, h = p.h * REF_H / 100;
        float scale = t->bs;
        float x, y;
        if (current && (c == TC_CAMERA || c == TC_PAUSE)) top_place(t, c, hud_moved, &x, &y);
        else place_xy(t, &p, &x, &y);
        float side_x = mirror_x(t, x);   /* which half it belongs to (cutout_shift) */
        const TouchEdit *e = &st->edit[st->preset == TOUCH_CLASSIC][c];
        int dpad = c == TC_STICK && st->dpad;
        k->x0 = mirror_x(t, x) + (side_x < W / 2 ? shift_left : -shift_right), k->y0 = y;
        k->r0 = w / 2 * t->bs, k->hh0 = h / 2 * t->bs;
        if (c == TC_STICK) t->dpad_fit = 1.0f;
        if (dpad) dpad_home(t, e, &x, &y);
        x += e->dx, y += e->dy;
        scale *= e->size;
        if (c == TC_TRICK) scale = e->size * st->trick_size / 31.0f;   /* its own setting */
        if (c == TC_STICK) scale = 1;
        k->on = 1;
        k->x = mirror_x(t, x) + (side_x < W / 2 ? shift_left : -shift_right);
        k->y = y;
        k->box = p.shape == SHAPE_BOX;
        k->r = w / 2 * scale;
        k->hw = w / 2 * scale, k->hh = h / 2 * scale;
        if (c == TC_TRICK) k->r = st->trick_size / 2.0f * e->size;
        if (dpad) k->r = k->hw = k->hh = dpad_extent(t, e);   /* the pad's half extent: arms reach it */
        /* a control the player edited stays on the screen whatever the Size setting or the screen
           did since (finding 226; the editor keeps it there itself): too large for it, centred */
        if (e->dx != 0 || e->dy != 0 || e->size != 1) {
            float rx = c == TC_TRICK ? 7 : k->box ? k->hw : k->r, ry = c == TC_TRICK ? 7 : k->box ? k->hh : k->r;
            k->x = 2 * rx >= W ? W / 2 : clampf(k->x, rx, W - rx);
            k->y = 2 * ry >= H ? H / 2 : clampf(k->y, ry, H - ry);
        }
    }
    /* the camera cutout (read at run time) is kept clear: a control still over it (an edited one,
       or any of a 0.8.0 layout) moves inward */
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
       cluster's touch areas; Classic's to 44% of the width, below the L bar. All from the places
       before the player's edits (x0, y0): a button dragged into it takes the touches on itself
       (routing tries the buttons first), but moving one never moves the area's edges, so a bar
       dragged to the bottom can't leave no steering (the review of 2026-11-13; finding 229) */
    float left = t->screen.strip_left, x1 = left + 60 * (t->tablet ? TOUCH_TABLET_SCALE : 1) + tablet_inward(t), top = 0;
    if (st->preset == TOUCH_CLASSIC) {
        x1 = 0.44f * W;
        top = t->ctl[TC_LBAR].y0 + t->ctl[TC_LBAR].hh0 * AREA_GROW + GAP_MM;
    } else {
        if (x1 > W / 2 - GAP_MM) x1 = W / 2 - GAP_MM;
        for (int c = 0; c < TC_COUNT; c++) {
            const Ctl *k = &t->ctl[c];
            if (!k->on || c == TC_STICK || c == TC_TRICK || c == TC_CAMERA || c == TC_PAUSE) continue;
            float edge = mirror_x(t, k->x0) - k->r0 * AREA_GROW - GAP_MM;
            if (edge < x1) x1 = edge;
        }
        /* the top buttons are on the other side (top_place): the whole height steers */
        top = current ? t->screen.strip_top : t->ctl[TC_CAMERA].y0 + t->ctl[TC_CAMERA].hh0 * AREA_GROW + GAP_MM;
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

int touch_control_size(const Touch *t, int control, float *half_w, float *half_h)
{
    if (control < 0 || control >= TC_COUNT || !t->ctl[control].on) return 0;
    const Ctl *k = &t->ctl[control];
    if (half_w) *half_w = k->box ? k->hw : k->r;
    if (half_h) *half_h = k->box ? k->hh : k->r;
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

/* A press of `fields` fields, counted from the first pad read that has it (an event handled after
   a stall, with the VBLANKs of the stall already gone, still gets all of them). */
static void pulse(Touch *t, uint16_t buttons, int fields)
{
    for (int i = 0; i < 16; i++)
        if (!t->pulse_buttons[i]) {
            t->pulse_buttons[i] = buttons;
            t->pulse_fields[i] = fields;
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
/* nothing pressed for `fields` fields */
static void seq_wait(Touch *t, int fields)
{
    if (t->seq_at >= t->seq_len) seq_reset(t);
    if (t->seq_len + 1 > (int)(sizeof t->seq / sizeof *t->seq)) return;
    t->seq[t->seq_len++] = (Step){0, fields};
}

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
        if (t->f[i].used && !t->f[i].lifted && t->f[i].id == id) return &t->f[i];
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

/* When a button press (PEDAL or a button) may go out: ✕ and SELECT by their windows' rules, the
   rest at once. */
static void press_rule(Touch *t, Finger *f)
{
    if (f->buttons & PAD_CROSS) cross_press_begins(t, f, f->job != JOB_PEDAL);
    else if (f->buttons & PAD_SELECT) select_press_begins(t, f);
    else f->sent = 1, f->due_now = 0, f->due_race = 0;
}

/* An older press of some of the same buttons lifted before the game let it up: this one would
   merge with it (one press, no double tap), so it waits. */
static int lifted_before(const Touch *t, const Finger *f)
{
    for (int i = 0; i < TOUCH_FINGERS; i++) {
        const Finger *g = &t->f[i];
        if (g != f && g->used && g->lifted && (g->buttons & f->buttons) && g->order < f->order) return 1;
    }
    return 0;
}

/* A new press (route_thumb, route_classic): after an older lifted one of its buttons, or by its rule. */
static void press_begins(Touch *t, Finger *f)
{
    if (lifted_before(t, f)) {
        f->after_lift = 1;
        f->sent = 0, f->due_now = 0, f->due_race = 0;
        return;
    }
    press_rule(t, f);
}

/* A press the game hasn't read yet, its finger lifting: 1 when it still goes (the finger is kept,
   lifted, until it has gone for PRESS_FIELDS); 0 when there is nothing to send (a held-back PEDAL
   that flicked: the flick was its attack). A press held back to see whether it flicks didn't:
   it goes now. A guarded one, or one waiting for an earlier tap's release, goes when it would have. */
static int press_lifts(Touch *t, Finger *f)
{
    if (f->job == JOB_PEDAL && f->flicked && !f->sent) return 0;
    if (f->due_now) f->due_now = 0, f->due_race = 0;
    return 1;
}

/* Held-back and guarded presses whose time came. */
static void due_presses(Touch *t)
{
    for (int i = 0; i < TOUCH_FINGERS; i++) {
        Finger *f = &t->f[i];
        if (!f->used || f->sent || !f->buttons || f->after_lift) continue;
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

/* The moments with one thing to press (findings 209, 214): the camera's flight over the track, a
   movie, the title's attract demo and a race's replay running. The race controls do nothing in
   them (the pad is the game's own or the recording's) and the one thing a pad does is START (the
   demo takes any button): the overlay shows one button and nothing else, SKIP, or MENU in a replay
   (START opens its RE-START / SAVE / QUIT / CONTINUE menu). That menu is a popup like the others:
   while it is up (menu_up, the popup manager's own state: finding 218; one update's clock delta
   said "stopped" on most updates above 60 a second, and MENU flickered, a held tap was dropped) the
   overlay stands aside for the menus' mouse layer; CONTINUE closes it and the button returns. The
   replay's load and countdown show nothing (replay_ran: its clock hasn't moved yet). */
static int replay_on(const Touch *t) { return t->have_game && t->game.replay && !t->editor; }
static int flyover_on(const Touch *t)
{
    return t->have_game && !t->editor &&
           (t->game.flyover || t->game.movie || t->game.attract || (t->game.replay && t->replay_ran && !t->game.menu_up));
}

int touch_visible(const Touch *t)
{
    if (t->editor) return 1;
    if (t->pause == PAUSE_PAD) return 0;   /* the controller has the game's menu */
    if (t->pause != PAUSE_NONE) return 1;
    if (flyover_on(t)) return !t->hidden_by_pad;
    if (replay_on(t)) return 0;            /* the replay's menu, or its load: not the race's controls */
    return race_on_screen(t) && !t->hidden_by_pad;
}

/* The overlay's touches: while it shows, and in a race it hid for a controller (the touch that
   brings it back); in the menus a touch is the menus' (their mouse). */
int touch_owns_touches(const Touch *t)
{
    return touch_visible(t) || (t->hidden_by_pad && ((race_on_screen(t) && !replay_on(t)) || flyover_on(t)));
}

int touch_hud_mode(const Touch *t)
{
    if (!t->set.hud_moved || t->editor || !race_on_screen(t) || t->hidden_by_pad || replay_on(t)) return 0;
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
            if (t->f[i].lifted) t->f[i].used = 0;
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
        if (!f->used || f->lifted) continue;   /* a lifted press was the race's before the pause (it ended there) */
        if (f->job == JOB_PEDAL || f->job == JOB_BUTTON) {
            f->after_lift = 0, f->shown = 0;
            press_rule(t, f);
        } else if (f->job == JOB_TRICK || f->job == JOB_AIR_WAIT || f->job == JOB_PANEL) f->job = JOB_IGNORED;
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
    int line = row == ROW_CODES ? ROW_CONTROLS : row;
    float y = top + rh * (line + 0.5f) + (line >= ROW_CONTROLS ? 2.0f : 0);
    if (row == ROW_CONTROLS || row == ROW_CODES)   /* half each, CONTROLS on the left */
        return (Box){W / 2 + (row == ROW_CODES ? 1 : -1) * W * 0.15f, y, W * 0.15f - 0.5f, rh / 2 - 0.4f};
    return (Box){W / 2, y, W * 0.30f, rh / 2 - 0.4f};
}

/* The CODES page (finding 189): ↑ ↓ ← → in one row, △ ○ ✕ □ in the next (9 mm targets, 15 mm
   apart), CLEAR, BACK and ENTER at the bottom of the panel. */
static Box code_box(const Touch *t, int b)
{
    float W = t->screen.width, H = t->screen.height;
    if (b < CODE_CLEAR) {
        int col = b % 4, line = b / 4;
        return (Box){W / 2 + (col - 1.5f) * 15.0f, H * 0.40f + line * fmaxf(13.0f, H * 0.19f), 4.5f, 4.5f};
    }
    return (Box){W / 2 + (b - CODE_BACK) * 22.0f, H * 0.95f - 5.5f, 9.0f, 4.5f};
}

/* The flight's SKIP button: low in the middle, under either thumb, clear of the game's track name
   at the top and of the places the race controls take later. */
static Box skip_box(const Touch *t)
{
    /* under the right thumb (the action side; Hand Left mirrors it), its edge 8 mm in from the
       usable edge (finding 226): low in the middle (0.8.0) it covered the attract demo's own PRESS
       ANY BUTTON TO EXIT, and a tablet's middle is out of both thumbs' reach (40 % up there) */
    float x = mirror_x(t, action_edge(t) - 8.0f - 15.0f);
    return (Box){x, t->screen.height * (big_screen(t) ? 0.6f : 0.86f), 15.0f, 5.5f};
}

static int skip_hit(const Touch *t, float x, float y)
{
    Box b = skip_box(t);
    return fabsf(x - b.x) <= b.hw + 3.0f && fabsf(y - b.y) <= b.hh + 3.0f;   /* a 3 mm rim of touch area */
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
    float total = w[0] + w[1] + w[2] + 2 * GAP_MM + 4;
    if (x + total > t->screen.width - 2) x = t->screen.width - 2 - total;   /* a tablet's top buttons sit at the edge */
    for (int i = 0; i < b; i++) x += w[i] + GAP_MM + (i + 1 == PANEL_LEAVE ? 4 : 0);
    Box r = {x + w[b] / 2, y, w[b] / 2, 4.5f};
    r.x = mirror_x(t, r.x);
    return r;
}

/* The editor's buttons, in the top buttons' row (their default places: the editor moves them):
   GROUND and AIR on one side of them, - + UNDO REDO RESET DONE on the wider side (both groups
   there, GROUND and AIR first, when the other side is too narrow); b = EDIT_BUTTONS is the size
   readout's place, under the second group. A screen too narrow for that gets one row below. */
static Box editor_box(const Touch *t, int b)
{
    /* GROUND AIR | - + UNDO REDO RESET DONE: 9 mm targets at least, narrow enough for both groups
       beside the top buttons of a 20:9 phone (finding 226) */
    static const float w[EDIT_BUTTONS] = {14, 10, 9, 9, 12, 12, 14, 14};
    float W = t->screen.width;
    Place cam, pau;
    place_of(t->set.preset, TC_CAMERA, t->set.attacks, 1, &cam);
    place_of(t->set.preset, TC_PAUSE, t->set.attacks, 1, &pau);
    float hw = cam.w * REF_W / 200, y = cam.py * REF_H / 100, hh = cam.h * REF_H / 200;
    float a0 = mirror_x(t, cam.px * REF_W / 100), a1 = mirror_x(t, pau.px * REF_W / 100);
    if (!t->set.legacy[t->set.preset == TOUCH_CLASSIC]) {   /* the top buttons' row (top_place, before edits) */
        float cx, cy, px, py;
        top_place(t, TC_CAMERA, t->set.hud_moved, &cx, &cy);
        top_place(t, TC_PAUSE, t->set.hud_moved, &px, &py);
        hw = pau.w * REF_W / 200 * t->bs;
        a0 = mirror_x(t, cx), a1 = mirror_x(t, px);
        if (fminf(cy, py) - pau.h * REF_H / 200 * t->bs > y + hh + 1)   /* below the bar's row: no obstacle */
            a0 = a1 = W + hw + 2;
    }
    float span0 = fminf(a0, a1) - hw - 2, span1 = fmaxf(a0, a1) + hw + 2;   /* the top buttons, a 2 mm margin */
    float group_a = w[EDIT_GROUND] + 2 + w[EDIT_AIR], group_b = -2;
    for (int i = EDIT_MINUS; i < EDIT_BUTTONS; i++) group_b += w[i] + 2;
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
        if (b == EDIT_BUTTONS) return (Box){bx + group_b / 2, y + hh + 3.0f, group_b / 2, 1.5f};   /* under - + ... */
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
    static const int counts[7] = {PAUSE_ROWS, 2, PANEL_BUTTONS, EDIT_BUTTONS, CODE_BUTTONS, 1, DPAD_ARMS};
    if (kind < 0 || kind > 6 || index < 0 || index >= counts[kind]) return 0;
    if (kind == TOUCH_TARGET_DPAD) {
        if (!t->set.dpad || !t->ctl[TC_STICK].on) return 0;
        float off = dpad_off(t, &t->set.edit[t->set.preset == TOUCH_CLASSIC][TC_STICK]);
        *x = t->ctl[TC_STICK].x + (index == DPAD_LEFT ? -off : index == DPAD_RIGHT ? off : 0);
        *y = t->ctl[TC_STICK].y + (index == DPAD_UP ? -off : index == DPAD_DOWN ? off : 0);
        return 1;
    }
    Box b = kind == TOUCH_TARGET_PAUSE ? pause_row_box(t, index) : kind == TOUCH_TARGET_CONFIRM ? confirm_box(t, index)
          : kind == TOUCH_TARGET_PANEL ? panel_box(t, index) : kind == TOUCH_TARGET_CODES ? code_box(t, index)
          : kind == TOUCH_TARGET_SKIP ? skip_box(t) : editor_box(t, index);
    *x = b.x, *y = b.y;
    return 1;
}

static int pause_row_at(const Touch *t, float x, float y)
{
    if (t->pause == PAUSE_CODES) {
        for (int b = 0; b < CODE_BUTTONS; b++) if (in_box(code_box(t, b), x, y)) return b;
        return -1;
    }
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

/* A tap on the CODES page: a press typed, CLEAR, BACK (to the rows), ENTER: the Resume row's
   presses, the quiet lead, then each press typed (PRESS_FIELDS down, PRESS_FIELDS up: one press
   each for the game's reader, func_001E6F58, at any frame rate), the race touches let go. */
static void code_tapped(Touch *t, int b)
{
    if (b >= 0 && b < CODE_CLEAR) {
        if (t->code_len < CODE_PRESSES) t->code[t->code_len++] = (uint8_t)b, buzz(t, TOUCH_BUZZ_TICK);
        return;
    }
    if (b == CODE_CLEAR) t->code_len = 0;
    else if (b == CODE_BACK) t->code_len = 0, t->pause = PAUSE_OPEN;
    else if (b == CODE_ENTER && t->code_len > 0 && !seq_busy(t) && t->game.pause_row[ROW_RESUME] >= 0) {
        seq_cursor_to(t, t->game.pause_row[ROW_RESUME]);
        seq_add(t, PAD_CROSS);
        seq_wait(t, CODE_LEAD_FIELDS);
        for (int i = 0; i < t->code_len; i++) seq_add(t, CODE_PAD[t->code[i]]);
        t->code_len = 0;
        race_resumes(t);
        drop_race_touches(t);   /* a held PEDAL would join the code's presses (the reader ORs them in) */
        t->code_sending = 1;
    }
}

static void pause_row_tapped(Touch *t, int row)
{
    if (t->pause == PAUSE_CODES) { code_tapped(t, row); return; }
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
    case ROW_CODES:
        t->pause = PAUSE_CODES;
        t->code_len = 0;
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

static TouchEdit *edit_of(Touch *t, int c) { return &t->set.edit[t->set.preset == TOUCH_CLASSIC][c]; }

/* A control's half extents for the editor's rules (the trick stick's faint spot: its 14 mm centre). */
static void rule_half(const Touch *t, int c, float *rx, float *ry)
{
    const Ctl *k = &t->ctl[c];
    *rx = k->box ? k->hw : k->r, *ry = k->box ? k->hh : k->r;
    if (c == TC_TRICK) *rx = *ry = 7;
}

/* The gap between two controls' drawn shapes (negative: they overlap). */
static float shape_gap(const Ctl *k, const Ctl *m)
{
    if (!k->box && !m->box) return dist(k->x, k->y, m->x, m->y) - k->r - m->r;
    float ax = fabsf(k->x - m->x) - (k->box ? k->hw : k->r) - (m->box ? m->hw : m->r);
    float ay = fabsf(k->y - m->y) - (k->box ? k->hh : k->r) - (m->box ? m->hh : m->r);
    return fmaxf(ax, ay);
}

/* A control's touch area enters one of PEDAL's flick lanes (Thumb zones with flick attacks). */
static int in_flick_lane(const Touch *t, int c)
{
    const Ctl *k = &t->ctl[c], *p = &t->ctl[TC_PEDAL];
    float r = (k->box ? fmaxf(k->hw, k->hh) : k->r) * AREA_GROW;
    for (int i = 0; i <= 16; i++) {
        float a = i * PI_F / 8, qx = k->x + (i < 16 ? r * cosf(a) : 0), qy = k->y + (i < 16 ? r * sinf(a) : 0);
        float dx = qx - p->x, dy = qy - p->y, d = hypotf(dx, dy);
        if (d <= p->r || d > FLICK_REACH_MM) continue;
        if (fabsf(dy) <= fabsf(dx) * tanf(FLICK_LANE_DEG * PI_F / 180)) return 1;
    }
    return 0;
}

static int flick_lanes(const Touch *t)
{
    return t->set.preset == TOUCH_THUMB_ZONES && t->set.attacks == TOUCH_ATTACK_FLICK && t->ctl[TC_PEDAL].on;
}

/* The editor's rules for the control the player moves or resizes (finding 226; finding 141 had a
   3 mm gap between every two controls): wholly on the screen; the steering stick's home and the
   trick stick's spot out of the gesture strips (a drag starting there is the system's); no
   overlap with another control's drawn shape (what the player sees is what a touch presses: a
   touch in two areas goes to the nearer, so overlapping shapes would steal each other's
   presses); nothing in PEDAL's flick lanes (a flick would press it); and not under the editor's
   own bar, which must stay usable. The default layouts keep their 3 mm gaps; the rules only
   check the edited control, so a layout another rule (the cutout, the Size setting) bent does
   not lock the editor. */
static int steering_kept(const Touch *t, int c);

static int control_fits(const Touch *t, int c)
{
    const Ctl *k = &t->ctl[c];
    if (!k->on) return 1;
    float W = t->screen.width, H = t->screen.height, rx, ry;
    rule_half(t, c, &rx, &ry);
    if (k->x - rx < -0.01f || k->x + rx > W + 0.01f || k->y - ry < -0.01f || k->y + ry > H + 0.01f) return 0;
    if ((c == TC_STICK || c == TC_TRICK) && in_strip(t, k->x, k->y)) return 0;
    if (t->editor)
        for (int b = 0; b < EDIT_BUTTONS; b++) {
            Box x = editor_box(t, b);
            if (fabsf(k->x - x.x) < rx + x.hw && fabsf(k->y - x.y) < ry + x.hh) return 0;
        }
    if (c == TC_STICK) return steering_kept(t, c);   /* the D-pad's home: under no button */
    if (c == TC_TRICK || c == TC_PLUS_LEFT || c == TC_PLUS_RIGHT) return 1;
    for (int d = 0; d < TC_COUNT; d++) {
        const Ctl *m = &t->ctl[d];
        if (d == c || !m->on || d == TC_STICK || d == TC_TRICK || d == TC_PLUS_LEFT || d == TC_PLUS_RIGHT) continue;
        if (shape_gap(k, m) < 0) return 0;
    }
    if (flick_lanes(t)) {
        if (c != TC_PEDAL && in_flick_lane(t, c)) return 0;
        if (c == TC_PEDAL)
            for (int d = 0; d < TC_COUNT; d++)
                if (d != TC_PEDAL && d != TC_STICK && d != TC_TRICK && t->ctl[d].on && in_flick_lane(t, d)) return 0;
    }
    return steering_kept(t, c);
}

/* Steering stays usable (the review of 2026-11-13; finding 229): with the stick, some square of
   STEER_FREE_MM inside the steering area that no other control's touch area covers (routing gives a
   touch to a button first, so a button over the area takes that part); with the D-pad, its rest
   point and its four arms on the screen, out of the gesture strips and under no other control.
   `skip`: a control left out (-1 none). */
enum { STEER_FREE_MM = 10, STEER_GRID = 512 };
static int taken_by(const Touch *t, float x, float y, int skip)
{
    for (int c = 0; c < TC_COUNT; c++) {
        const Ctl *k = &t->ctl[c];
        if (c == skip || c == TC_STICK || c == TC_TRICK || !k->on) continue;
        if (c == TC_PLUS_LEFT || c == TC_PLUS_RIGHT) {   /* classic_face_at's 3.5 mm */
            if (dist(x, y, k->x, k->y) <= 3.5f) return 1;
            continue;
        }
        if (hit(t, c, x, y, AREA_GROW) < 1e8f) return 1;
    }
    return 0;
}

static int steering_ok_without(const Touch *t, int skip)
{
    const TouchScreen *sc = &t->screen;
    if (t->set.dpad) {
        const Ctl *k = &t->ctl[TC_STICK];
        const TouchEdit *e = &t->set.edit[t->set.preset == TOUCH_CLASSIC][TC_STICK];
        float off = dpad_off(t, e);
        for (int a = -1; a < DPAD_ARMS; a++) {
            float x = k->x + (a == DPAD_LEFT ? -off : a == DPAD_RIGHT ? off : 0), y = k->y + (a == DPAD_UP ? -off : a == DPAD_DOWN ? off : 0);
            if (x < 0 || x > sc->width || y < 0 || y > sc->height || in_strip(t, x, y) || taken_by(t, x, y, skip)) return 0;
        }
        return 1;
    }
    float x0 = fmaxf(t->steer_x0, 0), x1 = fminf(t->steer_x1, sc->width);
    float y0 = fmaxf(t->steer_y0, 0), y1 = fminf(t->steer_y1, sc->height);
    int nx = (int)(x1 - x0), ny = (int)(y1 - y0);
    if (nx < STEER_FREE_MM || ny < STEER_FREE_MM) return 0;
    if (nx > STEER_GRID) nx = STEER_GRID;
    if (ny > STEER_GRID) ny = STEER_GRID;
    /* 1 mm cells: each column's run of free cells up to this row; a row with STEER_FREE_MM columns in
       a row whose runs reach STEER_FREE_MM holds a free square */
    unsigned short run[STEER_GRID];
    memset(run, 0, sizeof run);
    for (int j = 0; j < ny; j++) {
        int across = 0;
        for (int i = 0; i < nx; i++) {
            float x = x0 + i + 0.5f, y = y0 + j + 0.5f;
            run[i] = in_strip(t, x, y) || taken_by(t, x, y, skip) ? 0 : (unsigned short)(run[i] + 1);
            across = run[i] >= STEER_FREE_MM ? across + 1 : 0;
            if (across >= STEER_FREE_MM) return 1;
        }
    }
    return 0;
}

static int steering_ok(const Touch *t) { return steering_ok_without(t, -1); }

/* The edited control c leaves steering usable: refused only when c is what takes it away (a layout
   another rule bent, or a screen too small, never locks the editor). */
static int steering_kept(const Touch *t, int c)
{
    return steering_ok(t) || !steering_ok_without(t, c);
}

/* The control's edit moved so it lies on the screen (a control larger than the screen: centred). */
static void keep_on_screen(Touch *t, int c)
{
    const Ctl *k = &t->ctl[c];
    float W = t->screen.width, H = t->screen.height, rx, ry;
    rule_half(t, c, &rx, &ry);
    float x = 2 * rx >= W ? W / 2 : clampf(k->x, rx, W - rx), y = 2 * ry >= H ? H / 2 : clampf(k->y, ry, H - ry);
    if (x == k->x && y == k->y) return;
    TouchEdit *e = edit_of(t, c);
    e->dx += (x - k->x) * (right_handed(t) ? 1.0f : -1.0f);   /* edits are stored as the right hand has them */
    e->dy += y - k->y;
    lay_out(t, t->set.hud_moved);
}

/* The control's size, as near `want` as fits (TOUCH_SIZE_MIN to TOUCH_SIZE_MAX, kept on the
   screen: a control grown at an edge moves inward). Returns the size it got. */
static float size_set(Touch *t, int c, float want)
{
    TouchEdit *e = edit_of(t, c), start = *e;
    want = clampf(want, TOUCH_SIZE_MIN, TOUCH_SIZE_MAX);
    float lo = start.size, hi = want;
    for (int i = 0; i < 14; i++) {
        float size = i == 0 ? want : (lo + hi) / 2;
        *e = start;
        e->size = size;
        lay_out(t, t->set.hud_moved);
        keep_on_screen(t, c);
        if (control_fits(t, c)) {
            if (i == 0) return size;
            lo = size;
        } else hi = size;
        if (i > 0 && fabsf(hi - lo) < 0.005f) break;
    }
    *e = start;
    e->size = lo;
    lay_out(t, t->set.hud_moved);
    keep_on_screen(t, c);
    if (!control_fits(t, c)) { *e = start; lay_out(t, t->set.hud_moved); }
    return e->size;
}

/* - and +: 5 % steps up to 100 %, 10 % to 200 %, 25 % above (25 % to 600 % in 37 steps; a held
   button repeats). */
static void size_step(Touch *t, int dir)
{
    if (t->edit_selected < 0 || (t->edit_selected == TC_STICK && !t->set.dpad)) return;   /* the stick's home has no size */
    float now_size = edit_of(t, t->edit_selected)->size;
    float probe = dir > 0 ? now_size + 0.001f : now_size - 0.001f;
    float step = probe < 1 ? 0.05f : probe < 2 ? 0.10f : 0.25f;
    float want = dir > 0 ? floorf(now_size / step + 1.001f) * step : ceilf(now_size / step - 1.001f) * step;
    size_set(t, t->edit_selected, want);
}

/* ---- the history: UNDO_STEPS snapshots of the preset's edits, undo_at the one shown ---- */

static void history_take(Touch *t, EditStep *step)
{
    int p = t->set.preset == TOUCH_CLASSIC;
    memcpy(step->e, t->set.edit[p], sizeof step->e);
    step->legacy = t->set.legacy[p];
}

static void history_start(Touch *t)
{
    history_take(t, &t->undo[0]);
    t->undo_count = 1, t->undo_at = 0;
}

/* After a finished change (a gesture, a - / + run, RESET): one step, if anything changed. */
static void history_push(Touch *t)
{
    EditStep now_step;
    history_take(t, &now_step);
    if (t->undo_count > 0 && !memcmp(&now_step, &t->undo[t->undo_at], sizeof now_step)) return;
    t->undo_count = t->undo_at + 1;   /* a new change drops what was undone */
    if (t->undo_count == UNDO_STEPS) {
        memmove(t->undo, t->undo + 1, sizeof *t->undo * (UNDO_STEPS - 1));
        t->undo_count--;
    }
    t->undo[t->undo_count++] = now_step;
    t->undo_at = t->undo_count - 1;
}

static void history_show(Touch *t, int at)
{
    int p = t->set.preset == TOUCH_CLASSIC;
    t->undo_at = at;
    memcpy(t->set.edit[p], t->undo[at].e, sizeof t->undo[at].e);
    t->set.legacy[p] = t->undo[at].legacy;
    lay_out(t, t->set.hud_moved);
    if (t->edit_selected >= 0 && !editable(t, t->edit_selected)) t->edit_selected = -1;
}

static int can_undo(const Touch *t) { return t->undo_at > 0; }
static int can_redo(const Touch *t) { return t->undo_at + 1 < t->undo_count; }

static void editor_open(Touch *t, int alone)
{
    t->editor = 1;
    t->editor_alone = alone;
    t->editor_tab = EDIT_GROUND;
    t->edit_selected = t->set.preset == TOUCH_CLASSIC ? TC_CROSS : TC_PEDAL;
    t->pinch_finger = t->repeat_finger = -1;
    drop_race_touches(t);
    history_start(t);
}

void touch_open_editor(Touch *t) { editor_open(t, 0); }
void touch_open_editor_alone(Touch *t) { editor_open(t, 1); }
int touch_editor_open(const Touch *t) { return t->editor; }
int touch_tablet(const Touch *t) { return t->tablet; }

static void editor_close(Touch *t)
{
    t->editor = 0;
    t->pinch_finger = t->repeat_finger = -1;
    t->saved = 1;
}

static void editor_button(Touch *t, int b)
{
    switch (b) {
    case EDIT_GROUND: t->editor_tab = EDIT_GROUND; t->edit_selected = t->set.preset == TOUCH_CLASSIC ? TC_CROSS : TC_PEDAL; break;
    case EDIT_AIR:
        if (t->set.preset == TOUCH_THUMB_ZONES) t->editor_tab = EDIT_AIR, t->edit_selected = TC_TRICK;
        break;
    case EDIT_MINUS: case EDIT_PLUS:
        size_step(t, b == EDIT_PLUS ? 1 : -1);
        history_push(t);
        break;
    case EDIT_UNDO: if (can_undo(t)) history_show(t, t->undo_at - 1); break;
    case EDIT_REDO: if (can_redo(t)) history_show(t, t->undo_at + 1); break;
    case EDIT_RESET: {
        /* every control of the preset back to its default, on the current places (a 0.8.0
           layout's own places go with its edits); one step, so UNDO brings them back */
        for (int c = 0; c < TC_COUNT; c++) *edit_of(t, c) = (TouchEdit){0, 0, 1};
        t->set.legacy[t->set.preset == TOUCH_CLASSIC] = 0;
        lay_out(t, t->set.hud_moved);
        history_push(t);
        break;
    }
    case EDIT_DONE: editor_close(t); break;
    }
}

/* The finger holding a control in the editor (-1 none). */
static int holding_finger(const Touch *t, const Finger *except)
{
    for (int i = 0; i < TOUCH_FINGERS; i++) {
        const Finger *g = &t->f[i];
        if (g != except && g->used && g->job == JOB_EDIT && g->control >= 0 && i != t->pinch_finger) return i;
    }
    return -1;
}

static void editor_down(Touch *t, Finger *f)
{
    f->job = JOB_EDIT;
    f->control = -1;
    f->target = -1;
    for (int b = 0; b < EDIT_BUTTONS; b++)
        if (in_box(editor_box(t, b), f->x, f->y)) {
            f->target = b;
            if (b == EDIT_MINUS || b == EDIT_PLUS)   /* held: repeats (editor_tick) */
                t->repeat_finger = (int)(f - t->f), t->repeat_at = t->now + REPEAT_FIRST_MS, t->repeated = 0;
            return;
        }
    /* a second finger while one holds a control: the two pinch it larger or smaller */
    int held = holding_finger(t, f);
    if (held >= 0 && t->pinch_finger < 0) {
        const Finger *g = &t->f[held];
        t->pinch_finger = (int)(f - t->f);
        t->pinch_d0 = fmaxf(dist(f->x, f->y, g->x, g->y), 5.0f);
        t->pinch_size0 = edit_of(t, g->control)->size;
        return;
    }
    float best = 1e9f;
    for (int c = 0; c < TC_COUNT; c++) {
        if (!editable(t, c)) continue;
        float out = hit(t, c, f->x, f->y, c == TC_TRICK ? 1.0f : AREA_GROW);
        if (out < best) best = out, f->control = c;
    }
    if (f->control >= 0) {
        t->edit_selected = f->control;
        t->edit_before = t->edit_ok = *edit_of(t, f->control);
        f->grab_dx = t->ctl[f->control].x - f->x;
        f->grab_dy = t->ctl[f->control].y - f->y;
    }
}

static void editor_move(Touch *t, Finger *f)
{
    int i = (int)(f - t->f);
    if (i == t->pinch_finger || (t->pinch_finger >= 0 && f->control >= 0)) {   /* the pinch */
        const Finger *g = &t->f[t->pinch_finger];
        int held = holding_finger(t, NULL);
        if (held >= 0 && g->used) {
            const Finger *h = &t->f[held];
            float d = dist(g->x, g->y, h->x, h->y);
            size_set(t, h->control, t->pinch_size0 * d / t->pinch_d0);
            t->edit_ok = *edit_of(t, h->control);
        }
        if (i == t->pinch_finger) return;
    }
    if (f->control < 0) return;
    TouchEdit *e = edit_of(t, f->control);
    float want_x = f->x + f->grab_dx, want_y = f->y + f->grab_dy;
    const Ctl *k = &t->ctl[f->control];
    float sx = right_handed(t) ? 1.0f : -1.0f;   /* edits are stored as the right hand has them */
    e->dx += (want_x - k->x) * sx;
    e->dy += want_y - k->y;
    lay_out(t, t->set.hud_moved);
    keep_on_screen(t, f->control);   /* dragged past an edge: it stays against it */
    if (control_fits(t, f->control)) t->edit_ok = *e;
}

static void editor_up(Touch *t, Finger *f)
{
    int i = (int)(f - t->f);
    if (i == t->repeat_finger) t->repeat_finger = -1;
    if (f->target >= 0) {
        int ran = (f->target == EDIT_MINUS || f->target == EDIT_PLUS) && t->repeated;
        if (ran) history_push(t);   /* the run is one step */
        else if (in_box(editor_box(t, f->target), f->x, f->y)) editor_button(t, f->target);
        return;
    }
    if (i == t->pinch_finger) {
        t->pinch_finger = -1;
        if (holding_finger(t, f) < 0) history_push(t);
        return;
    }
    if (f->control >= 0) {
        /* a drop where it does not fit: back to the last place it fitted on the way */
        if (!control_fits(t, f->control)) {
            *edit_of(t, f->control) = t->edit_ok;
            lay_out(t, t->set.hud_moved);
        }
        if (t->pinch_finger >= 0) t->pinch_finger = -1;
        history_push(t);
    }
}

/* Once a pad read while the editor shows: a held - or + repeats. */
static void editor_tick(Touch *t)
{
    if (t->repeat_finger < 0) return;
    const Finger *f = &t->f[t->repeat_finger];
    if (!f->used || f->job != JOB_EDIT || (f->target != EDIT_MINUS && f->target != EDIT_PLUS)) { t->repeat_finger = -1; return; }
    if (!in_box(editor_box(t, f->target), f->x, f->y)) return;   /* slid off: waits */
    while (t->now >= t->repeat_at) {
        size_step(t, f->target == EDIT_PLUS ? 1 : -1);
        t->repeated = 1;
        t->repeat_at += REPEAT_MS;
    }
}

/* ---------------------------------------------------------------- touches */

static void end_finger_job(Touch *t, Finger *f)
{
    switch (f->job) {
    case JOB_STICK:
        if (t->stick_finger == (int)(f - t->f)) t->stick_finger = -1, t->stick_dx = t->stick_dy = 0;
        break;
    case JOB_DPAD:
        if (t->dpad_finger == (int)(f - t->f)) t->dpad_finger = -1, t->dpad_buttons = 0;
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

static void stick_follow(Touch *t, Finger *f);

/* A touch takes the steering stick: a floating stick centres under it (no deflection yet), a fixed
   one deflects at once by where it landed (a thumb held still off-centre steers without a move). */
static void stick_begins(Touch *t, Finger *f)
{
    f->job = JOB_STICK;
    t->stick_finger = (int)(f - t->f);
    if (t->set.stick_fixed) t->stick_cx = t->ctl[TC_STICK].x, t->stick_cy = t->ctl[TC_STICK].y;
    else t->stick_cx = f->x0, t->stick_cy = f->y0;
    stick_follow(t, f);
}

/* The D-pad (finding 214): the directions a thumb at x, y presses. Nothing inside the dead zone
   round the rest point; else every direction whose axis is within DPAD_REACH_DEG of the thumb's
   angle, which is one direction near an axis and two neighbours between them, never opposite ones
   (two axes 180° apart can't both be within 71.5°). `held`, the directions pressed now, keep a few
   degrees more than the others, so a thumb on a band's edge doesn't flap. */
static uint16_t dpad_read(const Touch *t, float x, float y, uint16_t held)
{
    const Ctl *k = &t->ctl[TC_STICK];
    const TouchEdit *e = &t->set.edit[t->set.preset == TOUCH_CLASSIC][TC_STICK];
    float dead = DPAD_DEAD * (dpad_off(t, e) - dpad_half(t, e));
    float dx = x - k->x, dy = y - k->y;
    if (hypotf(dx, dy) < (held ? dead * 0.8f : dead)) return 0;
    float angle = atan2f(-dy, dx);   /* y up */
    uint16_t out = 0;
    for (int a = 0; a < DPAD_ARMS; a++) {
        float off = fabsf(angle_diff(angle, DPAD_AXIS_DEG[a] * PI_F / 180)) * 180 / PI_F;
        if (off < DPAD_REACH_DEG + ((held & DPAD_PAD[a]) ? DPAD_STICKY_DEG : -DPAD_STICKY_DEG)) out |= DPAD_PAD[a];
    }
    return out;
}

static void dpad_set(Touch *t, const Finger *f)
{
    uint16_t now = dpad_read(t, f->x, f->y, t->dpad_buttons), fresh = now & ~t->dpad_buttons;
    if (fresh) buzz(t, TOUCH_BUZZ_TICK);   /* a direction came */
    for (int a = 0; a < DPAD_ARMS; a++)
        if (fresh & DPAD_PAD[a]) {
            if (t->dpad_out & DPAD_PAD[a]) t->dpad_gap |= DPAD_PAD[a];   /* still showing: up for a field first */
            t->dpad_latched |= DPAD_PAD[a];
            t->dpad_until[a] = 0;   /* counted from the first field that sends it */
        }
    t->dpad_buttons = now;
}

/* Each update, for the pad read after it: the D-pad's directions. A thumb's press shows while it is
   down and at least PRESS_FIELDS (a tap between two pad reads would be lost: the game's double tap,
   func_0017EDE8, wants a press, a field up and a press inside 30 fields); a direction pressed again
   while its last press still shows is let up for one field first. */
static void dpad_field(Touch *t)
{
    uint16_t live = t->dpad_finger >= 0 ? t->dpad_buttons : 0, out = 0;
    for (int a = 0; a < DPAD_ARMS; a++) {
        uint16_t bit = DPAD_PAD[a];
        if (t->dpad_gap & bit) { t->dpad_gap &= (uint16_t)~bit; continue; }
        if (t->dpad_latched & bit) {
            static const int opposite[DPAD_ARMS] = {DPAD_DOWN, DPAD_UP, DPAD_RIGHT, DPAD_LEFT};
            int shown = t->dpad_until[a] != 0;
            if (!shown) t->dpad_until[a] = t->game.vsync + press_vsyncs(t, PRESS_FIELDS);
            /* a thumb still down that slid off it (shown once), or onto the opposite arm: no hold,
               so a slide never sends opposite directions together */
            int slid = t->dpad_finger >= 0 && !(live & bit) && shown;
            if ((int32_t)(t->game.vsync - t->dpad_until[a]) >= 0 || slid || (live & DPAD_PAD[opposite[a]]))
                t->dpad_latched &= (uint16_t)~bit;
            else out |= bit;
        }
        out |= live & bit;
    }
    t->dpad_out = out;
}

/* The steering touch of a new finger: the D-pad when it steers (a thumb on or near the pad: its
   touch area is the pad grown 20 %), else the stick (the steering area). 1 when taken. */
static int steer_begins(Touch *t, Finger *f)
{
    float x = f->x0, y = f->y0;
    if (in_strip(t, x, y)) return 0;
    if (t->set.dpad) {
        const Ctl *k = &t->ctl[TC_STICK];
        if (t->dpad_finger >= 0 || dist(x, y, k->x, k->y) > k->r * AREA_GROW) return 0;
        f->job = JOB_DPAD;
        t->dpad_finger = (int)(f - t->f);
        t->dpad_buttons = 0;
        dpad_set(t, f);
        return 1;
    }
    if (!in_rect(x, y, t->steer_x0, t->steer_y0, t->steer_x1, t->steer_y1) || t->stick_finger >= 0) return 0;
    stick_begins(t, f);
    return 1;
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
        press_begins(t, f);
        if (f->sent) buzz(t, TOUCH_BUZZ_TICK);
        return;
    }
    if (c >= 0) {
        f->job = JOB_BUTTON;
        f->control = c;
        if (c == TC_PAUSE) { f->buttons = 0; pause_pressed(t); return; }
        f->buttons = c == TC_CAMERA ? PAD_SELECT : c == TC_SQUARE ? PAD_SQUARE : PAD_CIRCLE;
        press_begins(t, f);
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
        press_begins(t, f);
        if (c == TC_HOP) t->last_hop_race = race_ms(t);
        buzz(t, TOUCH_BUZZ_TICK);
        return;
    }
    if (steer_begins(t, f)) return;
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
        press_begins(t, f);
        buzz(t, TOUCH_BUZZ_TICK);
        return;
    }
    if (c >= 0) {
        f->job = JOB_BUTTON;
        f->control = c;
        if (c == TC_PAUSE) { f->buttons = 0; pause_pressed(t); return; }
        f->buttons = c == TC_CAMERA ? PAD_SELECT : c == TC_L3 ? PAD_L3 : c == TC_R3 ? PAD_R3 : bar_buttons(t, c, x);
        press_begins(t, f);
        buzz(t, TOUCH_BUZZ_TICK);
        return;
    }
    if (steer_begins(t, f)) return;
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
    f->order = ++t->downs;
    f->x0 = f->x = x, f->y0 = f->y = y, f->t0 = now;
    f->control = -1;
    f->target = -1;
    f->started_race = race_ms(t);
    history_add(f, x, y, now);
    if (t->hidden_by_pad) {   /* the touch that brings the overlay back is never sent */
        t->hidden_by_pad = 0;
        f->job = JOB_IGNORED;
        if (race_on_screen(t) || flyover_on(t)) return;
    }
    if (t->editor) { editor_down(t, f); return; }
    if (!touch_visible(t)) { f->job = JOB_IGNORED; return; }
    if (flyover_on(t)) {   /* only SKIP: a finger already down when the flight began never was one */
        f->job = skip_hit(t, x, y) ? JOB_SKIP : JOB_IGNORED;
        return;
    }
    if (t->pause != PAUSE_NONE) {   /* only the rows (and Pause, Back) send input */
        static const int pause_button[] = {TC_PAUSE};
        f->job = JOB_PANEL;
        f->target = t->pause == PAUSE_CONTROLS ? 0 : pause_row_at(t, x, y);
        if (t->pause == PAUSE_OPEN && f->target < 0 && control_at(t, x, y, pause_button, 1) == TC_PAUSE) f->target = PAUSE_BUTTON;
        return;
    }
    if (t->code_sending) { f->job = JOB_IGNORED; return; }   /* a code's presses go (about a second) */
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
    case JOB_DPAD: if (t->dpad_finger == (int)(f - t->f)) dpad_set(t, f); break;
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
    else if (f->job == JOB_SKIP) {   /* fires on the lift, on the button it started on (a drag off it cancels) */
        if (flyover_on(t) && skip_hit(t, x, y)) {
            pulse(t, PAD_START, PRESS_FIELDS);
            t->skip_pulse = 1;
            buzz(t, TOUCH_BUZZ_TICK);
        }
    } else if (f->job == JOB_TRICK && t->freeze && !t->trick.held) t->freeze = 0;   /* a lift without a push */
    else if (f->job == JOB_AIR_WAIT && !paused(t)) {
        /* a tap that ended in the air layout's wait: the ground layout was still on screen, so it
           was that button's (the flag also comes over bumps); a short press */
        static const int ground[] = {TC_HOP, TC_BRAKE, TC_SLIDE};
        int c = control_at(t, f->x0, f->y0, ground, 3);
        if (c >= 0) pulse(t, c == TC_HOP ? PAD_R2 : c == TC_BRAKE ? PAD_TRIANGLE : PAD_L3, PRESS_FIELDS);
        if (c == TC_HOP) t->last_hop_race = race_ms(t);
    } else if ((f->job == JOB_BUTTON || f->job == JOB_PEDAL) && f->buttons && !f->shown && !paused(t) && press_lifts(t, f)) {
        /* the game hasn't read this press yet (a tap between two reads, a stall, a held-back or
           waiting press): the finger stays, lifted, and its press goes as a short one; its release
           then comes in a field of its own, so ✕'s and SELECT's windows count what the game saw */
        f->lifted = 1;
        return;
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
    if (t->editor) { editor_close(t); return; }
    if (t->pause == PAUSE_CONTROLS) { t->pause = PAUSE_OPEN; return; }   /* the overlay's own page */
    if (t->pause == PAUSE_CODES) { code_tapped(t, CODE_BACK); return; }
    /* the question's No; while the presses that opened it still go, nothing (as a tap): the game
       has not shown it yet, so the overlay stays on it and the next Back answers */
    if (t->pause == PAUSE_CONFIRM) { pause_row_tapped(t, CONFIRM_NO); return; }
    if (t->pause == PAUSE_OPEN) { pause_row_tapped(t, ROW_RESUME); return; }
    if (t->pause == PAUSE_ASKED) return;
    if (t->pause == PAUSE_PAD) { if (!seq_busy(t)) seq_add(t, PAD_TRIANGLE); return; }   /* the menu's back, as in menus */
    if (race_on_screen(t)) { pause_pressed(t); return; }
    if (!seq_busy(t)) seq_add(t, PAD_TRIANGLE);   /* in menus Back is △ */
}

void touch_pad_used(Touch *t, double now)
{
    t->now = now;
    if (t->pause == PAUSE_OPEN || t->pause == PAUSE_CONTROLS || t->pause == PAUSE_CONFIRM || t->pause == PAUSE_CODES) {
        /* the controller works the game's menu from here (moves its cursor, answers the question):
           the rows would no longer say where the game is, and the overlay's own presses stop */
        t->pause = PAUSE_PAD;
        seq_reset(t);
    }
    if (t->code_sending) seq_reset(t), t->code_sending = 0;   /* a code going: the controller's now */
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

/* The oldest queued move whose finger lifts or presses again later in the queue (before any
   cancel) goes: that up or down says where the finger was. 1 when one went. */
static int queue_drop_old_move(TouchQueue *q)
{
    for (int i = 0; i < q->count; i++) {
        const TouchEvent *e = &q->ev[i];
        if (e->kind != TOUCH_EV_MOVE) continue;
        for (int j = i + 1; j < q->count; j++) {
            const TouchEvent *n = &q->ev[j];
            if (n->kind == TOUCH_EV_CANCEL) break;
            if (n->id != e->id || !finger_event(n->kind)) continue;
            if (n->kind == TOUCH_EV_MOVE) break;   /* not after merging; keep it */
            memmove(q->ev + i, q->ev + i + 1, sizeof *q->ev * (size_t)(q->count - i - 1));
            q->count--;
            q->merged++;
            return 1;
        }
    }
    return 0;
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
            if (o->kind == TOUCH_EV_MOVE) { o->x = x, o->y = y, o->ms = ms; q->merged++; return; }
            break;
        }
        /* none (the finger pressed in the stall and this is its first move): it stays, or a thumb
           held still would never steer. A move its finger's later up or down makes old gives its
           place, else it takes one of the reserve (once a finger: its next moves rewrite it) */
        if (queue_drop_old_move(q) || q->count < TOUCH_QUEUE) q->ev[q->count++] = e;
        return;   /* the queue is all presses: the finger's up still says where it ended */
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

static int pressed_control(const Touch *t, int c);

/* The action pictures' ground or air (finding 229). The air flag has to hold for the air wait (the
   one Thumb zones' air layout waits) before the air pictures show, and the ground for
   TOUCH_GROUND_MS before the ground ones come back, so a bump or a rail's edge does not flicker
   them; a control under a finger keeps the picture it had when the finger came down. */
static void pictures_step(Touch *t)
{
    const TouchGame *g = &t->game;
    int on = race_on_screen(t), air = on && g->air;
    if (air != t->pic_flag || g->race_ms < t->pic_flag_since) t->pic_flag = air, t->pic_flag_since = g->race_ms;
    int wait = air ? (t->set.air_wait_ms > 0 ? t->set.air_wait_ms : 150) : TOUCH_GROUND_MS;
    if (!on) t->pic_air = 0;
    else if (g->race_ms - t->pic_flag_since >= wait - 0.5) t->pic_air = air;
    for (int c = 0; c < TC_COUNT; c++)
        if (!on || !pressed_control(t, c)) t->pic_air_ctl[c] = (unsigned char)t->pic_air;
}

int touch_air_picture(const Touch *t, int control)
{
    return control >= 0 && control < TC_COUNT && !t->editor && t->pic_air_ctl[control];
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
    } else if (t->pause != PAUSE_NONE && t->pause != PAUSE_ASKED && g->clock_moving && !seq_busy(t)) {
        race_resumes(t);   /* closed some other way (a controller resumed, restarted, answered the question) */
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
    if (g->online || t->set.preset != TOUCH_THUMB_ZONES || !touch_visible(t) || t->editor || flyover_on(t)) { t->hint = 0; return; }
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
    t->replay_ran = 0;   /* a replay started again: its countdown first */
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

/* Each update, first: presses waiting for an older lifted press of their buttons start once that
   one is gone (its release was in the last field's pad), by their own rule; then the lifted
   presses that were in PRESS_FIELDS pad reads end (or at once while paused: the pause took them). */
static void lifted_presses_step(Touch *t)
{
    for (int i = 0; i < TOUCH_FINGERS; i++) {
        Finger *f = &t->f[i];
        if (!f->used || !f->after_lift || lifted_before(t, f)) continue;
        f->after_lift = 0;
        press_rule(t, f);
        if (f->lifted && !press_lifts(t, f)) { end_finger_job(t, f); f->used = 0; continue; }
        if (f->sent) buzz(t, TOUCH_BUZZ_TICK);
    }
    for (int i = 0; i < TOUCH_FINGERS; i++) {
        Finger *f = &t->f[i];
        if (!f->used || !f->lifted || f->after_lift) continue;
        if (paused(t) || (f->shown && (int32_t)(t->game.vsync - f->lift_until) >= 0)) {
            end_finger_job(t, f);
            f->used = 0;
        }
    }
}

void touch_update(Touch *t, const TouchGame *game, double now)
{
    t->now = now;
    if (t->have_game && game->race_ms < t->game.race_ms) race_clock_back(t, game->race_ms - t->game.race_ms);
    int was_on = t->race_was_on;
    t->game = *game;
    t->have_game = 1;
    if (!game->replay) t->replay_ran = 0;
    else if (game->clock_moving) t->replay_ran = 1;
    if (game->race_hud) {
        if (!was_on) t->race_new = 1;
        t->hud_seen = now;
    }
    t->race_was_on = race_on_screen(t);
    if (t->editor) editor_tick(t);
    if (flyover_on(t) != t->fly_was) {
        t->fly_was = flyover_on(t);
        if (t->fly_was) {   /* a pause menu is gone once the race is loading again */
            if (t->pause != PAUSE_NONE) t->pause = PAUSE_NONE, seq_reset(t);
            t->panel_open = 0;
        } else {
            /* the flight is over (skipped or played out): a SKIP finger still down never skips
               the countdown, and a START press still going is taken back, so the countdown does
               not see it (it would pause the race) */
            for (int i = 0; i < TOUCH_FINGERS; i++)
                if (t->f[i].used && t->f[i].job == JOB_SKIP) t->f[i].job = JOB_IGNORED;
            if (t->skip_pulse)
                for (int i = 0; i < 16; i++)
                    if (t->pulse_buttons[i] == PAD_START) t->pulse_buttons[i] = 0, t->pulse_fields[i] = 0;
            t->skip_pulse = 0;
        }
    }
    lifted_presses_step(t);
    if (was_on && !t->race_was_on && t->pause == PAUSE_NONE && !t->editor) {   /* the race is gone (the editor's fingers are its own) */
        drop_race_touches(t);
        t->panel_open = 0;
        t->hold_cross_until = 0;
        t->hint = 0;
    }
    seq_advance(t);
    if (t->code_sending && (!seq_busy(t) || t->pause != PAUSE_NONE)) t->code_sending = 0;
    for (int i = 0; i < 16; i++)
        if (t->pulse_buttons[i] && !t->pulse_fields[i] && (int32_t)(game->vsync - t->pulse_until[i]) >= 0) t->pulse_buttons[i] = 0;
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
    pictures_step(t);
    if (t->trick.on) {
        Finger *f = &t->f[t->trick.finger];
        trick_step(t, f);
        trick_tick(t);
    }
    due_presses(t);
    cross_track(t);
    tilt_step(t);
    hints_step(t);
    /* the pad read after this update has these presses: a lifted one lasts PRESS_FIELDS from now,
       a pulse its fields */
    if (!paused(t) && touch_visible(t))
        for (int i = 0; i < TOUCH_FINGERS; i++) {
            Finger *f = &t->f[i];
            if (!f->used || !f->sent || f->shown) continue;
            f->shown = 1;
            if (f->lifted) f->lift_until = game->vsync + press_vsyncs(t, PRESS_FIELDS);
        }
    for (int i = 0; i < 16; i++)
        if (t->pulse_buttons[i] && t->pulse_fields[i]) {
            t->pulse_until[i] = game->vsync + press_vsyncs(t, t->pulse_fields[i]);
            t->pulse_fields[i] = 0;
        }
    if (!paused(t) && touch_visible(t) && !flyover_on(t)) dpad_field(t);
    else t->dpad_latched = t->dpad_gap = t->dpad_out = 0;   /* not the race's controls: nothing pending */
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
    if (!paused(t) && touch_visible(t) && !flyover_on(t)) {   /* the flight: only SKIP's START (a pulse) */
        for (int i = 0; i < TOUCH_FINGERS; i++) {
            const Finger *f = &t->f[i];
            if (f->used && f->sent && (f->job == JOB_PEDAL || f->job == JOB_BUTTON)) b |= f->buttons;
        }
        if (t->hold_cross_until > 0) b |= PAD_CROSS;
        if (t->trick.on) b |= (uint16_t)(t->trick.sent | (t->trick.triangle ? PAD_TRIANGLE : 0));
        if (t->stick_finger >= 0) sx = t->stick_dx, sy = t->stick_dy;
        b |= t->dpad_out;   /* the d-pad's own buttons: full lock, as a PS2 d-pad (dpad_field) */
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
const char *touch_take_note(Touch *t)
{
    if (!t->note_ready) return NULL;
    t->note_ready = 0;
    return t->note;
}
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
    t->stick_finger = t->dpad_finger = t->pinch_finger = t->repeat_finger = -1;
    t->cross_released = t->select_released = -1e9;
    t->cross_release_wall = -1e9;
    t->edit_selected = -1;
    lay_out(t, 1);
    return t;
}

void touch_free(Touch *t) { free(t); }

static void steer_rescue(Touch *t);

void touch_configure(Touch *t, const TouchSettings *s, const TouchScreen *screen)
{
    int preset_changed = s->preset != t->set.preset || s->dpad != t->set.dpad;   /* the steering touch ends too */
    if (preset_changed) t->dpad_latched = t->dpad_gap = t->dpad_out = 0;   /* no D-pad press outlives the D-pad */
    int hints = t->set.hints_shown;
    t->set = *s;
    if (t->saved) t->set.hints_shown |= hints;   /* a hint shown since the last save isn't lost */
    if (screen && screen->width > 10 && screen->height > 10) t->screen = *screen;
    if (preset_changed) drop_race_touches(t);
    lay_out(t, t->set.hud_moved);
    steer_rescue(t);
}

const TouchSettings *touch_settings(const Touch *t) { return &t->set; }

/* The editor's names of the controls (its readout and the steering rescue's line). */
static const char *const CONTROL_NAMES[TC_COUNT] = {
    "PEDAL", "HOP", "BRAKE", "SLIDE", "SQUARE", "CIRCLE", "CAMERA", "PAUSE", "CROSS", "TRIANGLE", "SQUARE", "CIRCLE",
    "+", "+", "L1 L2", "R1 R2", "L3", "R3", "STICK", "TRICK STICK"};

/* A loaded layout (dhd.ini by a build before this check, or edited by hand) whose edited controls
   leave no room to steer (steering_ok): it is not thrown away. The edited controls go back to their
   default places one at a time, in the controls' order, until steering is usable, for this run only:
   dhd.ini keeps the player's edits until the editor's Done saves the layout. The log says so once.
   If even the default places leave no room (a screen too small), nothing changes. */
static void steer_rescue(Touch *t)
{
    if (t->editor || steering_ok(t)) return;
    TouchEdit *edits = t->set.edit[t->set.preset == TOUCH_CLASSIC], kept[TC_COUNT];
    memcpy(kept, edits, sizeof kept);
    for (int c = 0; c < TC_COUNT; c++) edits[c] = (TouchEdit){0, 0, 1};
    lay_out(t, t->set.hud_moved);
    int defaults_ok = steering_ok(t);
    memcpy(edits, kept, sizeof kept);
    lay_out(t, t->set.hud_moved);
    if (!defaults_ok) return;
    char names[120] = "";
    for (int c = 0; c < TC_COUNT && !steering_ok(t); c++) {
        TouchEdit *e = &edits[c];
        if (c == TC_TRICK || (e->dx == 0 && e->dy == 0 && e->size == 1)) continue;
        *e = (TouchEdit){0, 0, 1};
        lay_out(t, t->set.hud_moved);
        size_t n = strlen(names);
        snprintf(names + n, sizeof names - n, "%s%s", n ? ", " : "", CONTROL_NAMES[c]);
    }
    if (!t->rescue_noted) {
        t->rescue_noted = 1;
        snprintf(t->note, sizeof t->note, "touch: the saved layout left no room to steer; at their default places for now: %s", names);
        t->note_ready = 1;
    }
}

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

/* Button Style Actions (finding 229): a control's picture (TI_*), drawn mirrored or not, and its
   badge, the pad's own symbol (TS_*) or name, so the game's prompts ("PRESS ✕", the trick lists)
   still map onto the touch buttons. */
typedef struct { int icon, mirror, symbol; const char *name; } Act;

static int actions(const Touch *t) { return t->set.style == TOUCH_STYLE_ACTIONS; }

static void icon(Draw *d, float x, float y, float half, int id, int mirror, uint32_t colour)
{
    put(d, TS_ICON, x, y, half, mirror ? -1.0f : 1.0f, (float)id, 0, 0, colour);
}

/* The badge: a small dark disc with the symbol or the name in white. */
static void badge(Draw *d, float x, float y, float r, int symbol, const char *name, float a)
{
    put(d, TS_DISC, x, y, r, 0, 0, 0, rgba(10, 14, 18, 0.92f * a), rgba(255, 255, 255, 0.8f * a));
    uint32_t w = rgba(255, 255, 255, a);
    if (symbol) put(d, symbol, x, y, r * 0.8f, 0, 0, 0, 0, w);
    else if (name && *name) text(d, x, y, fminf(r * 1.2f, r * 1.85f / (0.9f * (float)strlen(name))), 0, name, w);
}

/* A badge's radius on a round control of radius r: a fixed share, kept readable on small buttons and
   inside the control (its upper right, where neither thumb covers it and no neighbour is touched). */
static float badge_radius(float r)
{
    float br = clampf(r * 0.33f, 2.0f, 3.4f);
    return fminf(br, r * 0.48f);
}

/* A round control in Button Style Actions: the same disc, its picture (above its word, if any) and
   the badge. */
static void action_button(Draw *d, const Ctl *k, int pressed, float dim, Act act, const char *label)
{
    float a = d->alpha * dim, r = k->r;
    uint32_t fill = pressed ? rgba(166, 227, 154, 0.55f * a) : rgba(10, 14, 18, 0.45f * a);
    put(d, TS_DISC, k->x, k->y, r, 0, 0, 0, fill, rgba(255, 255, 255, 0.88f * a));
    int word = label && *label;
    float g = r * (word ? 0.40f : 0.58f);
    icon(d, k->x - r * (word ? 0.1f : 0.05f), word ? k->y - r * 0.18f : k->y + r * 0.05f, g, act.icon, act.mirror,
         rgba(255, 255, 255, 0.95f * a));
    if (word) text(d, k->x, k->y + r * 0.45f, fminf(r * 0.32f, 3.2f), 0, label, rgba(255, 255, 255, 0.95f * a));
    float br = badge_radius(r), off = (r - br * 1.02f) * 0.7071f;
    badge(d, k->x + off, k->y - off, br, act.symbol, act.name, a);
}

/* A round control in the style chosen: Symbols draws the pad's symbol (and the word), Actions the
   picture and the badge. */
static void styled_button(const Touch *t, Draw *d, const Ctl *k, int pressed, float dim, int symbol, const char *label, Act act)
{
    if (actions(t)) action_button(d, k, pressed, dim, act, label);
    else button(d, k, pressed, dim, symbol, label);
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
        uint32_t w = rgba(255, 255, 255, 0.95f * d->alpha);
        if (classic && actions(t)) {   /* the picture (the camera, Pause), its pad name under it */
            box_button(d, k->x, k->y, k->hw, k->hh, pressed_control(t, c), d->alpha, "", 0);
            icon(d, k->x, k->y - k->hh * 0.2f, k->hh * 0.5f, c == TC_CAMERA ? TI_CAMERA : TI_PAUSE, 0, w);
            const char *name = c == TC_CAMERA ? "SELECT" : "START";
            text(d, k->x, k->y + k->hh * 0.56f, fminf(k->hh * 0.32f, (2 * k->hw - 1.6f) / (0.9f * (float)strlen(name))), 0, name, w);
            continue;
        }
        box_button(d, k->x, k->y, k->hw, k->hh, pressed_control(t, c), d->alpha, classic ? (c == TC_CAMERA ? "SELECT" : "START") : "",
                   fminf(k->hh * 0.5f, 2.2f));
        if (classic) continue;
        if (c == TC_PAUSE) {
            put(d, TS_RECT, k->x - k->hw * 0.25f, k->y, k->hw * 0.12f, k->hh * 0.42f, 0.2f, 0, w, 0);
            put(d, TS_RECT, k->x + k->hw * 0.25f, k->y, k->hw * 0.12f, k->hh * 0.42f, 0.2f, 0, w, 0);
        } else {   /* a camera: body and lens */
            put(d, TS_RECT, k->x, k->y + k->hh * 0.05f, k->hw * 0.55f, k->hh * 0.36f, 0.5f, 0, 0, w);
            put(d, TS_RING, k->x, k->y + k->hh * 0.05f, k->hh * 0.2f, 0.35f, 0, 0, 0, w);
        }
    }
}

/* The D-pad: four boxes round a faint rest ring, each with an arrowhead (a wedge: the shapes have no
   rotation, and the 8x8 font's carets sit in the top half of their cell); a pressed arm goes green
   as the other buttons do. */
static void draw_dpad(const Touch *t, Draw *d)
{
    static const float wedge_mid[DPAD_ARMS] = {-PI_F / 2, PI_F / 2, 0, PI_F};   /* the wedge opens away from the point */
    static const float toward_x[DPAD_ARMS] = {0, 0, -1, 1}, toward_y[DPAD_ARMS] = {-1, 1, 0, 0};
    const Ctl *k = &t->ctl[TC_STICK];
    const TouchEdit *e = &t->set.edit[t->set.preset == TOUCH_CLASSIC][TC_STICK];
    float half = dpad_half(t, e), off = dpad_off(t, e), len = half * 0.95f;
    put(d, TS_RING, k->x, k->y, DPAD_DEAD * (off - half), 0.3f, 0, 0, 0, rgba(255, 255, 255, 0.35f * d->alpha));
    for (int a = 0; a < DPAD_ARMS; a++) {
        float x = k->x + toward_x[a] * off, y = k->y + toward_y[a] * off;
        int pressed = t->dpad_finger >= 0 && (t->dpad_buttons & DPAD_PAD[a]);
        box_button(d, x, y, half, half, pressed, d->alpha, "", 0);
        put(d, TS_SECTOR, x + toward_x[a] * len * 0.5f, y + toward_y[a] * len * 0.5f, 0, len,
            wedge_mid[a] - 0.7f, wedge_mid[a] + 0.7f, rgba(255, 255, 255, 0.95f * d->alpha), 0);
    }
}

static void draw_stick(const Touch *t, Draw *d)
{
    if (t->set.dpad) { draw_dpad(t, d); return; }
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
    if (actions(t)) icon(d, kx, ky, ring * 0.44f * 0.72f, TI_BAR, 0, rgba(255, 255, 255, 0.85f * d->alpha));
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
        static const int pictures[4] = {TI_BOOT, TI_HAND, TI_WRAP, TI_LINK};
        for (int i = 0; i < 4; i++) {
            float ang = PI_F / 4 + i * PI_F / 2, r = (r_in + r_out) / 2;
            if (actions(t)) {   /* the shoulder's own trick inside, its name toward the rim */
                float ri = r_in + (r_out - r_in) * 0.33f, rn = r_in + (r_out - r_in) * 0.76f;
                icon(d, cx + cosf(ang) * ri, cy - sinf(ang) * ri, 1.7f * sc, pictures[i], 0, rgba(255, 255, 255, 0.85f * a));
                text(d, cx + cosf(ang) * rn, cy - sinf(ang) * rn, 1.8f * sc, 0, names[i], rgba(255, 255, 255, 0.85f * a));
            } else text(d, cx + cosf(ang) * r, cy - sinf(ang) * r, 2.2f * sc, 0, names[i], rgba(255, 255, 255, 0.85f * a));
        }
    }
}

static void draw_thumb(const Touch *t, Draw *d)
{
    int air = t->air_layout;
    draw_stick(t, d);
    const Ctl *p = &t->ctl[TC_PEDAL];
    styled_button(t, d, p, pressed_control(t, TC_PEDAL), air ? 0.45f : 1.0f, TS_CROSS, "PEDAL", (Act){TI_PEDAL, 0, TS_CROSS, NULL});
    if (!air && t->set.attacks == TOUCH_ATTACK_FLICK) {   /* the flick hints: not buttons */
        float off = 16.0f * t->bs, s = 2.6f;
        uint32_t w = rgba(255, 255, 255, 0.92f * d->alpha);
        put(d, TS_CHEVRON, p->x - off - s * 1.1f, p->y, s * 0.8f, 0, PI_F, 0, 0, w);
        put(d, TS_CHEVRON, p->x + off + s * 1.1f, p->y, s * 0.8f, 0, 0, 0, 0, w);
        if (actions(t)) {   /* the fists: the left-hand attack (◻) left, the right-hand one (○) right */
            icon(d, p->x - off + s * 0.4f, p->y, s * 1.25f, TI_FIST, 1, w);
            icon(d, p->x + off - s * 0.4f, p->y, s * 1.25f, TI_FIST, 0, w);
        } else {
            put(d, TS_SQUARE, p->x - off + s * 0.4f, p->y, s, 0, 0, 0, 0, w);
            put(d, TS_CIRCLE, p->x + off - s * 0.4f, p->y, s, 0, 0, 0, 0, w);
        }
    }
    if (t->set.attacks == TOUCH_ATTACK_BUTTONS) {
        styled_button(t, d, &t->ctl[TC_SQUARE], pressed_control(t, TC_SQUARE), 1, TS_SQUARE, "", (Act){TI_FIST, 1, TS_SQUARE, NULL});
        styled_button(t, d, &t->ctl[TC_CIRCLE], pressed_control(t, TC_CIRCLE), 1, TS_CIRCLE, "", (Act){TI_FIST, 0, TS_CIRCLE, NULL});
    }
    if (!air) {
        styled_button(t, d, &t->ctl[TC_HOP], pressed_control(t, TC_HOP), 1, 0, "HOP", (Act){TI_HOP, 0, 0, "R2"});
        styled_button(t, d, &t->ctl[TC_BRAKE], pressed_control(t, TC_BRAKE), 1, TS_TRIANGLE, "BRAKE", (Act){TI_BRAKE, 0, TS_TRIANGLE, NULL});
        styled_button(t, d, &t->ctl[TC_SLIDE], pressed_control(t, TC_SLIDE), 1, 0, "SLIDE", (Act){TI_SLIDE, 0, 0, "L3"});
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
    /* Actions: L2 Saran Wrap, L1 One Hander, R1 One Legger; R2 the bunny hop on the ground, its
       trick in the air (finding 229) */
    int ia = bar == TC_LBAR ? TI_WRAP : TI_BOOT, ib = bar == TC_LBAR ? TI_HAND : touch_air_picture(t, bar) ? TI_LINK : TI_HOP;
    if (!right_handed(t)) { const char *x = a; a = b; b = x; int i = ia; ia = ib; ib = i; }
    float th = fminf(k->hh * 0.55f, 3.0f);
    if (actions(t)) {
        uint32_t w = rgba(255, 255, 255, 0.95f * d->alpha);
        float g = fminf(k->hh * 0.62f, k->hw * 0.2f), gap = g * 0.5f;
        for (int side = -1; side <= 1; side += 2) {
            float sx = k->x + side * k->hw * 0.63f;
            const char *name = side < 0 ? a : b;
            float nh = fminf(th * 0.62f, 2.4f), nw = 0.9f * nh * 2;
            float left = sx - (2 * g + gap + nw) / 2;   /* the picture, then its pad name */
            icon(d, left + g, k->y, g, side < 0 ? ia : ib, 0, w);
            text(d, left + 2 * g + gap + nw / 2, k->y, nh, 0, name, line);
        }
    } else {
        text(d, k->x - k->hw * 0.63f, k->y, th, 0, a, line);
        text(d, k->x + k->hw * 0.63f, k->y, th, 0, b, line);
    }
    text(d, k->x, k->y, th * 0.6f, 0, m, line);
}

static void draw_classic(const Touch *t, Draw *d)
{
    draw_stick(t, d);
    draw_bar(t, d, TC_LBAR);
    draw_bar(t, d, TC_RBAR);
    styled_button(t, d, &t->ctl[TC_L3], pressed_control(t, TC_L3), 1, 0, actions(t) ? "" : "L3", (Act){TI_SLIDE, 0, 0, "L3"});
    styled_button(t, d, &t->ctl[TC_R3], pressed_control(t, TC_R3), 1, 0, actions(t) ? "" : "R3", (Act){TI_SPRINT, 0, 0, "R3"});
    /* △ brakes on the ground and superstyles a trick in the air */
    styled_button(t, d, &t->ctl[TC_TRIANGLE], pressed_control(t, TC_TRIANGLE), 1, TS_TRIANGLE, "",
                  (Act){touch_air_picture(t, TC_TRIANGLE) ? TI_STAR : TI_BRAKE, 0, TS_TRIANGLE, NULL});
    styled_button(t, d, &t->ctl[TC_CSQUARE], pressed_control(t, TC_CSQUARE), 1, TS_SQUARE, "", (Act){TI_FIST, 1, TS_SQUARE, NULL});
    styled_button(t, d, &t->ctl[TC_CCIRCLE], pressed_control(t, TC_CCIRCLE), 1, TS_CIRCLE, "", (Act){TI_FIST, 0, TS_CIRCLE, NULL});
    styled_button(t, d, &t->ctl[TC_CROSS], pressed_control(t, TC_CROSS), 1, TS_CROSS, "", (Act){TI_PEDAL, 0, TS_CROSS, NULL});
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
    if (t->pause == PAUSE_CODES) {
        text(d, W / 2, H * 0.05f + 5, 3.9f, 0, "CODES", green);
        static const char arrows[4] = {'^', 'v', '<', '>'};
        static const int symbols[4] = {TS_TRIANGLE, TS_CIRCLE, TS_CROSS, TS_SQUARE};
        if (!t->code_len) text(d, W / 2, H * 0.05f + 11.5f, 2.2f, 0, "TYPE A CODE, THEN ENTER", off);
        for (int i = 0; i < t->code_len; i++) {   /* what is typed, one symbol after another */
            float x = W / 2 + (i - (t->code_len - 1) / 2.0f) * 5.0f, y = H * 0.05f + 11.5f;
            if (t->code[i] < 4) {
                char c[2] = {arrows[t->code[i]], 0};
                text(d, x, y, 3.0f, 0, c, white);
            } else put(d, symbols[t->code[i] - 4], x, y, 2.0f, 0, 0, 0, 0, white);
        }
        static const char *const words[CODE_BUTTONS - CODE_CLEAR] = {"CLEAR", "BACK", "ENTER"};
        for (int b = 0; b < CODE_BUTTONS; b++) {
            Box x = code_box(t, b);
            int pressed = 0;
            for (int i = 0; i < TOUCH_FINGERS; i++)
                if (t->f[i].used && t->f[i].job == JOB_PANEL && t->f[i].target == b && pause_row_at(t, t->f[i].x, t->f[i].y) == b) pressed = 1;
            int dim = b == CODE_ENTER && !t->code_len;
            box_button(d, x.x, x.y, x.hw, x.hh, pressed, dim ? 0.4f : 1.0f, b >= CODE_CLEAR ? words[b - CODE_CLEAR] : "", 2.4f);
            if (b < 4) {
                char c[2] = {arrows[b], 0};
                text(d, x.x, x.y, 3.4f, 0, c, white);
            } else if (b < CODE_CLEAR) put(d, symbols[b - 4], x.x, x.y, 2.6f, 0, 0, 0, 0, white);
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
    static const char *const rows[PAUSE_ROWS] = {"RESUME", "RESTART RACE", "QUIT RACE", "CONTROLS", "BAIL OUT", "CODES"};
    for (int r = 0; r < PAUSE_ROWS; r++) {
        Box b = pause_row_box(t, r);
        int enabled = r >= ROW_CONTROLS || t->game.pause_row[r] >= 0;
        int pressed = 0;
        for (int i = 0; i < TOUCH_FINGERS; i++)
            if (t->f[i].used && t->f[i].job == JOB_PANEL && t->f[i].target == r && pause_row_at(t, t->f[i].x, t->f[i].y) == r) pressed = 1;
        if (pressed) put(d, TS_RECT, b.x, b.y, b.hw, b.hh, 1.0f, 0, rgba(166, 227, 154, 0.17f), 0);
        if (r == ROW_CONTROLS) put(d, TS_RECT, W / 2, b.y - b.hh - 1.0f, W * 0.30f, 0.15f, 0, 0, rgba(166, 227, 154, 0.45f), 0);
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
    /* alone (the launcher's entry): a plain background, the screen's outline and its middle lines,
       instead of the game's picture under a dimmer */
    put(d, TS_RECT, W / 2, H / 2, W, H, 0, 0, t->editor_alone ? rgba(28, 34, 42, 1) : rgba(6, 8, 12, 0.38f), 0);
    if (t->editor_alone) {
        put(d, TS_RECT, W / 2, H / 2, 0.12f, H / 2, 0, 0, rgba(255, 255, 255, 0.10f), 0);
        put(d, TS_RECT, W / 2, H / 2, W / 2, 0.12f, 0, 0, rgba(255, 255, 255, 0.10f), 0);
    }
    if (s->strip_left > 0) put(d, TS_STRIPES, s->strip_left / 2, H / 2, s->strip_left / 2, H / 2, 0, 0, rgba(255, 90, 90, 0.28f), 0);
    if (s->strip_right > 0) put(d, TS_STRIPES, W - s->strip_right / 2, H / 2, s->strip_right / 2, H / 2, 0, 0, rgba(255, 90, 90, 0.28f), 0);
    if (s->strip_bottom > 0) put(d, TS_STRIPES, W / 2, H - s->strip_bottom / 2, W / 2, s->strip_bottom / 2, 0, 0, rgba(255, 90, 90, 0.28f), 0);
    Draw full = *d;
    full.alpha = 1;
    if (t->editor_tab == EDIT_AIR) {
        draw_trick(t, &full);
        styled_button(t, &full, &t->ctl[TC_PEDAL], 0, 0.45f, TS_CROSS, "PEDAL", (Act){TI_PEDAL, 0, TS_CROSS, NULL});
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
    static const char *const names[EDIT_BUTTONS] = {"GROUND", "AIR", "-", "+", "UNDO", "REDO", "RESET", "DONE"};
    for (int b = 0; b < EDIT_BUTTONS; b++) {
        Box x = editor_box(t, b);
        int on = (b == EDIT_GROUND && t->editor_tab == EDIT_GROUND) || (b == EDIT_AIR && t->editor_tab == EDIT_AIR);
        int dim = (b == EDIT_UNDO && !can_undo(t)) || (b == EDIT_REDO && !can_redo(t)) ||
                  (b == EDIT_AIR && t->set.preset != TOUCH_THUMB_ZONES);
        uint32_t fill = b == EDIT_DONE ? rgba(166, 227, 154, 1) : on ? rgba(255, 255, 255, 0.18f) : rgba(10, 14, 18, 0.9f);
        put(d, TS_RECT, x.x, x.y, x.hw, x.hh, 1.4f, 0, fill, rgba(255, 255, 255, 0.28f));
        text(d, x.x, x.y, b == EDIT_MINUS || b == EDIT_PLUS ? 3.0f : 2.2f, 0, names[b],
             b == EDIT_DONE ? rgba(16, 32, 16, 1) : rgba(255, 255, 255, dim ? 0.3f : 1));
    }
    if (t->edit_selected >= 0 && t->ctl[t->edit_selected].on) {
        /* the selected control, its drawn size and its own size in per cent (finding 226) */
        const char *const *readout = CONTROL_NAMES;
        const Ctl *k = &t->ctl[t->edit_selected];
        char info[64];
        float size = (k->box ? fmaxf(k->hw, k->hh) : k->r) * 2;
        const char *name = t->set.dpad && t->edit_selected == TC_STICK ? "D-PAD" : readout[t->edit_selected];
        if (t->set.preset == TOUCH_CLASSIC && t->edit_selected == TC_CAMERA) name = "SELECT";
        if (t->set.preset == TOUCH_CLASSIC && t->edit_selected == TC_PAUSE) name = "START";
        if (t->edit_selected == TC_STICK && !t->set.dpad)   /* the stick's home: a place, no size */
            snprintf(info, sizeof info, "%s", name);
        else
            snprintf(info, sizeof info, "%s %.0f MM %.0f%%", name, size, edit_of((Touch *)t, t->edit_selected)->size * 100);
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
    if (flyover_on(t) && t->pause == PAUSE_NONE) {   /* the flight over the track: SKIP (finding 209) */
        Box b = skip_box(t);
        int pressed = 0;
        for (int i = 0; i < TOUCH_FINGERS; i++)
            if (t->f[i].used && t->f[i].job == JOB_SKIP && skip_hit(t, t->f[i].x, t->f[i].y)) pressed = 1;
        box_button(&d, b.x, b.y, b.hw, b.hh, pressed, d.alpha, t->game.replay ? "MENU" : "SKIP", 3.4f);
        return d.n;
    }
    if (t->set.preset == TOUCH_CLASSIC) draw_classic(t, &d);
    else draw_thumb(t, &d);
    if (t->label[0] && t->set.labels != TOUCH_LABELS_OFF) {
        char shown[TOUCH_TEXT + 4];
        snprintf(shown, sizeof shown, "%s%s", t->label, t->trick.triangle ? " *" : "");
        text(&d, t->screen.width / 2, t->screen.height * 0.26f, 3.6f, 0, shown, rgba(255, 255, 255, 0.95f));
    }
    if (t->panel_open) draw_online_panel(t, &d);
    if (t->hint) draw_hint(t, &d);
    if (t->pause == PAUSE_OPEN || t->pause == PAUSE_CONTROLS || t->pause == PAUSE_CONFIRM || t->pause == PAUSE_CODES)
        draw_panel_rows(t, &d);
    return d.n;
}
