/* Puppet riders (findings 044, 202): rider slots that show a rider of another PC, the base of
   state-sync online races.

   The race step (func_0011A100, patched in port/patches.py) runs five passes over the riders:
   input (func_0012C8B0: the pad or the AI writes the rider's virtual pad at +0xFE0), bookkeeping
   (func_0012D248), physics (func_0012D360), the state machine (func_0012D380: the current State
   at +0x4ECC picks the animation) and the rest (func_0012D3C8: effects, track progress, sound).
   A puppet's input is its owner's: hle_puppet_input writes the virtual pad from the newest
   snapshot, and the rider keeps it until the next. Then it rides on this game's own physics like
   any rider, so its wheels follow this game's ground, it lands, steers and bumps as a rider does,
   and it keeps riding between snapshots. Before the physics, hle_puppet_physics steers its body
   toward its owner's snapshots (puppet_sync.h): each one is compared with where this puppet was at
   that snapshot's moment, and the difference is made up over about a tenth of a second, the part
   that would push the rider into the ground left to its wheels. A difference too large to be
   riding (a respawn, a long loss) puts it where the snapshot says at once. The State follows the
   snapshot too: the game's own switches are blocked (func_0012FE38 asks hle_puppet_switch).

   Crashes and hits belong to the puppet's own PC. The puppet never starts a crash by itself
   (func_00155E78 asks hle_puppet_crash) and takes or deals no hit here (func_0012E940 asks
   hle_puppet_hit; a hit by the local rider on a puppet becomes an event for its PC). When its
   snapshot's crash bit rises it starts the same crash, which then plays out here (the bike and
   the thrown body are simulated locally until the respawn). A hit by this PC's rider is decided
   on the puppet as this PC shows it, at the present, and applied by the victim's PC to its own
   rider as it is when the event arrives (finding 169).

   Snapshots arrive 60 a second for a player's own rider, 30 for the AI riders (finding 169), each
   stamped with the sender's server clock, which places it on this PC's race clock: its age on
   arrival is measured, not guessed. Every PC runs at its own frame rate (30-240), so the clocks
   here are race time in seconds, not ticks: a tick (the race clock's field) is 1/60 s, and a step
   may be part of one or up to two of them (port_field_seconds, findings 114, 139).

   Tests, one PC (the puppet bench, port/tools/puppet_bench.py, finding 202):
   DHD_PUPPET_RECORD=file   each race step, a snapshot of rider DHD_PUPPET_FROM (default 0; "all":
                            every rider of the race), with its wheels' clearance above the ground
   DHD_PUPPET_PLAY=file     replay those snapshots onto puppets (DHD_PUPPET_MAP=src:dst,...;
                            default every recorded AI rider onto its own slot, a one-rider file onto
                            DHD_PUPPET_SLOT, default 1), sent as online (60 a second for slot 0, 30
                            for the others) through DHD_PUPPET_LINK (puppet_link.h: delay, jitter,
                            loss, bursts, reordering, clock errors, hitches)
   DHD_PUPPET_CAMERA=1      player 0's camera follows the puppet (slot DHD_PUPPET_SLOT, online too)
   DHD_PUPPET_TRACE=file    each race step, each puppet as drawn (text, hle_puppet_field)
   DHD_NET_LINK=link        online: the STATE this game receives goes through such a link first
   Online (port/hle/online.c): hle_puppet_online, hle_puppet_pack, hle_puppet_receive. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define RIDERS ((uint8_t *)0x082AF2C0)          /* D_002AF2C0: 10 riders */
#define RIDER_SIZE 0x7BD0
#define NRIDERS 10
int hle_online_race_riders(void);  /* online.c: the riders in the online race (computer riders off: fewer) */
#define RACE_CLOCK (*(volatile int32_t *)0x0843EEC8)  /* ticks since GO in phase 2 */
double port_field_seconds(int counter);  /* fps.c */
double host_wall_seconds(void);
static void net_link_release(void);
#define RACE_TIME port_field_seconds(RACE_CLOCK)      /* seconds since GO, part of a tick too */
#define RACE_PHASE (*(volatile int32_t *)0x0843F348)  /* on the race screen: 1 countdown, 2-4 race */
#define MENU (*(volatile int32_t *)0x0829DED8)        /* 9: the race screen */
#define RACING (MENU == 9 && RACE_PHASE >= 2 && RACE_PHASE <= 4)  /* func_0016B5A8 */


#define FIELD(r, off, type) (*(type *)((r) + (off)))
#define PAD_AT 0x1174    /* virtual pad (+0xFE0) +0x194: this tick's 25 actions, last tick's, timers */
#include "pad_snapshot.h"
#define POS_AT 0x4270    /* rigid body (+0x40C0) +0x1B0: position */
#define ROT_AT 0x4280    /* +0x1C0: orientation, 3 rows */
#define VEL_AT 0x4180    /* +0xC0: velocity (units a second) */
#define SPIN_AT 0x4190   /* +0xD0: angular velocity */
#define MOMENTUM_AT 0x4120  /* +0x60: linear momentum, mass (+0x150) times velocity */
#define MASS_AT 0x4210
#define PREV_POS_AT 0x4110  /* +0x50: the position before this step (the physics sweeps from it) */
#define FRONT_AT 0x4414  /* +0x2E0 +0x74: front wheel contact (0 none, 1 ground, 2 air) */
#define BACK_AT 0x4494   /* +0x360 +0x74: back wheel */
#define STATE_AT 0x4ECC  /* State *: the states are objects inside the rider */
#define FRONT_WHEEL_AT 0x7944  /* wheel matrices; func_00134CC8 spins them: (+0x24, +0x28) = (c, s), */
#define BACK_WHEEL_AT 0x7948   /* +0x34 = -s, +0x38 = c */
#define CRASH_AT 0x3FD0  /* crash controller (func_00155E78 starts, func_00156580 runs a crash) */
#define CRASH_KIND_AT 0x407C  /* its +0xAC: what started the crash */
#define CRASH_FLAGS_AT 0x4098 /* its +0xC8: bit 2 = crashing */
#define CHECKPOINT_AT 0x7990  /* checkpoints passed (func_0019F210) */
#define ATTACK_AT 0x6390      /* the attack the combat power-ups give: 0 punch, 1 kick, 2 check, 3 stick, 4 bottle */
#define BOTTLES_AT 0x7ADC     /* bottles left to throw (func_00179A60 shows both) */

