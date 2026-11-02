/* The touch controls' host side (finding 138): SDL's touch events to port/hle/touch.c, the game's
   two reads in (HostTouch, host_service_touch_field once a field from pad.c), player 1's pad and
   the renderer's overlay out.

   Threads: the SDL thread (sdl_host.c) queues the touches, Back, controller use and the screen's
   size in mm (touch_host_event, touch_host_tick); the game thread drains the queue into touch.c
   once a field and merges its pad (host.c's host_pad_device); the presenting thread draws the
   shapes the game thread last published (gpu_rust.c, touch_host_overlay). Each crossing is one
   mutex.

   Where touch is on: Android (a touchscreen), or any desktop with DHD_TOUCH_MOUSE=1, where the left
   mouse button is a finger (DHD_TOUCH_SCREEN_MM=WxH maps the window to a phone's size, e.g.
   154.5x69.5 for the Mi 11i; else the window's real size from its DPI). The host then sets
   DHD_TOUCH (android / mouse) for the core's settings pages.

   The settings are the touch_* keys of dhd.ini (DHD_TOUCH_* variables, the TOUCH CONTROLS pages);
   for the checks only: DHD_TOUCH_HUD=game keeps the game's HUD (the fallback layout), and
   DHD_TOUCH_AIR_WAIT=ms sets the air layout's wait (150, or 33 if check 6 finds the flag steady). */
#include "host_api.h"
#include "../hle/hle.h"
#include "../hle/touch.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The platform's part: Android's in android_touch.c; the desktop's below. */
#ifdef __ANDROID__
int android_touch_screen(float *xdpi, float *ydpi, int insets[8]);   /* gesture insets l t r b, cutout l t r b (px) */
void android_touch_buzz(int kind);
int android_touch_tilt(float *degrees);
void android_touch_tilt_on(int on);
#endif

enum { EV_DOWN, EV_MOVE, EV_UP, EV_CANCEL, EV_BACK, EV_PAD_USED, EV_PAD_LOST };
typedef struct { int kind; long long id; float x, y; double ms; } Event;   /* x, y: 0..1 of the window */
enum { QUEUE = 256 };

static SDL_Mutex *lock;
static Event queue[QUEUE];
static int queued;
static int available = -1;            /* touch is on here: 1 Android, 2 the mouse */
static SDL_AtomicInt owns;            /* the overlay takes the touches (no menu mouse from them) */
static SDL_AtomicInt want_buzz;       /* TOUCH_BUZZ_* for the SDL thread */
static SDL_AtomicInt tilt_wanted;     /* tilt steering is on */
static SDL_AtomicInt tilt_milli;      /* the phone's roll, millidegrees */
static TouchScreen screen;            /* under lock */
static int screen_known;

static Touch *touch;                  /* the game thread's */
static float published[TOUCH_SHAPES * 16];
static int published_count;
static float published_w, published_h;

static double ms_now(void) { return (double)SDL_GetTicksNS() / 1e6; }

static SDL_InitState started;

/* Touch is on here (1 Android, 2 the mouse; 0 off), and the lock is made, once, from any thread. */
static int touch_on(void)
{
    if (SDL_ShouldInit(&started)) {
        lock = SDL_CreateMutex();
#ifdef __ANDROID__
        available = 1;
#else
        const char *mouse = getenv("DHD_TOUCH_MOUSE");
        available = mouse && atoi(mouse) ? 2 : 0;
#endif
        SDL_SetInitialized(&started, true);
    }
    return available;
}

static void push(int kind, long long id, float x, float y, double ms)
{
    if (!lock) return;
    SDL_LockMutex(lock);
    if (queued < QUEUE) queue[queued++] = (Event){kind, id, x, y, ms};
    SDL_UnlockMutex(lock);
}

/* ---- the screen in mm (SDL thread) ---- */

