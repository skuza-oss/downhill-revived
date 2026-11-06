/* touch.c: the touch controls' rules (finding 141): scaling, the flick lanes, the sprint and
   resume guards, the trick stick and the pause rows' ↓ + ✕ sequences. Built and run by
   port/tools/test_runtime.py (a host program). */
#include "touch.h"
#include "hle.h"
#include "../include/fps.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static Touch *t;
static TouchGame game;
static double now;
static int failures;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "test_touch:%d: %s\n", __LINE__, #cond); failures++; } } while (0)

static void start(int preset, int attacks)
{
    if (t) touch_free(t);
    t = touch_new();
    TouchSettings s;
    touch_settings_default(&s);
    s.preset = preset;
    s.attacks = attacks;
    s.vibration = 0;
    s.hints_shown = ~0u;   /* no hints in these tests */
    TouchScreen screen = {154.5f, 69.5f, 0, 0, 0, 0, 0, 0, 0, 0};
    touch_configure(t, &s, &screen);
    memset(&game, 0, sizeof game);
    game.race_hud = 1;
    game.clock_moving = 1;
    game.fps = 60;
    game.race_ms = 10000;
    game.pause_row[0] = 0, game.pause_row[1] = 4, game.pause_row[2] = 6;
    now = 100000;
    touch_update(t, &game, now);
}

/* One field: the clocks advance (the race clock only while it runs). */
static void field(void)
{
    now += 1000.0 / 60;
    game.vsync++;
    if (game.clock_moving) game.race_ms += 1000.0 / 60;
    touch_update(t, &game, now);
}
static void fields(int n) { while (n-- > 0) field(); }
static void wait_ms(double ms) { double end = now + ms; while (now + 0.001 < end) field(); }

static uint16_t buttons(void)
{
    uint16_t b = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    touch_pad(t, &b, &lx, &ly, &rx, &ry);
    return b;
}

static void at(int control, float *x, float *y)
{
    float r;
    assert(touch_control(t, control, x, y, &r));
}

static void tap(long long id, int control)
{
    float x, y;
    at(control, &x, &y);
    touch_down(t, id, x, y, now);
    field();
    touch_up(t, id, x, y, now);
    field();
}

/* A finger moved from (x0,y0) to (x1,y1) in `ms`, one sample a field. */
static void slide(long long id, float x0, float y0, float x1, float y1, double ms)
{
    int n = (int)(ms / (1000.0 / 60));
    if (n < 1) n = 1;
    for (int i = 1; i <= n; i++) {
        now += ms / n;
        touch_move(t, id, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, now);
        game.vsync++;
        if (game.clock_moving) game.race_ms += ms / n;
        touch_update(t, &game, now);
    }
}

static void test_scaling(void)
{
    CHECK(fabsf(touch_height_scale(69.5f, TOUCH_THUMB_ZONES) - 1.0278f) < 0.001f);   /* 103% on the Mi 11i */
    CHECK(fabsf(touch_height_scale(62.0f, TOUCH_THUMB_ZONES) - 0.8889f) < 0.001f);   /* 89% on a 62 mm screen */
    CHECK(fabsf(touch_button_scale(0.8889f) - 0.8667f) < 0.001f);                    /* buttons 87% */
    CHECK(fabsf(touch_button_scale(0.70f) - 0.64f) < 0.001f);                        /* Size 70%: buttons 64% */
    CHECK(touch_height_scale(62.0f, TOUCH_CLASSIC) >= 1.0f);                         /* Classic needs only 62 mm */
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    float x, y, r;
    touch_control(t, TC_PEDAL, &x, &y, &r);
    CHECK(fabsf(x - 131.3f) < 0.2f && fabsf(y - 49.3f) < 0.2f && fabsf(r - 11.6f) < 0.2f);   /* PEDAL 23 mm */
    touch_control(t, TC_HOP, &x, &y, &r);
    CHECK(fabsf((69.5f - y + r) - 54.0f) < 0.6f);   /* HOP's top 54 mm up */
    /* a short screen: everything scales about the bottom-right corner, gaps stay >= 3 mm */
    TouchSettings s = *touch_settings(t);
    TouchScreen small = {120.0f, 62.0f, 0, 0, 0, 0, 0, 0, 0, 0};
    touch_configure(t, &s, &small);
    float px, py, pr, hx, hy, hr, bx, by, br;
    touch_control(t, TC_PEDAL, &px, &py, &pr);
    touch_control(t, TC_HOP, &hx, &hy, &hr);
    touch_control(t, TC_BRAKE, &bx, &by, &br);
    CHECK(fabsf(pr * 2 - 23.175f * 0.8667f) < 0.2f);
    CHECK(hypotf(px - hx, py - hy) - pr - hr >= 3.0f);
    CHECK(hypotf(px - bx, py - by) - pr - br >= 3.0f);
    CHECK(hy - hr >= 0);
}

static void test_flick(void)
{
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    float x, y, r;
    touch_control(t, TC_PEDAL, &x, &y, &r);
    touch_down(t, 1, x, y, now);
    field();
    CHECK(buttons() & PAD_CROSS);
    /* a fast flick left, inside the lane: ◻ for 3 fields while ✕ stays held */
    slide(1, x, y, x - r - 10, y + 2, 60);
    CHECK((buttons() & (PAD_SQUARE | PAD_CROSS)) == (PAD_SQUARE | PAD_CROSS));
    fields(4);
    CHECK(!(buttons() & PAD_SQUARE) && (buttons() & PAD_CROSS));
    /* no second attack until the thumb is back inside the rim */
    slide(1, x - r - 10, y + 2, x - r - 20, y, 60);
    CHECK(!(buttons() & PAD_SQUARE));
    slide(1, x - r - 20, y, x, y, 100);
    slide(1, x, y, x + r + 10, y, 50);
    CHECK(buttons() & PAD_CIRCLE);   /* right lane: ○ */
    slide(1, x + r + 10, y, x, y, 100);
    fields(4);
    /* slow: no attack; outside the ±30° lane: none */
    slide(1, x, y, x - r - 8, y, 600);
    CHECK(!(buttons() & PAD_SQUARE));
    slide(1, x - r - 8, y, x, y, 100);
    slide(1, x, y, x - (r + 8) * 0.7f, y - (r + 8) * 0.7f, 40);   /* 45° up: no lane */
    CHECK(!(buttons() & (PAD_SQUARE | PAD_CIRCLE)));
    touch_up(t, 1, x, y, now);
    field();
    /* Buttons mode: no flicks */
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_BUTTONS);
    touch_control(t, TC_PEDAL, &x, &y, &r);
    touch_down(t, 2, x, y, now);
    field();
    slide(2, x, y, x - r - 10, y, 50);
    CHECK(!(buttons() & PAD_SQUARE));
    touch_up(t, 2, x, y, now);
}

