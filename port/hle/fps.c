/* The game clock: one delta-time clock at every frame rate, offline and online, never synthesized
   pictures (findings 049, 067, 114, 139). */
#include "hle.h"
#include "../runtime/portable.h"
#include "../include/fps.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern int pads_online(void);
extern int D_0029E3D0, D_0029E3D4, D_0029E3D8, D_0029E3DC;
extern int D_0029E3E0,D_0029E3E8;
extern float D_0029E3FC, D_0029E400;
extern int func_00243CE8(int);
static int requested;
static double phase, anim_phase;
static double step_fields, previous_wall, owed;
static int wall_mode, delta = -1;
static unsigned clock_serial;
static int race_loop;   /* the race's loop ran since the front end's last frame (fps.h, finding 159) */

void port_loop_frame(int race)
{
    if (race != race_loop) hle_log(race ? "game: a race's loop runs\n" : "game: the front end runs\n");
    race_loop = race;
}

int port_in_race(void) { return race_loop && *(int *)GUEST(0x084B26AC) != PORT_MODE_ATTRACT; }
int port_race_running(int first_phase)
{
    return port_race_rule(race_loop, *(int *)GUEST(0x084B26AC), *(int *)GUEST(0x0843F348), first_phase);
}

/* Online, from the countdown to the finish (race screen, race phase 1-4). */
static int online_race(void)
{
    int race_phase=*(int *)GUEST(0x0843F348);
    return pads_online() && *(int *)GUEST(0x0829DED8)==9 && race_phase>=1 && race_phase<=4;
}

/* The step's limit: two fields (1/30 s), the original's own (func_00238D98 clamps its step to 1..2
   fields). Below 30 frames a second the game runs slower than real time; the cap keeps every
   system (physics, the ragdoll, particles, timers) inside the steps it was made for. */
#define MOST_FIELDS 2.0

/* Paced play (kernel_init: normal play, or the browser's DHD_FPS_WALL=1) steps the game by the
   real time since the last frame, at every selected rate (finding 139). Deterministic runs
   (DHD_FRAME_CLOCK=1: the state trace, race comparisons, benches) keep the rational clock, 60/N
   fields a step. */
static double vblank_wall, vblank_debt;
void port_clock_wall(int enabled) { wall_mode=enabled; delta=-1; port_clock_rebase(); }
/* A wait that is no game time (a lobby, an online start): the next step starts from now. */
void port_clock_rebase(void) { previous_wall=0; owed=0; vblank_wall=0; vblank_debt=0; }
/* The delta-time clock's code paths (finding 067's fractional steps) are on: paced play at any
   rate, any rate but 60, or DHD_STEP_CLOCK=1 (a deterministic run of those paths at 60, for the
   rate drift check). Only a deterministic run at 60 keeps the original arithmetic, exactly. */
int port_delta_clock(void)
{
    if (delta < 0) {
        const char *s = host_getenv("DHD_STEP_CLOCK");
        delta = wall_mode || port_fps() != 60 || (s && atoi(s) == 1);
    }
    return delta;
}

/* This frame's step in fields. The wall time owed to the game (`owed`) gains the time since the
   last frame and gives half of it as the step: an exponential average of the frame times, so
   frame-time jitter reaches the physics at a third of its size (an alternating 9/17 ms comes out
   as 11.7/14.3 ms) and a hitch is spread over the next few frames instead of one leap. It
   conserves time: the steps add up to the wall time, less what is still owed (about a frame). No
   debt is kept beyond that, so there is no fast-forward: what the two-field cap leaves over
   (frames slower than 1/30 s, a stall, a window drag) is dropped offline, and the game runs that
   much slower. An online race keeps it and gives it back at up to two fields a step once the
   frames are quicker, so this rider's race clock stays where the other players see it (finding
   114). A step is never shorter than one at the selected rate, 60/N fields. */
