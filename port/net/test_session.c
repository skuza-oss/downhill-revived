/* Session wire parsing and serializers without a server or game data. */
#include "client.h"
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
    puts("test_session: session and ten-player checks passed");
    return 0;
}
