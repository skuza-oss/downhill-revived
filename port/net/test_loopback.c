/* Four players in one process: client.c over a fake transport, through a fake relay that does
   what online/server does (docs/online-protocol.md): welcome and room snapshots, slots at start,
   STATE and EVENT relayed with the slot byte overwritten, PING answered, left when a socket
   closes, or away and back when a racer with a reconnect token drops and rejoins (finding 140). Each player has its own one-way latency; delivery keeps each socket's order. Each
   player's game thread sends numbered STATE frames for a while, as online.c does about 20 times
   a second, and checks that every other player's frames arrive whole and in order. */
#include "client.h"
#include "json.h"

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); failures++; } } while (0)

enum { PLAYERS = 4, RACE_MS = 3000, FRAME_MS = 50 };

typedef struct Msg { struct Msg *next; double due; int binary; size_t n; char data[]; } Msg;

typedef struct {
    NetClient client;
    int index, latency_ms;     /* one way, to the relay */
    int open, member, slot;
    int ready, rider, bike;
    Msg *head, *tail;          /* to deliver, in order */
    double last_due;
    HANDLE thread, game;
    volatile LONG stop;
    /* the race */
    int drop_at_ms, rejoin_after_ms;  /* rejoin: away, then back in its slot after this long */
    double dropped_at;
    int away, saw_away, dropped;
    int sent, got[PLAYERS], out_of_order;
    int said;                  /* chat lines the relay took from this player */
} Player;

static Player players[PLAYERS];
static CRITICAL_SECTION relay;
static int next_member, host_member, started;

static void token_of(const Player *p, char *out)  /* the reconnect token the relay gave p */
{
    int i;
    for (i = 0; i < 64; i++)
        out[i] = "0123456789abcdef"[(p->index * 7 + i) & 15];
    out[64] = 0;
}

static void deliver(Player *to, const Player *from, int binary, const void *data, size_t n)
{
    Msg *m = malloc(sizeof *m + n + 1);
    double due = net_now_ms() + from->latency_ms + to->latency_ms;
    if (!to->open)
        return free(m);
    if (due < to->last_due)
        due = to->last_due;  /* one socket: in order */
    to->last_due = due;
    m->next = NULL, m->due = due, m->binary = binary, m->n = n;
    memcpy(m->data, data, n);
    m->data[n] = 0;
    if (to->tail)
        to->tail->next = m;
    else
        to->head = m;
    to->tail = m;
}

static void text_to(Player *to, const Player *from, const char *text)
{
    deliver(to, from, 0, text, strlen(text));
}

static void send_room(void)
{
    char text[2048];
    int n, i, first = 1;
    n = snprintf(text, sizeof text, "{\"type\":\"room\",\"code\":\"TEST22\",\"hostId\":%d,\"roomName\":\"loop\","
                 "\"public\":false,\"maxPlayers\":4,\"started\":%s,\"version\":\"test\",\"settings\":{\"track\":3,"
                 "\"laps\":2,\"seed\":null},\"members\":[", host_member, started ? "true" : "false");
    for (i = 0; i < PLAYERS; i++)
        if (players[i].open && players[i].member >= 0) {
            n += snprintf(text + n, sizeof text - n, "%s{\"id\":%d,\"name\":\"P%d\",\"ready\":%s,\"rider\":%d,"
                          "\"bike\":%d,\"rttMs\":%d}", first ? "" : ",", players[i].member, i,
                          players[i].ready ? "true" : "false", players[i].rider, players[i].bike,
                          players[i].latency_ms * 2);
            first = 0;
        }
    snprintf(text + n, sizeof text - n, "]}");
    for (i = 0; i < PLAYERS; i++)
        text_to(&players[i], &players[i], text);
}

static void start_race(void)
{
    char text[2048];
    int i, slot = 0;
    double now = (double)time(NULL) * 1000;
    for (i = 0; i < PLAYERS; i++)
        players[i].slot = slot++;
    started = 1;
    for (i = 0; i < PLAYERS; i++) {
        int n = snprintf(text, sizeof text, "{\"type\":\"start\",\"yourSlot\":%d,\"track\":3,\"laps\":2,"
                         "\"seed\":2864434397,\"players\":[", players[i].slot), j;
        for (j = 0; j < PLAYERS; j++)
            n += snprintf(text + n, sizeof text - n, "%s{\"slot\":%d,\"memberId\":%d,\"name\":\"P%d\",\"rider\":%d,"
                          "\"bike\":%d}", j ? "," : "", players[j].slot, players[j].member, j, players[j].rider,
                          players[j].bike);
        snprintf(text + n, sizeof text - n, "],\"serverTime\":%.0f,\"startAt\":%.0f}", now, now + 300);
        text_to(&players[i], &players[i], text);
    }
}

