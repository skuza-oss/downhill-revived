/* The touch controls' host side (finding 141): SDL's touch events to port/hle/touch.c, the game's
   two reads in (HostTouch, host_service_touch_field once a field from pad.c), player 1's pad and
   the renderer's overlay out.

   Threads: the SDL thread (sdl_host.c) queues the touches, Back, controller use and the screen's
   size in mm (touch_host_event, touch_host_tick); the game thread drains the queue into touch.c
   once a field and merges its pad (host.c's host_pad_device); the presenting thread draws the
   shapes the game thread last published (gpu_rust.c, touch_host_overlay). Each crossing is one
   mutex.

   Where touch is on: Android and iOS (a touchscreen; iOS's touches come from the app's Metal view,
   port/ios/render/metal_bridge.m, as SDL's finger events: ios_touch_finger), or any desktop with DHD_TOUCH_MOUSE=1, where the left
   mouse button is a finger (DHD_TOUCH_SCREEN_MM=WxH maps the window to a phone's size, e.g.
   154.5x69.5 for the Mi 11i; else the window's real size from its DPI). The host then sets
   DHD_TOUCH (android, the core's word for a phone's touchscreen, iOS's too / mouse) for the core's
   settings pages.

   The settings are the touch_* keys of dhd.ini (DHD_TOUCH_* variables, the TOUCH CONTROLS pages);
   for the checks only: DHD_TOUCH_HUD=game keeps the game's HUD (the fallback layout), and
   DHD_TOUCH_AIR_WAIT=ms sets the air layout's wait (150, or 33 if check 6 finds the flag steady);
   DHD_TOUCH_LOG=1 logs every touch (mm), the screen's insets and each change of the buttons sent
   (2: every move as well).

   The layout editor alone (finding 226; the shared mobile launcher's "Edit the touch layout"):
   DHD_TOUCH_EDITOR=1, or the game's argument --touch-editor (touch_host_args), starts the game
   with its sound muted and its movies skipped, opens the overlay's editor over a plain background
   as soon as the screen's size is known, and ends the game (as EXIT GAME does) once Done or Back
   has saved the layout, so the launcher that started it is shown again. */
#include "host_api.h"
#include "../hle/hle.h"
#include "../hle/touch.h"
#include "../gs/present.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
#define TOUCH_IOS 1
#endif

/* The platform's part: Android's in android_touch.c, iOS's in port/ios/render/metal_bridge.m; the
   desktop's below. */
#ifdef TOUCH_IOS
/* the view's size in mm, the strips to keep clear (home indicator) l t r b and the cutout's band
   x0 y0 x1 y1 (mm); 0 before the view is laid out */
int ios_touch_screen(float *width_mm, float *height_mm, float strips[4], float cut[4]);
void ios_touch_buzz(int kind);   /* TOUCH_BUZZ_*: the Taptic Engine */
int ios_touch_tilt(float *degrees);   /* CoreMotion's roll, as Android's (finding 226) */
int ios_touch_tilt_on(int on);   /* 1 the device has a tilt sensor, 0 none */
void gs_window_pixels(unsigned *width, unsigned *height);   /* present.c: the picture's pixels */
#endif
#ifdef __ANDROID__
int android_touch_screen(float *xdpi, float *ydpi, int insets[8]);   /* gesture insets l t r b, cutout l t r b (px) */
void android_touch_buzz(int kind);
int android_touch_tilt(float *degrees);
int android_touch_tilt_on(int on);   /* 1 the phone has a tilt sensor, 0 none, -1 not asked */
#endif

enum { EV_DOWN = TOUCH_EV_DOWN, EV_MOVE = TOUCH_EV_MOVE, EV_UP = TOUCH_EV_UP, EV_CANCEL = TOUCH_EV_CANCEL,
       EV_BACK = TOUCH_EV_BACK, EV_PAD_USED = TOUCH_EV_PAD_USED, EV_PAD_LOST = TOUCH_EV_PAD_LOST };