static void clock_spend(double step, double keep)
{
    owed-=step;
    if (owed>keep && !online_race()) owed=keep;  /* only the cap leaves more than a step */
    if (owed<-MOST_FIELDS) owed=-MOST_FIELDS;    /* frames quicker than the rate: at most 2 fields ahead */
}
static double clock_interval(void)
{
    const double least=60.0/port_fps();
    if (!wall_mode) return least;
    double now=host_wall_seconds();
    if (!previous_wall) {
        previous_wall=now;
        return least;
    }
    double elapsed=(now-previous_wall)*60;
    previous_wall=now;
    if (!(elapsed>=0) || !isfinite(elapsed)) elapsed=least;
    owed+=elapsed;
    double step=owed*0.5;
    if (step<least) step=least;
    if (step>MOST_FIELDS) step=MOST_FIELDS;
    clock_spend(step,step);
    return step;
}
/* A step the race keeps for the rest of a field (the field grid, below): the wall time still
   counts against it, and what it leaves is owed to the next chosen step (up to two fields: a
   held step is shorter than the frames when they slow down). */
static void clock_hold(double step)
{
    if (!wall_mode) return;
    double now=host_wall_seconds();
    double elapsed=previous_wall ? (now-previous_wall)*60 : step;
    previous_wall=now;
    if (!(elapsed>=0) || !isfinite(elapsed)) elapsed=step;
    owed+=elapsed;
    clock_spend(step,MOST_FIELDS);
}
/* Steps that are no wall time (the grid's whole fields, a replay's recorded ones): the wall
   clock starts again from this frame. */
static void clock_rest(void)
{
    if (wall_mode) previous_wall=host_wall_seconds();
    owed=0;
}
/* The mixer's time for one VBLANK on the delta-time clock (kernel.c vsync; finding 139). The frame
   clock delivers a VBLANK every BUS_HZ/N cycles, 1024 per kernel boundary (timer.c), and a frame
   crosses about 400 boundaries: above about 360 one update spans several VBLANKs. Charging the
   update's step on each VBLANK ran the sound that many times too fast.
   Paced, a VBLANK is the wall time since the previous one, at most two fields each (the game
   clock's own cap: below 30 frames a second sound and game slow down together). A hitch is
   caught up (the device played on from its cushion meanwhile), but only by 0.1 s: a longer
   stall already played as a gap, and more would only overfill the output queue, which skips it
   (port/host/sound_out_sdl.c). Unpaced, it is the virtual field, 1/N s. */
double port_vblank_seconds(void)
{
    const double nominal=1.0/port_fps(), most=MOST_FIELDS/60;
    if (!wall_mode) return nominal;
    double now=host_wall_seconds();
    if (vblank_wall && now>=vblank_wall && isfinite(now-vblank_wall)) vblank_debt+=now-vblank_wall;
    else vblank_debt+=nominal;
    vblank_wall=now;
    if (vblank_debt>0.1) vblank_debt=0.1;
    double result=vblank_debt<most ? vblank_debt : most;
    vblank_debt-=result;
    return result;
}
static int whole;
static uint64_t race_steps;  /* real race updates, for DHD_FPS_TRACE */

static void reset_body_clocks(void);
void port_fps_race_step(void) { race_steps++; }

/* The field grid of an offline race (finding 142; port/hle/replay.c says why). The race asks for
   it every update (port_clock_grid), and the next step follows it:
   GRID_WHOLE   from the load to GO: one field a step, pictures every 1/60 s;
   GRID_RECORD  from GO: the step is chosen when a field ends, as a code in 1/120 field, and kept
                until the next field ends; the wall time not stepped stays owed (time conserved);
   GRID_PLAY    a replay: each step is the recorded code (port_clock_force), and the pictures
                follow it.
   The part of a field run and the animation subticks are counted in integer 1/120 fields, so the
   same codes give the same whole fields and subticks at any selected rate. A frame without a race
   update (menus) is off the grid; a pause calls no wait and changes nothing. */
#define CODE_FIELD 120
#define CODE_MOST 240  /* two fields */
static int grid, grid_asked, grid_seen;
static int grid_code=CODE_FIELD, grid_phase, grid_phase_before, grid_anim, grid_ended;

void port_clock_grid(int mode) { grid_asked=mode; grid_seen=1; }
int port_clock_recording(void) { return grid==GRID_RECORD; }
int port_clock_code(void) { return grid_code; }

/* A step in fields as a code: the nearest 1/120 field, no shorter than a step at the selected
   rate and no longer than two fields. */
