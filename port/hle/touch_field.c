/* The touch controls' game side (finding 141), once a field from pad_vsync: what the overlay
   reads from the game, handed to the host's touch_host.c (host_service_touch_field, HostTouch):
   - the rider's "in the air" flag: player 1's rider (D_002AF2C0, slot 0) +0x4564, the rigid body's
     (+0x40C0) airborne age, a short that func_00135A30 counts up every frame in the air and resets
     on contact (findings 049, 067; at high rates fps.c's port_air_tick keeps it in fields);
   - the race HUD hook: widescreen.c's HUD calls (the 3D pieces through func_00176930, the HUD's
     text) set touch_hud_seen; in a race from its countdown (port_race_running(1), fps.h; not
     the shell's screen id, finding 159) that says the race HUD is drawn, which is when the
     overlay shows. The same hook moves the HUD by touch_hud_place (widescreen.c,
     touch_hud_move).
   Besides those, the race clock (D_0043EEC8, the double-tap windows count it) and, for the pause
   rows' ↓ steps, the rows the game's pause menu builds (func_0015E238's own conditions, a menu
   read). Out: where the HUD goes, the online panel's Leave race, and the trick-stick hint's hold:
   the game waits here, then forgets the stall (no game time is caught up after it). */
#include "hle.h"
#include "../include/fps.h"

#include <string.h>

#define EE32(a) (*(volatile int32_t *)(uintptr_t)(0x08000000u + (a)))
#define EE16(a) (*(volatile int16_t *)(uintptr_t)(0x08000000u + (a)))
#define EE8(a) (*(volatile uint8_t *)(uintptr_t)(0x08000000u + (a)))

enum {
    RACE_CLOCK = 0x43EEC8,     /* fields since GO */
    RIDER_AIR = 0x2AF2C0 + 0x4564,
    RACE_STATE = 0x4B25A0,     /* D_004B25A0: +0x10C the mode, +0x110, +0x288 */
    SAVE = 0x4225A0            /* D_004225A0: the save's +0x12199, +0x1219F */
};

int touch_hud_place;    /* widescreen.c: 0 the game's HUD, 1 moved for the right hand, 2 for the left */
int touch_hud_seen;   /* widescreen.c's HUD hooks: the race HUD was drawn since the last field */
static int pending;    /* a TOUCH_COMMAND_* from the TOUCH CONTROLS pages */

void port_online_touch_leave(void);   /* online.c */
extern uint64_t kernel_vsync_count;

void touch_command(int command) { pending = command; }

/* The offline pause menu's rows (func_0015E238): RESUME, [0x7D], OPTIONS, TRICK LIST, [NEW LEVEL],
   [RE-START], [0x6C], [0x3F], MUSIC PLAYLIST, QUIT. The cursor opens on the first. */
static void pause_rows(int32_t rows[3])
{
    int mode = EE32(RACE_STATE + 0x10C), x110 = EE32(RACE_STATE + 0x110), x288 = EE32(RACE_STATE + 0x288);
    int n = 0;
    rows[0] = n++;                                              /* RESUME (0x19) */
    if (mode == 5 && x288 == 1) n++;                            /* 0x7D */
    n += 2;                                                     /* OPTIONS (0x48), TRICK LIST (0x47) */
    if (x110 == 0 && mode != 15 && mode != 16 && mode != 17 && mode != 11) n++;   /* NEW LEVEL (0x29) */
    rows[1] = mode != 17 ? n++ : -1;                            /* RE-START (0x22) */
    if (EE8(SAVE + 0x12199)) n++;                               /* 0x6C */
    if (EE8(SAVE + 0x1219F) && x288 == 1) n++;                  /* 0x3F */
    n++;                                                        /* MUSIC PLAYLIST (0x85) */
    rows[2] = n;                                                /* QUIT (0x1A) */
}

void touch_field(void)
{
    static int32_t last_clock;
    static double last_ms;
    HostTouch h;
    memset(&h, 0, sizeof h);
    int clock = EE32(RACE_CLOCK);
    int race_screen = port_race_running(1);
    h.race_hud = touch_hud_seen && race_screen;
    touch_hud_seen = 0;
    h.air = race_screen && EE16(RIDER_AIR) != 0;
    h.online = pads_online();
    h.clock_moving = race_screen && clock != last_clock;
    last_clock = clock;
    if (race_screen) last_ms = port_field_seconds(clock) * 1000.0;
    h.race_ms = last_ms;
    pause_rows(h.pause_row);
    h.fps = port_fps();
    h.vsync = (uint32_t)kernel_vsync_count;
    h.command = pending;
    pending = 0;
    host_service_touch_field(GADDR(&h));
    touch_hud_place = h.hud_mode;
    if (h.leave_race) port_online_touch_leave();
    if (!h.freeze) return;
    /* the trick stick's hint: the game waits for the first push (or a lift, PEDAL, Back, 5 s) */
    hle_log("touch: the trick-stick hint holds the game\n");
    while (h.freeze) {
        host_sleep_ms(4);
        h.command = 0;
        h.clock_moving = 0;
        h.race_hud = 1;
        h.vsync = (uint32_t)kernel_vsync_count;
        host_service_touch_field(GADDR(&h));
    }
    port_clock_rebase();
    hle_log("touch: the hint let the game go on\n");
}
