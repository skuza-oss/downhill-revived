/* One online player (client.h): docs/online-protocol.md's lobby messages, the race's relayed
   frames and pings. */
#include "client.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "../include/version.h"
#include "../host/host_api.h"
#include <SDL3/SDL_timer.h>

enum { PING_LOBBY_MS = 1000, PING_RACE_MS = 500, SILENT_MS = 10000 };

double net_now_ms(void) { return (double)SDL_GetTicksNS() / 1e6; }

static void copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void changed(NetClient *c)
{
    c->lobby.seq++;
    SDL_BroadcastCondition(c->changed);
}

/* Sends with the lock held: the transports only queue. */
static void say(NetClient *c, const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (c->connected)
        c->tr.send_text(c->tr.ctx, text);
}

static void send_binary(NetClient *c, const uint8_t *data, size_t n)
{
    if (c->connected && n)
        c->tr.send_binary(c->tr.ctx, data, n);
}

void client_init(NetClient *c, const char *name, const char *version, const char *iso)
{
    memset(c, 0, sizeof *c);
    c->lock = SDL_CreateMutex();
    c->changed = SDL_CreateCondition();
    copy(c->name, sizeof c->name, name);
    {   /* the server takes 1-24 UTF-16 units */
        int units = 0;
        char *p;
        for (p = c->name; *p; p++)
            if ((*p & 0xC0) != 0x80 && (units += (*p & 0xF8) == 0xF0 ? 2 : 1) > 24)
                break;
        *p = 0;
    }
    copy(c->version, sizeof c->version, version);
    copy(c->iso, sizeof c->iso, iso);
    c->lobby.seed = -1;
    c->lobby.ping_ms = -1;
}

void client_free(NetClient *c)
{
    SDL_DestroyCondition(c->changed);
    SDL_DestroyMutex(c->lock);
}

void client_connected(NetClient *c, const NetTransport *tr)
{
    char name[200];
    SDL_LockMutex(c->lock);
    c->tr = *tr;
    c->connected = 1;
    c->last_rx = c->next_ping = net_now_ms();
    json_escape(name, sizeof name, c->name);
    /* clientVersion: this release (port/include/version.h), for the server's version gate */
    /* features: this client shows the race room's countdown (the server runs one only when
       everyone's does) */
    say(c, "{\"type\":\"hello\",\"protocol\":%d,\"version\":\"%s\",\"clientVersion\":\"%s\",\"isoSha256\":\"%s\","
        "\"name\":\"%s\",\"features\":[\"countdown\"]}", NET_PROTOCOL, c->version, DHD_VERSION, c->iso, name);
    if (c->lobby.state != NET_RECONNECTING)
        c->lobby.state = NET_CONNECTING;
    changed(c);
    SDL_UnlockMutex(c->lock);
}

static void read_room(NetClient *c, const char *t)
{
    NetLobby *l = &c->lobby;
    long long v;
    int i, n;
    json_string(t, "code", l->code, sizeof l->code);
    if (json_int(t, "hostId", &v))
        l->host_id = (int)v;
    l->is_host = l->host_id == l->member_id;
    json_string(t, "roomName", l->room_name, sizeof l->room_name);
    json_bool(t, "public", &l->is_public);
    json_bool(t, "started", &l->started);
    if (json_int(t, "maxPlayers", &v))
        l->max_players = (int)v;
    if (json_int(t, "settings.track", &v))
        l->track = (int)v;
    if (json_int(t, "settings.laps", &v))
        l->laps = (int)v;
    l->seed = json_int(t, "settings.seed", &v) ? v : -1;
    {
        double ends;
        c->countdown_ends = json_number(t, "countdownEndsAt", &ends) ? ends : 0;
    }
    n = json_count(t, "members");
    l->nmembers = n < 0 ? 0 : n > NET_MAX_SLOTS ? NET_MAX_SLOTS : n;
    for (i = 0; i < l->nmembers; i++) {
        NetMember *m = &l->member[i];
        char path[32];
        memset(m, 0, sizeof *m);
        m->rtt_ms = -1;
#define FIELD(name) (snprintf(path, sizeof path, "members[%d]." name, i), path)
        if (json_int(t, FIELD("id"), &v))
            m->id = (int)v;
        json_string(t, FIELD("name"), m->name, sizeof m->name);
        json_bool(t, FIELD("ready"), &m->ready);
        if (json_int(t, FIELD("rider"), &v))
            m->rider = (int)v;
        if (json_int(t, FIELD("bike"), &v))
            m->bike = (int)v;
        if (json_int(t, FIELD("rttMs"), &v))
            m->rtt_ms = (int)v;
        json_bool(t, FIELD("choosing"), &m->choosing);
#undef FIELD
    }
}

