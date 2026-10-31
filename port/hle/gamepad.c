/* Controllers through SDL3 (finding 059): Xbox pads (XInput), DualSense, DualShock 4, Switch Pro,
   8BitDo and most other USB and Bluetooth pads (DirectInput and HID), with SDL's built-in
   mapping database. Vibration goes to every pad that has motors.

   SDL runs on one thread for the whole process (sdl_host.c): it creates the window, waits for
   events and calls in here to keep each player's latest state and to send the vibration the
   game asks for. The game thread only copies states (gamepad_read). Players are slots: a pad
   that connects takes the first free one and keeps it until it disconnects.

   Buttons by position, as on a DualShock 2: the bottom face button is Cross, right Circle, left
   Square, top Triangle, whatever the labels (a Switch Pro's B is Cross). Back/Share/Create is
   SELECT, Start/Options/+ is START, the triggers are L2/R2 (with their pressure).
   Which SDL button, trigger or axis drives each control is controls.c's (finding 124): the defaults
   are the layout above, and the CONTROLS page rebinds them. The mapping is read afresh each pass,
   so a rebind takes effect on the next one.
   A pad SDL knows as a joystick but has no gamepad mapping for gets a default one: hat 0 the
   d-pad, axes 0/1 the left stick, 2/3 the right stick, buttons 0-3 Cross Circle Square
   Triangle, 4/5 L1/R1, 6/7 L2/R2, 8 SELECT, 9 START, 10/11 L3/R3. gamecontrollerdb.txt beside
   dhd.exe (SDL's format) adds or corrects mappings; so does SDL_GAMECONTROLLERCONFIG.
   DHD_SDL=0: no pads (the keyboard still plays). */
#include "hle.h"
#include "controls.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SLOTS = 4 };

typedef struct {
    SDL_JoystickID id;           /* 0: free */
    SDL_Gamepad *gamepad;        /* or, without a mapping, */
    SDL_Joystick *joystick;
    int light, heavy;            /* vibration sent last */
    Uint64 sent;                 /* when (it is sent again before SDL's duration runs out) */
} Slot;

static Slot slots[SLOTS];       /* SDL thread only */
static HostPad latest[SLOTS];   /* under lock */
static int present[SLOTS];      /* under lock */
static SDL_Mutex *lock;         /* made by gamepad_thread_start */
static SDL_AtomicInt want_light[SLOTS], want_heavy[SLOTS];
static SDL_AtomicInt running;   /* 1 once the pads are up, -1 if they couldn't start */

static const uint16_t pressure_order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                            PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};

