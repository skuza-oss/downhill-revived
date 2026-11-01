/* Track bar names (finding 129).
 *
 * The race-progress "track bar" is a 3D object placed in front of the camera: one small dot per
 * rider, each moved along the bar by func_00179338 (which also stores the rider's 0..1 progress
 * at rider+0x7A40). func_00179338 feeds each rider's dot to func_00203B68 through a 16-byte
 * "handle" held in the HUD object: handle[0] = owner (bar node array + base position),
 * handle[1] = the marker object whose +0x10 (x/y/z) is the dot's world point.
 *
 * hle_trackbar runs right after func_00179338 (port/patches_trackbar.py). Online the dots' names
 * are the real players' (hle_online_slot_name); offline they read "YOU" (this view's rider),
 * P1..P4 (the other split-screen players) and "CPU".
 * This PC's name and dot read red, every other rider's green. */
#include "hle.h"
#include <string.h>

#define EE32(a) (*(volatile int32_t *)(uintptr_t)(0x08000000u + (a)))
#define EE_PTR(a) ((void *)(uintptr_t)(0x08000000u + (a)))
#define RIDER(slot) ((uint8_t *)EE_PTR(0x2AF2C0 + 0x7BD0 * (slot)))
#define TB_RIDERS 10

/* The game's HUD text/transform API (port/hle/online.c declares the same). */
void func_0023D618(int view, int *x, int *y);         /* the view's centre */
void func_0023D650(int view, int *w, int *h);         /* its size */
void func_00222D48(int view, float *pos, float *screen); /* world point to the view's GS coords */
void func_0020FB80(int pen, int w, int h, int dw, int dh);
void func_0020F518(int pen, int r, int g, int b, int a);
void func_0020F420(int pen, int view);
void func_0020F9F0(int pen, int view, int x, int y, const char *text); /* centred */
int func_0020F860(int pen, const char *text);                         /* text width, sixteenths */
int func_0023DED0(void);                                              /* HUD mode (0/1/2) */
extern float port_fov_x;                                              /* widescreen.c, 1 when off */
const char *hle_online_slot_name(int slot);                            /* online.c: the online name, else 0 */

static int in_ram(uint32_t p) { return p >= 0x08000000u && p < 0x0A000000u; }

/* The offset (in the HUD object) of the handle func_00179338 gives rider slot i, replicated from
   its disassembly (src/unsorted/00179338.c): the local player and "type 1" riders have fixed
   handles; the others come from a per-rider array indexed past the split-screen views. */
static uint32_t handle_offset(uint32_t local, uint32_t rider, uint32_t type, int mode, int slot, int players)
{
    if (mode == 2) {
        if (rider == local) return 0x1124;
        if (type == 1) return 0x1134;
        return (uint32_t)(0x1054 + (slot - players) * 0x10);
    }
    if (rider == local) return 0x10E4;
    if (type == 1) return 0x10F4;
    return (uint32_t)(0xFC4 + (slot - players) * 0x10);
}

/* A name tag with a black edge, as the names over the riders (online.c hle_online_names). */
static void draw_name(int view, int x, int y, const char *text, int r, int g, int b)
{
    int alpha = 200;
    func_0020F518(0, 0, 0, 0, alpha);
    func_0020F9F0(0, view, x - 1, y, text);
    func_0020F9F0(0, view, x + 1, y, text);
    func_0020F9F0(0, view, x, y - 1, text);
    func_0020F9F0(0, view, x, y + 1, text);
    func_0020F518(0, r, g, b, alpha);
    func_0020F9F0(0, view, x, y, text);
}

enum { TB_SIZE = 80 };

