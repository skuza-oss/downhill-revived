/* Reproducible input without window focus: start:duration:button[+button],...
   Times are host seconds from boot; intervals include start, exclude end. */
#include "hle.h"
#include "../runtime/portable.h"
#include "stick.h"
#include "controls_table.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


int autopad_read(const char *script, double seconds, uint16_t *buttons)
{
    static const struct { const char *name; uint16_t mask; } names[] = {
        {"cross",PAD_CROSS}, {"circle",PAD_CIRCLE}, {"square",PAD_SQUARE}, {"triangle",PAD_TRIANGLE},
        {"start",PAD_START}, {"select",PAD_SELECT}, {"up",PAD_UP}, {"down",PAD_DOWN},
        {"left",PAD_LEFT}, {"right",PAD_RIGHT}, {"l1",PAD_L1}, {"r1",PAD_R1}, {"l2",PAD_L2}, {"r2",PAD_R2}
    };
    *buttons = 0;
    if (!script) return 0;
    /* An opt-in file lets proof drivers advance only after the expected menu
       state, without sending later START presses into a running race. */
    char file_script[4096];
    if (*script == '@') {
        /* Keep readers valid across the proof driver's atomic replacement: the host's reads share the
           file for deleting too (host_win32.c). */
        HostFile file = host_file_open(script + 1, HOST_FILE_READ);
        if (file == HOST_FILE_NONE) return 0;
        int64_t size = host_file_read(file, file_script, sizeof file_script);
        host_file_close(file);
        if (size < 0 || size == (int64_t)sizeof file_script || memchr(file_script, 0, (size_t)size)) return 0;
        while (size && (file_script[size-1] == '\r' || file_script[size-1] == '\n' ||
                        file_script[size-1] == ' ' || file_script[size-1] == '\t')) size--;
        file_script[size] = 0;
        script = file_script;
    }
    while (*script) {
        double start, duration;
        char chord[128];
        int consumed = 0;
        if (sscanf(script, "%lf:%lf:%127[a-z0-9+]%n", &start, &duration, chord, &consumed) != 3 ||
            !isfinite(start) || !isfinite(duration) || start < 0 || duration <= 0)
            return 0;
        uint16_t mask = 0;
        char *name = chord;
        for (;;) {
            char *end = strchr(name, '+');
            if (end) *end = 0;
            unsigned i;
            for (i = 0; i < sizeof names / sizeof *names && strcmp(name, names[i].name); i++) {}
            if (i == sizeof names / sizeof *names) return 0;
            mask |= names[i].mask;
            if (!end) break;
            name = end + 1;
        }
        if (seconds >= start && seconds < start + duration) *buttons |= mask;
        script += consumed;
        if (!*script) break;
        if (*script++ != ',' || !*script) return 0;
    }
    return 1;
}

/* Race-relative input and capture for PCSX2 comparisons (DHD_RACE_SYNC). The patched PCSX2
   (port/tools/prepare_pcsx2_compare.py) applies the same rule, so both runs meet at the same
   game moments whatever their boot, loading and frame times. Moments are keyed by the game's
   race clock (60 per second, one per PS2 vsync), which restarts at the countdown (menu 9, race
   phase 1) and at GO (phase 2, still running in phase 3). The port's own vsyncs don't serve:
   under its virtual clock it spends more of them per game frame than the PS2. */
#define RACE_MENU (*(volatile int32_t *)0x0829DED8)
#define RACE_PHASE (*(volatile int32_t *)0x0843F348)
#define RACE_CLOCK (*(volatile int32_t *)0x0843EEC8)
static int race_started[2], race_clock;
static unsigned long long race_field = ~0ull;
static volatile unsigned long long race_pad_field;

/* Opt-in visual survey: NEW LEVEL retains shell menu 2. Arm the existing
   race-clock input/capture only after the built-in unlock bootstrap has
   loaded a fresh rider with its ordinary cheat flags cleared. Default
   comparisons still arm on their first menu-9 countdown, unchanged. */
static HleEnv race_sync_env, race_unlocked_env;
static int race_sync_menu(void)
{
    return RACE_MENU == 9 || (hle_env(&race_unlocked_env, "DHD_RACE_SYNC_UNLOCKED") && RACE_MENU == 2);
}