typedef struct Snapshot {
    int32_t clock;           /* race ticks (DHD_PUPPET_PLAY's one in RATE) */
    double time;             /* race time, seconds (on the wire in ms) */
    int32_t state;           /* offset of the current State in the rider */
    float pos[4], rot[3][4], vel[4], spin[4];
    float wheel[2][2];       /* (c, s) of the front and back wheel */
    uint8_t contact[2];
    uint8_t crash, crash_kind;   /* crash flags (bit 2: crashing) and kind */
    uint8_t checkpoint;      /* checkpoints passed (+0x7990) */
    uint8_t attack, bottles; /* power-ups: the attack kind and bottles left */
    uint32_t pad[PAD_WORDS];
    int32_t slot;            /* tests (DHD_PUPPET_RECORD): the rider's slot */
    float ground[2];         /* tests: each wheel's clearance above the ground (ground_clearance) */
} Snapshot;

enum { RING = 32, MAX_EVENTS = 16, NET_SLOTS = 10 };
/* A rider's record in a STATE bundle (state_bundle.h, payload version 6, finding 169): its global
   id (a human's lobby slot, an AI rider's slot), its race time in ms (finding 114), the body, the
   checkpoints passed and the attack power-ups, then the changed pad words (six repeats each, a
   full copy every PAD_REFRESH sends, finding 158). */
#include "state_bundle.h"
#include "puppet_sync.h"
#include "puppet_link.h"
enum { EVENT_VERSION = 2, EVENT_HIT = 1 };  /* a hit: online.c's EVENT version */

typedef struct Puppet {
    int on;                  /* this slot is a puppet */
    Snapshot ring[RING];     /* by clock, oldest first */
    int count;
    /* On arrival, how old each snapshot was on the shared clock (s), smoothed (gain 1/16; finding
       169): the log's riders' delay */
    int have_age;
    double age;
    /* Steering (finding 202, puppet_sync.h): its own recent poses and the correction under way,
       the newest snapshot used, the last step's moment, and whether it rides where its snapshots
       put it yet (no: the next snapshot puts it there at once) */
    PuppetSync sync;
    double used, stepped_at;
    int synced, after_crash, resyncs;
    float error;             /* the last snapshot's difference from this puppet (units) */
    double behind;           /* the newest snapshot's age when last used (s) */
    double shown;            /* the moment (this race clock) the pose last written shows */
    uint32_t mirror[PAD_WORDS];  /* the remote virtual pad, as the deltas built it */
    PadSender sent_pad;
    int remote_crash;
} Puppet;

extern void func_0012DDC0(void *rider);              /* body to scene node */
extern void func_0012FE38(void *rider, void *state); /* Exit the current State, Enter this one */
extern void func_00155E78(void *crash, int kind);    /* start a crash */
extern void func_0012E940(void *self, int kind, void *direction, void *other);  /* other hits self */

typedef struct Delivery {
    double at;               /* on this game's race clock */
    double shift;            /* the sender's stamp: its race time plus this (its clock's lag) */
    int index;               /* into pp.file */
} Delivery;
enum { HELD_MAX = 512, HELD_BYTES = 1024 };

static struct {
    int init, from;          /* DHD_PUPPET_FROM: the recorded rider, -1 all */
    int allow;               /* the puppet code itself starts this crash / applies this hit */
    FILE *record, *trace;
    Snapshot *file;          /* DHD_PUPPET_PLAY, in recording order */
    int file_count, playing;
    int play_from[NRIDERS];  /* each slot's recorded rider (-1: not a replayed puppet) */
    Delivery *deliveries;  /* the file's snapshots that get through, by arrival */
    int delivery_count, delivery_next;
    PuppetLink link, net_link;  /* DHD_PUPPET_LINK; DHD_NET_LINK on the online receive path */
    struct { double at; int sender, size; uint8_t bytes[HELD_BYTES]; } held[HELD_MAX];  /* DHD_NET_LINK */
    int held_count;
    Puppet p[NRIDERS];
    /* online */
    int online, local, nslots, host;
    double go_server;        /* this PC's GO on the server clock (ms), 0 until known */
    int gone[NET_SLOTS];     /* lobby slots that left */
    int kept[NET_SLOTS];     /* of those, the ones that had finished: their riders stay puppets */
    int hits_taken;
    uint8_t events[MAX_EVENTS][32];
    int event_size[MAX_EVENTS], nevents;
} pp;

static int slot_of(const uint8_t *r) { return (int)((r - RIDERS) / RIDER_SIZE); }
static uint8_t *rider(int slot) { return RIDERS + slot * RIDER_SIZE; }
#define RACE_RIDERS (*(volatile int32_t *)(RIDERS + 0x501A8))  /* the race's riders (func_0011A100) */

/* ---- tests: the ground under a rider's wheels ---- */

/* The game's own world query (func_00214BA0, as the wheels' probes in func_00131998 use it): a
   segment from `from` to `to`; up to max hits, each a point (w: where along the segment, 0-1)
   and the surface's normal. */
typedef struct GroundHit {
    uint8_t header[16];
    float pos[4], normal[4];
} __attribute__((aligned(16))) GroundHit;
typedef struct GroundQuery {
    float *from, *to;
    float t0, t1, radius;
    void *matrix, *scale;
    int max;
    GroundHit *hits;
    int found;
} GroundQuery;
_Static_assert(sizeof(GroundHit) == 0x30, "the game's hit record");
_Static_assert(sizeof(GroundQuery) == 0x28, "the game's query record (32-bit pointers: the core)");
extern int func_00214BA0(void *queries, int count);
extern void func_00134D70(void *wheel, void *center, void *body);  /* a wheel's centre in the world */
#define BODY_AT 0x40C0
#define WHEEL_BODY_AT(w) (BODY_AT + ((w) ? 0x360 : 0x2E0))
#define CONTACT_STATE_AT 0x74    /* a wheel body's contact: 0 none, 1 ground, 2 air */
#define CONTACT_NORMAL_AT 0x50   /* its contact's (+0x30) surface normal (+0x20) */
#define SYNC_RESPAWN 3.0f        /* after a crash, a larger difference is the respawn: shown at once */

/* How far wheel w (0 front, 1 back) of the rider as it is now stands clear of the ground under
   it, in game units: from the wheel's centre (func_00134D70, the pose's own) to the first
   surface on a segment REACH units above it to REACH below it along the world's up axis, measured
   along that surface's normal, less the wheel's radius. Below 0: the wheel is in the ground.
   NAN: no ground within REACH (in the air). Tests only (the bench's trace and recordings). The
   world's z axis is up (a wheel's contact lies its radius below its centre in z). */
