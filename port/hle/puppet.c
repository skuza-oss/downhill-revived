/* Puppet riders (finding 044): rider slots driven by snapshots of a rider on another PC instead
   of their own controller and physics, the base of state-sync online races (docs/ONLINE-PLAN.md).

   The race step (func_0011A100, patched in port/patches.py) runs five passes over the riders:
   input (func_0012C8B0: the pad or the AI writes the rider's virtual pad at +0xFE0), bookkeeping
   (func_0012D248), physics (func_0012D360), the state machine (func_0012D380: the current State
   at +0x4ECC picks the animation) and the rest (func_0012D3C8: effects, track progress, sound).
   A puppet skips the first and the third: hle_puppet_input writes the virtual pad from the
   snapshot, hle_puppet_physics the rigid body (position, orientation, velocities, wheel contacts
   and spin) and the State. The state machine, animation, progress and the rest then run on that,
   as for any rider, except that the State's own switches are blocked (func_0012FE38 asks
   hle_puppet_switch): a puppet is in the State its snapshot says.

   Crashes and hits belong to the puppet's own PC. The puppet never starts a crash by itself
   (func_00155E78 asks hle_puppet_crash) and takes or deals no hit here (func_0012E940 asks
   hle_puppet_hit; a hit by the local rider on a puppet becomes an event for its PC). When its
   snapshot's crash bit rises it starts the same crash, which then plays out here (the bike and
   the thrown body are simulated locally until the respawn).

   Snapshots arrive about 20 a second. Each puppet keeps the last few and shows the remote rider
   DELAY behind the newest it has (interpolated between the two around that moment, a short
   extrapolation when one is late); the remote clock is estimated from the arrivals. Every PC
   runs at its own frame rate (60-240), so the clocks here are race time in seconds, not ticks:
   a tick (the race clock's field) is 1001/60000 s at 60 and 1/60 s above, and above 60 a step
   is part of one (port_field_seconds, finding 114).

   Tests, one PC:
   DHD_PUPPET_RECORD=file   each race step, a snapshot of rider DHD_PUPPET_FROM (default 0)
   DHD_PUPPET_PLAY=file     replay those snapshots onto slot DHD_PUPPET_SLOT (default 1), one in
                            DHD_PUPPET_RATE (default 3) ticks arriving as it was taken
   DHD_PUPPET_DELAY=ms      how far behind the newest snapshot (default 100)
   DHD_PUPPET_CAMERA=1      player 0's camera follows the puppet (slot DHD_PUPPET_SLOT, online too)
   DHD_PUPPET_TRACE=file    each race step, each puppet as shown: race time here, the sender's
                            race time it shows, slot, position (text; tests, finding 114)
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
#define RACE_TIME port_field_seconds(RACE_CLOCK)      /* seconds since GO, part of a tick too */
#define RACE_PHASE (*(volatile int32_t *)0x0843F348)  /* on the race screen: 1 countdown, 2-4 race */
#define MENU (*(volatile int32_t *)0x0829DED8)        /* 9: the race screen */
#define RACING (MENU == 9 && RACE_PHASE >= 2 && RACE_PHASE <= 4)  /* func_0016B5A8 */


#define FIELD(r, off, type) (*(type *)((r) + (off)))
#define PAD_AT 0x1174    /* virtual pad (+0xFE0) +0x194: this tick's 25 actions, last tick's, timers */
#define PAD_WORDS 171
#define PREV_FIRST 25    /* words 25-49: last tick's actions, which the receiver shifts itself */
#define PREV_LAST 49
#define POS_AT 0x4270    /* rigid body (+0x40C0) +0x1B0: position */
#define ROT_AT 0x4280    /* +0x1C0: orientation, 3 rows */
#define VEL_AT 0x4180    /* +0xC0: velocity (units a second) */
#define SPIN_AT 0x4190   /* +0xD0: angular velocity */
#define MOMENTUM_AT 0x4120  /* +0x60: linear momentum, mass (+0x150) times velocity */
#define MASS_AT 0x4210
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
} Snapshot;

