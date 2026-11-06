/* One online player (client.h): docs/online-protocol.md's lobby messages, the race's relayed
   frames and pings. */
#include "client.h"

#include <math.h>
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

/* dhd_net routes STATE to state and EVENT to events on the room SFU association;
   only binary PING/PONG use the WebSocket. The fake transport tests exercise the same API. */
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

void client_set_install_id(NetClient *c, const char *id)
{
    c->install_id[0] = 0;
    if (id && strlen(id) == 32 && strspn(id, "0123456789abcdef") == 32)
        memcpy(c->install_id, id, 33);
}

void client_free(NetClient *c)
{
    SDL_DestroyCondition(c->changed);
    SDL_DestroyMutex(c->lock);
}

void client_connected(NetClient *c, const NetTransport *tr)
{
    char name[200], token[96] = "", install[64] = "";
    SDL_LockMutex(c->lock);
    c->tr = *tr;
    c->connected = 1;
    c->last_rx = c->next_ping = net_now_ms();
    json_escape(name, sizeof name, c->name);
    /* a rejoin: the room's token takes our member back, host rights included (finding 137) */
    if (c->rejoin_token[0])
        snprintf(token, sizeof token, ",\"rejoinToken\":\"%s\"", c->rejoin_token);
    /* installId: this copy of the game, for the server's bans (finding 161); no other player gets it */
    if (c->install_id[0])
        snprintf(install, sizeof install, ",\"installId\":\"%s\"", c->install_id);
    /* clientVersion: this release (port/include/version.h), for the server's version gate */
    /* features: this client shows the race room's countdown (the server runs one only when
       everyone's does) */
    say(c, "{\"type\":\"hello\",\"protocol\":%d,\"version\":\"%s\",\"clientVersion\":\"%s\",\"isoSha256\":\"%s\","
        "\"name\":\"%s\",\"features\":[\"countdown\",\"session\"]%s%s}", NET_PROTOCOL, c->version, DHD_VERSION, c->iso,
        name, install, token);
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
    l->phase[0] = l->last_event[0] = 0;
    json_string(t, "phase", l->phase, sizeof l->phase);
    json_string(t, "lastEvent", l->last_event, sizeof l->last_event);
#define TIME(key, field) l->field = json_number(t, key, &l->field) ? l->field : 0
    TIME("raceStartedAt", race_started_at); TIME("finishEndsAt", finish_ends_at);
    TIME("nextRaceAt", next_race_at); TIME("countdownEndsAt", countdown_ends_at);
    TIME("serverTime", server_time);
#undef TIME
    json_bool(t, "waitingForRider", &l->waiting_for_rider);
    json_bool(t, "rulesPending", &l->rules_pending);
    l->finish_window = json_int(t, "rules.finishWindow", &v) ? (int)v : 60;
    {
        char choice[16];
        json_string(t, "rules.nextRace", choice, sizeof choice);
        l->next_race = !strcmp(choice, "host");
        json_string(t, "rules.then", choice, sizeof choice);
        l->then = !strcmp(choice, "same") ? 0 : !strcmp(choice, "random") ? 2 : 1;
    }
    l->rotation_count = json_count(t, "rotation");
    if (l->rotation_count < 0) l->rotation_count = 0;
    if (l->rotation_count > 64) l->rotation_count = 64;
    for (i = 0; i < l->rotation_count; i++) {
        char path[32];
        snprintf(path, sizeof path, "rotation[%d]", i);
        l->rotation[i] = json_int(t, path, &v) ? (unsigned char)v : 0;
    }
    l->last_race_id = json_int(t, "lastRace.id", &v) ? (int)v : 0;
    l->last_race_track = json_int(t, "lastRace.track", &v) ? (int)v : 0;
    n = json_count(t, "lastRace.entrants");
    l->nresults = n < 0 ? 0 : n > NET_MAX_SLOTS ? NET_MAX_SLOTS : n;
    for (i = 0; i < l->nresults; i++) {
        NetResult *r = &l->result[i];
        char path[64];
        memset(r, 0, sizeof *r);
#define RESULT(key) (snprintf(path, sizeof path, "lastRace.entrants[%d]." key, i), path)
        r->id = json_int(t, RESULT("id"), &v) ? (int)v : 0;
        r->rider = json_int(t, RESULT("rider"), &v) ? (int)v : 0;
        r->place = json_int(t, RESULT("place"), &v) ? (int)v : 0;
        r->ticks = json_int(t, RESULT("ticks"), &v) ? v : -1;
        json_string(t, RESULT("name"), r->name, sizeof r->name);
        json_string(t, RESULT("dnfReason"), r->dnf_reason, sizeof r->dnf_reason);
#undef RESULT
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
        json_bool(t, FIELD("inRace"), &m->in_race);
        json_string(t, FIELD("raceState"), m->race_state, sizeof m->race_state);
        json_string(t, FIELD("dnfReason"), m->dnf_reason, sizeof m->dnf_reason);
        m->finish_ticks = json_int(t, FIELD("finishTicks"), &v) ? v : -1;
        m->place = json_int(t, FIELD("place"), &v) ? (int)v : 0;
        for (int k = 0; k < c->where_count; k++)
            if (c->where_id[k] == m->id) memcpy(m->region, c->where_region[k], sizeof m->region);
        if (m->id == l->member_id) c->ready = m->ready;

#undef FIELD
    }
    for (i = 0; i < l->nmembers; i++)
        if (l->member[i].id == l->member_id && l->phase[0] && !l->member[i].in_race) {
            c->racing = 0; l->state = NET_LOBBY; l->started = 0; c->relayed_head = c->relayed_count = 0;
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
    c->relayed_head = c->relayed_count = 0;
    memset(c->snapshot, 0, sizeof c->snapshot);
    memset(c->sender_go, 0, sizeof c->sender_go);
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
    if (c->window_n < NET_RTT_WINDOW) c->window[c->window_n++] = (float)rtt;
    if (!c->window_count++ || rtt > c->window_worst) c->window_worst = (float)rtt;
}

/* ---- dhd.log's net: lines (finding 135) ----
   One line on joining (where this PC and the room are), one when the room's location becomes
   known, one every NET_LOG_MS in a room or race and one on leaving: this PC's round-trips since
   the last line and every other player's as the server measures them. Nothing per frame. The lines
   are made with the lock held and printed after it is released (the log is a pipe). */

/* "last 30 s: ping 34 ms median, 51 worst (30 pings); players: ANNA (host, PL) 36 ms", and a new
   window from now. */
static void window_stats(NetClient *c, char *out, size_t cap)
{
    const NetLobby *l = &c->lobby;
    double now = net_now_ms();
    float sorted[NET_RTT_WINDOW];
    int n = snprintf(out, cap, "last %.0f s: ", (now - c->window_start) / 1000), others = 0;
    if (c->window_n) {
        int k = c->window_n;
        memcpy(sorted, c->window, k * sizeof sorted[0]);
        for (int i = 1; i < k; i++)
            for (int j = i; j > 0 && sorted[j] < sorted[j - 1]; j--) {
                float x = sorted[j];
                sorted[j] = sorted[j - 1], sorted[j - 1] = x;
            }
        n += snprintf(out + n, cap - n, "ping %d ms median, %d worst (%d ping%s)",
                      (int)((k % 2 ? sorted[k / 2] : (sorted[k / 2 - 1] + sorted[k / 2]) / 2) + 0.5f),
                      (int)(c->window_worst + 0.5f), c->window_count, c->window_count == 1 ? "" : "s");
    } else
        n += snprintf(out + n, cap - n, "no pings");
    for (int i = 0; i < l->nmembers && n < (int)cap - 64; i++) {
        const NetMember *m = &l->member[i];
        int host = m->id == l->host_id;
        char tag[16] = "", rtt[16] = "?";
        if (m->id == l->member_id) continue;
        if (host || m->region[0])
            snprintf(tag, sizeof tag, " (%s%s%s)", host ? "host" : "", host && m->region[0] ? ", " : "", m->region);
        if (m->rtt_ms >= 0) snprintf(rtt, sizeof rtt, "%d", m->rtt_ms);
        n += snprintf(out + n, cap - n, "%s %s%s %s ms", others++ ? "," : "; players:", m->name, tag, rtt);
        if (n >= (int)cap) n = (int)cap - 1;
    }
    if (!others && n < (int)cap - 16) n += snprintf(out + n, cap - n, "; no other players");
    for (int slot = 0; slot < NET_MAX_SLOTS && n < (int)cap - 100; slot++) {
        unsigned count = c->snapshot[slot].samples;
        if (!count) continue;
        unsigned sum = 0, q[3] = {0}, pc[3] = {50, 95, 99};
        for (unsigned ms = 0, k = 0; ms < 1002 && k < 3; ms++) {
            sum += c->snapshot[slot].age[ms];
            while (k < 3 && sum >= (count * pc[k] + 99) / 100) q[k++] = ms;
        }
        n += snprintf(out + n, cap - n, "; slot %d snapshot age p50/p95/p99 %u/%u/%u ms (1001=overflow), gaps>100ms %u",
                      slot, q[0], q[1], q[2], c->snapshot[slot].gaps);
        if (n >= (int)cap) n = (int)cap - 1;
        memset(c->snapshot[slot].age, 0, sizeof c->snapshot[slot].age);
        c->snapshot[slot].samples = c->snapshot[slot].gaps = 0;
    }
    c->window_n = c->window_count = 0;
    c->window_worst = 0;
    c->window_start = now;
}

/* "where the room runs" for the log: "FRA (EU)", "EU (location not known yet)" or "?" */
static void room_place(const NetLobby *l, char *out, size_t cap)
{
    if (l->room_colo[0]) snprintf(out, cap, "%s (%s)", l->room_colo, l->room_region[0] ? l->room_region : "region unknown");
    else if (l->room_region[0]) snprintf(out, cap, "%s (location not known yet)", l->room_region);
    else snprintf(out, cap, "an unknown location");
}

void client_on_text(NetClient *c, const char *text, size_t n)
{
    char type[24], line[1024] = "";
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
            c->clock_offset = server_time - (t + rtt / 2), c->have_offset = c->clock_known = 1;
        changed(c);
    } else if (!strcmp(type, "welcome")) {
        NetLobby *l = &c->lobby;
        int located;
        json_string(text, "code", c->lobby.code, sizeof c->lobby.code);
        if (json_int(text, "memberId", &v))
            c->lobby.member_id = (int)v;
        json_bool(text, "host", &c->lobby.is_host);
        if (c->lobby.is_host)
            c->lobby.host_id = c->lobby.member_id;
        {   /* kept for a rejoin's hello; only hex goes back into JSON */
            char token[sizeof c->rejoin_token] = "";
            if (json_string(text, "rejoinToken", token, sizeof token) && token[0] &&
                strspn(token, "0123456789abcdef") == strlen(token))
                memcpy(c->rejoin_token, token, sizeof token);
        }
        if (json_number(text, "serverTime", &server_time) && !c->have_offset)
            c->clock_offset = server_time - net_now_ms(), c->clock_known = 1;
        c->lobby.state = NET_LOBBY;
        c->lobby.error_code[0] = c->lobby.error[0] = 0;
        if (c->choosing)  /* after a reconnect: still on the rider screen */
            say(c, "{\"type\":\"pick\",\"choosing\":true}");
        else if (c->picked)  /* what we had chosen */
            say(c, "{\"type\":\"pick\",\"rider\":%d,\"bike\":%d}", c->rider, c->bike);
        if (c->ready)
            say(c, "{\"type\":\"ready\",\"ready\":true}");
        /* where this PC and the room are (finding 135; an older server sends none) */
        l->edge_colo[0] = l->country[0] = l->continent[0] = l->room_colo[0] = l->room_region[0] = 0;
        located = json_string(text, "edgeColo", l->edge_colo, sizeof l->edge_colo);
        json_string(text, "country", l->country, sizeof l->country);
        json_string(text, "continent", l->continent, sizeof l->continent);
        json_string(text, "roomColo", l->room_colo, sizeof l->room_colo);
        json_string(text, "roomRegion", l->room_region, sizeof l->room_region);
        {
            double now = net_now_ms();
            int n = snprintf(line, sizeof line, "net: %s room %s%s: ", c->joined_at ? "back in" : "in", l->code,
                             l->is_host ? " (host)" : "");
            if (!c->joined_at) c->joined_at = now;
            c->window_n = c->window_count = 0, c->window_worst = 0, c->window_start = now;
            c->next_log = now + NET_LOG_MS;
            if (!located)
                snprintf(line + n, sizeof line - n, "the server sent no locations (an older server)");
            else {
                char room[48];
                room_place(l, room, sizeof room);
                snprintf(line + n, sizeof line - n, "this PC reached Cloudflare %s%s%s%s%s%s; the room runs in %s",
                         l->edge_colo[0] ? l->edge_colo : "?", l->country[0] ? " from " : "", l->country,
                         l->continent[0] ? " (" : "", l->continent, l->continent[0] ? ")" : "", room);
            }
            snprintf(c->logged_colo, sizeof c->logged_colo, "%s", l->room_colo);
        }
        changed(c);
    } else if (!strcmp(type, "where")) {
        /* the room's location and every member's region, never their country (finding 135) */
        NetLobby *l = &c->lobby;
        char path[32];
        int n = json_count(text, "members");
        json_string(text, "region", l->room_region, sizeof l->room_region);
        json_string(text, "colo", l->room_colo, sizeof l->room_colo);
        c->where_count = 0;
        for (int i = 0; i < n && c->where_count < NET_MAX_SLOTS; i++) {
            int k = c->where_count;
            snprintf(path, sizeof path, "members[%d].id", i);
            if (!json_int(text, path, &v)) continue;
            c->where_id[k] = (int)v;
            snprintf(path, sizeof path, "members[%d].region", i);
            c->where_region[k][0] = 0;
            json_string(text, path, c->where_region[k], sizeof c->where_region[k]);
            c->where_count++;
        }
        for (int i = 0; i < l->nmembers; i++)
            for (int k = 0; k < c->where_count; k++)
                if (c->where_id[k] == l->member[i].id) memcpy(l->member[i].region, c->where_region[k], sizeof l->member[i].region);
        if (c->joined_at && l->room_colo[0] && strcmp(l->room_colo, c->logged_colo)) {
            char room[48];
            room_place(l, room, sizeof room);
            snprintf(line, sizeof line, "net: room %s runs in %s", l->code, room);
            snprintf(c->logged_colo, sizeof c->logged_colo, "%s", l->room_colo);
        }
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
        c->lobby.started = 0;
        c->lobby.state = NET_LOBBY;
        changed(c);
    } else if (!strcmp(type, "error")) {
        json_string(text, "code", c->lobby.error_code, sizeof c->lobby.error_code);
        json_string(text, "message", c->lobby.error, sizeof c->lobby.error);
        snprintf(line, sizeof line, "net: room %s: %s", c->lobby.code, c->lobby.error);
        changed(c);
    }
    SDL_UnlockMutex(c->lock);
    if (line[0]) fprintf(stderr, "%s\n", line);
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
        if (data[1] < NET_MAX_SLOTS) {
            int slot = data[1];
            if (data[0] == NET_FRAME_EVENT && n >= 12 && data[2] == 2 && data[3] == 5) {
                double go;
                memcpy(&go, data + 4, sizeof go);  /* existing EVENT_GO, server ms */
                if (isfinite(go) && go > 0) c->sender_go[slot] = go;
            }
            if (data[0] == NET_FRAME_STATE) {
                /* v4 standalone and v3's first batched rider (the WebSocket baseline). */
                int offset = n >= 8 && (data[2] == 4 || data[2] == 5) ? 4 : n >= 9 && data[3] == 3 ? 5 : 0;
                if (offset) {
                    uint32_t ms;
                    memcpy(&ms, data + offset, sizeof ms);
                    double now = net_now_ms();
                    double epoch = c->sender_go[slot] > 0 ? c->sender_go[slot] + ms : 0;
                    if (!c->snapshot[slot].arrived || !epoch || epoch > c->snapshot[slot].epoch) {
                        if (c->snapshot[slot].arrived && now - c->snapshot[slot].arrived > 100.5)
                            c->snapshot[slot].gaps++;
                        c->snapshot[slot].arrived = now;
                        c->snapshot[slot].epoch = epoch;
                    }
                }
            }
        }
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
    char line[4096] = "";
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
    if (c->joined_at && !c->leaving) {  /* a drop: the last window, and whether it comes back */
        int n = snprintf(line, sizeof line, "net: room %s: connection closed (%d)%s; ", c->lobby.code, code,
                         rejoin ? ", rejoining" : "");
        window_stats(c, line + n, sizeof line - n);
        if (!rejoin) c->joined_at = 0;
    }
    changed(c);
    SDL_UnlockMutex(c->lock);
    if (line[0]) fprintf(stderr, "%s\n", line);
    return rejoin;
}