enum { UP = 2 };
#define REACH 4.0f
static float ground_clearance(uint8_t *r, int w)
{
    uint8_t *wheel = r + WHEEL_BODY_AT(w);
    float center[4] __attribute__((aligned(16))), from[4] __attribute__((aligned(16))), to[4] __attribute__((aligned(16)));
    GroundHit hits[10];
    func_00134D70(wheel, center, r + BODY_AT);
    float radius = ((float *)wheel)[3];
    memcpy(from, center, sizeof from);
    memcpy(to, center, sizeof to);
    from[UP] += REACH, to[UP] -= REACH;
    GroundQuery q = {from, to, 1, 1, 0, NULL, NULL, 10, hits, 0};
    func_00214BA0(&q, 1);
    int best = -1;
    for (int i = 0; i < q.found && i < 10; i++)
        if (best < 0 || hits[i].pos[3] < hits[best].pos[3]) best = i;
    if (best < 0) return NAN;
    const float *n = hits[best].normal, *g = hits[best].pos;
    float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(len > 0)) return NAN;
    return ((center[0] - g[0]) * n[0] + (center[1] - g[1]) * n[1] + (center[2] - g[2]) * n[2]) / len - radius;
}

static void take(Snapshot *s, const uint8_t *r);

/* ---- tests: recorded riders replayed through a bad link (finding 202) ---- */

static int by_arrival(const void *a, const void *b)
{
    const Delivery *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at ? 1 : x->index - y->index;
}

/* DHD_PUPPET_MAP=src:dst,...: which recorded rider each puppet shows. Default: every recorded
   rider but slot 0 onto its own slot (the AI riders of the recorded race); a file of one rider onto
   DHD_PUPPET_SLOT (default 1). */
static void play_map(int map[NRIDERS], const char *text, int only)
{
    for (int i = 0; i < NRIDERS; i++) map[i] = -1;
    if (text && *text) {
        for (const char *s = text; *s;) {
            char *end;
            int a = (int)strtol(s, &end, 10), b;
            if (*end != ':') break;
            b = (int)strtol(end + 1, &end, 10);
            if (a >= 0 && a < NRIDERS && b >= 1 && b < NRIDERS) map[a] = b;
            s = *end ? end + 1 : end;
        }
    } else if (only >= 0) {
        const char *s = getenv("DHD_PUPPET_SLOT");
        map[only] = s ? atoi(s) : 1;
    } else {
        for (int i = 1; i < NRIDERS; i++) map[i] = i;
    }
}

/* The file's snapshots, as their senders would send them (60 or 30 a second: the first step of
   each race tick, or of every second one), through the link (puppet_link.h), sorted by the moment
   each arrives here. The sender's clock is the recorded race clock, moved by its hitch. */
static void play_schedule(const char *link_text)
{
    PuppetLink link;
    int map[NRIDERS], only = -1, last_tick[NRIDERS];
    link_parse(&link, link_text);
    for (int i = 0; i < pp.file_count; i++) {
        int slot = pp.file[i].slot;
        if (only == -1) only = slot;
        else if (only != slot) only = -2;
    }
    play_map(map, getenv("DHD_PUPPET_MAP"), only >= 0 ? only : -1);
    for (int i = 0; i < NRIDERS; i++) {
        last_tick[i] = -1000;
        if (map[i] > 0) pp.p[map[i]].on = 1, pp.play_from[map[i]] = i;
    }
    pp.deliveries = calloc(pp.file_count ? pp.file_count : 1, sizeof *pp.deliveries);
    pp.delivery_count = 0;
    for (int i = 0; i < pp.file_count; i++) {
        const Snapshot *s = &pp.file[i];
        if (s->slot < 0 || s->slot >= NRIDERS || map[s->slot] < 0) continue;
        int rate = link.rate ? link.rate : s->slot == 0 ? 60 : 30, every = rate >= 60 ? 1 : 2;
        if (s->clock == last_tick[s->slot] || s->clock % every) continue;
        /* a new race in the file (the recording went on into another) */
        if (s->clock < last_tick[s->slot]) last_tick[s->slot] = -1000;
        last_tick[s->slot] = s->clock;
        double sent = link_hitch_wall(link.hitch_at, link.hitch_len, s->time), arrives = sent;
        if (link.on && !link_pass(&link, sent, &arrives)) continue;
        /* the receiver's race clock at that wall moment; the stamp as the sender makes it */
        pp.deliveries[pp.delivery_count++] =
            (Delivery){link_hitch_race(link.rhitch_at, link.rhitch_len, arrives), sync_clock_excess(sent - s->time), i};
    }
    qsort(pp.deliveries, pp.delivery_count, sizeof *pp.deliveries, by_arrival);
    pp.link = link;
    fprintf(stderr, "puppet: %d snapshots of %d recorded, %d delivered through the link (%s)\n", pp.file_count,
            pp.file_count, pp.delivery_count, link_text && *link_text ? link_text : "perfect");
    for (int i = 0; i < NRIDERS; i++)
        if (map[i] > 0) fprintf(stderr, "puppet: recorded rider %d onto slot %d\n", i, map[i]);
}

static void setup(void)
{
    const char *s;
    pp.init = 1;
    pp.from = (s = getenv("DHD_PUPPET_FROM")) ? (!strcmp(s, "all") ? -1 : atoi(s)) : 0;
    for (int i = 0; i < NRIDERS; i++) pp.play_from[i] = -1;
    if ((s = getenv("DHD_PUPPET_RECORD")) && *s && !(pp.record = fopen(s, "wb")))
        fprintf(stderr, "puppet: cannot write %s\n", s);
    if ((s = getenv("DHD_PUPPET_TRACE")) && *s && !(pp.trace = fopen(s, "w")))
        fprintf(stderr, "puppet: cannot write %s\n", s);
    if ((s = getenv("DHD_NET_LINK")) && *s && link_parse(&pp.net_link, s))
        fprintf(stderr, "puppet: STATE arrives through a test link: %s\n", s);
    if ((s = getenv("DHD_PUPPET_PLAY")) && *s) {
        FILE *f = fopen(s, "rb");
        Snapshot one;
        if (!f) {
            fprintf(stderr, "puppet: cannot read %s\n", s);
            return;
        }
        while (fread(&one, sizeof one, 1, f) == 1) {
            pp.file = realloc(pp.file, (pp.file_count + 1) * sizeof *pp.file);
            pp.file[pp.file_count++] = one;
        }
        fclose(f);
        pp.playing = 1;
        play_schedule(getenv("DHD_PUPPET_LINK"));
    }
}

