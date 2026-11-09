/* net.h's API: one client (client.c) over one WebSocket (ws.h). The server's address
   comes from net_init or the DHD_NET_SERVER environment variable (the official server unless
   dhd.ini or the environment names another, port/hle/host.c). */
#include "net.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "json.h"
#include "ws.h"

enum { REJOIN_ATTEMPTS = 5, SFU_CODE_LENGTH = 6, P2P_CODE_LENGTH = 7 };

/* The rooms offered (host_api.h NET_OFFER_*). SFU rooms are not offered on phones yet (finding
   143): their speed and the touch controls come first; Android builds refuse every SFU connection
   here, whatever asks (the menu, DHD_NET_*, a dhd.ini server). P2P rooms are (finding 180). */
#ifdef __ANDROID__
#define NET_OFFERED NET_OFFER_P2P
#else
#define NET_OFFERED (NET_OFFER_SFU | NET_OFFER_P2P)
#endif
#define NET_NOT_OFFERED "this kind of room is not offered here yet"

static NetClient client;
static WsConn *conn;
static int inited;
static char base[256], name[100], version[64], iso[65], next_url[300];
static SDL_Mutex *api;  /* serialises connect, leave and the reset between rooms */
/* This room is a P2P one: no rejoin when its connection drops (the host carries the race; a
   guest who drops is the host's AI rider from then on). */
static int room_p2p;

static void send_text(void *ws, const char *text) { ws_send(ws, 0, text, strlen(text)); }
static void send_binary(void *ws, const void *data, size_t n) { ws_send(ws, 1, data, n); }

static void on_open(void *ctx, WsConn *ws)
{
    NetTransport tr;
    (void)ctx;
    tr.ctx = ws, tr.send_text = send_text, tr.send_binary = send_binary;
    client_connected(&client, &tr);
}

static void on_message(void *ctx, int binary, const uint8_t *data, size_t n)
{
    (void)ctx;
    if (binary)
        client_on_binary(&client, data, n);
    else
        client_on_text(&client, (const char *)data, n);
}

static const char *on_close(void *ctx, int code, const char *reason, int attempt)
{
    NetLobby lobby;
    (void)ctx;
    if (room_p2p) {
        client_on_closed(&client, code, reason);
        client_rejoin_failed(&client);
        return NULL;
    }
    if (!client_on_closed(&client, code, reason))
        return NULL;
    if (attempt >= REJOIN_ATTEMPTS) {
        client_rejoin_failed(&client);
        return NULL;
    }
    client_lobby(&client, &lobby);
    snprintf(next_url, sizeof next_url, "%s/v1/rooms/%s", base, lobby.code);
    return next_url;
}

static int on_tick(void *ctx)
{
    /* the send thread of conn itself: net_leave clears conn before it stops the connection, and
       a connection's threads end before the next one starts */
    WsConn *ws = conn;
    (void)ctx;
    client_race_rtt(&client, ws ? ws_race_rtt_ms(ws) : -1);
    return client_tick(&client);
}

void net_init(const char *base_url, const char *player_name, const char *build_version, const char *iso_sha256)
{
    const char *url = base_url && *base_url ? base_url : getenv("DHD_NET_SERVER");
    size_t n;
    if (!inited) {
        api = SDL_CreateMutex();
        client_init(&client, "", "", "");
        inited = 1;
    }
    snprintf(base, sizeof base, "%s", url ? url : "");
    n = strlen(base);
    while (n && base[n - 1] == '/')
        base[--n] = 0;
    snprintf(name, sizeof name, "%s", player_name);
    snprintf(version, sizeof version, "%s", build_version);
    snprintf(iso, sizeof iso, "%s", iso_sha256);
}

