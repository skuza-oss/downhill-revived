/* STATE bundles (finding 169): the riders one PC sends in one race tick, packed into as few
   messages as fit one datagram each. STATE travels on an unordered channel with no
   retransmissions; a message larger than one SCTP chunk is split, and losing any piece loses the
   whole message, so every bundle stays at or under STATE_BUNDLE_MAX bytes of payload.

   Payload version 6:
     [0]     6
     [1]     bundle index in this tick (0, 1, ...): the transport keeps the newest per index
     [2]     riders in the bundle (1-10)
     [3..10] f64: the moment the snapshots show on the server clock (ms since the Unix epoch): the
             sender's GO there plus its race time
     then each rider's record: its global id, its race time (i32 ms), the body and the rest of
     the snapshot (port/hle/puppet.c), its pad-word count k and k index/u32 pairs. A record is
     STATE_RECORD_FIXED + 5k bytes, so it carries its own length.

   The budget: str0m's SCTP chunk holds 1150 - 48 (DTLS) - 40 (SCTP) = 1062 bytes; a race frame
   adds the 8-byte race marker and the 2-byte type and slot. 1000 bytes on the wire leaves room
   for the SFU's own path to the receiver. */
#ifndef DHD_STATE_BUNDLE_H
#define DHD_STATE_BUNDLE_H
#include <stdint.h>
#include <string.h>

enum {
    STATE_BUNDLE_VERSION = 6,
    STATE_BUNDLE_HEADER = 11,
    STATE_WIRE_BUDGET = 1000,                       /* marker + type + slot + payload */
    STATE_BUNDLE_MAX = STATE_WIRE_BUDGET - 8 - 2,   /* payload bytes */
    STATE_RECORD_FIXED = 103,                       /* up to and with the pad-word count */
    STATE_RECORD_MAX = STATE_RECORD_FIXED + 146 * 5,
    STATE_BUNDLE_RIDERS = 10
};
_Static_assert(STATE_BUNDLE_HEADER + STATE_RECORD_MAX <= STATE_BUNDLE_MAX, "a full refresh fits one bundle");

/* A record's length from its bytes (0: not a record that fits in n). */
static inline int state_record_size(const uint8_t *in, int n)
{
    if (n < STATE_RECORD_FIXED || in[STATE_RECORD_FIXED - 1] > 146) return 0;
    int size = STATE_RECORD_FIXED + in[STATE_RECORD_FIXED - 1] * 5;
    return size <= n ? size : 0;
}

/* Packs the records (each rec[i] of size[i] bytes) into bundles, first fit, largest first; out
   holds up to STATE_BUNDLE_RIDERS bundles of STATE_BUNDLE_MAX bytes. Returns the bundles, their
   sizes in bundle_size. */
static inline int state_bundle_pack(const uint8_t *const *rec, const int *size, int count, double stamp,
                             uint8_t out[][STATE_BUNDLE_MAX], int *bundle_size)
{
    int order[STATE_BUNDLE_RIDERS], bundles = 0;
    if (count > STATE_BUNDLE_RIDERS) count = STATE_BUNDLE_RIDERS;
    for (int i = 0; i < count; i++) {
        int j = i;
        for (; j > 0 && size[order[j - 1]] < size[i]; j--) order[j] = order[j - 1];
        order[j] = i;
    }
    for (int k = 0; k < count; k++) {
        int i = order[k], b = 0;
        if (size[i] <= 0 || size[i] > STATE_RECORD_MAX) continue;
        while (b < bundles && bundle_size[b] + size[i] > STATE_BUNDLE_MAX) b++;
        if (b == bundles) {
            uint8_t *o = out[bundles++];
            o[0] = STATE_BUNDLE_VERSION, o[1] = (uint8_t)b, o[2] = 0;
            memcpy(o + 3, &stamp, 8);
            bundle_size[b] = STATE_BUNDLE_HEADER;
        }
        memcpy(out[b] + bundle_size[b], rec[i], size[i]);
        bundle_size[b] += size[i];
        out[b][2]++;
    }
    return bundles;
}

/* Checks a whole bundle before anything reads it: the header, every record's length, the count.
   Returns the riders in it (0: not a bundle) and its stamp. */
static inline int state_bundle_check(const uint8_t *in, int n, double *stamp)
{
    if (n < STATE_BUNDLE_HEADER || n > STATE_BUNDLE_MAX || in[0] != STATE_BUNDLE_VERSION || !in[2] ||
        in[2] > STATE_BUNDLE_RIDERS) return 0;
    int at = STATE_BUNDLE_HEADER, riders = 0;
    while (at < n) {
        int size = state_record_size(in + at, n - at);
        if (!size) return 0;
        at += size, riders++;
    }
    if (riders != in[2]) return 0;
    memcpy(stamp, in + 3, 8);
    return riders;
}
#endif