enum { RING = 32, MAX_EVENTS = 16, NET_SLOTS = 10 };
/* Wire format version 3: rider ids are global (a human's lobby slot, an AI rider's slot), the
   snapshot carries the checkpoints passed and the attack power-ups (version 2), and its clock is
   the sender's race time in ms, not its ticks (finding 114). STATE_FIXED: the payload before the
   pad words. */
enum { WIRE_VERSION = 3, STATE_FIXED = 104, EVENT_VERSION = 2, EVENT_HIT = 1 };  /* a hit: online.c's EVENT version */

typedef struct Puppet {
    int on;                  /* this slot is a puppet */
    Snapshot ring[RING];     /* by clock, oldest first */
    int count;
    double offset;           /* remote race time - local race time (s), smoothed over arrivals */
    int have_offset;
    uint32_t mirror[PAD_WORDS];  /* the remote virtual pad, as the deltas built it */
    int remote_crash;
    uint32_t sent_pad[PAD_WORDS];  /* owned slots: the pad as last sent */
    int sent_any;
} Puppet;

extern void func_0012DDC0(void *rider);              /* body to scene node */
extern void func_0012FE38(void *rider, void *state); /* Exit the current State, Enter this one */
extern void func_00155E78(void *crash, int kind);    /* start a crash */
extern void func_0012E940(void *self, int kind, void *direction, void *other);  /* other hits self */

static struct {
    int init, from, rate, play_slot;
    double delay;            /* seconds */
    int allow;               /* the puppet code itself starts this crash / applies this hit */
    FILE *record, *trace;
    Snapshot *file;          /* DHD_PUPPET_PLAY, in clock order */
    int file_count, file_next, file_fed;
    Puppet p[NRIDERS];
    /* online */
    int online, local, nslots, host;
    int gone[NET_SLOTS];     /* lobby slots that left */
    int kept[NET_SLOTS];     /* of those, the ones that had finished: their riders stay puppets */
    int hits_taken;
    uint8_t events[MAX_EVENTS][32];
    int event_size[MAX_EVENTS], nevents;
} pp;

static int slot_of(const uint8_t *r) { return (int)((r - RIDERS) / RIDER_SIZE); }
static uint8_t *rider(int slot) { return RIDERS + slot * RIDER_SIZE; }

static void setup(void)
{
    const char *s;
    pp.init = 1;
    pp.from = (s = getenv("DHD_PUPPET_FROM")) ? atoi(s) : 0;
    pp.rate = (s = getenv("DHD_PUPPET_RATE")) && atoi(s) > 0 ? atoi(s) : 3;
    pp.delay = ((s = getenv("DHD_PUPPET_DELAY")) ? atoi(s) : 100) / 1000.0;

    pp.play_slot = -1;
    if ((s = getenv("DHD_PUPPET_RECORD")) && *s && !(pp.record = fopen(s, "wb")))
        fprintf(stderr, "puppet: cannot write %s\n", s);
    if ((s = getenv("DHD_PUPPET_TRACE")) && *s && !(pp.trace = fopen(s, "w")))
        fprintf(stderr, "puppet: cannot write %s\n", s);
    pp.file_fed = -1;
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
        pp.play_slot = (s = getenv("DHD_PUPPET_SLOT")) ? atoi(s) : 1;
        if (pp.play_slot >= 1 && pp.play_slot < NRIDERS) pp.p[pp.play_slot].on = 1;
        fprintf(stderr, "puppet: %d snapshots onto slot %d, one in %d ticks, %.0f ms behind\n", pp.file_count,
                pp.play_slot, pp.rate, pp.delay * 1000);
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
}

/* A snapshot arrived for slot (local rider numbering). */
static void feed(int slot, const Snapshot *s)
{
    Puppet *p = &pp.p[slot];
    if (p->count && s->time < p->ring[p->count - 1].time - 1)
        p->count = 0, p->have_offset = 0;  /* a new race */
    if (p->count && s->time <= p->ring[p->count - 1].time) return;
    if (p->count == RING) memmove(p->ring, p->ring + 1, --p->count * sizeof *p->ring);
    p->ring[p->count++] = *s;
    double sample = s->time - RACE_TIME;
    if (!p->have_offset || sample > p->offset + 0.5 || sample < p->offset - 0.5)
        p->offset = sample, p->have_offset = 1;  /* first, or the remote clock jumped */
    else
        p->offset += 0.05 * (sample - p->offset);
}

