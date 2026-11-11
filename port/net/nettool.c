/* A command-line player for testing rooms and races: it joins a room through net.h as the game does,
   and in the race replays a recorded rider (--replay, port/tools/online_race.py's bots) or just
   stays in it for --seconds, counting the others' frames; then it prints a RESULT line and leaves.

   usage: nettool [--server URL] [--name NAME] [--seconds S] [--version V --iso SHA256]
                  [--replay FILE --rider N] [--bike A,B,C] [--pick N] [--ready-after S] [--races N] [--echo-ai] [--finish-after S | --give-up-after S]
                  host [--players N] [--code-file PATH] [--public | --p2p] | join CODE | list
   --p2p makes a P2P room (finding 180); a join goes to a P2P room when its code has 7 characters.
   The server is --server or DHD_NET_SERVER (ws://127.0.0.1:8787 for wrangler dev). With the
   game's --version and --iso hash it can join a room of real games (finding 041). */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "../hle/state_bundle.h"

static void wait_for(int (*done)(const NetLobby *), double seconds, const char *what)
{
    double until = net_seconds() + seconds;
    NetLobby l;
    for (;;) {
        net_get_lobby(&l);
        if (done(&l))
            return;
        if (l.state == NET_FAILED || net_seconds() > until) {
            printf("FAILED waiting for %s: state %d, %s %s\n", what, l.state, l.error_code, l.error);
            exit(1);
        }
        Sleep(20);
    }
}

static int players_wanted = 4;
static int in_lobby(const NetLobby *l) { return l->state == NET_LOBBY && l->code[0]; }
static int all_ready(const NetLobby *l)
{
    int i, ready = 0;
    for (i = 0; i < l->nmembers; i++)
        ready += l->member[i].ready;
    return l->nmembers >= players_wanted && ready == l->nmembers;
}
static int racing(const NetLobby *l) { return l->state == NET_RACING; }

/* --replay FILE: a player of a state-sync race (port/hle/online.c, finding 047)
   without a game. It says it has loaded and what it rides, takes the race's GO from the first
   game's GO event, then sends the snapshots a game recorded (DHD_NET_RECORD: bundles of the game's
   own rider alone, port/hle/state_bundle.h) as its own rider, stamped now, when the race time
   reaches each one's (ms, finding 114), and its finish when they run out. */
enum { EVENT_VERSION = 2, EVENT_LOADED = 2, EVENT_SETUP = 3, EVENT_START = 4, EVENT_GO = 5, EVENT_FINISH = 6, EVENT_RESULTS = 7 };

static void put32(uint8_t *o, int32_t v) { memcpy(o, &v, 4); }

/* One rider's record from a received STATE bundle sent again as this player's own rider (a bundle of
   one, stamped now), moved dx along x: 1 when sent. */
static int resend_rider(const uint8_t *in, int n, int rider, int as, float dx)
{
    double stamp;
    if (!state_bundle_check(in, n, &stamp)) return 0;
    for (int at = STATE_BUNDLE_HEADER; at < n;) {
        int size = state_record_size(in + at, n - at);
        if (in[at] == rider) {
            uint8_t out[STATE_BUNDLE_MAX];
            float x;
            memcpy(out, in, STATE_BUNDLE_HEADER);
            memcpy(out + STATE_BUNDLE_HEADER, in + at, size);
            out[1] = 0, out[2] = 1, out[STATE_BUNDLE_HEADER] = (uint8_t)as;
            stamp = net_server_ms();
            memcpy(out + 3, &stamp, 8);
            memcpy(&x, out + STATE_BUNDLE_HEADER + 5, 4);
            x += dx;
            memcpy(out + STATE_BUNDLE_HEADER + 5, &x, 4);
            return net_send_frame(NET_FRAME_STATE, out, STATE_BUNDLE_HEADER + size) != 0;
        }
        at += size;
    }
    return 0;
}

/* A SETUP event's rider and bike (the 30-byte form: no colour): --bike A,B,C picks the bike's three
   words, as a shop's choice (default 8 3 0). */
static int bike_words[3] = {8, 3, 0};
static void put_ride(uint8_t *e, int rider)
{
    put32(e + 2, 0x20 * (rider + 1)), put32(e + 6, rider);
    for (int k = 0; k < 3; k++) put32(e + 18 + 4 * k, bike_words[k]);
}

