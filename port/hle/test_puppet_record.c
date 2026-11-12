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
    return 0;
}
