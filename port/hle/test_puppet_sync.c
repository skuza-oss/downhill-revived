/* Puppets steered by snapshots (puppet_sync.h, finding 202): rotations, the re-sync's straight
   carry, the error measured against the puppet's own past, corrections already under way, the
   ground holding the rider, and convergence at any step size. */
#include "puppet_sync.h"
#include <assert.h>
#include <stdio.h>

static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

/* the rider's rows for heading a (radians) about the world z axis (up) */
static void heading(float a, float rot[3][3])
{
    memset(rot, 0, 9 * sizeof(float));
    rot[0][0] = cosf(a), rot[0][1] = sinf(a);
    rot[1][0] = -sinf(a), rot[1][1] = cosf(a);
    rot[2][2] = 1;
}

/* a puppet riding along x at 95 units a second from x = 0, sampled every dt from t = 0 to `until` */
static void ride(PuppetSync *s, double dt, double until)
{
    sync_reset(s);
    for (double t = 0; t <= until + 1e-9; t += dt) {
        SyncPose p = {{(float)(95 * t), 0, 0}, {95, 0, 0}, {{0}}};
        heading(0, p.rot);
        sync_record(s, t, &p);
    }
}

int main(void)
{
    /* a turn and its measurement are each other's inverse */
    {
        float a[3][3], b[3][3], w[3], want[3] = {0.05f, -0.2f, 0.3f};
        heading(0.4f, a);
        memcpy(b, a, sizeof b);
        sync_turn(b, want);
        sync_rotation_between(a, b, w);
        for (int k = 0; k < 3; k++) assert(near(w[k], want[k], 1e-4f));
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) assert(near(sync_dot(b[i], b[j]), i == j, 1e-5f));
        float none[3];
        sync_rotation_between(a, a, none);
        for (int k = 0; k < 3; k++) assert(none[k] == 0);
    }

    /* the re-sync's carry: along the velocity, turning with the angular velocity in its sense */
    {
        SyncPose from = {{10, 2, -5}, {30, -1, 4}, {{0}}}, out;
        float spin[3] = {0, 0, 2}, want[3][3];
        heading(0.3f, from.rot);
        sync_carry(&from, spin, 0.1f, &out);
        assert(near(out.pos[0], 13, 1e-4f) && near(out.pos[1], 1.9f, 1e-4f) && near(out.pos[2], -4.6f, 1e-4f));
        heading(0.5f, want);
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) assert(near(out.rot[i][k], want[i][k], 1e-4f));
    }

    static PuppetSync s;
    /* the owner was 0.5 units ahead 80 ms ago: that is what is still to correct, whatever the
       steps between (60, 30, 144 a second), and the puppet's present pose does not enter */
    for (int rate = 0; rate < 3; rate++) {
        double dt = 1.0 / (rate == 0 ? 60 : rate == 1 ? 30 : 144);
        ride(&s, dt, 2.0);
        SyncPose owner = {{(float)(95 * 1.92) + 0.5f, 0, 0}, {95, 0, 0}, {{0}}}, now = {{999, 999, 999}, {0, 0, 0}, {{0}}};
        heading(0, owner.rot);
        heading(1, now.rot);
        float e = sync_measure(&s, 1.92, &owner, &owner, &now, 0);
        assert(near(e, 0.5f, 1e-3f) && near(s.pending_pos[0], 0.5f, 1e-3f) && near(s.pending_vel[0], 0, 1e-4f));
        for (int k = 0; k < 3; k++) assert(near(s.pending_rot[k], 0, 1e-5f));
    }

    /* a velocity difference found 0.1 s ago has moved the puppet that much further off since */
    {
        ride(&s, 1 / 60.0, 2.0);
        SyncPose owner = {{(float)(95 * 1.9), 0, 0}, {100, 0, 0}, {{0}}}, now = {{0}, {0}, {{0}}};
        heading(0, owner.rot);
        heading(0, now.rot);
        sync_measure(&s, 1.9, &owner, &owner, &now, 0.1);
        assert(near(s.pending_vel[0], 5, 1e-3f) && near(s.pending_pos[0], 0.5f, 1e-3f));
        sync_measure(&s, 1.9, &owner, &owner, &now, 5.0);  /* never more than SYNC_PROJECT */
        assert(near(s.pending_pos[0], 5 * (float)SYNC_PROJECT, 1e-3f));
    }

    /* corrections applied since the snapshot's moment are not owed again */
    {
        ride(&s, 1 / 60.0, 1.0);
        SyncPose owner = {{95 * 0.9f + 1.0f, 0, 0}, {95, 0, 0}, {{0}}}, pose = {{95, 0, 0}, {95, 0, 0}, {{0}}};
        heading(0, owner.rot);
        heading(0, pose.rot);
        sync_measure(&s, 0.9, &owner, &owner, &pose, 0);
        double t = 1.0;
        float applied = 0;
        for (int i = 0; i < 6; i++) {  /* 0.1 s of steps, each recorded with what it moved */
            float before = pose.pos[0];
            sync_step(&s, 1 / 60.0f, NULL, &pose);
            applied += pose.pos[0] - before;
            t += 1 / 60.0;
            pose.pos[0] += 95 / 60.0f;
            sync_record(&s, t, &pose);
        }
        assert(applied > 0.4f && applied < 1.0f);
        /* the same snapshot measured again: only what is left */
        float left = sync_measure(&s, 0.9, &owner, &owner, &pose, 0);
        assert(near(left, 1.0f - applied, 1e-3f));
        /* a newer snapshot that agrees with where the puppet was at its moment: what was applied
           after that moment overshot, and is taken back */
        SyncPose agrees = {{0, 0, 0}, {95, 0, 0}, {{0}}};
        SyncSample then;
        assert(sync_at(&s, 1.05, &then));
        memcpy(agrees.pos, then.pose.pos, sizeof agrees.pos);
        heading(0, agrees.rot);
        sync_measure(&s, 1.05, &agrees, &agrees, &pose, 0);
        assert(near(s.pending_pos[0], -(s.applied_pos[0] - then.applied_pos[0]), 1e-4f) && s.pending_pos[0] < 0);
    }

    /* the ground: what would push the rider into it is neither applied nor owed; up is applied */
    {
        float up[3] = {0, 0, 2};
        SyncPose pose = {{0, 0, 0}, {0, 0, 0}, {{0}}};
        heading(0, pose.rot);
        sync_reset(&s);
        s.pending_pos[0] = 1, s.pending_pos[2] = -0.8f, s.pending_vel[2] = -3;
        sync_step(&s, 1 / 60.0f, up, &pose);
        assert(pose.pos[2] == 0 && pose.vel[2] == 0 && pose.pos[0] > 0);
        assert(near(s.pending_pos[2], 0, 1e-6f) && near(s.pending_vel[2], 0, 1e-6f));
        s.pending_pos[2] = 0.8f;
        sync_step(&s, 1 / 60.0f, up, &pose);
        assert(pose.pos[2] > 0);
    }

    /* convergence: 1 s of steps at 30, 60 or 144 a second makes up all but a trace at every rate;
       a large error slides no faster than SYNC_SLIDE, a small one goes faster than a plain
       exponential would */
    for (int rate = 0; rate < 3; rate++) {
        int n = rate == 0 ? 60 : rate == 1 ? 30 : 144;
        SyncPose pose = {{0, 0, 0}, {0, 0, 0}, {{0}}};
        heading(0, pose.rot);
        sync_reset(&s);
        s.pending_pos[1] = 6, s.pending_rot[2] = 0.2f;
        float speed = 0;
        for (int i = 0; i < n; i++) {
            float before = pose.pos[1];
            sync_step(&s, 1.0f / n, NULL, &pose);
            float now = (pose.pos[1] - before) * n;  /* the correction's speed: never a kick */
            assert(now <= SYNC_SLIDE + SYNC_SLIDE_GAIN * 6 + 1e-3f && fabsf(now - speed) <= SYNC_SLIDE_ACCEL / n + 1e-2f);
            speed = now;
        }
        assert(near(pose.pos[1], 6, 0.01f) && near(s.pending_rot[2], 0, 0.001f));
        sync_reset(&s);
        pose.pos[1] = 0;
        s.pending_pos[1] = 1;
        for (int i = 0; i < n / 4; i++) sync_step(&s, 1.0f / n, NULL, &pose);
        assert(pose.pos[1] > 0.6f && pose.pos[1] < 1.05f);
    }

    /* outside the history the snapshot carried to the present is compared with the present */
    {
        ride(&s, 1 / 60.0, 1.0);
        SyncPose owner = {{50, 0, 0}, {95, 0, 0}, {{0}}}, carried = {{51, 0, 0}, {95, 0, 0}, {{0}}},
                 now = {{48, 0, 0}, {95, 0, 0}, {{0}}};
        heading(0, owner.rot);
        heading(0, carried.rot);
        heading(0, now.rot);
        assert(near(sync_measure(&s, 5.0, &owner, &carried, &now, 0), 3, 1e-4f));
        assert(near(sync_measure(&s, -1.0, &owner, &carried, &now, 0), 3, 1e-4f));
    }

    /* after a jump the snapshots still on their way (taken between the jump's snapshot and the
       jump) find the straight path the puppet was given, not a difference of their age */
    {
        SyncPose owner = {{0, 0, 0}, {95, 0, 0}, {{0}}}, pose;
        float still[3] = {0, 0, 0};
        heading(0, owner.rot);
        sync_restart(&s, 10.0, &owner, still, 10.15);
        sync_carry(&owner, still, 0.15f, &pose);
        sync_record(&s, 10.15, &pose);
        SyncPose later = {{95 * 0.05f, 0, 0}, {95, 0, 0}, {{0}}};
        heading(0, later.rot);
        assert(near(sync_measure(&s, 10.05, &later, &later, &pose, 0), 0, 1e-3f));
    }
    /* an owner's reset (finding 216, the live case): riding at 76 units a second, then 364.8 units
       away standing; its next snapshot came in the same step, so the jump lies before the newest
       one's predecessor. Riding alone, or a jump before the last one used, is none. */
    {
        float pos[5][3] = {{-1047.7f, 1185.5f, 2209.8f}, {-1047.4f, 1186.2f, 2208.8f}, {-935.3f, 899.3f, 2404.1f},
                           {-935.3f, 899.2f, 2404.1f}, {-935.2f, 899.2f, 2404.1f}};
        float vel[5][3] = {{0, 77.5f, 0}, {0, 76.4f, 0}, {0, 0, 0}, {0, 1.2f, 0}, {0, 1.3f, 0}};
        double time[5] = {68.315, 68.332, 68.349, 68.365, 68.382};
        SyncMark m[5];
        for (int i = 0; i < 5; i++) m[i] = (SyncMark){time[i], pos[i], vel[i]};
        assert(sync_owner_jumped(m, 5, 68.332, 68.365));   /* the jump, then one more, in one step */
        assert(sync_owner_jumped(m, 4, 68.332, 68.349));   /* the jump alone */
        assert(!sync_owner_jumped(m, 5, 68.349, 68.382));  /* after it: standing */
        assert(!sync_owner_jumped(m, 2, 68.315, 68.332));  /* riding */
        assert(!sync_owner_jumped(m, 5, 68.332, 68.332));  /* nothing new */
    }
    puts("puppet sync: OK");
    return 0;
}