static void read_start(NetClient *c, const char *t)
{
    NetLobby *l = &c->lobby;
    long long v;
    double server_time, start_at;
    int i, n, nslots = 0;
    l->local_slot = json_int(t, "yourSlot", &v) ? (int)v : 0;
    l->race_track = json_int(t, "track", &v) ? (int)v : 0;
    l->race_laps = json_int(t, "laps", &v) ? (int)v : 0;
    l->race_seed = json_int(t, "seed", &v) ? (uint32_t)v : 0;
    memset(l->racer, 0, sizeof l->racer);
    n = json_count(t, "players");
    for (i = 0; i < n; i++) {
        char path[32];
        int slot;
        snprintf(path, sizeof path, "players[%d].slot", i);
        if (!json_int(t, path, &v) || v < 0 || v >= NET_MAX_SLOTS)
            continue;
        slot = (int)v;
        l->racer[slot].slot = slot;
        snprintf(path, sizeof path, "players[%d].memberId", i);
        if (json_int(t, path, &v))
            l->racer[slot].member_id = (int)v;
        snprintf(path, sizeof path, "players[%d].name", i);
        json_string(t, path, l->racer[slot].name, sizeof l->racer[slot].name);
        snprintf(path, sizeof path, "players[%d].rider", i);
        if (json_int(t, path, &v))
            l->racer[slot].rider = (int)v;
        snprintf(path, sizeof path, "players[%d].bike", i);
        if (json_int(t, path, &v))
            l->racer[slot].bike = (int)v;
        if (slot + 1 > nslots)
            nslots = slot + 1;
    }
    l->nslots = nslots;
    /* the start's time on our clock */
    if (json_number(t, "startAt", &start_at)) {
        if (!c->have_offset && json_number(t, "serverTime", &server_time))
            c->clock_offset = server_time - net_now_ms();
        l->start_at = (start_at - c->clock_offset) / 1000.0;
    } else
        l->start_at = net_now_ms() / 1000.0;
    c->racing = 1;
    l->offline = 0;
    l->started = 1;
    l->state = NET_RACING;
}

static void add_rtt(NetClient *c, double rtt)
{
    float sorted[5];
    int i, j, n;
    if (rtt < 0 || rtt > 60000)
        return;
    memmove(c->rtt + 1, c->rtt, sizeof c->rtt - sizeof c->rtt[0]);
    c->rtt[0] = (float)rtt;
    if (c->nrtt < 5)
        c->nrtt++;
    n = c->nrtt;
    memcpy(sorted, c->rtt, sizeof sorted);
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && sorted[j] < sorted[j - 1]; j--) {
            float x = sorted[j];
            sorted[j] = sorted[j - 1], sorted[j - 1] = x;
        }
    c->lobby.ping_ms = (int)(sorted[n / 2] + 0.5f);
}

/* A line for the chat ring: the oldest goes when it is full. */
static void chat_add(NetClient *c, int member_id, const char *name, const char *text, double at)
{
    NetChat *ch = &c->chat;
    NetChatLine *line;
    if (ch->count == NET_CHAT_LINES)
        memmove(ch->line, ch->line + 1, sizeof ch->line[0] * (NET_CHAT_LINES - 1));
    else
        ch->count++;
    line = &ch->line[ch->count - 1];
    memset(line, 0, sizeof *line);
    line->seq = ++ch->seq;
    line->member_id = member_id;
    copy(line->name, sizeof line->name, name);
    copy(line->text, sizeof line->text, text);
    line->at = at;
}

