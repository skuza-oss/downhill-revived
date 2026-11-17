/* The room's rows, names and results as data (room_view.h, finding 271). */
#include "room_view.h"
#include <assert.h>
#include <string.h>

static const char *const riders[16] = {"T-BAG", "ERIC CARTER", "TARA LLANES", "DRUD", "KALOLO", "COSMO"};

static NetMember *add(NetLobby *l, int id, const char *name, int ready, int rider)
{
    NetMember *m = &l->member[l->nmembers];
    l->member_score[l->nmembers] = -1;
    l->nmembers++;
    memset(m, 0, sizeof *m);
    m->id = id, m->ready = ready, m->rider = rider, m->rtt_ms = 20, m->finish_ticks = -1;
    snprintf(m->name, sizeof m->name, "%s", name);
    return m;
}

static void result(NetLobby *l, int id, const char *name, int place, long long ticks, const char *dnf)
{
    NetResult *r = &l->result[l->nresults];
    l->result_score[l->nresults] = -1;
    l->nresults++;
    memset(r, 0, sizeof *r);
    r->id = id, r->place = place, r->ticks = ticks;
    snprintf(r->name, sizeof r->name, "%s", name);
    snprintf(r->dnf_reason, sizeof r->dnf_reason, "%s", dnf);
}