static int replay(const char *path, int slot, int rider, double seconds)
{
    static uint8_t data[1 << 22];
    FILE *fp = fopen(path, "rb");
    size_t size = fp ? fread(data, 1, sizeof data, fp) : 0, at = 0;
    uint8_t e[30] = {EVENT_VERSION, EVENT_SETUP}, frame[NET_FRAME_MAX];
    double go = 0, until = net_seconds() + seconds;
    int sent = 0, last_clock = 0, results = 0, n;
    if (fp) fclose(fp);
    if (!size) {
        printf("FAILED: no recording in %s\n", path);
        return 1;
    }
    e[1] = EVENT_LOADED;
    net_send_frame(NET_FRAME_EVENT, e, 2);
    e[1] = EVENT_SETUP;
    put_ride(e, rider);
    net_send_frame(NET_FRAME_EVENT, e, sizeof e);
    while (net_seconds() < until && !results) {
        while ((n = net_receive_frame(frame, sizeof frame)) > 0)
            if (frame[0] == NET_FRAME_EVENT && n >= 4 && frame[2] == EVENT_VERSION) {
                if (frame[3] == EVENT_GO && n >= 12 && !go) {
                    memcpy(&go, frame + 4, 8);
                    printf("GO from player %d\n", frame[1]);
                } else if (frame[3] == EVENT_RESULTS) results = 1;
            }
        double clock = go ? net_server_ms() - go : -1;  /* race time, ms */
        while (go && at + 2 <= size) {
            uint16_t len;
            int32_t when;
            memcpy(&len, data + at, 2);
            uint8_t *bundle = data + at + 2;
            double stamp;
            if (at + 2 + len > size || !state_bundle_check(bundle, len, &stamp) || bundle[2] != 1) { at = size; break; }
            memcpy(&when, bundle + STATE_BUNDLE_HEADER + 1, 4);
            if (when > clock) break;
            bundle[STATE_BUNDLE_HEADER] = (uint8_t)slot;  /* the rider is this player's */
            stamp = net_server_ms();
            memcpy(bundle + 3, &stamp, 8);
            net_send_frame(NET_FRAME_STATE, bundle, len);
            last_clock = when, sent++, at += 2 + len;
        }
        if (go && at + 2 > size && last_clock) {
            uint8_t f[7] = {EVENT_VERSION, EVENT_FINISH, (uint8_t)slot};
            put32(f + 3, (int32_t)(last_clock * 0.06));  /* race ticks (1/60 s fields) */
            net_send_frame(NET_FRAME_EVENT, f, sizeof f);
            printf("FINISH at race time %d ms after %d snapshots\n", last_clock, sent);
            last_clock = 0;
        }
        Sleep(5);
    }
    printf("RESULT {\"slot\":%d,\"snapshots\":%d,\"results\":%d}\n", slot, sent, results);
    return 0;
}

