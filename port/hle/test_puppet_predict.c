/* The puppet predictor (puppet_predict.h, finding 169): a straight line, a turn, a correction that
   starts where the rider was shown and fades, and a respawn that snaps. */
#include "puppet_predict.h"
#include <assert.h>
#include <stdio.h>

static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

/* the rider's rows for heading a (radians) about the world y axis */
static void heading(float a, float rot[3][4])
{
    memset(rot, 0, 12 * sizeof(float));
    rot[0][0] = cosf(a), rot[0][2] = -sinf(a);
    rot[1][1] = 1;
    rot[2][0] = sinf(a), rot[2][2] = cosf(a);
}

int main(void)
{
    PuppetPose out, was, shown;
    float pos[3] = {10, 2, -5}, vel[3] = {30, -1, 4}, still[3] = {0, 0, 0}, rot[3][4];

    /* straight on: position along the velocity, orientation unchanged */
    heading(0.3f, rot);
    predict_pose(pos, rot, vel, still, 0.05f, &out);
    assert(near(out.pos[0], 11.5f, 1e-4f) && near(out.pos[1], 1.95f, 1e-4f) && near(out.pos[2], -4.8f, 1e-4f));
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) assert(near(out.rot[i][k], rot[i][k], 1e-6f));

    /* a turn at 2 rad/s about y: after 0.1 s every row has turned 0.2 rad, as the heading would */
    {
        float spin[3] = {0, 2, 0}, want[3][4];
        PuppetPose a;
        heading(0.3f, rot);
        predict_pose(pos, rot, vel, spin, 0.1f, &a);
        heading(0.3f + 0.2f, want);
        int same = 1, mirrored = 1;
        heading(0.3f - 0.2f, rot);  /* the other sense, to be sure the test tells them apart */
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) {
                same &= near(a.rot[i][k], want[i][k], 1e-4f);
                mirrored &= near(a.rot[i][k], rot[i][k], 1e-4f);
            }
        assert(same && !mirrored);
        /* rows stay orthonormal */
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                float d = a.rot[i][0] * a.rot[j][0] + a.rot[i][1] * a.rot[j][1] + a.rot[i][2] * a.rot[j][2];
                assert(near(d, i == j, 1e-5f));
            }
    }

    /* a correction: a newer snapshot moved the prediction 0.6 units and 0.1 rad; the rider is shown
       where it was, then fades to the new prediction */
    {
        PuppetCorrection c;
        heading(0.0f, rot);
        predict_pose(pos, rot, vel, still, 0, &was);
        float moved[3] = {pos[0] + 0.6f, pos[1], pos[2]};
        heading(0.1f, rot);
        PuppetPose raw;
        predict_pose(moved, rot, vel, still, 0, &raw);
        correction_clear(&c);
        assert(correction_start(&c, &was, &raw));
        correction_apply(&c, &raw, &shown);
        for (int k = 0; k < 3; k++) assert(near(shown.pos[k], was.pos[k], 1e-4f));
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) assert(near(shown.rot[i][k], was.rot[i][k], 1e-4f));
        float before = 0.6f;
        for (int step = 0; step < 30; step++) {  /* half a second at 60 */
            correction_decay(&c, 1 / 60.0f);
            correction_apply(&c, &raw, &shown);
            float off = fabsf(shown.pos[0] - raw.pos[0]);
            assert(off <= before + 1e-6f);
            before = off;
        }
        assert(before < 0.6f * 0.01f);  /* exp(-0.5/0.08) */
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) assert(near(shown.rot[i][k], raw.rot[i][k], 2e-3f));
    }

    /* a crash and respawn: the snapshot jumps far away; nothing fades, the rider is shown there */
    {
        PuppetCorrection c;
        float respawn[3] = {pos[0] - 40, pos[1] + 3, pos[2]};
        PuppetPose raw;
        heading(0.0f, rot);
        predict_pose(pos, rot, vel, still, 0, &was);
        predict_pose(respawn, rot, still, still, 0, &raw);
        correction_clear(&c);
        assert(!correction_start(&c, &was, &raw) && !c.on);
        correction_apply(&c, &raw, &shown);
        for (int k = 0; k < 3; k++) assert(shown.pos[k] == raw.pos[k]);
    }

    /* no spin and a zero or bad horizon change nothing */
    {
        float bad[3] = {NAN, 0, 0};
        heading(0.7f, rot);
        predict_pose(pos, rot, still, bad, 0.1f, &out);
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) assert(out.rot[i][k] == rot[i][k]);
    }
    puts("puppet prediction: OK");
    return 0;
}