static void take(Snapshot *s, const uint8_t *r)
{
    memset(s, 0, sizeof *s);
    s->clock = RACE_CLOCK;
    s->time = RACE_TIME;
    s->state = (int32_t)(FIELD(r, STATE_AT, uint8_t *) - r);
    memcpy(s->pos, r + POS_AT, 16);
    memcpy(s->rot, r + ROT_AT, 48);
    memcpy(s->vel, r + VEL_AT, 16);
    memcpy(s->spin, r + SPIN_AT, 16);
    for (int w = 0; w < 2; w++) {
        const uint8_t *m = FIELD(r, w ? BACK_WHEEL_AT : FRONT_WHEEL_AT, uint8_t *);
        if (m) {
            s->wheel[w][0] = FIELD(m, 0x24, float);
            s->wheel[w][1] = FIELD(m, 0x28, float);
        }
    }
    s->contact[0] = r[FRONT_AT];
    s->contact[1] = r[BACK_AT];
    s->crash = (uint8_t)FIELD(r, CRASH_FLAGS_AT, uint32_t);
    s->crash_kind = (uint8_t)FIELD(r, CRASH_KIND_AT, int32_t);
    s->checkpoint = (uint8_t)FIELD(r, CHECKPOINT_AT, int32_t);
    s->attack = (uint8_t)FIELD(r, ATTACK_AT, int32_t);
    s->bottles = (uint8_t)FIELD(r, BOTTLES_AT, int32_t);
    memcpy(s->pad, r + PAD_AT, sizeof s->pad);
    s->slot = slot_of(r);
}

/* A snapshot arrived for slot (local rider numbering). Its time is on this PC's race clock (online:
   the moment it shows, by the shared server clock; DHD_PUPPET_PLAY: the recorded race
   time), so its age here is how far behind the present it already is. */
static void feed(int slot, const Snapshot *s)
{
    Puppet *p = &pp.p[slot];
    /* A new race (a replayed file starting over). Online, hle_puppet_start clears the pad mirror
       and the ring at each race's start, and hle_puppet_receive has already applied this
       snapshot's pad words to the mirror, so nothing here may clear it. */
    if (p->count && s->time < p->ring[p->count - 1].time - 1)
        p->count = 0, p->have_age = 0, p->synced = 0, p->used = 0;
    if (p->count && s->time <= p->ring[p->count - 1].time) return;
    double age = RACE_TIME - s->time;
    if (!p->have_age)
        p->age = age, p->have_age = 1;
    else
        p->age += (age - p->age) / 16;
    if (p->count == RING) memmove(p->ring, p->ring + 1, --p->count * sizeof *p->ring);
    p->ring[p->count++] = *s;
}

/* The newest snapshot whose moment has come on this game's race clock. One from ahead of it (a
   clock that is off, this game behind after a hitch) waits for its moment; NULL when none has
   come yet. */
static const Snapshot *current(const Puppet *p)
{
    for (int i = p->count - 1; i >= 0; i--)
        if (p->ring[i].time <= RACE_TIME) return &p->ring[i];
    return NULL;
}

static void snapshot_pose(const Snapshot *s, SyncPose *out)
{
    memcpy(out->pos, s->pos, 12);
    memcpy(out->vel, s->vel, 12);
    for (int i = 0; i < 3; i++) memcpy(out->rot[i], s->rot[i], 12);
}

static void body_pose(const uint8_t *r, SyncPose *out)
{
    memcpy(out->pos, r + POS_AT, 12);
    memcpy(out->vel, r + VEL_AT, 12);
    for (int i = 0; i < 3; i++) memcpy(out->rot[i], r + ROT_AT + 16 * i, 12);
}

/* The rigid body to pose (velocity and momentum together: mass times velocity). The position
   before this step (written by the bookkeeping pass, which the physics integrates from) moves
   with it, so a correction stays made. A jump (spin given) puts the previous position there too,
   so the physics' sweep starts there and not across the track, and takes the snapshot's angular
   velocity. */
static void set_body(uint8_t *r, const SyncPose *pose, const float *spin)
{
    float mass = FIELD(r, MASS_AT, float), momentum[3] = {pose->vel[0] * mass, pose->vel[1] * mass, pose->vel[2] * mass};
    float *pos = (float *)(r + POS_AT), *prev = (float *)(r + PREV_POS_AT);
    for (int k = 0; k < 3; k++) {
        prev[k] = spin ? pose->pos[k] : prev[k] + pose->pos[k] - pos[k];
        pos[k] = pose->pos[k];
    }
    for (int i = 0; i < 3; i++) memcpy(r + ROT_AT + 16 * i, pose->rot[i], 12);
    memcpy(r + VEL_AT, pose->vel, 12);
    memcpy(r + MOMENTUM_AT, momentum, 12);
    if (spin) memcpy(r + SPIN_AT, spin, 12);
}

/* The ground under the wheels that touch it: their contacts' normals added (NULL: in the air). */
static const float *ground_normal(const uint8_t *r, float out[3])
{
    int touching = 0;
    out[0] = out[1] = out[2] = 0;
    for (int w = 0; w < 2; w++) {
        const uint8_t *wheel = r + WHEEL_BODY_AT(w);
        if (wheel[CONTACT_STATE_AT] != 1) continue;
        const float *n = (const float *)(wheel + CONTACT_NORMAL_AT);
        for (int k = 0; k < 3; k++) out[k] += n[k];
        touching = 1;
    }
    return touching && isfinite(out[0] + out[1] + out[2]) ? out : NULL;
}

/* A puppet's pose before this step's physics (finding 202). A snapshot newer than the last one
   used is compared with where this puppet was at its moment (puppet_sync.h); the difference is
   then made up a little each step. A difference too large to be riding (a respawn, a long loss,
   the first snapshot after this game's crash of the rider) puts the rider where its snapshot
   says, carried to now, at once. */
static void steer(uint8_t *r, Puppet *p, const Snapshot *s)
{
    SyncPose now, owner;
    double t = RACE_TIME;
    float dt = p->stepped_at > 0 && t > p->stepped_at ? (float)(t - p->stepped_at) : 0;
    p->stepped_at = t;
    body_pose(r, &now);
    if (s->time > p->used) {
        p->used = s->time;
        snapshot_pose(s, &owner);
        double carry = fmin(fmax(0, t - s->time), SYNC_CARRY_MAX);
        SyncPose carried;
        sync_carry(&owner, s->spin, (float)carry, &carried);
        float error = sync_measure(&p->sync, s->time, &owner, &carried, &now);
        p->error = error;
        if (!p->synced || error > SYNC_RESYNC || (p->after_crash && error > SYNC_RESPAWN)) {
            set_body(r, &carried, s->spin);
            sync_restart(&p->sync, t - carry, &owner, s->spin, t);
            if (p->synced)
                fprintf(stderr, "puppet: tick %d slot %d put where its rider is (%.1f units off%s)\n", RACE_CLOCK,
                        slot_of(r), error, p->after_crash ? ", after a crash" : "");
            p->synced = 1, p->after_crash = 0, p->resyncs++;
            return;
        }
        p->after_crash = 0;
    }
    float normal[3];
    sync_step(&p->sync, dt, ground_normal(r, normal), &now);
    set_body(r, &now, NULL);
}