int client_tick(NetClient *c)
{
    double now = net_now_ms();
    int dead = 0;
    char line[4096] = "";
    SDL_LockMutex(c->lock);
    if (c->connected) {
        if (c->racing) for (int slot = 0; slot < NET_MAX_SLOTS; slot++) {
            double epoch = c->snapshot[slot].epoch;
            if (!epoch) continue;
            double age = now + c->clock_offset - epoch;
            unsigned ms = age > 1000 ? 1001 : age < 0 ? 0 : (unsigned)age;
            c->snapshot[slot].age[ms]++;
            c->snapshot[slot].samples++;
        }
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
        if (c->joined_at && (c->lobby.state == NET_LOBBY || c->lobby.state == NET_RACING) && now >= c->next_log) {
            int n = snprintf(line, sizeof line, "net: room %s%s, ", c->lobby.code, c->racing ? " (racing)" : "");
            window_stats(c, line + n, sizeof line - n);
            c->next_log = now + NET_LOG_MS;
        }
    }
    SDL_UnlockMutex(c->lock);
    if (line[0]) fprintf(stderr, "%s\n", line);
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

void client_finish(NetClient *c, unsigned ticks, int place)
{
    SDL_LockMutex(c->lock);
    say(c, "{\"type\":\"finish\",\"ticks\":%u,\"place\":%d}", ticks, place);
    SDL_UnlockMutex(c->lock);
}

/* This PC left its race for the room: out of it at once, before the server's next snapshot says
   so, so the room never reads the race it left as one to join. */
void client_back(NetClient *c)
{
    SDL_LockMutex(c->lock);
    say(c, "{\"type\":\"back\"}");
    c->racing = 0;
    c->relayed_head = c->relayed_count = 0;
    c->lobby.started = 0;
    if (c->lobby.state == NET_RACING) c->lobby.state = NET_LOBBY;
    changed(c);
    SDL_UnlockMutex(c->lock);
}

void client_rules(NetClient *c, int finish_window, int next_race, int then)
{
    static const char *next[] = {"auto", "host"};
    static const char *tracks[] = {"same", "next", "random"};
    if ((finish_window != 0 && finish_window != 30 && finish_window != 60 && finish_window != 120) ||
        next_race < 0 || next_race > 1 || then < 0 || then > 2) return;
    SDL_LockMutex(c->lock);
    say(c, "{\"type\":\"settings\",\"finishWindow\":%d,\"nextRace\":\"%s\",\"then\":\"%s\"}",
        finish_window, next[next_race], tracks[then]);
    SDL_UnlockMutex(c->lock);
}

void client_rotation(NetClient *c, const unsigned char *ids, int count)
{
    char text[1024];
    int i, n, first;
    if (count < 0 || count > 64 || (count && !ids)) return;
    n = snprintf(text, sizeof text, "{\"type\":\"settings\",\"rotation\":[");
    for (i = 0; i < count; i++) n += snprintf(text + n, sizeof text - n, "%s%u", i ? "," : "", ids[i] & 31);
    n += snprintf(text + n, sizeof text - n, "],\"trackLimits\":[");
    for (i = 0; i < count; i++) n += snprintf(text + n, sizeof text - n, "%s%u", i ? "," : "", 10 - 2 * (ids[i] >> 5 & 3));
    /* bit 7: a track the host can pick that NEXT / RANDOM never choose (the server's pickOnly) */
    n += snprintf(text + n, sizeof text - n, "],\"pickOnly\":[");
    for (i = 0, first = 1; i < count; i++)
        if (ids[i] & 0x80) n += snprintf(text + n, sizeof text - n, "%s%u", first ? "" : ",", ids[i] & 31), first = 0;
    snprintf(text + n, sizeof text - n, "]}");
    SDL_LockMutex(c->lock);
    say(c, "%s", text);
    SDL_UnlockMutex(c->lock);
}

void client_leave(NetClient *c)
{
    char line[4096] = "";
    SDL_LockMutex(c->lock);
    c->leaving = 1;
    if (c->racing)
        say(c, "{\"type\":\"leave\"}");
    if (c->joined_at) {
        int s = (int)((net_now_ms() - c->joined_at) / 1000);
        int n = snprintf(line, sizeof line, "net: left room %s after %d:%02d; ", c->lobby.code, s / 60, s % 60);
        window_stats(c, line + n, sizeof line - n);
        c->joined_at = 0;
    }
    SDL_UnlockMutex(c->lock);
    if (line[0]) fprintf(stderr, "%s\n", line);
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

int client_read_lobbies(NetClient *c, const char *body, double sent, double got, NetLobbyInfo *out, int max)
{
    char path[48];
    double now;
    int i, n = json_count(body, "lobbies"), timed;
    if (n < 0)
        return -1;
    if (n > max)
        n = max;
    /* The list's times are the server's clock. In no room yet the offset to it is unknown (0.7.6
       counted from the epoch: ENDS IN 29893031:32); the answer's "now" gives it, half the fetch's
       round-trip late at most. A room's pings measure it better and are kept. */
    SDL_LockMutex(c->lock);
    if (json_number(body, "now", &now) && now > 0 && !c->have_offset)
        c->clock_offset = now - (sent + got) / 2, c->clock_known = 1;
    timed = c->clock_known;
    SDL_UnlockMutex(c->lock);
    for (i = 0; i < n; i++) {
        NetLobbyInfo *l = &out[i];
        long long v;
        memset(l, 0, sizeof *l);
#define FIELD(key) (snprintf(path, sizeof path, "lobbies[%d]." key, i), path)
        json_string(body, FIELD("code"), l->code, sizeof l->code);
        json_string(body, FIELD("roomName"), l->room_name, sizeof l->room_name);
        json_string(body, FIELD("version"), l->version, sizeof l->version);
        json_string(body, FIELD("hostName"), l->host_name, sizeof l->host_name);
        l->track = json_int(body, FIELD("track"), &v) ? (int)v : 0;
        l->players = json_int(body, FIELD("players"), &v) ? (int)v : 0;
        l->max_players = json_int(body, FIELD("maxPlayers"), &v) ? (int)v : 0;
        json_string(body, FIELD("phase"), l->phase, sizeof l->phase);
        json_bool(body, FIELD("full"), &l->full);
        json_number(body, FIELD("raceStartedAt"), &l->race_started_at);
        json_number(body, FIELD("finishEndsAt"), &l->finish_ends_at);
        json_number(body, FIELD("nextRaceAt"), &l->next_race_at);
        json_string(body, FIELD("region"), l->region, sizeof l->region);  /* finding 135 */
        json_string(body, FIELD("colo"), l->colo, sizeof l->colo);
        l->timed = timed;
#undef FIELD
    }
    return n;
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
