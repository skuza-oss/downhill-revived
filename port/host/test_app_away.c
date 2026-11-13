/* A phone's app back from the background, through the code the return runs (finding 227):
   - the host's flag (app_away.c): away, then one "came back" the core takes once (kernel.c reads it
     through host_service_host_came_back after the present that waited; fps.c's
     port_clock_after_away rebases on it: test_fps away);
   - what the touch controls do at the return (touch_host.c calls touch_pad_lost): an offline race
     opens the game's own pause (a START press), an online race does not pause (finding 215);
   - the online snapshot clock (finding 220's sync_clock_stood): an absence the clock was rebased
     over stands still once, by exactly its length; stamps neither jump back nor stall after it.
   GPL-3.0-or-later. */
#include "app_away.h"
#include "../hle/touch.h"
#include "../hle/hle.h"   /* PAD_START */
#include "../hle/puppet_sync.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static Touch *t;
static TouchGame game;
static double now;

static void field(void)
{
    now += 1000.0 / 60;
    game.vsync++;
    if (game.clock_moving) game.race_ms += 1000.0 / 60;
    touch_update(t, &game, now);
}

static uint16_t buttons(void)
{
    uint16_t b = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    touch_pad(t, &b, &lx, &ly, &rx, &ry);
    return b;
}

/* A race on screen, then the app away and back (touch_host.c: the cancel, then touch_pad_lost at
   the return); 1 when START was pressed in the fields after. */
static int back_in_a_race(int online)
{
    if (t) touch_free(t);
    t = touch_new();
    TouchSettings s;
    touch_settings_default(&s);
    s.vibration = 0;
    s.hints_shown = ~0u;
    TouchScreen screen = {154.5f, 69.5f, 0, 0, 0, 0, 0, 0, 0, 0};
    touch_configure(t, &s, &screen);
    memset(&game, 0, sizeof game);
    game.race_hud = 1, game.clock_moving = 1, game.fps = 60, game.race_ms = 10000, game.online = online;
    game.pause_row[0] = 0, game.pause_row[1] = 4, game.pause_row[2] = 6;
    now = 100000;
    for (int i = 0; i < 30; i++) field();
    touch_cancel_all(t, now);          /* away: every finger let go */
    now += 17000;                       /* 17 s in the background: no field */
    for (int i = 0; i < 9; i++) field();   /* the first fields back draw the race again */
    touch_pad_lost(t, now);             /* touch_host_app_away's pause_due */
    int start = 0;
    for (int i = 0; i < 12; i++) {
        field();
        start |= (buttons() & PAD_START) != 0;
    }
    return start;
}

int main(void)
{
    /* the flag: one came-back per return, none while away, a quick flip still one */
    assert(!app_away_now() && !app_away_came_back());
    app_away_set(1);
    assert(app_away_now() && !app_away_came_back());
    app_away_set(0);
    assert(!app_away_now() && app_away_came_back() && !app_away_came_back());
    app_away_set(1), app_away_set(0), app_away_set(1), app_away_set(0);
    assert(app_away_came_back() == 1 && !app_away_came_back());

    /* the touch controls at the return */
    assert(back_in_a_race(0));     /* offline: the game's pause */
    assert(!back_in_a_race(1));    /* online: the race goes on (finding 215) */
    touch_free(t);

    /* the snapshot clock across 17 s away with the clock rebased (no game time, nothing owed) */
    SyncStood stood = {0, 0};
    double wall = 1000, race = 0, base = sync_clock_base(wall, race, 0), last_stamp = -1;
    for (int i = 0; i < 600; i++) {   /* 10 s of racing, then away, then 10 s more */
        if (i == 300) wall += 17;      /* the absence: the wall goes on, the race clock doesn't */
        wall += 1.0 / 60, race += 1.0 / 60;
        sync_clock_stood(&stood, wall, base, race, 0);
        double stamp = base + race + stood.stood;   /* the wall moment this race clock stands for */
        if (last_stamp >= 0) assert(stamp >= last_stamp && stamp - last_stamp < 0.05 + (i == 300 ? 17.01 : 0));
        last_stamp = stamp;
    }
    assert(fabs(stood.stood - 17) < 0.05);   /* counted once, exactly, and not given back */
    puts("app away: one came-back per return; offline pause, online none; the snapshot clock stands 17 s once");
    return 0;
}
