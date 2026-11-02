/* What a public room is doing now, as the PUBLIC RACES row says it (port/hle/online_menu.c):
   RACING m:ss, ENDS IN m:ss, NEXT IN m:ss, STARTING or OPEN; NEXT RACE once a racing row's finish
   window is over (the server has closed that race, the list is older). Shared by the game's core and the
   network tests (test_session.c), so the rule is checked without a game.

   The times are the server's clock (ms since the Unix epoch). They are counted only when the row
   is `timed` (the list came with the server's "now", or a room's pings set the clock) and the
   result is sane (under an hour): 0.7.6, in no room yet, counted from the epoch and showed
   ENDS IN 29893031:32 (finding 135). Otherwise the row says RACING or NEXT RACE without a time. */
#ifndef NET_LOBBY_ROW_H
#define NET_LOBBY_ROW_H

#include <stdio.h>
#include <string.h>

#include "net.h"

enum { NET_ROW_MAX_SECONDS = 3600 };

/* server_ms: net_server_ms() now. */
static void net_lobby_now(const NetLobbyInfo *l, double server_ms, char *out, size_t cap)
{
    int timed = l->timed && server_ms > 0;
    if (!strcmp(l->phase, "racing")) {
        double left = (l->finish_ends_at - server_ms + 999) / 1000, so_far = (server_ms - l->race_started_at) / 1000;
        if (timed && l->finish_ends_at > server_ms && left <= NET_ROW_MAX_SECONDS)
            snprintf(out, cap, "ENDS IN %d:%02d", (int)left / 60, (int)left % 60);
        else if (timed && l->finish_ends_at > 0 && l->finish_ends_at <= server_ms)
            /* the window is over: the server closed the race then (room.ts alarm, closeRace) and
               the list is older; next it is waiting for the next race, as an intermission says */
            snprintf(out, cap, "NEXT RACE");
        else if (timed && l->race_started_at > 0 && so_far >= 0 && so_far <= NET_ROW_MAX_SECONDS)
            snprintf(out, cap, "RACING %d:%02d", (int)so_far / 60, (int)so_far % 60);
        else
            snprintf(out, cap, "RACING");
    } else if (!strcmp(l->phase, "intermission")) {
        double left = (l->next_race_at - server_ms + 999) / 1000;
        if (timed && l->next_race_at > server_ms && left <= NET_ROW_MAX_SECONDS)
            snprintf(out, cap, "NEXT IN %d:%02d", (int)left / 60, (int)left % 60);
        else
            snprintf(out, cap, "NEXT RACE");
    } else if (!strcmp(l->phase, "starting"))
        snprintf(out, cap, "STARTING");
    else
        snprintf(out, cap, "OPEN");
}

#endif
