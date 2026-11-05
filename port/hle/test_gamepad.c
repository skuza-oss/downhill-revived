/* gamepad.c smoke test (finding 059): SDL3 starts, enumerates the controllers that are plugged in
   and maps each to a pad, without the game. With no controller it only shows that SDL runs.
       test_gamepad [seconds]    also prints each player's state while it changes
       test_gamepad mapping      gamepad.c's reading of what SDL reports (no pad needed): each
                                 kind of pad's button symbols, and stick values through the
                                 dead zone (stick.h): small ones ignored, large ones passed,
                                 the whole travel used (finding 134); a state through
                                 the default and other layouts (finding 157)
   Built by port/tools/test_runtime.py (run there for 0 seconds and with mapping). */
#include "hle.h"
#include "stick.h"
#include "controls_table.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void host_thread_name(const char *name) { (void)name; }
void gs_window_event(unsigned type) { (void)type; }  /* present.c's: this test has no window */
void gs_mouse_event(const void *event) { (void)event; }
void gs_window_tick(void) {}
int touch_host_event(const union SDL_Event *e) { (void)e; return 0; }  /* touch_host.c's: no touch here */
void touch_host_tick(void) {}
double host_pacing_period(void) { return 1.0 / 60; }  /* host.c's, for keyboard.c's field timing */

/* A player's pad as the game gets it: the default layout, the game's dead zone. */
static int read_pad(int player, HostPad *p)
{
    GamepadRaw raw;
    memset(p, 0, sizeof *p);
    if (!gamepad_raw(player, &raw)) return 0;
    gamepad_apply(&control_defaults[CONTROL_PAD1], &raw, STICK_GAME, p);
    return 1;
}

/* the game thread's side, as the game's: SDL's own loop runs on the main thread (sdl_host_main) */
static int run(void *arg)
{
    double seconds = *(double *)arg;
    if (!gamepad_start()) {
        puts("test_gamepad: SDL didn't start");
        return 1;
    }
    int players = 0;
    HostPad last[4];
    memset(last, 0, sizeof last);
    for (int i = 0; i < 4; i++) {
        HostPad p;
        if (!read_pad(i, &p)) continue;
        players++;
        printf("player %d now: buttons %04X left %d,%d right %d,%d\n", i + 1, p.buttons, p.left_x, p.left_y,
               p.right_x, p.right_y);
    }
    printf("test_gamepad: SDL running, %d controller(s)\n", players);
    for (DWORD end = GetTickCount() + (DWORD)(seconds * 1000); GetTickCount() < end; Sleep(10))
        for (int i = 0; i < 4; i++) {
            HostPad p;
            if (read_pad(i, &p) && memcmp(&p, &last[i], sizeof p)) {
                printf("player %d: buttons %04X left %3d,%3d right %3d,%3d L2 %3d R2 %3d\n", i + 1, p.buttons,
                       p.left_x, p.left_y, p.right_x, p.right_y, p.pressure[10], p.pressure[11]);
                gamepad_vibrate(i, (p.buttons & PAD_CROSS) != 0, p.pressure[11]);  /* Cross / R2 rumble */
                last[i] = p;
            }
        }
    for (int i = 0; i < 4; i++) gamepad_vibrate(i, 0, 0);
    Sleep(50);
    return 0;
}

/* ---- test_gamepad mapping ---- */

static int failures;

static void check(int ok, const char *what, int got, int want)
{
    if (!ok) failures++;
    printf("%s %s: %d (want %d)\n", ok ? "ok  " : "FAIL", what, got, want);
}

static int id(const char *name)
{
    for (int i = 0; i < CONTROL_PAD_INPUTS; i++)
        if (!strcmp(control_pad_inputs[i].name, name)) return control_pad_inputs[i].id;
    return 0;
}

static HostPad applied(const ControlsLayout *l, const GamepadRaw *raw)
{
    HostPad p;
    memset(&p, 0, sizeof p);
    gamepad_apply(l, raw, STICK_GAME, &p);
    return p;
}

