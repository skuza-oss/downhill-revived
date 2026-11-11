/* Puppet riders steered by their owners' snapshots (finding 202). A puppet rides on this game's
   own physics with its owner's pad, so its wheels follow this game's ground and it keeps riding
   between snapshots. Each snapshot says where the owner's rider was at its moment; the puppet
   compares that with where it itself was at the same moment (a short history of its own poses)
   and moves toward the difference over a short time, the part into the ground left to the
   wheels' contacts. The prediction to the present is therefore the game's physics, not a
   straight line: through a landing, a dip or a turn the puppet does what a rider does there.

   A pose is the rigid body's position, velocity and orientation (three rows, each a world-space
   axis of the rider; world z is up). Pure functions on plain arrays; unit test test_puppet_sync.c.

   Sources: Glenn Fiedler, "Networked Physics" (gafferongames.com, 2014-2015: state
   synchronization, the receiver simulates and the sender's state is applied with smoothing);
   Rocket League's GDC 2018 talk (Jared Cone, "It IS Rocket Science!": other cars simulated with
   their last inputs, corrections when the server's state arrives). The comparison against the
   puppet's own past is this project's; no code was taken. */
#ifndef DHD_PUPPET_SYNC_H
#define DHD_PUPPET_SYNC_H
#include <math.h>
#include <string.h>

#define SYNC_TAU_POS 0.12f     /* s: a position error is made up to 1/e in this time */
#define SYNC_TAU_VEL 0.10f     /* s: the same for velocity */
#define SYNC_TAU_ROT 0.10f     /* s: and orientation */
#define SYNC_FAST 3.0f         /* units: an error this large is made up twice as fast, three times as
                                  large three times as fast (a far rider catches up, a near one glides) */
#define SYNC_SLIDE 30.0f       /* units a second: a correction never moves the rider faster than this
                                  on top of its riding (0.5 units a step at 60), so it never reads as
                                  a jump; riders ride about 95 a second */
#define SYNC_SLIDE_GAIN 6.0f   /* per second: the slide limit grows by this times the error, so a large
                                  error (a sender catching up at twice real time) is followed */
#define SYNC_SLIDE_ACCEL 600.0f /* units a second each second: how fast a correction's own speed may
                                  change, so it eases in and out instead of kicking (10 a step at 60) */
#define SYNC_PROJECT 0.3       /* s: a velocity difference found by a snapshot is carried to now (it
                                  has been moving the puppet away since), at most this long */
#define SYNC_COAST 3.0         /* s without a snapshot: the puppet stops riding on its owner's last
                                  inputs (puppet.c). A stopped game says so at once (HOLD); this is
                                  for a link that has gone quiet, where riding on is the better guess
                                  (live: a one-second SFU stall at the start braked the pack) */
#define SYNC_GUARD 0.6f        /* a puppet under this share of its owner's speed (a recent snapshot,
                                  no crash) was stopped by this game's physics: it takes its owner's */
#define SYNC_GUARD_AGE 0.4     /* s: how recent */
#define SYNC_RESYNC_AGE 0.4    /* s: an older snapshot never makes a jump (puppet.c steer) */
#define SYNC_CATCHUP 1.25f     /* a sender's race clock this much faster than the shared one is catching
                                  up (up to twice, finding 139): its puppet glides after its snapshots */
#define SYNC_CATCHUP_END 1.1f  /* ... until under this */
#define SYNC_FOLLOW_TAU 0.05f  /* s: the glide toward its rider meanwhile */
#define SYNC_START_WAIT 2.0   /* s after GO: a puppet whose snapshots have not begun is this game's AI */
#define SYNC_RESYNC 12.0f      /* game units (about 0.13 s of riding): a larger error is a jump */
#define SYNC_CARRY_MAX 0.25    /* s: a re-sync carries a snapshot at most this far */
enum { SYNC_HISTORY = 512 };   /* steps: about 2 s at 240 steps a second */
#define SYNC_CLOCK_SLACK 0.05  /* s of a race clock's lag behind the wall clock that are normal */