static SDL_Mutex *lock;
static TouchQueue queue;              /* under lock; x, y: 0..1 of the window */
static int available = -1;            /* touch is on here: 1 Android, 2 the mouse */
static SDL_AtomicInt owns;            /* the overlay takes the touches (no menu mouse from them) */
static SDL_AtomicInt want_buzz;       /* TOUCH_BUZZ_* for the SDL thread */
static SDL_AtomicInt tilt_wanted;     /* tilt steering is on */
static SDL_AtomicInt tilt_milli;      /* the phone's roll, millidegrees */
static SDL_AtomicInt tilt_known;      /* tilt_milli is a reading since tilt went on */
static SDL_AtomicInt tilt_sensor;     /* 1 a tilt sensor, 0 none, -1 not known yet */
static SDL_AtomicInt pad_named;       /* a controller sent input since the last touch (finding 227) */
static double pause_due;              /* game thread: the app came back; pause a race offline from then (finding 227) */
static TouchScreen screen;            /* under lock */
static int screen_known;

static Touch *touch;                  /* the game thread's */
static float published[TOUCH_SHAPES * 16];
static int published_count;
static float published_w, published_h;

static double ms_now(void) { return (double)SDL_GetTicksNS() / 1e6; }

/* The editor alone (header): wanted, opened, ended. */
static int editor_alone(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("DHD_TOUCH_EDITOR"); on = v && atoi(v) > 0; }
    return on;
}

/* launcher.c's main, before anything starts: --touch-editor is DHD_TOUCH_EDITOR=1, and either
   mutes the game and skips its movies (the editor covers them; nothing should play behind it). */
void touch_host_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (argv[i] && !strcmp(argv[i], "--touch-editor")) host_putenv_s("DHD_TOUCH_EDITOR", "1");
    if (!editor_alone()) return;
    host_putenv_s("DHD_SND_MUTE", "1");
    host_putenv_s("DHD_SKIP_MOVIES", "1");
    fprintf(stderr, "touch: the layout editor alone (DHD_TOUCH_EDITOR): muted, no movies\n");
}

static double trace_race_ms;   /* the race clock the trace prints */

/* DHD_TOUCH_LOG=1: the checks' trace in dhd.log. */
static int trace_on(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("DHD_TOUCH_LOG"); on = v ? atoi(v) : 0; }   /* 2: the moves too */
    return on;
}

static SDL_InitState started;

/* Touch is on here (1 Android, 2 the mouse; 0 off), and the lock is made, once, from any thread. */
static int touch_on(void)
{
    if (SDL_ShouldInit(&started)) {
        lock = SDL_CreateMutex();
#if defined(__ANDROID__) || defined(TOUCH_IOS)
        available = 1;
        gs_mouse_touchscreen();   /* else the menus' first reading takes the first tap as its baseline */
        SDL_SetAtomicInt(&tilt_sensor, -1);   /* the first tick asks (iOS since finding 226) */
#else
        const char *mouse = getenv("DHD_TOUCH_MOUSE");
        available = mouse && atoi(mouse) ? 2 : 0;
        SDL_SetAtomicInt(&tilt_sensor, 0);    /* a desktop has none */
#endif
        SDL_SetInitialized(&started, true);
    }
    return available;
}

/* The app left the screen or came back (host_app_away, the game thread; finding 227). Away: every
   finger is let go. Back: a moment later (once the game has drawn the race again, so the overlay
   knows the race is on screen) the overlay does what it does when a controller goes away
   mid-race (touch_pad_lost): offline it opens the game's own pause; online nothing, the race goes
   on (finding 215). */
static void push(int kind, long long id, float x, float y, double ms);

void touch_host_app_away(int away)
{
    if (!touch_on()) return;
    if (away) {
        push(EV_CANCEL, -1, 0, 0, ms_now());
        pause_due = 0;
    } else {
        pause_due = ms_now() + 150;
    }
}

