/* Online races: the API the rest of the port uses (docs/ONLINE-PLAN.md, finding 039, the wire
   protocol in docs/online-protocol.md).

   Races use state sync (findings 041, 044): every PC runs its own one-player race and sends
   snapshots of the riders it owns (STATE frames) and hits it deals (EVENT frames) with
   net_send_frame, through a relay over a WebSocket; the others' riders are puppets
   (port/hle/online.c, port/hle/puppet.c). A background thread keeps the connection (lobby JSON,
   binary race frames, pings). */
#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>

enum { NET_MAX_SLOTS = 8 };  /* a room holds 8 */

/* ---- connection and lobby ---- */

enum { NET_IDLE, NET_CONNECTING, NET_LOBBY, NET_RACING, NET_RECONNECTING, NET_FAILED };

typedef struct {
    int id;                    /* member id: fixed for the connection */
    char name[100];            /* UTF-8, up to 24 characters */
    int ready, rider, bike;
    int rtt_ms;                /* the server's measure, -1 unknown */
    int choosing;              /* on the rider screen */
} NetMember;

typedef struct {
    int slot, member_id;
    char name[100];
    int rider, bike;
    int left;                  /* gone: the host's AI drives the rider */
} NetRacer;

typedef struct {
    int state;                 /* NET_IDLE... */
    unsigned seq;              /* bumps on every change */
    char code[8];              /* the join code */
    int member_id, host_id, is_host;
    char room_name[100];
    int is_public, max_players, started;
    int track, laps;
    long long seed;            /* -1 until set */
    int nmembers;
    NetMember member[NET_MAX_SLOTS];
    int ping_ms;               /* this PC to the relay, -1 unknown */
    /* the race, once started */
    int local_slot, nslots;
    NetRacer racer[NET_MAX_SLOTS];
    int race_track, race_laps;
    uint32_t race_seed;
    double start_at;           /* net_seconds() of the server's start */
    int offline;               /* the connection dropped mid-race */
    /* the last error */
    char error_code[32], error[200];
    /* Everyone is ready: the race starts by itself in this many ms (the server's countdown),
       -1 when none runs. Worked out as the copy is made (net_get_lobby). */
    int countdown_ms;
    /* Rejoining (finding 126). New fields go here, at the end: the game, the x64 host and the
       browser's net module (port/web/www/net.js copies web_net_lobby_size() bytes) all copy this
       struct as it is, so the fields above keep their offsets. */
    unsigned members_reconnecting;  /* bit i: member[i] dropped; the server holds its lobby slot */
    unsigned racers_away;      /* bit s: racer s dropped mid-race ("away"); the server holds its slot
                                  until it comes "back" or has "left". Its rider is not driven meanwhile. */
    int rejoining;             /* this PC lost the race's connection and is rejoining its slot: frames
                                  are not sent; offline is set only if that fails */
} NetLobby;

/* Host settings; fields at -1 (room_name NULL) are left as they are. */
typedef struct {
    int track, laps, is_public, max_players;
    long long seed;
    const char *room_name;
} NetRoomSettings;

typedef struct {
    char code[8], room_name[100], version[64], host_name[100];
    int track, players, max_players;
} NetLobbyInfo;

/* base_url: "wss://host" or "ws://127.0.0.1:8787". version: the build's version string (the
   server refuses players whose version differs from the host's). iso_sha256: 64 lowercase hex. */
void net_init(const char *base_url, const char *name, const char *version, const char *iso_sha256);
int net_create_room(void);
int net_join_room(const char *code);   /* -1 when the code can't be one */
void net_leave(void);
/* Public lobbies (blocking HTTP GET); returns how many, -1 on error. */
int net_list_lobbies(NetLobbyInfo *out, int max);
void net_set_room(const NetRoomSettings *s);       /* host */
/* member -1: this PC. rider -1 (this PC): choosing a rider now, unready until the next pick. */
void net_pick(int member, int rider, int bike);
void net_set_ready(int ready);
void net_kick(int member);                         /* host */
void net_start_race(void);                         /* host; everyone must be ready */
void net_end_race(void);                           /* host: back to the lobby */
void net_get_lobby(NetLobby *out);                 /* a copy, for the UI */
double net_seconds(void);                          /* the clock start_at uses */
/* The server's clock (ms since the Unix epoch), estimated from the lobby pings' round-trips: the
   same moment on every PC to a few ms, for starting the race together. */
double net_server_ms(void);

/* ---- the race ---- */

int net_racing(void);
int net_local_slot(void);

/* Binary frames the relay forwards as they are to every other racer: a snapshot of riders this
   PC owns, an event it decided for another PC's rider. A frame is type, slot (the server sets
   it), payload. */
/* NET_FRAME_MAX: a STATE frame carries all of a player's riders (up to ten snapshots of at most
   255 bytes); the server's MAX_BINARY_BYTES is the same. */
enum { NET_FRAME_STATE = 5, NET_FRAME_EVENT = 6, NET_FRAME_MAX = 3072, NET_RELAY_QUEUE = 128 };
/* The race's pings (client.c): type, slot, the send time; the server answers PONG with the rest
   as it was. */
enum { NET_FRAME_PING = 3, NET_FRAME_PONG = 4 };
/* Sends type and payload (at most NET_FRAME_MAX - 2 bytes); 0 when not racing. */
int net_send_frame(int type, const void *payload, size_t n);
/* The oldest frame received, whole (out[0] type, out[1] sender's slot); its size, 0 if none. */
int net_receive_frame(uint8_t *out, size_t max);

/* ---- state hashes ---- */

typedef struct {
    const void *base;
    size_t size;
} NetRange;

/* A 64-bit hash (XXH64 per range, chained through the seed) of memory ranges: online.c's
   DHD_HASH_LOG of the race state, every NET_HASH_PERIOD fields. */
enum { NET_HASH_PERIOD = 60 };
uint64_t net_hash_ranges(const NetRange *ranges, int n);
uint64_t net_hash64(const void *data, size_t size, uint64_t seed);  /* XXH64 */

#endif
