/* Controllers through SDL3 (finding 059): Xbox pads (XInput), DualSense, DualShock 4, Switch Pro,
   8BitDo and most other USB and Bluetooth pads (DirectInput and HID), with SDL's built-in
   mapping database. Vibration goes to every pad that has motors.

   SDL runs on one thread for the whole process (sdl_host.c): it creates the window, waits for
   events and calls in here to keep each player's latest state and to send the vibration the
   game asks for. The game thread only copies states (gamepad_raw) and turns them into the PS2
   pad's with the player's layout (gamepad_apply). Players are slots: a pad that connects takes
   the first free one and keeps it until it disconnects.

   A state is the pad's inputs by their place (GamepadRaw: port/hle/controls_table.h's ids, the
   triggers' and the sticks' values), as SDL's gamepad API names them: the bottom face button is
   `south` whatever its label (a Switch Pro's B). The layout (dhd.ini pad1, pad2; finding 157)
   says which input is which PS2 button; the default is by position, as on a DualShock 2: south
   Cross, east Circle, west Square, north Triangle, Back/Share/Create SELECT, Start/Options/+
   START, the triggers L2/R2 (with their pressure), the shoulders L1/R1, the sticks pressed L3/R3.
   A pad SDL knows as a joystick but has no gamepad mapping for gets a default one: hat 0 the
   d-pad, axes 0/1 the left stick, 2/3 the right stick, buttons 0-3 south east west north, 4/5 the
   shoulders, 6/7 the triggers (fully pressed or not), 8 back, 9 start, 10/11 the sticks pressed.
   gamecontrollerdb.txt beside dhd.exe (SDL's format) adds or corrects mappings; so does
   SDL_GAMECONTROLLERCONFIG. DHD_SDL=0: no pads (the keyboard still plays).
   The sticks go through the layout's Invert and Swap Sticks, then the dead zone the player chose
   (stick.h, dhd.ini stick_dead_zone; finding 134), and each pad says which kind it is
   (PlayStation, Xbox, Nintendo) for the button icons (port/gs/button_icons.c). */
#include "hle.h"
#include "stick.h"
#include "controls_table.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SLOTS = 4, PRESSED = 30 };  /* a trigger's byte past PRESSED is a press of its button */

typedef struct {
    SDL_JoystickID id;           /* 0: free */
    SDL_Gamepad *gamepad;        /* or, without a mapping, */
    SDL_Joystick *joystick;
    int light, heavy;            /* vibration sent last */
    Uint64 sent;                 /* when (it is sent again before SDL's duration runs out) */
} Slot;

static Slot slots[SLOTS];       /* SDL thread only */
static GamepadRaw latest[SLOTS];  /* under lock */
static int present[SLOTS];      /* under lock */
static SDL_AtomicInt kinds[SLOTS];  /* each slot's PAD_KIND_* (gamepad_kind) */
static SDL_Mutex *lock;         /* made by gamepad_thread_start */
static SDL_AtomicInt want_light[SLOTS], want_heavy[SLOTS];
static SDL_AtomicInt running;   /* 1 once the pads are up, -1 if they couldn't start */

static const uint16_t pressure_order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                            PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};

static uint8_t trigger_byte(Sint16 v) { return (uint8_t)(v <= 0 ? 0 : v >> 7); }

/* The ids of controls_table.h's inputs by name (the joystick's default mapping). */
static int input_id(const char *name)
{
    for (int i = 0; i < CONTROL_PAD_INPUTS; i++)
        if (!strcmp(control_pad_inputs[i].name, name)) return control_pad_inputs[i].id;
    return 0;
}