/* The pad's button names apply (hle.h): no touch here (every desktop), or a controller has sent
   input since the last touch. A phone starts with touch's words, so a controller that is only
   there (the iOS Simulator's own, a pad in a bag) changes nothing until it is used. */
int touch_host_pad_prompts(void)
{
    return !touch_on() || SDL_GetAtomicInt(&pad_named);
}

static void push(int kind, long long id, float x, float y, double ms)
{
    if (kind == EV_DOWN) SDL_SetAtomicInt(&pad_named, 0);   /* the prompts are touch's again */
    if (!lock) return;
    SDL_LockMutex(lock);
    touch_queue_push(&queue, kind, id, x, y, ms);   /* full: moves merge, never an up (touch.h) */
    SDL_UnlockMutex(lock);
}

/* ---- the screen in mm (SDL thread) ---- */

static void screen_update(SDL_Window *window)
{
    TouchScreen s;
    memset(&s, 0, sizeof s);
#ifdef TOUCH_IOS
    /* no SDL window: the app's view (window is NULL) */
    (void)window;
    float strips[4], cut[4];
    if (!lock || !ios_touch_screen(&s.width, &s.height, strips, cut) || s.width < 20 || s.height < 20) return;
    s.strip_left = strips[0], s.strip_top = strips[1], s.strip_right = strips[2], s.strip_bottom = strips[3];
    s.cut_x0 = cut[0], s.cut_y0 = cut[1], s.cut_x1 = cut[2], s.cut_y1 = cut[3];
#else
    if (!window) return;
    int w = 0, h = 0;
    if (!SDL_GetWindowSizeInPixels(window, &w, &h) || w <= 0 || h <= 0) return;
#endif
#ifdef TOUCH_IOS
#elif defined(__ANDROID__)
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
#ifdef TOUCH_IOS
        if (trace_on())
            fprintf(stderr, "touch log: screen %.1f x %.1f mm, strips %.1f %.1f %.1f %.1f, cutout %.1f %.1f %.1f %.1f mm\n",
                    s.width, s.height, s.strip_left, s.strip_top, s.strip_right, s.strip_bottom,
                    s.cut_x0, s.cut_y0, s.cut_x1, s.cut_y1);
#elif defined(__ANDROID__)
        if (trace_on())
            fprintf(stderr, "touch log: screen %dx%d px, dpi %.1f x %.1f, gesture insets %d %d %d %d, cutout %d %d %d %d px\n",
                    w, h, xdpi, ydpi, in[0], in[1], in[2], in[3], in[4], in[5], in[6], in[7]);
#endif
        screen = s;
        screen_known = 2;   /* changed: the game thread lays out again */
    }
    SDL_UnlockMutex(lock);
}

/* ---- SDL thread ---- */

static long long last_window;
/* the mouse as a finger on a test desktop (DHD_TEST_INPUT): see the motion case below */
static float before_x, before_y, last_x, last_y, last_px, last_py, leave_x = -1, leave_y = -1;
static int leave_dropped;
static int test_input(void)
{
    static int on = -1;
    if (on < 0) on = getenv("DHD_TEST_INPUT") != NULL;
    return on;
}

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
#ifdef TOUCH_IOS
        if (!screen_known) screen_update(NULL);