static int race_sync_ready(void)
{
    if (!hle_env(&race_unlocked_env, "DHD_RACE_SYNC_UNLOCKED")) return 1;
    return *(volatile uint32_t *)0x084346B4 == 0x035FFFFFu &&
           *(volatile uint32_t *)0x082B6E68 == 0 &&
           *(volatile uint32_t *)0x082B6E6C == 0 &&
           *(volatile uint32_t *)0x082B6E70 == 0;
}

/* Every 30 s of wall time (host_heartbeat_start), where the run is: the virtual field, the game's menu,
   race phase and clock, and the field of the last pad read. The comparison drivers stop on a stalled
   heartbeat and report the screen, instead of waiting forever; the host runs the timer on its own
   thread, so it reports even when the EE no longer reaches a vsync. */
static void race_heartbeat(uint32_t ctx)
{
    extern uint64_t kernel_vsync_count;
    static unsigned long long last = ~0ull;
    unsigned long long field = kernel_vsync_count;
    uintptr_t eip = 0;  /* a host code address */
    if (field == last)  /* stalled: where the thread that reads the pad is spinning */
        eip = host_heartbeat_eip();
    last = field;
    fprintf(stderr, "compare: heartbeat field=%llu menu=%d phase=%d clock=%d pad=%llu eip=%08llX\n", field,
            RACE_MENU, RACE_PHASE, RACE_CLOCK, race_pad_field, (unsigned long long)eip);
}

/* DHD_NAV: menu navigation by the screen shown instead of by time (menu_nav.h), the same rule as
   the PCSX2 adapter's DHD_COMPARE_NAV. Holds this field's buttons; 0 for an invalid script. */
#include "menu_nav.h"
static void nav_dump(const char *name, const char *when)
{
    const char *dir = host_getenv("DHD_NAV_DUMP");
    char path[1024];
    if (!dir) return;
    snprintf(path, sizeof path, "%s/%s-%s.bin", dir, name, when);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    graphics_settle();
    fwrite((const void *)0x08000000, 32u << 20, 1, f);
    fclose(f);
    fprintf(stderr, "compare: nav dump %s\n", path);
}

static unsigned nav_read(unsigned address) { return *(volatile uint32_t *)(0x08000000u + (address & 0x1FFFFFCu)); }

int autopad_nav(const char *script, unsigned long long field, uint16_t *buttons)
{
    static MenuNav nav;
    static unsigned long long last = ~0ull;
    static unsigned held;
    static char *current;
    if (!current || strcmp(current, script)) {  /* a new script (online.c sets one at the race): from its first step */
        free(current);
        current = dhd_strdup(script);
        memset(&nav, 0, sizeof nav);
        held = 0;
    }
    if (field != last) {
        int step = nav.step;
        unsigned long long pressed = nav.pressed;
        static unsigned long long post;
        static char name[64];
        last = field;
        if (!menu_nav(&nav, script, RACE_MENU, RACE_PHASE, nav_read, field, &held)) return 0;
        if (nav.step != step)
            fprintf(stderr, "compare: nav step %d field=%llu menu=%d phase=%d\n", nav.step, field, RACE_MENU, RACE_PHASE);
        /* DHD_NAV_DUMP=dir: EE RAM as each press starts and MENU_NAV_RETRY - 1 fields later, for
           finding what a screen's press changed. Game data: keep the directory under build/. */
        if (nav.pressed && nav.pressed != pressed) {
            snprintf(name, sizeof name, "nav-s%d-t%d-f%llu", nav.step, nav.tries, field);
            nav_dump(name, "pre");
            post = field + MENU_NAV_RETRY - 1;
        } else if (field == post) {
            nav_dump(name, "post");
        }
    }
    *buttons = (uint16_t)held;
    return 1;
}

static int race_in(int which)
{
    int phase = RACE_PHASE;
    return race_started[which] && race_sync_menu() && (which ? phase == 2 || phase == 3 : phase == 1);
}