/* Online the race clock keeps the time it could not step (below 30 frames a second, a hitch)
   and catches up later (finding 139), so meanwhile it trails the wall clock by that much (owed,
   seconds). A snapshot's stamp is the moment its pose happened on the shared clock: the sender's
   GO there plus its race time plus what its clock trails beyond the normal pacing lag; a receiver
   places it on its own race clock less what its own clock trails. A game that falls behind then
   shows the others where they are, and is shown where it is, not seconds in the past or future. */
static inline double sync_clock_excess(double owed) { return owed > SYNC_CLOCK_SLACK ? owed - SYNC_CLOCK_SLACK : 0; }

typedef struct SyncPose {
    float pos[3], vel[3];
    float rot[3][3];
} SyncPose;

typedef struct SyncSample {
    double t;
    SyncPose pose;
    float applied_pos[3], applied_vel[3], applied_rot[3];  /* corrections applied up to here */
} SyncSample;

typedef struct PuppetSync {
    SyncSample hist[SYNC_HISTORY];
    int head, count;                                       /* newest at head - 1 */
    float applied_pos[3], applied_vel[3], applied_rot[3];  /* running sums of applied corrections */
    float pending_pos[3], pending_vel[3], pending_rot[3];  /* still to apply */
    float slide[3];                                        /* the position correction's speed, last step */
} PuppetSync;

static inline float sync_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline float sync_len(const float *a) { return sqrtf(sync_dot(a, a)); }

/* The rotation of angle |w| about w (Rodrigues), acting on column vectors. */
static inline void sync_axis_angle(const float w[3], float q[3][3])
{
    float n = sync_len(w);
    memset(q, 0, 9 * sizeof(float));
    q[0][0] = q[1][1] = q[2][2] = 1;
    if (!(n > 1e-7f) || !isfinite(n)) return;
    float x = w[0] / n, y = w[1] / n, z = w[2] / n, c = cosf(n), s = sinf(n), C = 1 - c;
    q[0][0] = c + x * x * C,     q[0][1] = x * y * C - z * s, q[0][2] = x * z * C + y * s;
    q[1][0] = y * x * C + z * s, q[1][1] = c + y * y * C,     q[1][2] = y * z * C - x * s;
    q[2][0] = z * x * C - y * s, q[2][1] = z * y * C + x * s, q[2][2] = c + z * z * C;
}

/* Every row turned by the world rotation w (|w| radians about w), then made orthonormal again. */
static inline void sync_turn(float rot[3][3], const float w[3])
{
    float q[3][3], out[3][3];
    sync_axis_angle(w, q);
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) out[i][k] = q[k][0] * rot[i][0] + q[k][1] * rot[i][1] + q[k][2] * rot[i][2];
    for (int i = 0; i < 3; i++) {  /* Gram-Schmidt, rows */
        for (int j = 0; j < i; j++) {
            float d = sync_dot(out[i], out[j]);
            for (int k = 0; k < 3; k++) out[i][k] -= d * out[j][k];
        }
        float n = sync_len(out[i]);
        for (int k = 0; k < 3; k++) rot[i][k] = n > 0 ? out[i][k] / n : out[i][k];
    }
}

/* The world rotation (as a rotation vector) that turns the rows `from` into the rows `to`:
   Q = to^T from (rows orthonormal), its axis and angle. */
static inline void sync_rotation_between(const float from[3][3], const float to[3][3], float w[3])
{
    float q[3][3];
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++) q[a][b] = to[0][a] * from[0][b] + to[1][a] * from[1][b] + to[2][a] * from[2][b];
    float v[3] = {q[2][1] - q[1][2], q[0][2] - q[2][0], q[1][0] - q[0][1]};
    float s = 0.5f * sync_len(v), c = 0.5f * (q[0][0] + q[1][1] + q[2][2] - 1);
    float angle = atan2f(s, c);
    if (!(s > 1e-6f)) {  /* none (or a half turn, which no rider makes between two snapshots) */
        w[0] = w[1] = w[2] = 0;
        return;
    }
    for (int k = 0; k < 3; k++) w[k] = v[k] / (2 * s) * angle;
}