static void screen_update(SDL_Window *window)
{
    if (!window) return;
    int w = 0, h = 0;
    if (!SDL_GetWindowSizeInPixels(window, &w, &h) || w <= 0 || h <= 0) return;
    TouchScreen s;
    memset(&s, 0, sizeof s);
#ifdef __ANDROID__
    float xdpi = 0, ydpi = 0;
    int in[8] = {0};
    if (!android_touch_screen(&xdpi, &ydpi, in) || xdpi < 50 || ydpi < 50) xdpi = ydpi = 160.0f * SDL_GetWindowDisplayScale(window);
    float mx = 25.4f / xdpi, my = 25.4f / ydpi;
    s.width = w * mx, s.height = h * my;
    s.strip_left = in[0] * mx, s.strip_top = in[1] * my, s.strip_right = in[2] * mx, s.strip_bottom = in[3] * my;
    if (in[4] || in[5] || in[6] || in[7]) {   /* the cutout as the band its inset covers */
        if (in[4]) s.cut_x0 = 0, s.cut_x1 = in[4] * mx, s.cut_y0 = 0, s.cut_y1 = s.height;
        if (in[6]) s.cut_x0 = s.width - in[6] * mx, s.cut_x1 = s.width, s.cut_y0 = 0, s.cut_y1 = s.height;
    }
#else
    const char *mm = getenv("DHD_TOUCH_SCREEN_MM");
    float sw = 0, sh = 0;
    if (mm && sscanf(mm, "%fx%f", &sw, &sh) == 2 && sw > 20 && sh > 20) s.width = sw, s.height = sh;
    else {
        float scale = SDL_GetWindowDisplayScale(window);
        float per_mm = 96.0f * (scale > 0 ? scale : 1) / 25.4f;
        s.width = w / per_mm, s.height = h / per_mm;
    }
#endif
    SDL_LockMutex(lock);
    if (!screen_known || memcmp(&s, &screen, sizeof s)) {
        screen = s;
        screen_known = 2;   /* changed: the game thread lays out again */
    }
    SDL_UnlockMutex(lock);
}

/* ---- SDL thread ---- */

static long long last_window;

/* One SDL event: 1 when it is the touch controls' (the caller drops it). */
int touch_host_event(const SDL_Event *e)
{
    if (!touch_on()) return 0;
    int owned = SDL_GetAtomicInt(&owns);
    switch (e->type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        SDL_Window *w = SDL_GetWindowFromID(e->tfinger.windowID);
        if (w && (long long)e->tfinger.windowID != last_window) { last_window = e->tfinger.windowID; screen_update(w); }
        int kind = e->type == SDL_EVENT_FINGER_DOWN ? EV_DOWN : e->type == SDL_EVENT_FINGER_MOTION ? EV_MOVE
                 : e->type == SDL_EVENT_FINGER_UP ? EV_UP : EV_CANCEL;
        push(kind, (long long)e->tfinger.fingerID, e->tfinger.x, e->tfinger.y, (double)e->common.timestamp / 1e6);
        return 0;   /* the window's touch list is SDL's; the synthesized mouse is dropped below */
    }
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.which == SDL_TOUCH_MOUSEID : e->button.which == SDL_TOUCH_MOUSEID)
            return owned;   /* a touch's mouse: the menus' only while the overlay is away */
        if (touch_on() == 2) {   /* the desktop's mouse as a finger */
            SDL_WindowID id = e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.windowID : e->button.windowID;
            SDL_Window *w = SDL_GetWindowFromID(id);
            int ww = 0, wh = 0;
            if (!w || !SDL_GetWindowSize(w, &ww, &wh) || ww <= 0 || wh <= 0) return 0;
            if ((long long)id != last_window) { last_window = id; screen_update(w); }
            float x = (e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.x : e->button.x) / ww;
            float y = (e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.y : e->button.y) / wh;
            double ms = (double)e->common.timestamp / 1e6;
            if (e->type == SDL_EVENT_MOUSE_MOTION) {
                if (e->motion.state & SDL_BUTTON_LMASK) push(EV_MOVE, 0x7FFF, x, y, ms);
            } else if (e->button.button == SDL_BUTTON_LEFT) push(e->button.down ? EV_DOWN : EV_UP, 0x7FFF, x, y, ms);
            return owned;
        }
        return 0;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_AC_BACK) {   /* Android's Back: pause, or △ in menus */
            if (!e->key.repeat) push(EV_BACK, 0, 0, 0, (double)e->common.timestamp / 1e6);
            return 1;
        }
        if (!e->key.repeat) push(EV_PAD_USED, 0, 0, 0, (double)e->common.timestamp / 1e6);
        return 0;
    case SDL_EVENT_KEY_UP:
        return e->key.scancode == SDL_SCANCODE_AC_BACK;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        push(EV_PAD_USED, 0, 0, 0, (double)e->common.timestamp / 1e6);
        return 0;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (abs(e->gaxis.value) > 16000) push(EV_PAD_USED, 0, 0, 0, (double)e->common.timestamp / 1e6);
        return 0;
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_JOYSTICK_REMOVED:
        push(EV_PAD_LOST, 0, 0, 0, (double)e->common.timestamp / 1e6);
        return 0;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
        screen_update(SDL_GetWindowFromID(e->window.windowID));
        return 0;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        push(EV_CANCEL, -1, 0, 0, ms_now());
        return 0;
    default:
        return 0;
    }
}