extern void func_0012C8B0(void *rider);  /* the input pass: the pad, or the AI controller */
static int to_global(int slot), owner_of(int slot);

/* Whether the rider a puppet shows is a computer rider (run by the host online; the bench's
   recorded AI riders). */
static int ai_rider(int slot)
{
    if (pp.play_from[slot] >= 0) return pp.play_from[slot] != 0;
    return pp.online && to_global(slot) >= pp.nslots;
}

static int puppet(const uint8_t *r)
{
    if (!pp.init) setup();
    int slot = slot_of(r);
    return slot >= 0 && slot < NRIDERS && pp.p[slot].on && pp.p[slot].count && RACING;
}

/* Instead of func_0012C8B0's pad or AI: the virtual pad as the remote rider had it at the newest
   snapshot. Last tick's actions are shifted here (as func_0017DCC8 does), so a press shows its
   edge once. Between snapshots the rider keeps its last inputs. */
int hle_puppet_input(uint8_t *r)
{
    const Snapshot *s;
    int slot = slot_of(r);
    if (!puppet(r) || !(s = current(&pp.p[slot]))) return 0;
    uint32_t *pad = (uint32_t *)(r + PAD_AT);
    /* A computer rider steers in its own States from its AI controller (+0x78D4), not from the
       pad (its pad words change two or three times a second): this game's AI controller runs for
       it as for any AI rider, along this track's racing line, and the owner's pad replaces what
       it pressed. The game's input pass shifts the pad itself. */
    if (ai_rider(slot)) func_0012C8B0(r);
    else memcpy(pad + PREV_FIRST, pad, 25 * 4);
    for (int i = 0; i < PAD_WORDS; i++)
        if (i < PREV_FIRST || i > PREV_LAST) pad[i] = s->pad[i];
    return 1;
}

/* Before func_0012D360's physics, which then runs for the puppet as for any rider: the body
   steered toward the snapshots, the State, crash, progress and power-ups as the newest says. */
int hle_puppet_physics(uint8_t *r)
{
    const Snapshot *s;
    int32_t now;
    int slot = slot_of(r);
    if (!puppet(r) || !(s = current(&pp.p[slot]))) return 0;
    Puppet *p = &pp.p[slot];
    p->shown = RACE_TIME;
    p->behind = RACE_TIME - s->time;
    /* A rider whose player left after finishing is run by no PC until the next race: it stays
       where its last snapshot put it (hle_puppet_drop), no physics. */
    if (pp.online && owner_of(slot) < 0 && !(FIELD(r, CRASH_FLAGS_AT, uint32_t) & 4)) {
        SyncPose last;
        float still[3] = {0, 0, 0};
        snapshot_pose(s, &last);
        memset(last.vel, 0, sizeof last.vel);
        set_body(r, &last, still);
        func_0012DDC0(r);
        return 1;
    }

    /* While it crashes the bike and the thrown body are their own bodies (+0x6420, +0x4EF0), run
       by the game's physics from the crash's start to its respawn; nothing is steered. While the
       snapshots say the owner's rider is crashing, nothing is steered either: the two crashes
       play out on their own, and the first snapshot after the owner's puts the rider there. */
    if (FIELD(r, CRASH_FLAGS_AT, uint32_t) & 4) {
        p->remote_crash = s->crash & 4;
        p->after_crash = 1, p->stepped_at = 0;
        return 0;
    }
    if (s->crash & 4) p->after_crash = 1, p->stepped_at = 0;
    else steer(r, p, s);
    /* A crash: started once, as the remote rider's began; then the crash runs its course. */
    int crashing = FIELD(r, CRASH_FLAGS_AT, uint32_t) & 4, remote = s->crash & 4;
    if (remote && !p->remote_crash && !crashing) {
        fprintf(stderr, "puppet: tick %d slot %d crash kind %d\n", RACE_CLOCK, slot, s->crash_kind);
        pp.allow = 1;
        func_00155E78(r + CRASH_AT, s->crash_kind);
        pp.allow = 0;
        crashing = FIELD(r, CRASH_FLAGS_AT, uint32_t) & 4;
    }
    p->remote_crash = remote;
    /* The State follows the snapshot: the game's own switches are blocked (hle_puppet_switch), as
       on an AI rider they flip back and forth against it every field. */
    now = (int32_t)(FIELD(r, STATE_AT, uint8_t *) - r);
    if (!crashing && !remote && s->state > 0 && s->state < RIDER_SIZE && now != s->state) {
        pp.allow = 1;
        func_0012FE38(r, r + s->state);
        pp.allow = 0;
    }
    /* Progress: the checkpoints passed follow the owner's (crossings here can differ by a tick
       or after a respawn), so places and the finish order agree. */
    if (FIELD(r, CHECKPOINT_AT, int32_t) != s->checkpoint) FIELD(r, CHECKPOINT_AT, int32_t) = s->checkpoint;
    /* Power-ups: the puppet attacks as its owner can (a kick shows as a kick) */
    FIELD(r, ATTACK_AT, int32_t) = s->attack;
    FIELD(r, BOTTLES_AT, int32_t) = s->bottles;
    return 0;
}

/* func_00155E78 (start a crash): not for a puppet, unless its snapshot says so. */
int hle_puppet_crash(uint8_t *r)
{
    return puppet(r) && !pp.allow;
}

/* func_0012FE38 (switch the State): a puppet's only as its snapshot says, or in a crash. */
int hle_puppet_switch(uint8_t *r, void *next)
{
    (void)next;  /* any State */
    return puppet(r) && !pp.allow && !(FIELD(r, CRASH_FLAGS_AT, uint32_t) & 4);
}

/* ---- online ---- */

/* Rider slots on this PC: its own rider is 0, the other humans 1.. in lobby order, the AI riders
   nslots.. (the same slots on every PC). On the wire a rider is named by its global id, the same on
   every PC: a human by their lobby slot, an AI rider by its slot. */
static int human_slot(int lobby) { return lobby == pp.local ? 0 : lobby < pp.local ? lobby + 1 : lobby; }
static int lobby_of(int slot) { return slot == 0 ? pp.local : slot <= pp.local ? slot - 1 : slot; }
static int to_local(int global) { return global < pp.nslots ? human_slot(global) : global; }
static int to_global(int slot) { return slot < pp.nslots ? lobby_of(slot) : slot; }
/* The PC that runs a rider: a human's own while connected, else the host (which also runs the AI);
   none (-1) for a player who left after finishing, until the next race (hle_puppet_drop). */
