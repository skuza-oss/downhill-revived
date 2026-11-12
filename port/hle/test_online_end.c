/* Standalone: clang port/hle/test_online_end.c -o <scratch>/test-end (finding 211) */
#include "online_end.h"
#include <assert.h>
#include <string.h>
#define WHY(...) race_end_reason(&(RaceEnd){__VA_ARGS__})
int main(void)
{
    /* the bug: the phone hosted, the finish window ran out with its rider still riding. The server's
       "ended" (done) came first, the result not yet: that is TIME UP, not the host */
    assert(!strcmp(WHY(.ended = NET_ENDED_DONE, .is_host = 1, .new_result = 0), "TIME UP - DNF"));
    assert(!strcmp(WHY(.ended = NET_ENDED_DONE, .new_result = 1), "TIME UP - DNF"));
    /* everyone finished, this PC too */
    assert(!strcmp(WHY(.ended = NET_ENDED_DONE, .finished = 1), "RACE OVER"));
    /* the host ended it: said to the guests, and to the host who pressed it */
    assert(!strcmp(WHY(.ended = NET_ENDED_VOID), "THE HOST ENDED THE RACE"));
    assert(!strcmp(WHY(.ended = NET_ENDED_VOID, .is_host = 1), "YOU ENDED THE RACE"));
    assert(!strcmp(WHY(.ended = NET_ENDED_VOID, .finished = 1), "THE HOST ENDED THE RACE"));
    /* the connection or the room's seat gone */
    assert(!strcmp(WHY(.link_lost = 1), "CONNECTION LOST"));
    assert(!strcmp(WHY(.link_lost = 1, .finished = 1), "CONNECTION LOST"));
    assert(!strcmp(WHY(.link_lost = 1, .removed = 1), "THE HOST REMOVED YOU"));
    assert(!strcmp(WHY(.removed = 1, .ended = NET_ENDED_DONE), "TIME UP - DNF"));  /* a stale error code alone says nothing */
    /* out of the race while it goes on for the others, by this PC's own entry */
    assert(!strcmp(WHY(.own_dnf = "time_up"), "TIME UP - DNF"));
    assert(!strcmp(WHY(.own_dnf = "gave_up"), "YOU LEFT THE RACE"));
    assert(!strcmp(WHY(.own_dnf = "left"), "LINK TO THE OTHERS LOST"));
    /* an older server: no reason; never the host unless it said so */
    assert(!strcmp(WHY(.ended = NET_ENDED_UNKNOWN), "THE RACE ENDED"));
    assert(!strcmp(WHY(.ended = NET_ENDED_UNKNOWN, .new_result = 1), "TIME UP - DNF"));
    assert(!strcmp(WHY(.ended = NET_ENDED_UNKNOWN, .finished = 1), "RACE OVER"));
    assert(!strcmp(WHY(.ended = NET_ENDED_NONE), "THE RACE ENDED"));
    return 0;
}
