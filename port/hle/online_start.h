/* The start of an online race (port/hle/online.c start_step, finding 187): which racer names the
   moment every game holds its countdown for, and what a game waiting at the start does. The same
   rule on every PC. */
#ifndef DHD_ONLINE_START_H
#define DHD_ONLINE_START_H

/* The namer names the moment once every other racer has loaded, or this long (s) after it came to
   the start itself. A game with no moment from the namer (over the race link, or from the room)
   this long after it came there does not start alone: before finding 187 it did, and raced a ghost
   race while the others raced theirs. It leaves the race instead. */
enum { START_NAME_AFTER_S = 60, START_GIVE_UP_S = 75 };

/* The slot that names the moment: the race's host, or once it left, the lowest slot still racing
   (the room takes the moment from that racer only: online/server/src/room.ts onRaceGo); -1 none. */
static inline int start_namer(int host_slot, const int *left, int nslots)
{
    if (host_slot >= 0 && host_slot < nslots && !left[host_slot]) return host_slot;
    for (int i = 0; i < nslots; i++)
        if (!left[i]) return i;
    return -1;
}

enum { START_WAIT, START_GO, START_LEAVE };

/* What a game at the start does now. racing: it is still in the race; waited: seconds since it came
   to the start; moment: it knows the start moment. START_GO: the countdown runs now (out of the
   race: the room takes it back); START_WAIT: it holds (with a moment, start_step runs to it);
   START_LEAVE: no moment came, it leaves the race. */
static inline int start_rule(int racing, double waited, int moment)
{
    if (!racing) return START_GO;
    if (moment || waited <= START_GIVE_UP_S) return START_WAIT;
    return START_LEAVE;
}

#endif
