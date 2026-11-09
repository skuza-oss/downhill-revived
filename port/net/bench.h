/* Opt-in transport measurements: DHD_NET_BENCH=CSV. No game state or clock synchronization.
   Probes use the same STATE/EVENT routes as gameplay and are consumed before the game sees them.
   A STATE probe is a valid v6 bundle with an out-of-range rider id and reserved bundle index 9.
   Measure RTT on the originating monotonic clock; never compare wall clocks between machines.
   Calls are serialized by NetClient's lock. */
#ifndef NET_BENCH_H
#define NET_BENCH_H
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

enum { BENCH_RING = 2048, BENCH_STATE_BYTES = 116, BENCH_EVENT_BYTES = 98 };
typedef struct {
    FILE *file;
    double race, next, flush_at;
    uint32_t sequence[2];
    double sent[2][BENCH_RING];
} NetBench;

static inline double bench_epoch_ms(void)
{
    struct timespec t;
    if (timespec_get(&t, TIME_UTC) != TIME_UTC) return 0;
    return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6;
}

static inline void bench_row(NetBench *b, const char *event, double now, const char *kind,
                      unsigned seq, int peer, size_t bytes, double value, double aux)
{
    fprintf(b->file, "%s,%.6f,%.0f,%s,%u,%d,%zu,%.6f,%.6f\n",
            event, now, b->race, kind, seq, peer, bytes, value, aux);
    if (now >= b->flush_at) { fflush(b->file); b->flush_at = now + 1000; }
}

static inline void bench_open(NetBench *b, const char *path)
{
    if (!path || !*path) return;
    b->file = fopen(path, "w");
    if (!b->file) { fprintf(stderr, "net: benchmark CSV could not be opened\n"); return; }
    fprintf(b->file, "event,local_ms,race_id,kind,seq,peer,bytes,value,aux\n");
    fflush(b->file);
}

/* Packets are type/slot + payload. The shared transports overwrite the sender slot. */
static inline size_t bench_packet(uint8_t *data, int kind, int slot, unsigned seq, double now, double epoch)
{
    size_t n = kind ? BENCH_STATE_BYTES : BENCH_EVENT_BYTES;
    memset(data, 0, n);
    data[0] = kind ? 5 : 6;
    uint8_t *record = data + 2;
    if (kind) {
        data[2] = 6; data[3] = 9; data[4] = 1;
        memcpy(data + 5, &epoch, 8);
        record += 11;
        record[102] = 0; /* one fixed-size record with no pad deltas */
    }
    record[0] = 254;
    memcpy(record + 1, "DHDBEN01", 8);
    record[9] = 0; record[10] = (uint8_t)slot;
    memcpy(record + 11, &seq, 4); memcpy(record + 15, &now, 8);
    return n;
}

static inline int bench_match(const uint8_t *data, size_t n)
{
    int state = n == BENCH_STATE_BYTES && data[0] == 5 && data[2] == 6 && data[3] == 9 && data[4] == 1;
    int event = n == BENCH_EVENT_BYTES && data[0] == 6;
    if (!state && !event) return 0;
    const uint8_t *r = data + (state ? 13 : 2);
    return r[0] == 254 && !memcmp(r + 1, "DHDBEN01", 8) && r[9] <= 1;
}
#endif