/* Every pass of the SDL thread: the motor, tilt and (now and then) the screen's insets. */
void touch_host_tick(void)
{
    if (!touch_on()) return;
    int buzz = SDL_GetAtomicInt(&want_buzz);
    if (buzz) SDL_SetAtomicInt(&want_buzz, 0);
#ifdef __ANDROID__
    static int tilt_was;
    static double insets_at;
    int tilt = SDL_GetAtomicInt(&tilt_wanted);
    if (tilt != tilt_was) { android_touch_tilt_on(tilt); tilt_was = tilt; }
    float deg;
    if (tilt && android_touch_tilt(&deg)) SDL_SetAtomicInt(&tilt_milli, (int)(deg * 1000));
    if (buzz) android_touch_buzz(buzz);
    if (last_window && ms_now() - insets_at > 2000) {   /* the gesture insets change with the navigation mode */
        insets_at = ms_now();
        screen_update(SDL_GetWindowFromID((SDL_WindowID)last_window));
    }
#else
    (void)buzz;
#endif
}

/* ---- the game thread ---- */

static int env_choice(const char *name, const char *const *words, int count, int fallback)
{
    const char *v = getenv(name);
    for (int i = 0; v && i < count; i++)
        if (!SDL_strcasecmp(v, words[i])) return i;
    return fallback;
}

static int env_int(const char *name, int fallback, int lo, int hi)
{
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    int n = atoi(v);
    return n < lo ? lo : n > hi ? hi : n;
}

static void settings_read(TouchSettings *s)
{
    static const char *const preset[] = {"thumb", "classic"}, *const hand[] = {"right", "left"}, *const steer[] = {"stick", "tilt"};
    static const char *const stick[] = {"floating", "fixed"}, *const labels[] = {"auto", "always", "off"};
    static const char *const attacks[] = {"flick", "buttons"}, *const pad[] = {"hide", "show"}, *const hud[] = {"game", "move"};
    touch_settings_default(s);
    s->preset = env_choice("DHD_TOUCH_PRESET", preset, 2, TOUCH_THUMB_ZONES);
    s->left_handed = env_choice("DHD_TOUCH_HAND", hand, 2, 0);
    s->tilt = env_choice("DHD_TOUCH_STEER", steer, 2, 0);
    const char *size = getenv("DHD_TOUCH_SIZE");
    s->size = size && atoi(size) > 0 ? env_int("DHD_TOUCH_SIZE", 100, 50, 150) : 0;
    s->opacity = env_int("DHD_TOUCH_OPACITY", 60, 10, 100);
    s->stick_fixed = env_choice("DHD_TOUCH_STICK", stick, 2, 0);
    s->stick_range = env_int("DHD_TOUCH_STICK_RANGE", 12, 6, 30);
    s->trick_size = env_int("DHD_TOUCH_TRICK_STICK", 31, 20, 45);
    s->labels = env_choice("DHD_TOUCH_LABELS", labels, 3, TOUCH_LABELS_AUTO);
    s->attacks = env_choice("DHD_TOUCH_ATTACKS", attacks, 2, TOUCH_ATTACK_FLICK);
    s->vibration = env_int("DHD_TOUCH_VIBRATION", 1, 0, 1) && env_int("DHD_VIBRATION", 1, 0, 1);
    s->show_with_pad = env_choice("DHD_TOUCH_WITH_PAD", pad, 2, 0);
    s->tilt_strength = env_int("DHD_TOUCH_TILT_STRENGTH", 50, 0, 100);
    const char *centre = getenv("DHD_TOUCH_TILT_CENTRE");
    s->tilt_centre = centre ? (float)atof(centre) : 0;
    const char *hints = getenv("DHD_TOUCH_HINTS");
    s->hints_shown = hints ? (unsigned)strtoul(hints, NULL, 10) : 0;
    s->hud_moved = env_choice("DHD_TOUCH_HUD", hud, 2, 1);
    s->air_wait_ms = env_int("DHD_TOUCH_AIR_WAIT", 150, 16, 500);
    touch_layout_parse(s, getenv("DHD_TOUCH_EDITS"));
}

