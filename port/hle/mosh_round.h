/* The arena's scored round online (finding 204): an online race on the Mosh Pit arena (track 23)
   runs by the game's own Mosh Bowl rules (mode 15, its timed variant: SETUP +0x364 = 0), two
   minutes, the points each rider scores with its hits, then the round's table.

   Each rider's score has one owner, the PC that runs the rider (port/hle/puppet.c owner_of): its
   human's own game, the host's for the computer riders and for a player who left. Its points come
   from the hits it deals (func_001301E0, decided on its owner's PC as every hit is, finding 044),
   its hits taken from the hits its owner applies to it (func_0012E940). The owner sends the
   rider's whole score as an EVENT (reliable and ordered) whenever it changes, and once more,
   final, when the round's two minutes run out on its own clock; everyone else only shows what the
   owner sent. So every game holds the same final numbers, and the table's order below is a rule
   on those numbers alone: the same table on every screen.

   Pure functions, no game memory: port/hle/test_mosh_round.c tests them. */
#ifndef DHD_MOSH_ROUND_H
#define DHD_MOSH_ROUND_H
#include <stdint.h>
#include <string.h>

enum {
    MOSH_TRACK = 23,          /* the Mosh Pit arena, the room's MOSH PIT (ARENA) */
    MOSH_MODE = 15,           /* SETUP +0x10C: the game's Mosh Bowl (12 is Single Event) */
    MOSH_ROUND_TICKS = 7200,  /* func_001B7198: D_004B1F40[0] = 0x1C20, two minutes of race ticks */
    MOSH_MAX_SCORE = 999999,  /* the HUD's six digits (func_00177F80, "%6d"); the room's bound */
    MOSH_RIDERS = 10,
};

/* EVENT payload version 2, type 10: the global rider id (a human's lobby slot, a computer rider's
   slot), flags (bit 0: final), the five point counts of func_001301B8's sum (rider +0x7AC8,
   +0x7ACC, +0x7AD0, +0x7AD4, +0x7AD8: the attacks' kinds 2, 0, 1, 4, 3), each a u32, then the
   hits taken (rider +0x79AC, u16). 26 bytes. */
enum { SCORE_EVENT_VERSION = 2, SCORE_EVENT_TYPE = 10, SCORE_EVENT_SIZE = 26, SCORE_FINAL = 1 };

typedef struct {
    uint32_t part[5];   /* the rider's point counts, in the game's field order */
    uint16_t hits;      /* hits taken */
    uint8_t known;      /* the owner has sent one (or this PC owns the rider) */
    uint8_t final;      /* the owner's round is over: these numbers stay */
} MoshScore;

static inline uint32_t mosh_points(const MoshScore *s)
{
    uint64_t sum = 0;
    for (int k = 0; k < 5; k++) sum += s->part[k];
    return sum > MOSH_MAX_SCORE ? MOSH_MAX_SCORE : (uint32_t)sum;
}

static inline int mosh_same(const MoshScore *a, const MoshScore *b)
{
    return !memcmp(a->part, b->part, sizeof a->part) && a->hits == b->hits && a->final == b->final;
}

static inline void score_encode(uint8_t *e, int global, const MoshScore *s)
{
    e[0] = SCORE_EVENT_VERSION, e[1] = SCORE_EVENT_TYPE, e[2] = (uint8_t)global, e[3] = s->final ? SCORE_FINAL : 0;
    for (int k = 0; k < 5; k++) memcpy(e + 4 + 4 * k, &s->part[k], 4);
    memcpy(e + 24, &s->hits, 2);
}

/* 1 and the rider's global id and score when e is a well-formed score event; a peer's numbers
   are not trusted: each count and the sum stay within the room's bound. */
static inline int score_decode(const uint8_t *e, int n, int *global, MoshScore *out)
{
    if (n != SCORE_EVENT_SIZE || e[0] != SCORE_EVENT_VERSION || e[1] != SCORE_EVENT_TYPE || e[2] >= MOSH_RIDERS ||
        (e[3] & ~SCORE_FINAL))
        return 0;
    MoshScore s;
    memset(&s, 0, sizeof s);
    uint64_t sum = 0;
    for (int k = 0; k < 5; k++) {
        memcpy(&s.part[k], e + 4 + 4 * k, 4);
        sum += s.part[k];
    }
    if (sum > MOSH_MAX_SCORE) return 0;
    memcpy(&s.hits, e + 24, 2);
    s.known = 1, s.final = e[3] & SCORE_FINAL;
    *global = e[2], *out = s;
    return 1;
}

/* The round's table: riders 0..count-1 (global ids) by points, highest first; a tie goes to the
   lower global id (the room's grid order: online/server/src/room.ts closeRace sorts its humans
   the same way). order gets the global ids, first place first. */
static inline void mosh_order(const MoshScore *scores, int count, int *order)
{
    for (int i = 0; i < count; i++) order[i] = i;
    for (int i = 1; i < count; i++)
        for (int j = i; j > 0; j--) {
            uint32_t a = mosh_points(&scores[order[j - 1]]), b = mosh_points(&scores[order[j]]);
            if (b > a) {
                int t = order[j - 1];
                order[j - 1] = order[j], order[j] = t;
            } else
                break;
        }
}

/* A rider's place (1 first) in the table, 0 when it isn't in it. */
static inline int mosh_place(const MoshScore *scores, int count, int global)
{
    int order[MOSH_RIDERS];
    if (count > MOSH_RIDERS) count = MOSH_RIDERS;
    mosh_order(scores, count, order);
    for (int i = 0; i < count; i++)
        if (order[i] == global) return i + 1;
    return 0;
}

/* Whether a received score replaces the one held: a final one stays (a late copy of an earlier
   send changes nothing; the owner sends nothing after its final but the same final again). */
static inline int mosh_take(MoshScore *held, const MoshScore *got)
{
    if (held->final) return 0;
    *held = *got;
    return 1;
}

#endif
