/* Race replays and the attract demo on the delta-time clock (finding 141).

   The race recorder (D_0043F9F0, func_001A8DE8 every race update, func_001A9640 between the
   clock's wait and its step) is an input recording: from GO it keeps pad 0 (buttons and the four
   stick bytes) and the step, and a replay runs the race again from the same seed with those
   inputs and steps. Every 60 samples it keeps the player's z, and the replay stops at the first
   one that differs ("End of Recorded Replay"): the replay has to be the same simulation, step
   for step. The menus reach it: REPLAY in the race results, RE-START / SAVE in the replay menu,
   Load Replay from the memory card (mode 19).

   Retail keeps one sample a frame, and each sample's step byte holds the next frame's step in
   whole fields (1 or 2). On the delta-time clock (fps.c) the steps are wall time, any size from
   60/N to 2 fields, so an offline race runs on the field grid instead:

   - One sample per field of race clock: an update that ends w whole fields (0, 1 or 2) records w
     samples, so 60 samples are a race second at every rate (the buffer, 0x626F samples, keeps
     its seven minutes) and a replay plays one sample per field.
   - The step is chosen when a field ends and is kept until the next one ends (fps.c,
     GRID_RECORD): a code in 1/120 field, in the step byte of the last sample (the byte retail
     uses for the step too; its 1 and 2 are whole fields, the codes are 30 to 240). A replay forces
     the same codes (GRID_PLAY), so it repeats every update exactly, and paces its pictures by
     them (the replay runs at the speed it was ridden, at any selected rate).
   - The pad is read when a field ends: updates in between keep the last sample's pad (the game
     sees its controls at 60 a second, as the original does), so a sample holds every update's
     input.
   - From the race load to GO (race phases 5 and 1) every step is one field (GRID_WHOLE): the
     pre-race wait and the countdown run as the original's, and GO starts on a field with the
     same step in the race and its replay.

   Online races record nothing (state sync, finding 044): the recorder stays off there, as before.
   A deterministic run at 60 (the original arithmetic) takes the original functions. */
#include "hle.h"
#include "../include/fps.h"

extern int pads_online(void);
extern int D_0029E3D0;

/* Race phase (D_0043F348, the race start's +0x488): 5 loading, 1 countdown, 2 from GO, 3 and 4
   finished. Game mode (D_004B25A0 + 0x10C): 10 is the attract demo. */
#define RACE_PHASE (*(int *)GUEST(0x0843F348))
#define GAME_MODE (*(int *)GUEST(0x084B26AC))
#define ATTRACT_DEMO 10
#define MOST_SAMPLES 0x626F

/* A sample (func_001A8EF0 writes it, func_001A8FE0 plays it). */
typedef struct {
    int16_t buttons;
    uint8_t stick[4];
    uint8_t step;  /* the step from the next sample's update on: whole fields (retail) or a code */
    uint8_t unused;
} Sample;

/* The recorder's fields used here (func_001A8958 / func_001A8CE8 set them up). */
typedef struct {
    char pad0[8];
    int mode;           /* +0x08: 0 playing (001A9058 advances), 3 recording */
    uint32_t samples;   /* +0x0C: Sample * */
    uint32_t header;    /* +0x10: the saved header: +0x4 samples recorded, +0x140 a z per 60 */
    char pad14[0x24 - 0x14];
    int index;          /* +0x24: samples recorded, or the next to play */
    char pad28[0x44 - 0x28];
    uint8_t playing;    /* +0x44 */
    char pad45[3];
    int8_t cameras;     /* +0x48: the replay camera director runs while recording (001A9230) */
    char pad49[0x9C8 - 0x49];
    int checked;        /* +0x9C8: the sample of the last z check */
} Recorder;

#define SAMPLE(rec, i) ((Sample *)GUEST((rec)->samples) + (i))
#define RECORDED(rec) (*(int *)GUEST((rec)->header + 4))
#define CHECK_Z(rec, second) (((float *)GUEST((rec)->header + 0x140))[second])

