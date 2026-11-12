/* A rider's record in a STATE bundle (state_bundle.h), as bytes and back, and what a receiver
   checks before any of it is used (finding 220). The record comes from another player's game: the
   owner check (puppet.c receive_record) stops a game from touching riders that are not its own,
   but a broken or modified game can still send any bytes for its own riders (a host owns every
   computer rider), and public rooms exist. So every field is checked here, the whole record before
   any of it is used, and a record that fails is refused whole.

   Pure functions on plain values; unit test test_puppet_record.c. */
#ifndef DHD_PUPPET_RECORD_H
#define DHD_PUPPET_RECORD_H
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "pad_snapshot.h"
#include "state_bundle.h"
#include "puppet_sync.h"

#define PUPPET_LINE_NONE 0xFFFF

/* The record's fields (puppet.c hle_puppet_pack, payload version 8): global id, race time (ms),
   the body (position, three orientation rows, velocity, angular velocity), power, a computer
   rider's turn scale, 8 reserved bytes, its racing line (node, next node, target point, the next
   target point, sideways offset and the one it eases toward), wheel contacts, the State's offset
   in the rider, crash flags and kind, checkpoints passed, attack kind, bottles left, then the pad
   update (pad_snapshot.h). */
typedef struct PuppetRecord {
    uint8_t rider;
    int32_t ms;
    float pos[3], rot[3][3], vel[3], spin[3];
    float power, turning;
    uint16_t node, next;
    float target[3], after[3], offset[2], offset_to[2];
    uint8_t contact[2];
    uint16_t state;
    uint8_t crash, crash_kind, checkpoint, attack, bottles;
    const uint8_t *pad;      /* the pad update: its count, then count index/u32 pairs */
    int pad_size;
} PuppetRecord;

static inline uint8_t *puppet_put(uint8_t *o, const void *v, size_t n) { memcpy(o, v, n); return o + n; }
static inline const uint8_t *puppet_get(const uint8_t *in, void *v, size_t n) { memcpy(v, in, n); return in + n; }

/* The fixed part (STATE_RECORD_FIXED - 1 bytes; the pad update follows). Returns its end. */
static inline uint8_t *puppet_record_write(const PuppetRecord *r, uint8_t *o)
{
    static const uint8_t reserved[8];
    *o++ = r->rider;
    o = puppet_put(o, &r->ms, 4);
    o = puppet_put(o, r->pos, 12);
    for (int k = 0; k < 3; k++) o = puppet_put(o, r->rot[k], 12);
    o = puppet_put(o, r->vel, 12);
    o = puppet_put(o, r->spin, 12);
    o = puppet_put(o, &r->power, 4);
    o = puppet_put(o, &r->turning, 4);
    o = puppet_put(o, reserved, 8);
    o = puppet_put(o, &r->node, 2);
    o = puppet_put(o, &r->next, 2);
    o = puppet_put(o, r->target, 12);
    o = puppet_put(o, r->after, 12);
    o = puppet_put(o, r->offset, 8);
    o = puppet_put(o, r->offset_to, 8);
    *o++ = r->contact[0], *o++ = r->contact[1];
    o = puppet_put(o, &r->state, 2);
    *o++ = r->crash, *o++ = r->crash_kind;
    *o++ = r->checkpoint;
    *o++ = r->attack, *o++ = r->bottles;
    return o;
}

/* A record of n bytes (state_record_size's length) into its fields. 0: shorter than a record. */
static inline int puppet_record_read(const uint8_t *in, int n, PuppetRecord *r)
{
    if (n < STATE_RECORD_FIXED) return 0;
    const uint8_t *i = in;
    memset(r, 0, sizeof *r);
    r->rider = *i++;
    i = puppet_get(i, &r->ms, 4);
    i = puppet_get(i, r->pos, 12);
    for (int k = 0; k < 3; k++) i = puppet_get(i, r->rot[k], 12);
    i = puppet_get(i, r->vel, 12);
    i = puppet_get(i, r->spin, 12);
    i = puppet_get(i, &r->power, 4);
    i = puppet_get(i, &r->turning, 4);
    i += 8;  /* reserved: never read */
    i = puppet_get(i, &r->node, 2);
    i = puppet_get(i, &r->next, 2);
    i = puppet_get(i, r->target, 12);
    i = puppet_get(i, r->after, 12);
    i = puppet_get(i, r->offset, 8);
    i = puppet_get(i, r->offset_to, 8);
    r->contact[0] = *i++, r->contact[1] = *i++;
    i = puppet_get(i, &r->state, 2);
    r->crash = *i++, r->crash_kind = *i++;
    r->checkpoint = *i++;
    r->attack = *i++, r->bottles = *i++;
    r->pad = i;
    r->pad_size = (int)(in + n - i);
    return 1;
}

