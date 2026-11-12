/* STATE bundles (state_bundle.h, finding 169): every bundle fits one datagram, the riders' full pad
   refreshes fall on different sends, and a damaged bundle is refused whole. */
#include "pad_snapshot.h"
#include "state_bundle.h"
#include <assert.h>
#include <stdio.h>

/* A record as puppet.c makes one: global id, the fixed part, then the pad update. */
static int record(uint8_t *out, int rider, PadSender *sender, const uint32_t *pad)
{
    memset(out, 0, STATE_RECORD_FIXED - 1);
    out[0] = (uint8_t)rider;
    return STATE_RECORD_FIXED - 1 + pad_snapshot_pack(sender, pad, out + STATE_RECORD_FIXED - 1);
}

int main(void)
{
    static uint8_t rec[10][STATE_RECORD_MAX], out[STATE_BUNDLE_RIDERS][STATE_BUNDLE_MAX];
    static PadSender sender[10];
    uint32_t pad[10][PAD_WORDS] = {{0}};
    const uint8_t *recs[10];
    int size[10], bundle_size[STATE_BUNDLE_RIDERS], worst = 0, most = 0, full_ticks = 0;

    /* A host's tick: its rider and nine AI riders, each with eight pad words changing every tick
       (the measured median, finding 158), phases as hle_puppet_start sets them. */
    for (int i = 0; i < 10; i++) sender[i].count = (unsigned)(30 - 3 * i) % 30;
    for (int tick = 0; tick < 600; tick++) {
        int fulls = 0;
        for (int i = 0; i < 10; i++) {
            for (int w = 0; w < 8; w++) pad[i][w] = (uint32_t)(tick * 31 + i * 7 + w);
            size[i] = record(rec[i], i, &sender[i], pad[i]);
            recs[i] = rec[i];
            fulls += size[i] == STATE_RECORD_MAX;
        }
        assert(fulls <= 1);  /* spread: never two full refreshes in one tick */
        full_ticks += fulls;
        int bundles = state_bundle_pack(recs, size, 10, 1.7e12 + tick, out, bundle_size);
        int riders = 0;
        for (int b = 0; b < bundles; b++) {
            double stamp;
            assert(bundle_size[b] <= STATE_BUNDLE_MAX && out[b][1] == b);
            assert(state_bundle_check(out[b], bundle_size[b], &stamp) == out[b][2] && stamp == 1.7e12 + tick);
            riders += out[b][2];
            if (bundle_size[b] > worst) worst = bundle_size[b];
        }
        assert(riders == 10);
        if (bundles > most) most = bundles;
    }
    assert(full_ticks == 200);  /* each rider once in 30 sends */
    assert(most <= 3);
    printf("state bundles: 10 riders a tick in at most %d bundles, largest %d of %d bytes\n", most, worst,
           STATE_BUNDLE_MAX);

    {   /* the whole budget: 2 + 8 bytes of frame and race marker, then the payload */
        assert(STATE_BUNDLE_MAX + 2 + 8 == STATE_WIRE_BUDGET && STATE_WIRE_BUDGET <= 1150 - 48 - 40);
    }
    {   /* a damaged bundle is refused before any record is read */
        double stamp;
        size[0] = record(rec[0], 3, &sender[0], pad[0]);
        int n = state_bundle_pack(recs, size, 1, 5.0, out, bundle_size);
        assert(n == 1 && state_bundle_check(out[0], bundle_size[0], &stamp) == 1);
        assert(!state_bundle_check(out[0], bundle_size[0] - 1, &stamp));    /* cut short */
        out[0][2] = 2;
        assert(!state_bundle_check(out[0], bundle_size[0], &stamp));        /* wrong count */
        out[0][2] = 1, out[0][0] = 5;
        assert(!state_bundle_check(out[0], bundle_size[0], &stamp));        /* an older version */
        out[0][0] = STATE_BUNDLE_VERSION, out[0][STATE_BUNDLE_HEADER + STATE_RECORD_FIXED - 1] = 200;
        assert(!state_bundle_check(out[0], bundle_size[0], &stamp));        /* a pad count past 146 */
    }
    /* HOLD (finding 202): its own version and index; a game that knows only bundles drops it */
    {
        uint8_t hold[STATE_HOLD_SIZE];
        double stamp = 0, last = 1762000000123.5, quiet = 0;
        assert(state_hold_make(hold, last, 260.4) == STATE_HOLD_SIZE && state_hold_check(hold, STATE_HOLD_SIZE, &stamp, &quiet) &&
               stamp == last && quiet == 260);
        assert(!state_bundle_check(hold, STATE_HOLD_SIZE, &stamp));         /* not a bundle */
        assert(!state_hold_check(hold, STATE_HOLD_SIZE - 1, &stamp, &quiet)); /* cut short */
        assert(state_hold_make(hold, last, 1e9) && state_hold_check(hold, STATE_HOLD_SIZE, &stamp, &quiet) && quiet == 65535);
        hold[1] = 0;
        assert(!state_hold_check(hold, STATE_HOLD_SIZE, &stamp, &quiet));   /* a bundle's index */
        assert(STATE_HOLD_INDEX < STATE_BUNDLE_RIDERS && STATE_HOLD_INDEX >= 3);  /* taken, and past a tick's bundles */
    }
    puts("state bundles: OK");
    return 0;
}