static int step_code(double fields)
{
    int code=(int)floor(fields*CODE_FIELD+0.5), least=(int)ceil(CODE_FIELD*60.0/port_fps()-1e-9);
    return code<least ? least : code>CODE_MOST ? CODE_MOST : code;
}
static void grid_advance(int code)
{
    grid_code=code;
    step_fields=(double)code/CODE_FIELD;
    grid_phase_before=grid_phase;
    grid_phase+=code;
    whole=grid_phase/CODE_FIELD;
    grid_phase-=whole*CODE_FIELD;
    phase=grid_phase*(double)port_fps()/CODE_FIELD;
    grid_ended=whole>0;
}
static void grid_enter(int mode)
{
    if (mode==grid) return;
    if (!grid) {
        /* a race starts on a field, with the subticks and the bodies' clocks from zero, the same
           in the race and in its replay */
        grid_phase=grid_anim=0;
        reset_body_clocks();
    } else if (!mode) {
        phase=grid_phase*(double)port_fps()/CODE_FIELD;
        anim_phase=grid_anim*(double)port_fps()/CODE_FIELD;
    }
    if (mode==GRID_RECORD) grid_ended=1;  /* the first step from GO is chosen */
    grid=mode;
}
/* This update's step on the grid (port_clock_wait). */
static void grid_wait(void)
{
    int code=grid_code;
    if (grid==GRID_WHOLE) {
        code=CODE_FIELD;
        clock_rest();
    } else if (grid==GRID_PLAY) {
        clock_rest();  /* the recorded step comes before the update (port_clock_force) */
    } else if (grid_ended) {
        double step=clock_interval();
        code=step_code(step);
        if (wall_mode) owed+=step-(double)code/CODE_FIELD;  /* the rounding stays owed */
    } else {
        clock_hold((double)code/CODE_FIELD);
    }
    grid_advance(code);
}
/* A replay's step: the code in the last sample played (func_001A9640's place, between the wait
   and the step). A step byte of 1 or 2 is the original's whole fields (a recording made on the
   original clock: it does not play the same on this one, and stops at its first check). */
void port_clock_force(int code)
{
    if (grid!=GRID_PLAY) return;
    if (code>=1 && code<=2) code*=CODE_FIELD;
    else if (code<CODE_FIELD/4 || code>CODE_MOST) code=CODE_FIELD;
    D_0029E3D8-=whole;
    grid_phase=grid_phase_before;
    grid_advance(code);
    D_0029E3D8+=whole;
}

/* The highest selectable rate: 240, or DHD_FPS_MAX when lower (the browser keeps 144). At 240 an
   update is 256 animation subticks (port_animation_step); keep the cap far below 1024. */
int port_fps_max(void)
{
    static int most;
    if (!most) {
        const char *s = host_getenv("DHD_FPS_MAX");
        most = s && atoi(s) >= 60 && atoi(s) < 240 ? atoi(s) : 240;
    }
    return most;
}
/* The selected rate: the most steps (and pictures) a second, 30 to port_fps_max (the settings
   offer 60 and up; 30-59 for tests and a hand-written dhd.ini). */
int port_fps(void)
{
    if (!requested) {
        const char *s = host_getenv("DHD_FPS");
        requested = s ? atoi(s) : 60;
        if (s && !dhd_stricmp(s, "monitor")) {
            requested = host_display_hz();
            if (!requested) requested = 60;
        }
        if (requested < 30) requested = 30;
        if (requested > port_fps_max()) requested = port_fps_max();
        hle_log("fps: requested %d steps/s at most\n", requested);
    }
    return requested;
}
/* The time a picture is held: one at the selected rate (59.94 Hz at 60), a field on the grid
   before GO, a recorded step in a replay. */
double port_frame_period(void)
{
    if (grid == GRID_WHOLE) return 1.0 / 60;
    if (grid == GRID_PLAY) return grid_code / (60.0 * CODE_FIELD);
    int n = port_fps();
    return n == 60 ? 1001.0 / 60000.0 : 1.0 / n;
}
float port_step(void) { return port_delta_clock() ? (float)(step_fields ? step_fields : 60.0/port_fps()) : (float)D_0029E3D0; }
int port_whole_step(void) { return port_delta_clock() ? whole : D_0029E3D0; }
/* A per-field countdown that retail took one off a frame and tests for 0: down by this update's
   whole fields (0 to 2), stopping at 0, so a two-field step cannot jump over it (finding 139). */
