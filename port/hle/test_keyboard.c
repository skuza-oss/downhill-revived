/* keyboard.c: the layout, menus versus riding, the steering ramp, latching and merging
   (finding 059), and a player's own layout (finding 157). Built and run by port/tools/test_runtime.py. */
#include "hle.h"
#include "controls_table.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t down[256];
static const ControlsLayout *layout;  /* NULL: the default one */
static int input_rate=60;

static HostPad neutral(void)
{
    HostPad p;
    memset(&p, 0, sizeof p);
    p.left_x = p.left_y = p.right_x = p.right_y = 128;
    return p;
}

static HostPad pad(int riding, double now)
{
    HostPad p = neutral();
    keyboard_pad(layout, down, riding, now, &p);
    return p;
}

/* The share of full steering the game gets from a raw stick byte (func_0017E1C0, func_0012DEB0). */
static double steering(int raw)
{
    int v = keyboard_game_axis(raw);
    int a = v < 0 ? -v : v;
    a = a >= 127 ? 127 : a * a / 127;
    return (v < 0 ? -a : a) / 128.0;
}

int main(void)
{
    HostPad p;
    keyboard_reset();

    /* menus: arrows are the d-pad, Space/X Cross, Esc/Backspace/S Triangle, Enter START */
    down[0x25] = 1;  /* left */
    p = pad(0, 0);
    assert(p.buttons == PAD_LEFT && p.left_x == 128 && p.pressure[1] == 255);
    down[0x25] = 0;
    down[' '] = 1;
    assert(pad(0, 0).buttons == PAD_CROSS);
    down[' '] = 0, down['X'] = 1;
    assert(pad(0, 0).buttons == PAD_CROSS);
    down['X'] = 0, down[0x1B] = 1;
    assert(pad(0, 0).buttons == PAD_TRIANGLE);
    down[0x1B] = 0, down[0x08] = 1;
    assert(pad(0, 0).buttons == PAD_TRIANGLE);
    down[0x08] = 0, down[0x0D] = 1;
    assert(pad(0, 0).buttons == PAD_START);
    down[0x0D] = 0;
    assert(pad(0, 0).buttons == 0);

    /* riding: Esc is START; it keeps that meaning while held, though the pause menu opens */
    down[0x1B] = 1;
    assert(pad(1, 1).buttons == PAD_START);
    assert(pad(0, 1.1).buttons == PAD_START);
    down[0x1B] = 0;
    pad(0, 1.2);
    down[0x1B] = 1;
    assert(pad(0, 1.3).buttons == PAD_TRIANGLE);
    down[0x1B] = 0;
    pad(0, 1.4);

    /* riding: right arrow ramps the left stick, linear in the game's steering, full in 0.1 s */
    down[0x27] = 1;
    double last = 0;
    for (int ms = 0; ms <= 140; ms += 4) {
        p = pad(1, 2 + ms / 1000.0);
        assert(p.buttons == 0 && p.left_y == 128);
        double s = steering(p.left_x), want = (ms / 1000.0 + 1.0 / 60) / 0.1;
        if (want > 1) want = 1;
        assert(s >= last);                            /* never back */
        assert(s > want - 0.02 && s < want + 0.03);   /* on the ramp */
        last = s;
    }
    assert(p.left_x == 255 || steering(p.left_x) >= 0.99);
    printf("ramp: 17 ms %.2f, 50 ms %.2f, 100 ms %.2f of full lock\n", steering(pad(1, 2.017).left_x),
           steering(pad(1, 2.05).left_x), steering(pad(1, 2.1).left_x));
    down[0x27] = 0;
    assert(pad(1, 2.2).left_x == 128);                /* centre at once */

    /* left: the other way; both: centre; turning restarts the ramp */
    down[0x25] = 1;
    p = pad(1, 3);
    assert(p.left_x < 128 && steering(p.left_x) < 0);
    p = pad(1, 3.2);
    assert(steering(p.left_x) <= -0.99);
    down[0x27] = 1;
    assert(pad(1, 3.25).left_x == 128);
    down[0x25] = 0;
    p = pad(1, 3.3);
    assert(steering(p.left_x) > 0 && steering(p.left_x) < 0.3);
    down[0x27] = 0;
    pad(1, 3.4);

    /* up/down lean on the stick's Y; a key pressed in a menu stays d-pad until let go */
    down[0x26] = 1;
    p = pad(1, 4.5);
    assert(p.left_y < 128 && p.left_x == 128 && p.buttons == 0);
    down[0x26] = 0;
    pad(1, 4.6);
    down[0x28] = 1;
    assert(pad(0, 5).buttons == PAD_DOWN);
    assert(pad(1, 5.1).buttons == PAD_DOWN && pad(1, 5.1).left_y == 128);
    down[0x28] = 0;
    pad(1, 5.2);

    /* merging with a controller: buttons add, the stick is the pad's unless an arrow moves it */
    HostPad c = neutral();
    c.buttons = PAD_R1;
    c.left_x = 40;
    c.pressure[9] = 255;
    down['Q'] = 1;
    keyboard_pad(NULL, down, 1, 6, &c);
    assert(c.buttons == (PAD_R1 | PAD_L2) && c.left_x == 40 && c.pressure[10] == 255 && c.pressure[9] == 255);
    down['Q'] = 0;

    /* the rest of the layout */
    static const struct { int key; uint16_t button; } table[] = {
        {'S', PAD_TRIANGLE}, {'Z', PAD_SQUARE}, {'C', PAD_CIRCLE}, {'A', PAD_L1}, {'D', PAD_R1}, {'Q', PAD_L2},
        {'E', PAD_R2}, {'W', PAD_R3}, {'F', PAD_L3}, {0x09, PAD_SELECT}};
    for (unsigned i = 0; i < sizeof table / sizeof *table; i++) {
        down[table[i].key] = 1;
        assert(pad(0, 7).buttons == table[i].button && pad(1, 7).buttons == table[i].button);
        down[table[i].key] = 0;
        pad(0, 7);
    }

    /* DHD_KEYS scripts */
    uint8_t fake[256];
    assert(keyboard_script("1:2:space+right,4:0.5:x", 1.5, fake) && fake[' '] && fake[0x27] && !fake['X']);
    assert(keyboard_script("1:2:space+right,4:0.5:x", 4.2, fake) && !fake[' '] && fake['X']);
    assert(!keyboard_script("1:2:nokey", 1, fake));
    keyboard_reset();
    memset(down,0,sizeof down);down[0x27]=1;input_rate=144;
    for (int i=0;i<16;i++) {
        double expected=(i+1)/(144.0*.1);
        if(expected>1) expected=1;
        double got=steering(pad(1,i/144.0).left_x);
        assert(got>expected-.03 && got<expected+.03);
    }
    input_rate = 60;

    /* the default layout is keyboard.c's old table: every key it read, Esc first */
    uint8_t keys[CONTROL_TARGETS * CONTROL_SLOTS + 1];
    assert(keyboard_keys(NULL, keys) == 19 && keys[0] == 0x1B);

    /* a layout of the player's: Cross on J and Space, X on Square, the d-pad on WASD (still the
       ramp while riding), a left stick key, the right stick up on I */
    ControlsLayout mine = control_defaults[CONTROL_KEYBOARD];
    mine.input[0][0] = 'J', mine.input[0][1] = ' ';        /* cross */
    mine.input[2][0] = 'X', mine.input[2][1] = 0;          /* square */
    mine.input[3][0] = 0x08, mine.input[3][1] = 0;         /* triangle: Backspace only (S is the d-pad's) */
    mine.input[12][0] = 'W', mine.input[13][0] = 'S', mine.input[14][0] = 'A', mine.input[15][0] = 'D';
    mine.input[4][0] = 'Q', mine.input[5][0] = 'E', mine.input[6][0] = 0, mine.input[7][0] = 0;  /* l1 r1; l2 r2 none */
    mine.input[9][0] = 0;                                  /* r3: none (W is up) */
    mine.input[18][0] = 'K';                               /* lstick_left */
    mine.input[20][0] = 'I';                               /* rstick_up */
    layout = &mine;
    keyboard_reset();
    memset(down, 0, sizeof down);
    down['J'] = 1;
    assert(pad(0, 10).buttons == PAD_CROSS && pad(0, 10).pressure[6] == 255);
    down['J'] = 0, down['X'] = 1;
    assert(pad(0, 10).buttons == PAD_SQUARE);
    down['X'] = 0, down['Z'] = 1;
    assert(pad(0, 10).buttons == 0);                       /* Z is no button now */
    down['Z'] = 0, down['W'] = 1;
    assert(pad(0, 10).buttons == PAD_UP);                  /* menus: the d-pad */
    down['W'] = 0;
    pad(0, 10.1);
    down['A'] = 1;
    p = pad(1, 11);
    assert(p.buttons == 0 && steering(p.left_x) < 0);      /* riding: the d-pad's keys steer, ramped */
    assert(steering(p.left_x) > -0.3);
    assert(steering(pad(1, 11.2).left_x) <= -0.99);
    down['A'] = 0;
    pad(1, 11.3);
    down['K'] = 1;
    p = pad(0, 12);
    assert(p.buttons == 0 && steering(p.left_x) < 0);      /* a stick key moves the stick in menus too */
    down['K'] = 0, down['I'] = 1;
    p = pad(1, 13);
    assert(p.right_y < 128 && p.left_x == 128 && p.buttons == 0);
    down['I'] = 0;
    pad(1, 13.1);
    down[0x1B] = 1;                                        /* Esc is no key of the layout: always back / pause */
    assert(pad(0, 14).buttons == PAD_TRIANGLE);
    down[0x1B] = 0;
    pad(0, 14.1);
    assert(keyboard_keys(&mine, keys) == 17);
    layout = NULL;
    puts("test_keyboard: ok (60 and 144 Hz ramps, a player's layout)");
    return 0;
}
/* Standalone keyboard tests retain the original 60-Hz input interval. */
int port_delta_clock(void) { return input_rate>60; }
double port_frame_period(void) { return 1.0/input_rate; }
