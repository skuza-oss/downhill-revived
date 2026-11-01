/* Discord Rich Presence: the C interface of the Rust crate dhd_presence (port/render/presence,
   linked into dhd_render's static library, findings 106, 136). What the player is doing shows on
   their Discord profile, through Discord's local IPC (no SDK); the crate's own thread connects,
   sends only changes (at most one per 15 s; the same again after a minute, to notice a Discord that
   went away), retries every 30 s while Discord isn't running, and logs a line at most. The game's host (port/hle/host.c, host_service_presence) calls it. Strings are
   UTF-8 and NUL-terminated. */
#ifndef DHD_PRESENCE_H
#define DHD_PRESENCE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DhdPresenceLog)(const char *line);  /* a line for the game's log, from any thread */

/* Starts it when setting (DHD_DISCORD's value, NULL when unset) has it on, as the game reads the
   switch; log (may be NULL) gets its lines. 1 when it runs; 0 off (and always on Android). */
int dhd_presence_start(const char *setting, DhdPresenceLog log);

/* The player's state: kind a PRESENCE_* value (port/host/host_api.h); a race's (or a room's) track,
   the player's place (1 first, 0 not known), riders and rider (the game's index + 1, 0 not known);
   an online room's join code (empty: a private room), players and room size. */
void dhd_presence_set(int kind, const char *track, int place, int riders, int rider, const char *room,
                      int members, int max_members);

/* A friend's join from Discord (the Social SDK, finding 136): the room's code, NUL-terminated,
   into out (size bytes, the crate wants 7 or more), taken -- at most once. 1 when there was one. The
   core takes it (port/hle/host.c's host_service_presence_join, online_menu.c) and only on the
   official server; a plain-IPC presence never has one. */
int dhd_presence_take_join(char *out, int size);

/* Clears the presence and closes the connection (at exit), waiting at most half a second. */
void dhd_presence_stop(void);

#ifdef __cplusplus
}
#endif

#endif
