/* One online player: the lobby state machine and the race's relayed frames, driven by messages
   from any transport (ws.h, or test_loopback.c's fake relay). Thread-safe: the network
   threads deliver messages while the game thread sends and takes frames. */
#ifndef NET_CLIENT_H
#define NET_CLIENT_H

#include <SDL3/SDL_mutex.h>

#include "net.h"

enum { NET_PROTOCOL = 3, NET_RTT_WINDOW = 128, NET_LOG_MS = 30000 };

typedef struct {
    void *ctx;
    void (*send_text)(void *ctx, const char *text);
    void (*send_binary)(void *ctx, const void *data, size_t n);
} NetTransport;

typedef struct {
    SDL_Mutex *lock;
    SDL_Condition *changed;
    NetTransport tr;
    int connected, leaving;
    char name[100], version[64], iso[65];
    /* this room's rejoin token from the welcome (hex; "" none yet): a rejoin's hello sends it back
       to get the member again (finding 137). client_init clears it for each new room. */
    char rejoin_token[65];
    /* this game's install id (32 lowercase hex, "" none; finding 161), sent in every hello */
    char install_id[33];
    NetLobby lobby;
    int racing;
    /* what to repeat after a lobby reconnect */
    int picked, rider, bike, ready, choosing;
    double countdown_ends;  /* the server's countdown end, server ms; 0 none */
    /* pings and the server clock */
    double next_ping, last_rx, clock_offset;  /* offset: server ms - local ms */
    int have_offset;   /* the offset is from a pong (the best measure) */
    int clock_known;   /* the offset is set at all: a pong, a welcome or the public list's "now" */
    float rtt[5];
    int nrtt;
    /* dhd.log's net: lines (finding 135): every member's region from the server's "where" (by
       member id), and this PC's round-trips since the last line */
    int where_count, where_id[NET_MAX_SLOTS];
    char where_region[NET_MAX_SLOTS][8];
    float window[NET_RTT_WINDOW];
    int window_n, window_count;
    float window_worst;
    double window_start, next_log, joined_at;  /* joined_at 0: not in a room (nothing to log) */
    char logged_colo[8];  /* the room location the log has named */
    /* Estimated age on the server clock: sender's existing GO stamp + snapshot race ms.
       Histograms are sampled every network tick; 1001 is the overflow bin. */
    double sender_go[NET_MAX_SLOTS];
    struct { double arrived, epoch; unsigned age[1002], samples, gaps; } snapshot[NET_MAX_SLOTS];
    /* STATE and EVENT frames received, oldest first (a full queue drops the oldest) */
    uint8_t relayed[NET_RELAY_QUEUE][NET_FRAME_MAX];
    uint16_t relayed_size[NET_RELAY_QUEUE];
    unsigned relayed_head, relayed_count;
} NetClient;

double net_now_ms(void);  /* a monotonic clock */

void client_init(NetClient *c, const char *name, const char *version, const char *iso);
/* The install id for the hellos (finding 161): kept only when it is 32 lowercase hex digits. */
void client_set_install_id(NetClient *c, const char *id);
void client_free(NetClient *c);
/* The transport connected: hello goes out. */
void client_connected(NetClient *c, const NetTransport *tr);
void client_on_text(NetClient *c, const char *text, size_t n);
void client_on_binary(NetClient *c, const uint8_t *data, size_t n);
/* The connection ended. Returns 1 when it should rejoin the room by its code (a lobby drop). */
int client_on_closed(NetClient *c, int code, const char *reason);
/* Every few ms from a network thread: pings. Returns 1 when the server has been silent too long
   and the connection should be dropped. */
int client_tick(NetClient *c);

void client_set_room(NetClient *c, const NetRoomSettings *s);
void client_pick(NetClient *c, int member, int rider, int bike);
void client_ready(NetClient *c, int ready);
void client_kick(NetClient *c, int member);
void client_start(NetClient *c);
void client_end(NetClient *c);
void client_finish(NetClient *c, unsigned ticks, int place);
void client_back(NetClient *c);
void client_rules(NetClient *c, int finish_window, int next_race, int then);
void client_rotation(NetClient *c, const unsigned char *ids, int count);
void client_leave(NetClient *c);  /* says so before the transport closes */
void client_lobby(NetClient *c, NetLobby *out);
/* The public list's answer (GET /v1/lobbies) into out, fetched between local times sent and got
   (net_now_ms). Its "now" sets the server clock when no room's pings have (finding 135). Returns
   the rooms, -1 when the text isn't a list. */
int client_read_lobbies(NetClient *c, const char *body, double sent, double got, NetLobbyInfo *out, int max);

int client_send_frame(NetClient *c, int type, const void *payload, size_t n);
int client_receive_frame(NetClient *c, uint8_t *out, size_t max);

#endif
