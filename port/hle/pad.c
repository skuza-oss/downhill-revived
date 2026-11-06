/* Controllers: the IOP's padman (finding 020). The game's libpad binds the "new" padman servers
   (0x80000100 commands, 0x80000101). PORT_OPEN gives the IOP an EE area of two 128-byte frames;
   padman refreshes the older frame every vsync, and the EE reads the frame with the higher
   counter (scePadRead, scePadGetState, scePadInfoMode read it directly). Here pad_vsync writes
   those frames from the host's controller (host_pad_read) at each VBLANK. Request and frame
   layouts as Sony's libpad uses them (ps2sdk's ee/rpc/pad documents the same protocol).
   Every pad is a DualShock 2. One pad is port 0 slot 0; two are ports 0 and 1; three or four
   sit in a multitap on port 0, slots 0-3 (DHD_PADS; finding 040). Played by hand there are
   always four, each connected while its player has a controller (pads_live). Player i's input
   comes from online.c's pads_read: the host's controller or a script (online too: the race is
   this PC's own, the others' riders are puppets). */
#include "hle.h"
#include "../include/fps.h"

#include <stdlib.h>
#include <string.h>

enum { PAD_OPEN = 0x01, PAD_SET_MMODE = 0x06, PAD_SET_ACTDIR = 0x07, PAD_SET_ACTALIGN = 0x08,
       PAD_GET_BTNMASK = 0x09, PAD_SET_BTNINFO = 0x0A, PAD_SET_VREF = 0x0B, PAD_GET_PORTMAX = 0x0C,
       PAD_GET_SLOTMAX = 0x0D, PAD_CLOSE = 0x0E, PAD_END = 0x0F, PAD_INIT = 0x10, PAD_GET_MODVER = 0x12 };
enum { STATE_DISCONN = 0, STATE_STABLE = 6, RSTAT_COMPLETE = 0 };
enum { ID_DIGITAL = 0x41, ID_ANALOG = 0x73, ID_PRESSURE = 0x79 };

typedef struct {                 /* one frame of the EE's pad area */
    uint8_t data[32];            /* the controller's reply: 0, id, buttons (active low), sticks */
    uint32_t act_dir[2], act_align[2];
    uint8_t act_data[8][4];
    uint16_t mode_table[4];
    uint32_t frame, find_pad_retries, length;
    uint8_t mode_config, mode_cur_id, model, button_data_ready, nr_of_modes, mode_cur_offs;
    uint8_t nr_of_actuators, num_act_comb, val_c6, mode, lock, act_dir_size;
    uint8_t state, req_state, current_task, run_task, stat70bit, padding[11];
} PadFrame;

typedef struct {
    int open;
    PadFrame *area;              /* two frames, in EE memory */
    int analog, pressure, locked;
    uint8_t align[6];
} Pad;

static Pad pads[2][4];          /* [port][slot] */
static uint32_t frame_counter;
static uint32_t *open_slots;     /* PAD_INIT's status area: frame, open slots of port 0 and 1 */

static void *ee(uint32_t addr) { return (void *)(uintptr_t)addr; }