static void read_chat(NetClient *c, const char *t)
{
    char name[NET_CHAT_NAME], text[NET_CHAT_TEXT];
    long long v;
    double at;
    name[0] = 0;
    if (!json_string(t, "text", text, sizeof text) || !text[0])
        return;
    json_string(t, "name", name, sizeof name);
    if (!json_number(t, "at", &at))
        at = net_now_ms() + c->clock_offset;
    chat_add(c, json_int(t, "memberId", &v) ? (int)v : -1, name, text, at);
}

void client_on_text(NetClient *c, const char *text, size_t n)
{
    char type[24];
    long long v;
    double t, server_time;
    (void)n;
    SDL_LockMutex(c->lock);
    c->last_rx = net_now_ms();
    if (!json_string(text, "type", type, sizeof type))
        ;
    else if (!strcmp(type, "ping") && json_int(text, "s", &v))
        say(c, "{\"type\":\"pong\",\"s\":%lld}", v);
    else if (!strcmp(type, "pong") && json_number(text, "t", &t)) {
        double now = net_now_ms(), rtt = now - t;
        add_rtt(c, rtt);
        if (json_number(text, "serverTime", &server_time) &&
            (!c->have_offset || rtt <= c->lobby.ping_ms))
            c->clock_offset = server_time - (t + rtt / 2), c->have_offset = 1;
        changed(c);
    } else if (!strcmp(type, "welcome")) {
        json_string(text, "code", c->lobby.code, sizeof c->lobby.code);
        if (json_int(text, "memberId", &v))
            c->lobby.member_id = (int)v;
        json_bool(text, "host", &c->lobby.is_host);
        if (c->lobby.is_host)
            c->lobby.host_id = c->lobby.member_id;
        if (json_number(text, "serverTime", &server_time) && !c->have_offset)
            c->clock_offset = server_time - net_now_ms();
        c->lobby.state = NET_LOBBY;
        c->lobby.error_code[0] = c->lobby.error[0] = 0;
        if (c->choosing)  /* after a reconnect: still on the rider screen */
            say(c, "{\"type\":\"pick\",\"choosing\":true}");
        else if (c->picked)  /* what we had chosen */
            say(c, "{\"type\":\"pick\",\"rider\":%d,\"bike\":%d}", c->rider, c->bike);
        if (c->ready)
            say(c, "{\"type\":\"ready\",\"ready\":true}");
        changed(c);
    } else if (!strcmp(type, "room")) {
        read_room(c, text);
        changed(c);
    } else if (!strcmp(type, "start")) {
        c->countdown_ends = 0;
        read_start(c, text);
        changed(c);
    } else if (!strcmp(type, "left") && json_int(text, "slot", &v) && v >= 0 && v < NET_MAX_SLOTS) {
        c->lobby.racer[v].left = 1;
        changed(c);
    } else if (!strcmp(type, "ended")) {
        c->racing = 0;
        c->ready = 0;
        c->lobby.started = 0;
        c->lobby.state = NET_LOBBY;
        changed(c);
    } else if (!strcmp(type, "chat")) {
        read_chat(c, text);
        changed(c);
    } else if (!strcmp(type, "error")) {
        json_string(text, "code", c->lobby.error_code, sizeof c->lobby.error_code);
        json_string(text, "message", c->lobby.error, sizeof c->lobby.error);
        if (!strcmp(c->lobby.error_code, "slow_down"))  /* a chat line too many: say so in the chat */
            chat_add(c, -1, "", c->lobby.error[0] ? c->lobby.error : "Too many messages: wait a moment.",
                     net_now_ms() + c->clock_offset);
        changed(c);
    }
    SDL_UnlockMutex(c->lock);
}

void client_on_binary(NetClient *c, const uint8_t *data, size_t n)
{
    SDL_LockMutex(c->lock);
    c->last_rx = net_now_ms();
    if (n >= 2 + sizeof(double) && data[0] == NET_FRAME_PONG) {
        double sent;
        memcpy(&sent, data + 2, sizeof sent);
        add_rtt(c, net_now_ms() - sent);
    } else if (c->racing && n >= 2 && n <= NET_FRAME_MAX && (data[0] == NET_FRAME_STATE || data[0] == NET_FRAME_EVENT)) {
        unsigned at = (c->relayed_head + c->relayed_count) % NET_RELAY_QUEUE;
        if (c->relayed_count == NET_RELAY_QUEUE)
            c->relayed_head = (c->relayed_head + 1) % NET_RELAY_QUEUE;
        else
            c->relayed_count++;
        memcpy(c->relayed[at], data, n);
        c->relayed_size[at] = (uint16_t)n;
    }
    SDL_UnlockMutex(c->lock);
}