/* A tap shorter than a field (down and up between two of the game's reads) still reaches the pad. */
static void test_quick_tap(void)
{
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    float x, y;
    at(TC_HOP, &x, &y);
    touch_down(t, 1, x, y, now);
    touch_up(t, 1, x, y, now + 5);
    CHECK(buttons() & PAD_R2);
    int held = 0;
    for (int i = 0; i < 10; i++) { field(); held += (buttons() & PAD_R2) != 0; }
    CHECK(held == 3);   /* in the next three pad reads (PRESS_FIELDS), then up */
    /* a held tap is not lengthened */
    touch_down(t, 2, x, y, now);
    field();
    CHECK(buttons() & PAD_R2);
    touch_up(t, 2, x, y, now);
    CHECK(!(buttons() & PAD_R2));
}

/* The fields (from 0) at which `button` went down in the next n fields' pad reads; their count. */
static int presses(uint16_t button, int n, int *at_field, int max)
{
    int count = 0, was = 0;
    for (int i = 0; i < n; i++) {
        field();
        int down = (buttons() & button) != 0;
        if (down && !was && count < max) at_field[count++] = i;
        else if (down && !was) count++;
        was = down;
    }
    return count;
}

static void drain(TouchQueue *q);

/* A stall of the game thread: ms pass, the VBLANKs go on, no update (no pad read) in between. */
static void stall(double ms)
{
    now += ms;
    game.vsync += (uint32_t)(ms / (1000.0 / 60));
    game.race_ms += ms;
}

/* Taps the game never read before their lift (down and up drained before the next pad read: a
   quick tap, a stall) still reach the game: PEDAL, Classic's ✕, the camera's SELECT (Sol's round-2
   #2), and a double tap stays two presses with a release between them. */