extern short func_00220268(int pad);
extern void func_002202B8(int pad, short buttons);
extern void func_002202D0(int pad, int a, int b, int c, int d);
extern void func_001A8EF0(void *rec);
extern void func_001A9568(void *rec);
extern void func_001A9598(void *rec, int camera);
extern void func_001A9230(void *rec);
extern void func_001A9EA8(void *rec);
extern void func_001AA0F0(void);
extern void func_001B45F8(void *menu, int which, int flag);
extern void func_0020F588(void *unused, int index, int r, int g, int b, int alpha);
extern void func_0020FB80(int index, int x, int y, int width, int height);
extern void func_0020F9F0(int index, int font, int x, int y, int text);
extern char D_004B1F40[];  /* the race's menu: which 6 is the replay menu */
extern char D_0028E888[];  /* "Replay" (the text shown as a replay starts) */

/* The player's z, the position the recorder keeps every 60 samples (D_002AF2C0 + 0x50130: the
   human rider; +0x792C its body node; +0x10 the node's position). */
static float player_z(void)
{
    uint32_t rider = *(uint32_t *)GUEST(0x082AF2C0 + 0x50130);
    uint32_t node = *(uint32_t *)GUEST(rider + 0x792C);
    return *(float *)GUEST(node + 0x18);
}

static void set_pad(const Sample *s)
{
    func_002202B8(0, s->buttons);
    func_002202D0(0, (int8_t)s->stick[0], (int8_t)s->stick[1], (int8_t)s->stick[2], (int8_t)s->stick[3]);
}

/* One update of a race being recorded: a sample for each field it ended. */
static void record(Recorder *rec, int fields)
{
    int i;
    RECORDED(rec) = rec->index;
    if (!fields && rec->index > 0 && rec->index < MOST_SAMPLES)
        set_pad(SAMPLE(rec, rec->index - 1));  /* no field ended: the controls of the last one */
    for (i = 0; i < fields && rec->index < MOST_SAMPLES; i++) {
        /* func_001A8EF0 reads the pad into the sample and keeps the z every 60 samples; it also
           writes a two-field frame's 2 into the previous sample, which on the grid holds the step
           code of the updates since then. */
        uint8_t kept = rec->index > 0 ? SAMPLE(rec, rec->index - 1)->step : 0;
        func_001A8EF0(rec);
        if (rec->index > 0) SAMPLE(rec, rec->index - 1)->step = kept;
        SAMPLE(rec, rec->index)->step = (uint8_t)port_clock_code();
        rec->index++;
    }
    if (rec->cameras)
        for (i = 0; i < fields; i++) func_001A9230(rec);  /* once a field, as once a frame at 60 */
}

/* One update of a replay (func_001A9058 on the field grid): the pad of the field it starts, a z
   check for each field it ends. 0: the replay stopped (it differs from the race). */
static int play(Recorder *rec, int fields)
{
    int i;
    func_001A9568(rec);  /* the live buttons, for the replay's own controls */
    set_pad(SAMPLE(rec, fields ? rec->index : rec->index - 1));
    func_001A9598(rec, 0);
    for (i = 0; i < fields; i++) {
        int at = rec->index;
        if (at - rec->checked > 60) {
            if (at / 60 > 0 && CHECK_Z(rec, at / 60) != player_z()) {
                hle_log("replay: stopped at sample %d of %d: the player is at z %.6g, the race was at %.6g\n",
                        at, RECORDED(rec), player_z(), CHECK_Z(rec, at / 60));
                func_001B45F8(D_004B1F40, 6, 0);
                return 0;
            }
            rec->checked = at;
        }
        if (rec->mode == 0) rec->index++;
    }
    /* "Replay" for its first five seconds, fading out over the sixth */
    if (rec->index < 360) {
        func_0020F588(0, 0, 110, 110, 200, rec->index < 300 ? 80 : 360 - rec->index);
        func_0020FB80(0, 256, 192, -91, -32);
        func_0020F9F0(0, 0, 240, 200, (int)GADDR(D_0028E888));
    }
    return 1;
}

/* The race start (func_0019EE98, every frame of the race screen, before its phase changes):
   from the load to GO every step is one field. Phases 2-4 are func_001A8DE8's. */
void port_replay_race_phase(int phase)
{
    int offline = !pads_online() && GAME_MODE != ATTRACT_DEMO;
    port_clock_grid(offline && (phase == 5 || phase == 1) ? GRID_WHOLE : GRID_OFF);
}