static int owner_of(int slot)
{
    if (slot >= pp.nslots || !pp.gone[lobby_of(slot)]) return slot < pp.nslots ? lobby_of(slot) : pp.host;
    return pp.kept[lobby_of(slot)] ? -1 : pp.host;
}
static int owned(int slot) { return owner_of(slot) == pp.local; }

void hle_puppet_online(int local, int nslots, int host)
{
    if (!pp.init) setup();
    pp.online = 1, pp.local = local, pp.nslots = nslots, pp.host = host;
    for (int i = 1; i < NRIDERS; i++) pp.p[i].on = !owned(i);
    fprintf(stderr, "puppet: online, player %d of %d (host %d); puppets:", local, nslots, host);
    for (int i = 1; i < NRIDERS; i++)
        if (pp.p[i].on) fprintf(stderr, " %d", i);
    fprintf(stderr, "\n");
}

/* An online race's start (online.c start_step): what the puppets had is from before it (its clock
   starts now). */
void hle_puppet_start(void)
{
    for (int i = 0; i < NRIDERS; i++) {  /* slot 0 too: this PC's rider starts afresh on every PC */
        Puppet *p = &pp.p[i];
        memset(&p->sent_pad, 0, sizeof p->sent_pad);
        /* Full refreshes (834 bytes) at different sends for different riders, so two seldom share
           a tick and one bundle (finding 169): the AI riders' 30 phases 3 apart, this PC's rider
           at its first send and then once a second (it sends 60 a second, the AI 30). */
        p->sent_pad.count = (unsigned)(30 - 3 * i) % 30;
        p->sent_pad.period = i ? 30 : 60;
        if (!i) continue;
        memset(p->mirror, 0, sizeof p->mirror);
        p->count = 0, p->have_age = 0, p->remote_crash = 0;
        p->synced = 0, p->used = 0, p->stepped_at = 0, p->after_crash = 0;
        sync_reset(&p->sync);
        if (i < pp.nslots && pp.kept[lobby_of(i)]) {  /* a new race: the host's AI rides it now */
            pp.kept[lobby_of(i)] = 0;
            p->on = !owned(i);
        }
    }
    pp.hits_taken = 0, pp.nevents = 0, pp.go_server = 0;
    pp.held_count = 0;
}

/* How far this game's race clock trails the wall clock beyond the normal (puppet_sync.h
   sync_clock_excess), seconds: added to the stamps it sends, taken off the ones it places. */
double port_fps_behind(void);  /* fps.c */
double hle_puppet_clock_excess(void) { return sync_clock_excess(port_fps_behind()); }

/* This PC's GO on the server clock (ms; online.c): from here snapshots stamped on that clock land on
   this PC's race clock. */
void hle_puppet_go(double go_server_ms) { pp.go_server = go_server_ms; }

/* The online race is over (online.c, back at the menus): no puppets, the next race is the game's own. */
void hle_puppet_offline(void)
{
    if (!pp.online) return;
    pp.online = 0;
    for (int i = 1; i < NRIDERS; i++)
        if (pp.play_from[i] < 0) memset(&pp.p[i], 0, sizeof pp.p[i]);
    memset(pp.gone, 0, sizeof pp.gone);
    memset(pp.kept, 0, sizeof pp.kept);
    pp.hits_taken = 0, pp.nevents = 0;
    fprintf(stderr, "puppet: offline\n");
}

/* A player left (lobby slot); host: who runs the AI now (the lowest still connected). The riders
   this PC now owns stop being puppets: their own AI and physics take over where they are, and
   from here on this PC sends their snapshots. A player who had already finished (finished: the
   race has their time) is not handed to the AI, which would race their rider on past the line:
   it stays a puppet on every PC, on its last snapshot, run by no PC until the next race. */
void hle_puppet_drop(int lobby, int host, int finished)
{
    if (!pp.online || lobby < 0 || lobby >= NET_SLOTS) return;
    pp.gone[lobby] = 1;
    pp.kept[lobby] = finished;
    pp.host = host;
    if (finished && lobby != pp.local)
        fprintf(stderr, "puppet: tick %d slot %d (player %d) had finished: it stays where its player left it\n",
                RACE_CLOCK, human_slot(lobby), lobby);
    for (int i = 1; i < NRIDERS; i++) {
        Puppet *p = &pp.p[i];
        if (p->on && owned(i)) {
            p->on = 0, p->count = 0;
            fprintf(stderr, "puppet: tick %d slot %d (player %d) is this PC's AI now\n", RACE_CLOCK, i, to_global(i));
        }
    }
}

/* func_0012C450 (a rider to its start), index: its place on the start grid (rider +0x7978). Each
   PC's own rider is slot 0; online it starts where the host has that player (the host's slot of
   the same lobby number), so the humans line up side by side as on the host. */
int hle_puppet_grid(uint8_t *r, int index)
{
    int slot = slot_of(r), want = index;
    /* Only when the race has no start-point list (func_001A0498 found none): the place is a node
       of the track's path. The human (pad 0) and the first AI rider (AI index 0) share node 0
       there, so online every rider takes the node of its global id: distinct, the same on every PC. */
    if (pp.online && slot >= 0 && slot < NRIDERS) want = to_global(slot);
    if (pp.online) fprintf(stderr, "puppet: slot %d starts at grid place %d (its own %d)\n", slot, want, index);
    return want;
}

/* func_0011AEC0, after every rider is placed: offline the game swaps each human with a random
   rider (func_00222BD8), so the player starts at a random gate. That draw differs from PC to PC,
   and every PC's human is its own slot 0: each PC put its player in another gate, a puppet's
   owner and an AI rider could share one, and when the snapshots began at GO the puppets jumped to
   where their owners were, onto other riders. Online the swap is skipped (the draw still happens):
   every rider stays on the start point of its global id (hle_puppet_start_point), so the humans
   line up side by side and the grid is the same on every PC. */
int hle_puppet_keep_grid(void)
{
    if (pp.online) fprintf(stderr, "puppet: online grid kept (no random start swap)\n");
    return pp.online;
}

/* func_0012C450 first asks the race (D_0043EEC0, func_001A0498) for its next start point: they
   go out in the order the riders are placed, slot 0 first. Online each PC's own rider is slot 0,
   so the point follows the rider's global id instead and every PC has the same grid. The first
   call still runs at point 0, which sets the start's orientation for all of them. */
