/* Standalone: clang port/hle/test_online_start.c -o <scratch>/test-start (finding 187) */
#include "online_start.h"
#include <assert.h>
int main(void)
{
    int left[4] = {0, 0, 0, 0};
    assert(start_namer(0, left, 4) == 0);
    left[0] = 1;  /* the host left during the load: the lowest slot still racing names it */
    assert(start_namer(0, left, 4) == 1);
    left[1] = 1;
    assert(start_namer(0, left, 4) == 2);
    assert(start_namer(3, left, 4) == 3);
    left[2] = left[3] = 1;
    assert(start_namer(0, left, 4) == -1);
    assert(start_namer(0, left, 0) == -1);

    /* out of the race: the countdown runs and the room takes the game back */
    assert(start_rule(0, 1, 0) == START_GO);
    /* waiting for the moment: hold, also with no moment yet */
    assert(start_rule(1, 0, 0) == START_WAIT);
    assert(start_rule(1, START_GIVE_UP_S, 0) == START_WAIT);
    assert(start_rule(1, 10, 1) == START_WAIT);
    /* no moment after the give-up: never alone, out of the race (it raced alone before) */
    assert(start_rule(1, START_GIVE_UP_S + 0.1, 0) == START_LEAVE);
    /* a moment known (the room's, or late over the race link): held for it, whatever the wait */
    assert(start_rule(1, START_GIVE_UP_S + 30, 1) == START_WAIT);
    assert(START_NAME_AFTER_S < START_GIVE_UP_S);
    return 0;
}