static void relay_text(Player *p, const char *text)
{
    char type[24], reply[256];
    long long v;
    double t;
    json_string(text, "type", type, sizeof type);
    if (!strcmp(type, "hello")) {
        char name[64], iso[80], token[80], want[80];
        int i;
        CHECK(json_int(text, "protocol", &v) && v == NET_PROTOCOL);
        CHECK(json_string(text, "isoSha256", iso, sizeof iso) && strlen(iso) == 64);
        CHECK(json_string(text, "name", name, sizeof name) && name[0] == 'P');
        CHECK(strstr(text, "\"reconnect\"") != NULL);  /* in features */
        token_of(p, want);
        if (json_string(text, "reconnectToken", token, sizeof token)) {
            /* a rejoin: the right token, into the held slot, and everyone hears back */
            CHECK(started && p->away && !strcmp(token, want));
            p->open = 1, p->away = 0;
            snprintf(reply, sizeof reply, "{\"type\":\"welcome\",\"protocol\":1,\"code\":\"TEST22\",\"memberId\":%d,"
                     "\"host\":%s,\"serverTime\":1,\"chat\":true,\"reconnectToken\":\"%s\",\"slot\":%d}", p->member,
                     p->member == host_member ? "true" : "false", want, p->slot);
            text_to(p, p, reply);
            snprintf(reply, sizeof reply, "{\"type\":\"back\",\"slot\":%d,\"memberId\":%d}", p->slot, p->member);
            for (i = 0; i < PLAYERS; i++)
                text_to(&players[i], p, reply);
            return;
        }
        p->member = next_member++;
        if (host_member < 0)
            host_member = p->member;
        /* a token only for the player that will rejoin: the others drop as before */
        snprintf(reply, sizeof reply, "{\"type\":\"welcome\",\"protocol\":1,\"code\":\"TEST22\",\"memberId\":%d,"
                 "\"host\":%s,\"serverTime\":1,\"chat\":true%s%s%s}", p->member, p->member == host_member ? "true" : "false",
                 p->rejoin_after_ms ? ",\"reconnectToken\":\"" : "", p->rejoin_after_ms ? want : "",
                 p->rejoin_after_ms ? "\"" : "");
        text_to(p, p, reply);
        send_room();
    } else if (!strcmp(type, "ready") && json_bool(text, "ready", &p->ready))
        send_room();
    else if (!strcmp(type, "pick")) {
        CHECK(!json_find(text, "member"));
        json_int(text, "rider", &v), p->rider = (int)v;
        json_int(text, "bike", &v), p->bike = (int)v;
        send_room();
    } else if (!strcmp(type, "start")) {
        int i, all = 1;
        CHECK(p->member == host_member);
        for (i = 0; i < PLAYERS; i++)
            all &= players[i].ready;
        if (all)
            start_race();
        else
            text_to(p, p, "{\"type\":\"error\",\"code\":\"not_ready\",\"message\":\"Not ready.\"}");
    } else if (!strcmp(type, "ping") && json_number(text, "t", &t)) {
        snprintf(reply, sizeof reply, "{\"type\":\"pong\",\"t\":%.3f,\"serverTime\":%.0f}", t, (double)time(NULL) * 1000);
        text_to(p, p, reply);
    } else if (!strcmp(type, "chat")) {
        /* the server's rules (src/room.ts): 1-120 characters, 5 a player, then slow_down */
        char said[512], escaped[1024], line[1200];
        int i;
        CHECK(json_string(text, "text", said, sizeof said) && said[0] && strlen(said) <= NET_CHAT_MAX);
        CHECK(said[0] != ' ' && said[strlen(said) - 1] != ' ');
        if (++p->said > 5) {
            text_to(p, p, "{\"type\":\"error\",\"code\":\"slow_down\",\"message\":\"Too many messages: wait a moment.\"}");
            return;
        }
        json_escape(escaped, sizeof escaped, said);
        snprintf(line, sizeof line, "{\"type\":\"chat\",\"memberId\":%d,\"name\":\"P%d\",\"text\":\"%s\",\"at\":%d}",
                 p->member, p->index, escaped, 1000 + p->said);
        for (i = 0; i < PLAYERS; i++)
            text_to(&players[i], p, line);
    } else if (!strcmp(type, "pong") || !strcmp(type, "leave"))
        ;
    else {
        printf("  relay: unexpected %s\n", text);
        failures++;
    }
}

