#ifndef DHD_FPS_H
#define DHD_FPS_H
#ifdef __cplusplus
extern "C" {
#endif
/* Simulation clock (port/hle/fps.c, finding 067). 60 retains the original NTSC path and
   arithmetic; online races run at the selected rate too (finding 114). */
int port_fps(void);
int port_fps_max(void);  /* 240, or DHD_FPS_MAX when lower */
int port_fps_high(void);
void port_fps_wall_mode(int enabled);
double port_frame_period(void);
double port_vblank_seconds(void);  /* the mixer's time for one VBLANK above 60 */
int port_input_stamp(int legacy_field);

/* Elapsed time of this real update, and its conversions of per-field constants. */
float port_step(void);
int port_whole_step(void);
float port_decay(float factor);
float port_blend(float rate);
float port_particle_sum(float factor);
void port_particle_rotation(float *cosine,float *sine);
int port_particle_dead(float age,float life_per_field);
float port_counter_fields(int counter);
double port_field_seconds(int counter);  /* a field counter as seconds, part of a field too */
double port_fps_behind(void);            /* game time owed to wall time above 60, seconds */

/* The game clock (00238A68/00238C00/00238D98) at high rates. */
void port_clock_reset(void);
void port_clock_wait(int *out);
void port_clock_step(void);

/* Animation timestamps in 1/1024-field units at high rates. */
int port_animation_step(void);
float port_animation_fields(float ticks);
int port_animation_scale(void);

/* Rider physics at high rates. */
int port_air_tick(void *body, int previous);
int port_air_first(void *body, int current);
float port_air_age(void *body, int current);
void port_ragdoll_step(void *points, int count, int stride, float damping);
void port_fps_force_window(void *body, int clear);
void port_fps_contact_recovery(void *body);

/* dhd.log's clock: lines (every VBLANK; finding 139): game time against wall time in a race. */
void port_clock_observe(void);

/* DHD_FPS_TRACE race observer (port/tools/test_fps_race.py). */
void port_fps_race_step(void);
void port_fps_observe(void);
#ifdef __cplusplus
}
#endif
#endif