#endif
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
                /* DHD_TEST_INPUT (a desktop that is never shown, driven by posted messages): Windows
                   reports a leave after each posted move and SDL first moves its pointer to the OS
                   cursor. That motion is no finger's: it is dropped once the leave has told where
                   the OS cursor is (the first one is taken back at its leave), as present.c does for
                   the menus' mouse; else a held finger would jump to the OS cursor after every move. */
                if (test_input() && e->motion.x == leave_x && e->motion.y == leave_y) { leave_dropped = 1; return owned; }
                leave_dropped = 0;
                before_x = last_x, before_y = last_y;
                last_x = x, last_y = y, last_px = e->motion.x, last_py = e->motion.y;
                if (e->motion.state & SDL_BUTTON_LMASK) push(EV_MOVE, 0x7FFF, x, y, ms);
            } else if (e->button.button == SDL_BUTTON_LEFT) {
                /* a test's press is where its posted move put the finger, not SDL's idea of the
                   pointer (the OS cursor's), as present.c does for the menus' mouse (finding 218) */
                if (test_input()) x = last_x, y = last_y;
                push(e->button.down ? EV_DOWN : EV_UP, 0x7FFF, x, y, ms);
            }
            return owned;
        }
        return 0;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:   /* test desktops: the motion before it was the OS cursor's (above) */
        if (touch_on() == 2 && test_input() && !leave_dropped) {
            leave_x = last_px, leave_y = last_py;
            push(EV_MOVE, 0x7FFF, before_x, before_y, (double)e->common.timestamp / 1e6);
            last_x = before_x, last_y = before_y;
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
        SDL_SetAtomicInt(&pad_named, 1);
        push(EV_PAD_USED, 0, 0, 0, (double)e->common.timestamp / 1e6);
        return 0;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (abs(e->gaxis.value) > 16000) {
            SDL_SetAtomicInt(&pad_named, 1);
            push(EV_PAD_USED, 0, 0, 0, (double)e->common.timestamp / 1e6);
        }
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
#ifndef TOUCH_IOS
    if (!last_window && lock) {   /* the screen's size before the first touch (the editor alone needs it at once) */
        int n = 0;
        SDL_Window **windows = SDL_GetWindows(&n);
        if (windows && n > 0) {
            last_window = (long long)SDL_GetWindowID(windows[0]);
            screen_update(windows[0]);
        }
        SDL_free(windows);
    }
#endif
    int buzz = SDL_GetAtomicInt(&want_buzz);
    if (buzz) SDL_SetAtomicInt(&want_buzz, 0);
#if defined(__ANDROID__) || defined(TOUCH_IOS)
#ifdef TOUCH_IOS
#define platform_tilt_on ios_touch_tilt_on
#define platform_tilt ios_touch_tilt
#else
#define platform_tilt_on android_touch_tilt_on
#define platform_tilt android_touch_tilt
#endif
    static int tilt_was = -1;   /* the first pass asks whether there is a sensor */
    static double tilt_retry_at;
    int tilt = SDL_GetAtomicInt(&tilt_wanted);
    if (tilt != tilt_was && ms_now() >= tilt_retry_at) {
        SDL_SetAtomicInt(&tilt_known, 0);   /* the next reading is the new sensor's */
        int sensor = platform_tilt_on(tilt);
        if (sensor >= 0) SDL_SetAtomicInt(&tilt_sensor, sensor), tilt_was = tilt;
        else tilt_retry_at = ms_now() + 250;   /* not asked (no activity yet): again in a moment, not every pass */
    }
    float deg;
    if (tilt && platform_tilt(&deg)) {
        SDL_SetAtomicInt(&tilt_milli, (int)(deg * 1000));
        SDL_SetAtomicInt(&tilt_known, 1);
    }
#endif
#ifdef __ANDROID__
    static double insets_at;
    if (buzz) android_touch_buzz(buzz);
    if (last_window && ms_now() - insets_at > 2000) {   /* the gesture insets change with the navigation mode */
        insets_at = ms_now();
        screen_update(SDL_GetWindowFromID((SDL_WindowID)last_window));
    }
#elif defined(TOUCH_IOS)
    static double screen_at;
    if (buzz) ios_touch_buzz(buzz);
    if (ms_now() - screen_at > 1000) {   /* a turn of the device moves the cutout's side */
        screen_at = ms_now();
        screen_update(NULL);
    }
#else
    (void)buzz;
#endif
}