int main(int argc, char **argv)
{
    const char *server = NULL, *name = "Tester", *mode = NULL, *code = NULL, *code_file = NULL;
    const char *version = "dhd-port nettool 1", *iso = "0000000000000000000000000000000000000000000000000000000000000000";
    const char *replay_file = NULL;
    int follow_host = 0, followed = 0; /* synthetic moving opponents for room checks */
    int rider = 4, seconds = 600, received = 0;
    int echo_ai = 0;         /* --echo-ai: session bot mirrors a host AI as a real puppet */
    int pick = -1;            /* --pick N: the lobby's rider number (1..16); 0 stays choosing */
    double ready_after = 0;   /* --ready-after S: Ready after S seconds; below 0 never */
    int i, slot, races = 0, public_room = 0;  /* --public: the host's room is on the public list */
    int p2p = 0;              /* --p2p: the host's room is a P2P one */
    double finish_after = -1, back_after = -1;
    uint8_t frame[NET_FRAME_MAX];
    double until;
    NetLobby l;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--server") && i + 1 < argc) server = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--players") && i + 1 < argc) players_wanted = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--code-file") && i + 1 < argc) code_file = argv[++i];
        else if (!strcmp(argv[i], "--version") && i + 1 < argc) version = argv[++i];
        else if (!strcmp(argv[i], "--iso") && i + 1 < argc) iso = argv[++i];
        else if (!strcmp(argv[i], "--follow-host")) follow_host = 1;
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay_file = argv[++i];
        else if (!strcmp(argv[i], "--rider") && i + 1 < argc) rider = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bike") && i + 1 < argc)
            sscanf(argv[++i], "%d,%d,%d", &bike_words[0], &bike_words[1], &bike_words[2]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pick") && i + 1 < argc) pick = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ready-after") && i + 1 < argc) ready_after = atof(argv[++i]);
        else if (!strcmp(argv[i], "--races") && i + 1 < argc) races = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--echo-ai")) echo_ai = 1;
        else if (!strcmp(argv[i], "--public")) public_room = 1;
        else if (!strcmp(argv[i], "--p2p")) p2p = 1;
        else if (!strcmp(argv[i], "--finish-after") && i + 1 < argc) finish_after = atof(argv[++i]);
        else if (!strcmp(argv[i], "--give-up-after") && i + 1 < argc) back_after = atof(argv[++i]);
        else if (!mode) mode = argv[i];
        else code = argv[i];
    }
    if (!mode || (!strcmp(mode, "join") && !code)) {
        printf("usage: nettool [--server URL] [--name NAME] [--seconds S] [--version V --iso SHA256]\n"
               "               [--replay FILE --rider N] [--bike A,B,C] [--pick N] [--ready-after S] [--races N] [--echo-ai] [--finish-after S | --give-up-after S] host [--players N] [--code-file PATH] [--public | --p2p] | join CODE | list\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    timeBeginPeriod(1);
    net_init(server, name, version, iso);
    if (!strcmp(mode, "list")) {
        NetLobbyInfo info[32];
        int n = net_list_lobbies(info, 32);
        printf("%d public lobbies\n", n);
        for (i = 0; i < n; i++)
            printf("  %s  %s  %d/%d  %s  %s %s\n", info[i].code, info[i].room_name, info[i].players, info[i].max_players,
                   info[i].version, info[i].region[0] ? info[i].region : "-", info[i].colo[0] ? info[i].colo : "-");
        return n < 0;
    }
    if (!strcmp(mode, "host") ? net_create_room(p2p) : net_join_room(code)) {
        printf("FAILED: bad code or no server (set DHD_NET_SERVER)\n");
        return 1;
    }
    wait_for(in_lobby, 15, "the lobby");
    net_get_lobby(&l);
    printf("CODE %s member %d%s\n", l.code, l.member_id, l.is_host ? " (host)" : "");
    if (code_file) {
        FILE *fp = fopen(code_file, "w");
        fprintf(fp, "%s\n", l.code);
        fclose(fp);
    }
    if (public_room && l.is_host) {
        NetRoomSettings s = {-1, -1, 1, -1, -1, NULL};
        net_set_room(&s);
    }
    if (pick == 0) net_pick(-1, -1, 0);  /* on the rider screen, as far as the room knows */
    else net_pick(-1, pick > 0 ? pick : replay_file ? rider + 1 : l.member_id % NET_MAX_SLOTS + 1, 1);
    if (races > 0) {
        char phase[16] = "";
        double phase_at = net_seconds(), race_at = 0, loaded_at = 0;
        int acted = 0, readied = 0, started_count = 0, was_racing = 0, others_loaded = 0, n;
        int saw_go = 0;
        unsigned echoed = 0;
        double go_at = 0;  /* as the room's host: the countdown's start it announced (online.c start_step) */
        until = net_seconds() + seconds;
        if (l.is_host) net_rules(60, 0, 1);
        while (net_seconds() < until) {
            double now = net_seconds();
            net_get_lobby(&l);
            if (l.state == NET_FAILED) break;
            if (strcmp(phase, l.phase)) {
                snprintf(phase, sizeof phase, "%s", l.phase);
                printf("PHASE %s race %d\n", phase, started_count);
                phase_at = now;
                if (!strcmp(phase, "intermission") || !strcmp(phase, "open")) readied = 0;
            }
            if (l.state == NET_RACING && !was_racing) {
                started_count++; race_at = l.start_at > now ? l.start_at : now; acted = 0; readied = 0;
                others_loaded = 0, go_at = 0, saw_go = 0;
                printf("RACE %d slot %d\n", started_count, l.local_slot);
                /* its rider and bike, as replay() says them: the games' setup needn't wait 30 s for this one */
                uint8_t e[30] = {EVENT_VERSION, EVENT_SETUP};
                put_ride(e, rider);
                net_send_frame(NET_FRAME_EVENT, e, sizeof e);
            }
            was_racing = l.state == NET_RACING;
            if (l.state == NET_RACING && !acted && now - loaded_at >= 2) {
                /* "loaded", as a game says it until GO: the games' start needn't wait 60 s for this one;
                   and its rider again: a game still in its menus keeps only its newest 128 frames
                   (NET_RELAY_QUEUE), and a burst of bots' messages can push a rider's one out. Every
                   2 s: a game sends each once, so a room of bots must not flood the slow loaders. */
                uint8_t e[2] = {EVENT_VERSION, EVENT_LOADED}, ride[30] = {EVENT_VERSION, EVENT_SETUP};
                put_ride(ride, rider);
                net_send_frame(NET_FRAME_EVENT, ride, sizeof ride);
                net_send_frame(NET_FRAME_EVENT, e, 2);
                loaded_at = now;
                /* the host's start, as a host game's start_step says it once the others loaded: a
                   guest game's countdown needn't wait 75 s for this one */
                if (l.is_host && others_loaded) {
                    uint8_t s[10] = {EVENT_VERSION, EVENT_START};
                    if (!go_at) go_at = net_server_ms() + 1500;
                    memcpy(s + 2, &go_at, 8);
                    net_send_frame(NET_FRAME_EVENT, s, sizeof s);
                }
            }
            if (l.state == NET_RACING && !acted && (!echo_ai || saw_go) &&
                ((finish_after >= 0 && now - race_at >= finish_after) ||
                 (back_after >= 0 && now - race_at >= back_after))) {
                if (finish_after >= 0) {
                    unsigned ticks = (unsigned)((now - race_at) * 60);
                    if (echo_ai) {
                        uint8_t e[7] = {EVENT_VERSION, EVENT_FINISH, (uint8_t)l.local_slot};
                        put32(e + 3, (int32_t)ticks);
                        net_send_frame(NET_FRAME_EVENT, e, sizeof e);
                        printf("FINISH %.2f s, %u snapshots echoed\n", now - race_at, echoed);
                    }
                    net_finish(ticks, l.local_slot + 1);
                }
                if (finish_after < 0 || !echo_ai) net_back();
                acted = 1;
            }
            if (l.state != NET_RACING && l.last_race_id >= races) break;
            if (l.state == NET_LOBBY && (!strcmp(phase, "open") || !strcmp(phase, "intermission")) && !readied && ready_after >= 0 && pick != 0 &&
                (l.last_race_id > 0 || l.nmembers >= players_wanted) && now - phase_at >= ready_after) {
                net_set_ready(1); readied = 1;
            }
            while ((n = net_receive_frame(frame, sizeof frame)) > 0) {
                received++;
                if (follow_host && frame[0] == NET_FRAME_STATE && frame[1] == 0)
                    followed += resend_rider(frame + 2, n - 2, 0, l.local_slot, 6.0f * l.local_slot);
                if (frame[0] == NET_FRAME_EVENT && n >= 4 && frame[2] == EVENT_VERSION && frame[3] == EVENT_LOADED) others_loaded = 1;
                if (echo_ai && !saw_go && frame[0] == NET_FRAME_EVENT && n >= 12 &&
                    frame[2] == EVENT_VERSION && frame[3] == EVENT_GO) {
                    double server_go;
                    memcpy(&server_go, frame + 4, sizeof server_go);
                    race_at = now - (net_server_ms() - server_go) / 1000.0;
                    saw_go = 1;
                }
                /* Mirror a distinct host-owned AI's live snapshots. This exercises actual
                   puppet playback with no stored game data in the test fixture. */
                /* The AI riders follow the humans (ten in all): with many humans, bots share them. */
                if (echo_ai && saw_go && !acted && l.state == NET_RACING && frame[0] == NET_FRAME_STATE && l.nslots < NET_MAX_SLOTS)
                    echoed += resend_rider(frame + 2, n - 2, l.nslots + (l.local_slot - 1) % (NET_MAX_SLOTS - l.nslots),
                                           l.local_slot, 2.0f * l.local_slot);
            }
            Sleep(10);
        }
        printf("RESULT {\"races\":%d,\"received\":%d,\"echoed\":%u,\"snapshots\":%d}\n", started_count, received, echoed, followed);
        net_leave();
        return l.last_race_id < races;
    }
    if (ready_after > 0) Sleep((DWORD)(ready_after * 1000));
    if (ready_after >= 0 && pick != 0) net_set_ready(1);
    if (l.is_host) {
        wait_for(all_ready, 60, "the players");
        Sleep(300);
        net_start_race();
    }
    wait_for(racing, seconds, "the start");
    net_get_lobby(&l);
    slot = l.local_slot;
    printf("RACE slot %d of %d, ping %d ms, seed %u\n", slot, l.nslots, l.ping_ms, l.race_seed);
    if (replay_file) {
        int r = replay(replay_file, slot, rider, seconds);
        net_leave();
        return r;
    }
    until = net_seconds() + seconds;
    while (net_seconds() < until && net_racing()) {
        while (net_receive_frame(frame, sizeof frame) > 0)
            received++;
        Sleep(5);
    }
    net_get_lobby(&l);
    printf("RESULT {\"slot\":%d,\"received\":%d,\"ping\":%d,\"offline\":%d}\n", slot, received, l.ping_ms,
           l.offline);
    net_leave();
    timeEndPeriod(1);
    return 0;
}