static void relay_binary(Player *p, const uint8_t *data, size_t n)
{
    uint8_t frame[NET_FRAME_MAX];
    int i;
    CHECK(n >= 2 && n <= sizeof frame);
    if (n < 2 || n > sizeof frame)
        return;
    memcpy(frame, data, n);
    frame[1] = (uint8_t)p->slot;  /* the server says who sent it */
    if (frame[0] == NET_FRAME_STATE || frame[0] == NET_FRAME_EVENT) {
        for (i = 0; i < PLAYERS; i++)
            if (&players[i] != p)
                deliver(&players[i], p, 1, frame, n);
    } else if (frame[0] == NET_FRAME_PING) {
        frame[0] = NET_FRAME_PONG;
        deliver(p, p, 1, frame, n);
    } else {
        printf("  relay: unexpected binary frame type %d\n", frame[0]);
        failures++;
    }
}

static void tr_text(void *ctx, const char *text)
{
    EnterCriticalSection(&relay);
    relay_text(ctx, text);
    LeaveCriticalSection(&relay);
}

static void tr_binary(void *ctx, const void *data, size_t n)
{
    EnterCriticalSection(&relay);
    if (((Player *)ctx)->open)
        relay_binary(ctx, data, n);
    LeaveCriticalSection(&relay);
}

/* The socket closes: the others hear left, after every frame already relayed; or away, when the
   player can rejoin (it had a token). */
static void drop(Player *p)
{
    char text[160];
    int i, rejoin;
    EnterCriticalSection(&relay);
    p->open = 0;
    p->away = p->rejoin_after_ms > 0;
    if (p->away)
        snprintf(text, sizeof text, "{\"type\":\"away\",\"slot\":%d,\"memberId\":%d}", p->slot, p->member);
    else
        snprintf(text, sizeof text, "{\"type\":\"left\",\"slot\":%d,\"memberId\":%d,\"reason\":\"disconnect\"}",
                 p->slot, p->member);
    for (i = 0; i < PLAYERS; i++)
        if (players[i].open)
            text_to(&players[i], p, text);
    LeaveCriticalSection(&relay);
    rejoin = client_on_closed(&p->client, 1006, "dropped");
    CHECK(rejoin == p->away);  /* with a token the client wants to come back */
    p->dropped = 1;
    p->dropped_at = net_now_ms();
}

/* The transport is back (as ws.h reconnects): hello with the token, back in the same slot. */
static void reconnect(Player *p)
{
    NetTransport tr;
    tr.ctx = p, tr.send_text = tr_text, tr.send_binary = tr_binary;
    p->dropped_at = 0;
    client_connected(&p->client, &tr);
}

/* The network thread: deliveries when due, and ticks. */
static DWORD WINAPI network(void *arg)
{
    Player *p = arg;
    while (!p->stop) {
        Msg *m = NULL;
        EnterCriticalSection(&relay);
        if (p->head && p->head->due <= net_now_ms()) {
            m = p->head;
            p->head = m->next;
            if (!p->head)
                p->tail = NULL;
        }
        LeaveCriticalSection(&relay);
        if (m) {
            if (p->open) {
                if (m->binary)
                    client_on_binary(&p->client, (uint8_t *)m->data, m->n);
                else
                    client_on_text(&p->client, m->data, m->n);
            }
            free(m);
            continue;
        }
        client_tick(&p->client);
        Sleep(1);
    }
    return 0;
}

/* The game thread: a numbered STATE frame every FRAME_MS, as online.c's state_sync sends, and
   the others' frames taken as they come. */