/* One frame: the DualShock 2's answer in its current mode, and padman's bookkeeping. */
static void fill(Pad *p, PadFrame *f, int player)
{
    HostPad in;
    int n;
    pads_read(player, &in);
    memset(f->data, 0xFF, sizeof f->data);
    f->data[0] = 0;
    f->data[1] = p->pressure ? ID_PRESSURE : p->analog ? ID_ANALOG : ID_DIGITAL;
    f->data[2] = (uint8_t)~in.buttons;
    f->data[3] = (uint8_t)(~in.buttons >> 8);
    n = 4;
    if (p->analog) {
        f->data[4] = in.right_x, f->data[5] = in.right_y, f->data[6] = in.left_x, f->data[7] = in.left_y;
        n = 8;
    }
    if (p->pressure) {
        memcpy(f->data + 8, in.pressure, 12);
        n = 20;
    }
    f->length = (uint32_t)n;
    f->frame = ++frame_counter;
    f->find_pad_retries = 0;
    f->mode_config = 2;  /* the controller has mode and actuator information */
    f->model = 3;        /* DualShock 2 */
    f->button_data_ready = 1;
    f->nr_of_modes = 2;
    f->mode_table[0] = 4, f->mode_table[1] = 7;  /* digital, analog */
    f->mode_cur_offs = (uint8_t)p->analog;
    f->mode_cur_id = f->data[1];
    f->nr_of_actuators = 2;
    f->num_act_comb = 1;
    f->mode = (uint8_t)p->analog;
    f->lock = (uint8_t)p->locked;
    /* played by hand, players 2-4's pads without a controller are unplugged (pads_live; player 1
       always has the keyboard, and online scripts drive it); the game's pad loop sets a pad up
       again when its controller comes back */
    f->state = pads_live() && player > 0 && !in.connected ? STATE_DISCONN : STATE_STABLE;
    f->req_state = RSTAT_COMPLETE;
    f->current_task = 1;  /* stable: the info reads are allowed */
    f->run_task = 0;
    /* actuators: 0 small (on/off), 1 large (speed) */
    memcpy(f->act_data[0], "\x00\x01\x00\x01", 4);
    memcpy(f->act_data[1], "\x00\x02\x00\x01", 4);
}

/* The player a (port, slot) pad belongs to: the slot with a multitap, else the port. */
static int player_of(int port, int slot) { return pads_multitap() ? slot : port; }

/* This SDK version sends actuator bytes by SIF DMA rather than the RPC above.
   Deliver them to the same host motors; there is no IOP padman DMA buffer. */
int scePadSetActDirect(int port, int slot, const unsigned char *data) RETAIL(func_00245688);
int scePadSetActDirect(int port, int slot, const unsigned char *data)
{
    if (port < 0 || port >= 2 || slot < 0 || slot >= 4 || !data || !pads[port][slot].open) return 0;
    Pad *p = &pads[port][slot];
    int light = 0, heavy = 0;
    for (int i = 0; i < 6; i++) {
        if (p->align[i] == 0 && data[i]) light = 1;
        if (p->align[i] == 1) heavy = data[i];
    }
    host_service_host_pad_vibrate(player_of(port, slot), light, heavy);
    return 1;
}

static int connected(int port, int slot)
{
    int n = pads_connected();
    if (pads_multitap()) return port == 0 && slot < n;
    return slot == 0 && port < n;
}

void pad_vsync(void)
{
    int port, slot;
    menu_mouse_field();  /* the mouse in the menus: the presses it makes this field */
    touch_field();       /* the touch controls (touch_field.c): before the pads are read */
    pads_begin_field();
    presence_field();
    for (port = 0; port < 2; port++)
        for (slot = 0; slot < 4; slot++) {
            Pad *p = &pads[port][slot];
            if (p->open && p->area) {
                PadFrame *older = p->area[0].frame <= p->area[1].frame ? &p->area[0] : &p->area[1];
                fill(p, older, player_of(port, slot));
            }
        }
    if (open_slots) {
        open_slots[0]++;
        open_slots[1] = pads[0][0].open ? 1 : 0;
        open_slots[2] = 0;
    }
}

int port_controls_capturing(void);  /* controls_settings.c */

/* Just-in-time frames (kernel.c): the controller read again into the frames pad_vsync just wrote,
   at the end of the wait after the VBLANK, so the game reads the newest state. Only the input
   bytes change (no new frame counter). Scripted and online input stay as pad_vsync read them, and
   so does the held-back input while PC SETTINGS > Controls waits for a key or button (pads_read). */