int port_count_down(int value) { int w = port_whole_step(); return value > 0 && value < w ? 0 : value - w; }
/* An up-counter just advanced by this update's whole fields: has it reached `mark` in this
   update? (Retail compared it with == once a frame; a two-field step can jump over it, and an
   update with no whole field must not count it again.) */
int port_count_reached(int value, int mark) { return value >= mark && value - port_whole_step() < mark; }
float port_decay(float f) { return port_delta_clock() ? powf(f, port_step()) : f; }
float port_blend(float r) { return port_delta_clock() && r > 0 && r < 1 ? (float)(1.0-pow(1.0-(double)r,(wall_mode || grid ? step_fields : 60.0/port_fps()))) : r; }
float port_particle_sum(float q) {
    /* Evaluate the geometric sum in double: float cancellation near q=1
       otherwise makes gently damped particles move systematically too far. */
    return q == 1.0f ? port_step() : (float)((double)q*(1.0-pow((double)q,(wall_mode || grid ? step_fields : 60.0/port_fps())))/(1.0-q));
}
void port_particle_rotation(float *c,float *s)
{
    float angle=atan2f(*s,*c)*port_step();
    *c=cosf(angle);
    *s=sinf(angle);
}
/* The original burst tests age before advancing it, so it retires one full
   legacy field after crossing 1. Preserve that lifetime, including the
   original float accumulation/field quantization, while moving every step. */
int port_particle_dead(float age,float rate)
{
    typedef struct { unsigned bits; int fields; } Life;
    static Life cache[4096];
    union { float f; unsigned u; } key;
    key.f=rate;
    if (!(rate>0) || !isfinite(rate)) return age>=1;
    unsigned start=(key.u*2654435761u)>>20,index=start;
    unsigned probes=0;
    while (cache[index].bits && cache[index].bits!=key.u && probes++<16) {
        index=(index+1)&4095;
    }
    if (probes>16) index=start; /* bounded replacement when the cache fills */
    if (cache[index].bits!=key.u) {
        float legacy=0;
        int fields=1;
        while (legacy<1 && fields<65536) { legacy+=rate; fields++; }
        cache[index].bits=key.u;
        cache[index].fields=fields;
    }
    return (double)age>=(double)rate*(cache[index].fields-(double)port_step());
}
void port_clock_reset(void)
{
    phase = anim_phase = 0;
    whole = 0;
    step_fields=60.0/port_fps();
    previous_wall=0;
    owed=0;
    grid=grid_seen=0;
    reset_body_clocks();
    port_clock_step();
}
void port_clock_wait(int *out)
{
    D_0029E3E0 += *(volatile int *)GUEST(0x10000800);
    *(volatile int *)GUEST(0x10000800)=0;
    int result = func_00243CE8(0);
    if (out) *out = result == 0;
    clock_serial++;
    grid_enter(grid_seen ? grid_asked : GRID_OFF);
    grid_seen=0;
    if (grid) {
        grid_wait();
    } else {
        step_fields=clock_interval();
        phase += wall_mode ? step_fields*port_fps() : 60;
        whole = (int)(phase / port_fps());
        phase -= whole*port_fps();
    }
    D_0029E3D8 += whole;
}
void port_clock_step(void)
{
    D_0029E3D0 = whole;
    int stepped = wall_mode || grid;  /* the step is this update's own, not 60/N */
    /* The integer rate is rounded: truncated, the 1.001-field steps of paced 60 made it 59. Its
       readers that take a time step read the exact one (port_step_seconds, port_step_rate). */
    D_0029E3D4 = stepped ? (int)lround(60.0/step_fields) : port_fps();
    D_0029E3FC = stepped ? (float)(60.0/step_fields) : (float)port_fps();
    D_0029E400 = stepped ? (float)(step_fields/60.0) : 1.0f / port_fps();
    D_0029E3DC = D_0029E3D8;
    D_0029E3E0 += *(volatile int *)GUEST(0x10000800);
    *(volatile int *)GUEST(0x10000800)=0;
    D_0029E3E8=D_0029E3E0;
}
/* The steps a second (func_00238F30, D_0029E3D4) as about twenty units read it: as 1/rate, the
   seconds of this step (the rigid-body integrator 002327B0, the contact solver, 00209248's
   callers), or as a rate. Retail's rate is exact, 60 or 30. On the delta-time clock the step is
   any length, and an integer rate is off by up to half a step a second (truncated it was 1.2-1.6 %
   too long at paced 60): there they read the exact step (patches_fps.py). The original path keeps
   retail's expression in the unit itself: the game's float code rounds toward zero (the EE's
   mode), so a 1/60 computed here would not be retail's 1/60. */