#ifdef TOUCH_IOS
/* UIKit's touches (metal_bridge.m, the main thread), x and y 0..1 of the view: SDL's finger
   events, as Android's arrive, and for the first finger the mouse SDL makes of it there (the menus'
   layer, finding 092), in the picture's pixels; both reach touch_host_event and present.c on the
   SDL thread. kind: TOUCH_EV_DOWN, _MOVE, _UP or _CANCEL. */
void ios_touch_finger(int kind, long long id, float x, float y)
{
    static long long primary;   /* the finger the mouse follows, 0 none (main thread only) */
    if (!touch_on() || !SDL_WasInit(SDL_INIT_EVENTS)) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = kind == EV_DOWN ? SDL_EVENT_FINGER_DOWN : kind == EV_MOVE ? SDL_EVENT_FINGER_MOTION
           : kind == EV_UP ? SDL_EVENT_FINGER_UP : SDL_EVENT_FINGER_CANCELED;
    e.tfinger.timestamp = SDL_GetTicksNS();
    e.tfinger.touchID = 1;
    e.tfinger.fingerID = (SDL_FingerID)id;
    e.tfinger.x = x, e.tfinger.y = y;
    e.tfinger.pressure = kind == EV_UP || kind == EV_CANCEL ? 0.0f : 1.0f;
    SDL_PushEvent(&e);
    if (kind == EV_DOWN && !primary) primary = id;
    if (id != primary) return;
    unsigned pw = 0, ph = 0;
    gs_window_pixels(&pw, &ph);
    SDL_Event m;
    SDL_zero(m);
    m.motion.timestamp = e.tfinger.timestamp;
    m.motion.which = SDL_TOUCH_MOUSEID;
    float px = x * (float)pw, py = y * (float)ph;
    if (kind == EV_DOWN || kind == EV_MOVE) {
        m.type = SDL_EVENT_MOUSE_MOTION;
        m.motion.x = px, m.motion.y = py;
        m.motion.state = kind == EV_MOVE ? SDL_BUTTON_LMASK : 0;
        SDL_PushEvent(&m);
    }
    if (kind != EV_MOVE) {
        SDL_zero(m);
        m.type = kind == EV_DOWN ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        m.button.timestamp = e.tfinger.timestamp;
        m.button.which = SDL_TOUCH_MOUSEID;
        m.button.button = SDL_BUTTON_LEFT;
        m.button.down = kind == EV_DOWN;
        m.button.clicks = 1;
        m.button.x = px, m.button.y = py;
        SDL_PushEvent(&m);
        if (kind != EV_DOWN) primary = 0;
    }
}

/* The app went inactive (metal_bridge.m): every finger is gone, as Android's DID_ENTER_BACKGROUND. */
void ios_touch_cancel(void)
{
    if (touch_on()) push(EV_CANCEL, -1, 0, 0, ms_now());
}

/* iOS has no system Back (finding 227): a swipe in from the screen's left edge is Android's Back
   (touch_back: in menus the page's BACK, the pause rows' way back), outside a race only, where
   the overlay does not take the touches: in a race the left edge is the steering thumb's. */