int func_001A0498(void *race, float *pos, void *matrix);
void race_sync_hit(void);  /* autopad.c: h+N capture moments */

void hle_puppet_start_point(uint8_t *race, uint8_t *r)
{
    int32_t *index = (int32_t *)(race + 0xC), active = *(int32_t *)(race + 0x10);
    int slot = slot_of(r);
    if (!pp.online || !active || slot < 0) return;
    int want = to_global(slot);
    if (*index == 0 && want != 0) {
        float pos[4] __attribute__((aligned(16))), matrix[16] __attribute__((aligned(16)));
        func_001A0498(race, pos, matrix);
    }
    fprintf(stderr, "puppet: slot %d takes start point %d (in order %d)\n", slot, want, *index);
    *index = want;
}

static uint8_t *put(uint8_t *o, const void *v, size_t n) { memcpy(o, v, n); return o + n; }

/* One rider's record for a STATE bundle (state_bundle.h), for an owned slot: rider (global id),
   race time (ms), body, wheels, contacts, State, crash, checkpoints passed, attack kind, bottles,
   then repeated pad changes (six sends) with a full refresh every 30 sends. Last tick's actions
   do not travel; the full refresh bounds recovery after longer loss bursts. 0: not owned. */
int hle_puppet_pack(int slot, uint8_t *out, int max)
{
    if (!pp.online || slot < 0 || slot >= hle_online_race_riders() || !owned(slot) || !RACING || max < STATE_RECORD_MAX) return 0;
    Snapshot s;
    take(&s, rider(slot));
    uint8_t *o = out;
    uint16_t state = (uint16_t)s.state;
    int32_t ms = (int32_t)floor(s.time * 1000 + 0.5);
    *o++ = (uint8_t)to_global(slot);
    o = put(o, &ms, 4);
    o = put(o, s.pos, 12);
    for (int k = 0; k < 3; k++) o = put(o, s.rot[k], 12);
    o = put(o, s.vel, 12);
    o = put(o, s.spin, 12);
    o = put(o, s.wheel, 16);
    *o++ = s.contact[0], *o++ = s.contact[1];
    o = put(o, &state, 2);
    *o++ = s.crash, *o++ = s.crash_kind;
    *o++ = s.checkpoint;
    *o++ = s.attack, *o++ = s.bottles;
    o += pad_snapshot_pack(&pp.p[slot].sent_pad, s.pad, o);
    return (int)(o - out);
}

static const uint8_t *get(const uint8_t *in, void *v, size_t n) { memcpy(v, in, n); return in + n; }

/* One rider's record (its size checked) from the PC in lobby slot sender, showing server time
   stamp (ms). */
static void receive_record(int sender, const uint8_t *in, int n, double stamp)
{
    int slot = to_local(in[0]);
    if (slot <= 0 || slot >= hle_online_race_riders() || !pp.p[slot].on || owner_of(slot) != sender) return;
    Puppet *p = &pp.p[slot];
    const uint8_t *i = in + 1, *end = in + n;
    uint16_t state;
    int32_t ms;
    Snapshot s;
    memset(&s, 0, sizeof s);
    i = get(i, &ms, 4);
    s.clock = ms;  /* the sender's race time (ms), for the trace */
    s.time = (stamp - pp.go_server) / 1000 - hle_puppet_clock_excess();
    /* Reject stale unordered packets before touching the pad mirror. */
    if (p->count && s.time <= p->ring[p->count - 1].time) return;
    i = get(i, s.pos, 12);
    for (int k = 0; k < 3; k++) i = get(i, s.rot[k], 12);
    i = get(i, s.vel, 12);
    i = get(i, s.spin, 12);
    i = get(i, s.wheel, 16);
    s.contact[0] = *i++, s.contact[1] = *i++;
    i = get(i, &state, 2);
    s.state = state;
    s.crash = *i++, s.crash_kind = *i++;
    s.checkpoint = *i++;
    s.attack = *i++, s.bottles = *i++;
    if (!pad_snapshot_apply(p->mirror, i, (int)(end - i))) return;
    memcpy(s.pad, p->mirror, sizeof s.pad);
    feed(slot, &s);
}

/* A STATE bundle from the PC in lobby slot sender: every record, once the whole bundle checks. */
static void receive_bundle(int sender, const uint8_t *in, int n)
{
    double stamp;
    if (!pp.online || sender == pp.local || !pp.go_server || !state_bundle_check(in, n, &stamp) ||
        !isfinite(stamp)) return;
    stamp += pp.net_link.skew * 1000;  /* DHD_NET_LINK's clock error (tests; 0 otherwise) */
    for (int at = STATE_BUNDLE_HEADER; at < n;) {
        int size = state_record_size(in + at, n - at);
        receive_record(sender, in + at, size, stamp);
        at += size;
    }
}

/* Online STATE in. Under DHD_NET_LINK (tests: a bad network on the live path) a bundle is first
   held as the link says (lost, late, out of order) and released by net_link_release. */
void hle_puppet_receive(int sender, const uint8_t *in, int n)
{
    double arrives;
    if (!pp.init) setup();
    if (!pp.net_link.on) {
        receive_bundle(sender, in, n);
        return;
    }
    if (n > HELD_BYTES || pp.held_count == HELD_MAX || !link_pass(&pp.net_link, host_wall_seconds(), &arrives)) return;
    pp.held[pp.held_count].at = arrives;
    pp.held[pp.held_count].sender = sender;
    pp.held[pp.held_count].size = n;
    memcpy(pp.held[pp.held_count++].bytes, in, n);
}

/* DHD_NET_LINK: the held bundles whose moment has come, in the order they arrive. */
static void net_link_release(void)
{
    double now = host_wall_seconds();
    for (;;) {
        int next = -1;
        for (int i = 0; i < pp.held_count; i++)
            if (pp.held[i].at <= now && (next < 0 || pp.held[i].at < pp.held[next].at)) next = i;
        if (next < 0) return;
        int sender = pp.held[next].sender, size = pp.held[next].size;
        uint8_t bytes[HELD_BYTES];
        memcpy(bytes, pp.held[next].bytes, size);
        pp.held[next] = pp.held[--pp.held_count];
        receive_bundle(sender, bytes, size);
    }
}

/* func_0012E940 (rider self hit by other). A hit by a puppet is decided on its PC; a hit by a
   rider this PC runs on a puppet becomes an EVENT for the PC that runs the puppet (none for a
   finished rider whose player left). */