float port_step_seconds(void) { return D_0029E400; }
float port_step_rate(void) { return D_0029E3FC; }

/* Stored animation timestamps retain their four-byte integer layout. On the delta-time clock
   their unit is 1/1024 field; all consumers convert elapsed values. */
int port_animation_step(void)
{
    if (port_animation_scale() == 1) return D_0029E3D0;
    if (grid) {
        grid_anim += grid_code*1024;
        int ticks = grid_anim / CODE_FIELD;
        grid_anim -= ticks*CODE_FIELD;
        return ticks;
    }
    anim_phase += wall_mode ? step_fields*1024*port_fps() : 60 * 1024;
    int ticks = (int)(anim_phase / port_fps());
    anim_phase -= ticks*port_fps();
    return ticks;
}
int port_animation_scale(void) { return port_delta_clock() ? 1024 : 1; }
float port_animation_fields(float t) { return port_animation_scale() > 1 ? t * (1.0f/1024) : t; }

/* Per-body fractional airborne age; no layout changes to retail short x4A4.
   A reset to zero by contact invalidates the residual on the following step. */
typedef struct {
    void *body;
    float age;
    int first,force_field,force_valid;
    double stalled_steps;
    float stall_origin[3];
} Air;
static Air air[32];
static void reset_body_clocks(void)
{
    clock_serial+=2; /* invalidate previous-step Verlet spans across scene/reset boundaries */
    memset(air,0,sizeof air);
}
static Air *air_for(void *body)
{
    for (int i=0; i<32; i++) if (air[i].body == body) return &air[i];
    for (int i=0; i<32; i++) if (!air[i].body) { air[i].body=body; return &air[i]; }
    /* Rider bodies come from a fixed pool of eight; refuse silent aliasing. */
    abort();
}
int port_air_tick(void *body, int previous)
{
    Air *a=air_for(body);
    if (!previous) a->age=0;
    a->first = !previous;
    /* The take-off happened somewhere in this step: its first step counts at most one field, as
       retail's first airborne frame counts 1 at any step. A whole two-field step (30 a second)
       otherwise made every bump "airborne" at once (00138560: x4A4 > 1), and riders crashed every
       few seconds (finding 139). */
    a->age += previous ? port_step() : fminf(port_step(), 1.0f);
    /* Integer consumers are elapsed-field timers: ceil made >1 true at
       1.25 fields at 144 Hz, while the original first becomes true at 2.
       Keep a nonzero sentinel until the first complete field so contact and
       the separately tracked first-air-step event remain distinguishable. */
    int fields=(int)a->age;
    return fields>0 ? fields : 1;
}
int port_air_first(void *body, int current) { return current && air_for(body)->first; }
float port_air_age(void *body, int current) { return current ? air_for(body)->age : 0; }
/* Finer collision steps can settle a pedalling bike into a stable wall contact
   where the coarse retail solver falls/rebounds. Its old automatic recovery
   only watches airborne/out-of-world cases (00155208), so this equilibrium
   otherwise lasts indefinitely. Detect sustained immobility in physical time
   and enter the ordinary collision recovery controller, without scripted input
   or direct position/velocity writes. */
