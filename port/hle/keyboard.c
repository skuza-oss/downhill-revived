/* The keyboard as player 1's DualShock 2 (finding 059). host.c reads which keys are down (only
   while the game's window has focus) and this file turns them into pad state, merged with the
   controller's, so both work at once. Which key stands for which button is the player's layout
   (dhd.ini keyboard, finding 157; port/hle/controls_table.h, the defaults below when NULL).

   The game reads its controls two ways (src: func_00220550 sets D_0029E290):
   - in menus (D_0029E290 = 0) only buttons count, each press once: the d-pad moves, Cross
     accepts, Triangle goes back, START starts a race or pauses;
   - while riding (D_0029E290 = 1) buttons are held and the left stick steers (X) and leans (Y)
     in proportion (func_0017E1C0, func_0017E300). The d-pad steers too, but always at full lock
     (func_0012DEB0 reads a digital press as 1.0).
   So while riding the keys on the d-pad move the left stick instead of the d-pad, ramped: full
   tilt ~100 ms after the press, back to centre at once on release. The ramp is linear in what
   the game makes of the stick: func_00220AE0 has a dead zone (raw 92-162) and two slopes, and the
   steering squares the result (v * v / 127), so each step of the ramp picks the raw value whose
   steering is the wanted fraction of full lock. Keys on a stick direction (the layout's
   lstick_* and rstick_*) move that stick the same way, in menus too. Esc is no key of the
   layout: it pauses while riding (START) and goes back in menus (Triangle), always.

   A key keeps the meaning it had when it went down until it is let go: Esc pressed while riding
   stays START while the pause menu opens, so the menu doesn't see a Triangle and close again.

   The default layout (keys are Windows virtual-key codes, by their place on a US keyboard):
     arrows     d-pad in menus; steer / lean while riding (left stick, ramped)
     Space, X   Cross     confirm; pedal (twice: sprint)
     S          Triangle  back; brake (tweak a trick in the air)
     Backspace  Triangle  back; brake
     Esc        Triangle in menus (back), START while riding (pause)
     Enter      START     start the race, pause
     Tab        SELECT    hold + up/down: camera; with Q: look back; twice: bail out
     Z          Square    punch
     C          Circle    kick
     A, D       L1, R1    tricks (both: Superman)
     Q, E       L2, R2    tricks; R2 bunny hop
     W          R3        sprint
     F          L3        powerslide */
#include "hle.h"
#include "../include/fps.h"
#include "controls_table.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VK_ESCAPE_ = 0x1B };
enum { NO_AXIS, LEFT_X, LEFT_Y, RIGHT_X, RIGHT_Y, AXES };

#define RAMP_SECONDS 0.1
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
/* Native devices consume the module's published rate, never call game logic. */
#define FIELD_SECONDS (host_pacing_period() < 1.0/60 ? host_pacing_period() : 1.0/60)
#else
#define FIELD_SECONDS (port_delta_clock() ? port_frame_period() : 1.0 / 60)
#endif

static struct {
    int8_t held[256];       /* each key: 0 up, 1 down as in menus, 2 down as while riding */
    int8_t dir[AXES];       /* each stick axis: the direction the keys last gave it, */
    double since[AXES];     /* from when (its ramp starts there) */
} state;

static const struct ControlsLayout *layout_or_default(const struct ControlsLayout *layout)
{
    return layout ? layout : &control_defaults[CONTROL_KEYBOARD];
}

int keyboard_keys(const struct ControlsLayout *layout, uint8_t *list)
{
    layout = layout_or_default(layout);
    int count = 0;
    list[count++] = VK_ESCAPE_;
    for (int t = 0; t < CONTROL_TARGETS; t++)
        for (int s = 0; s < CONTROL_SLOTS; s++)
            if (layout->input[t][s]) list[count++] = layout->input[t][s];
    return count;
}

void keyboard_reset(void) { memset(&state, 0, sizeof state); }

/* The keys the SDL thread reports (keyboard_key). Written there, read by the game thread: a byte
   each, as GetAsyncKeyState's state was read. typed[] starts as the identity: a key that was never
   pressed types its own code. */
static volatile uint8_t position_down[256], position_typed[256], typed_down[256];
static volatile int has_focus;
static volatile int typed_ready;

static void typed_init(void)
{
    if (typed_ready) return;
    for (unsigned i = 0; i < 256; i++) position_typed[i] = (uint8_t)i;
    typed_ready = 1;
}

void keyboard_key(unsigned position, unsigned typed, int down)
{
    if (position >= 256 || typed >= 256) return;
    typed_init();
    if (down) position_typed[position] = (uint8_t)typed;
    position_down[position] = down != 0;
    typed_down[position_typed[position]] = down != 0;
}

unsigned keyboard_typed_of(unsigned position)
{
    if (position >= 256) return position;
    typed_init();
    return position_typed[position];
}

int keyboard_position_down(unsigned position) { return position < 256 && position_down[position]; }

int keyboard_is_down(unsigned typed) { return typed < 256 && typed_down[typed]; }

void keyboard_focus(int focused)
{
    has_focus = focused != 0;
    if (!has_focus)  /* a key let go while the window was away is let go here too */
        for (unsigned i = 0; i < 256; i++) position_down[i] = typed_down[i] = 0;
}

int keyboard_focused(void) { return has_focus; }