static void test_unread_taps(void)
{
    static TouchQueue q;
    int at_field[4];
    float x, y;
    for (int classic = 0; classic < 2; classic++) {
        int cross = classic ? TC_CROSS : TC_PEDAL;
        /* one quick tap: ✕ for PRESS_FIELDS, then up */
        start(classic ? TOUCH_CLASSIC : TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
        at(cross, &x, &y);
        touch_down(t, 1, x, y, now);
        touch_up(t, 1, x, y, now + 5);
        CHECK(presses(PAD_CROSS, 10, at_field, 4) == 1 && at_field[0] == 0);
        CHECK(!(buttons() & PAD_CROSS));
        /* the camera */
        fields(40);
        at(TC_CAMERA, &x, &y);
        touch_down(t, 2, x, y, now);
        touch_up(t, 2, x, y, now + 5);
        CHECK(presses(PAD_SELECT, 10, at_field, 4) == 1 && at_field[0] == 0);
        /* a tap queued during a 100 ms stall (70 ms long), as the host drains it */
        fields(40);
        at(cross, &x, &y);
        memset(&q, 0, sizeof q);
        touch_queue_push(&q, TOUCH_EV_DOWN, 3, x, y, now + 10);
        touch_queue_push(&q, TOUCH_EV_UP, 3, x, y, now + 80);
        stall(100);
        drain(&q);
        CHECK(presses(PAD_CROSS, 10, at_field, 4) == 1 && at_field[0] == 0);
        /* a double tap inside one stall: two presses, a release between, inside the game's
           double-tap window (a sprint) */
        fields(40);
        memset(&q, 0, sizeof q);
        touch_queue_push(&q, TOUCH_EV_DOWN, 4, x, y, now + 10);
        touch_queue_push(&q, TOUCH_EV_UP, 4, x, y, now + 60);
        touch_queue_push(&q, TOUCH_EV_DOWN, 4, x, y, now + 110);   /* the same finger id again */
        touch_queue_push(&q, TOUCH_EV_UP, 4, x, y, now + 160);
        stall(200);
        drain(&q);
        CHECK(presses(PAD_CROSS, 40, at_field, 4) == 2 && at_field[1] - at_field[0] >= 2 && at_field[1] - at_field[0] < 30);
        CHECK(!(buttons() & PAD_CROSS));
        /* the camera twice in one stall: two SELECTs, the second after the game's bail window */
        fields(40);
        at(TC_CAMERA, &x, &y);
        memset(&q, 0, sizeof q);
        touch_queue_push(&q, TOUCH_EV_DOWN, 5, x, y, now + 10);
        touch_queue_push(&q, TOUCH_EV_UP, 5, x, y, now + 60);
        touch_queue_push(&q, TOUCH_EV_DOWN, 6, x, y, now + 110);
        touch_queue_push(&q, TOUCH_EV_UP, 6, x, y, now + 160);
        stall(200);
        drain(&q);
        CHECK(presses(PAD_SELECT, 60, at_field, 4) == 2 && at_field[1] - at_field[0] >= 30);
        /* a first tap lost in a stall no longer spoils the double tap that follows it */
        fields(40);
        at(cross, &x, &y);
        memset(&q, 0, sizeof q);
        touch_queue_push(&q, TOUCH_EV_DOWN, 7, x, y, now + 10);
        touch_queue_push(&q, TOUCH_EV_UP, 7, x, y, now + 60);
        stall(100);
        drain(&q);
        CHECK(presses(PAD_CROSS, 5, at_field, 4) == 1);   /* the first tap went */
        touch_down(t, 8, x, y, now);   /* held: the second tap, inside the window it opened */
        CHECK(presses(PAD_CROSS, 20, at_field, 4) == 1);
        CHECK(classic ? at_field[0] == 0 : at_field[0] >= 6);   /* Thumb zones holds a real second tap back (120 ms) */
        touch_up(t, 8, x, y, now);
        field();
    }
    /* Thumb zones: a second tap held back to see whether it flicks, lifted before its 120 ms
       without a flick: a real second tap, it goes (a sprint); one that flicked sends no ✕ */
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    tap(1, TC_PEDAL);
    at(TC_PEDAL, &x, &y);
    touch_down(t, 2, x, y, now);
    fields(4);
    CHECK(!(buttons() & PAD_CROSS));
    touch_up(t, 2, x, y, now);
    CHECK(presses(PAD_CROSS, 10, at_field, 4) == 1 && at_field[0] == 0);
    fields(40);
    float r;
    tap(3, TC_PEDAL);
    touch_control(t, TC_PEDAL, &x, &y, &r);
    touch_down(t, 4, x, y, now);
    slide(4, x, y, x - r - 10, y, 50);   /* the flick: ◻ */
    touch_up(t, 4, x - r - 10, y, now);
    CHECK(presses(PAD_CROSS, 10, at_field, 4) == 0);
}

/* A HOP tap that begins and ends in the air layout's wait (the flag came, the layout is still the
   ground's) is a short HOP press; one held into the air layout becomes the trick stick. */
static void test_air_wait_tap(void)
{
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    float x, y;
    at(TC_HOP, &x, &y);
    game.air = 1;
    field();
    touch_down(t, 1, x, y, now);
    fields(4);
    CHECK(!(buttons() & PAD_R2));   /* nothing while it waits */
    touch_up(t, 1, x, y, now);
    CHECK(buttons() & PAD_R2);
    int held = 0;
    for (int i = 0; i < 6; i++) { field(); held += (buttons() & PAD_R2) != 0; }
    CHECK(held == 3);   /* three pad reads */
    game.air = 0;
    fields(2);
    game.air = 1;
    field();
    touch_down(t, 2, x, y, now);
    wait_ms(200);                   /* the layout came: the trick stick, which sends nothing unmoved */
    touch_up(t, 2, x, y, now);
    CHECK(!(buttons() & PAD_R2));
    game.air = 0;
    fields(2);
}

static void test_sprint_guards(void)
{
    float x, y, r;
    /* a real second tap is held back up to 120 ms, then sent inside the window: a sprint */
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    tap(1, TC_PEDAL);
    at(TC_PEDAL, &x, &y);
    touch_down(t, 2, x, y, now);
    field();
    CHECK(!(buttons() & PAD_CROSS));
    wait_ms(125);
    CHECK(buttons() & PAD_CROSS);
    touch_up(t, 2, x, y, now);
    fields(40);
    /* the held-back tap that flicks: no ✕ until it lifts */
    tap(3, TC_PEDAL);
    touch_control(t, TC_PEDAL, &x, &y, &r);
    touch_down(t, 4, x, y, now);
    slide(4, x, y, x - r - 10, y, 50);
    CHECK((buttons() & PAD_SQUARE) && !(buttons() & PAD_CROSS));
    slide(4, x - r - 10, y, x, y, 200);
    CHECK(!(buttons() & PAD_CROSS));
    touch_up(t, 4, x, y, now);
    fields(40);
    /* after HOP was opened and released since ✕ went up, a PEDAL touch inside the window waits
       for the window to close */
    tap(5, TC_PEDAL);
    tap(6, TC_HOP);
    at(TC_PEDAL, &x, &y);
    touch_down(t, 7, x, y, now);
    wait_ms(200);
    CHECK(!(buttons() & PAD_CROSS));
    wait_ms(400);
    CHECK(buttons() & PAD_CROSS);
    touch_up(t, 7, x, y, now);
    fields(40);
    /* BRAKE doesn't count: ✕, △, ✕ still sprints (after the hold-back) */
    tap(8, TC_PEDAL);
    tap(9, TC_BRAKE);
    touch_down(t, 10, x, y, now);
    wait_ms(130);
    CHECK(buttons() & PAD_CROSS);
    touch_up(t, 10, x, y, now);
    fields(40);
    /* claw grip: HOP held by another finger from before ✕ went up doesn't count */
    float hx, hy;
    at(TC_HOP, &hx, &hy);
    touch_down(t, 11, x, y, now);
    field();
    touch_down(t, 12, hx, hy, now);
    field();
    touch_up(t, 11, x, y, now);
    field();
    touch_up(t, 12, hx, hy, now);
    field();
    touch_down(t, 13, x, y, now);
    wait_ms(130);
    CHECK(buttons() & PAD_CROSS);
    touch_up(t, 13, x, y, now);
    fields(40);
    /* after a flick that lifts, ✕ stays held until the window closes, and the next PEDAL touch
       continues the same press (no release, so no sprint) */
    touch_control(t, TC_PEDAL, &x, &y, &r);
    touch_down(t, 14, x, y, now);
    field();
    slide(14, x, y, x - r - 10, y, 50);
    touch_up(t, 14, x - r - 10, y, now);
    field();
    CHECK(buttons() & PAD_CROSS);
    wait_ms(200);
    touch_down(t, 15, x, y, now);
    field();
    CHECK(buttons() & PAD_CROSS);
    touch_up(t, 15, x, y, now);
    field();
    CHECK(!(buttons() & PAD_CROSS));
    /* a flick-lift with no PEDAL touch: the overlay lets ✕ up after the window; a PEDAL touch in
       the next window then waits (an overlay-made release never sprints) */
    fields(40);
    touch_down(t, 16, x, y, now);
    field();
    slide(16, x, y, x - r - 10, y, 50);
    touch_up(t, 16, x - r - 10, y, now);
    wait_ms(520);
    CHECK(!(buttons() & PAD_CROSS));
    touch_down(t, 17, x, y, now);
    wait_ms(200);
    CHECK(!(buttons() & PAD_CROSS));
    wait_ms(400);
    CHECK(buttons() & PAD_CROSS);
    touch_up(t, 17, x, y, now);
    fields(40);
    /* windows count race time: the clock stopped (countdown), the window doesn't run out */
    tap(19, TC_PEDAL);
    tap(18, TC_HOP);
    game.clock_moving = 0;
    touch_down(t, 20, x, y, now);
    wait_ms(1000);
    CHECK(!(buttons() & PAD_CROSS));
    game.clock_moving = 1;
    wait_ms(520);
    CHECK(buttons() & PAD_CROSS);
    touch_up(t, 20, x, y, now);
}

/* The vsync-timed presses the pause rows send: the presses as a list of button edges. */
static int record(uint16_t *edges, int max, int nfields)
{
    int n = 0;
    uint16_t before = buttons();
    for (int i = 0; i < nfields; i++) {
        field();
        uint16_t b = buttons();
        uint16_t pressed = (uint16_t)(b & ~before);
        if (pressed && n < max) edges[n++] = pressed;
        before = b;
    }
    return n;
}

static void test_pause(void)
{
    uint16_t edges[32];
    float x, y;
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    /* PEDAL held through the pause */
    at(TC_PEDAL, &x, &y);
    touch_down(t, 1, x, y, now);
    field();
    CHECK(buttons() & PAD_CROSS);
    touch_back(t, now);   /* Android Back pauses: START */
    int n = record(edges, 32, 8);
    CHECK(n == 1 && edges[0] == PAD_START);
    game.clock_moving = 0;   /* the menu is open: the race clock stops */
    fields(3);
    CHECK(!(buttons() & PAD_CROSS));   /* race touches send nothing */
    /* Restart race (row 4 in the game's menu): four ↓, then ✕ */
    touch_down(t, 2, 154.5f / 2, 0, now);   /* outside the rows: nothing */
    touch_up(t, 2, 154.5f / 2, 0, now);
    n = record(edges, 32, 4);
    CHECK(n == 0);
    /* Restart race: the game's row 4, so four ↓ then ✕; the question opens */
    float rx, ry;
    CHECK(touch_target(t, TOUCH_TARGET_PAUSE, 1, &rx, &ry));
    touch_down(t, 3, rx, ry, now);
    touch_up(t, 3, rx, ry, now);
    n = record(edges, 32, 60);
    CHECK(n == 5 && edges[0] == PAD_DOWN && edges[3] == PAD_DOWN && edges[4] == PAD_CROSS);
    /* the question: Yes is ↓ then ✕ */
    touch_back(t, now);   /* Back on the question: △ (No), back to the rows */
    n = record(edges, 32, 20);
    CHECK(n == 1 && edges[0] == PAD_TRIANGLE);
    /* Resume: the question's No put the game's cursor back on RESUME, so ✕ alone */
    touch_back(t, now);   /* Back on the rows: Resume */
    n = record(edges, 32, 60);
    CHECK(n == 1 && edges[0] == PAD_CROSS);
    game.clock_moving = 1;
    /* resume guard: PEDAL still down waits for the double-tap window */
    field();
    CHECK(!(buttons() & PAD_CROSS));
    wait_ms(520);
    CHECK(buttons() & PAD_CROSS);
    touch_up(t, 1, x, y, now);
    field();
    /* START ignored (the clock keeps moving): race touches work again after 0.5 s */
    touch_back(t, now);
    record(edges, 32, 10);
    wait_ms(600);
    at(TC_HOP, &x, &y);
    touch_down(t, 4, x, y, now);
    field();
    CHECK(buttons() & PAD_R2);
    touch_up(t, 4, x, y, now);
    field();
    /* Back while the presses opening the question still go: ignored, the overlay stays on the
       question (the game has not seen it yet), and the next Back answers No */
    touch_back(t, now);
    record(edges, 32, 8);
    game.clock_moving = 0;
    fields(3);
    touch_down(t, 5, rx, ry, now);   /* Restart race */
    touch_up(t, 5, rx, ry, now);
    field();
    touch_back(t, now);   /* within the ↓ ↓ ↓ ↓ ✕ (the first ↓ went in that field) */
    n = record(edges, 32, 60);
    CHECK(n == 4 && edges[0] == PAD_DOWN && edges[3] == PAD_CROSS);   /* no △ */
    touch_back(t, now);
    n = record(edges, 32, 20);
    CHECK(n == 1 && edges[0] == PAD_TRIANGLE);
}

/* The overlay's pause rows opened Restart race's question; a controller answers it (Sol's round-2
   #1). The overlay hands the menu over at once (hidden, nothing sent, touches not taken as rows),
   and gives the race controls back when the race goes on: after Yes when the restarted race's
   clock runs, after No when the controller resumes. */
static int pedal_works(void)
{
    float x, y;
    touch_down(t, 90, 10, 60, now);   /* the touch that brings the overlay back (the controller hid it) */
    touch_up(t, 90, 10, 60, now);
    field();
    at(TC_PEDAL, &x, &y);
    touch_down(t, 91, x, y, now);
    wait_ms(520);   /* the resume guard */
    int works = touch_visible(t) && (buttons() & PAD_CROSS);
    touch_up(t, 91, x, y, now);
    field();
    return works;
}

static void test_pause_controller(void)
{
    uint16_t edges[32];
    TouchShape shapes[TOUCH_SHAPES];
    for (int yes = 0; yes < 2; yes++) {
        start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
        touch_back(t, now);   /* pause */
        record(edges, 32, 8);
        game.clock_moving = 0;
        fields(3);
        float rx, ry;
        touch_target(t, TOUCH_TARGET_PAUSE, 1, &rx, &ry);   /* Restart race: ↓ ↓ ↓ ↓ ✕, the question */
        touch_down(t, 1, rx, ry, now);
        touch_up(t, 1, rx, ry, now);
        record(edges, 32, 60);
        CHECK(touch_visible(t));
        touch_pad_used(t, now);   /* the controller answers */
        field();
        CHECK(!touch_visible(t) && touch_shapes(t, shapes, TOUCH_SHAPES) == 0);
        float cx, cy;
        touch_target(t, TOUCH_TARGET_CONFIRM, 1, &cx, &cy);
        touch_down(t, 2, cx, cy, now);   /* a tap where Yes was: no row, no press */
        touch_up(t, 2, cx, cy, now);
        CHECK(record(edges, 32, 20) == 0);
        if (yes) {   /* Yes: the race restarts (the countdown, the clock back to 0), then runs */
            game.race_ms = 0;
            fields(30);
            game.clock_moving = 1;
            field();
        } else {     /* No: the game's menu again, the clock still stopped; Back is the menu's △ */
            fields(30);
            CHECK(!touch_visible(t) && !buttons());
            touch_back(t, now);
            CHECK(record(edges, 32, 20) == 1 && edges[0] == PAD_TRIANGLE);
            game.clock_moving = 1;   /* the controller resumes */
            field();
        }
        CHECK(pedal_works());
    }
    /* a controller used while the rows' presses still go: they stop (the controller's now) */
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    touch_back(t, now);
    record(edges, 32, 8);
    game.clock_moving = 0;
    fields(3);
    float rx, ry;
    touch_target(t, TOUCH_TARGET_PAUSE, 1, &rx, &ry);
    touch_down(t, 1, rx, ry, now);
    touch_up(t, 1, rx, ry, now);
    CHECK(record(edges, 32, 4) == 1 && edges[0] == PAD_DOWN);
    touch_pad_used(t, now);
    CHECK(record(edges, 32, 60) == 0);
    game.clock_moving = 1;
    field();
    CHECK(pedal_works());
}

/* Restart race: the resume guard set by the old race's clock does not hold PEDAL back in the new
   race (the clock went back to 0). */
static void test_restart_clock(void)
{
    float x, y;
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    game.race_ms = 60000;
    field();
    touch_back(t, now);   /* pause */
    record(NULL, 0, 8);
    game.clock_moving = 0;
    fields(3);
    touch_back(t, now);   /* Resume: its ✕ counts as a release the overlay made, at 60 s */
    record(NULL, 0, 20);
    game.clock_moving = 1;
    fields(2);
    game.race_ms = 0, game.clock_moving = 0;   /* the race restarts: the countdown */
    fields(30);
    game.clock_moving = 1;
    wait_ms(2000);
    at(TC_PEDAL, &x, &y);
    touch_down(t, 1, x, y, now);
    field();
    CHECK(buttons() & PAD_CROSS);
    touch_up(t, 1, x, y, now);
    field();
}

/* Issue #54 (finding 159): a career continued from a save races with the shell's screen id still
   22 (CONTINUE SAVED GAME), not 9 (GET READY), so the race test (port_race_rule, fps.h) does not read
   the screen: a race's loop ran since the front end's last frame, not the attract demo (mode 10),
   the race phase from first_phase (1 the countdown for the overlay) to 4. The continued career's
   own modes (0-5, func_001EE2A0) count. After a race the phase stays (3 on the title after QUIT),
   so the front end's frame is what ends it. The overlay then shows as in any race. */
static void test_race_running(void)
{
    for (int mode = 0; mode <= 5; mode++) {
        CHECK(port_race_rule(1, mode, 1, 1));
        CHECK(port_race_rule(1, mode, 3, 1));
        CHECK(port_race_rule(1, mode, 4, 1));
        CHECK(!port_race_rule(1, mode, 1, 2));   /* the countdown, for a test from GO */
        CHECK(!port_race_rule(1, mode, 5, 1));   /* the load and the flight over the track */
        CHECK(!port_race_rule(0, mode, 3, 1));   /* the menus after a race: the phase left at 3 */
    }
    CHECK(!port_race_rule(1, PORT_MODE_ATTRACT, 3, 1));
    start(TOUCH_CLASSIC, TOUCH_ATTACK_FLICK);
    game.race_hud = 0;
    wait_ms(500);
    CHECK(!touch_visible(t));
    game.race_hud = port_race_rule(1, 3, 2, 1);   /* touch_field: the HUD hook in a continued career's race */
    field();
    CHECK(touch_visible(t));
}

/* The trick-stick hint: it holds the game until a push, and when the hold ends without one (5 s)
   the hint goes too. */
static void test_trick_hint(void)
{
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    TouchSettings set = *touch_settings(t);
    set.hints_shown = ~0u & ~(unsigned)TOUCH_HINT_TRICK;
    touch_configure(t, &set, NULL);
    /* a hop just over 400 ms: the rider lands before the hold begins; the hint waits for later */
    game.air = 1;
    wait_ms(435);   /* the hint came at 400 ms; its hold would begin 3 fields later */
    game.air = 0;
    fields(4);
    CHECK(!touch_freeze(t));
    CHECK(!(touch_settings(t)->hints_shown & TOUCH_HINT_TRICK));
    fields(10);
    game.air = 1;
    wait_ms(250);   /* the air layout, before the hint (400 ms) */
    TouchShape shapes[TOUCH_SHAPES];
    int no_hint = touch_shapes(t, shapes, TOUCH_SHAPES);
    wait_ms(350);
    fields(4);
    CHECK(touch_freeze(t));
    CHECK(touch_shapes(t, shapes, TOUCH_SHAPES) > no_hint);
    /* the game is held: its clocks stand still */
    game.clock_moving = 0;
    double held = now;
    while (touch_freeze(t) && now - held < 6000) { now += 1000.0 / 60; touch_update(t, &game, now); }
    CHECK(!touch_freeze(t));
    game.clock_moving = 1;
    field();
    CHECK(touch_shapes(t, shapes, TOUCH_SHAPES) == no_hint);   /* its box, text and ring went */
    game.air = 0;
    fields(2);
}

/* The layout editor's buttons sit beside the camera and Pause buttons, never on them (both
   presets, both hands, the Mi 11i's screen), and DONE closes the editor. */
static void test_editor_bar(void)
{
    for (int preset = 0; preset < 2; preset++)
        for (int left = 0; left < 2; left++) {
            start(preset ? TOUCH_CLASSIC : TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
            TouchSettings set = *touch_settings(t);
            set.left_handed = left;
            touch_configure(t, &set, NULL);
            touch_open_editor(t);
            field();
            for (int c = TC_CAMERA; c <= TC_PAUSE; c++) {
                float cx, cy, cr;
                CHECK(touch_control(t, c, &cx, &cy, &cr));
                static const float half_w[6] = {9, 7, 5, 5, 8, 8};   /* GROUND AIR - + RESET DONE */
                for (int b = 0; b < 6; b++) {
                    float bx, by;
                    CHECK(touch_target(t, TOUCH_TARGET_EDITOR, b, &bx, &by));
                    CHECK(fabsf(bx - cx) >= half_w[b] + cr || fabsf(by - cy) >= 4.5f + cr);   /* no overlap */
                }
            }
            /* a drop in open space stays (Thumb zones: BRAKE 6 mm up, clear of the flick lanes;
               Classic: L3 10 mm inward); onto PEDAL / ✕ it snaps back */
            int moved = preset ? TC_L3 : TC_BRAKE;
            float bx, by, br, nx, ny;
            touch_control(t, moved, &bx, &by, &br);
            float mx = preset ? (left ? -10.0f : 10.0f) : 0, my = preset ? 0 : -6.0f;
            touch_down(t, 8, bx, by, now);
            slide(8, bx, by, bx + mx, by + my, 100);
            touch_up(t, 8, bx + mx, by + my, now);
            field();
            touch_control(t, moved, &nx, &ny, &br);
            CHECK(fabsf(nx - (bx + mx)) < 0.2f && fabsf(ny - (by + my)) < 0.2f);
            float px, py, pr;
            touch_control(t, preset ? TC_CROSS : TC_PEDAL, &px, &py, &pr);
            touch_down(t, 8, nx, ny, now);
            slide(8, nx, ny, px, py, 100);
            touch_up(t, 8, px, py, now);
            field();
            touch_control(t, moved, &bx, &by, &br);
            CHECK(fabsf(bx - nx) < 0.2f && fabsf(by - ny) < 0.2f);
            float dx, dy;
            touch_target(t, TOUCH_TARGET_EDITOR, 5, &dx, &dy);   /* DONE */
            touch_down(t, 9, dx, dy, now);
            touch_up(t, 9, dx, dy, now);
            field();
            TouchSettings saved;
            CHECK(touch_take_saved(t, &saved));   /* DONE closed the editor and saved the layout */
        }
}

static void test_classic_resume(void)
{
    uint16_t edges[16];
    float x, y;
    start(TOUCH_CLASSIC, TOUCH_ATTACK_FLICK);
    /* Classic sends ✕ as it is: a double tap is a double tap */
    tap(1, TC_CROSS);
    at(TC_CROSS, &x, &y);
    touch_down(t, 2, x, y, now);
    field();
    CHECK(buttons() & PAD_CROSS);
    /* ✕ slides onto the + spot: ◻ added, ✕ stays */
    float px, py;
    at(TC_PLUS_LEFT, &px, &py);
    touch_move(t, 2, px, py, now);
    field();
    CHECK((buttons() & (PAD_CROSS | PAD_SQUARE)) == (PAD_CROSS | PAD_SQUARE));
    touch_move(t, 2, x, y, now);
    field();
    CHECK((buttons() & (PAD_CROSS | PAD_SQUARE)) == PAD_CROSS);
    /* the resume guard applies to Classic's ✕ too */
    touch_back(t, now);
    record(edges, 16, 8);
    game.clock_moving = 0;
    fields(3);
    touch_back(t, now);   /* Resume */
    record(edges, 16, 12);
    game.clock_moving = 1;
    field();
    CHECK(!(buttons() & PAD_CROSS));
    wait_ms(520);
    CHECK(buttons() & PAD_CROSS);
    touch_up(t, 2, x, y, now);
    /* the shoulder bar's middle presses both */
    float bx, by, br;
    touch_control(t, TC_LBAR, &bx, &by, &br);
    touch_down(t, 3, bx, by, now);
    field();
    CHECK((buttons() & (PAD_L1 | PAD_L2)) == (PAD_L1 | PAD_L2));
    touch_up(t, 3, bx, by, now);
}

/* The air: the stick opens at a touch in the right half once the air layout is up. */
static void take_off(void)
{
    game.air = 1;
    wait_ms(160);   /* the air layout: 150 ms after the flag */
}

static uint16_t shoulders(void) { return buttons() & (PAD_L1 | PAD_L2 | PAD_R1 | PAD_R2 | PAD_TRIANGLE); }

static void test_trick_stick(void)
{
    const float cx = 100, cy = 35;
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    /* push →, hold: Can Can (R1+R2) once the thumb settles for 70 ms */
    take_off();
    touch_down(t, 1, cx, cy, now);
    slide(1, cx, cy, cx + 10, cy, 50);
    CHECK(shoulders() == 0);   /* moving: nothing sent */
    wait_ms(90);
    CHECK(shoulders() == (PAD_R1 | PAD_R2));
    /* past the edge: △ after the gate (60 ms after the latch) + 33 ms */
    slide(1, cx + 10, cy, cx + 16, cy, 20);
    wait_ms(60);
    CHECK(shoulders() == (PAD_R1 | PAD_R2 | PAD_TRIANGLE));
    touch_up(t, 1, cx + 16, cy, now);
    field();
    CHECK(shoulders() == 0);
    game.air = 0;
    fields(2);

    /* roll through the centre: ↖, back through the middle (missing it), ↘ in one movement: L1 + R2 */
    take_off();
    touch_down(t, 2, cx, cy, now);
    slide(2, cx, cy, cx - 7.5f, cy - 7.5f, 50);      /* ↖ at 10.6 mm */
    slide(2, cx - 7.5f, cy - 7.5f, cx + 1.5f, cy - 1, 40);   /* back through the middle, off centre */
    slide(2, cx + 1.5f, cy - 1, cx + 8, cy + 8, 40);  /* out to ↘ */
    wait_ms(90);
    CHECK(shoulders() == (PAD_L1 | PAD_R2));
    touch_up(t, 2, cx + 8, cy + 8, now);
    game.air = 0;
    fields(2);

    /* a fast overshoot past 14 mm before the gate sends nothing and can come back in */
    take_off();
    touch_down(t, 3, cx, cy, now);
    slide(3, cx, cy, cx, cy - 16, 30);   /* ↑ past the edge in 30 ms */
    slide(3, cx, cy - 16, cx, cy - 10, 16);   /* back in before the gate */
    CHECK(shoulders() == 0);
    wait_ms(90);
    CHECK(shoulders() == (PAD_L1 | PAD_R1));   /* Superman, no △ */
    touch_up(t, 3, cx, cy - 10, now);
    game.air = 0;
    fields(2);

    /* the first latch needs 4 mm of travel; a sector neighbour needs 12° past the edge */
    take_off();
    touch_down(t, 4, cx, cy, now);
    slide(4, cx, cy, cx + 9, cy, 30);                  /* → */
    float a = (22.5f + 6) * 3.14159265f / 180;            /* 6° into ↗: not yet */
    slide(4, cx + 9, cy, cx + 9 * cosf(a), cy - 9 * sinf(a), 30);
    wait_ms(90);
    CHECK(shoulders() == (PAD_R1 | PAD_R2));
    touch_up(t, 4, cx, cy, now);
    game.air = 0;
    fields(2);

    /* landing sends what is held at once, and it stays until the lift */
    take_off();
    touch_down(t, 5, cx, cy, now);
    slide(5, cx, cy, cx - 10, cy, 30);   /* ← moving */
    game.air = 0;
    field();
    CHECK(shoulders() == (PAD_L1 | PAD_L2));
    fields(10);
    CHECK(shoulders() == (PAD_L1 | PAD_L2));
    touch_up(t, 5, cx - 10, cy, now);
    field();
    CHECK(shoulders() == 0);

    /* signature: ← then → through the middle, then out: all four + △ */
    take_off();
    touch_down(t, 6, cx, cy, now);
    slide(6, cx, cy, cx - 9, cy, 40);
    slide(6, cx - 9, cy, cx, cy, 40);
    slide(6, cx, cy, cx + 16, cy, 50);
    wait_ms(120);
    CHECK(shoulders() == (PAD_L1 | PAD_L2 | PAD_R1 | PAD_R2 | PAD_TRIANGLE));
    touch_up(t, 6, cx + 16, cy, now);
    game.air = 0;
    fields(2);

    /* a touch in the air wait becomes the stick when the layout comes, its movement counting */
    game.air = 1;
    field();
    touch_down(t, 7, cx, cy, now);
    slide(7, cx, cy, cx, cy + 9, 40);   /* ↓ inside the wait */
    wait_ms(200);
    CHECK(shoulders() == (PAD_L2 | PAD_R2));   /* Bar Kneel */
    touch_up(t, 7, cx, cy + 9, now);
    game.air = 0;
    fields(2);

    /* the sector table */
    CHECK(touch_sector_buttons(2) == (PAD_L1 | PAD_R1) && touch_sector_buttons(3) == PAD_L1 && touch_sector_buttons(7) == PAD_R2);
    CHECK(!strcmp(touch_trick_name(PAD_L1 | PAD_L2 | PAD_R1), "CLIFF HANGER"));
}

static void test_online_panel(void)
{
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    game.online = 1;
    field();
    tap(1, TC_PAUSE);   /* online: the panel, no START */
    CHECK(!(buttons() & PAD_START));
    /* Leave race: the first tap arms it, the second leaves */
    TouchShape shapes[TOUCH_SHAPES];
    int n = touch_shapes(t, shapes, TOUCH_SHAPES);
    CHECK(n > 0);
    float lx, ly;
    CHECK(touch_target(t, TOUCH_TARGET_PANEL, 2, &lx, &ly));
    int left = 0;
    for (int i = 1; i <= 2; i++) {
        touch_down(t, 2, lx, ly, now);
        touch_up(t, 2, lx, ly, now);
        field();
        if (touch_take_ask(t) == TOUCH_ASK_LEAVE_RACE && !left) left = i;
    }
    /* Bail out online: SELECT twice, no START */
    tap(3, TC_PAUSE);
    float bx, by;
    touch_target(t, TOUCH_TARGET_PANEL, 1, &bx, &by);
    touch_down(t, 4, bx, by, now);
    touch_up(t, 4, bx, by, now);
    uint16_t edges[8];
    int n2 = record(edges, 8, 20);
    CHECK(n2 == 2 && edges[0] == PAD_SELECT && edges[1] == PAD_SELECT);
    CHECK(left == 2);   /* never on the first tap */
}

static uint8_t stick_x(void)
{
    uint16_t b = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    touch_pad(t, &b, &lx, &ly, &rx, &ry);
    return lx;
}

/* The host's drain: every queued event into the rules (the queue holds mm here), then emptied. */
static void drain(TouchQueue *q)
{
    for (int i = 0; i < q->count; i++) touch_event(t, &q->ev[i], q->ev[i].x, q->ev[i].y);
    q->count = 0;
}

static void test_queue(void)
{
    static TouchQueue q;
    float x, y;
    /* a press held through a stall: its moves fill the queue, the lift still arrives (Sol's repro) */
    start(TOUCH_CLASSIC, TOUCH_ATTACK_FLICK);
    at(TC_CROSS, &x, &y);
    memset(&q, 0, sizeof q);
    touch_queue_push(&q, TOUCH_EV_DOWN, 1, x, y, now);
    for (int i = 0; i < 1000; i++) touch_queue_push(&q, TOUCH_EV_MOVE, 1, x, y, now + 1 + i * 0.25);
    touch_queue_push(&q, TOUCH_EV_UP, 1, x, y, now + 300);
    CHECK(q.count < TOUCH_QUEUE && q.ev[q.count - 1].kind == TOUCH_EV_UP && q.merged > 0 && !q.cancelled);
    drain(&q);
    field();
    CHECK(buttons() & PAD_CROSS);      /* the game never read it: it goes as a short press */
    fields(3);
    CHECK(!(buttons() & PAD_CROSS));   /* and ends, not stuck */
    /* a finger's merged moves end where it was: the stick as if every move had come */
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    memset(&q, 0, sizeof q);
    touch_queue_push(&q, TOUCH_EV_DOWN, 1, 30, 45, now);
    for (int i = 1; i <= 600; i++) touch_queue_push(&q, TOUCH_EV_MOVE, 1, 30 + i * 0.01f, 45, now + i * 0.25);
    touch_queue_push(&q, TOUCH_EV_DOWN, 2, 100, 20, now + 200);   /* another finger in between */
    for (int i = 1; i <= 600; i++) touch_queue_push(&q, TOUCH_EV_MOVE, 1, 36 - i * 0.002f, 45, now + 200 + i * 0.25);
    CHECK(q.count <= TOUCH_QUEUE - TOUCH_QUEUE_RESERVE && !q.cancelled);
    CHECK(q.ev[q.count - 1].kind == TOUCH_EV_MOVE && fabsf(q.ev[q.count - 1].x - (36 - 600 * 0.002f)) < 0.001f);
    drain(&q);
    field();
    uint8_t merged_x = stick_x();
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    touch_down(t, 1, 30, 45, now);
    touch_move(t, 1, 36, 45, now + 150);
    touch_move(t, 1, 36 - 600 * 0.002f, 45, now + 350);
    field();
    CHECK(merged_x > 128 && merged_x == stick_x());
    /* a controller held at full tilt: one hide, not a queue of them */
    memset(&q, 0, sizeof q);
    for (int i = 0; i < 1000; i++) touch_queue_push(&q, TOUCH_EV_PAD_USED, 0, 0, 0, now + i);
    CHECK(q.count == 1);
    /* the last resort: a stall full of taps fills even the reserve; every finger is let go, Back stays */
    for (int back = 0; back < 2; back++) {
        start(TOUCH_CLASSIC, TOUCH_ATTACK_FLICK);
        at(TC_CROSS, &x, &y);
        memset(&q, 0, sizeof q);
        touch_queue_push(&q, TOUCH_EV_DOWN, 1, x, y, now);
        if (back) touch_queue_push(&q, TOUCH_EV_BACK, 0, 0, 0, now + 1);
        for (int i = 0; i < 200; i++) {
            touch_queue_push(&q, TOUCH_EV_DOWN, 2, 77, 30, now + 2 + i);
            touch_queue_push(&q, TOUCH_EV_UP, 2, 77, 30, now + 2.5 + i);
        }
        touch_queue_push(&q, TOUCH_EV_UP, 1, x, y, now + 300);
        CHECK(q.cancelled == 1 && q.ev[0].kind == TOUCH_EV_CANCEL && q.ev[1].kind == (back ? TOUCH_EV_BACK : TOUCH_EV_DOWN));
        if (back) continue;   /* Back pauses: the rest is the stuck-press check */
        drain(&q);
        fields(2);
        CHECK(!(buttons() & PAD_CROSS));
    }
    /* a finger id down again without its up: what it held ends first */
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    touch_down(t, 1, 30, 45, now);
    touch_move(t, 1, 40, 45, now + 50);
    field();
    CHECK(stick_x() > 200);
    touch_down(t, 1, 100, 20, now);   /* elsewhere, no stick */
    field();
    CHECK(stick_x() == 128);
    /* a thumb pressed in a stall whose queue is past the moves' part: its first move (none of its
       own to merge into) is kept, so held still it steers (Muse's round-2 F1) */
    for (int full = 0; full < 2; full++) {
        start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
        memset(&q, 0, sizeof q);
        if (full) {   /* every place taken, a stroke of another finger among them: its move goes, its up stays */
            touch_queue_push(&q, TOUCH_EV_DOWN, 3, 100, 20, now);
            touch_queue_push(&q, TOUCH_EV_MOVE, 3, 101, 20, now + 0.1);
            for (int i = 0; i < 126; i++) {
                touch_queue_push(&q, TOUCH_EV_DOWN, 2, 77, 30, now + 1 + i);
                touch_queue_push(&q, TOUCH_EV_UP, 2, 77, 30, now + 1.5 + i);
            }
            touch_queue_push(&q, TOUCH_EV_UP, 3, 101, 20, now + 150);
        } else
            for (int i = 0; i < 127; i++) {
                touch_queue_push(&q, TOUCH_EV_DOWN, 2, 77, 30, now + i);
                touch_queue_push(&q, TOUCH_EV_UP, 2, 77, 30, now + i + 0.5);
            }
        touch_queue_push(&q, TOUCH_EV_DOWN, 1, 30, 45, now + 200);   /* the thumb on the stick */
        CHECK(q.count == (full ? TOUCH_QUEUE : TOUCH_QUEUE - 1) && !q.cancelled);
        touch_queue_push(&q, TOUCH_EV_MOVE, 1, 40, 45, now + 201);   /* its first move: full right */
        touch_queue_push(&q, TOUCH_EV_MOVE, 1, 41, 45, now + 202);   /* the next one rewrites it */
        int moves = 0, last_x = 0;
        for (int i = 0; i < q.count; i++)
            if (q.ev[i].kind == TOUCH_EV_MOVE) moves++, last_x = (int)q.ev[i].x;
        CHECK(moves == 1 && last_x == 41 && !q.cancelled && q.count <= TOUCH_QUEUE);
        CHECK(q.ev[q.count - 1].kind == TOUCH_EV_MOVE);
        if (full) CHECK(q.ev[q.count - 3].kind == TOUCH_EV_UP && q.ev[q.count - 3].id == 3);
        drain(&q);
        field();
        CHECK(stick_x() > 200);
        touch_up(t, 1, 41, 45, now);
        field();
    }
}

/* Tilt steering: until the sensor answers (or with none) the stick steers on the ground. */
static void test_tilt(void)
{
    start(TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
    TouchSettings s = *touch_settings(t);
    s.tilt = 1;
    s.tilt_strength = 50;   /* full turn at 25° */
    TouchScreen screen = {154.5f, 69.5f, 0, 0, 0, 0, 0, 0, 0, 0};
    touch_configure(t, &s, &screen);
    touch_tilt(t, 0, 0);   /* no reading yet */
    touch_down(t, 1, 30, 45, now);
    touch_move(t, 1, 36, 45, now);   /* half right */
    wait_ms(300);
    CHECK(stick_x() > 170 && stick_x() < 200);
    touch_tilt(t, 1, -25);   /* the sensor: full left; it takes over from the stick on the ground */
    wait_ms(300);
    CHECK(stick_x() <= 2);
    touch_tilt(t, 0, 0);   /* gone again (tilt turned off and on): the stick */
    wait_ms(300);
    CHECK(stick_x() > 170 && stick_x() < 200);
    touch_up(t, 1, 36, 45, now);
    field();
}

/* Stick = Fixed: a thumb that lands off the stick's centre steers at once, held still (no move);
   a floating stick centres under the thumb, so it starts neutral (Sol's round-2 #3). */
static void test_fixed_stick(void)
{
    for (int preset = 0; preset < 2; preset++)
        for (int fixed = 0; fixed < 2; fixed++) {
            start(preset ? TOUCH_CLASSIC : TOUCH_THUMB_ZONES, TOUCH_ATTACK_FLICK);
            TouchSettings s = *touch_settings(t);
            s.stick_fixed = fixed;   /* range 12 mm */
            touch_configure(t, &s, NULL);
            float x, y;
            at(TC_STICK, &x, &y);
            touch_down(t, 1, x + 8, y, now);   /* 8 mm right of the centre */
            fields(10);
            if (fixed) CHECK(stick_x() == 212);   /* 128 + 8 / 12 * 127 */
            else CHECK(stick_x() == 128);
            touch_move(t, 1, x + 8, y, now);   /* the same place again: no change */
            field();
            if (fixed) CHECK(stick_x() == 212);
            else CHECK(stick_x() == 128);
            touch_up(t, 1, x + 8, y, now);
            field();
            CHECK(stick_x() == 128);
        }
}

static void test_layout_text(void)
{
    TouchSettings s, back;
    touch_settings_default(&s);
    s.edit[0][TC_PEDAL] = (TouchEdit){-3.5f, 2, 1.1f};
    s.edit[1][TC_CROSS] = (TouchEdit){1, -1, 0.9f};
    s.edit[1][TC_L3] = (TouchEdit){2, 3, 1.1f};    /* names with a digit */
    s.edit[1][TC_R3] = (TouchEdit){-2, -3, 0.9f};
    char text[1024];
    touch_layout_format(&s, text, sizeof text);
    touch_settings_default(&back);
    touch_layout_parse(&back, text);
    CHECK(fabsf(back.edit[0][TC_PEDAL].dx + 3.5f) < 0.01f && fabsf(back.edit[0][TC_PEDAL].size - 1.1f) < 0.01f);
    CHECK(fabsf(back.edit[1][TC_CROSS].dy + 1) < 0.01f);
    CHECK(back.edit[0][TC_HOP].size == 1);
    for (int c = 0; c < TC_COUNT; c++)   /* every control comes back as saved */
        for (int p = 0; p < 2; p++)
            CHECK(fabsf(back.edit[p][c].dx - s.edit[p][c].dx) < 0.01f && fabsf(back.edit[p][c].dy - s.edit[p][c].dy) < 0.01f &&
                  fabsf(back.edit[p][c].size - s.edit[p][c].size) < 0.01f);
}

int main(void)
{
    test_scaling();
    test_flick();
    test_quick_tap();
    test_unread_taps();
    test_air_wait_tap();
    test_sprint_guards();
    test_pause();
    test_pause_controller();
    test_restart_clock();
    test_race_running();
    test_editor_bar();
    test_trick_hint();
    test_classic_resume();
    test_trick_stick();
    test_online_panel();
    test_layout_text();
    test_fixed_stick();
    test_queue();
    test_tilt();
    touch_free(t);
    if (failures) { fprintf(stderr, "test_touch: %d failed\n", failures); return 1; }
    printf("test_touch: all passed\n");
    return 0;
}
