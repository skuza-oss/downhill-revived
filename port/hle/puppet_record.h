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

#endif