/* What the game makes of a raw stick byte: func_00220AE0 (riding, D_0029E290 = 1). */
int keyboard_game_axis(int raw)
{
    int value;
    if (raw >= 226) {
        value = (int)(((float)raw - 225.0f) * 2.133333444595337f + 64.0f);
        if (value >= 128) value = 127;
    } else if (raw >= 163) {
        value = raw - 162;
        if (value >= 65) value = 64;
    } else if (raw < 30) {
        value = (int)((float)raw * 2.133333444595337f + -127.0f);
        if (value <= -128) value = -127;
    } else if (raw < 92) {
        value = (int)((92.0f - (float)raw) * -1.0322580337524414f);
        if (value <= -65) value = -64;
    } else
        value = 0;
    if (value >= 128) value = 127;
    else if (value <= -128) value = -127;
    return abs(value) <= 1 ? 0 : value;
}

/* The raw byte for a fraction (0..1] of full steering in direction dir: the first one out from
   the centre whose steering (value squared / 127, func_0017E1C0) reaches it. */
static uint8_t ramp_raw(double fraction, int dir)
{
    int want = (int)ceil(fraction * 127.0 - 1e-9), raw = 128;
    if (want < 1) want = 1;
    for (int step = 1; step <= 128; step++) {
        raw = dir > 0 ? 127 + step : 128 - step;
        if (raw > 255) raw = 255;
        if (raw < 0) raw = 0;
        int v = abs(keyboard_game_axis(raw));
        if (v * v / 127 >= want || (v >= 127)) break;
    }
    return (uint8_t)raw;
}

/* The left stick axis and way a d-pad button steers while riding (0: not the d-pad). */
static int dpad_axis(uint16_t bit, int *dir)
{
    switch (bit) {
    case PAD_LEFT: *dir = -1; return LEFT_X;
    case PAD_RIGHT: *dir = 1; return LEFT_X;
    case PAD_UP: *dir = -1; return LEFT_Y;
    case PAD_DOWN: *dir = 1; return LEFT_Y;
    default: return NO_AXIS;
    }
}

/* Merges the keys that are down into `out` (a controller's state, or a neutral pad): buttons and
   pressures add, a stick follows its keys (the left one the d-pad's keys while riding). */
void keyboard_pad(const struct ControlsLayout *layout, const uint8_t down[256], int riding, double now, HostPad *out)
{
    static const uint16_t pressure_order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                                PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
    layout = layout_or_default(layout);
    uint16_t buttons = 0;
    int dir[AXES] = {0};
    if (!down[VK_ESCAPE_]) state.held[VK_ESCAPE_] = 0;
    else {
        if (!state.held[VK_ESCAPE_]) state.held[VK_ESCAPE_] = riding ? 2 : 1;
        buttons |= state.held[VK_ESCAPE_] == 2 ? PAD_START : PAD_TRIANGLE;
    }
    for (int t = 0; t < CONTROL_TARGETS; t++)
        for (int s = 0; s < CONTROL_SLOTS; s++) {
            int key = layout->input[t][s], way = 0, axis;
            if (!key || key == VK_ESCAPE_) continue;
            if (!down[key]) {
                state.held[key] = 0;
                continue;
            }
            if (!state.held[key]) state.held[key] = riding ? 2 : 1;
            const ControlTarget *c = &control_targets[t];
            if (c->axis) dir[c->axis] += c->dir;
            else if (state.held[key] == 2 && (axis = dpad_axis(c->bit, &way))) dir[axis] += way;
            else buttons |= c->bit;
        }
    for (int axis = LEFT_X; axis < AXES; axis++) {
        if (dir[axis] != state.dir[axis]) {  /* pressed, let go or turned: the ramp starts again */
            state.dir[axis] = (int8_t)dir[axis];
            state.since[axis] = now;
        }
        if (!dir[axis]) continue;  /* none, or both directions: the centre */
        double fraction = (now - state.since[axis] + FIELD_SECONDS) / RAMP_SECONDS;
        if (fraction > 1) fraction = 1;
        uint8_t raw = ramp_raw(fraction, dir[axis]);
        if (axis == LEFT_X) out->left_x = raw;
        else if (axis == LEFT_Y) out->left_y = raw;
        else if (axis == RIGHT_X) out->right_x = raw;
        else out->right_y = raw;
    }
    out->buttons |= buttons;
    for (int i = 0; i < 12; i++)
        if (buttons & pressure_order[i]) out->pressure[i] = 255;
}

/* A key by its dhd.ini name (controls_table.h; "esc" too): its virtual-key code, 0 none. */
static int key_named(const char *name, size_t n)
{
    if (n == 3 && !strncmp(name, "esc", 3)) return VK_ESCAPE_;
    for (int i = 0; i < CONTROL_KEYS; i++)
        if (strlen(control_keys[i].name) == n && !strncmp(control_keys[i].name, name, n)) return control_keys[i].vk;
    return 0;
}

/* DHD_KEYS (tests): "start:duration:key[+key],..." as DHD_RACE_PAD, seconds on the race clock
   after GO; keys by their dhd.ini names (up down left right space enter tab backspace, a letter,
   a digit, f1, kp1, lshift, ...) or esc. Sets down[] for the given time; 0 if the script is
   invalid. */
int keyboard_script(const char *script, double seconds, uint8_t down[256])
{
    memset(down, 0, 256);
    while (script && *script) {
        double start, duration;
        char chord[128];
        int consumed = 0;
        if (sscanf(script, "%lf:%lf:%127[a-z0-9_+]%n", &start, &duration, chord, &consumed) != 3 || start < 0 ||
            duration <= 0)
            return 0;
        int active = seconds >= start && seconds < start + duration;
        for (char *name = chord; *name;) {
            char *end = strchr(name, '+');
            size_t n = end ? (size_t)(end - name) : strlen(name);
            int key = key_named(name, n);
            if (!key) return 0;
            if (active) down[key] = 1;
            name += n + (end != NULL);
        }
        script += consumed;
        if (*script == ',') script++;
        else if (*script) return 0;
    }
    return 1;
}