void pad_refresh(void)
{
    static int live = -1;
    int port, slot;
    if (port_controls_capturing()) return;
    if (live < 0)
        live = !getenv("DHD_AUTOPAD") && !getenv("DHD_AUTOPAD_FRAMES") && !getenv("DHD_NAV") &&
               !getenv("DHD_NET_CREATE") && !getenv("DHD_NET_JOIN");
    if (!live || pads_online())  /* the ONLINE menu's race: its script and pads, as with DHD_NET_* */
        return;
    for (port = 0; port < 2; port++)
        for (slot = 0; slot < 4; slot++) {
            Pad *p = &pads[port][slot];
            if (!p->open || !p->area)
                continue;
            PadFrame *f = p->area[0].frame > p->area[1].frame ? &p->area[0] : &p->area[1];
            HostPad in;
            host_service_host_pad_device(player_of(port, slot), GADDR(&in));
            menu_mouse_merge(player_of(port, slot), &in);
            f->data[2] = (uint8_t)~in.buttons;
            f->data[3] = (uint8_t)(~in.buttons >> 8);
            if (f->length >= 8)
                f->data[4] = in.right_x, f->data[5] = in.right_y, f->data[6] = in.left_x, f->data[7] = in.left_y;
            if (f->length >= 20)
                memcpy(f->data + 8, in.pressure, 12);
        }
}

void rpc_pad(int fno, void *send, int ssize, void *recv, int rsize)
{
    int32_t *a = send, *r = recv, cmd = ssize >= 4 ? a[0] : -1, result = 1;
    int port = ssize >= 8 ? a[1] & 1 : 0, slot = ssize >= 12 ? a[2] & 3 : 0;
    Pad *p = &pads[port][slot];
    (void)fno;
    if (rsize < 0x18)
        return;
    switch (cmd) {
    case PAD_INIT:
        open_slots = ee((uint32_t)a[4]);
        break;
    case PAD_GET_MODVER:
        result = 0x400;  /* padman 4.0 */
        break;
    case PAD_GET_PORTMAX:
        result = 2;
        break;
    case PAD_GET_SLOTMAX:
        result = pads_multitap() ? 4 : 1;
        break;
    case PAD_OPEN:
        if (connected(port, a[2])) {
            p->open = 1;
            p->area = ee((uint32_t)a[4]);
            fill(p, &p->area[0], player_of(port, slot));
            fill(p, &p->area[1], player_of(port, slot));
            r[5] = 0;  /* padman's own buffer: none here */
        } else
            result = 0;  /* nothing in that port */
        break;
    case PAD_CLOSE:
        p->open = 0;
        host_service_host_pad_vibrate(player_of(port, slot), 0, 0);
        break;
    case PAD_SET_MMODE:  /* mode (0 digital, 1 analog), lock (3 locks it) */
        p->analog = a[3] & 1;
        p->locked = a[4] == 3;
        if (!p->analog)
            p->pressure = 0;
        r[5] = 1;
        break;
    case PAD_SET_ACTALIGN: case PAD_SET_ACTDIR:  /* vibration: which byte drives which motor, then the bytes */
        if (cmd == PAD_SET_ACTALIGN)
            memcpy(p->align, (uint8_t *)send + 12, 6);
        else {
            const uint8_t *data = (const uint8_t *)send + 12;
            int light = 0, heavy = 0, i;
            for (i = 0; i < 6; i++) {
                if (p->align[i] == 0 && data[i]) light = 1;   /* actuator 0: on or off */
                if (p->align[i] == 1) heavy = data[i];         /* actuator 1: speed */
            }
            host_service_host_pad_vibrate(player_of(port, slot), light, heavy);
        }
        r[5] = 1;
        break;
    case PAD_GET_BTNMASK:  /* every button reports pressure */
        result = 0x3FFFF;
        break;
    case PAD_SET_BTNINFO:  /* pressure on (nonzero) or off */
        p->pressure = p->analog && a[3] != 0;
        r[4] = 1;
        break;
    case PAD_SET_VREF: case PAD_END:
        break;
    default:
        rpc_unknown("pad", fno, send, ssize, recv, rsize);
        return;
    }
    r[3] = result;
}

void rpc_pad_ext(int fno, void *send, int ssize, void *recv, int rsize)
{
    rpc_unknown("pad (ext)", fno, send, ssize, recv, rsize);
}

/* The popup cache is about real game updates, not elapsed 60-Hz fields.
   The main loop and modal popup loop both advance this frame counter.
   RPC waits can deliver another VBLANK inside an update: that must not
   erase the edge before the other consumers of this update read it. */
int port_input_stamp(int legacy_field)
{
    extern unsigned int D_0029E10C;
    return port_delta_clock() ? (int)D_0029E10C : legacy_field;
}
