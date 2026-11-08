/* Loss-tolerant virtual pad updates (finding 158). Words 25-49 are local history. */
#ifndef DHD_PAD_SNAPSHOT_H
#define DHD_PAD_SNAPSHOT_H
#include <stdint.h>
#include <string.h>
enum { PAD_WORDS = 171, PREV_FIRST = 25, PREV_LAST = 49 };
typedef struct PadSender {
    uint32_t last[PAD_WORDS];
    uint8_t repeats[PAD_WORDS];
    unsigned count;
} PadSender;
/* Caller reserves 1 + 146*5 bytes. First update and each thirtieth are full. */
static int pad_snapshot_pack(PadSender *p, const uint32_t *pad, uint8_t *out)
{
    uint8_t *o = out + 1;
    int full = p->count++ % 30 == 0;
    *out = 0;
    for (int i = 0; i < PAD_WORDS; i++) {
        if (i >= PREV_FIRST && i <= PREV_LAST) continue;
        if (pad[i] != p->last[i]) p->repeats[i] = 6;
        p->last[i] = pad[i];
        if (full || p->repeats[i]) {
            *o++ = (uint8_t)i;
            memcpy(o, &pad[i], 4); o += 4;
            (*out)++;
        }
        if (p->repeats[i]) p->repeats[i]--;
    }
    return (int)(o - out);
}
/* Validate the entire update before changing any mirror word. */
static int pad_snapshot_apply(uint32_t *mirror, const uint8_t *in, int n)
{
    if (n < 1 || n != 1 + in[0] * 5 || in[0] > 146) return 0;
    for (int k = 0; k < in[0]; k++) {
        int w = in[1 + k*5];
        if (w >= PAD_WORDS || (w >= PREV_FIRST && w <= PREV_LAST)) return 0;
    }
    for (int k = 0; k < in[0]; k++)
        memcpy(&mirror[in[1 + k*5]], in + 2 + k*5, 4);
    return 1;
}
#endif
