/* Discord Rich Presence (finding 106): what the player is doing, read from the game's own state
   twice a second (pad_vsync) and sent to the host (host_service_presence) when it changes. The
   native host shows it on the player's Discord profile when the discord setting is on
   (port/render/presence: it paces the updates and starts the elapsed time); the browser's does
   nothing with it. */
#include "hle.h"
#include "../include/fps.h"
#include "../net/net.h"

#include <stdio.h>
#include <string.h>

#define EE32(a) (*(volatile int32_t *)(uintptr_t)(0x08000000u + (a)))
#define EE_PTR(a) ((void *)(uintptr_t)(0x08000000u + (a)))

/* The game's state (findings 040, 094): in a race (port_in_race, fps.h: the race's loop runs, from
   the flight over the track to the results, not the attract demo; not the shell's screen 9, which a
   career continued from a save never shows, finding 159); the race phase (5 the flight, 1 the
   countdown, 2-4 the race: the places are worked out from 2, the countdown has every rider 1st); the
   setup block D_004B25A0 (+0x110 1 for the career's races; +0x2A0 the track); the loaded level's
   name; the riders D_002AF2C0 (the player's first, +0x798C its place, 1 first) and their count. */
enum { PHASE = 0x43F348, SETUP = 0x4B25A0, LEVEL_NAME = 0x4BD4DC, RIDERS = 0x2AF2C0,
       PLACE = RIDERS + 0x798C, RIDER_COUNT = RIDERS + 0x501A8 };
enum { FLYBY_PHASE = 5, CHECK_FIELDS = 30 };

const char *port_track_name(int index);  /* online_menu.c */

static void state(HostPresence *p)
{
    static NetLobby lobby;
    host_service_net_get_lobby(GADDR(&lobby));
    int in_room = lobby.state >= NET_LOBBY && lobby.state <= NET_RECONNECTING && lobby.code[0];
    memset(p, 0, sizeof *p);
    if (host_service_movie_playing()) {
        p->kind = PRESENCE_MOVIE;
    } else if (port_in_race()) {
        p->kind = pads_online() || lobby.state == NET_RACING ? PRESENCE_ONLINE_RACE
                  : EE32(SETUP + 0x110) == 1 ? PRESENCE_CAREER : PRESENCE_RACE;
        const char *name = port_track_name(EE32(SETUP + 0x2A0));
        snprintf(p->track, sizeof p->track, "%.23s", name ? name : (const char *)EE_PTR(LEVEL_NAME));
        int phase = EE32(PHASE);
        if (phase >= 2 && phase < FLYBY_PHASE) p->place = EE32(PLACE), p->riders = EE32(RIDER_COUNT);
    } else if (in_room) {
        p->kind = PRESENCE_ROOM;
        /* a private room's code stays here: only a public room's leaves the core */
        if (lobby.is_public) snprintf(p->room, sizeof p->room, "%.7s", lobby.code);
        p->members = lobby.nmembers, p->max_members = lobby.max_players;
    } else {
        p->kind = PRESENCE_MENUS;
    }
}

void presence_field(void)
{
    static unsigned fields;
    static HostPresence sent;
    HostPresence now;
    if (fields++ % CHECK_FIELDS) return;
    state(&now);
    if (!memcmp(&now, &sent, sizeof now)) return;
    sent = now;
    host_service_presence(GADDR(&sent));
}
