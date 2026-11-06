/* Puppet prediction (finding 169): a remote rider shown at the present moment instead of in the
   past. From its newest snapshot the pose is carried forward by its velocity and angular velocity;
   when a newer snapshot moves the prediction, the difference is shown first and then fades
   (CORRECTION_TAU), so the rider never jumps, unless the difference is too large to be a
   misprediction (a respawn), where it snaps.

   A rider's orientation is three rows (+0x1C0 of the rigid body, 4 floats each); its angular
   velocity (+0xD0) is in world space. Each row is a world-space axis of the rider, so over h
   seconds every row turns by the rotation of angle |w| h about w (checked against recorded paths:
   finding 169, "Prediction"). Pure functions on plain arrays, unit-tested (test_puppet_predict.c). */
#ifndef DHD_PUPPET_PREDICT_H
#define DHD_PUPPET_PREDICT_H
#include <math.h>
#include <string.h>

enum { PREDICT_MAX_MS = 250 };              /* never carried further than this past a snapshot */
#define CORRECTION_TAU 0.08f               /* s: a correction fades to 1/e in this time */
#define CORRECTION_SNAP 8.0f               /* game units: a larger jump is shown at once */

typedef struct PuppetPose {
    float pos[3];
    float rot[3][4];
} PuppetPose;

/* The rotation of angle |w| h about w (Rodrigues), as a 3x3 matrix acting on column vectors. */
static inline void predict_axis_angle(const float w[3], float h, float q[3][3])
{
    float n = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]), a = n * h;
    memset(q, 0, 9 * sizeof(float));
    q[0][0] = q[1][1] = q[2][2] = 1;
    if (!(a > 1e-7f) || !isfinite(a)) return;
    float x = w[0] / n, y = w[1] / n, z = w[2] / n, c = cosf(a), s = sinf(a), C = 1 - c;
    q[0][0] = c + x * x * C,     q[0][1] = x * y * C - z * s, q[0][2] = x * z * C + y * s;
    q[1][0] = y * x * C + z * s, q[1][1] = c + y * y * C,     q[1][2] = y * z * C - x * s;
    q[2][0] = z * x * C - y * s, q[2][1] = z * y * C + x * s, q[2][2] = c + z * z * C;
}

/* The pose h seconds after (pos, rot) moving with vel (units a second) and spin (radians a second,
   world space). */
static inline void predict_pose(const float pos[3], const float rot[3][4], const float vel[3], const float spin[3],
                                float h, PuppetPose *out)
{
    float q[3][3];
    predict_axis_angle(spin, h, q);
    for (int k = 0; k < 3; k++) out->pos[k] = pos[k] + vel[k] * h;
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++)
            out->rot[i][k] = q[k][0] * rot[i][0] + q[k][1] * rot[i][1] + q[k][2] * rot[i][2];
        out->rot[i][3] = rot[i][3];
    }
}

/* What is added to the raw prediction while a correction fades: a position offset and a rotation
   applied to the rows' combination (shown = E raw, E 3x3). */
typedef struct PuppetCorrection {
    float pos[3];
    float rot[3][3];
    int on;
} PuppetCorrection;

static inline void correction_clear(PuppetCorrection *c)
{
    memset(c, 0, sizeof *c);
    c->rot[0][0] = c->rot[1][1] = c->rot[2][2] = 1;
}

/* The prediction moved from was (what was being shown) to raw: start showing was and fade to raw.
   Returns 0 (and clears) when the jump is too large to fade: a respawn, a teleport, a long loss. */
static inline int correction_start(PuppetCorrection *c, const PuppetPose *was, const PuppetPose *raw)
{
    float d2 = 0;
    for (int k = 0; k < 3; k++) {
        c->pos[k] = was->pos[k] - raw->pos[k];
        d2 += c->pos[k] * c->pos[k];
    }
    if (!(d2 <= CORRECTION_SNAP * CORRECTION_SNAP)) {
        correction_clear(c);
        return 0;
    }
    /* E = was raw^T (rows are orthonormal, so raw^T is raw's inverse) */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            c->rot[i][j] = was->rot[i][0] * raw->rot[j][0] + was->rot[i][1] * raw->rot[j][1] + was->rot[i][2] * raw->rot[j][2];
    c->on = 1;
    return 1;
}

/* dt seconds later: the correction is exp(-dt/tau) of what it was (rotation blended toward the
   identity, then made orthonormal again). */
static inline void correction_decay(PuppetCorrection *c, float dt)
{
    if (!c->on || !(dt > 0)) return;
    float keep = expf(-dt / CORRECTION_TAU);
    for (int k = 0; k < 3; k++) c->pos[k] *= keep;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) c->rot[i][j] = (i == j) + (c->rot[i][j] - (i == j)) * keep;
    for (int i = 0; i < 3; i++) {  /* Gram-Schmidt, rows */
        for (int j = 0; j < i; j++) {
            float d = c->rot[i][0] * c->rot[j][0] + c->rot[i][1] * c->rot[j][1] + c->rot[i][2] * c->rot[j][2];
            for (int k = 0; k < 3; k++) c->rot[i][k] -= d * c->rot[j][k];
        }
        float n = sqrtf(c->rot[i][0] * c->rot[i][0] + c->rot[i][1] * c->rot[i][1] + c->rot[i][2] * c->rot[i][2]);
        for (int k = 0; k < 3; k++) c->rot[i][k] /= n > 0 ? n : 1;
    }
    if (keep < 0.001f) correction_clear(c);
}

/* shown = raw with the correction */
static inline void correction_apply(const PuppetCorrection *c, const PuppetPose *raw, PuppetPose *out)
{
    PuppetPose r = *raw;
    for (int k = 0; k < 3; k++) out->pos[k] = r.pos[k] + (c->on ? c->pos[k] : 0);
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++)
            out->rot[i][k] = c->on ? c->rot[i][0] * r.rot[0][k] + c->rot[i][1] * r.rot[1][k] + c->rot[i][2] * r.rot[2][k]
                                   : r.rot[i][k];
        out->rot[i][3] = r.rot[i][3];
    }
}
#endif