int ios_touch_back_allowed(void) { return touch_on() && !SDL_GetAtomicInt(&owns); }
void ios_touch_back(void)
{
    if (ios_touch_back_allowed()) push(EV_BACK, 0, 0, 0, ms_now());
}
#endif

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
    static const char *const preset[] = {"thumb", "classic"}, *const hand[] = {"right", "left"}, *const steer[] = {"stick", "tilt", "dpad"};
    static const char *const stick[] = {"floating", "fixed"}, *const labels[] = {"auto", "always", "off"};
    static const char *const attacks[] = {"flick", "buttons"}, *const pad[] = {"hide", "show"}, *const hud[] = {"game", "move"};
    touch_settings_default(s);
    s->preset = env_choice("DHD_TOUCH_PRESET", preset, 2, TOUCH_CLASSIC);  /* the full pad by default */
    s->left_handed = env_choice("DHD_TOUCH_HAND", hand, 2, 0);
    int steering = env_choice("DHD_TOUCH_STEER", steer, 3, 0);   /* stick, tilt or the D-pad (finding 214) */
    s->tilt = steering == 1;
    s->dpad = steering == 2;
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
    if (log && known && s.dpad) {   /* where the D-pad sits (its rest point and an arm's centre) */
        float x = 0, y = 0, ax = 0, ay = 0;
        if (touch_control(touch, TC_STICK, &x, &y, NULL) && touch_target(touch, TOUCH_TARGET_DPAD, 3, &ax, &ay))
            fprintf(stderr, "touch: d-pad at %.1f,%.1f mm, arms %.1f mm from it\n", x, y, ax - x);
    }
    if (log && known)   /* before the window's size is known there is nothing to say */
        fprintf(stderr, "touch: %s, %s hand, %s, size %s, screen %.1f x %.1f mm (strips l %.1f t %.1f r %.1f b %.1f mm, "
                "cutout x %.1f-%.1f mm)\n",
                s.preset == TOUCH_CLASSIC ? "classic pad" : "thumb zones", s.left_handed ? "left" : "right",
                s.tilt ? "tilt" : s.dpad ? "d-pad" : "stick", s.size ? "set" : "auto", sc.width, sc.height, sc.strip_left, sc.strip_top,
                sc.strip_right, sc.strip_bottom, sc.cut_x0, sc.cut_x1);
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

/* Whether the device has a tilt sensor, for TOUCH CONTROLS (DHD_TOUCH_TILT_SENSOR: the Steer With
   row refuses Tilt without one), and once in dhd.log when tilt is wanted but there is none (the
   stick keeps steering). */
static void tilt_sensor_note(void)
{
    static int told = -2, logged;
    int sensor = SDL_GetAtomicInt(&tilt_sensor);
    if (sensor != told && sensor >= 0) {
        host_putenv_s("DHD_TOUCH_TILT_SENSOR", sensor ? "1" : "0");
        told = sensor;
    }
    if (sensor == 0 && SDL_GetAtomicInt(&tilt_wanted) && !logged) {
        fprintf(stderr, "touch: tilt steering is on, but this device has no gravity or accelerometer sensor: the stick steers\n");
        logged = 1;
    }
}

/* DHD_TOUCH_SCRIPT (the checks only): fingers by the race clock, `S:down|move|up:ID:X:Y;...` with S
   race seconds (during a race's HUD), ID any number and X, Y in mm: the same events as a screen
   gives, queued like them. Posted mouse messages on a test desktop can't hold a finger or drag it
   (SDL loses the mouse focus after each), and a mouse is one finger; this holds two, steady. */