static void lerp(float *out, const float *a, const float *b, float t, int n)
{
    for (int i = 0; i < n; i++)
        out[i] = a[i] + (b[i] - a[i]) * t;
}

/* Rows a and b blended, then made orthonormal again (Gram-Schmidt from the first row). */
static void blend_rotation(float out[3][4], const float a[3][4], const float b[3][4], float t)
{
    for (int i = 0; i < 3; i++)
        lerp(out[i], a[i], b[i], t, 3);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < i; j++) {
            float d = out[i][0] * out[j][0] + out[i][1] * out[j][1] + out[i][2] * out[j][2];
            for (int k = 0; k < 3; k++) out[i][k] -= d * out[j][k];
        }
        float n = sqrtf(out[i][0] * out[i][0] + out[i][1] * out[i][1] + out[i][2] * out[i][2]);
        for (int k = 0; k < 3; k++) out[i][k] /= n > 0 ? n : 1;
    }
}

/* What to show now: the remote rider pp.delay behind its newest snapshot (out->time: that moment
   on the remote race clock). */
static int wanted(int slot, Snapshot *out)
{
    Puppet *p = &pp.p[slot];
    if (!p->count) return 0;
    double t = RACE_TIME + p->offset - pp.delay;
    int i = p->count - 1;
    while (i > 0 && p->ring[i].time > t) i--;
    const Snapshot *a = &p->ring[i], *b = i + 1 < p->count ? &p->ring[i + 1] : NULL;
    *out = *a;
    out->time = t;
    if (b && t > a->time) {
        float f = (float)((t - a->time) / (b->time - a->time));
        lerp(out->pos, a->pos, b->pos, f, 3);
        lerp(out->vel, a->vel, b->vel, f, 3);
        lerp(out->spin, a->spin, b->spin, f, 3);
        blend_rotation(out->rot, a->rot, b->rot, f);
    } else if (!b && t > a->time) {  /* late: carry on along the velocity, up to 0.2 s */
        float dt = (float)(t - a->time > 0.2 ? 0.2 : t - a->time);
        for (int k = 0; k < 3; k++) out->pos[k] += a->vel[k] * dt;
    }
    return 1;
}

static int puppet(const uint8_t *r)
{
    if (!pp.init) setup();
    int slot = slot_of(r);
    return slot >= 0 && slot < NRIDERS && pp.p[slot].on && pp.p[slot].count && RACING;
}

/* Instead of func_0012C8B0's pad or AI: the virtual pad as the remote rider had it. Last tick's
   actions are shifted here (as func_0017DCC8 does), so a press shows its edge once. */
int hle_puppet_input(uint8_t *r)
{
    Snapshot s;
    if (!puppet(r) || !wanted(slot_of(r), &s)) return 0;
    uint32_t *pad = (uint32_t *)(r + PAD_AT);
    memcpy(pad + PREV_FIRST, pad, 25 * 4);
    for (int i = 0; i < PAD_WORDS; i++)
        if (i < PREV_FIRST || i > PREV_LAST) pad[i] = s.pad[i];
    return 1;
}

