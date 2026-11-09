/* Session wire parsing and serializers without a server or game data. */
#include "client.h"
#include "lobby_row.h"
#include <SDL3/SDL_timer.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char sent[4096];
static void text(void *ctx, const char *s) { (void)ctx; snprintf(sent,sizeof sent,"%s",s); }
static int binaries;
static void binary(void *ctx, const void *s, size_t n) { (void)ctx; (void)s; (void)n; binaries++; }
int main(void)
{
    NetClient c;
    NetLobby l;
    NetTransport t;
    unsigned char rotation[] = {1,3 | 32,5 | 64};
    const char *welcome = "{\"type\":\"welcome\",\"memberId\":7,\"code\":\"TEST22\",\"serverTime\":1000}";
    const char *room = "{\"type\":\"room\",\"hostId\":7,\"phase\":\"intermission\",\"serverTime\":1000,"
        "\"nextRaceAt\":31000,\"raceStartedAt\":900,\"finishEndsAt\":null,\"countdownEndsAt\":null,"
        "\"rules\":{\"finishWindow\":120,\"nextRace\":\"auto\",\"then\":\"random\"},\"rulesPending\":true,"
        "\"waitingForRider\":true,\"lastEvent\":\"start_cancelled\",\"rotation\":[1,3,5],"
        "\"members\":[{\"id\":7,\"name\":\"Test\",\"ready\":true,\"inRace\":false,\"raceState\":\"finished\",\"finishTicks\":300,\"place\":4}],"
        "\"lastRace\":{\"id\":2,\"track\":3,\"entrants\":[{\"id\":7,\"name\":\"Test\",\"rider\":5,\"ticks\":300,\"place\":4},"
        "{\"id\":8,\"name\":\"Other\",\"dnfReason\":\"time_up\",\"place\":5}]}}";
    /* dhd.log shows a P2P room's code only by its first two characters (finding 180) */
    if (!getenv("DHD_NET_LOG_CODE")) assert(!strcmp(net_log_code("ABCDEFG").text,"AB*****"));
    assert(!strcmp(net_log_code("TEST22").text,"TEST22") && !strcmp(net_log_code("").text,""));
    client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    memset(&t,0,sizeof t); t.send_text=text; t.send_binary=binary;
    client_connected(&c,&t);
    assert(strstr(sent,"\"session\""));
    client_on_text(&c,welcome,strlen(welcome));
    client_on_text(&c,room,strlen(room));
    client_lobby(&c,&l);
    assert(!strcmp(l.phase,"intermission") && l.next_race_at==31000 && l.finish_ends_at==0);
    assert(l.rules_pending && l.waiting_for_rider && l.finish_window==120 && l.then==2 && !l.next_race);
    assert(l.rotation_count==3 && l.rotation[2]==5 && l.member[0].finish_ticks==300);
    assert(l.last_race_id==2 && l.nresults==2 && l.result[1].ticks==-1 && !strcmp(l.result[1].dnf_reason,"time_up"));
    assert(c.ready && !c.racing && l.state==NET_LOBBY);
    client_finish(&c,321,4); assert(strstr(sent,"\"ticks\":321,\"place\":4"));
    client_back(&c); assert(!strcmp(sent,"{\"type\":\"back\"}"));
    client_rules(&c,60,1,2); assert(strstr(sent,"\"nextRace\":\"host\"") && strstr(sent,"\"then\":\"random\""));
    client_rotation(&c,rotation,3); assert(strstr(sent,"[1,3,5]"));
    assert(strstr(sent,"\"trackLimits\":[10,8,6]") && strstr(sent,"\"pickOnly\":[]"));
    {   /* bit 7: pick-only (MOSH, finding 128); the limit class stays in bits 5-6 */
        unsigned char arena[] = {0, 23 | 64 | 128, 2};
        client_rotation(&c,arena,3);
        assert(strstr(sent,"\"rotation\":[0,23,2]") && strstr(sent,"\"trackLimits\":[10,6,10]") && strstr(sent,"\"pickOnly\":[23]"));
    }
    char ten[4096];
    int at = snprintf(ten, sizeof ten, "{\"type\":\"room\",\"members\":[");
    for (int i = 0; i < 10; i++) at += snprintf(ten + at, sizeof ten - at,
        "%s{\"id\":%d,\"name\":\"Player%d\",\"ready\":true}", i ? "," : "", i, i);
    snprintf(ten + at, sizeof ten - at, "]}");
    client_on_text(&c,ten,strlen(ten)); client_lobby(&c,&l);
    assert(l.nmembers == 10 && l.member[9].id == 9 && l.member[9].ready);
    at = snprintf(ten, sizeof ten, "{\"type\":\"start\",\"yourSlot\":9,\"players\":[");
    for (int i = 0; i < 10; i++) at += snprintf(ten + at, sizeof ten - at,
        "%s{\"slot\":%d,\"memberId\":%d,\"name\":\"Player%d\"}", i ? "," : "", i, i, i);
    snprintf(ten + at, sizeof ten - at, "]}");
    client_on_text(&c,ten,strlen(ten)); client_lobby(&c,&l);
    assert(l.nslots == 10 && l.local_slot == 9 && l.racer[9].member_id == 9);
    client_free(&c);
    {   /* the race's start moment over the room (finding 187): asked for in the hello, sent only to a
           server that lists it, taken only for this race, once */
        const char *older = "{\"type\":\"welcome\",\"memberId\":1,\"code\":\"GOGO22\",\"serverTime\":1000}";
        const char *newer = "{\"type\":\"welcome\",\"memberId\":1,\"code\":\"GOGO22\",\"serverTime\":1000,"
            "\"features\":[\"sfu_silent\",\"race_go\"]}";
        const char *start = "{\"type\":\"start\",\"yourSlot\":0,\"startAt\":5000,\"serverTime\":2000,"
            "\"players\":[{\"slot\":0,\"memberId\":1,\"name\":\"Test\"},{\"slot\":1,\"memberId\":2,\"name\":\"B\"}]}";
        const char *other = "{\"type\":\"race_go\",\"race\":4000,\"at\":9000}";
        const char *go = "{\"type\":\"race_go\",\"race\":5000,\"at\":9500.5}";
        const char *again = "{\"type\":\"race_go\",\"race\":5000,\"at\":12000}";
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
        assert(strstr(sent,"\"race_rejoin\",\"race_go\",\"p2p\"]"));
        client_on_text(&c,older,strlen(older));
        client_on_text(&c,start,strlen(start));
        sent[0] = 0;
        client_race_go(&c,9500); assert(!sent[0]);  /* a 0.7.13 server would refuse it */
        client_on_text(&c,newer,strlen(newer));
        client_race_go(&c,9500.5); assert(!strcmp(sent,"{\"type\":\"race_go\",\"race\":5000,\"at\":9500.500}"));
        client_on_text(&c,other,strlen(other)); client_lobby(&c,&l); assert(l.race_go_at == 0);
        client_on_text(&c,go,strlen(go)); client_on_text(&c,again,strlen(again));
        client_lobby(&c,&l); assert(l.race_go_at == 9500.5);
        client_on_text(&c,"{\"type\":\"ended\"}",16); client_lobby(&c,&l); assert(l.race_go_at == 0);
        sent[0] = 0; client_race_go(&c,9500); assert(!sent[0]);  /* no race */
        client_on_text(&c,start,strlen(start)); client_lobby(&c,&l); assert(l.race_go_at == 0);
        client_free(&c);
    }
    {   /* where this PC and the room are (finding 135): the welcome's fields, the where message's
           regions kept across room snapshots, and the net: lines on stderr */
        const char *located = "{\"type\":\"welcome\",\"memberId\":1,\"host\":false,\"code\":\"WHERE2\",\"serverTime\":1000,"
            "\"edgeColo\":\"WAW\",\"country\":\"PL\",\"continent\":\"EU\",\"roomColo\":\"\",\"roomRegion\":\"EU\"}";
        const char *where = "{\"type\":\"where\",\"region\":\"EU\",\"colo\":\"FRA\","
            "\"members\":[{\"id\":0,\"region\":\"EU\"},{\"id\":1,\"region\":\"EU\"},{\"id\":2,\"region\":\"ASIA\"}]}";
        const char *members = "{\"type\":\"room\",\"hostId\":0,\"members\":[{\"id\":0,\"name\":\"ANNA\",\"rttMs\":31},"
            "{\"id\":1,\"name\":\"Test\",\"rttMs\":40},{\"id\":2,\"name\":\"BOB\",\"rttMs\":null}]}";
        const char *pong = "{\"type\":\"pong\",\"t\":0}";
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
        client_on_text(&c,located,strlen(located));
        client_lobby(&c,&l);
        assert(!strcmp(l.edge_colo,"WAW") && !strcmp(l.country,"PL") && !strcmp(l.continent,"EU"));
        assert(!l.room_colo[0] && !strcmp(l.room_region,"EU"));
        client_on_text(&c,where,strlen(where));
        client_on_text(&c,members,strlen(members));
        client_on_text(&c,pong,strlen(pong));
        client_lobby(&c,&l);
        assert(!strcmp(l.room_colo,"FRA") && l.nmembers == 3);
        assert(!strcmp(l.member[0].region,"EU") && !strcmp(l.member[1].region,"EU") && !strcmp(l.member[2].region,"ASIA"));
        assert(c.window_count == 1 && c.joined_at > 0);
        client_leave(&c);  /* net: left room WHERE2 ... */
        assert(c.joined_at == 0 && c.window_count == 0);
        client_free(&c);
        /* an older server: no locations, all empty */
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
        client_on_text(&c,welcome,strlen(welcome));
        client_lobby(&c,&l);
        assert(!l.edge_colo[0] && !l.country[0] && !l.room_region[0]);
        client_free(&c);
    }
    {   /* the room's area and its creator's pick (finding 181): from the welcome, then the where
           message; the list's rows; the race ping's samples and their net: line */
        const char *picked = "{\"type\":\"welcome\",\"memberId\":0,\"host\":true,\"code\":\"AREA22\",\"serverTime\":1000,"
            "\"edgeColo\":\"MRS\",\"country\":\"DZ\",\"continent\":\"AF\",\"roomColo\":\"\",\"roomRegion\":\"ASIA\","
            "\"roomArea\":\"ASIA SE\",\"roomChosen\":true}";
        const char *where = "{\"type\":\"where\",\"region\":\"ASIA\",\"area\":\"ASIA SE\",\"colo\":\"SIN\",\"members\":[{\"id\":0,\"region\":\"AF\"}]}";
        const char *list = "{\"protocol\":4,\"lobbies\":[{\"code\":\"AREA22\",\"region\":\"EU\",\"area\":\"EU WEST\",\"colo\":\"MRS\"},"
            "{\"code\":\"OLD222\",\"region\":\"NA\"}]}";
        NetLobbyInfo rows[2];
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        assert(client_race_ms(&c) == -1);
        client_connected(&c,&t);
        client_on_text(&c,picked,strlen(picked));  /* net: in room AREA22 (host): ... ASIA SE (location not known yet, picked by its creator) */
        client_lobby(&c,&l);
        assert(!strcmp(l.room_area,"ASIA SE") && l.room_chosen && !strcmp(l.room_region,"ASIA"));
        client_on_text(&c,where,strlen(where));  /* net: room AREA22 runs in SIN (ASIA SE, picked by its creator) */
        client_lobby(&c,&l);
        assert(!strcmp(l.room_colo,"SIN") && !strcmp(l.room_area,"ASIA SE"));
        /* the race ping: one sample a second, the counter shows the median of the last 5 */
        client_race_rtt(&c, 40);
        assert(client_race_ms(&c) == 40 && c.race_window_n == 1);
        client_race_rtt(&c, 900);  /* within the same second: not a sample */
        assert(client_race_ms(&c) == 40 && c.race_window_n == 1);
        for (int ms = 41; ms <= 44; ms++) { c.next_race_sample = 0; client_race_rtt(&c, ms); }
        assert(client_race_ms(&c) == 42 && c.race_window_n == 5);
        c.next_race_sample = 0; client_race_rtt(&c, 500);  /* one slow check moves the median by one */
        assert(client_race_ms(&c) == 43);
        c.next_log = 0;
        client_tick(&c);  /* net: room AREA22, last 0 s: no pings; race network 42 ms median, 500 worst (6 samples)... */
        assert(c.race_window_n == 0);
        client_race_rtt(&c, -1);  /* the association is gone */
        assert(client_race_ms(&c) == -1);
        client_free(&c);
        /* the list: the area when the server sends one, else nothing (an older server: the region) */
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        assert(client_read_lobbies(&c, list, net_now_ms(), net_now_ms(), rows, 2) == 2);
        assert(!strcmp(rows[0].area, "EU WEST") && !strcmp(rows[0].region, "EU") && !rows[1].area[0] && !strcmp(rows[1].region, "NA"));
        client_free(&c);
        /* a 0.7.13 server's welcome: no area, not picked */
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
        {
            const char *old = "{\"type\":\"welcome\",\"memberId\":1,\"code\":\"OLD222\",\"serverTime\":1000,"
                "\"edgeColo\":\"WAW\",\"country\":\"PL\",\"continent\":\"EU\",\"roomColo\":\"FRA\",\"roomRegion\":\"EU\"}";
            client_on_text(&c,old,strlen(old));
        }
        client_lobby(&c,&l);
        assert(!l.room_area[0] && !l.room_chosen && !strcmp(l.room_region,"EU"));
        client_free(&c);
    }
    {   /* the public list in no room (finding 135): its "now" sets the clock, so a racing room's
           ENDS IN counts from the server's time; 0.7.6 counted from the epoch (ENDS IN 29893031:32).
           Without "now" (an older server) and no room, the row shows no time at all. */
        char body[1024], now_text[24];
        NetLobbyInfo rows[4];
        double now = 1793570000000.0;
        snprintf(body, sizeof body, "{\"protocol\":2,\"now\":%.0f,\"lobbies\":["
                 "{\"code\":\"ABC234\",\"roomName\":\"HEINRICHPY\",\"players\":6,\"maxPlayers\":10,\"phase\":\"racing\","
                 "\"raceStartedAt\":%.0f,\"finishEndsAt\":%.0f,\"region\":\"EU\"},"
                 "{\"code\":\"DEF567\",\"players\":2,\"maxPlayers\":10,\"phase\":\"intermission\",\"nextRaceAt\":%.0f},"
                 "{\"code\":\"GHJ892\",\"players\":3,\"maxPlayers\":10,\"phase\":\"racing\",\"raceStartedAt\":%.0f}]}",
                 now, now - 65000, now + 42000, now + 20000, now - 125000);
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        assert(!c.clock_known);
        assert(client_read_lobbies(&c, body, net_now_ms(), net_now_ms(), rows, 4) == 3);
        assert(c.clock_known && rows[0].timed && !strcmp(rows[0].region, "EU"));
        net_lobby_now(&rows[0], net_now_ms() + c.clock_offset, now_text, sizeof now_text);
        printf("list in no room: %s %s; ", rows[0].room_name, now_text);
        assert(!strcmp(now_text, "ENDS IN 0:42") || !strcmp(now_text, "ENDS IN 0:41"));
        net_lobby_now(&rows[1], net_now_ms() + c.clock_offset, now_text, sizeof now_text);
        printf("%s; ", now_text);
        assert(!strncmp(now_text, "NEXT IN 0:", 10));
        net_lobby_now(&rows[2], net_now_ms() + c.clock_offset, now_text, sizeof now_text);
        printf("%s\n", now_text);
        assert(!strcmp(now_text, "RACING 2:05") || !strcmp(now_text, "RACING 2:04"));
        /* the finish window over (the list older than that): the server closed the race, so the
           row says what comes next, not the race's clock (0.7.7 showed RACING 1:06) */
        net_lobby_now(&rows[0], net_now_ms() + c.clock_offset + 50000, now_text, sizeof now_text);
        printf("window over: %s\n", now_text);
        assert(!strcmp(now_text, "NEXT RACE"));
        client_free(&c);
        /* an older server's list (no "now"), no room: no clock, no times */
        snprintf(body, sizeof body, "{\"protocol\":2,\"lobbies\":[{\"code\":\"ABC234\",\"phase\":\"racing\","
                 "\"raceStartedAt\":%.0f,\"finishEndsAt\":%.0f},{\"code\":\"DEF567\",\"phase\":\"intermission\",\"nextRaceAt\":%.0f}]}",
                 now - 65000, now + 42000, now + 20000);
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        assert(client_read_lobbies(&c, body, net_now_ms(), net_now_ms(), rows, 4) == 2 && !rows[0].timed);
        net_lobby_now(&rows[0], net_now_ms() + c.clock_offset, now_text, sizeof now_text);
        assert(!strcmp(now_text, "RACING"));
        net_lobby_now(&rows[1], net_now_ms() + c.clock_offset, now_text, sizeof now_text);
        assert(!strcmp(now_text, "NEXT RACE"));
        /* even marked timed, an epoch-sized difference never shows */
        rows[0].timed = 1;
        net_lobby_now(&rows[0], net_now_ms(), now_text, sizeof now_text);
        assert(!strcmp(now_text, "RACING"));
        client_free(&c);
    }
    {   /* the rejoin token (finding 137): kept from the welcome, sent back by the rejoin's hello,
           gone for the next room; a token that isn't hex never goes back into JSON */
        const char *tokened = "{\"type\":\"welcome\",\"memberId\":3,\"code\":\"BACK22\",\"serverTime\":1000,"
            "\"rejoinToken\":\"0123456789abcdef0123456789abcdef\"}";
        const char *odd = "{\"type\":\"welcome\",\"memberId\":3,\"code\":\"BACK22\",\"serverTime\":1000,"
            "\"rejoinToken\":\"ab\\\",\\\"x\"}";
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
        assert(strstr(sent,"\"type\":\"hello\"") && !strstr(sent,"rejoinToken"));
        client_on_text(&c,tokened,strlen(tokened));
        assert(client_on_closed(&c,1006,"") == 1);  /* a drop in the room: rejoin */
        client_connected(&c,&t);
        assert(strstr(sent,"\"type\":\"hello\"") && strstr(sent,",\"rejoinToken\":\"0123456789abcdef0123456789abcdef\"}"));
        client_free(&c);
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);  /* the next room: no token from the last one */
        assert(!strstr(sent,"rejoinToken"));
        client_on_text(&c,odd,strlen(odd));
        assert(client_on_closed(&c,1006,"") == 1);
        client_connected(&c,&t);
        assert(strstr(sent,"\"type\":\"hello\"") && !strstr(sent,"rejoinToken"));
        client_free(&c);
    }
    {   /* Snapshot freshness and long gaps are per sender and sampled on network ticks. */
        /* A bundle's header (port/hle/state_bundle.h): version 6, index, riders, the stamp. */
        unsigned char state[13] = {NET_FRAME_STATE, 2, 6, 0, 1};
        double stamp = 100000 + net_now_ms() - 150, newer = stamp + 1;
        client_init(&c,"Test","test","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
        c.racing = 1;
        c.clock_offset = 100000;
        memcpy(state + 5, &stamp, sizeof stamp);
        c.snapshot[2].arrived = net_now_ms() - 150;
        client_on_binary(&c,state,sizeof state);
        assert(c.snapshot[2].gaps == 1 && c.snapshot[1].gaps == 0 && c.snapshot[2].messages == 1);
        client_tick(&c);
        assert(c.snapshot[2].samples == 1 && c.snapshot[1].samples == 0);
        {   /* about 150 ms old (a loaded PC may take a little longer between the two lines) */
            unsigned near = 0;
            for (int ms = 145; ms < 400; ms++) near += c.snapshot[2].age[ms];
            assert(c.snapshot[2].epoch == stamp && near == 1);
        }
        {
            unsigned char later[13];
            memcpy(later, state, sizeof later);
            memcpy(later + 5, &newer, sizeof newer);
            client_on_binary(&c,later,sizeof later);
            assert(c.snapshot[2].epoch == newer);
            client_on_binary(&c,state,sizeof state);  /* an older unordered message */
            assert(c.snapshot[2].epoch == newer && c.snapshot[2].messages == 3);
        }
        c.snapshot[2].epoch = c.clock_offset + net_now_ms() - 2000;
        client_tick(&c);
        assert(c.snapshot[2].age[1001] == 1);
        client_free(&c);
    }
    {   /* The shared clock (finding 169): a race PONG with the server's time sets the offset, then
           follows it by at most 2 ms a pong; a slow round trip or a PONG without the clock changes
           nothing. */
        unsigned char pong[2 + 16] = {NET_FRAME_PONG, 0};
        double sent, server;
        client_init(&c,"Test","test","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
#define PONG(rtt, offset) (sent = net_now_ms() - (rtt), server = sent + (rtt) / 2 + (offset), \
                           memcpy(pong + 2, &sent, 8), memcpy(pong + 10, &server, 8))
        PONG(10, 1000.0);
        client_on_binary(&c,pong,sizeof pong);
        assert(c.have_offset && fabs(c.clock_offset - 1000) < 1.5);
        PONG(10, 1100.0);
        client_on_binary(&c,pong,sizeof pong);
        assert(fabs(c.clock_offset - 1002) < 1.5);
        PONG(400, 900.0);                  /* slower than the usual round trip: not trusted */
        client_on_binary(&c,pong,sizeof pong);
        assert(fabs(c.clock_offset - 1002) < 1.5);
        PONG(10, 500.0);
        client_on_binary(&c,pong,10);      /* no server clock: not a PONG */
        assert(fabs(c.clock_offset - 1002) < 1.5);
        /* round trips slow for a long time, one way (a busy uplink: the server's time is read
           late on the way out, so it looks 60 ms ahead): the median follows them, the clock
           must not (the 45 ms skew of finding 169's PC-Mac run) */
        for (int i = 0; i < 40; i++) {
            PONG(160, 1062.0);
            client_on_binary(&c,pong,sizeof pong);
        }
        assert(c.lobby.ping_ms >= 150 && fabs(c.clock_offset - 1002) < 1.5);
        PONG(11, 1003.0);                  /* a quick one again: trusted */
        client_on_binary(&c,pong,sizeof pong);
        assert(fabs(c.clock_offset - 1003) < 1.5);
        /* DHD_NET_TEST_PONG_DELAY (finding 175): a pong is held, then counts at its time */
        c.pong_delay_ms = 50, c.pong_delay_from_s = 0, c.joined_at = net_now_ms() - 1;
        PONG(11, 1004.0);
        client_on_binary(&c,pong,sizeof pong);
        assert(c.held_count == 1 && fabs(c.clock_offset - 1003) < 1.5);
        {
            double until = net_now_ms() + 1000;
            while (c.held_count && net_now_ms() < until) { SDL_Delay(5); client_tick(&c); }
        }
        assert(!c.held_count && c.lobby.ping_ms >= 50);  /* its round trip includes the hold */
#undef PONG
        client_free(&c);
    }
    {   /* the install id (finding 161): in every hello once set, only when it is 32 lowercase hex;
           none (an id never made) sends a hello without it, as older releases do */
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
        assert(strstr(sent,"\"type\":\"hello\"") && !strstr(sent,"installId"));
        client_free(&c);
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_set_install_id(&c,"0123456789abcdef0123456789abcdef");
        client_connected(&c,&t);
        assert(strstr(sent,",\"installId\":\"0123456789abcdef0123456789abcdef\""));
        client_set_install_id(&c,"0123456789ABCDEF0123456789abcdef");
        client_connected(&c,&t);
        assert(!strstr(sent,"installId"));
        client_set_install_id(&c,"\",\"x\":\"0123456789abcdef01234567");
        client_connected(&c,&t);
        assert(!strstr(sent,"installId"));
        client_free(&c);
    }
    {   /* A drop in the race (finding 175): the race goes on (state, frames), the rejoin's hello
           names the race, the welcome keeps it racing and what the drop may have lost goes again;
           a refusal or the end of the attempts leaves it racing alone, offline. */
        const char *tokened = "{\"type\":\"welcome\",\"memberId\":1,\"code\":\"RACE22\",\"serverTime\":1000,"
            "\"rejoinToken\":\"0123456789abcdef0123456789abcdef\"}";
        const char *start = "{\"type\":\"start\",\"yourSlot\":1,\"startAt\":5000,\"serverTime\":2000,\"players\":["
            "{\"slot\":0,\"memberId\":0,\"name\":\"A\"},{\"slot\":1,\"memberId\":1,\"name\":\"Test\"}]}";
        const char *back_in = "{\"type\":\"welcome\",\"memberId\":1,\"code\":\"RACE22\",\"serverTime\":9000,"
            "\"rejoinToken\":\"0123456789abcdef0123456789abcdef\",\"sfu\":\"ready\",\"race\":{\"startAt\":5000}}";
        const char *no_race = "{\"type\":\"welcome\",\"memberId\":1,\"code\":\"RACE22\",\"serverTime\":9000,"
            "\"rejoinToken\":\"0123456789abcdef0123456789abcdef\"}";
        const char *out = "{\"type\":\"room\",\"phase\":\"racing\",\"members\":[{\"id\":1,\"name\":\"Test\",\"inRace\":false}]}";
        unsigned char payload[4] = {1, 2, 3, 4};
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
        assert(strstr(sent,"\"race_rejoin\"") && !strstr(sent,"raceStartAt"));
        client_on_text(&c,tokened,strlen(tokened));
        client_on_text(&c,start,strlen(start));
        assert(c.racing && c.race_at == 5000);
        assert(client_on_closed(&c,1006,"") == 1);
        client_lobby(&c,&l);
        assert(c.rejoining && l.state == NET_RACING && !l.offline);
        binaries = 0;
        assert(client_send_frame(&c, NET_FRAME_STATE, payload, sizeof payload) && binaries == 1);  /* over the SFU */
        assert(client_on_closed(&c,4011,"rate_limited") == 1);  /* a rejoin refused by the limits: again */
        client_connected(&c,&t);
        assert(strstr(sent,"\"rejoinToken\":\"0123456789abcdef0123456789abcdef\",\"raceStartAt\":5000}"));
        client_lobby(&c,&l);
        assert(l.state == NET_RACING);
        client_finish(&c, 777, 2);
        assert(client_on_closed(&c,1006,"") == 1);
        client_finish(&c, 777, 2);  /* said while the socket was down: lost */
        client_connected(&c,&t);
        client_on_text(&c,back_in,strlen(back_in));
        client_lobby(&c,&l);
        assert(l.state == NET_RACING && c.racing && !c.rejoining && c.rejoin_tries == 0);
        assert(!strcmp(sent,"{\"type\":\"finish\",\"ticks\":777,\"place\":2}"));  /* said again */
        /* gave up while the socket was down: BACK again after the rejoin */
        assert(client_on_closed(&c,1006,"") == 1);
        client_back(&c);
        client_connected(&c,&t);
        assert(strstr(sent,"\"raceStartAt\":5000"));
        client_on_text(&c,back_in,strlen(back_in));
        assert(!strcmp(sent,"{\"type\":\"back\"}"));
        client_free(&c);
        /* back, but the race is over for us: the room's snapshot takes the game back to the room */
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
        client_on_text(&c,tokened,strlen(tokened));
        client_on_text(&c,start,strlen(start));
        assert(client_on_closed(&c,1006,"") == 1);
        client_connected(&c,&t);
        client_on_text(&c,no_race,strlen(no_race));
        client_on_text(&c,out,strlen(out));
        client_lobby(&c,&l);
        assert(!c.racing && l.state == NET_LOBBY && c.race_at == 0);
        client_free(&c);
        /* a refusal in the race, or the attempts' end: racing alone, offline, as before */
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
        client_on_text(&c,tokened,strlen(tokened));
        client_on_text(&c,start,strlen(start));
        assert(client_on_closed(&c,1006,"") == 1);
        assert(client_on_closed(&c,4006,"room_not_found") == 0);
        client_lobby(&c,&l);
        assert(l.offline && l.state == NET_FAILED && !c.rejoining);
        client_free(&c);
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
        client_on_text(&c,tokened,strlen(tokened));
        client_on_text(&c,start,strlen(start));
        assert(client_on_closed(&c,1006,"") == 1);
        client_rejoin_failed(&c);
        client_lobby(&c,&l);
        assert(l.offline && l.state == NET_FAILED);
        binaries = 0;
        assert(!client_send_frame(&c, NET_FRAME_STATE, payload, sizeof payload) && binaries == 0);
        client_free(&c);
        /* in the room, a first join refused by the limits stays refused */
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
        assert(client_on_closed(&c,4011,"rate_limited") == 0);
        client_free(&c);
    }
    {   /* the room restarts for new code (finding 184): its notice, its 1012 close, the rejoin */
        const char *notice = "{\"type\":\"notice\",\"code\":\"server_update\",\"message\":\"Server updated, reconnecting...\"}";
        const char *odd = "{\"type\":\"notice\",\"code\":\"server_update\",\"message\":\"café\n\"}";
        client_init(&c,"Test","0.7.4","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        client_connected(&c,&t);
        client_on_text(&c,welcome,strlen(welcome));
        client_on_text(&c,notice,strlen(notice));
        client_lobby(&c,&l);
        assert(!strcmp(l.notice,"Server updated, reconnecting...") && l.state == NET_LOBBY);
        assert(client_on_closed(&c,1012,"server_update") == 1);
        client_lobby(&c,&l);
        assert(l.state == NET_RECONNECTING && l.notice[0] && !l.error[0]);
        client_connected(&c,&t);
        client_on_text(&c,welcome,strlen(welcome));
        client_lobby(&c,&l);
        assert(l.state == NET_LOBBY && !l.notice[0]);
        client_on_text(&c,odd,strlen(odd));  /* only what the game's font can draw */
        client_lobby(&c,&l);
        for (const char *q = l.notice; *q; q++) assert(*q >= 0x20 && *q <= 0x7e);
        client_free(&c);
    }
    puts("test_session: session and ten-player checks passed");
    return 0;
}
