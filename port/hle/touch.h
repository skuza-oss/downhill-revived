/* On-screen touch controls (finding 141): the platform-neutral part. Touches in, PS2 pad input
   and an overlay's shapes out; no SDL, no OS, no game memory. The host (port/host/touch_host.c)
   feeds it the touches, the screen's size in mm and the two things read from the game (the
   rider's air flag and whether the race HUD is drawn, HostTouch in host_api.h), calls
   touch_update once per pad read, merges touch_pad's output into player 1's pad and hands the
   shapes to the renderer. Unit tests: port/hle/test_touch.c.

   Two presets: Thumb zones (default; a floating steering stick, PEDAL with flick attacks, HOP,
   BRAKE, SLIDE, and in the air a trick stick under the right thumb) and Classic pad (the PS2 pad
   laid flat). Every touch ends up as plain pad input: no assists, no gameplay change.

   Units: positions and sizes in mm from the screen's top-left corner (x right, y down). Two
   clocks: `now` (ms of wall time, for how a thumb moves) and the game's (race_ms: race time,
   1/60 s fields at any frame rate, stopped while paused, for the game's own windows such as the
   double-tap; vsync: the game's VBLANK count, for presses the game must see). */
#ifndef PORT_TOUCH_H
#define PORT_TOUCH_H

#include <stdint.h>

enum { TOUCH_FINGERS = 10, TOUCH_SHAPES = 512, TOUCH_TEXT = 48 };

enum { TOUCH_THUMB_ZONES, TOUCH_CLASSIC };
enum { TOUCH_LABELS_AUTO, TOUCH_LABELS_ALWAYS, TOUCH_LABELS_OFF };
enum { TOUCH_ATTACK_FLICK, TOUCH_ATTACK_BUTTONS };

/* The controls a layout has (also the editor's and dhd.ini touch_layout's names). */
enum {
    TC_PEDAL, TC_HOP, TC_BRAKE, TC_SLIDE, TC_SQUARE, TC_CIRCLE,     /* Thumb zones (◻ ○: Buttons mode) */
    TC_CAMERA, TC_PAUSE,                                            /* both presets' top buttons */
    TC_CROSS, TC_TRIANGLE, TC_CSQUARE, TC_CCIRCLE, TC_PLUS_LEFT, TC_PLUS_RIGHT,  /* Classic */
    TC_LBAR, TC_RBAR, TC_L3, TC_R3,
    TC_STICK,                                                       /* the steering stick's home */
    TC_TRICK,                                                       /* the trick stick's faint spot */
    TC_COUNT
};

typedef struct {
    float dx, dy;    /* mm from the preset's place */
    float size;      /* times the preset's size (1 = as designed) */
} TouchEdit;

/* The player's settings (dhd.ini touch_*, the TOUCH CONTROLS pages). */
typedef struct {
    int preset;           /* TOUCH_THUMB_ZONES / TOUCH_CLASSIC */
    int left_handed;      /* Hand: Left mirrors both presets and the HUD */
    int tilt;             /* Steer with: 0 stick, 1 tilt */
    int size;             /* percent; 0: the height rule's own (100% capped) */
    int opacity;          /* percent */
    int stick_fixed;      /* Stick: 0 floating, 1 fixed */
    int stick_range;      /* mm */
    int trick_size;       /* mm */
    int labels;           /* TOUCH_LABELS_* */
    int attacks;          /* TOUCH_ATTACK_* */
    int vibration;
    int show_with_pad;    /* With a controller: 0 hide, 1 show */
    int tilt_strength;    /* percent */
    float tilt_centre;    /* the tilt's zero, degrees */
    unsigned hints_shown; /* TOUCH_HINT_* bits already shown (Show hints again clears them) */
    int hud_moved;        /* 1: the HUD moves out of the controls' way (the HUD hook); 0: the fallback layout */
    int air_wait_ms;      /* 150, or 33 when the air flag is steady (check 6) */
    TouchEdit edit[2][TC_COUNT];  /* per preset */
} TouchSettings;