/* ---- The State: only a State object the rider has ----

   The rider's constructor (func_0012B620) builds 18 State objects inside the rider and lists them
   in its own table at +0x6394..+0x63D8, in this order; every switch the game makes
   (func_0012FE38's callers) names one of these, and the race's setup (func_0012BF88) starts each
   rider in +0x5FE0. A record's State is an offset into the rider whose object's virtual Enter the
   receiver calls (puppet.c, func_0012FE38): any other offset is refused. Names from findings 044,
   121 and 122 and from the switches that enter each (port/tools/test_rider_states.py checks this
   list against the constructor's table and the switches). */
static const uint16_t PUPPET_STATES[] = {
    0x4ED0,  /* riding (every State's way back) */
    0x4EE0,  /* crashed (func_00155E78 enters it when a crash starts) */
    0x5E40,  /* func_00174778's: entered from riding by its own test (func_0019C008, func_001524D8) */
    0x5E70,  /* entered from riding by its own test (func_0019C008, func_001DBEF8) */
    0x5E90,  /* the attack (punch, kick, check, stick, bottle; finding 121) */
    0x5FE0,  /* the grid before GO (func_0012BF88) */
    0x5FF0,  /* entered from riding and the camera's mode (func_00174968, func_0019C008) */
    0x6210,  /* the AI's (finding 044), func_00181DE0, func_001DBEF8 */
    0x621C,  /* func_001DBC58's: the AI's too (finding 044) */
    0x6248,  /* the punch (func_00184330's; finding 044) */
    0x6268,  /* the stagger after a small hit (func_00181C30's; func_0012E940) */
    0x6280,  /* entered by the checkpoints' pass (func_0019F210) */
    0x6298,  /* the push-off at the start (finding 044; func_0012F110) */
    0x62B4,  /* entered from riding (func_00184630) */
    0x62E0,  /* a crash's own State (finding 122: the crash camera follows it) */
    0x6310,  /* entered from riding (func_0019C008, func_00174968) */
    0x6320,  /* a crash's own State (finding 122; func_0012F7D0, func_0012D860); no crash starts in it */
    0x6330,  /* entered with a target (func_00152108: +0x7BA4) */
};
enum { PUPPET_STATE_COUNT = sizeof PUPPET_STATES / sizeof PUPPET_STATES[0] };

static inline int puppet_state_known(uint32_t offset)
{
    for (int i = 0; i < PUPPET_STATE_COUNT; i++)
        if (PUPPET_STATES[i] == offset) return 1;
    return 0;
}

/* ---- The racing line: only the loaded track's nodes ----

   The racing-line graph (D_0029EA20) holds its nodes (0x20 bytes each) in its first 0x10000 bytes
   and its own fields after them; its node count is at +0x1004C (func_00117658 loads it with the
   nodes, func_00117770 sets the default graph's), and the game's accessor func_001177F8 refuses an
   index at or past it. A record's node or next node is handed to the game's graph walker
   (puppet.c take_line, func_0010B2B8 -> func_00118650) and installed as the AI's next node, so an
   index past the loaded nodes (the graph's own fields, or an unused node of a shorter track) would
   be read as links: only an index under the loaded count is a node (PUPPET_LINE_NONE: none). */
enum { PUPPET_NODE_CAPACITY = 0x800 };  /* 0x10000 / 0x20 (func_00117658 walks 0x800) */

/* The loaded count as a usable bound (0-PUPPET_NODE_CAPACITY). */
static inline int puppet_node_count(int32_t loaded)
{
    return loaded < 0 ? 0 : loaded > PUPPET_NODE_CAPACITY ? PUPPET_NODE_CAPACITY : loaded;
}
static inline int puppet_node_ok(uint16_t node, int32_t loaded)
{
    return node == PUPPET_LINE_NONE || node < puppet_node_count(loaded);
}

/* ---- Numbers: every one finite and sane ----

   One NaN in a pose poisons the rigid body and everything that reads it (the steering, set_body,
   the AI controller's direction to its target). Every float of a record must be finite, and each
   within a bound far outside anything a race shows. Measured on the puppet bench's recordings
   (port/tools/puppet_bench.py record: every rider of three tracks each race step, 75 s; finding
   220 has the numbers) and widened many times (the orientation rows' length fivefold, the rest at
   least eighteen); the counts from the game's own code. */
#define PUPPET_WORLD_MAX 200000.0f  /* units, each axis of a position or target point (tracks: within 11300) */
#define PUPPET_SPEED_MAX 5000.0f    /* units a second (riders: up to 263) */
#define PUPPET_SPIN_MAX 500.0f      /* radians a second (riders: up to 23) */
#define PUPPET_ROW_SLACK 0.5f       /* an orientation row's length within 1 +- this (riders: 0.91-1.00) */
#define PUPPET_ROW_VOLUME_MIN 0.5f  /* the rows' determinant over their lengths' product at least this: not
                                       degenerate (no two near parallel, not near one plane) and not mirrored
                                       (riders: 1.0000; their rows are orthogonal to 3e-7) */