static void read_gamepad(SDL_Gamepad *g, GamepadRaw *raw)
{
    for (int i = 0; i < CONTROL_PAD_INPUTS; i++) {
        const ControlPadInput *in = &control_pad_inputs[i];
        if (in->sdl >= 0 && SDL_GetGamepadButton(g, (SDL_GamepadButton)in->sdl)) raw->buttons |= 1u << in->id;
    }
    raw->trigger[0] = trigger_byte(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    raw->trigger[1] = trigger_byte(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    raw->axis[0] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
    raw->axis[1] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
    raw->axis[2] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
    raw->axis[3] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY);
}

/* The default mapping for a joystick without a gamepad mapping (see the top). */
static void read_joystick(SDL_Joystick *j, GamepadRaw *raw)
{
    static const char *const buttons[12] = {"south", "east", "west", "north", "lb", "rb", "lt", "rt", "back", "start", "ls", "rs"};
    int n = SDL_GetNumJoystickButtons(j), axes = SDL_GetNumJoystickAxes(j);
    for (int i = 0; i < n && i < 12; i++) {
        if (!SDL_GetJoystickButton(j, i)) continue;
        if (i == 6 || i == 7) raw->trigger[i - 6] = 255;
        else raw->buttons |= 1u << input_id(buttons[i]);
    }
    if (SDL_GetNumJoystickHats(j) > 0) {
        Uint8 hat = SDL_GetJoystickHat(j, 0);
        if (hat & SDL_HAT_UP) raw->buttons |= 1u << input_id("dpad_up");
        if (hat & SDL_HAT_DOWN) raw->buttons |= 1u << input_id("dpad_down");
        if (hat & SDL_HAT_LEFT) raw->buttons |= 1u << input_id("dpad_left");
        if (hat & SDL_HAT_RIGHT) raw->buttons |= 1u << input_id("dpad_right");
    }
    if (axes > 1) raw->axis[0] = SDL_GetJoystickAxis(j, 0), raw->axis[1] = SDL_GetJoystickAxis(j, 1);
    if (axes > 3) raw->axis[2] = SDL_GetJoystickAxis(j, 2), raw->axis[3] = SDL_GetJoystickAxis(j, 3);
}

/* An input's value in a state: a button's 255 while held, a trigger's byte. */
static int input_value(const GamepadRaw *raw, int id)
{
    for (int i = 0; i < CONTROL_PAD_INPUTS; i++)
        if (control_pad_inputs[i].id == id && control_pad_inputs[i].trigger) return raw->trigger[control_pad_inputs[i].trigger - 1];
    return id > 0 && id < 32 && raw->buttons & 1u << id ? 255 : 0;
}

/* An axis turned round: -32768 and 32767 change places, the centre stays. */
static int16_t inverted(int16_t v) { return v == -32768 ? 32767 : (int16_t)-v; }

void gamepad_apply(const struct ControlsLayout *layout, const GamepadRaw *raw, int dead_zone, HostPad *out)
{
    uint8_t pressure[CONTROL_BUTTONS] = {0};
    for (int t = 0; t < CONTROL_BUTTONS; t++)
        for (int s = 0; s < CONTROL_SLOTS; s++) {
            int value = layout->input[t][s] ? input_value(raw, layout->input[t][s]) : 0;
            if (value > pressure[t]) pressure[t] = (uint8_t)value;
            if (value > PRESSED) out->buttons |= control_targets[t].bit;
        }
    for (int i = 0; i < 12; i++)
        for (int t = 0; t < CONTROL_BUTTONS; t++)
            if (control_targets[t].bit == pressure_order[i]) out->pressure[i] = pressure[t];
    int16_t axis[4];
    static const uint8_t invert[4] = {CONTROL_INVERT_LX, CONTROL_INVERT_LY, CONTROL_INVERT_RX, CONTROL_INVERT_RY};
    for (int i = 0; i < 4; i++) axis[i] = layout->invert & invert[i] ? inverted(raw->axis[i]) : raw->axis[i];
    int left = layout->swap_sticks ? 2 : 0, right = 2 - left;
    out->left_x = stick_byte(axis[left], dead_zone), out->left_y = stick_byte(axis[left + 1], dead_zone);
    out->right_x = stick_byte(axis[right], dead_zone), out->right_y = stick_byte(axis[right + 1], dead_zone);
}

/* What its buttons say: the kind SDL knows it as, else the bottom face button's label (a Steam
   Deck or another pad SDL calls standard says A); a pad with neither keeps the PS2's symbols. */
int gamepad_kind_of(int type, int south_label)
{
    switch (type) {
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5:
        return PAD_KIND_PLAYSTATION;
    case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE:
        return PAD_KIND_XBOX;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return PAD_KIND_NINTENDO;
    default:
        break;
    }
    switch (south_label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return PAD_KIND_XBOX;
    case SDL_GAMEPAD_BUTTON_LABEL_B: return PAD_KIND_NINTENDO;
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return PAD_KIND_PLAYSTATION;
    default: return PAD_KIND_NONE;
    }
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
        SDL_SetAtomicInt(&kinds[i], gamepad_kind_of(SDL_GetGamepadType(s->gamepad),
                                                    SDL_GetGamepadButtonLabel(s->gamepad, SDL_GAMEPAD_BUTTON_SOUTH)));
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
    SDL_SetAtomicInt(&kinds[i], PAD_KIND_NONE);
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
        GamepadRaw raw;
        if (!s->id) continue;
        memset(&raw, 0, sizeof raw);
        if (s->gamepad) read_gamepad(s->gamepad, &raw);
        else read_joystick(s->joystick, &raw);
        SDL_LockMutex(lock);
        latest[i] = raw;
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

int gamepad_raw(int player, GamepadRaw *out)
{
    if (player < 0 || player >= SLOTS || !gamepad_start()) return 0;
    SDL_LockMutex(lock);
    int ok = present[player];
    if (ok) *out = latest[player];
    SDL_UnlockMutex(lock);
    return ok;
}

int gamepad_kind(int player)
{
    return player >= 0 && player < SLOTS ? SDL_GetAtomicInt(&kinds[player]) : PAD_KIND_NONE;
}

void gamepad_vibrate(int player, int light, int heavy)
{
    if (player < 0 || player >= SLOTS) return;
    SDL_SetAtomicInt(&want_light[player], light ? 1 : 0);
    SDL_SetAtomicInt(&want_heavy[player], heavy & 255);
}