int main(void)
{
    char out[ROOM_NAME_SIZE];
    /* names: what the game lets a player type stays; the rest drawn safely and cut with ".." */
    room_name("WMWMWMWMWMWM", out), assert(!strcmp(out, "WMWMWMWMWMWM"));
    room_name("Bo", out), assert(!strcmp(out, "BO"));
    room_name("xX_DarkRider_Xx", out), assert(!strcmp(out, "XX-DARKRID.."));
    room_name("ABCDEFGHIJKLMNOPQRSTUVWX", out), assert(!strcmp(out, "ABCDEFGHIJ.."));
    room_name("J\xc3\xbcrgen", out), assert(!strcmp(out, "J-RGEN"));     /* a UTF-8 letter: one '-' */
    room_name("QUENTIN-MWMW", out), assert(!strcmp(out, "QUENTIN-MWMW"));
    room_name(NULL, out), assert(!strcmp(out, "-"));
    room_name("", out), assert(!strcmp(out, "-"));
    room_name("   ", out), assert(!strcmp(out, "-"));
    assert(strlen("WWWWWWWWWW..") + 1 <= ROOM_NAME_SIZE);
    /* one time format: 1804 ticks is 0:30.06 (the game's CONGRATULATIONS prints 0:30:04, sixtieths) */
    char t[16];
    room_time(t, sizeof t, 1804), assert(!strcmp(t, "0:30.06"));
    room_time(t, sizeof t, 55 * 60), assert(!strcmp(t, "0:55.00"));
    room_finish(t, sizeof t, 99, 1200), assert(!strcmp(t, "1200 PTS"));

    /* before the first race: join order, slot numbers, the rider in the middle, open places after */
    static NetLobby l;
    memset(&l, 0, sizeof l);
    l.member_id = 10, l.host_id = 1, l.max_players = 10;
    snprintf(l.phase, sizeof l.phase, "open");
    add(&l, 1, "WWWWWWWWWWWW", 1, 1);
    add(&l, 2, "ANNA", 0, 4);
    add(&l, 10, "WMWMWMWMWMWM", 0, 6)->choosing = 1;
    RoomRow rows[NET_MAX_SLOTS];
    int n = room_rows(&l, riders, rows);
    assert(n == 10);
    assert(!strcmp(rows[0].place, "1") && rows[0].host && rows[0].state_kind == RS_READY && !strcmp(rows[0].middle, "T-BAG"));
    assert(rows[1].state_kind == RS_WAIT && rows[1].edge == EDGE_GREY && !strcmp(rows[1].middle, "DRUD"));
    assert(rows[2].own && rows[2].state_kind == RS_CHOOSING && !rows[2].middle[0]);
    assert(rows[3].member == -1 && rows[3].state_kind == RS_OPEN && !strcmp(rows[3].place, "4"));
    l.max_players = 2;
    assert(room_rows(&l, riders, rows) == 3);  /* never fewer rows than members */

    /* a race runs: finished by place, racing, out, not in it (ready for the next one or not) */
    memset(&l, 0, sizeof l);
    l.member_id = 5, l.host_id = 1, l.max_players = 6;
    snprintf(l.phase, sizeof l.phase, "racing");
    NetMember *a = add(&l, 1, "HOSTY", 0, 1), *b = add(&l, 2, "SECOND", 0, 2), *c = add(&l, 3, "RIDER", 0, 3);
    NetMember *d = add(&l, 4, "OUTER", 0, 4), *e = add(&l, 5, "ME", 0, 5), *f = add(&l, 6, "LATE", 1, 6);
    snprintf(a->race_state, 12, "finished"), a->place = 2, a->finish_ticks = 4200;
    snprintf(b->race_state, 12, "finished"), b->place = 1, b->finish_ticks = 3300;
    snprintf(c->race_state, 12, "racing");
    snprintf(d->race_state, 12, "dnf"), snprintf(d->dnf_reason, 16, "gave_up");
    (void)e, (void)f;
    n = room_rows(&l, riders, rows);
    assert(n == 6);
    assert(rows[0].member == 1 && !strcmp(rows[0].place, "1ST") && !strcmp(rows[0].state, "0:55.00") && rows[0].edge == EDGE_AMBER);
    assert(rows[1].member == 0 && !strcmp(rows[1].place, "2ND") && rows[1].host);
    assert(rows[2].state_kind == RS_RACING && rows[2].middle_kind == MID_NONE);
    assert(rows[3].state_kind == RS_OUT && !strcmp(rows[3].state, "OUT - GAVE UP") && rows[3].dim);
    assert(rows[4].own && rows[4].state_kind == RS_NOT_IN && !strcmp(rows[4].state, "NOT IN THIS RACE"));
    assert(rows[5].state_kind == RS_READY_NEXT);
    l.member_score[0] = 300;  /* the arena's scored round: points where times are (finding 204) */
    room_rows(&l, riders, rows);
    assert(!strcmp(rows[1].state, "300 PTS"));

    /* after the race: by the last race's places, its DNFs, then those not in it; ready states again */
    memset(&l, 0, sizeof l);
    l.member_id = 3, l.host_id = 1, l.max_players = 10, l.last_race_id = 4;
    snprintf(l.phase, sizeof l.phase, "intermission");
    add(&l, 1, "WWWWWWWWWWWW", 1, 1);
    add(&l, 2, "ANNA", 1, 4);
    add(&l, 3, "WMWMWMWMWMWM", 1, 6);
    add(&l, 4, "NEWCOMER", 0, 2);
    result(&l, 1, "WWWWWWWWWWWW", 1, 3300, "");
    result(&l, 3, "WMWMWMWMWMWM", 2, 5485, "");
    result(&l, 2, "ANNA", 3, -1, "time_up");
    n = room_rows(&l, riders, rows);
    assert(!strcmp(rows[0].place, "1ST") && !strcmp(rows[0].middle, "0:55.00") && rows[0].middle_kind == MID_RESULT);
    assert(rows[1].own && !strcmp(rows[1].place, "2ND") && !strcmp(rows[1].middle, "1:31.41"));
    assert(rows[2].member == 1 && !strcmp(rows[2].middle, "DNF") && rows[2].dim && rows[2].state_kind == RS_READY);
    assert(rows[3].member == 3 && !strcmp(rows[3].middle, "ERIC CARTER") && rows[3].state_kind == RS_WAIT);

    /* the results screen: places, gaps to the winner, DNF reasons, the headline */
    ResultRow res[NET_MAX_SLOTS];
    n = room_results(&l, res);
    assert(n == 3);
    assert(!strcmp(res[0].place, "1ST") && !strcmp(res[0].time, "0:55.00") && !res[0].gap[0]);
    assert(res[1].own && !strcmp(res[1].gap, "+0:36.41"));
    assert(!res[2].finished && !strcmp(res[2].time, "DNF - TIME UP"));
    char head[48];
    room_results_headline(&l, NET_ENDED_DONE, 0, head, sizeof head), assert(!strcmp(head, "YOU FINISHED 2ND"));
    room_results_headline(&l, NET_ENDED_VOID, 0, head, sizeof head), assert(!strcmp(head, "THE HOST ENDED THE RACE"));
    room_results_headline(&l, NET_ENDED_DONE, 1, head, sizeof head), assert(!strcmp(head, "WWWWWWWWWWWW WINS"));
    l.member_id = 2;
    room_results_headline(&l, NET_ENDED_DONE, 0, head, sizeof head), assert(!strcmp(head, "TIME UP - DNF"));
    snprintf(l.result[2].dnf_reason, 16, "gave_up");
    room_results_headline(&l, NET_ENDED_DONE, 0, head, sizeof head), assert(!strcmp(head, "YOU GAVE UP"));
    l.result_score[0] = 900, l.result_score[1] = 650;  /* scored: the gap in points */
    room_results(&l, res);
    assert(!strcmp(res[0].time, "900 PTS") && !strcmp(res[1].gap, "-250 PTS"));
    return 0;
}