/* A state through layouts (finding 157): the default is the old mapping by position (south
   Cross, the triggers L2 and R2 with their pressure, the sticks as they are); another layout
   moves the buttons, swaps and inverts the sticks. */
static void run_layouts(void)
{
    static const struct { const char *input; uint16_t button; } positions[] = {
        {"south", PAD_CROSS}, {"east", PAD_CIRCLE}, {"west", PAD_SQUARE}, {"north", PAD_TRIANGLE}, {"back", PAD_SELECT},
        {"start", PAD_START}, {"ls", PAD_L3}, {"rs", PAD_R3}, {"lb", PAD_L1}, {"rb", PAD_R1}, {"dpad_up", PAD_UP},
        {"dpad_down", PAD_DOWN}, {"dpad_left", PAD_LEFT}, {"dpad_right", PAD_RIGHT}};
    const ControlsLayout *def = &control_defaults[CONTROL_PAD1];
    for (size_t i = 0; i < sizeof positions / sizeof *positions; i++) {
        GamepadRaw raw = {0};
        raw.buttons = 1u << id(positions[i].input);
        HostPad p = applied(def, &raw);
        check(p.buttons == positions[i].button, positions[i].input, p.buttons, positions[i].button);
    }
    GamepadRaw raw = {0};
    raw.buttons = 1u << id("south"), raw.trigger[0] = 20, raw.trigger[1] = 200;
    raw.axis[0] = 5000, raw.axis[1] = -5000, raw.axis[2] = 32767, raw.axis[3] = -32768;
    HostPad p = applied(def, &raw);
    check(p.buttons == (PAD_CROSS | PAD_R2), "a light L2 is no press, R2 is", p.buttons, PAD_CROSS | PAD_R2);
    check(p.pressure[10] == 20 && p.pressure[11] == 200 && p.pressure[6] == 255 && p.pressure[5] == 0, "pressures", p.pressure[10], 20);
    check(p.left_x == 147 && p.left_y == 108 && p.right_x == 255 && p.right_y == 0, "sticks", p.left_x, 147);
    /* another layout: Cross on east, Circle on south and L2 (main, alternate), sticks swapped, the left X inverted */
    ControlsLayout l = *def;
    l.input[0][0] = (uint8_t)id("east"), l.input[1][0] = (uint8_t)id("south"), l.input[1][1] = (uint8_t)id("lt");
    l.input[6][0] = 0;
    l.swap_sticks = 1, l.invert = CONTROL_INVERT_LX;
    p = applied(&l, &raw);
    check(p.buttons == (PAD_CIRCLE | PAD_R2), "south is Circle", p.buttons, PAD_CIRCLE | PAD_R2);
    check(p.pressure[5] == 255 && p.pressure[10] == 0, "Circle's pressure: the most of its inputs", p.pressure[5], 255);
    check(p.left_x == 255 && p.left_y == 0 && p.right_x == 108 && p.right_y == 108, "swapped, left X inverted", p.right_x, 108);
    raw.buttons = 1u << id("east"), raw.trigger[0] = 0;
    check(applied(&l, &raw).buttons == (PAD_CROSS | PAD_R2), "east is Cross", applied(&l, &raw).buttons, PAD_CROSS | PAD_R2);
    raw.buttons = 0, raw.trigger[0] = 100;
    check(applied(&l, &raw).buttons == (PAD_CIRCLE | PAD_R2), "LT is Circle's alternate", applied(&l, &raw).buttons, PAD_CIRCLE | PAD_R2);
    raw.axis[0] = 0;
    check(applied(&l, &raw).right_x == 128, "an inverted centre stays the centre", applied(&l, &raw).right_x, 128);
}

/* What gamepad.c does with what SDL reports: each pad's kind (the button symbols, prompts.c) and a
   stick value through the dead zone (stick.h). The SDL build has no virtual joysticks (sdl3.py),
   so the values are given here, as SDL hands them to gamepad.c. */