void race_sync_vsync(unsigned long long field)
{
    if (!hle_env(&race_sync_env, "DHD_RACE_SYNC") || field == race_field) return;
    if (!race_started[0] && !race_sync_ready()) return;
    static int heartbeat;
    if (!heartbeat++) host_callback_heartbeat(HOST_CALLBACK(race_heartbeat), 0);
    race_field = field;
    race_clock = RACE_CLOCK;
    /* DHD_RACE_FRAMELOG: the game's frame counters every field, as the PCSX2 adapter's
       DHD_COMPARE_FRAMELOG: main-loop frames (D_0029E10C) and load-bar frames (D_00663108). */
    static int frame_log = -1;
    if (frame_log < 0) frame_log = host_getenv("DHD_RACE_FRAMELOG") != NULL;
    if (frame_log)
        fprintf(stderr, "compare: frame field=%llu frame=%d loadbar=%d menu=%d phase=%d clock=%d\n", field,
                *(volatile int32_t *)0x0829E10C, *(volatile int32_t *)0x08663108, RACE_MENU, RACE_PHASE, race_clock);
    for (int i = 0; i < 2; i++)
        if (!race_started[i] && race_sync_menu() && RACE_PHASE == i + 1) {
            race_started[i] = 1;
            fprintf(stderr, "compare: race anchor=%c field=%llu clock=%d\n", "cg"[i], field, race_clock);
        }
    /* DHD_RACE_FINISH_AT=seconds (tests): player 1's rider finishes at that race-clock time through
       the game's own finish-now flag (0x43EF22 + slot, as online.c's DHD_NET_TEST_FINISH), so a
       scripted race can end in any place, ahead of or behind the AI riders. */
    static HleEnv finish_env;
    const char *finish_at = hle_env(&finish_env, "DHD_RACE_FINISH_AT");
    if (finish_at && race_in(1) && race_clock > atof(finish_at) * 60 &&
        !*(volatile uint8_t *)0x0843EF18 && !*(volatile uint8_t *)0x0843EF22) {
        *(volatile uint8_t *)0x0843EF22 = 1;
        fprintf(stderr, "compare: finish now field=%llu clock=%d\n", field, race_clock);
    }
}

/* From the countdown the pad is neutral, then DHD_RACE_PAD (player 0; DHD_RACE_PAD<n> for
   player n, else DHD_RACE_PAD) keyed by race-clock seconds after GO. Returns 0 while the boot
   input still applies. */
int race_sync_pad(int player, uint16_t *buttons)
{
    static HleEnv all_env, own_env[4];
    const char *own = NULL;
    if (player > 0 && player < 4) {
        static const char *const names[4] = {"", "DHD_RACE_PAD1", "DHD_RACE_PAD2", "DHD_RACE_PAD3"};
        own = hle_env(&own_env[player], names[player]);
    } else if (player) {
        char name[32];
        snprintf(name, sizeof name, "DHD_RACE_PAD%d", player);
        own = getenv(name);
    }
    const char *script = own ? own : hle_env(&all_env, "DHD_RACE_PAD");
    race_pad_field = race_field;
    if (!race_started[0]) return 0;
    *buttons = 0;
    if (race_in(1) && script && *script && !autopad_read(script, race_clock / 60.0, buttons)) {
        fprintf(stderr, "Invalid DHD_RACE_PAD\n");
        exit(1);
    }
    return 1;
}

double race_sync_seconds(void) { return race_in(1) ? race_clock / 60.0 : -1; }

/* A race seeds its RNGs (start grid, AI) from the EE time since a timer reset (0016B8C8:
   func_00238F10), which follows load timing. DHD_RACE_SEED pins it, so the port starts the race
   PCSX2 compared against (the seed is stored at 0x0043FA10). */
int hle_race_seed(int seed)
{
    const char *pinned = host_getenv("DHD_RACE_SEED");
    int value = pinned && *pinned ? (int)strtol(pinned, NULL, 0) : seed;
    if (host_getenv("DHD_RACE_SYNC"))
        fprintf(stderr, "compare: race seed %d (timer %d)\n", value, seed);
    return value;
}

/* DHD_RACE_DUMP=dir: at each moment, EE RAM (32 MB) then the scratchpad (16 KB) to
   dir/ee-c+N.bin, as the patched PCSX2 does with DHD_COMPARE_RACE_DUMP, for comparing game
   state. Game data: keep the directory under build/. */
static void race_sync_dump(char anchor, long value)
{
    const char *dir = host_getenv("DHD_RACE_DUMP");
    char path[1024];
    if (!dir) return;
    snprintf(path, sizeof path, "%s/ee-%c%+ld.bin", dir, anchor, value);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    graphics_settle();
    fwrite((const void *)0x08000000, 32u << 20, 1, f);
    fwrite((const void *)0x70000000, 16u << 10, 1, f);
    fclose(f);
    fprintf(stderr, "compare: race dump %s clock=%d\n", path, race_clock);
}

