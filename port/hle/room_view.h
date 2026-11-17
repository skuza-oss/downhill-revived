/* What the race room shows, as data (finding 271): the list's rows in their order, each row's
   texts, a member's name as the font can draw it, the results screen's rows. online_menu.c draws
   these; online.c's results screen too. No game calls here, so test_room_view.c checks them alone.

   One list, one member a row (finding 271, the owner's pictures): before the first race in join
   order, the slot's number in front; while a race runs and after one, sorted: the finished by
   place, then those still racing, then those out, then those not in it. The middle column says
   the rider before a race and the last race's time (or DNF) after one; the right one the state. */
#ifndef DHD_ROOM_VIEW_H
#define DHD_ROOM_VIEW_H

#include "../net/net.h"
#include <stdio.h>
#include <string.h>

enum { ROOM_NAME_LETTERS = 12, ROOM_NAME_SIZE = ROOM_NAME_LETTERS + 3 };

/* A member's name for the room's font: the letters the game lets a player type (A-Z, 0-9, space,
   '-') and '.', lower case as capitals; any other character, or a UTF-8 sequence, one '-'. At most
   12; a longer name (another client's: the server takes 24 of anything) its first 10 and "..";
   an empty or blank one "-". */
static inline void room_name(const char *in, char *out)
{
    char all[100];
    int n = 0;
    for (const unsigned char *c = (const unsigned char *)(in ? in : ""); *c && n < (int)sizeof all - 1; c++) {
        unsigned char k = *c;
        if (k >= 0x80 && (k & 0xC0) == 0x80) continue;  /* a UTF-8 sequence's tail: its lead made the '-' */
        if (k >= 'a' && k <= 'z') k = (unsigned char)(k - 32);
        all[n++] = (char)((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') || k == ' ' || k == '-' || k == '.' ? k : '-');
    }
    all[n] = 0;
    if (strspn(all, " ") == (size_t)n) snprintf(out, ROOM_NAME_SIZE, "-");  /* none or blanks only: a '-', never an empty row */
    else if (n > ROOM_NAME_LETTERS) snprintf(out, ROOM_NAME_SIZE, "%.10s..", all);
    else snprintf(out, ROOM_NAME_SIZE, "%s", all);
}

/* A race time in ticks (60 a second) as M:SS.CC: the server's time, as the game reported it. The
   game's own CONGRATULATIONS line prints the same ticks as H:MM:SS through its seconds formatter
   (func_002238A8 given ticks), so its last two digits are sixtieths: 0:30:04 there is 0:30.06 here,
   the same 1804 ticks (finding 271). */
static inline void room_time(char *out, int size, long long ticks)
{
    if (ticks < 0) ticks = 0;
    snprintf(out, size, "%d:%02d.%02d", (int)(ticks / 3600), (int)(ticks / 60 % 60), (int)(ticks % 60 * 100 / 60));
}

/* A finish: the race's time, or a scored round's points (finding 204; score -1: a race). */
static inline void room_finish(char *out, int size, long long ticks, int score)
{
    if (score >= 0) snprintf(out, size, "%d PTS", score);
    else room_time(out, size, ticks);
}

static inline const char *room_ordinal(int place, char *out, int size)
{
    snprintf(out, size, "%d%s", place, place % 100 / 10 == 1 ? "TH" : place % 10 == 1 ? "ST" : place % 10 == 2 ? "ND"
             : place % 10 == 3 ? "RD" : "TH");
    return out;
}

static inline const char *room_dnf_name(const char *reason)
{
    return !strcmp(reason, "time_up") ? "TIME UP" : !strcmp(reason, "gave_up") ? "GAVE UP" : !strcmp(reason, "left") ? "LEFT" : "OUT";
}

/* A row's state: the right-hand column. */
enum { RS_OPEN, RS_READY, RS_WAIT, RS_CHOOSING, RS_RACING, RS_DONE, RS_OUT, RS_NOT_IN, RS_READY_NEXT };
/* A row's left edge and the middle column's colour. */
enum { EDGE_GREY, EDGE_GREEN, EDGE_AMBER };
enum { MID_NONE, MID_RIDER, MID_RESULT };

typedef struct {
    int member;             /* in NetLobby.member, -1: an open place */
    char place[8];          /* "3", "1ST", "-" */
    char name[ROOM_NAME_SIZE];
    char middle[24];
    int middle_kind;        /* MID_* */
    char state[24];         /* the state's words ("READY", "0:55.00", "OUT - GAVE UP") */
    int state_kind;         /* RS_* */
    int edge;               /* EDGE_* */
    int dim;                /* out, not in the race, DNF: the name dimmed */
    int own, host;
} RoomRow;

/* This member's entry in the room's last race, -1 none. */
static inline int room_last_result(const NetLobby *l, int id)
{
    for (int k = 0; k < l->nresults && k < NET_MAX_SLOTS; k++)
        if (l->result[k].id == id) return k;
    return -1;
}

/* A member's sort key while a race runs (smaller first): finished by place, racing, out, not in. */
static inline long long room_live_key(const NetLobby *l, int i)
{
    const NetMember *m = &l->member[i];
    if (!strcmp(m->race_state, "finished")) return (long long)(m->place > 0 ? m->place : 99) * 100 + i;
    if (!strcmp(m->race_state, "racing")) return 10000 + i;
    if (!strcmp(m->race_state, "dnf")) return 20000 + i;
    return 30000 + i;
}

/* After a race: the last race's finishers by place, its DNFs, then those not in it, each in join order. */
static inline long long room_after_key(const NetLobby *l, int i)
{
    int k = room_last_result(l, l->member[i].id);
    if (k < 0) return 30000 + i;
    const NetResult *r = &l->result[k];
    if (r->ticks >= 0) return (long long)(r->place > 0 ? r->place : 99) * 100 + i;
    return 20000 + k * 16 + i;
}

/* The list: rows for the members, then open places up to the room's size (10 rows at most).
   riders: the 16 riders' names by id 1-16 (the shell's list). Returns the rows. */
static inline int room_rows(const NetLobby *l, const char *const *riders, RoomRow *out)
{
    int n = l->nmembers < NET_MAX_SLOTS ? l->nmembers : NET_MAX_SLOTS, order[NET_MAX_SLOTS];
    int racing = !strcmp(l->phase, "racing"), after = !racing && l->last_race_id > 0;
    long long key[NET_MAX_SLOTS];
    for (int i = 0; i < n; i++) order[i] = i, key[i] = racing ? room_live_key(l, i) : after ? room_after_key(l, i) : i;
    for (int i = 1; i < n; i++)  /* insertion sort: stable, ten at most */
        for (int j = i; j > 0 && key[order[j]] < key[order[j - 1]]; j--) {
            int t = order[j];
            order[j] = order[j - 1], order[j - 1] = t;
        }
    for (int r = 0; r < n; r++) {
        int i = order[r];
        const NetMember *m = &l->member[i];
        RoomRow *w = &out[r];
        memset(w, 0, sizeof *w);
        w->member = i;
        room_name(m->name, w->name);
        w->own = m->id == l->member_id, w->host = m->id == l->host_id;
        snprintf(w->place, sizeof w->place, "-");
        if (racing) {
            w->middle_kind = MID_NONE;
            if (!strcmp(m->race_state, "finished")) {
                room_ordinal(m->place, w->place, sizeof w->place);
                room_finish(w->state, sizeof w->state, m->finish_ticks, l->member_score[i]);
                w->state_kind = RS_DONE, w->edge = EDGE_AMBER;
            } else if (!strcmp(m->race_state, "racing")) {
                snprintf(w->state, sizeof w->state, "RACING");
                w->state_kind = RS_RACING, w->edge = EDGE_GREEN;
            } else if (!strcmp(m->race_state, "dnf")) {
                snprintf(w->state, sizeof w->state, "OUT - %s", room_dnf_name(m->dnf_reason));
                w->state_kind = RS_OUT, w->edge = EDGE_GREY, w->dim = 1;
            } else if (m->ready) {  /* not in this race, ready for the next (finding 271: kept from the old cards) */
                snprintf(w->state, sizeof w->state, "READY NEXT");
                w->state_kind = RS_READY_NEXT, w->edge = EDGE_GREEN;
            } else {
                snprintf(w->state, sizeof w->state, "NOT IN THIS RACE");
                w->state_kind = RS_NOT_IN, w->edge = EDGE_GREY, w->dim = 1;
            }
            continue;
        }
        if (after) {
            int k = room_last_result(l, m->id);
            if (k >= 0 && l->result[k].ticks >= 0) {
                room_ordinal(l->result[k].place, w->place, sizeof w->place);
                room_finish(w->middle, sizeof w->middle, l->result[k].ticks, l->result_score[k]);
                w->middle_kind = MID_RESULT;
            } else if (k >= 0) {
                snprintf(w->middle, sizeof w->middle, "DNF");
                w->middle_kind = MID_RESULT, w->dim = 1;
            }
        } else
            snprintf(w->place, sizeof w->place, "%d", r + 1);
        if (w->middle_kind == MID_NONE) {  /* before a race, or not in the last one: the rider (none
                                              while choosing: the CHOOSING chip says it) */
            snprintf(w->middle, sizeof w->middle, "%s", m->choosing ? "" : m->rider >= 1 && m->rider <= 16 && riders
                     ? riders[m->rider - 1] : "NO RIDER");
            w->middle_kind = MID_RIDER;
        }
        if (m->choosing) snprintf(w->state, sizeof w->state, "CHOOSING"), w->state_kind = RS_CHOOSING, w->edge = EDGE_GREY;
        else if (m->ready) snprintf(w->state, sizeof w->state, "READY"), w->state_kind = RS_READY, w->edge = EDGE_GREEN;
        else snprintf(w->state, sizeof w->state, "WAIT"), w->state_kind = RS_WAIT, w->edge = EDGE_GREY;
    }
    int rows = n, size = l->max_players > n ? l->max_players : n;
    for (; rows < size && rows < NET_MAX_SLOTS; rows++) {
        RoomRow *w = &out[rows];
        memset(w, 0, sizeof *w);
        w->member = -1;
        snprintf(w->place, sizeof w->place, "%d", rows + 1);
        snprintf(w->state, sizeof w->state, "OPEN");
        w->state_kind = RS_OPEN, w->dim = 1;
    }
    return rows;
}

/* ---- the results screen (finding 271) ---- */

typedef struct {
    char place[8], name[ROOM_NAME_SIZE], time[16], gap[16];
    int finished, own;
} ResultRow;

/* The last race's results, in the room's order (NetLobby.result: the server's), with each
   finisher's gap to the winner and each DNF's reason. Returns the rows. */
static inline int room_results(const NetLobby *l, ResultRow *out)
{
    int n = l->nresults < NET_MAX_SLOTS ? l->nresults : NET_MAX_SLOTS, best = -1;
    for (int k = 0; k < n; k++)
        if (l->result[k].ticks >= 0 && (best < 0 || l->result[k].place < l->result[best].place)) best = k;
    for (int k = 0; k < n; k++) {
        const NetResult *r = &l->result[k];
        ResultRow *w = &out[k];
        memset(w, 0, sizeof *w);
        room_name(r->name, w->name);
        w->own = r->id == l->member_id;
        w->finished = r->ticks >= 0;
        if (w->finished) {
            room_ordinal(r->place, w->place, sizeof w->place);
            room_finish(w->time, sizeof w->time, r->ticks, l->result_score[k]);
            if (k != best && best >= 0 && l->result_score[k] >= 0 && l->result_score[best] >= 0)
                snprintf(w->gap, sizeof w->gap, "-%d PTS", l->result_score[best] - l->result_score[k]);
            else if (k != best && best >= 0) {
                char t[16];
                room_time(t, sizeof t, r->ticks - l->result[best].ticks);
                snprintf(w->gap, sizeof w->gap, "+%s", t);
            }
        } else {
            snprintf(w->place, sizeof w->place, "-");
            snprintf(w->time, sizeof w->time, "DNF - %s", room_dnf_name(r->dnf_reason));
        }
    }
    return n;
}

/* The results screen's large line: how the race went for this PC. ended: NetLobby.ended;
   watcher: this PC watched the race (finding 269). */
static inline void room_results_headline(const NetLobby *l, int ended, int watcher, char *out, int size)
{
    if (ended == NET_ENDED_VOID) {
        snprintf(out, size, "THE HOST ENDED THE RACE");
        return;
    }
    int n = l->nresults < NET_MAX_SLOTS ? l->nresults : NET_MAX_SLOTS;
    char name[ROOM_NAME_SIZE], place[8];
    for (int k = 0; k < n && !watcher; k++) {
        const NetResult *r = &l->result[k];
        if (r->id != l->member_id) continue;
        if (r->ticks >= 0) snprintf(out, size, "YOU FINISHED %s", room_ordinal(r->place, place, sizeof place));
        else if (!strcmp(r->dnf_reason, "gave_up")) snprintf(out, size, "YOU GAVE UP");
        else snprintf(out, size, "TIME UP - DNF");
        return;
    }
    for (int k = 0; k < n; k++)
        if (l->result[k].ticks >= 0 && l->result[k].place == 1) {
            room_name(l->result[k].name, name);
            snprintf(out, size, "%s WINS", name);
            return;
        }
    snprintf(out, size, n ? "NOBODY FINISHED" : "RACE OVER");
}

#endif