/* A snapshot's pose carried h seconds forward in a straight line, turning with its angular
   velocity (a re-sync's starting pose). */
static inline void sync_carry(const SyncPose *from, const float spin[3], float h, SyncPose *out)
{
    float w[3] = {spin[0] * h, spin[1] * h, spin[2] * h};
    *out = *from;
    for (int k = 0; k < 3; k++) out->pos[k] = from->pos[k] + from->vel[k] * h;
    sync_turn(out->rot, w);
}

static inline void sync_reset(PuppetSync *s) { memset(s, 0, sizeof *s); }

static inline void sync_record(PuppetSync *s, double t, const SyncPose *pose);

/* A jump to a snapshot (its pose at race time t, carried to the present `now`): the history
   starts afresh with the straight carry between them, every 1/120 s, so the snapshots still on
   their way (taken between t and now) are compared with the path the puppet was given. */
static inline void sync_restart(PuppetSync *s, double t, const SyncPose *owner, const float spin[3], double now)
{
    sync_reset(s);
    for (double at = t; at < now - 1e-6; at += 1 / 120.0) {
        SyncPose p;
        sync_carry(owner, spin, (float)(at - t), &p);
        sync_record(s, at, &p);
    }
}

/* The pose after a step at race time t (after everything that moves a rider in the step). */
static inline void sync_record(PuppetSync *s, double t, const SyncPose *pose)
{
    if (s->count && t <= s->hist[(s->head + SYNC_HISTORY - 1) % SYNC_HISTORY].t) return;
    SyncSample *e = &s->hist[s->head];
    e->t = t, e->pose = *pose;
    memcpy(e->applied_pos, s->applied_pos, sizeof e->applied_pos);
    memcpy(e->applied_vel, s->applied_vel, sizeof e->applied_vel);
    memcpy(e->applied_rot, s->applied_rot, sizeof e->applied_rot);
    s->head = (s->head + 1) % SYNC_HISTORY;
    if (s->count < SYNC_HISTORY) s->count++;
}

/* The puppet as it was at time t: between the two samples around it (orientation: the nearer
   one's, the corrections' sums: the earlier one's). 0 when t is outside the history. */
static inline int sync_at(const PuppetSync *s, double t, SyncSample *out)
{
    if (!s->count) return 0;
    const SyncSample *newest = &s->hist[(s->head + SYNC_HISTORY - 1) % SYNC_HISTORY];
    const SyncSample *oldest = &s->hist[(s->head + SYNC_HISTORY - s->count) % SYNC_HISTORY];
    if (t < oldest->t || t > newest->t) return 0;
    for (int i = 1; i <= s->count; i++) {
        const SyncSample *a = &s->hist[(s->head + SYNC_HISTORY - i) % SYNC_HISTORY];
        if (a->t > t) continue;
        const SyncSample *b = i > 1 ? &s->hist[(s->head + SYNC_HISTORY - i + 1) % SYNC_HISTORY] : a;
        float f = b->t > a->t ? (float)((t - a->t) / (b->t - a->t)) : 0;
        *out = f < 0.5f ? *a : *b;
        memcpy(out->applied_pos, a->applied_pos, sizeof out->applied_pos);
        memcpy(out->applied_vel, a->applied_vel, sizeof out->applied_vel);
        memcpy(out->applied_rot, a->applied_rot, sizeof out->applied_rot);
        out->t = t;
        for (int k = 0; k < 3; k++) {
            out->pose.pos[k] = a->pose.pos[k] + (b->pose.pos[k] - a->pose.pos[k]) * f;
            out->pose.vel[k] = a->pose.vel[k] + (b->pose.vel[k] - a->pose.vel[k]) * f;
        }
        return 1;
    }
    return 0;
}

/* A snapshot (its pose `owner` at race time t) arrived. What is still to correct: how far the
   owner was from where this puppet was at t, less what was applied since t (that part was already
   on its way). When the puppet has no history at t, the snapshot carried to the present
   (`carried`) is compared with the present pose (`now`). Returns the distance still to correct
   (units). */