/* Test switches (finding 141's headless check):
   DHD_REPLAY_TEST=S   at race second S of a recorded race its replay starts, as REPLAY in the
                       race results does (func_0015C920, between two frames);
   DHD_REPLAY_TRACE=P  every rider's position at each field of race clock, in the race and in its
                       replay: CSV "pass,sample,rider,x,y,z" (pass 0 the race, 1 the replay; pass
                       2: the attract demo's fields left, every 600). */
static int test_at = -1;
static FILE *trace;
static void test_init(void)
{
    static int init;
    if (init) return;
    init = 1;
    const char *s = host_getenv("DHD_REPLAY_TEST"), *path = host_getenv("DHD_REPLAY_TRACE");
    if (s && *s) test_at = atoi(s) * 60;
    if (path && *path) {
        trace = fopen(path, "w");
        if (trace) fprintf(trace, "pass,sample,rider,x,y,z\n");
    }
}
static void trace_riders(const Recorder *rec)
{
    int count = *(int *)GUEST(0x082AF2C0 + 0x501A8), i;
    for (i = 0; i < count && i < 10; i++) {
        uint32_t node = *(uint32_t *)GUEST(0x082AF2C0 + i * 0x7BD0 + 0x792C);
        const float *p = GUEST(node + 0x10);
        fprintf(trace, "%d,%d,%d,%.9g,%.9g,%.9g\n", rec->playing, rec->index, i, p[0], p[1], p[2]);
    }
    if (rec->playing && rec->index + 2 >= RECORDED(rec)) fflush(trace);
}

/* func_001A8DE8 on the delta-time clock: the attract demo, the recording or the replay of a race
   update. 0: the replay stopped. */
int port_replay_race(void *recorder)
{
    Recorder *rec = recorder;
    int phase = RACE_PHASE, fields = D_0029E3D0, i;
    test_init();
    if (trace && fields && phase >= 2 && phase <= 4 && GAME_MODE != ATTRACT_DEMO) trace_riders(rec);
    if (phase < 2 || phase > 4)
        return 1;  /* before GO: the race start's grid (port_replay_race_phase) */
    if (pads_online()) {
        port_clock_grid(GRID_OFF);
        return 1;
    }
    if (GAME_MODE == ATTRACT_DEMO) {
        /* the demo's camera cuts and its two minutes (0x1C20 fields), counted once a field */
        port_clock_grid(GRID_OFF);
        for (i = 0; i < fields; i++) {
            func_001A9EA8(rec);
            if (trace && *(int *)((char *)rec + 0x9C4) % 600 == 0) {  /* the demo's fields left */
                fprintf(trace, "2,%d,-1,0,0,0\n", *(int *)((char *)rec + 0x9C4));
                fflush(trace);
            }
        }
        return 1;
    }
    if (rec->playing) {
        port_clock_grid(GRID_PLAY);
        if (rec->index >= RECORDED(rec)) {
            hle_log("replay: the end of the recording, %d samples\n", rec->index);
            func_001AA0F0();
            func_001B45F8(D_004B1F40, 6, 0);  /* End of Recorded Replay */
            return 0;
        }
        return play(rec, fields);
    }
    port_clock_grid(GRID_RECORD);
    record(rec, fields);
    return 1;
}

/* func_001A9640 on the delta-time clock, between the clock's wait and its step: the step code
   chosen for this update goes into the last sample (recording), or comes from it (a replay). */
void port_replay_frame(void *recorder)
{
    Recorder *rec = recorder;
    if (rec->index <= 0) return;
    if (test_at > 0 && !rec->playing && rec->index >= test_at && port_clock_recording()) {
        /* what the results menu's REPLAY does (func_0014EFA0, event 0x1D); then the race loop
           ends (func_001B45F8) with a reason func_001BA7D0 opens no menu for (7), and the race
           screen (func_001B6708) starts the race again, now as its replay */
        extern void func_0015C920(void);
        hle_log("replay test: the replay starts after %d samples\n", rec->index);
        test_at = 0;
        func_0015C920();
        func_001B45F8(D_004B1F40, 7, 0);
        return;
    }
    if (rec->playing)
        port_clock_force(SAMPLE(rec, rec->index - 1)->step);
    else if (port_clock_recording())
        SAMPLE(rec, rec->index - 1)->step = (uint8_t)port_clock_code();
}