static DWORD WINAPI game(void *arg)
{
    Player *p = arg;
    uint8_t frame[NET_FRAME_MAX];
    int32_t last[PLAYERS], seq;
    double start = net_now_ms(), next = start;
    int i, n;
    for (i = 0; i < PLAYERS; i++)
        last[i] = -1;
    while (net_now_ms() < start + RACE_MS + 500) {  /* (and half a second for the last frames) */
        if (p->drop_at_ms && net_now_ms() >= start + p->drop_at_ms && !p->dropped)
            drop(p);
        if (p->dropped_at && p->rejoin_after_ms && net_now_ms() >= p->dropped_at + p->rejoin_after_ms) {
            NetLobby l;
            client_lobby(&p->client, &l);
            CHECK(l.state == NET_RECONNECTING && l.rejoining && !l.offline);
            reconnect(p);
        }
        {
            NetLobby l;
            client_lobby(&p->client, &l);
            if (l.racers_away)
                p->saw_away = 1;
        }
        if (net_now_ms() >= next && net_now_ms() < start + RACE_MS && p->open) {
            uint8_t payload[64];
            memset(payload, p->index, sizeof payload);
            memcpy(payload, &p->sent, 4);
            if (client_send_frame(&p->client, NET_FRAME_STATE, payload, sizeof payload))
                p->sent++;
            next += FRAME_MS;
        }
        while ((n = client_receive_frame(&p->client, frame, sizeof frame)) > 0) {
            int from = frame[1];
            CHECK(n == 2 + 64 && frame[0] == NET_FRAME_STATE && from < PLAYERS && from != p->slot);
            if (n != 2 + 64 || from >= PLAYERS)
                continue;
            memcpy(&seq, frame + 2, 4);
            if (seq != last[from] + 1)
                p->out_of_order++;
            last[from] = seq;
            p->got[from]++;
        }
        Sleep(1);
    }
    return 0;
}

/* Chat in the room: a line from each player reaches everyone in order, the sender too; a line is
   trimmed, and cut to NET_CHAT_MAX bytes between UTF-8 characters; the sixth line in a row is
   refused (slow_down), which only its sender sees, as a note in its own chat. */
static void chat(void)
{
    char line[200], want[16];
    NetChat ch;
    double until;
    int i, k;
    CHECK(!client_chat(&players[0].client, "  \t "));
    for (i = 0; i < PLAYERS; i++) {
        snprintf(line, sizeof line, "  hi from P%d\t ", i);
        CHECK(client_chat(&players[i].client, line));
    }
    /* 119 bytes, then a two-byte character across the limit: it goes whole or not at all */
    memset(line, 'x', 119);
    snprintf(line + 119, sizeof line - 119, "\xC3\xA9 and more");
    CHECK(client_chat(&players[1].client, line));
    for (k = 0; k < 5; k++)
        CHECK(client_chat(&players[2].client, "gg"));  /* its sixth line: refused */
    until = net_now_ms() + 3000;
    do {
        Sleep(10);
        client_get_chat(&players[3].client, &ch);
    } while (ch.seq < 9 && net_now_ms() < until);
    for (i = 0; i < PLAYERS; i++) {
        client_get_chat(&players[i].client, &ch);
        CHECK(ch.seq == (i == 2 ? 10u : 9u) && ch.count == NET_CHAT_LINES);
        /* the oldest line went: the ring has lines 2..9 (player 2: 3..9 and its note) */
        if (i == 2) {
            CHECK(ch.line[NET_CHAT_LINES - 1].member_id == -1 && strstr(ch.line[NET_CHAT_LINES - 1].text, "wait"));
            continue;
        }
        for (k = 0; k < 3; k++) {  /* lines 2..4: players 1-3's hellos */
            snprintf(want, sizeof want, "hi from P%d", k + 1);
            CHECK(!strcmp(ch.line[k].text, want) && ch.line[k].member_id == players[k + 1].member);
            CHECK(ch.line[k].seq == (unsigned)k + 2 && !strcmp(ch.line[k].name, ch.line[k].text + 8));
        }
        CHECK(strlen(ch.line[3].text) == 119 && ch.line[3].text[118] == 'x');
        CHECK(!strcmp(ch.line[4].text, "gg") && ch.line[4].at == 1002);
        CHECK(ch.line[7].member_id == players[2].member);
    }
    printf("  chat: %u lines, slow_down after five\n", ch.seq);
}

static const int *lan_latency;  /* chat() runs in that round */