static inline float sync_measure(PuppetSync *s, double t, const SyncPose *owner, const SyncPose *carried,
                                 const SyncPose *now, double ahead)
{
    SyncSample then;
    float rot[3];
    if (!sync_at(s, t, &then)) {
        owner = carried;
        then.pose = *now;
        memcpy(then.applied_pos, s->applied_pos, sizeof then.applied_pos);
        memcpy(then.applied_vel, s->applied_vel, sizeof then.applied_vel);
        memcpy(then.applied_rot, s->applied_rot, sizeof then.applied_rot);
    }
    sync_rotation_between(then.pose.rot, owner->rot, rot);
    for (int k = 0; k < 3; k++) {
        s->pending_pos[k] = owner->pos[k] - then.pose.pos[k] - (s->applied_pos[k] - then.applied_pos[k]);
        s->pending_vel[k] = owner->vel[k] - then.pose.vel[k] - (s->applied_vel[k] - then.applied_vel[k]);
        s->pending_rot[k] = rot[k] - (s->applied_rot[k] - then.applied_rot[k]);
    }
    /* the velocity difference has moved the puppet further off since t */
    float h = (float)(ahead > SYNC_PROJECT ? SYNC_PROJECT : ahead > 0 ? ahead : 0);
    for (int k = 0; k < 3; k++) s->pending_pos[k] += s->pending_vel[k] * h;
    return sync_len(s->pending_pos);
}

/* One step of dt seconds: a part of what is still to correct, applied to pose. ground: the
   normal of the ground under the wheels that touch it (NULL in the air): what would push the
   rider into it is left out, the wheels decide its height there. */
static inline void sync_step(PuppetSync *s, float dt, const float *ground, SyncPose *pose)
{
    if (!(dt > 0)) return;
    float speed = 1 + sync_len(s->pending_pos) / SYNC_FAST;
    float fp = 1 - expf(-dt * speed / SYNC_TAU_POS), fv = 1 - expf(-dt * speed / SYNC_TAU_VEL), fr = 1 - expf(-dt / SYNC_TAU_ROT);
    float dp[3], dv[3], dr[3];
    for (int k = 0; k < 3; k++) dp[k] = s->pending_pos[k] * fp, dv[k] = s->pending_vel[k] * fv, dr[k] = s->pending_rot[k] * fr;
    if (ground) {
        float n = sync_len(ground);
        if (n > 0) {
            float u[3] = {ground[0] / n, ground[1] / n, ground[2] / n}, into = sync_dot(dp, u), down = sync_dot(dv, u);
            /* into the ground: not applied and not owed (the ground holds the rider) */
            if (into < 0)
                for (int k = 0; k < 3; k++) dp[k] -= into * u[k], s->pending_pos[k] -= into / fp * u[k];
            if (down < 0)
                for (int k = 0; k < 3; k++) dv[k] -= down * u[k], s->pending_vel[k] -= down / fv * u[k];
        }
    }
    float slide = sync_len(dp) / dt, most = SYNC_SLIDE + SYNC_SLIDE_GAIN * sync_len(s->pending_pos);
    if (slide > most)
        for (int k = 0; k < 3; k++) dp[k] *= most / slide;
    /* the correction's speed changes at most SYNC_SLIDE_ACCEL a second */
    float change[3], step = SYNC_SLIDE_ACCEL * dt;
    for (int k = 0; k < 3; k++) change[k] = dp[k] / dt - s->slide[k];
    float c = sync_len(change);
    for (int k = 0; k < 3; k++) {
        if (c > step) change[k] *= step / c;
        s->slide[k] += change[k];
        dp[k] = s->slide[k] * dt;
    }
    for (int k = 0; k < 3; k++) {
        pose->pos[k] += dp[k], pose->vel[k] += dv[k];
        s->pending_pos[k] -= dp[k], s->pending_vel[k] -= dv[k], s->pending_rot[k] -= dr[k];
        s->applied_pos[k] += dp[k], s->applied_vel[k] += dv[k], s->applied_rot[k] += dr[k];
    }
    sync_turn(pose->rot, dr);
}
#endif