void touch_settings_default(TouchSettings *s);
/* dhd.ini touch_layout: "preset.control=dx,dy,size;..." and back (out: at least 1024 bytes). */
void touch_layout_parse(TouchSettings *s, const char *text);
void touch_layout_format(const TouchSettings *s, char *out, int size);

/* The screen: its size and the strips nothing dragged may start in (Android's system gesture
   insets), and the camera cutout to keep clear, all in mm. */
typedef struct {
    float width, height;
    float strip_left, strip_right, strip_top, strip_bottom;
    float cut_x0, cut_y0, cut_x1, cut_y1;   /* the cutout (empty: x1 <= x0) */
} TouchScreen;

/* What the game says each field (port/hle/pad.c through HostTouch, host_api.h). */
typedef struct {
    int race_hud;        /* the race HUD was drawn (the HUD hook): the race is on screen */
    int air;             /* player 1's rider is in the air (rider +0x4564, finding 141) */
    int online;          /* an online race (no pause: the panel) */
    int clock_moving;    /* the race clock advanced since the last field */
    int pause_row[3];    /* the game's pause menu: RESUME, RE-START, QUIT's rows (-1: none) */
    double race_ms;      /* race time, ms (fields * 1000/60), stopped while paused */
    uint32_t vsync;      /* the game's VBLANKs so far */
    int fps;             /* the game's VBLANKs a second (60, or the high rate) */
} TouchGame;

/* One shape of the overlay (the renderer draws them in order, port/render/src/present/touch.rs). */
enum {
    TS_DISC,       /* filled circle with a two-tone rim: x,y centre, a radius */
    TS_RING,       /* circle outline: a radius, b width */
    TS_RECT,       /* rounded rectangle: x,y centre, a,b half sizes, c corner radius */
    TS_SECTOR,     /* annulus piece: a inner, b outer radius, c start, d end angle (radians, y up) */
    TS_CROSS, TS_CIRCLE, TS_TRIANGLE, TS_SQUARE,   /* the PS2 symbols: a radius */
    TS_CHEVRON,    /* ‹ (c = pi) or › (c = 0): a size */
    TS_CHAR,       /* one character of the 8x8 font: a its height, c the character */
    TS_STRIPES     /* the editor's no-start strips: a rectangle with stripes (a,b half sizes) */
};
typedef struct {
    int kind;
    float x, y, a, b, c, d;
    uint32_t fill, line;   /* 0xRRGGBBAA */
} TouchShape;

/* Vibration asked for (the host's motor): TOUCH_BUZZ_*. */
enum { TOUCH_BUZZ_NONE, TOUCH_BUZZ_TICK, TOUCH_BUZZ_DOUBLE, TOUCH_BUZZ_STRONG };
/* Commands to the game's side (HostTouch.leave_race ...). */
enum { TOUCH_ASK_NONE, TOUCH_ASK_LEAVE_RACE = 1 };
/* Hints, each shown once (TouchSettings.hints_shown). */
enum { TOUCH_HINT_PEDAL = 1, TOUCH_HINT_ATTACK = 2, TOUCH_HINT_HOP = 4, TOUCH_HINT_SPRINT = 8,
       TOUCH_HINT_SLIDE = 16, TOUCH_HINT_TRICK = 32 };

typedef struct Touch Touch;
Touch *touch_new(void);
void touch_free(Touch *t);
void touch_configure(Touch *t, const TouchSettings *s, const TouchScreen *screen);
const TouchSettings *touch_settings(const Touch *t);

/* Input. id: the finger (any number, unique while down); x, y in mm; now in ms. */
void touch_down(Touch *t, long long id, float x, float y, double now);
void touch_move(Touch *t, long long id, float x, float y, double now);
void touch_up(Touch *t, long long id, float x, float y, double now);
void touch_cancel_all(Touch *t, double now);   /* the window lost the touches (focus, background) */
void touch_back(Touch *t, double now);         /* Android's Back */
void touch_pad_used(Touch *t, double now);     /* a controller or the keyboard moved: hide (setting) */
void touch_pad_lost(Touch *t, double now);     /* the controller went away mid-race: show, pause offline */
/* The phone's roll (tilt steering); known 0: no reading yet, or no sensor (the stick keeps steering). */
void touch_tilt(Touch *t, int known, float degrees);
void touch_open_editor(Touch *t);              /* TOUCH CONTROLS > Edit layout */