#define PUPPET_OFFSET_MAX 1000.0f   /* units: a racing line's sideways offset (riders: within 22) */
#define PUPPET_POWER_MAX 10.0f      /* power, either sign (riders: 0.75-1.22; puppet.c uses only 0.5-2) */
#define PUPPET_TURNING_MAX 100.0f   /* a computer rider's turn scale, either sign (riders: 0-1.04; used only 0-1) */
enum {
    PUPPET_CONTACT_MAX = 2,      /* a wheel's contact: 0 none, 1 ground, 2 air */
    PUPPET_CRASH_KIND_MAX = 10,  /* func_00155E78's kinds: its callers pass 1-10 */
    PUPPET_CHECKPOINT_MAX = 35,  /* func_0019F210 reads checkpoint plane [passed + 1] of 36 (+0x220) and writes the
                                    rider's +0x79B0[passed]: past this it would read and write outside both */
    PUPPET_ATTACK_MAX = 4,       /* punch, kick, check, stick, bottle (finding 121) */
    PUPPET_BOTTLES_MAX = 3,      /* bottles left (func_0012C9E0, func_00130910); the cheat's endless -1 is 255 here */
    /* The pad (pad_snapshot.h) holds no floats: its words are integers the game compares. This
       tick's 25 action words (the ones a puppet's pad takes) are buttons (0 or 1, func_0017EB00) and
       analogue amounts (0-127 after func_0017E1C0's square, func_0017DFB0's 0-128): a byte at most. */
    PUPPET_PAD_ACTION_MAX = 255
};