void port_fps_contact_recovery(void *body)
{
    if (!port_delta_clock()) return;
    unsigned char *b=body,*r=*(unsigned char **)(b+0x158);
    Air *a=air_for(body);
    int slot=*(int *)(r+0x797C);
    float *p=(float *)(b+0x50);
    int contact=b[0x354]!=2 || b[0x3D4]!=2;
    int supported=contact || (a->stalled_steps && port_air_age(body,*(short *)(b+0x4A4))<4);
    int pedal=*(float *)(r+0x7A78)>0 || r[0x7A69];
    extern float func_0012DF48(void *);
    /* AI already has the retail grounded recovery states (001100D0/103B0).
       Preserve their own field timers; this fills the human controller gap. */
    /* in a race from GO, also one started from CONTINUE SAVED GAME (screen 22, finding 159) */
    if (*(int *)(r+0x7A5C)!=1 || slot<0 || slot>=10 || !port_race_running(2) ||
        *(unsigned char *)GUEST(0x0843EF18+slot) || (r[0x4098]&4) ||
        !supported || !pedal || func_0012DF48(r)>0) {
        a->stalled_steps=0;
        return;
    }
    if (!a->stalled_steps) memcpy(a->stall_origin,p,sizeof a->stall_origin);
    float moved2=0;
    for (int i=0;i<3;i++) { float delta=p[i]-a->stall_origin[i]; moved2+=delta*delta; }
    if (moved2>=1) { a->stalled_steps=0; return; } /* slow climbing makes actual progress */
    a->stalled_steps+=port_step();
    if (a->stalled_steps<120-0.00001) return;
    a->stalled_steps=0;
    hle_log("fps: rider %d stuck on the ground while pedalling for 2 s: the collision recovery\n", slot);
    extern void func_00155E78(void *,int);
    func_00155E78(r+0x3FD0,2);
}
/* A zero-time impact clears the force accumulator (00231C10) for the rest of
   the retail 1/60 step. At high rates the clear lasts for the rest of that
   legacy field: later updates in the same field also run without force.
   Clock stamps are interval ends: an exact boundary still belongs to the
   preceding integration interval, not the following field. */
void port_fps_force_window(void *body,int clear)
{
    if (!port_delta_clock()) return;
    Air *a=air_for(body);
    int field=D_0029E3D8-(phase==0);
    if (clear) {
        a->force_field=field;
        a->force_valid=1;
    } else if (a->force_valid && a->force_field==field) {
        float *force=(float *)((char *)body+0xE0);
        for (int i=0;i<8;i++) force[i]=(i==3 || i==7) ? 1.0f : 0.0f;
    }
}
float port_counter_fields(int counter) { return port_delta_clock() ? counter+(float)phase/port_fps() : (float)counter; }
/* A whole-field counter the game advanced this update (the race clock), in seconds: a field is
   1/60 s on the delta-time clock (wall-clock steps), plus the part of a field the clock is past
   it, and 1001/60000 s on the original clock (a deterministic run at 60, the NTSC grid). Online
   snapshots carry this (finding 114). */
double port_field_seconds(int counter)
{
    return port_delta_clock() ? (counter+phase/port_fps())/60.0 : counter*(1001.0/60000.0);
}
/* Game time still owed to wall time (an online race catching up), in seconds. */
double port_fps_behind(void) { return wall_mode && owed>0 ? owed/60.0 : 0; }

/* prev comes from the preceding real animation/simulation step, including at
   crash entry (001B1C20). Its displacement already has the selected step's
   span: scaling it again would shorten the initial throw. */
void port_ragdoll_step(void *points, int count, int stride, float damping)
{
    float dt=port_step(), d=port_decay(damping), span=1, dt2=dt*dt;
    /* Verlet stores displacement, so variable steps must convert its previous
       span to this step. New/reinitialized groups already span the current dt. */
    typedef struct { void *points; unsigned serial; float dt; } History;
    static History history[128];
    unsigned index=0;
    while (index<128 && history[index].points!=points) index++;
    if (index==128) {
        for (index=0;index<128;index++)
            if (!history[index].points || clock_serial-history[index].serial>1) break;
        if (index==128) abort(); /* no silent history aliasing */
    }
    if (history[index].points==points && history[index].serial+1==clock_serial) {
        float previous=history[index].dt;
        span=dt/previous;
        if (dt!=previous) dt2=dt*(dt+previous)*0.5f;
    }
    history[index]=(History){points,clock_serial,dt};
    for (int i=0;i<count;i++) {
        /* Particle layout checked in 00239A80; points addresses particles[0]. */
        char *p=(char *)points+i*stride;
        float *pos=(float *)p, *prev=(float *)(p+0x10), *force=(float *)(p+0x20);
        for (int k=0;k<3;k++) {
            float old=pos[k];
            pos[k] += (pos[k]-prev[k]) * d * span + force[k] * (dt2*0.0002777778f);
            prev[k]=old;
        }
        prev[3]=pos[3]; /* retail CopyVector also preserves the fourth lane */
    }
}