/* The host's queue between its input thread and the game's field (no lock here: the host holds
   its own). It never loses what ends a press: a finger's moves are merged (the latest kept) once
   the moves have filled their part, so ups, cancels, Back and a controller's loss always find a
   place; a finger with no queued move keeps its latest one (a reserve place if need be: a thumb
   pressed in a stall and held still must still steer); if even that part fills (a long stall
   full of taps), the queue becomes one cancel of every finger, with the Backs and controller
   losses kept after it. */
enum { TOUCH_EV_DOWN, TOUCH_EV_MOVE, TOUCH_EV_UP, TOUCH_EV_CANCEL, TOUCH_EV_BACK, TOUCH_EV_PAD_USED, TOUCH_EV_PAD_LOST };
typedef struct { int kind; long long id; float x, y; double ms; } TouchEvent;   /* x, y: the host's units */
enum { TOUCH_QUEUE = 256, TOUCH_QUEUE_RESERVE = 32 };   /* the last RESERVE places: no controller use, a move only as its finger's one queued move */
typedef struct {
    TouchEvent ev[TOUCH_QUEUE];
    int count;
    int merged, cancelled;   /* moves merged, queues turned into a cancel (since the host last looked) */
} TouchQueue;
void touch_queue_push(TouchQueue *q, int kind, long long id, float x, float y, double ms);
/* One queued event into the rules (x, y in mm). */
void touch_event(Touch *t, const TouchEvent *e, float x, float y);

/* Once per pad read: the game's state, then the pad. */
void touch_update(Touch *t, const TouchGame *game, double now);
/* The pad input the touches make now (merged into player 1's: buttons or-ed, sticks where moved). */
void touch_pad(const Touch *t, uint16_t *buttons, uint8_t *left_x, uint8_t *left_y, uint8_t *right_x, uint8_t *right_y);

/* Outputs since the last call, then cleared. */
int touch_take_buzz(Touch *t);
int touch_take_ask(Touch *t);
/* Saved settings that changed (hints shown, the editor's Done): 1 and a copy in *out. */
int touch_take_saved(Touch *t, TouchSettings *out);

/* State the host passes on. */
int touch_visible(const Touch *t);       /* the overlay is drawn (the race HUD, or the editor) */
int touch_hud_mode(const Touch *t);      /* 0 the game's HUD; 1 moved (right hand), 2 moved (left hand) */
int touch_owns_touches(const Touch *t);  /* touches are the overlay's (no menu mouse from them) */
int touch_freeze(const Touch *t);        /* the trick-stick hint holds the game */

/* The overlay's shapes, mm; returns the count. */
int touch_shapes(const Touch *t, TouchShape *out, int max);

/* ---- pieces the tests reach (and the editor uses) ---- */
float touch_height_scale(float screen_height, int preset);  /* s before the Size setting: may pass 1 */
float touch_button_scale(float s);                          /* 1.2 s - 0.2 */
/* The trick stick's buttons for one 8-way sector (0 = →, counting anticlockwise). */
uint16_t touch_sector_buttons(int sector);
const char *touch_trick_name(uint16_t buttons);             /* NULL: no tabled trick */
/* Where a panel's button is (its centre, mm): the pause rows (Resume, Restart race, Quit race,
   Controls, Bail out), the question (No, Yes), the online panel (Close, Bail out, Leave race),
   the editor's bar (Ground, Air, -, +, Reset, Done). 0 if there is no such button. */
enum { TOUCH_TARGET_PAUSE, TOUCH_TARGET_CONFIRM, TOUCH_TARGET_PANEL, TOUCH_TARGET_EDITOR };
int touch_target(const Touch *t, int kind, int index, float *x, float *y);
/* Where a control sits now (centre and radius, mm); 0 if the layout has none. */
int touch_control(const Touch *t, int control, float *x, float *y, float *r);

#endif