static void save(const char *key, const char *env, const char *value)
{
    host_setting_save(key, value);
    host_putenv_s(env, value);
}

/* What the player changed through the overlay (hints shown, the editor's Done) into dhd.ini. */
static void save_settings(const TouchSettings *s)
{
    char text[2048];
    snprintf(text, sizeof text, "%u", s->hints_shown);
    save("touch_hints", "DHD_TOUCH_HINTS", text);
    touch_layout_format(s, text, sizeof text);
    save("touch_edits", "DHD_TOUCH_EDITS", text);
    fprintf(stderr, "touch: saved (hints %u, layout \"%s\")\n", s->hints_shown, text);
}

static void configure(int log)
{
    TouchSettings s;
    TouchScreen sc;
    settings_read(&s);
    SDL_LockMutex(lock);
    sc = screen;
    int known = screen_known;
    if (screen_known == 2) screen_known = 1;
    SDL_UnlockMutex(lock);
    touch_configure(touch, &s, known ? &sc : NULL);
    SDL_SetAtomicInt(&tilt_wanted, s.tilt);
    if (log)
        fprintf(stderr, "touch: %s, %s hand, %s, size %s, screen %.1f x %.1f mm (strips %.1f %.1f %.1f mm)\n",
                s.preset == TOUCH_CLASSIC ? "classic pad" : "thumb zones", s.left_handed ? "left" : "right",
                s.tilt ? "tilt" : "stick", s.size ? "set" : "auto", sc.width, sc.height, sc.strip_left, sc.strip_right, sc.strip_bottom);
}

static void publish(void)
{
    static TouchShape shapes[TOUCH_SHAPES];
    int n = touch_shapes(touch, shapes, TOUCH_SHAPES);
    SDL_LockMutex(lock);
    for (int i = 0; i < n; i++) {
        float *f = published + i * 16;
        const TouchShape *sh = &shapes[i];
        f[0] = (float)sh->kind, f[1] = sh->x, f[2] = sh->y, f[3] = sh->a, f[4] = sh->b, f[5] = sh->c, f[6] = sh->d, f[7] = 0;
        for (int k = 0; k < 4; k++) {
            f[8 + k] = (float)((sh->fill >> (24 - 8 * k)) & 0xFF) / 255.0f;
            f[12 + k] = (float)((sh->line >> (24 - 8 * k)) & 0xFF) / 255.0f;
        }
    }
    published_count = n;
    published_w = screen.width, published_h = screen.height;
    SDL_UnlockMutex(lock);
}