int client_on_closed(NetClient *c, int code, const char *reason)
{
    int rejoin = 0;
    SDL_LockMutex(c->lock);
    c->connected = 0;
    if (c->racing) {
        /* No reconnect during a race (protocol 1): the others hand our rider to the host's AI,
           and here the race goes on without them (online.c). */
        c->lobby.offline = 1;
        c->lobby.state = NET_FAILED;
    } else if (c->leaving)
        c->lobby.state = NET_IDLE;
    else if (code >= 4000 && code < 4100)  /* refused */
        c->lobby.state = NET_FAILED;
    else if (c->lobby.code[0] && c->lobby.state != NET_CONNECTING) {
        c->lobby.state = NET_RECONNECTING;
        rejoin = 1;
    } else
        c->lobby.state = NET_FAILED;
    if (!c->lobby.error[0] && c->lobby.state == NET_FAILED)
        snprintf(c->lobby.error, sizeof c->lobby.error, "connection closed (%d%s%s)", code,
                 reason && *reason ? ": " : "", reason ? reason : "");
    changed(c);
    SDL_UnlockMutex(c->lock);
    return rejoin;
}

int client_tick(NetClient *c)
{
    double now = net_now_ms();
    int dead = 0;
    SDL_LockMutex(c->lock);
    if (c->connected) {
        if (now >= c->next_ping) {
            if (c->racing) {
                uint8_t ping[2 + sizeof(double)] = {NET_FRAME_PING, (uint8_t)c->lobby.local_slot};
                memcpy(ping + 2, &now, sizeof now);
                send_binary(c, ping, sizeof ping);
                c->next_ping = now + PING_RACE_MS;
            } else {
                if (c->lobby.state == NET_LOBBY)
                    say(c, "{\"type\":\"ping\",\"t\":%.3f}", now);
                c->next_ping = now + PING_LOBBY_MS;
            }
        }
        dead = now - c->last_rx > SILENT_MS;
    }
    SDL_UnlockMutex(c->lock);
    return dead;
}

void client_set_room(NetClient *c, const NetRoomSettings *s)
{
    char text[512], name[200];
    int n = snprintf(text, sizeof text, "{\"type\":\"settings\"");
    if (s->track >= 0)
        n += snprintf(text + n, sizeof text - n, ",\"track\":%d", s->track);
    if (s->laps >= 0)
        n += snprintf(text + n, sizeof text - n, ",\"laps\":%d", s->laps);
    if (s->seed >= 0)
        n += snprintf(text + n, sizeof text - n, ",\"seed\":%lld", s->seed);
    if (s->is_public >= 0)
        n += snprintf(text + n, sizeof text - n, ",\"public\":%s", s->is_public ? "true" : "false");
    if (s->max_players >= 0)
        n += snprintf(text + n, sizeof text - n, ",\"maxPlayers\":%d", s->max_players);
    if (s->room_name) {
        json_escape(name, sizeof name, s->room_name);
        n += snprintf(text + n, sizeof text - n, ",\"roomName\":\"%s\"", name);
    }
    snprintf(text + n, sizeof text - n, "}");
    SDL_LockMutex(c->lock);
    say(c, "%s", text);
    SDL_UnlockMutex(c->lock);
}

void client_pick(NetClient *c, int member, int rider, int bike)
{
    SDL_LockMutex(c->lock);
    if ((member < 0 || member == c->lobby.member_id) && rider < 0) {
        c->choosing = 1, c->ready = 0;
        say(c, "{\"type\":\"pick\",\"choosing\":true}");
    } else if (member < 0 || member == c->lobby.member_id) {
        c->picked = 1, c->rider = rider, c->bike = bike, c->choosing = 0;
        say(c, "{\"type\":\"pick\",\"rider\":%d,\"bike\":%d,\"choosing\":false}", rider, bike);
    } else
        say(c, "{\"type\":\"pick\",\"member\":%d,\"rider\":%d,\"bike\":%d}", member, rider, bike);
    SDL_UnlockMutex(c->lock);
}