/* Instead of func_0012D360's physics: the rigid body and the State from the snapshot. */
int hle_puppet_physics(uint8_t *r)
{
    Snapshot s;
    int32_t now;
    int slot = slot_of(r);
    if (!puppet(r) || !wanted(slot, &s)) return 0;
    Puppet *p = &pp.p[slot];

    /* While it crashes the bike and the thrown body are their own bodies (+0x6420, +0x4EF0), run
       here from the crash's start; writing the rider's body would fight them. The game's physics
       runs the crash to its respawn. */
    if (FIELD(r, CRASH_FLAGS_AT, uint32_t) & 4) {
        p->remote_crash = s.crash & 4;
        return 0;
    }
    float mass = FIELD(r, MASS_AT, float), momentum[4] = {s.vel[0] * mass, s.vel[1] * mass, s.vel[2] * mass, 1};
    memcpy(r + POS_AT, s.pos, 12);
    for (int k = 0; k < 3; k++) memcpy(r + ROT_AT + 16 * k, s.rot[k], 12);
    memcpy(r + VEL_AT, s.vel, 12);
    memcpy(r + SPIN_AT, s.spin, 12);
    memcpy(r + MOMENTUM_AT, momentum, 12);
    r[FRONT_AT] = s.contact[0];
    r[BACK_AT] = s.contact[1];
    for (int w = 0; w < 2; w++) {
        uint8_t *m = FIELD(r, w ? BACK_WHEEL_AT : FRONT_WHEEL_AT, uint8_t *);
        if (m && (s.wheel[w][0] || s.wheel[w][1])) {
            FIELD(m, 0x24, float) = s.wheel[w][0];
            FIELD(m, 0x28, float) = s.wheel[w][1];
            FIELD(m, 0x34, float) = -s.wheel[w][1];
            FIELD(m, 0x38, float) = s.wheel[w][0];
        }
    }
    /* A crash: started once, as the remote rider's began; then the crash runs its course. */
    int crashing = FIELD(r, CRASH_FLAGS_AT, uint32_t) & 4, remote = s.crash & 4;
    if (remote && !p->remote_crash && !crashing) {
        fprintf(stderr, "puppet: tick %d slot %d crash kind %d\n", RACE_CLOCK, slot, s.crash_kind);
        pp.allow = 1;
        func_00155E78(r + CRASH_AT, s.crash_kind);
        pp.allow = 0;
        crashing = FIELD(r, CRASH_FLAGS_AT, uint32_t) & 4;
    }
    p->remote_crash = remote;
    /* The State follows the snapshot: the game's own switches are blocked (hle_puppet_switch), as
       on an AI rider they flip back and forth against it every field. */
    now = (int32_t)(FIELD(r, STATE_AT, uint8_t *) - r);
    if (!crashing && !remote && s.state > 0 && s.state < RIDER_SIZE && now != s.state) {
        pp.allow = 1;
        func_0012FE38(r, r + s.state);
        pp.allow = 0;
    }
    /* Progress: the checkpoints passed follow the owner's (crossings here can differ by a tick
       or after a respawn), so places and the finish order agree. */
    if (FIELD(r, CHECKPOINT_AT, int32_t) != s.checkpoint) FIELD(r, CHECKPOINT_AT, int32_t) = s.checkpoint;
    /* Power-ups: the puppet attacks as its owner can (a kick shows as a kick) */
    FIELD(r, ATTACK_AT, int32_t) = s.attack;
    FIELD(r, BOTTLES_AT, int32_t) = s.bottles;
    func_0012DDC0(r);
    if (pp.trace) fprintf(pp.trace, "%.6f %.6f %d %.5f %.5f %.5f\n", RACE_TIME, s.time, slot, s.pos[0], s.pos[1], s.pos[2]);
    return 1;
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
    for (int i = 1; i < NRIDERS; i++) {
        Puppet *p = &pp.p[i];
        p->count = 0, p->have_offset = 0, p->offset = 0, p->remote_crash = 0, p->sent_any = 0;
        if (i < pp.nslots && pp.kept[lobby_of(i)]) {  /* a new race: the host's AI rides it now */
            pp.kept[lobby_of(i)] = 0;
            p->on = !owned(i);
        }
    }
    pp.hits_taken = 0, pp.nevents = 0;
}