static uint8_t axis_byte(Sint16 v)
{
    int x = (v >> 8) + 128;
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

static uint8_t trigger_byte(Sint16 v) { return (uint8_t)(v <= 0 ? 0 : v >> 7); }

/* The SDL button a source is; SDL_GAMEPAD_BUTTON_INVALID when it isn't one. */
static SDL_GamepadButton sdl_button_of(unsigned source)
{
    switch (source) {
    case PADSRC_SOUTH: return SDL_GAMEPAD_BUTTON_SOUTH;
    case PADSRC_EAST: return SDL_GAMEPAD_BUTTON_EAST;
    case PADSRC_WEST: return SDL_GAMEPAD_BUTTON_WEST;
    case PADSRC_NORTH: return SDL_GAMEPAD_BUTTON_NORTH;
    case PADSRC_BACK: return SDL_GAMEPAD_BUTTON_BACK;
    case PADSRC_START: return SDL_GAMEPAD_BUTTON_START;
    case PADSRC_LEFT_SHOULDER: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case PADSRC_RIGHT_SHOULDER: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case PADSRC_L3: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case PADSRC_R3: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case PADSRC_DPAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case PADSRC_DPAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case PADSRC_DPAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case PADSRC_DPAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return SDL_GAMEPAD_BUTTON_INVALID;
    }
}

/* The SDL axis a source is (a trigger or a stick); SDL_GAMEPAD_AXIS_INVALID when it isn't one. */
static SDL_GamepadAxis sdl_axis_of(unsigned source)
{
    switch (source) {
    case PADSRC_LEFT_TRIGGER: return SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    case PADSRC_RIGHT_TRIGGER: return SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    case PADSRC_LEFT_X: return SDL_GAMEPAD_AXIS_LEFTX;
    case PADSRC_LEFT_Y: return SDL_GAMEPAD_AXIS_LEFTY;
    case PADSRC_RIGHT_X: return SDL_GAMEPAD_AXIS_RIGHTX;
    case PADSRC_RIGHT_Y: return SDL_GAMEPAD_AXIS_RIGHTY;
    default: return SDL_GAMEPAD_AXIS_INVALID;
    }
}

/* The controls' bound sources (controls.c, finding 124) into `out`: a button control is a button
   or a trigger, a stick control is an axis. */
static void read_gamepad(SDL_Gamepad *g, HostPad *out)
{
    uint8_t l2 = 0, r2 = 0;
    int total = control_pad_count();
    if (total > CONTROL_PAD_MAX) total = CONTROL_PAD_MAX;
    for (int i = 0; i < total; i++) {
        unsigned source = control_pad_source(i);
        uint8_t value = 0;
        if (!source) continue;
        if (control_pad_axis(i)) {
            SDL_GamepadAxis a = sdl_axis_of(source);
            uint8_t v = a == SDL_GAMEPAD_AXIS_INVALID ? 128 : axis_byte(SDL_GetGamepadAxis(g, a));
            switch (control_pad_axis(i)) {
            case CONTROL_AXIS_LEFT_X: out->left_x = v; break;
            case CONTROL_AXIS_LEFT_Y: out->left_y = v; break;
            case CONTROL_AXIS_RIGHT_X: out->right_x = v; break;
            case CONTROL_AXIS_RIGHT_Y: out->right_y = v; break;
            default: break;
            }
            continue;
        }
        if (source == PADSRC_LEFT_TRIGGER || source == PADSRC_RIGHT_TRIGGER) {
            value = trigger_byte(SDL_GetGamepadAxis(g, sdl_axis_of(source)));
            if (value > 30) out->buttons |= (uint16_t)control_pad_button(i);
        } else {
            SDL_GamepadButton b = sdl_button_of(source);
            if (b != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(g, b)) {
                out->buttons |= (uint16_t)control_pad_button(i);
                value = 255;
            }
        }
        if (control_pad_button(i) == PAD_L2) l2 = value;
        else if (control_pad_button(i) == PAD_R2) r2 = value;
    }
    for (int i = 0; i < 12; i++)
        out->pressure[i] = pressure_order[i] == PAD_L2 ? l2 : pressure_order[i] == PAD_R2 ? r2
                         : out->buttons & pressure_order[i] ? 255 : 0;
}

/* The default mapping for a joystick without a gamepad mapping (see the top). */
static void read_joystick(SDL_Joystick *j, HostPad *out)
{
    static const uint16_t buttons[12] = {PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE, PAD_L1, PAD_R1,
                                         PAD_L2, PAD_R2, PAD_SELECT, PAD_START, PAD_L3, PAD_R3};
    int n = SDL_GetNumJoystickButtons(j), axes = SDL_GetNumJoystickAxes(j);
    for (int i = 0; i < n && i < 12; i++)
        if (SDL_GetJoystickButton(j, i)) out->buttons |= buttons[i];
    if (SDL_GetNumJoystickHats(j) > 0) {
        Uint8 hat = SDL_GetJoystickHat(j, 0);
        if (hat & SDL_HAT_UP) out->buttons |= PAD_UP;
        if (hat & SDL_HAT_DOWN) out->buttons |= PAD_DOWN;
        if (hat & SDL_HAT_LEFT) out->buttons |= PAD_LEFT;
        if (hat & SDL_HAT_RIGHT) out->buttons |= PAD_RIGHT;
    }
    if (axes > 1) out->left_x = axis_byte(SDL_GetJoystickAxis(j, 0)), out->left_y = axis_byte(SDL_GetJoystickAxis(j, 1));
    if (axes > 3) out->right_x = axis_byte(SDL_GetJoystickAxis(j, 2)), out->right_y = axis_byte(SDL_GetJoystickAxis(j, 3));
    for (int i = 0; i < 12; i++) out->pressure[i] = out->buttons & pressure_order[i] ? 255 : 0;
}

static int slot_of(SDL_JoystickID id)
{
    for (int i = 0; i < SLOTS; i++)
        if (slots[i].id == id) return i;
    return -1;
}

static void attach(SDL_JoystickID id)
{
    int i = slot_of(id);
    if (i >= 0 || (i = slot_of(0)) < 0) return;  /* already there, or four are */
    Slot *s = &slots[i];
    const char *name, *how;
    if (SDL_IsGamepad(id)) {
        if (!(s->gamepad = SDL_OpenGamepad(id))) return;
        name = SDL_GetGamepadName(s->gamepad);
        how = SDL_GetGamepadStringForType(SDL_GetGamepadType(s->gamepad));
    } else {
        if (!(s->joystick = SDL_OpenJoystick(id))) return;
        name = SDL_GetJoystickName(s->joystick);
        how = "no mapping: default layout";
    }
    s->id = id;
    s->light = s->heavy = 0;
    if (s->gamepad) SDL_SetGamepadPlayerIndex(s->gamepad, i);
    SDL_PropertiesID props = s->gamepad ? SDL_GetGamepadProperties(s->gamepad) : SDL_GetJoystickProperties(s->joystick);
    int motors = SDL_GetBooleanProperty(props, SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
    fprintf(stderr, "controller: %s (%s%s) is player %d\n", name ? name : "?", how ? how : "gamepad",
            motors ? ", vibration" : "", i + 1);
}

static void detach(SDL_JoystickID id)
{
    int i = slot_of(id);
    if (i < 0) return;
    SDL_LockMutex(lock);
    if (slots[i].gamepad) SDL_CloseGamepad(slots[i].gamepad);
    if (slots[i].joystick) SDL_CloseJoystick(slots[i].joystick);
    memset(&slots[i], 0, sizeof slots[i]);
    present[i] = 0;
    SDL_UnlockMutex(lock);
    fprintf(stderr, "controller: player %d disconnected\n", i + 1);
}

/* A joystick that turns out to be a gamepad (a mapping added) is opened again as one. */
static void remap(SDL_JoystickID id)
{
    int i = slot_of(id);
    if (i >= 0 && slots[i].joystick && SDL_IsGamepad(id)) {
        detach(id);
        attach(id);
    }
}

static void rumble(Slot *s, int light, int heavy)
{
    Uint64 now = SDL_GetTicks();
    if (light == s->light && heavy == s->heavy && (!light && !heavy || now - s->sent < 500)) return;
    /* the small motor is on or off (high frequency), the large one has a speed (low frequency) */
    Uint16 low = (Uint16)(heavy * 257), high = light ? 0xFFFF : 0;
    if (s->gamepad) SDL_RumbleGamepad(s->gamepad, low, high, 1000);
    else if (s->joystick) SDL_RumbleJoystick(s->joystick, low, high, 1000);
    s->light = light, s->heavy = heavy, s->sent = now;
}

static void stop_vibration(void);

/* ---- called by the SDL thread (sdl_host.c) ---- */

static int enabled(void)
{
    const char *setting = getenv("DHD_SDL");  /* DHD_SDL=0: no pads */
    return !(setting && !atoi(setting));
}

/* Once, on the SDL thread after SDL's events are up: 1 if the pads are up. */
int gamepad_thread_start(void)
{
    static int registered;
    if (!enabled()) {
        SDL_SetAtomicInt(&running, -1);
        return 0;
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");  /* the window may not have the focus */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "controller: SDL didn't start (%s); no controllers (the keyboard still plays)\n", SDL_GetError());
        SDL_SetAtomicInt(&running, -1);
        return 0;
    }
    lock = SDL_CreateMutex();
    const char *base = SDL_GetBasePath();  /* the folder of dhd.exe, with its separator */
    char path[1024];
    snprintf(path, sizeof path, "%sgamecontrollerdb.txt", base ? base : "");
    SDL_PathInfo info;
    if (SDL_GetPathInfo(path, &info)) {
        int added = SDL_AddGamepadMappingsFromFile(path);
        fprintf(stderr, "controller: %d mappings from %s\n", added, path);
    }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids && i < count; i++) attach(ids[i]);
    SDL_free(ids);
    SDL_SetAtomicInt(&running, 1);
    gamepad_thread_update();  /* the pads there at the start have a state */
    if (!registered++) atexit(stop_vibration);
    return 1;
}

/* A device came or went (SDL's joystick and gamepad events, relayed by sdl_host.c). */
void gamepad_device(int change, unsigned id)
{
    if (SDL_GetAtomicInt(&running) != 1) return;
    if (change == PADDEV_ADDED) attach(id);
    else if (change == PADDEV_REMOVED) detach(id);
    else if (change == PADDEV_REMAPPED) remap(id);
}

/* Every pass of the SDL thread: the pads' states for the game, and the vibration asked for. */
void gamepad_thread_update(void)
{
    if (SDL_GetAtomicInt(&running) != 1) return;
    for (int i = 0; i < SLOTS; i++) {
        Slot *s = &slots[i];
        HostPad p;
        if (!s->id) continue;
        memset(&p, 0, sizeof p);
        p.left_x = p.left_y = p.right_x = p.right_y = 128;
        if (s->gamepad) read_gamepad(s->gamepad, &p);
        else read_joystick(s->joystick, &p);
        SDL_LockMutex(lock);
        latest[i] = p;
        present[i] = 1;
        SDL_UnlockMutex(lock);
        rumble(s, SDL_GetAtomicInt(&want_light[i]), SDL_GetAtomicInt(&want_heavy[i]));
    }
}

/* At exit: no pad left vibrating (the motors keep the last command when the game ends mid-rumble). */
static void stop_vibration(void)
{
    if (!lock) return;
    SDL_LockMutex(lock);
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].gamepad) SDL_RumbleGamepad(slots[i].gamepad, 0, 0, 0);
        else if (slots[i].joystick) SDL_RumbleJoystick(slots[i].joystick, 0, 0, 0);
    }
    SDL_UnlockMutex(lock);
}

/* ---- the game's side ---- */

int gamepad_start(void)
{
    if (!enabled()) return 0;
    sdl_host_start();  /* the SDL thread, once; it returns when the pads are up or failed */
    return SDL_GetAtomicInt(&running) > 0;
}

int gamepad_read(int player, HostPad *out)
{
    if (player < 0 || player >= SLOTS || !gamepad_start()) return 0;
    SDL_LockMutex(lock);
    int ok = present[player];
    if (ok) *out = latest[player];
    SDL_UnlockMutex(lock);
    return ok;
}

void gamepad_vibrate(int player, int light, int heavy)
{
    if (player < 0 || player >= SLOTS) return;
    SDL_SetAtomicInt(&want_light[player], light ? 1 : 0);
    SDL_SetAtomicInt(&want_heavy[player], heavy & 255);
}
