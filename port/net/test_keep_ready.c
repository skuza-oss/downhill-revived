/* keep_ready (client.c): a ready player whose ready the server cleared because the host changed
   the track says ready again by itself; a change of the laps field (the room's options: computer
   riders, CHEATS), a race's end (a session room's rotation), an own unready and the rider screen
   don't; DHD_KEEP_READY=0 turns it off. No
   server or game data. */
#include "client.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char sent[4096];
static int nsent;
static void text(void *ctx, const char *s) { (void)ctx; snprintf(sent, sizeof sent, "%s", s); nsent++; }
static void binary(void *ctx, const void *s, size_t n) { (void)ctx; (void)s; (void)n; }

static int last_race;  /* the room's lastRace.id: the server sends it in every room message once set */

static void room(NetClient *c, int track, int laps, int ready)
{
    char t[512];
    snprintf(t, sizeof t, "{\"type\":\"room\",\"hostId\":1,\"settings\":{\"track\":%d,\"laps\":%d},"
             "\"lastRace\":{\"id\":%d},"
             "\"members\":[{\"id\":1,\"name\":\"Host\",\"ready\":false},{\"id\":2,\"name\":\"Me\",\"ready\":%s}]}",
             track, laps, last_race, ready ? "true" : "false");
    client_on_text(c, t, strlen(t));
}

static int readied_by_itself(void) { return !strcmp(sent, "{\"type\":\"ready\",\"ready\":true}"); }

int main(void)
{
    NetClient c;
    NetTransport t;
    const char *welcome = "{\"type\":\"welcome\",\"memberId\":2,\"code\":\"TEST22\",\"serverTime\":1000}";
    client_init(&c, "Me", "test", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    memset(&t, 0, sizeof t); t.send_text = text; t.send_binary = binary;
    client_connected(&c, &t);
    client_on_text(&c, welcome, strlen(welcome));
    room(&c, 3, 2, 0);

    /* ready, then the host picks another track: the server clears it, the client readies again */
    client_ready(&c, 1); room(&c, 3, 2, 1);
    nsent = 0; room(&c, 5, 2, 0);
    assert(nsent == 1 && readied_by_itself() && c.ready);
    room(&c, 5, 2, 1);
    /* a race ended in a session room and the rotation moved to its next track: the server spent
       the ready (room.ts closeRace), it stays spent */
    last_race = 1;
    nsent = 0; room(&c, 4, 2, 0);
    assert(nsent == 0 && !c.ready);
    /* ready by hand for that one, then the host picks another track: readied again */
    client_ready(&c, 1); room(&c, 4, 2, 1);
    nsent = 0; room(&c, 5, 2, 0);
    assert(nsent == 1 && readied_by_itself() && c.ready);
    room(&c, 5, 2, 1);
    /* the laps field changed (CHEATS on, say, finding 117): ready again by hand */
    nsent = 0; room(&c, 5, 6, 0);
    assert(nsent == 0 && !c.ready);
    /* the track and the options at once: by hand too */
    client_ready(&c, 1); room(&c, 5, 6, 1);
    nsent = 0; room(&c, 6, 3, 0);
    assert(nsent == 0 && !c.ready);
    /* unready with the track unchanged (the player's own, or a race's end): nothing sent */
    client_ready(&c, 1); room(&c, 6, 3, 1);
    nsent = 0; room(&c, 6, 3, 0);
    assert(nsent == 0 && !c.ready);
    /* not ready before the change: stays not ready */
    nsent = 0; room(&c, 7, 3, 0);
    assert(nsent == 0 && !c.ready);
    /* on the rider screen: not readied (the server would refuse it) */
    client_ready(&c, 1); room(&c, 7, 3, 1);
    client_pick(&c, -1, -1, 0);
    nsent = 0; room(&c, 8, 3, 0);
    assert(nsent == 0 && !c.ready);
    /* turned off in dhd.ini */
    client_pick(&c, -1, 2, 1);
    client_ready(&c, 1); room(&c, 8, 3, 1);
#ifdef _WIN32
    _putenv("DHD_KEEP_READY=0");
#else
    setenv("DHD_KEEP_READY", "0", 1);
#endif
    nsent = 0; room(&c, 9, 3, 0);
    assert(nsent == 0 && !c.ready);
    client_free(&c);
    puts("test_keep_ready: ok");
    return 0;
}