void client_ready(NetClient *c, int ready)
{
    SDL_LockMutex(c->lock);
    c->ready = ready != 0;
    say(c, "{\"type\":\"ready\",\"ready\":%s}", ready ? "true" : "false");
    SDL_UnlockMutex(c->lock);
}

void client_kick(NetClient *c, int member)
{
    SDL_LockMutex(c->lock);
    say(c, "{\"type\":\"kick\",\"member\":%d}", member);
    SDL_UnlockMutex(c->lock);
}

void client_start(NetClient *c)
{
    SDL_LockMutex(c->lock);
    say(c, "{\"type\":\"start\"}");
    SDL_UnlockMutex(c->lock);
}

void client_end(NetClient *c)
{
    SDL_LockMutex(c->lock);
    say(c, "{\"type\":\"end\"}");
    SDL_UnlockMutex(c->lock);
}

void client_leave(NetClient *c)
{
    SDL_LockMutex(c->lock);
    c->leaving = 1;
    if (c->racing)
        say(c, "{\"type\":\"leave\"}");
    SDL_UnlockMutex(c->lock);
}

void client_lobby(NetClient *c, NetLobby *out)
{
    SDL_LockMutex(c->lock);
    *out = c->lobby;
    out->countdown_ms = -1;
    if (c->countdown_ends > 0 && out->state == NET_LOBBY) {
        double left = c->countdown_ends - (net_now_ms() + c->clock_offset);
        out->countdown_ms = left > 0 ? (int)left : 0;
    }
    SDL_UnlockMutex(c->lock);
}

int client_chat(NetClient *c, const char *text)
{
    char line[NET_CHAT_MAX + 1], escaped[6 * NET_CHAT_MAX + 1];
    size_t n = 0, i;
    int sent = 0;
    /* what the server would make of it: control characters as spaces, trimmed; at most
       NET_CHAT_MAX bytes, not cutting a UTF-8 character */
    while (*text == ' ' || (*text > 0 && *text < 0x20))
        text++;
    for (i = 0; text[i] && n < NET_CHAT_MAX; i++)
        line[n++] = (unsigned char)text[i] < 0x20 || text[i] == 0x7F ? ' ' : text[i];
    if ((text[i] & 0xC0) == 0x80) {  /* cut inside a character: drop the part that fitted */
        while (n && (line[n - 1] & 0xC0) == 0x80)
            n--;
        if (n)
            n--;
    }
    while (n && line[n - 1] == ' ')
        n--;
    line[n] = 0;
    if (!n)
        return 0;
    json_escape(escaped, sizeof escaped, line);
    SDL_LockMutex(c->lock);
    if (c->connected) {
        say(c, "{\"type\":\"chat\",\"text\":\"%s\"}", escaped);
        sent = 1;
    }
    SDL_UnlockMutex(c->lock);
    return sent;
}

void client_get_chat(NetClient *c, NetChat *out)
{
    SDL_LockMutex(c->lock);
    *out = c->chat;
    SDL_UnlockMutex(c->lock);
}

int client_send_frame(NetClient *c, int type, const void *payload, size_t n)
{
    uint8_t frame[NET_FRAME_MAX];
    int sent = 0;
    if (n > sizeof frame - 2) return 0;
    frame[0] = (uint8_t)type;
    frame[1] = 0;
    memcpy(frame + 2, payload, n);
    SDL_LockMutex(c->lock);
    if (c->racing && c->connected) {
        send_binary(c, frame, n + 2);
        sent = 1;
    }
    SDL_UnlockMutex(c->lock);
    return sent;
}

int client_receive_frame(NetClient *c, uint8_t *out, size_t max)
{
    int n = 0;
    SDL_LockMutex(c->lock);
    if (c->relayed_count) {
        unsigned at = c->relayed_head;
        n = c->relayed_size[at] <= max ? c->relayed_size[at] : 0;
        memcpy(out, c->relayed[at], n);
        c->relayed_head = (at + 1) % NET_RELAY_QUEUE;
        c->relayed_count--;
    }
    SDL_UnlockMutex(c->lock);
    return n;
}
