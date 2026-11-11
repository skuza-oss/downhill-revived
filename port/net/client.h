/* One online player: the lobby state machine and the race's relayed frames, driven by messages
   from any transport (ws.h, or test_loopback.c's fake relay). Thread-safe: the network
   threads deliver messages while the game thread sends and takes frames. */
#ifndef NET_CLIENT_H
#define NET_CLIENT_H

#include <SDL3/SDL_mutex.h>

#include "net.h"

enum { NET_PROTOCOL = 4, NET_RTT_WINDOW = 128, NET_LOG_MS = 30000, NET_ARRIVALS = 64 };

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
    /* the room's welcome listed "race_go": it takes the race's start moment (finding 187) */
    int server_race_go;
    /* the room's welcome listed "score": a round's finish may carry its score (finding 204) */
    int server_scores;
    NetLobby lobby;
    int racing;
    /* In a race whose room connection dropped (finding 175): the race goes on over the SFU while
       the transport rejoins; race frames are still sent. */
    int rejoining;
    unsigned rejoin_tries;  /* closes since the last welcome */
    /* the race this game is in, its start's startAt (server ms; 0 none), sent in a rejoin's hello;
       its finish or BACK, said again after a rejoin in case the drop lost it */
    double race_at;
    int finished, finish_place, gave_up;
    unsigned finish_ticks;
    int finish_score;  /* a scored round's (finding 204), -1 none */
    /* what to repeat after a lobby reconnect */
    int picked, rider, bike, ready, choosing;
    double countdown_ends;  /* the server's countdown end, server ms; 0 none */
    /* pings and the server clock */
    double next_ping, last_rx, clock_offset;  /* offset: server ms - local ms */
    int have_offset;   /* the offset is from a pong (the best measure) */
    /* the quickest recent round trip (ms, -1 none), rising slowly with time: only pongs near it
       measure the clock (client.c clock_sample) */
    double rtt_floor, rtt_floor_at;
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
    /* The race ping (finding 181): the SFU association's round trip, sampled once a second
       (client_race_rtt); the last 5 for the counter (race_ms, their median, -1 none) and the
       samples since the last net: line. */
    float race_rtt[5];
    int nrace_rtt, race_ms;
    double next_race_sample;
    float race_window[NET_RTT_WINDOW];
    int race_window_n;
    float race_window_worst;
    /* Each sender's newest snapshot age on the server clock (now minus the bundle's stamp, the
       moment its snapshots show), sampled every network tick; 1001 is the overflow
       bin. Messages: STATE messages received from that sender; states_sent: ours. */
    struct {
        double arrived, epoch;
        unsigned age[1002], samples, gaps, messages;
        float arrival[NET_ARRIVALS];  /* the last snapshots' ages on arrival (ms), a ring */
        unsigned arrivals;
    } snapshot[NET_MAX_SLOTS];
    unsigned states_sent;
    /* DHD_NET_TEST_PONG_DELAY=N[@S] (a test switch, finding 175): from S seconds (default 60) after
       the first welcome, every pong (the room's JSON one and the race's PONG frame) is held N ms
       plus 0-N/2 ms more before it counts, a slow return path one way. Held pongs, oldest first. */
    double pong_delay_ms, pong_delay_from_s;
    unsigned pong_delays;
    struct { double due; int binary; uint16_t n; char data[128]; } held[16];
    unsigned held_head, held_count;
    int releasing;  /* client_tick hands a held pong over: it counts now */
    /* DHD_NET_TEST_DROP=S (a test switch, finding 175): the room connection is cut once in this
       room, S seconds after a race's startAt (negative: during its setup; the start message comes
       3 s before startAt), as a network drop is (no close frame) */
    double test_drop_s;
    int test_drop, test_dropped;
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
/* The connection ended. Returns 1 when it should rejoin the room by its code (a drop in the
   room or, finding 175, in a race). */
int client_on_closed(NetClient *c, int code, const char *reason);
/* The rejoin's attempts are over: the room is lost (in a race: this PC races on alone). */
void client_rejoin_failed(NetClient *c);
/* Every few ms from a network thread: pings. Returns 1 when the server has been silent too long
   and the connection should be dropped. */
int client_tick(NetClient *c);

void client_set_room(NetClient *c, const NetRoomSettings *s);
void client_pick(NetClient *c, int member, int rider, int bike);
void client_ready(NetClient *c, int ready);
void client_kick(NetClient *c, int member);
void client_start(NetClient *c);
void client_end(NetClient *c);
void client_finish(NetClient *c, unsigned ticks, int place, int score);
void client_back(NetClient *c);
void client_rules(NetClient *c, int finish_window, int next_race, int then);
void client_rotation(NetClient *c, const unsigned char *ids, int count);
/* The race's start moment this game named (server ms), to a room that takes it (finding 187). */
void client_race_go(NetClient *c, double at);
void client_leave(NetClient *c);  /* says so before the transport closes */
void client_lobby(NetClient *c, NetLobby *out);
/* The public list's answer (GET /v1/lobbies) into out, fetched between local times sent and got
   (net_now_ms). Its "now" sets the server clock when no room's pings have (finding 135). Returns
   the rooms, -1 when the text isn't a list. */
int client_read_lobbies(NetClient *c, const char *body, double sent, double got, NetLobbyInfo *out, int max);

int client_send_frame(NetClient *c, int type, const void *payload, size_t n);
/* The other riders' delay for the counter in a race (ms), -1 none (finding 169). */
int client_riders_ms(NetClient *c);
/* The network thread, every tick: the transport's race ping (ws_race_rtt_ms; -1 none). Sampled
   once a second for the counter and the net: lines (finding 181). */
void client_race_rtt(NetClient *c, int ms);
int client_race_ms(NetClient *c);  /* the counter's race ping, -1 none */
int client_receive_frame(NetClient *c, uint8_t *out, size_t max);

#endif