/* dhd.log's "clock:" line (finding 139), after every 30 s of race with the race clock running
   (on with the perf: lines, DHD_PERF_LOG): the race clock's seconds against wall seconds (the game
   speed), the slowest and fastest whole second of it (a fast-forward shows as a fastest second
   above 1), the smallest and largest game step in fields (D_0029E400, the seconds a step), and the
   sound the mixer rendered against wall time. Measured between the frames on which the race
   clock's whole fields moved; a stretch over a quarter second without a move (a pause, the wait
   after the finish) counts for neither. Read-only. */
void port_clock_observe(void)
{
    extern uint64_t snd_test_position(void);
    static int on = -1, last_ticks;
    static double last_wall, wall, game, second_wall, second_game, slowest, fastest, sound;
    static float least = 1e9f, most;
    static uint32_t last_samples;
    if (on < 0) {
        const char *s = host_getenv("DHD_PERF_LOG");
        on = !s || strcmp(s, "0") != 0;
    }
    if (!on || !port_race_running(2)) {
        last_wall = 0;
        return;
    }
    double now = host_wall_seconds();
    int ticks = *(int *)GUEST(0x0843EEC8);
    uint32_t samples = (uint32_t)snd_test_position();
    float step = D_0029E400 * 60;
    if (!last_wall || ticks < last_ticks || (ticks == last_ticks && now - last_wall > 0.25)) {
        last_wall = now, last_ticks = ticks, last_samples = samples;
        return;
    }
    if (step < least) least = step;
    if (step > most) most = step;
    if (ticks > last_ticks) {
        double dt = now - last_wall, dg = (ticks - last_ticks) * (port_delta_clock() ? 1.0 / 60 : 1001.0 / 60000);
        if (!game) slowest = 1e9, fastest = 0;
        wall += dt, game += dg, second_wall += dt, second_game += dg;
        sound += (uint32_t)(samples - last_samples) / 48000.0;
        if (second_wall >= 1) {
            double speed = second_game / second_wall;
            if (speed < slowest) slowest = speed;
            if (speed > fastest) fastest = speed;
            second_wall = second_game = 0;
        }
        if (wall >= 30) {
            hle_log("clock: race %.0f s game x%.3f (seconds x%.3f-x%.3f), steps %.2f-%.2f fields, sound x%.3f\n",
                    wall, game / wall, slowest, fastest, least, most, sound / wall);
            wall = game = sound = 0;
            least = 1e9f, most = 0;
        }
        last_wall = now, last_ticks = ticks, last_samples = samples;
    }
}

/* DHD_FPS_TRACE=path: read-only race observer for port/tools/test_fps_race.py. One row per
   rider per race tick, plus every real update through tick 360 in path.steps.csv (the first
   update carrying a tick comes earlier at high rates). DHD_FPS_STOP=N ends the game at tick N; the
   game also ends when the race does (port/tools/rate_drift.py).
   Raw offsets are shared with puppet.c and rider.h. The CSVs hold game-derived state. */