static int connect_to(const char *path, int p2p)
{
    char url[300];
    WsHandler h = {NULL, on_open, on_message, on_close, on_tick};
    if (!inited)
        net_init(NULL, "Rider", "", "");
    net_leave();
    SDL_LockMutex(api);
    client_free(&client);
    client_init(&client, name, version, iso);
    /* the install id (port/hle/host.c; finding 161), read here on the game's thread, the one that
       sets the environment */
    client_set_install_id(&client, getenv("DHD_NET_INSTALL_ID"));
    if (!(NET_OFFERED & (p2p ? NET_OFFER_P2P : NET_OFFER_SFU))) {
        client.lobby.state = NET_FAILED;
        snprintf(client.lobby.error, sizeof client.lobby.error, NET_NOT_OFFERED);
        SDL_UnlockMutex(api);
        return -1;
    }
    if (!base[0]) {
        client.lobby.state = NET_FAILED;
        snprintf(client.lobby.error, sizeof client.lobby.error, "no server: set DHD_NET_SERVER");
        SDL_UnlockMutex(api);
        return -1;
    }
    client.lobby.state = NET_CONNECTING;
    client.lobby.is_p2p = room_p2p = p2p;
    snprintf(url, sizeof url, "%s%s", base, path);
    conn = ws_start(url, &h);
    SDL_UnlockMutex(api);
    return 0;
}

int net_create_room(int p2p)
{
    /* the region its creator picked (finding 181): a hint's letters only, never anything else in
       the URL; "auto", unset or anything odd lets the server pick. A P2P room has none: its
       server only relays signaling (finding 180). */
    const char *region = getenv("DHD_NET_ROOM_REGION");
    char path[64];
    if (p2p)
        snprintf(path, sizeof path, "/v1/p2p/rooms/new");
    else if (region && *region && strcmp(region, "auto") && strlen(region) < 16 && strspn(region, "abcdefghijklmnopqrstuvwxyz-") == strlen(region))
        snprintf(path, sizeof path, "/v1/rooms/new?region=%s", region);
    else
        snprintf(path, sizeof path, "/v1/rooms/new");
    return connect_to(path, p2p);
}