static inline int puppet_finite(const float *v, int n)
{
    for (int i = 0; i < n; i++)
        if (!isfinite(v[i])) return 0;
    return 1;
}
static inline int puppet_within(const float *v, int n, float most)
{
    for (int i = 0; i < n; i++)
        if (!(fabsf(v[i]) <= most)) return 0;  /* NaN fails too */
    return 1;
}
static inline float puppet_len3(const float *v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

/* The whole record: `pad` is the pad mirror as its update leaves it (applied to a copy first), and
   node_count the racing-line graph's loaded count. NULL: it may be used; else why not (for the
   log). Nothing of a refused record is used. */
static inline const char *puppet_record_check(const PuppetRecord *r, const uint32_t *pad, int32_t node_count)
{
    if (!puppet_finite(r->pos, 3) || !puppet_finite(&r->rot[0][0], 9) || !puppet_finite(r->vel, 3) ||
        !puppet_finite(r->spin, 3) || !isfinite(r->power) || !isfinite(r->turning) || !puppet_finite(r->target, 3) ||
        !puppet_finite(r->after, 3) || !puppet_finite(r->offset, 2) || !puppet_finite(r->offset_to, 2))
        return "not finite";
    if (!puppet_within(r->pos, 3, PUPPET_WORLD_MAX)) return "position";
    if (!(puppet_len3(r->vel) <= PUPPET_SPEED_MAX)) return "speed";
    if (!(puppet_len3(r->spin) <= PUPPET_SPIN_MAX)) return "spin";
    for (int i = 0; i < 3; i++)
        if (!(fabsf(puppet_len3(r->rot[i]) - 1) <= PUPPET_ROW_SLACK)) return "orientation";
    {
        const float (*m)[3] = r->rot;
        float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                    m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if (!(det >= PUPPET_ROW_VOLUME_MIN * puppet_len3(m[0]) * puppet_len3(m[1]) * puppet_len3(m[2]))) return "orientation";
    }
    if (!(fabsf(r->power) <= PUPPET_POWER_MAX) || !(fabsf(r->turning) <= PUPPET_TURNING_MAX)) return "power";
    if (!puppet_node_ok(r->node, node_count) || !puppet_node_ok(r->next, node_count) ||
        !puppet_within(r->target, 3, PUPPET_WORLD_MAX) || !puppet_within(r->after, 3, PUPPET_WORLD_MAX) ||
        !puppet_within(r->offset, 2, PUPPET_OFFSET_MAX) || !puppet_within(r->offset_to, 2, PUPPET_OFFSET_MAX))
        return "racing line";
    if (!puppet_state_known(r->state)) return "state";
    if (r->contact[0] > PUPPET_CONTACT_MAX || r->contact[1] > PUPPET_CONTACT_MAX || r->crash_kind > PUPPET_CRASH_KIND_MAX ||
        r->checkpoint > PUPPET_CHECKPOINT_MAX || r->attack > PUPPET_ATTACK_MAX ||
        (r->bottles > PUPPET_BOTTLES_MAX && r->bottles != 255))
        return "counts";
    for (int w = 0; w < PREV_FIRST; w++)
        if (pad[w] > PUPPET_PAD_ACTION_MAX) return "pad";
    return NULL;
}

/* A read record taken against a rider's pad mirror: its pad update applied to a copy (`pad`, the
   pad the record shows), the whole record checked, and the mirror changed only when it passes.
   NULL: taken; else why it was refused (the mirror as it was). */
static inline const char *puppet_record_take(const PuppetRecord *r, uint32_t mirror[PAD_WORDS], int32_t node_count,
                                             uint32_t pad[PAD_WORDS])
{
    memcpy(pad, mirror, PAD_WORDS * sizeof *pad);
    if (!pad_snapshot_apply(pad, r->pad, r->pad_size)) return "pad";
    const char *why = puppet_record_check(r, pad, node_count);
    if (!why) memcpy(mirror, pad, PAD_WORDS * sizeof *pad);
    return why;
}

/* ---- Stamps: none from the future ----

   A stamp is placed on this race clock through the shared server clock (puppet.c receive_record,
   receive_bundle). Its error there is the two games' clock offsets (each within half a trusted
   round trip, port/net/client.c clock_sample: tens of ms, a few hundred on the worst links) plus
   what a game's race clock trails beyond the pacing lag, which sync_clock_excess takes off. A stamp
   further ahead of this game's race time than PUPPET_AHEAD_MAX is no clock error: refused. One
   such snapshot taken would make every honest one after it "stale" (older than the newest) until
   this game's clock reached it, and a HOLD so stamped kept its riders held for the race. */
#define PUPPET_AHEAD_MAX 1.0
/* A HOLD names its sender's last bundle's stamp. Bundles can be lost, so it may be newer than the
   newest snapshot this game took from it, by at most this much (more: it is not about the
   snapshots this game has; the riders then coast as on a quiet link, SYNC_COAST). */
#define PUPPET_HOLD_MATCH 0.5

static inline int puppet_stamp_ok(double t, double now) { return isfinite(t) && t <= now + PUPPET_AHEAD_MAX; }

/* A snapshot stamped t arriving at now, the newest taken from its rider at `newest` (have: any):
   taken when it is not from the future and newer than that newest (else stale, or refused). */
static inline int puppet_snapshot_new(double t, int have, double newest) { return !have || t > newest; }

/* A rider's hold (state_bundle.h HOLD): held since its sender's snapshot of moment held_at; fed_at:
   when (this race clock) the first snapshot taken after the hold arrived (0: none yet). */
typedef struct PuppetHold {
    int held;
    double held_at, fed_at;
} PuppetHold;

/* A HOLD stamped t from a sender quiet for `quiet` s (its own clock) arrives at now; `newest`: the
   newest snapshot this game took of the rider (have: any). PUPPET_HOLD_TAKEN when it is about that
   snapshot and sane: not from the future, not more than PUPPET_HOLD_MATCH after it, and its stop no
   longer than its stamp is old here; PUPPET_HOLD_OVER when a newer snapshot is already here (the
   stop is over, as before); else PUPPET_HOLD_REFUSED. */
enum { PUPPET_HOLD_REFUSED = -1, PUPPET_HOLD_OVER = 0, PUPPET_HOLD_TAKEN = 1 };
static inline int puppet_hold_ok(double t, double quiet, int have, double newest, double now)
{
    if (!puppet_stamp_ok(t, now) || !isfinite(quiet) || quiet < 0) return PUPPET_HOLD_REFUSED;
    if (have && newest > t + 0.001) return PUPPET_HOLD_OVER;
    if (!have || t > newest + PUPPET_HOLD_MATCH) return PUPPET_HOLD_REFUSED;
    return quiet <= now - t + PUPPET_AHEAD_MAX ? PUPPET_HOLD_TAKEN : PUPPET_HOLD_REFUSED;  /* its stop began at its last bundle */
}
static inline void puppet_hold_start(PuppetHold *h, double t) { h->held = 1, h->held_at = t, h->fed_at = 0; }

/* A snapshot was taken into the ring at now (whatever its stamp). */
static inline void puppet_hold_fed(PuppetHold *h, double now)
{
    if (h->held && !(h->fed_at > 0)) h->fed_at = now > 0 ? now : 1e-9;
}

/* Whether the hold is over: snapshots newer than it have come (`shown`: the newest one whose
   moment has come), or snapshots have been arriving again for SYNC_COAST by this game's clock,
   whatever their stamps (a sender whose clock moved back, a HOLD stamped ahead of its snapshots). */
static inline int puppet_hold_resumed(const PuppetHold *h, double shown) { return shown > h->held_at + 0.001; }
static inline int puppet_hold_fed_long(const PuppetHold *h, double now) { return h->fed_at > 0 && now - h->fed_at > SYNC_COAST; }

#endif
