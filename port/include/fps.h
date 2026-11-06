#ifndef DHD_FPS_H
#define DHD_FPS_H
#ifdef __cplusplus
extern "C" {
#endif
/* The game clock (port/hle/fps.c, findings 067, 114, 139): paced play steps by the real time
   since the last frame (at most two fields) at every rate, offline and online; a deterministic
   run at 60 keeps the original NTSC path and arithmetic. */
int port_fps(void);      /* the selected rate, 30 to port_fps_max */
int port_fps_max(void);  /* 240, or DHD_FPS_MAX when lower */
int port_delta_clock(void);           /* the delta-time clock's code paths are on */
void port_clock_wall(int enabled);    /* paced: steps follow the wall clock (kernel_init) */
void port_clock_rebase(void);         /* a wait that is no game time: the next step starts from now */
double port_frame_period(void);
double port_vblank_seconds(void);  /* the mixer's time for one VBLANK on the delta-time clock */
int port_input_stamp(int legacy_field);

/* Elapsed time of this real update, and its conversions of per-field constants. */
float port_step(void);
int port_whole_step(void);
float port_step_seconds(void);  /* the delta-time clock's exact 1/func_00238F30(): this step's seconds */
float port_step_rate(void);     /* the delta-time clock's exact (float)func_00238F30() */
float port_decay(float factor);
float port_blend(float rate);
float port_particle_sum(float factor);
void port_particle_rotation(float *cosine,float *sine);
int port_particle_dead(float age,float life_per_field);
int port_count_down(int value);            /* a countdown by whole fields, stopping at 0 */
int port_count_reached(int value, int mark); /* an up-counter reached mark in this update */
float port_counter_fields(int counter);
double port_field_seconds(int counter);  /* a field counter as seconds, part of a field too */
double port_fps_behind(void);            /* game time owed to wall time (online catch-up), seconds */

/* The game clock (00238A68/00238C00/00238D98) on the delta-time clock. */
void port_clock_reset(void);
void port_clock_wait(int *out);
void port_clock_step(void);

/* The field grid of an offline race and its replay (finding 142, port/hle/replay.c). */
enum { GRID_OFF, GRID_WHOLE, GRID_RECORD, GRID_PLAY };
void port_clock_grid(int mode);     /* the race asks for the next step's grid, every update */
int port_clock_recording(void);     /* the step is a recorded race's (GRID_RECORD) */
int port_clock_code(void);          /* this update's step in 1/120 field */
void port_clock_force(int code);    /* a replay: this update's step is the recorded code */
void port_replay_race_phase(int phase);
int port_replay_race(void *recorder);
void port_replay_frame(void *recorder);

/* Animation timestamps in 1/1024-field units on the delta-time clock. */
int port_animation_step(void);
float port_animation_fields(float ticks);
int port_animation_scale(void);

/* Rider physics on the delta-time clock. */
int port_air_tick(void *body, int previous);
int port_air_first(void *body, int current);
float port_air_age(void *body, int current);
void port_ragdoll_step(void *points, int count, int stride, float damping);
void port_fps_force_window(void *body, int clear);
void port_fps_contact_recovery(void *body);

/* Where the game is (finding 159). A race is on while the race world's update (func_0016C0C8, run by
   the race's loop and its load) has run since the front end's last frame (port_shell_frame), paused
   too, and it is not the title's
   attract demo (mode 10, D_004B25A0 +0x10C). Not the shell's screen id D_0029DED8: a race keeps the
   id of the screen that started it, 9 (GET READY) on most routes but 22 for CONTINUE SAVED GAME; and
   not the race phase alone: D_0043F348 keeps its last value in the menus after a race. The race
   phase then says which part: 5 the load and the flight over the track, 1 the countdown, 2-4 the
   race to its results. */
enum { PORT_MODE_ATTRACT = 10 };
static inline int port_race_rule(int race_loop, int mode, int phase, int first_phase)
{
    return race_loop && mode != PORT_MODE_ATTRACT && phase >= first_phase && phase <= 4;
}
void port_loop_frame(int race);           /* 1 from the race world's update, 0 from the front end's frame */
int port_in_race(void);                   /* a race's loop runs, any phase (not the attract demo) */
int port_race_running(int first_phase);   /* port_race_rule now: the phase first_phase (1 or 2) to 4 */

/* dhd.log's clock: lines (every VBLANK; finding 139): game time against wall time in a race. */
void port_clock_observe(void);

/* DHD_FPS_TRACE race observer (port/tools/test_fps_race.py). */
void port_fps_race_step(void);
void port_fps_observe(void);
#ifdef __cplusplus
}
#endif
#endif