/* The online race is over (online.c, back at the menus): no puppets, the next race is the game's own. */
void hle_puppet_offline(void)
{
    if (!pp.online) return;
    pp.online = 0;
    for (int i = 1; i < NRIDERS; i++)
        if (i != pp.play_slot) memset(&pp.p[i], 0, sizeof pp.p[i]);
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
            p->on = 0, p->count = 0, p->sent_any = 0;
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

/* One STATE payload for an owned slot: version, rider (global id), race time (ms), body, wheels, contacts,
   State, crash, checkpoints passed, attack kind, bottles, then the virtual pad words changed since the last one sent (the
   relay keeps order and loses nothing; last tick's actions don't travel). 0: not owned. */
int hle_puppet_pack(int slot, uint8_t *out, int max)
{
    if (!pp.online || slot < 0 || slot >= hle_online_race_riders() || !owned(slot) || !RACING || max < 128) return 0;
    Puppet *p = &pp.p[slot];
    Snapshot s;
    take(&s, rider(slot));
    uint8_t *o = out, *count;
    uint16_t state = (uint16_t)s.state;
    int32_t ms = (int32_t)floor(s.time * 1000 + 0.5);
    *o++ = WIRE_VERSION;
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
    count = o++;
    *count = 0;
    for (int i = 0; i < PAD_WORDS && o + 5 <= out + max; i++)
        if ((i < PREV_FIRST || i > PREV_LAST) && (!p->sent_any || s.pad[i] != p->sent_pad[i])) {
            *o++ = (uint8_t)i;
            o = put(o, &s.pad[i], 4);
            p->sent_pad[i] = s.pad[i];
            (*count)++;
        }
    p->sent_any = 1;
    return (int)(o - out);
}

static const uint8_t *get(const uint8_t *in, void *v, size_t n) { memcpy(v, in, n); return in + n; }

/* A STATE payload from the PC in lobby slot sender. */
void hle_puppet_receive(int sender, const uint8_t *in, int n)
{
    if (!pp.online || n < STATE_FIXED || in[0] != WIRE_VERSION || sender == pp.local) return;
    int slot = to_local(in[1]);
    if (slot <= 0 || slot >= hle_online_race_riders() || !pp.p[slot].on) return;
    Puppet *p = &pp.p[slot];
    const uint8_t *i = in + 2, *end = in + n;
    uint16_t state;
    int32_t ms;
    Snapshot s;
    memset(&s, 0, sizeof s);
    i = get(i, &ms, 4);
    s.time = ms / 1000.0;
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
    for (int k = *i++; k > 0 && i + 5 <= end; k--) {
        int w = *i++;
        uint32_t v;
        i = get(i, &v, 4);
        if (w < PAD_WORDS) p->mirror[w] = v;
    }
    memcpy(s.pad, p->mirror, sizeof s.pad);
    feed(slot, &s);
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

/* How far behind each puppet shows its rider, in ms of race time: the delay plus the trip, plus
   or minus how far apart the two GOs were (finding 044). As text, for the log. */
int hle_puppet_lag(char *out, int size)
{
    int n = 0;
    out[0] = 0;
    for (int i = 1; i < NRIDERS && n < size - 32; i++)
        if (pp.p[i].on && pp.p[i].have_offset)
            n += snprintf(out + n, size - n, " %d:%.0f", to_global(i), (pp.delay - pp.p[i].offset) * 1000);
    return n;
}

int hle_puppet_hits_taken(void) { return pp.hits_taken; }
int hle_puppet_global(int slot) { return to_global(slot); }
int hle_puppet_local(int global) { return to_local(global); }
int hle_puppet_owned(int slot) { return owned(slot); }

/* After the race step: record this step's snapshot, and the replayed ones arrive (the first of
   each RATE-th tick; above 60 a tick has several steps). */
void hle_puppet_field(void)
{
    if (!pp.init) setup();
    if (!RACING) return;
    if (pp.record) {
        Snapshot s;
        take(&s, rider(pp.from));
        fwrite(&s, sizeof s, 1, pp.record);
        fflush(pp.record);
    }
    if (pp.trace) fflush(pp.trace);
    while (pp.play_slot > 0 && pp.file_next < pp.file_count && pp.file[pp.file_next].time <= RACE_TIME) {
        const Snapshot *s = &pp.file[pp.file_next++];
        if (s->clock % pp.rate == 0 && s->clock != pp.file_fed) feed(pp.play_slot, s), pp.file_fed = s->clock;
    }
}
