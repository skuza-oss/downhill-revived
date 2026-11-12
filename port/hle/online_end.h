/* Why an online race closes for this PC (port/hle/online.c state_sync, finding 211): the words the
   race screen shows for the two seconds before it goes back to the room. The same rule on every PC,
   host and guest; it reads what the room said (the "ended" message's reason, this PC's own entry),
   never the absence of a new result: the "ended" message comes before the room snapshot that
   carries the result, and that gap used to read as "the host ended it". */
#ifndef DHD_ONLINE_END_H
#define DHD_ONLINE_END_H

#include "../net/net.h"
#include <string.h>

typedef struct {
    int ended;            /* NetLobby.ended: NET_ENDED_* */
    int is_host;          /* this PC is the room's host */
    int link_lost;        /* the connection to the room failed or dropped (offline, FAILED) */
    int removed;          /* the host removed this PC from the room (error code "kicked") */
    int finished;         /* this PC's rider finished */
    const char *own_dnf;  /* this PC's DNF reason in the running race ("time_up", "gave_up", "left"), or "" / NULL */
    int new_result;       /* the room's last race is not the one this race began with (older servers) */
} RaceEnd;

static inline const char *race_end_reason(const RaceEnd *e)
{
    if (e->ended == NET_ENDED_VOID) return e->is_host ? "YOU ENDED THE RACE" : "THE HOST ENDED THE RACE";
    if (e->link_lost) return e->removed ? "THE HOST REMOVED YOU" : "CONNECTION LOST";
    if (e->finished) return "RACE OVER";
    if (e->ended == NET_ENDED_DONE) return "TIME UP - DNF";
    const char *own = e->own_dnf ? e->own_dnf : "";
    if (!strcmp(own, "time_up")) return "TIME UP - DNF";
    if (!strcmp(own, "gave_up")) return "YOU LEFT THE RACE";
    if (!strcmp(own, "left")) return "LINK TO THE OTHERS LOST";
    /* an older server sends no reason: a new result still says the finish window ran out, and
       nothing says who ended it, so nothing blames anyone */
    return e->new_result ? "TIME UP - DNF" : "THE RACE ENDED";
}

#endif