/* Online tests: h+N moments count from the first hit this PC dealt or took (puppet.c). */
static int hit_clock = -1;
void race_sync_hit(void)
{
    if (hit_clock < 0) hit_clock = RACE_CLOCK;
}

/* DHD_RACE_CAPTURE lists c+N / g+N / h+N race-clock values. Each moment is captured on the vsync
   after its phase shows a clock of at least N (h: N after the first hit), and on the
   2 * DHD_RACE_CAPTURE_WINDOW after it. Names this field's capture, or returns 0. */
int race_sync_capture(const char *prefix, char *name, size_t size)
{
    static unsigned long long crossed[16];
    const char *spec = host_getenv("DHD_RACE_CAPTURE"), *window = host_getenv("DHD_RACE_CAPTURE_WINDOW");
    long w = window ? atol(window) : 0;
    for (int i = 0; spec && *spec && i < 16; i++) {
        char *end;
        int which = *spec == 'g' || *spec == 'h';
        long value = strtol(spec + 1, &end, 10);
        if (!crossed[i] && race_in(which) && race_clock >= value + (*spec == 'h' ? hit_clock : 0) &&
            (*spec != 'h' || hit_clock >= 0)) {
            char name[32];
            crossed[i] = race_field + 2;  /* from the next vsync, like PCSX2 */
            race_sync_dump(*spec, value);
            snprintf(name, sizeof name, "%c%+ld", *spec, value);
            host_service_gs_capture_request(GADDR(name));  /* DHD_RACE_GSDUMP, gs/capture.c */
        }
        if (crossed[i] && race_field + 1 - crossed[i] <= (unsigned long long)(2 * w)) {
            snprintf(name, size, "%s-%c%+ld+%llu.bmp", prefix, *spec, value, race_field + 1 - crossed[i]);
            fprintf(stderr, "compare: race moment=%c%+ld delta=%llu field=%llu clock=%d phase=%d\n", *spec, value,
                    race_field + 1 - crossed[i], race_field, race_clock, RACE_PHASE);
            return 1;
        }
        spec = *end ? end + 1 : end;
    }
    return 0;
}

/* Scripted input is game-side work. Device reads remain native. */
static int riding_controls(void) { return *(volatile int32_t *)GUEST(0x0829E290)!=0; }
static const uint16_t pressure_order[12]={PAD_RIGHT,PAD_LEFT,PAD_UP,PAD_DOWN,PAD_TRIANGLE,PAD_CIRCLE,
    PAD_CROSS,PAD_SQUARE,PAD_L1,PAD_R1,PAD_L2,PAD_R2};
/* The human rider's steering input (D_002AF2C0 + 0x50130): right +0x1190, left +0x1194. */
static void rider_steering(int *right, int *left)
{
    uint32_t rider = *(volatile uint32_t *)GUEST(0x082FF3F0);
    *right = *left = 0;
    if (rider >= 0x400000 && rider < 0x0A000000) *right = *(volatile int32_t *)GUEST(rider + 0x1190), *left = *(volatile int32_t *)GUEST(rider + 0x1194);
}

/* DHD_STICKS (tests, finding 134): the left stick from the race's GO on, "start:duration:lx=P[/ly=P]"
   with P the travel in percent (-100..100), through the dead zone a controller's stick gets
   (stick.h, DHD_STICK_DEAD_ZONE); each change is logged with the bytes and the steering. */