static void run(const char *name, const int latency[PLAYERS], int dropper, int rejoin)
{
    int i;
    NetTransport tr;
    double until;
    NetLobby lobby;
    printf("%s\n", name);
    memset(players, 0, sizeof players);
    next_member = 0, host_member = -1, started = 0;
    for (i = 0; i < PLAYERS; i++) {
        Player *p = &players[i];
        char who[8];
        snprintf(who, sizeof who, "P%d", i);
        p->index = i, p->latency_ms = latency[i], p->member = -1, p->open = 1;
        p->rejoin_after_ms = i == dropper ? rejoin : 0;
        client_init(&p->client, who, "test", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        p->thread = CreateThread(NULL, 0, network, p, 0, NULL);
    }
    for (i = 0; i < PLAYERS; i++) {  /* join in order: player 0 hosts */
        tr.ctx = &players[i], tr.send_text = tr_text, tr.send_binary = tr_binary;
        client_connected(&players[i].client, &tr);
        Sleep(30);
    }
    for (i = 0; i < PLAYERS; i++) {
        client_pick(&players[i].client, -1, i + 1, i % 2);
        client_ready(&players[i].client, 1);
    }
    until = net_now_ms() + 3000;
    do {
        Sleep(10);
        client_lobby(&players[0].client, &lobby);
    } while ((lobby.nmembers < PLAYERS || !lobby.member[PLAYERS - 1].ready) && net_now_ms() < until);
    CHECK(lobby.state == NET_LOBBY && lobby.is_host && lobby.nmembers == PLAYERS);
    CHECK(lobby.member[2].rider == 3 && lobby.member[3].bike == 1 && lobby.member[1].rtt_ms == latency[1] * 2);
    if (dropper < 0 && latency == lan_latency)
        chat();
    client_start(&players[0].client);
    until = net_now_ms() + 3000;
    for (i = 0; i < PLAYERS; i++) {
        do {
            Sleep(5);
            client_lobby(&players[i].client, &lobby);
        } while (lobby.state != NET_RACING && net_now_ms() < until);
        CHECK(lobby.state == NET_RACING && lobby.nslots == PLAYERS && lobby.local_slot == i);
        CHECK(lobby.racer[2].rider == 3 && lobby.race_seed == 2864434397u);
    }
    for (i = 0; i < PLAYERS; i++) {
        Player *p = &players[i];
        p->drop_at_ms = i == dropper ? RACE_MS / 2 : 0;
        p->game = CreateThread(NULL, 0, game, p, 0, NULL);
    }
    for (i = 0; i < PLAYERS; i++) {
        WaitForSingleObject(players[i].game, 30000);
        CloseHandle(players[i].game);
    }
    for (i = 0; i < PLAYERS; i++) {
        Player *p = &players[i];
        int j;
        client_lobby(&p->client, &lobby);
        printf("  player %d: sent %d, got %d %d %d %d, ping %d ms%s\n", i, p->sent, p->got[0], p->got[1], p->got[2],
               p->got[3], lobby.ping_ms, lobby.offline ? ", offline" : "");
        if (i == dropper && rejoin) {
            /* back in its race, relaying both ways, with one gap per sender (what they sent while
               it was away never reached it) */
            CHECK(!lobby.offline && lobby.state == NET_RACING && !lobby.rejoining);
            CHECK(p->out_of_order <= PLAYERS - 1);
            CHECK(p->sent >= (RACE_MS - rejoin) / FRAME_MS - 4);
            for (j = 0; j < PLAYERS; j++)
                if (j != i)
                    CHECK(p->got[j] > 0 && p->got[j] < players[j].sent);
            continue;
        }
        CHECK(!p->out_of_order);
        if (i == dropper) {
            CHECK(lobby.offline && lobby.state == NET_FAILED);
            continue;
        }
        /* (a pong waits behind the frames already queued on the socket: up to the slowest one way) */
        CHECK(lobby.ping_ms >= latency[i] * 2 && lobby.ping_ms < latency[i] * 2 + 100);
        CHECK(p->sent >= RACE_MS / FRAME_MS - 2);
        for (j = 0; j < PLAYERS; j++)
            if (j != i)
                CHECK(p->got[j] == players[j].sent);  /* everything the others sent, the dropper's too */
        if (dropper >= 0 && rejoin)  /* away, then back: never left */
            CHECK(p->saw_away && !lobby.racer[dropper].left && !lobby.racers_away);
        else if (dropper >= 0)
            CHECK(lobby.racer[dropper].left);
    }
    for (i = 0; i < PLAYERS; i++) {
        Msg *m;
        players[i].stop = 1;
        WaitForSingleObject(players[i].thread, INFINITE);
        CloseHandle(players[i].thread);
        while ((m = players[i].head))
            players[i].head = m->next, free(m);
        client_free(&players[i].client);
    }
}

int main(void)
{
    static const int lan[PLAYERS] = {2, 3, 2, 4}, wan[PLAYERS] = {10, 25, 45, 70};
    lan_latency = lan;
    timeBeginPeriod(1);
    InitializeCriticalSection(&relay);
    run("four players, close", lan, -1, 0);
    run("four players, 10-70 ms away", wan, -1, 0);
    run("a player drops halfway", wan, 3, 0);
    run("a player drops halfway and rejoins its slot", wan, 2, 600);
    timeEndPeriod(1);
    if (failures) {
        printf("test_loopback: %d failures\n", failures);
        return 1;
    }
    printf("test_loopback: ok\n");
    return 0;
}
