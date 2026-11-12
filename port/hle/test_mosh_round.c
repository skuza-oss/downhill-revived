/* The arena's scored round (finding 204): the score event and the round's table. Built and run
   by port/net/build_tests.py (check.py port). */
#include "mosh_round.h"
#include <assert.h>
#include <stdio.h>

static MoshScore score(uint32_t points, uint16_t hits, int final)
{
    MoshScore s;
    memset(&s, 0, sizeof s);
    s.part[1] = points, s.hits = hits, s.known = 1, s.final = (uint8_t)final;
    return s;
}

int main(void)
{
    /* the event round-trips, and a final stays final */
    uint8_t e[SCORE_EVENT_SIZE];
    MoshScore a = score(0, 3, 1), b;
    a.part[0] = 200, a.part[1] = 50, a.part[2] = 100, a.part[3] = 400, a.part[4] = 300;
    score_encode(e, 7, &a);
    int global = -1;
    assert(score_decode(e, sizeof e, &global, &b) && global == 7);
    assert(mosh_points(&b) == 1050 && b.hits == 3 && b.final && b.known && mosh_same(&a, &b));
    /* malformed or out-of-bounds events are refused */
    assert(!score_decode(e, sizeof e - 1, &global, &b));
    e[0] = 1; assert(!score_decode(e, sizeof e, &global, &b)); e[0] = SCORE_EVENT_VERSION;
    e[1] = 9; assert(!score_decode(e, sizeof e, &global, &b)); e[1] = SCORE_EVENT_TYPE;
    e[2] = MOSH_RIDERS; assert(!score_decode(e, sizeof e, &global, &b)); e[2] = 7;
    e[3] = 2; assert(!score_decode(e, sizeof e, &global, &b)); e[3] = SCORE_FINAL;
    MoshScore big = score(MOSH_MAX_SCORE, 0, 0);
    big.part[0] = 1;
    score_encode(e, 0, &big);
    assert(!score_decode(e, sizeof e, &global, &b));  /* the sum past six digits */
    big.part[0] = 0;
    score_encode(e, 0, &big);
    assert(score_decode(e, sizeof e, &global, &b) && mosh_points(&b) == MOSH_MAX_SCORE);
    /* a held final is never replaced; a live one is */
    MoshScore held = score(100, 1, 0), late = score(50, 0, 0), fin = score(300, 2, 1);
    assert(mosh_take(&held, &late, 2, 0) && mosh_points(&held) == 50);
    assert(mosh_take(&held, &fin, 2, 0) && held.final);
    assert(!mosh_take(&held, &late, 2, 0) && mosh_points(&held) == 300);
    assert(!mosh_take(&held, &fin, 2, 0));  /* the same sender's final again changes nothing */

    /* Two finals for one rider (finding 221): rider 3, its player (sender 3) final 500, the host
       (sender 0) took it over and finals 400. Whichever arrives first, the table gets 500. */
    {
        MoshScore player = score(500, 1, 1), host = score(400, 1, 1), h1, h2;
        memset(&h1, 0, sizeof h1);
        memset(&h2, 0, sizeof h2);
        assert(mosh_take(&h1, &player, 3, 1) && !mosh_take(&h1, &host, 0, 0));
        assert(mosh_take(&h2, &host, 0, 0) && mosh_take(&h2, &player, 3, 1));
        assert(mosh_points(&h1) == 500 && mosh_points(&h2) == 500 && h1.by_player && h2.by_player);
        assert(mosh_points(&h1) == mosh_points(&h2) && mosh_same(&h1, &h2));
        /* the player's final stands against a second player's final too (a repeat) */
        assert(!mosh_take(&h1, &player, 3, 1));
        /* a live score from the takeover owner never replaces a final of any kind */
        MoshScore live = score(900, 0, 0);
        assert(!mosh_take(&h1, &live, 0, 0) && !mosh_take(&h2, &live, 0, 0));
    }
    /* Between two takeover owners (the host changed) the later one wins, in either order. The
       receiver takes finals only from the current owner, so the later owner is the sender that
       differs from the one held. */
    {
        MoshScore first = score(300, 2, 1), later = score(350, 2, 1), h1, h2;
        memset(&h1, 0, sizeof h1);
        memset(&h2, 0, sizeof h2);
        assert(mosh_take(&h1, &first, 0, 0) && mosh_take(&h1, &later, 1, 0));  /* the old owner's first */
        assert(mosh_take(&h2, &later, 1, 0));                                  /* the new owner's only */
        assert(mosh_points(&h1) == 350 && mosh_points(&h2) == 350 && h1.from == 1 && h2.from == 1);
        assert(!mosh_take(&h1, &first, 1, 0));  /* the later owner's own final again */
    }
    /* the player's final replaces a takeover final even after a second takeover owner's */
    {
        MoshScore a = score(300, 0, 1), b = score(350, 0, 1), p = score(500, 0, 1), h;
        memset(&h, 0, sizeof h);
        assert(mosh_take(&h, &a, 0, 0) && mosh_take(&h, &b, 1, 0) && mosh_take(&h, &p, 3, 1));
        assert(mosh_points(&h) == 500 && !mosh_take(&h, &b, 1, 0));
    }

    /* the table: points first, a tie to the lower global id; the same on every PC whatever order
       the scores arrived in (the table reads only the numbers) */
    MoshScore t[6] = {score(300, 2, 1), score(500, 0, 1), score(300, 1, 1), score(0, 4, 1), score(900, 1, 1),
                      score(500, 3, 1)};
    int order[6], want[6] = {4, 1, 5, 0, 2, 3};
    mosh_order(t, 6, order);
    for (int i = 0; i < 6; i++) assert(order[i] == want[i]);
    assert(mosh_place(t, 6, 4) == 1 && mosh_place(t, 6, 5) == 3 && mosh_place(t, 6, 3) == 6);
    assert(mosh_place(t, 6, 9) == 0);
    /* two humans and four computer riders, the humans' places as the room gets them */
    MoshScore all_zero[3] = {score(0, 0, 1), score(0, 0, 1), score(0, 0, 1)};
    mosh_order(all_zero, 3, order);
    assert(order[0] == 0 && order[1] == 1 && order[2] == 2);
    puts("test_mosh_round: ok");
    return 0;
}