static void sticks_script(HostPad *out)
{
    static HleEnv sticks_env, zone_env;
    const char *script = hle_env(&sticks_env, "DHD_STICKS");
    double seconds = race_sync_seconds();
    if (!script || seconds < 0) return;
    int dead = stick_dead_zone_of(hle_env(&zone_env, "DHD_STICK_DEAD_ZONE"));
    int lx = 0, ly = 0;
    for (const char *at = script; *at;) {
        double start, duration;
        char axes[64];
        int used = 0;
        if (sscanf(at, "%lf:%lf:%63[a-z0-9=/-]%n", &start, &duration, axes, &used) != 3) {
            fprintf(stderr, "Invalid DHD_STICKS: expected start:duration:lx=P[/ly=P],...\n");
            exit(1);
        }
        if (seconds >= start && seconds < start + duration)
            for (char *axis = strtok(axes, "/"); axis; axis = strtok(NULL, "/")) {
                int value = atoi(axis + 3);
                value = value < -100 ? -100 : value > 100 ? 100 : value;
                if (!strncmp(axis, "lx=", 3)) lx = value;
                if (!strncmp(axis, "ly=", 3)) ly = value;
            }
        at += used;
        if (*at == ',') at++;
    }
    out->left_x = stick_byte(lx >= 0 ? lx * 32767 / 100 : lx * 32768 / 100, dead);
    out->left_y = stick_byte(ly >= 0 ? ly * 32767 / 100 : ly * 32768 / 100, dead);
    int right, left;
    rider_steering(&right, &left);
    static int last[5] = {-1};
    int now[5] = {lx, out->left_x, out->left_y, right, left};
    if (memcmp(now, last, sizeof now)) {
        hle_log("sticks race=%.3f dead=%d lx=%d%% byte=%d ly=%d steer right=%d left=%d\n", seconds, dead, lx,
                out->left_x, out->left_y, right, left);
        memcpy(last, now, sizeof now);
    }
}

/* DHD_KEYS (tests, finding 059): fake keys from the race's GO on, as DHD_RACE_PAD, through the
   keyboard's layout (dhd.ini keyboard, finding 157; the host's); each change is logged with the
   steering the rider got from it. */
static void keys_script(HostPad *out)
{
    static HleEnv keys_env;
    const char *script = hle_env(&keys_env, "DHD_KEYS");
    double seconds = race_sync_seconds();
    uint8_t down[256];
    if (!script || seconds < 0) return;
    if (!keyboard_script(script, seconds, down)) {
        fprintf(stderr, "Invalid DHD_KEYS: expected start:duration:key[+key],...\n");
        exit(1);
    }
    ControlsLayout layout;
    int own = host_service_controls_layout(CONTROL_KEYBOARD, GADDR(&layout));
    keyboard_pad(own ? &layout : NULL, down, riding_controls(), host_seconds(), out);
    int right, left;
    rider_steering(&right, &left);
    static int last[5] = {-1};
    int now[5] = {out->buttons, out->left_x, out->left_y, right, left};
    if (memcmp(now, last, sizeof now)) {
        hle_log("keys race=%.3f riding=%d buttons=%04X lx=%d ly=%d steer right=%d left=%d\n", seconds,
                riding_controls(), out->buttons, out->left_x, out->left_y, right, left);
        memcpy(last, now, sizeof now);
    }
}

/* DHD_TEST_PAD (player 1) and DHD_TEST_PAD2 (player 2) (tests, finding 157): a controller's
   inputs by their place from the race's GO on, "start:duration:input[+input],..." with the inputs
   by their dhd.ini names (south, east, lt, dpad_up, ...; lx=P, ly=P, rx=P, ry=P a stick's travel in
   percent, -100..100), through that player's layout (dhd.ini pad1, pad2) as a real controller's
   are (host_service_controls_map: gamepad_apply); each change is logged with what the game got. */
