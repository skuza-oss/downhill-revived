/* One online player: the lobby state machine and the race's relayed frames, driven by messages
   from any transport (ws.h, or test_loopback.c's fake relay). Thread-safe: the network
   threads deliver messages while the game thread sends and takes frames. */
#ifndef NET_CLIENT_H
#define NET_CLIENT_H

#include <SDL3/SDL_mutex.h>

#include "net.h"

enum { NET_PROTOCOL = 2 };

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
    /* The server's secret for a reconnect (hello.reconnectToken, finding 140): kept while a lobby
       drop reconnects into the same slot, sent in the hello of the next connection. Empty until a
       welcome carries one, and cleared again by a welcome without one (a server without the
       feature). */
    char reconnect_token[80];
    int server_chat;  /* the welcome said the server takes chat (an older one closes on it) */
    double dropped_at;  /* when a race's connection dropped (net_now_ms), 0 when connected */
    NetLobby lobby;
    int racing;
    /* what to repeat after a lobby reconnect */
    int picked, rider, bike, ready, choosing;
    double countdown_ends;  /* the server's countdown end, server ms; 0 none */
    /* pings and the server clock */
    double next_ping, last_rx, clock_offset;  /* offset: server ms - local ms */
    int have_offset;
    float rtt[5];
    int nrtt;
    /* STATE and EVENT frames received, oldest first (a full queue drops the oldest) */
    uint8_t relayed[NET_RELAY_QUEUE][NET_FRAME_MAX];
    uint16_t relayed_size[NET_RELAY_QUEUE];
    unsigned relayed_head, relayed_count;
    NetChat chat;  /* the last lines said (net.h) */
} NetClient;

double net_now_ms(void);  /* a monotonic clock */

void client_init(NetClient *c, const char *name, const char *version, const char *iso);
void client_free(NetClient *c);
/* The transport connected: hello goes out. */
void client_connected(NetClient *c, const NetTransport *tr);
void client_on_text(NetClient *c, const char *text, size_t n);
void client_on_binary(NetClient *c, const uint8_t *data, size_t n);

/* Voice chat (finding 131, port/host/voice.c): a race's voice EVENT frames ([type, slot, version,
   NET_VOICE_KIND, ...]) go to this hook the moment they arrive, on the connection's thread, and
   are not queued: the queue below is read once a game frame, which would hand voice over in
   bursts after every stall, and 50 voice frames a second a talker would push the race's
   snapshots out of it. payload starts at the version byte, as the game reads an EVENT. NULL:
   voice frames queue like any other EVENT (the game ignores the kind). */
enum { NET_VOICE_VERSION = 2, NET_VOICE_KIND = 11 };
extern void (*net_voice_hook)(int slot, const uint8_t *payload, int n);
/* The connection ended. Returns 1 when it should rejoin the room by its code: a lobby drop, or a
   race drop the server offered a rejoin for (a reconnect token, finding 140). */
int client_on_closed(NetClient *c, int code, const char *reason);
/* The transport stopped trying to reconnect: a race goes on offline, a lobby has failed. */
void client_give_up(NetClient *c);
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
int client_chat(NetClient *c, const char *text);  /* 1 sent */
void client_get_chat(NetClient *c, NetChat *out);

int client_send_frame(NetClient *c, int type, const void *payload, size_t n);
int client_receive_frame(NetClient *c, uint8_t *out, size_t max);

#endif