void port_fps_observe(void)
{
    static int init, last=-1;
    static FILE *out,*steps_out;
    if (!init) {
        init=1;
        const char *path=host_getenv("DHD_FPS_TRACE");
        if (path && *path) {
            out=fopen(path,"w");
            if (!out) abort();
            fprintf(out,"tick,hz,slot,x,y,z,speed,checkpoint,race_ticks,finished,finish_ticks,crash,attack,animation,animation_slot,particles,frame,contact_mask,air_age,gauge,boost\n");
            char steps_path[2048];
            snprintf(steps_path,sizeof steps_path,"%s.steps.csv",path);
            steps_out=fopen(steps_path,"w");
            if (!steps_out) abort();
            fprintf(steps_out,"tick,hz,slot,frame,x,y,z,speed\n");
        }
    }
    static int raced, still;
    if (!out) return;
    int tick=*(int *)GUEST(0x0843EEC8);
    /* The race is over (rate_drift.py: the finish times are in) when it ends (port_race_running),
       or when its clock stops (every rider in, the results wait) for 3600 updates (15 s at 240). */
    if ((raced && (tick==last ? ++still : (still=0))>3600) || !port_race_running(2)) {
        if (raced) {
            fclose(out);
            fclose(steps_out);
            out=NULL;
            host_exit(0);
        }
        return;
    }
    raced=1;
    if (tick<=360) {
        for (int i=0;i<10;i++) {
            unsigned char *r=(unsigned char *)GUEST(0x082AF2C0+i*0x7BD0);
            float *p=(float *)(r+0x4270),*v=(float *)(r+0x4180);
            fprintf(steps_out,"%d,%d,%d,%llu,%.9g,%.9g,%.9g,%.9g\n",tick,port_fps(),i,
                (unsigned long long)race_steps,p[0],p[1],p[2],sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]));
        }
        fflush(steps_out);
    }
    if (tick==last) return;
    last=tick;
    int particles=0;
    int index=*(int *)GUEST(0x0829E308);
    for (int i=0;i<64 && index>=0 && index<64;i++) {
        int n=*(int *)GUEST(0x0867E180+index*0x2860+0x2850);
        if (n>0 && n<100000) particles+=n;
        index=*(int *)GUEST(0x0871F980+index*12+8);
    }
    for (int i=0;i<10;i++) {
        unsigned char *r=(unsigned char *)GUEST(0x082AF2C0+i*0x7BD0);
        float *p=(float *)(r+0x4270), *v=(float *)(r+0x4180);
        float speed=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
        /* Read the owner's active clip, rather than a potentially stopped pedal
           parameter. The latter can stay at zero throughout a valid race. */
        extern int func_0012D300(void *);
        int animation_slot=19+func_0012D300(r);
        unsigned char *track=*(unsigned char **)(r+0x1E40+0x14+animation_slot*16+4);
        unsigned char *owner=*(unsigned char **)(r+0x1E40+0x14+3*16);
        if ((uintptr_t)owner>=0x08000000 && (uintptr_t)owner<0x0A000000) {
            unsigned char *list=*(unsigned char **)(owner+0x24);
            if ((uintptr_t)list>=0x08000000 && (uintptr_t)list<0x0A000000) {
                unsigned char *active_track=*(unsigned char **)(list+8);
                if ((uintptr_t)active_track>=0x08000000 && (uintptr_t)active_track<0x0A000000) {
                    track=active_track;
                    animation_slot=-1;
                }
            }
        }
        float anim=-1;
        if ((uintptr_t)track>=0x08000000 && (uintptr_t)track<0x0A000000) {
            int now=*(int *)GUEST(0x0829E160), start=*(int *)(track+0x14);
            int elapsed=track[0x28] ? now-start : start;
            anim=port_animation_fields((float)elapsed)* *(float *)(track+0x1C)+ *(float *)(track+0x18);
            unsigned char *range=*(unsigned char **)(track+4);
            if ((uintptr_t)range>=0x08000000 && (uintptr_t)range<0x0A000000) {
                float lo=*(float *)(range+4),hi=*(float *)(range+8);
                if (hi>lo) anim=(anim-lo)/(hi-lo);
            }
        }
        fprintf(out,"%d,%d,%d,%.8g,%.8g,%.8g,%.8g,%u,%d,%u,%d,%u,%u,%.8g,%d,%d,%llu,%u,%d,%.8g,%.8g\n",
                tick,port_fps(),i,p[0],p[1],p[2],speed,r[0x7990],*(int *)(r+0x7994),
                *(unsigned char *)GUEST(0x0843EF18+i),*(int *)GUEST(0x0843EEDC+4*i),
                (r[0x4098]>>2)&1,r[0x6390],anim,animation_slot,particles,(unsigned long long)race_steps,
                r[0x4414]|r[0x4494]<<2,*(short *)(r+0x4564),*(float *)(r+0x2E98),*(float *)(r+0x7A78));
    }
    fflush(out);
    const char *stop=host_getenv("DHD_FPS_STOP");
    if (stop && tick>=atoi(stop)) {
        fclose(out);
        fclose(steps_out);
        out=NULL;
        host_exit(0);
    }
}
