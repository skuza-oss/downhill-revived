/* The measurement payload must survive the real state-bundle validator, while never
   naming a real rider. It must also be distinguishable from ordinary gameplay. */
#include "bench.h"
#include "../hle/state_bundle.h"
#include <assert.h>

int main(void)
{
    uint8_t frame[BENCH_STATE_BYTES];
    for (int kind = 0; kind < 2; kind++) {
        size_t n = bench_packet(frame, kind, 1, 42, 1234, 5678);
        assert(bench_match(frame, n));
        assert(!bench_match(frame, n - 1));
        if (kind) {
            double stamp;
            assert(state_bundle_check(frame + 2, (int)n - 2, &stamp) == 1);
            assert(stamp == 5678);
            assert(frame[13] >= 10); /* no real rider can be modified */
        }
        frame[kind ? 14 : 3] ^= 1;
        assert(!bench_match(frame, n));
    }
    NetBench b = {0};
    bench_open(&b, NULL); assert(!b.file);
    puts("test_bench: valid isolated state probes, event probes and opt-in guard passed");
    return 0;
}
