/* Session wire parsing and serializers without a server or game data. */
#include "client.h"
#include "lobby_row.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char sent[4096];
static void text(void *ctx, const char *s) { (void)ctx; snprintf(sent,sizeof sent,"%s",s); }
static void binary(void *ctx, const void *s, size_t n) { (void)ctx; (void)s; (void)n; }
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
           follows it by at most 2 ms a pong; a slow round trip or an older server's PONG changes
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
        client_on_binary(&c,pong,10);      /* a protocol 3 PONG: the send time only */
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
    puts("test_session: session and ten-player checks passed");
    return 0;
}