/* pad.c, once a field: the game's two reads in; the HUD's place, Leave race and the hint's hold out. */
void host_service_touch_field(uint32_t address)
{
    HostTouch *h = (HostTouch *)GUEST(address);
    if (!touch_on()) return;
    static int set_up;
    if (!touch) {
        touch = touch_new();
        host_putenv_s("DHD_TOUCH", touch_on() == 1 ? "android" : "mouse");
    }
    int relay = 0;
    SDL_LockMutex(lock);
    relay = screen_known == 2;
    SDL_UnlockMutex(lock);
    if (!set_up || relay || h->command == TOUCH_COMMAND_SETTINGS) { configure(!set_up || relay); set_up = 1; }
    if (h->command == TOUCH_COMMAND_EDIT) touch_open_editor(touch);
    if (h->command == TOUCH_COMMAND_HINTS) {
        save("touch_hints", "DHD_TOUCH_HINTS", "0");
        configure(0);
    }
    if (h->command == TOUCH_COMMAND_TILT_CENTRE) {
        char v[32];
        snprintf(v, sizeof v, "%.1f", SDL_GetAtomicInt(&tilt_milli) / 1000.0);
        save("touch_tilt_centre", "DHD_TOUCH_TILT_CENTRE", v);
        configure(0);
    }
    /* the touches so far, in order */
    Event events[QUEUE];
    int n;
    SDL_LockMutex(lock);
    n = queued;
    memcpy(events, queue, sizeof *events * (size_t)n);
    queued = 0;
    TouchScreen sc = screen;
    SDL_UnlockMutex(lock);
    for (int i = 0; i < n; i++) {
        const Event *e = &events[i];
        float x = e->x * sc.width, y = e->y * sc.height;
        switch (e->kind) {
        case EV_DOWN: touch_down(touch, e->id, x, y, e->ms); break;
        case EV_MOVE: touch_move(touch, e->id, x, y, e->ms); break;
        case EV_UP: touch_up(touch, e->id, x, y, e->ms); break;
        case EV_CANCEL: touch_cancel_all(touch, e->ms); break;
        case EV_BACK: touch_back(touch, e->ms); break;
        case EV_PAD_USED: touch_pad_used(touch, e->ms); break;
        case EV_PAD_LOST: touch_pad_lost(touch, e->ms); break;
        }
    }
    touch_tilt(touch, SDL_GetAtomicInt(&tilt_milli) / 1000.0f);
    TouchGame g;
    memset(&g, 0, sizeof g);
    g.race_hud = h->race_hud, g.air = h->air, g.online = h->online, g.clock_moving = h->clock_moving;
    for (int i = 0; i < 3; i++) g.pause_row[i] = h->pause_row[i];
    g.race_ms = h->race_ms, g.vsync = h->vsync, g.fps = h->fps;
    touch_update(touch, &g, ms_now());
    int buzz = touch_take_buzz(touch);
    if (buzz) SDL_SetAtomicInt(&want_buzz, buzz);
    TouchSettings saved;
    if (touch_take_saved(touch, &saved)) save_settings(&saved);
    h->hud_mode = touch_hud_mode(touch);
    h->leave_race = touch_take_ask(touch) == TOUCH_ASK_LEAVE_RACE;
    h->freeze = touch_freeze(touch);
    SDL_SetAtomicInt(&owns, touch_owns_touches(touch));
    publish();
}

/* host_pad_device, player 1: the touches' buttons and stick merged in. */
void touch_host_merge(HostPad *out)
{
    if (!touch) return;
    uint16_t buttons = 0;
    uint8_t lx = out->left_x, ly = out->left_y, rx = out->right_x, ry = out->right_y;
    touch_pad(touch, &buttons, &lx, &ly, &rx, &ry);
    if (buttons) {
        out->buttons |= buttons;
        static const uint16_t order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                           PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
        for (int i = 0; i < 12; i++)
            if (buttons & order[i]) out->pressure[i] = 255;
    }
    out->left_x = lx, out->left_y = ly, out->right_x = rx, out->right_y = ry;
}

/* The presenting thread: a copy of the shapes for this picture, 16 floats each (at most max);
   returns the count (0: none). */
int touch_host_overlay(float *items, int max, float *width_mm, float *height_mm)
{
    if (!touch_on()) return 0;
    SDL_LockMutex(lock);
    int n = published_count < max ? published_count : max;
    memcpy(items, published, sizeof *items * 16 * (size_t)n);
    *width_mm = published_w, *height_mm = published_h;
    SDL_UnlockMutex(lock);
    return n;
}
