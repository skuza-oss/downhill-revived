/* A rider's record from another game (puppet_record.h, finding 220): its bytes both ways, and what a
   receiver refuses. */
#include "puppet_record.h"
#include <assert.h>
#include <stdio.h>

/* A real rider's record: riding along x at 95 units a second, upright, on its racing line. */
static PuppetRecord riding(void)
{
    PuppetRecord r;
    memset(&r, 0, sizeof r);
    r.rider = 4, r.ms = 12345;
    r.pos[0] = 1520.5f, r.pos[1] = -880.25f, r.pos[2] = 140.0f;
    r.rot[0][0] = r.rot[1][1] = r.rot[2][2] = 1;
    r.vel[0] = 95, r.spin[2] = 0.3f;
    r.power = 1, r.turning = 0.08f;
    r.node = 120, r.next = 121;
    r.target[0] = 1600, r.target[1] = -870, r.target[2] = 138;
    r.after[0] = 1680, r.after[1] = -860, r.after[2] = 136;
    r.offset[0] = 2.5f, r.offset_to[0] = 3;
    r.contact[0] = r.contact[1] = 1;
    r.state = 0x4ED0;
    r.checkpoint = 3;
    return r;
}

/* The record's bytes with a full pad update (as hle_puppet_pack makes them). */
static int bytes(const PuppetRecord *r, const uint32_t *pad, uint8_t *out)
{
    PadSender sender;
    memset(&sender, 0, sizeof sender);
    uint8_t *o = puppet_record_write(r, out);
    assert(o - out == STATE_RECORD_FIXED - 1);
    o += pad_snapshot_pack(&sender, pad, o);
    return (int)(o - out);
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t next_random(void)
{
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

/* Random and mutated bundles through the receive path's checks (state_bundle_check, then each
   record read and taken as puppet.c receive_record does): nothing may read outside the bundle
   (the guest's memory is bounds-checked; port/tools/test_runtime.py runs it under the address
   sanitizer on the host as well), a taken record holds only sane values, and a refused one leaves
   the mirror as it was. Returns the bundles that passed the bundle check. */
static int fuzz(int rounds, int *taken_out, int *refused_out)
{
    static uint8_t recs[3][STATE_RECORD_MAX], out[STATE_BUNDLE_RIDERS][STATE_BUNDLE_MAX], buf[STATE_BUNDLE_MAX + 64];
    uint32_t mirror[PAD_WORDS] = {0}, before[PAD_WORDS], shown[PAD_WORDS];
    int bundles_ok = 0, taken = 0, refused = 0;
    for (int round = 0; round < rounds; round++) {
        /* a real bundle of up to three real records with random pads */
        const uint8_t *rp[3];
        int size[3], bundle_size[STATE_BUNDLE_RIDERS], count = 1 + (int)(next_random() % 3);
        for (int i = 0; i < count; i++) {
            uint32_t pad[PAD_WORDS] = {0};
            for (int w = 0; w < PAD_WORDS; w++)
                if (next_random() % 8 == 0) pad[w] = w < PREV_FIRST ? next_random() % 2 : next_random() % 5000;
            PuppetRecord r = riding();
            r.rider = (uint8_t)(1 + i);
            size[i] = bytes(&r, pad, recs[i]);
            rp[i] = recs[i];
        }
        int nb = state_bundle_pack(rp, size, count, 1.7e12 + round, out, bundle_size);
        int n = bundle_size[0];
        memcpy(buf, out[0], n);
        /* mutate: flip bytes, write random words (floats among them: NaN, inf, huge), cut, grow, or all random */
        switch (next_random() % 6) {
        case 0: for (int k = 0, f = 1 + (int)(next_random() % 8); k < f; k++) buf[next_random() % n] ^= (uint8_t)(1u << (next_random() % 8)); break;
        case 1: for (int k = 0, f = 1 + (int)(next_random() % 4); k < f; k++) {
                    static const uint32_t words[] = {0x7FC00000u, 0x7F800000u, 0xFF800000u, 0x7F7FFFFFu, 0x4E6E6B28u, 0};
                    uint32_t v = next_random() % 2 ? words[next_random() % 6] : next_random();
                    int at = (int)(next_random() % (unsigned)(n - 4));
                    memcpy(buf + at, &v, 4);
                }
                break;
        case 2: n = (int)(next_random() % (unsigned)(n + 1)); break;
        case 3: for (int k = n; k < n + 64 && k < (int)sizeof buf; k++) buf[k] = (uint8_t)next_random();
                n += (int)(next_random() % 64);
                if (n > (int)sizeof buf) n = sizeof buf;
                break;
        case 4: for (int k = 0; k < n; k++) buf[k] = (uint8_t)next_random(); break;
        default: break;  /* unchanged: it must be taken */
        }
        (void)nb;
        double stamp;
        if (!state_bundle_check(buf, n, &stamp)) continue;
        bundles_ok++;
        for (int at = STATE_BUNDLE_HEADER; at < n;) {
            int rs = state_record_size(buf + at, n - at);
            assert(rs > 0 && at + rs <= n);
            PuppetRecord r;
            assert(puppet_record_read(buf + at, rs, &r));
            memcpy(before, mirror, sizeof before);
            const char *why = puppet_record_take(&r, mirror, 1300, shown);
            if (why) {
                refused++;
                assert(!memcmp(before, mirror, sizeof before));
            } else {
                taken++;
                assert(puppet_finite(r.pos, 3) && puppet_finite(&r.rot[0][0], 9) && puppet_finite(r.vel, 3) &&
                       puppet_finite(r.spin, 3) && puppet_finite(r.target, 3) && puppet_finite(r.after, 3) &&
                       isfinite(r.power) && isfinite(r.turning) && puppet_state_known(r.state) &&
                       puppet_node_ok(r.node, 1300) && puppet_node_ok(r.next, 1300) && r.checkpoint <= PUPPET_CHECKPOINT_MAX);
                for (int w = 0; w < PREV_FIRST; w++) assert(mirror[w] <= PUPPET_PAD_ACTION_MAX);
            }
            at += rs;
        }
    }
    *taken_out = taken, *refused_out = refused;
    return bundles_ok;
}

int main(void)
{
    static uint8_t rec[STATE_RECORD_MAX];
    uint32_t pad[PAD_WORDS] = {0};
    pad[7] = 1, pad[60] = 0x3F800000u;
    PuppetRecord r = riding(), back;
    int n = bytes(&r, pad, rec);
    assert(state_record_size(rec, n) == n);
    assert(puppet_record_read(rec, n, &back));
    assert(back.rider == r.rider && back.ms == r.ms && !memcmp(back.pos, r.pos, sizeof r.pos) &&
           !memcmp(back.rot, r.rot, sizeof r.rot) && !memcmp(back.vel, r.vel, sizeof r.vel) &&
           !memcmp(back.spin, r.spin, sizeof r.spin) && back.power == r.power && back.turning == r.turning &&
           back.node == r.node && back.next == r.next && !memcmp(back.target, r.target, sizeof r.target) &&
           !memcmp(back.after, r.after, sizeof r.after) && !memcmp(back.offset, r.offset, sizeof r.offset) &&
           !memcmp(back.offset_to, r.offset_to, sizeof r.offset_to) && back.contact[1] == 1 && back.state == r.state &&
           back.checkpoint == r.checkpoint);
    assert(back.pad == rec + STATE_RECORD_FIXED - 1 && back.pad_size == n - (STATE_RECORD_FIXED - 1));
    uint32_t mirror[PAD_WORDS] = {0};
    assert(pad_snapshot_apply(mirror, back.pad, back.pad_size) && mirror[7] == 1 && mirror[60] == 0x3F800000u);
    assert(!puppet_record_read(rec, STATE_RECORD_FIXED - 1, &back));  /* shorter than a record */
    printf("puppet record: %d bytes both ways\n", n);

    /* the racing line: only the loaded track's nodes (the review's node=2050, next=2050: the graph's
       own fields past its 0x800 nodes; an unused node of a shorter track) */
    {
        assert(puppet_node_ok(120, 1300) && puppet_node_ok(1299, 1300) && puppet_node_ok(PUPPET_LINE_NONE, 1300));
        assert(!puppet_node_ok(1300, 1300) && !puppet_node_ok(2050, 1300));
        assert(!puppet_node_ok(2050, 5000) && !puppet_node_ok(0x800, 0x7FFFFFFF) && puppet_node_ok(0x7FF, 5000));
        assert(!puppet_node_ok(0, 0) && !puppet_node_ok(0, -3) && puppet_node_ok(PUPPET_LINE_NONE, -3));
        assert(puppet_node_count(-1) == 0 && puppet_node_count(900) == 900 && puppet_node_count(1 << 30) == 0x800);
    }
    /* the State: only the rider's own State objects (the review's state=1: unaligned bytes inside
       the rider taken as an object whose Enter is called); every listed one passes */
    {
        assert(PUPPET_STATE_COUNT == 18);
        for (int i = 0; i < PUPPET_STATE_COUNT; i++) assert(puppet_state_known(PUPPET_STATES[i]));
        assert(puppet_state_known(0x4ED0) && puppet_state_known(0x5FE0) && puppet_state_known(0x6330));
        assert(!puppet_state_known(1) && !puppet_state_known(0) && !puppet_state_known(0x4ED1) &&
               !puppet_state_known(0x4ECC) && !puppet_state_known(0x4EF0) && !puppet_state_known(0x7BCF) &&
               !puppet_state_known(0xFFFF));
    }

    /* numbers: each bad one refused, the record as a real rider sends it taken, and a refused
       record leaves the pad mirror as it was */
    {
        const int32_t nodes = 1300;
        uint32_t mirror[PAD_WORDS] = {0}, before[PAD_WORDS], shown[PAD_WORDS];
        PuppetRecord ok = riding(), bad;
        int m = bytes(&ok, pad, rec);
        assert(puppet_record_read(rec, m, &ok));
        assert(puppet_record_take(&ok, mirror, nodes, shown) == NULL && mirror[7] == 1 && shown[60] == 0x3F800000u);
        memset(mirror, 0, sizeof mirror);
#define REFUSED(change, want)                                                                                     \
        do {                                                                                                      \
            bad = ok;                                                                                             \
            change;                                                                                               \
            memcpy(before, mirror, sizeof before);                                                                \
            const char *why = puppet_record_take(&bad, mirror, nodes, shown);                                     \
            assert(why && !strcmp(why, want) && !memcmp(before, mirror, sizeof before));                          \
        } while (0)
        REFUSED(bad.pos[0] = NAN, "not finite");
        REFUSED(bad.pos[2] = INFINITY, "not finite");
        REFUSED(bad.target[0] = NAN, "not finite");          /* the review's: a valid node, a NaN target */
        REFUSED(bad.target[1] = -INFINITY, "not finite");
        REFUSED(bad.after[2] = NAN, "not finite");
        REFUSED(bad.offset[1] = NAN, "not finite");
        REFUSED(bad.offset_to[0] = INFINITY, "not finite");
        REFUSED(bad.rot[1][2] = NAN, "not finite");
        REFUSED(bad.vel[1] = NAN, "not finite");
        REFUSED(bad.spin[0] = INFINITY, "not finite");
        REFUSED(bad.power = NAN, "not finite");
        REFUSED(bad.turning = NAN, "not finite");
        REFUSED(bad.pos[1] = 3e5f, "position");
        REFUSED((bad.vel[0] = 4000, bad.vel[1] = 4000), "speed");
        REFUSED(bad.spin[2] = 600, "spin");
        REFUSED(memset(bad.rot, 0, sizeof bad.rot), "orientation");                         /* degenerate */
        REFUSED(memcpy(bad.rot[1], bad.rot[0], sizeof bad.rot[0]), "orientation");          /* two rows alike */
        REFUSED(bad.rot[2][2] = 3, "orientation");                                           /* not a unit row */
        REFUSED(bad.rot[2][2] = -1, "orientation");                                          /* mirrored */
        REFUSED((bad.rot[1][0] = 0.7071f, bad.rot[1][1] = 0.7071f, bad.rot[2][0] = 0.7071f, bad.rot[2][1] = 0.7071f,
                 bad.rot[2][2] = 0), "orientation");                                         /* two rows alike, unit */
        REFUSED(bad.power = 50, "power");
        REFUSED(bad.turning = -1e6f, "power");
        REFUSED(bad.target[0] = 1e9f, "racing line");
        REFUSED(bad.offset[0] = 5000, "racing line");
        REFUSED(bad.node = 2050, "racing line");
        REFUSED(bad.next = 2050, "racing line");
        REFUSED(bad.state = 1, "state");
        REFUSED(bad.contact[0] = 3, "counts");
        REFUSED(bad.crash_kind = 11, "counts");
        REFUSED(bad.checkpoint = 36, "counts");
        REFUSED(bad.attack = 5, "counts");
        REFUSED(bad.bottles = 4, "counts");
        /* a pad action word past a byte; the update itself is checked on a copy too */
        {
            uint32_t wild[PAD_WORDS] = {0};
            static uint8_t other[STATE_RECORD_MAX];
            wild[3] = 0x7FC00000u;  /* a NaN's bits in a button word */
            int k = bytes(&ok, wild, other);
            assert(puppet_record_read(other, k, &bad));
            memcpy(before, mirror, sizeof before);
            assert(!strcmp(puppet_record_take(&bad, mirror, nodes, shown), "pad") && !memcmp(before, mirror, sizeof before));
            other[STATE_RECORD_FIXED] = 30;  /* the first pair's word: last tick's, never sent */
            assert(puppet_record_read(other, k, &bad));
            assert(!strcmp(puppet_record_take(&bad, mirror, nodes, shown), "pad") && !memcmp(before, mirror, sizeof before));
        }
        /* what real riders send, at the edges of what the bench's recordings showed: taken */
        bad = ok, bad.pos[2] = -11240, bad.vel[0] = 262, bad.spin[0] = 22.2f, bad.power = 0.75f, bad.turning = 1.04f;
        bad.rot[0][0] = 0.911f, bad.offset[0] = -21, bad.checkpoint = 35, bad.attack = 4, bad.bottles = 255, bad.crash_kind = 10;
        bad.node = PUPPET_LINE_NONE, bad.next = PUPPET_LINE_NONE, bad.state = 0x5FE0, bad.contact[1] = 2;
        assert(puppet_record_take(&bad, mirror, nodes, shown) == NULL);
        uint32_t steer[PAD_WORDS] = {0};
        steer[7] = 127, steer[8] = 0, steer[80] = 1173, steer[154] = 16777216, steer[96] = 4296;  /* analogue, timers, flags */
        static uint8_t third[STATE_RECORD_MAX];
        int k = bytes(&ok, steer, third);
        assert(puppet_record_read(third, k, &bad) && puppet_record_take(&bad, mirror, nodes, shown) == NULL && mirror[7] == 127);
#undef REFUSED
    }

    /* stamps from the future (the review's finding 5) */
    {
        const double day = 86400;
        double newest = 10.0, now = 10.1;
        /* a snapshot a day ahead is refused; the next normal one is taken (it is newer than the newest) */
        assert(!puppet_stamp_ok(now + day, now) && !puppet_stamp_ok(NAN, now) && !puppet_stamp_ok(INFINITY, now));
        assert(puppet_stamp_ok(now + 0.9, now) && puppet_stamp_ok(now - 0.2, now));
        double next = now - 0.08;  /* stamped 80 ms ago: on its way that long */
        assert(puppet_stamp_ok(next, now) && puppet_snapshot_new(next, 1, newest));
        /* (taken, the day-ahead one would have been the newest: every honest one after it stale) */
        assert(!puppet_snapshot_new(next, 1, now + day));

        /* HOLD: about the newest snapshot here, sane */
        assert(puppet_hold_ok(newest, 0.3, 1, newest, newest + 0.35) == PUPPET_HOLD_TAKEN);
        assert(puppet_hold_ok(newest + 0.1, 0.3, 1, newest, newest + 0.45) == PUPPET_HOLD_TAKEN);  /* its last bundles lost */
        assert(puppet_hold_ok(newest - 0.5, 0.3, 1, newest, newest + 0.35) == PUPPET_HOLD_OVER);  /* a newer one is here */
        assert(puppet_hold_ok(newest + day, 0.25, 1, newest, now) == PUPPET_HOLD_REFUSED);        /* the review's: a day ahead */
        assert(puppet_hold_ok(newest + 0.6, 0.3, 1, newest, newest + 0.7) == PUPPET_HOLD_REFUSED); /* not about these snapshots */
        assert(puppet_hold_ok(newest, 60, 1, newest, newest + 0.35) == PUPPET_HOLD_REFUSED);      /* stopped longer than it is old */
        assert(puppet_hold_ok(newest, NAN, 1, newest, now) == PUPPET_HOLD_REFUSED &&
               puppet_hold_ok(NAN, 0.3, 1, newest, now) == PUPPET_HOLD_REFUSED &&
               puppet_hold_ok(newest, 0.3, 0, 0, now) == PUPPET_HOLD_REFUSED);

        /* A HOLD a day ahead, then normal snapshots: it is refused, so the rider is never held. Its
           release on its own: a HOLD taken 0.4 s ahead of the newest snapshot, then the sender's
           snapshots come again with stamps that do not pass it for seconds (a broken clock): before,
           held_step never saw one newer than the hold and the rider stood for the race; now it rides
           SYNC_COAST after they began to arrive. */
        PuppetHold h = {0};
        assert(puppet_hold_ok(newest + day, 0.3, 1, newest, now) != PUPPET_HOLD_TAKEN);
        assert(!h.held);
        double t = newest + 0.4, clock = newest + 0.7;
        assert(puppet_hold_ok(t, 0.3, 1, newest, clock) == PUPPET_HOLD_TAKEN);
        puppet_hold_start(&h, t);
        double fed_from = 0, released = 0;
        for (int step = 0; step < 60 * 10 && !released; step++) {
            clock += 1 / 60.0;
            double stamp = newest + 0.001 * step;  /* a clock that barely runs: behind the hold for 6 s */
            if (step >= 30 && step % 2 == 0) {  /* from 0.5 s on, 30 a second */
                assert(puppet_stamp_ok(stamp, clock));
                if (!fed_from) fed_from = clock;
                puppet_hold_fed(&h, clock);
            }
            if (puppet_hold_resumed(&h, stamp) || puppet_hold_fed_long(&h, clock)) released = clock;
        }
        assert(released > 0 && fabs(released - fed_from - SYNC_COAST) < 0.05);
        /* and with honest stamps it resumes at the first snapshot past the hold, as before */
        PuppetHold g = {0};
        puppet_hold_start(&g, newest);
        assert(!puppet_hold_resumed(&g, newest) && puppet_hold_resumed(&g, newest + 1 / 30.0));
        assert(!puppet_hold_fed_long(&g, newest + 100));  /* nothing fed: no release by time alone */
    }

    {
        enum { ROUNDS = 200000 };
        int taken, refused, passed = fuzz(ROUNDS, &taken, &refused);
        assert(taken > 0 && refused > 0);
        printf("puppet record: %d random and mutated bundles, %d passed the bundle check; %d records taken, %d refused\n",
               ROUNDS, passed, taken, refused);
    }
    printf("puppet record: OK\n");
    return 0;
}