void hle_trackbar(void *hud_ptr)
{
    uint8_t *hud = hud_ptr;
    if (!hud) return;
    int view = *(int *)(hud + 0x1280);                 /* obj->player: this HUD's view */
    if (view < 0 || view > 3) return;
    uint32_t local = *(uint32_t *)(hud + 0x1164);      /* obj->rider: this PC's rider */
    int mode = func_0023DED0();
    int players = EE32(0x4B25A0 + 0x288);              /* D_004B25A0.x288: views/players */
    int nriders = EE32(0x2AF2C0 + 0x501A8);            /* riders in the race */
    if (nriders <= 0 || nriders > TB_RIDERS) nriders = TB_RIDERS;

    /* DHD_TB_DUMP: the handles, owners and markers, every 600th call (finding 129: to find the
       dot's colour); read through the cache, not looked up on every call of this per-field hook */
    static HleEnv dump_env;
    static int calls;
    int dump = hle_env(&dump_env, "DHD_TB_DUMP") && calls++ % 600 == 150;
    uint8_t *pen = EE_PTR(0x4F3570), kept[0x20];
    int cx, cy, w, h, drawn = 0;
    func_0023D618(view, &cx, &cy);
    func_0023D650(view, &w, &h);
    memcpy(kept, pen, sizeof kept);
    func_0020FB80(0, 320 * TB_SIZE / 100, 240 * TB_SIZE / 100,
                  -102 * TB_SIZE / 100, -24 * TB_SIZE / 100);

    for (int slot = 0; slot < nriders; slot++) {
        uint8_t *rider = RIDER(slot);
        uint32_t type = *(uint32_t *)(rider + 0x7A5C); /* 0 = not riding */
        if (!type) continue;
        float t = *(float *)(rider + 0x7A40);          /* progress along the bar (func_00179338) */
        uint32_t off = handle_offset(local, (uint32_t)(uintptr_t)rider, type, mode, slot, players);
        if (off + 8 > 0x1160) continue;
        uint32_t owner = *(uint32_t *)(hud + off), marker = *(uint32_t *)(hud + off + 4);
        int is_local = (uint32_t)(uintptr_t)rider == local;

        float at[4] __attribute__((aligned(16))), screen[4] __attribute__((aligned(16)));
        int have_dot = 0, dotx = 0, doty = 0;
        if (in_ram(owner) && in_ram(marker)) {
            memcpy(at, (void *)(uintptr_t)(marker + 0x10), sizeof at);
            func_00222D48(view, at, screen);
            if (screen[2] >= 1) {
                float lx = screen[0] - (cx - w / 2), ly = screen[1] - (cy - h / 2);
                if (lx >= 0 && lx < w && ly >= 0 && ly < h) {
                    dotx = (int)(w / 2 + (lx - w / 2) / port_fov_x);
                    doty = (int)ly;
                    have_dot = 1;
                }
            }
        }

        if (dump) {
            uint32_t *m = in_ram(marker) ? (uint32_t *)(uintptr_t)marker : 0;
            uint32_t *o = in_ram(owner) ? (uint32_t *)(uintptr_t)owner : 0;
            fprintf(stderr, "tb slot %d view %d mode %d np %d type %u t %.4f is_local %d off %03X "
                    "owner %08X marker %08X dot %d (%d,%d)\n",
                    slot, view, mode, players, type, t, is_local, off, owner, marker, have_dot, dotx, doty);
            if (o) fprintf(stderr, "tb  owner: %08X %08X %08X %08X %08X %08X\n",
                           o[0], o[1], o[2], o[3], o[4], o[5]);
            if (m) fprintf(stderr, "tb  marker: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\n",
                           m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9]);
        }

        if (!have_dot) continue;
        /* offline: this view's rider, the other split-screen players (slots 0..players-1, one
           per view, as handle_offset has them), the computer riders */
        char player[4];
        const char *name = hle_online_slot_name(slot);
        if (!name || !name[0]) {
            if (is_local) name = "YOU";
            else if (slot < players) { snprintf(player, sizeof player, "P%d", slot + 1); name = player; }
            else name = "CPU";
        }
        int half = (func_0020F860(0, name) + 31) / 32;
        int tx = dotx + 8 + half, ty = doty;   /* centred text: left edge at dotx + 8 */
        if (tx - half * port_fov_x < 1 || tx + half * port_fov_x >= w - 1 || ty < 4 || ty >= h - 4)
            continue;
        if (is_local) draw_name(view, tx, ty, name, 208, 48, 40);    /* red */
        else draw_name(view, tx, ty, name, 56, 200, 72);             /* green */
        drawn = 1;
    }

    memcpy(pen, kept, sizeof kept);
    for (int i = 0; drawn && i < 4; i++) func_0020F420(0, i);
}