static int run_mapping(void)
{
    static const struct { const char *name; int type, label, want; } kinds[] = {
        {"Xbox 360", SDL_GAMEPAD_TYPE_XBOX360, SDL_GAMEPAD_BUTTON_LABEL_A, PAD_KIND_XBOX},
        {"Xbox One", SDL_GAMEPAD_TYPE_XBOXONE, SDL_GAMEPAD_BUTTON_LABEL_A, PAD_KIND_XBOX},
        {"DualShock 4", SDL_GAMEPAD_TYPE_PS4, SDL_GAMEPAD_BUTTON_LABEL_CROSS, PAD_KIND_PLAYSTATION},
        {"DualSense", SDL_GAMEPAD_TYPE_PS5, SDL_GAMEPAD_BUTTON_LABEL_CROSS, PAD_KIND_PLAYSTATION},
        {"Switch Pro", SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, SDL_GAMEPAD_BUTTON_LABEL_B, PAD_KIND_NINTENDO},
        {"Joy-Con pair", SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR, SDL_GAMEPAD_BUTTON_LABEL_B, PAD_KIND_NINTENDO},
        {"standard, A at the bottom (Steam Deck)", SDL_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_BUTTON_LABEL_A, PAD_KIND_XBOX},
        {"standard, B at the bottom", SDL_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_BUTTON_LABEL_B, PAD_KIND_NINTENDO},
        {"unknown labels", SDL_GAMEPAD_TYPE_UNKNOWN, SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN, PAD_KIND_NONE},
    };
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++) {
        int got = gamepad_kind_of(kinds[i].type, kinds[i].label);
        check(got == kinds[i].want, kinds[i].name, got, kinds[i].want);
    }
    /* SDL's value, the dead zone (STICK_GAME or %), and the byte the game must get */
    static const struct { int v, dead, want; } cases[] = {
        {0, STICK_GAME, 128}, {5000, STICK_GAME, 147}, {-5000, STICK_GAME, 108}, {32767, STICK_GAME, 255},
        {-32768, STICK_GAME, 0},
        {5000, 20, 128},  {-5000, 20, 128}, {6500, 20, 128},     /* inside 20%: the centre */
        {6600, 20, 178},  {-6600, 20, 76},                       /* just past it: the first bytes that steer */
        {16384, 20, 207}, {-16384, 20, 48},                      /* half way: past the game's own dead zone */
        {32767, 20, 255}, {-32768, 20, 0},                       /* the end of the travel: full */
        {2000, 5, 179},   {1000, 5, 128}, {13000, 40, 128}, {14000, 40, 182},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char what[64];
        int got = stick_byte(cases[i].v, cases[i].dead);
        snprintf(what, sizeof what, "stick %6d at %s%d", cases[i].v, cases[i].dead < 0 ? "game " : "", cases[i].dead);
        check(got == cases[i].want, what, got, cases[i].want);
    }
    /* every value past the zone moves the game (a byte outside its own dead band and the full-lock
       pair), and the bytes only grow with the stick */
    for (int dead = 0; dead <= 95; dead += 5) {
        int last = 128, bad = 0;
        for (int v = 0; v <= 32767; v += 7) {
            int b = stick_byte(v, dead);
            if (b < last || (b != 128 && b < STICK_FIRST_POS)) bad++;
            last = b;
        }
        last = 128;
        for (int v = 0; v >= -32768; v -= 7) {
            int b = stick_byte(v, dead);
            if (b > last || (b != 128 && b > STICK_FIRST_NEG)) bad++;
            last = b;
        }
        if (bad) check(0, "monotone, past the game's band", bad, 0);
    }
    check(stick_dead_zone_of("game") == STICK_GAME && stick_dead_zone_of(NULL) == STICK_GAME &&
          stick_dead_zone_of("15") == 15 && stick_dead_zone_of("400") == 95, "settings' values", 1, 1);
    run_layouts();
    printf("test_gamepad mapping: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "mapping")) return run_mapping();
    double seconds = argc > 1 ? atof(argv[1]) : 0;
    return sdl_host_main(run, &seconds);
}