int net_join_room(const char *code)
{
    static const char alphabet[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
    char path[32], up[P2P_CODE_LENGTH + 2];
    int i, p2p;
    for (i = 0; code[i] && i < P2P_CODE_LENGTH + 1; i++)
        up[i] = (char)toupper((unsigned char)code[i]);
    up[i] = 0;
    p2p = i == P2P_CODE_LENGTH;
    if ((i != SFU_CODE_LENGTH && !p2p) || strspn(up, alphabet) != (size_t)i)
        return -1;
    if (!(NET_OFFERED & (p2p ? NET_OFFER_P2P : NET_OFFER_SFU)))
        return -1;
    snprintf(path, sizeof path, p2p ? "/v1/p2p/rooms/%s" : "/v1/rooms/%s", up);
    return connect_to(path, p2p);
}

void net_leave(void)
{
    WsConn *c;
    if (!inited)
        return;
    SDL_LockMutex(api);
    c = conn, conn = NULL;
    if (c) {
        client_leave(&client);
        ws_stop(c);
    }
    SDL_UnlockMutex(api);
}

int net_list_lobbies(NetLobbyInfo *out, int max)
{
    static char body[64 * 1024];
    char url[300];
    double sent, got;
    int n;
    if (!(NET_OFFERED & NET_OFFER_SFU) || !base[0] || !inited)  /* P2P rooms are never listed */
        return -1;
    snprintf(url, sizeof url, "%s/v1/lobbies", base);
    sent = net_now_ms();
    if (http_get(url, body, sizeof body) < 0)
        return -1;
    got = net_now_ms();
    SDL_LockMutex(api);  /* not while connect_to makes the client again */
    n = client_read_lobbies(&client, body, sent, got, out, max);
    SDL_UnlockMutex(api);
    return n;
}

void net_set_room(const NetRoomSettings *s) { client_set_room(&client, s); }
void net_pick(int member, int rider, int bike) { client_pick(&client, member, rider, bike); }
void net_set_ready(int ready) { client_ready(&client, ready); }
void net_kick(int member) { client_kick(&client, member); }
void net_start_race(void) { client_start(&client); }
void net_end_race(void) { client_end(&client); }
void net_finish(unsigned ticks, int place) { client_finish(&client, ticks, place); }
void net_back(void) { client_back(&client); }
void net_rules(int finish_window, int next_race, int then) { client_rules(&client, finish_window, next_race, then); }
void net_rotation(const unsigned char *ids, int count) { client_rotation(&client, ids, count); }

void net_get_lobby(NetLobby *out)
{
    if (!inited) {
        memset(out, 0, sizeof *out);
        return;
    }
    client_lobby(&client, out);
}

double net_seconds(void)
{
    return net_now_ms() / 1000.0;
}

double net_server_ms(void)
{
    return net_now_ms() + client.clock_offset;
}

/* This PC's ping while in a room, else -1 (the FPS & Ping counter, port/gs/gpu_rust.c): a few
   fields, read as net_racing reads its flag, not a copy of the lobby for every picture. */
int net_ping_ms(void)
{
    const NetLobby *l = &client.lobby;
    return inited && (l->state == NET_LOBBY || l->state == NET_RACING) && !l->offline && l->code[0] ? l->ping_ms : -1;
}

/* In a race, the other riders' delay (ms; client_riders_ms), else -1: the counter shows it
   instead of the ping to the room's server, which carries no race data (finding 169). */
int net_race_ms(void)
{
    const NetLobby *l = &client.lobby;
    return inited && (l->state == NET_LOBBY || l->state == NET_RACING) && !l->offline && l->code[0] ? client_race_ms(&client) : -1;
}

int net_riders_ms(void)
{
    return inited && client.racing && !client.lobby.offline ? client_riders_ms(&client) : -1;
}

int net_racing(void)
{
    return inited && client.racing;
}

int net_local_slot(void)
{
    return client.lobby.local_slot;
}





int net_send_frame(int type, const void *payload, size_t n)
{
    return inited && client_send_frame(&client, type, payload, n);
}

int net_receive_frame(uint8_t *out, size_t max)
{
    return inited ? client_receive_frame(&client, out, max) : 0;
}


/* Guest-address-only module boundary; native device code stays above. */
#include "../host/host_api.h"
#include "../include/guest.h"
void host_service_net_init(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) { net_init((const char *)GUEST(a0), (const char *)GUEST(a1), (const char *)GUEST(a2), (const char *)GUEST(a3)); }
int host_service_net_offered(void) { return NET_OFFERED; }
int host_service_net_create_room(int p2p) { return net_create_room(p2p); }
int host_service_net_join_room(uint32_t a0) { return net_join_room((const char *)GUEST(a0)); }
void host_service_net_leave(void) { net_leave(); }
void host_service_net_get_lobby(uint32_t a0) { net_get_lobby((NetLobby *)GUEST(a0)); }
void host_service_net_pick(int a0, int a1, int a2) { net_pick(a0, a1, a2); }
void host_service_net_finish(uint32_t ticks, int place) { net_finish(ticks, place); }
void host_service_net_back(void) { net_back(); }
void host_service_net_rules(int finish_window, int next_race, int then) { net_rules(finish_window, next_race, then); }
void host_service_net_rotation(uint32_t address, int count) { if (count >= 0 && count <= 64) net_rotation((const unsigned char *)GUEST(address), count); }
void host_service_net_end_race(void) { net_end_race(); }
void host_service_net_set_ready(int a0) { net_set_ready(a0); }
void host_service_net_start_race(void) { net_start_race(); }
int host_service_net_racing(void) { return net_racing(); }
double host_service_net_seconds(void) { return net_seconds(); }
double host_service_net_server_ms(void) { return net_server_ms(); }
int host_service_net_send_frame(int a0, uint32_t a1, uint32_t a2) { return net_send_frame(a0, (const void *)GUEST(a1), a2); }
int host_service_net_receive_frame(uint32_t a0, uint32_t a1) { return net_receive_frame((uint8_t *)GUEST(a0), a1); }
int host_service_net_list_lobbies(uint32_t a0, int a1) { return net_list_lobbies((NetLobbyInfo *)GUEST(a0), a1); }

void host_service_net_set_room(uint32_t address) {
    const uint32_t *in=GUEST(address);
    NetRoomSettings out;
    out.track=(int)in[0]; out.laps=(int)in[1];
    out.is_public=(int)in[2]; out.max_players=(int)in[3];
    memcpy(&out.seed,in+4,8);
    out.room_name=in[6] ? GUEST(in[6]) : NULL;  /* 0: the name stays (a track or bots change) */
    net_set_room(&out);
}
