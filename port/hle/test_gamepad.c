/* gamepad.c smoke test (finding 059): SDL3 starts, enumerates the controllers that are plugged in
   and maps each to a pad, without the game. With no controller it only shows that SDL runs.
       test_gamepad [seconds]    also prints each player's state while it changes
       test_gamepad mapping      gamepad.c's reading of what SDL reports (no pad needed): each
                                 kind of pad's button symbols, and stick values through the
                                 dead zone (stick.h): small ones ignored, large ones passed,
                                 the whole travel used (finding 134)
   Built by port/tools/test_runtime.py (run there for 0 seconds and with mapping). */
#include "hle.h"
#include "stick.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void host_thread_name(const char *name) { (void)name; }
void gs_window_event(unsigned type) { (void)type; }  /* present.c's: this test has no window */
void gs_mouse_event(const void *event) { (void)event; }
void gs_window_tick(void) {}
double host_pacing_period(void) { return 1.0 / 60; }  /* host.c's, for keyboard.c's field timing */

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
        if (!gamepad_read(i, &p)) continue;
        players++;
        printf("player %d now: buttons %04X left %d,%d right %d,%d\n", i + 1, p.buttons, p.left_x, p.left_y,
               p.right_x, p.right_y);
    }
    printf("test_gamepad: SDL running, %d controller(s)\n", players);
    for (DWORD end = GetTickCount() + (DWORD)(seconds * 1000); GetTickCount() < end; Sleep(10))
        for (int i = 0; i < 4; i++) {
            HostPad p;
            if (gamepad_read(i, &p) && memcmp(&p, &last[i], sizeof p)) {
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
    printf("test_gamepad mapping: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "mapping")) return run_mapping();
    double seconds = argc > 1 ? atof(argv[1]) : 0;
    return sdl_host_main(run, &seconds);
}