static void test_pad_script(int player, HostPad *out)
{
    static HleEnv envs[2];
    static int last[2][3] = {{-1}, {-1}};
    if (player < 0 || player > 1) return;
    const char *script = hle_env(&envs[player], player ? "DHD_TEST_PAD2" : "DHD_TEST_PAD");
    double seconds = race_sync_seconds();
    if (!script || seconds < 0) return;
    GamepadRaw raw;
    memset(&raw, 0, sizeof raw);
    for (const char *at = script; *at;) {
        double start, duration;
        char items[128];
        int used = 0, bad = sscanf(at, "%lf:%lf:%127[a-z0-9_=+-]%n", &start, &duration, items, &used) != 3 || duration <= 0;
        int active = !bad && seconds >= start && seconds < start + duration;
        for (char *item = bad ? NULL : strtok(items, "+"); item; item = strtok(NULL, "+")) {
            static const char *const axes[4] = {"lx=", "ly=", "rx=", "ry="};
            int known = 0;
            for (int a = 0; a < 4; a++)
                if (!strncmp(item, axes[a], 3)) {
                    int value = atoi(item + 3);
                    value = value < -100 ? -100 : value > 100 ? 100 : value;
                    if (active) raw.axis[a] = (int16_t)(value >= 0 ? value * 32767 / 100 : value * 32768 / 100);
                    known = 1;
                }
            for (int i = 0; i < CONTROL_PAD_INPUTS && !known; i++)
                if (!strcmp(control_pad_inputs[i].name, item)) {
                    known = 1;
                    if (!active) continue;
                    if (control_pad_inputs[i].trigger) raw.trigger[control_pad_inputs[i].trigger - 1] = 255;
                    else raw.buttons |= 1u << control_pad_inputs[i].id;
                }
            bad |= !known;
        }
        at += used;
        if (*at == ',') at++;
        else if (*at) bad = 1;
        if (bad) {
            fprintf(stderr, "Invalid DHD_TEST_PAD%s: expected start:duration:input[+input],... (south, east, lt, lx=P...)\n",
                    player ? "2" : "");
            exit(1);
        }
    }
    HostPad pad;
    memset(&pad, 0, sizeof pad);
    host_service_controls_map(player, GADDR(&raw), GADDR(&pad));
    out->buttons |= pad.buttons;
    out->left_x = pad.left_x, out->left_y = pad.left_y, out->right_x = pad.right_x, out->right_y = pad.right_y;
    int now[3] = {pad.buttons, pad.left_x, pad.right_y};
    if (memcmp(now, last[player], sizeof now)) {
        hle_log("test pad %d race=%.3f inputs=%08X buttons=%04X lx=%d ry=%d\n", player + 1, seconds, raw.buttons, pad.buttons,
                pad.left_x, pad.right_y);
        memcpy(last[player], now, sizeof now);
    }
}

/* DHD_NAV, DHD_AUTOPAD or DHD_AUTOPAD_FRAMES: every player gets the script's buttons (the same
   chord on every pad, so the menus accept any of them); from the race countdown DHD_RACE_PAD
   (player 0) or DHD_RACE_PAD<n> (player n, else DHD_RACE_PAD) drives each player. Returns 0
   when no script is set. */
int host_pad_script(int player, HostPad *out)
{
    size_t i;
    memset(out, 0, sizeof *out);
    out->right_x = out->right_y = out->left_x = out->left_y = 128;
    static HleEnv autopad_env, frames_env, nav_env;  /* online.c changes DHD_NAV (hle_env_changed) */
    const char *script = hle_env(&autopad_env, "DHD_AUTOPAD");
    const char *frame_script = hle_env(&frames_env, "DHD_AUTOPAD_FRAMES");
    const char *nav = hle_env(&nav_env, "DHD_NAV");  /* menus by the screen shown (autopad.c), else by time */
    if (frame_script) script = frame_script;
    if (nav) script = nav;
    if (!script)
        return 0;
    extern uint64_t kernel_vsync_count;
    double input_time = frame_script ? (double)(kernel_vsync_count ? kernel_vsync_count - 1 : 0) : host_seconds();
    if (nav ? !autopad_nav(nav, kernel_vsync_count, &out->buttons) :
              !autopad_read(script, input_time, &out->buttons)) {
        if (nav) fprintf(stderr, "Invalid DHD_NAV: expected menu[/phase]:button[+button],...\n");
        else fprintf(stderr, "Invalid %s: expected start:duration:button[+button],...\n",
                     frame_script ? "DHD_AUTOPAD_FRAMES" : "DHD_AUTOPAD");
        exit(1);
    }
    race_sync_vsync(kernel_vsync_count);
    race_sync_pad(player, &out->buttons);
    test_pad_script(player, out);
    if (player == 0) keys_script(out), sticks_script(out);
    static uint16_t previous;
    if (player == 0 && previous != out->buttons) {
        if (frame_script || nav)
            hle_log("autopad field=%llu game=%.3f wall=%.3f: %04X\n",
                    (unsigned long long)kernel_vsync_count, host_seconds(), host_wall_seconds(), out->buttons);
        else hle_log("autopad %.3f: %04X\n", host_seconds(), out->buttons);
        previous = out->buttons;
    }
    for (i = 0; i < 12; i++) out->pressure[i] = out->buttons & pressure_order[i] ? 255 : 0;
    return 1;
}


void host_pad_read(int port, HostPad *out)
{
    if (!host_pad_script(port, out))
        host_service_host_pad_device(port, GADDR(out));
}