static void script_fire(double race_ms, int race_hud)
{
    static struct { double s; int kind; long long id; float x, y; } ev[256];
    static int count = -1, next;
    if (count < 0) {
        const char *text = getenv("DHD_TOUCH_SCRIPT");
        count = 0;
        while (text && *text && count < 256) {
            double s;
            char kind[8];
            long long id;
            float x, y;
            int n = 0;
            if (sscanf(text, " %lf:%7[a-z]:%lld:%f:%f%n", &s, kind, &id, &x, &y, &n) != 5) break;
            ev[count].s = s, ev[count].id = id, ev[count].x = x, ev[count].y = y;
            ev[count].kind = !strcmp(kind, "down") ? EV_DOWN : !strcmp(kind, "up") ? EV_UP : EV_MOVE;
            count++;
            text += n;
            while (*text == ';' || *text == ' ') text++;
        }
        if (count) fprintf(stderr, "touch: DHD_TOUCH_SCRIPT has %d events\n", count);
    }
    if (!race_hud || next >= count) return;
    SDL_LockMutex(lock);
    float w = screen.width, h = screen.height;
    SDL_UnlockMutex(lock);
    if (w < 10 || h < 10) return;
    for (; next < count && ev[next].s * 1000.0 <= race_ms; next++)
        push(ev[next].kind, ev[next].id, ev[next].x / w, ev[next].y / h, ms_now());
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
    /* The iOS launcher's "Edit the touch layout" (findings 227, 228): ios_main.m sets
       DHD_TOUCH_EDIT_AT_START=1, and the layout editor opens once, as TOUCH CONTROLS > Edit layout
       opens it, when the start's movies are over and no race is on (two seconds into the run at
       least); the game goes on after Done. iOS can't use the editor alone below: its end would
       close the app (a core starts once per process and an iOS app can't start itself again).
       Android's launcher uses the editor alone and comes back (finding 228). */
    static int edit_at_start = -1;
    if (edit_at_start < 0) {
        const char *v = getenv("DHD_TOUCH_EDIT_AT_START");
        edit_at_start = v && *v == '1';
    }
    if (edit_at_start == 1 && !h->movie && !h->attract && !h->race_hud && !h->replay &&
        h->vsync >= 2u * (uint32_t)(h->fps > 0 ? h->fps : 60)) {
        edit_at_start = 0;
        fprintf(stderr, "touch: the layout editor opens (the launcher asked for it)\n");
        touch_open_editor(touch);
    }
    static int alone_state;   /* the editor alone: 0 waiting for the screen, 1 open, 2 ended */
    if (editor_alone() && alone_state == 0) {
        SDL_LockMutex(lock);
        int known = screen_known;
        SDL_UnlockMutex(lock);
        if (known) {
            configure(1);
            touch_open_editor_alone(touch);
            alone_state = 1;
            fprintf(stderr, "touch: the layout editor alone is open\n");
        }
    }
    if (h->command == TOUCH_COMMAND_HINTS) {
        save("touch_hints", "DHD_TOUCH_HINTS", "0");
        configure(0);
    }
    if (h->command == TOUCH_COMMAND_TILT_CENTRE && !SDL_GetAtomicInt(&tilt_known)) {
        fprintf(stderr, "touch: no tilt reading yet; the tilt centre is not changed\n");
    } else if (h->command == TOUCH_COMMAND_TILT_CENTRE) {
        char v[32];
        snprintf(v, sizeof v, "%.1f", SDL_GetAtomicInt(&tilt_milli) / 1000.0);
        save("touch_tilt_centre", "DHD_TOUCH_TILT_CENTRE", v);
        configure(0);
    }
    script_fire(h->race_ms, h->race_hud);
    /* the touches so far, in order */
    static TouchEvent events[TOUCH_QUEUE];
    int n, merged, cancelled;
    SDL_LockMutex(lock);
    n = queue.count;
    memcpy(events, queue.ev, sizeof *events * (size_t)n);
    merged = queue.merged, cancelled = queue.cancelled;
    queue.count = queue.merged = queue.cancelled = 0;
    TouchScreen sc = screen;
    SDL_UnlockMutex(lock);
    if (cancelled) fprintf(stderr, "touch: the touch queue filled while the game waited; every finger was let go\n");
    else if (merged && trace_on()) fprintf(stderr, "touch log: the queue filled; %d moves merged\n", merged);
    for (int i = 0; i < n; i++) {
        const TouchEvent *e = &events[i];
        float x = e->x * sc.width, y = e->y * sc.height;
        if (trace_on() > (e->kind == EV_MOVE)) {
            static const char *const kinds[] = {"down", "move", "up", "cancel", "back", "pad used", "pad lost"};
            fprintf(stderr, "touch log: %s %lld at %.1f,%.1f mm (%.0f ms) race %.0f ms hud %d air %d\n", kinds[e->kind], e->id, x, y,
                    e->ms, h->race_ms, h->race_hud, h->air);
        }
        touch_event(touch, e, x, y);
    }
    tilt_sensor_note();
    int known = SDL_GetAtomicInt(&tilt_known) && SDL_GetAtomicInt(&tilt_wanted);
    touch_tilt(touch, known, SDL_GetAtomicInt(&tilt_milli) / 1000.0f);
    TouchGame g;
    memset(&g, 0, sizeof g);
    g.race_hud = h->race_hud, g.flyover = h->flyover, g.movie = h->movie, g.replay = h->replay, g.attract = h->attract, g.menu_up = h->menu_up, g.air = h->air, g.online = h->online, g.clock_moving = h->clock_moving;
    for (int i = 0; i < 3; i++) g.pause_row[i] = h->pause_row[i];
    g.race_ms = h->race_ms, g.vsync = h->vsync, g.fps = h->fps;
    trace_race_ms = h->race_ms;
    static int trace_air = -1, trace_fly = 0;
    if (trace_on() && h->flyover != trace_fly) {
        fprintf(stderr, "touch log: flyover %d (%.0f ms)\n", h->flyover, ms_now());
        trace_fly = h->flyover;
    }
    static int trace_attract = 0;
    if (trace_on() && h->attract != trace_attract) {   /* the title's attract demo (finding 218) */
        fprintf(stderr, "touch log: attract %d (%.0f ms)\n", h->attract, ms_now());
        trace_attract = h->attract;
    }
    static int trace_movie = 0;
    if (trace_on() && h->movie != trace_movie) {
        fprintf(stderr, "touch log: movie %d (%.0f ms)\n", h->movie, ms_now());
        trace_movie = h->movie;
    }
    if (trace_on() && !h->race_hud) trace_air = -1;   /* each race starts with a line */
    else if (trace_on() && h->air != trace_air) {
        fprintf(stderr, "touch log: air %d (%.0f ms) race %.0f ms\n", h->air, ms_now(), h->race_ms);
        trace_air = h->air;
    }
    if (pause_due && ms_now() >= pause_due) {   /* back from the background (touch_host_app_away) */
        pause_due = 0;
        if (h->race_hud)
            fprintf(stderr, "touch: back from the background in a race: %s\n",
                    h->online ? "online, the race goes on" : "the game's pause");
        touch_pad_lost(touch, ms_now());
    }
    touch_update(touch, &g, ms_now());
    int buzz = touch_take_buzz(touch);
    if (buzz) SDL_SetAtomicInt(&want_buzz, buzz);
    TouchSettings saved;
    if (touch_take_saved(touch, &saved)) save_settings(&saved);
    if (alone_state == 1 && !touch_editor_open(touch)) {   /* Done or Back: saved above; the game ends */
        alone_state = 2;
        fprintf(stderr, "touch: the layout editor alone is done; the game ends\n");
        gs_window_close();
    }
    h->hud_mode = touch_hud_mode(touch);
    h->leave_race = touch_take_ask(touch) == TOUCH_ASK_LEAVE_RACE;
    h->freeze = touch_freeze(touch);
    int own = touch_owns_touches(touch);
    if (own != SDL_GetAtomicInt(&owns))   /* who a tap goes to: a menu that ignores taps shows here (finding 218) */
        fprintf(stderr, "touch: %s\n", own ? "the overlay takes the touches" : "the menus take the touches");
    SDL_SetAtomicInt(&owns, own);
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
    static uint16_t last_buttons;
    static int last_visible = -1, last_lx = 128, last_ly = 128;
    int visible = touch_visible(touch);
    if (trace_on() && (buttons != last_buttons || visible != last_visible || abs(lx - last_lx) >= 8 || abs(ly - last_ly) >= 8 ||
                       (lx == 128) != (last_lx == 128) || (ly == 128) != (last_ly == 128))) {
        fprintf(stderr, "touch log: sent %04x stick %d,%d visible %d (%.0f ms) race %.0f ms\n", buttons, lx, ly, visible,
                ms_now(), trace_race_ms);
        last_lx = lx, last_ly = ly;
    }
    last_buttons = buttons, last_visible = visible;
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