int hle_puppet_hit(uint8_t *self, int kind, void *direction, uint8_t *other)
{
    if (pp.allow || (!puppet(self) && !puppet(other))) return 0;
    int victim = slot_of(self), attacker = slot_of(other);
    if (pp.online && !puppet(other) && owner_of(victim) >= 0 && pp.nevents < MAX_EVENTS) {
        uint8_t *e = pp.events[pp.nevents], *o = e;
        *o++ = EVENT_VERSION;
        *o++ = EVENT_HIT;
        *o++ = (uint8_t)owner_of(victim);  /* the PC that applies it */
        *o++ = (uint8_t)to_global(victim);
        *o++ = (uint8_t)to_global(attacker);
        *o++ = (uint8_t)kind;
        o = put(o, direction, 12);
        pp.event_size[pp.nevents++] = (int)(o - e);
    }
    race_sync_hit();
    fprintf(stderr, "puppet: tick %d hit kind %d on slot %d by slot %d (%s)\n", RACE_CLOCK, kind, victim, attacker,
            puppet(other) ? "its PC decides" : "event for the victim's PC");
    return 1;
}

/* The next EVENT payload to send; 0 if none. */
int hle_puppet_take_event(uint8_t *out, int max)
{
    if (!pp.nevents || pp.event_size[0] > max) return 0;
    int n = pp.event_size[0];
    memcpy(out, pp.events[0], n);
    memmove(pp.events, pp.events + 1, --pp.nevents * sizeof pp.events[0]);
    memmove(pp.event_size, pp.event_size + 1, pp.nevents * sizeof pp.event_size[0]);
    return n;
}

/* A hit EVENT payload from the PC in lobby slot sender, on a rider this PC runs. */
void hle_puppet_event(int sender, const uint8_t *in, int n)
{
    if (!pp.online || n < 18 || in[0] != EVENT_VERSION || in[1] != EVENT_HIT || in[2] != pp.local || !RACING) return;
    int victim = to_local(in[3]), attacker = to_local(in[4]);
    float direction[4] __attribute__((aligned(16))) = {0, 0, 0, 0};
    if (victim < 0 || victim >= NRIDERS || attacker <= 0 || attacker >= NRIDERS || !owned(victim)) return;
    memcpy(direction, in + 6, 12);
    fprintf(stderr, "puppet: tick %d hit kind %d on slot %d by slot %d (event from player %d)\n", RACE_CLOCK, in[5],
            victim, attacker, sender);
    pp.hits_taken++;
    race_sync_hit();
    pp.allow = 1;
    func_0012E940(rider(victim), in[5], direction, rider(attacker));
    pp.allow = 0;
}

/* Per puppet: how far from where its rider was its last snapshot found it (units, finding 202),
   and its snapshots' usual age on arrival (ms, finding 169). As text, for the log. */
int hle_puppet_lag(char *out, int size)
{
    int n = 0;
    out[0] = 0;
    for (int i = 1; i < NRIDERS && n < size - 32; i++)
        if (pp.p[i].on && pp.p[i].have_age)
            n += snprintf(out + n, size - n, " %d:%.2f/%.0f", to_global(i), pp.p[i].error, pp.p[i].age * 1000);
    return n;
}

int hle_puppet_hits_taken(void) { return pp.hits_taken; }
int hle_puppet_global(int slot) { return to_global(slot); }
int hle_puppet_local(int global) { return to_local(global); }
int hle_puppet_owned(int slot) { return owned(slot); }

/* After the race step: record this step's snapshot, and the replayed ones arrive (the first of
   each RATE-th tick; above 60 a tick has several steps). */
/* The bench's receiving game's clock lag beyond the normal (its hitch; hle_puppet_clock_excess
   in a real game, which the bench's deterministic clock never has). */
static double bench_behind(void)
{
    return sync_clock_excess(link_hitch_wall(pp.link.rhitch_at, pp.link.rhitch_len, RACE_TIME) - RACE_TIME);
}

void hle_puppet_field(void)
{
    if (!pp.init) setup();
    if (!RACING) return;
    if (pp.record) {
        int first = pp.from < 0 ? 0 : pp.from, last = pp.from < 0 ? RACE_RIDERS : pp.from + 1;
        for (int i = first; i < last && i < NRIDERS; i++) {
            Snapshot s;
            take(&s, rider(i));
            s.ground[0] = ground_clearance(rider(i), 0);
            s.ground[1] = ground_clearance(rider(i), 1);
            fwrite(&s, sizeof s, 1, pp.record);
        }
        fflush(pp.record);
    }
    /* Each steered puppet's pose after the whole step (bumps included): what its next snapshots
       are compared with (puppet_sync.h). */
    for (int i = 1; i < NRIDERS; i++) {
        Puppet *p = &pp.p[i];
        if (p->on && p->synced && !(FIELD(rider(i), CRASH_FLAGS_AT, uint32_t) & 4)) {
            SyncPose pose;
            body_pose(rider(i), &pose);
            sync_record(&p->sync, RACE_TIME, &pose);
        }
    }
    /* Each puppet as it is drawn after the whole step (bumps included): race time here, the
       moment it shows, slot, position, the moment of its recorded rider it should show (the
       bench: the sender's race clock now, its hitch included), each wheel's clearance above the
       ground (nan: none within reach), 1 while it crashes, and its recorded rider (-1 online). */
    if (pp.trace) {
        for (int i = 1; i < NRIDERS; i++) {
            Puppet *p = &pp.p[i];
            if (!p->on || !p->count) continue;
            const float *pos = (const float *)(rider(i) + POS_AT);
            double truth = pp.playing ? link_hitch_race(pp.link.hitch_at, pp.link.hitch_len, RACE_TIME + bench_behind()) : RACE_TIME;
            fprintf(pp.trace, "%.6f %.6f %d %.5f %.5f %.5f %.6f %.4f %.4f %d %d\n", RACE_TIME, p->shown, i, pos[0],
                    pos[1], pos[2], truth, ground_clearance(rider(i), 0), ground_clearance(rider(i), 1),
                    !!(FIELD(rider(i), CRASH_FLAGS_AT, uint32_t) & 4), pp.play_from[i]);
        }
        fflush(pp.trace);
    }
    /* the replayed snapshots that have arrived, placed on this clock off by the link's skew */
    while (pp.delivery_next < pp.delivery_count && pp.deliveries[pp.delivery_next].at <= RACE_TIME) {
        const Delivery *d = &pp.deliveries[pp.delivery_next++];
        Snapshot s = pp.file[d->index];
        s.time += d->shift + pp.link.skew - bench_behind();
        for (int i = 1; i < NRIDERS; i++)
            if (pp.play_from[i] == s.slot) feed(i, &s);
    }
    net_link_release();
}

int hle_puppet_owner(int global) { return global >= 0 && global < NRIDERS ? owner_of(to_local(global)) : -1; }
