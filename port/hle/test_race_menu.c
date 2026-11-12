/* The race menu online (finding 215): who is alone, the rows, and the pad's gate with its edges.
   Built and run by port/net/build_tests.py (check.py port). */
#include "race_menu.h"
#include <assert.h>
#include <stdio.h>

static uint16_t field(RaceMenu *m, uint16_t buttons, int overlay, int *did, int *chosen)
{
    return race_menu_field(m, buttons, overlay, did, chosen);
}

int main(void)
{
    int did, chosen;
    RaceMenu m;

    /* alone: no other human still racing; this game's own rider does not count */
    unsigned char racing[4] = {1, 1, 0, 0};
    assert(!race_menu_alone(racing, 4, 0));
    racing[1] = 0;
    assert(race_menu_alone(racing, 4, 0));
    racing[0] = 0;  /* finished and watching: still alone */
    assert(race_menu_alone(racing, 4, 0));
    racing[3] = 1;
    assert(!race_menu_alone(racing, 4, 0) && race_menu_alone(racing, 4, 3));
    assert(race_menu_alone(racing, 1, 0));  /* a room of one */
    assert(race_menu_racing(0, 0, 0) && !race_menu_racing(1, 0, 0) && !race_menu_racing(0, 1, 0) && !race_menu_racing(0, 0, 1));

    /* rows: a guest's room, a host's room, a race outside a room */
    race_menu_reset(&m);
    race_menu_rows(&m, 1, 0);
    assert(m.rows == 3 && m.row[0] == RM_RESUME && m.row[1] == RM_ROOM && m.row[2] == RM_LEAVE);
    race_menu_rows(&m, 1, 1);
    assert(m.rows == 4 && m.row[2] == RM_END && m.row[3] == RM_LEAVE);
    race_menu_rows(&m, 0, 1);
    assert(m.rows == 2 && m.row[0] == RM_RESUME && m.row[1] == RM_LEAVE);

    /* closed: the pad reaches the race; START without the overlay too (the game's own pause) */
    race_menu_reset(&m);
    race_menu_rows(&m, 1, 1);
    assert(field(&m, RM_CROSS, 1, &did, &chosen) == RM_CROSS && did == RM_NOTHING);
    assert(field(&m, RM_CROSS | RM_START, 0, &did, &chosen) == (RM_CROSS | RM_START) && !m.open);
    field(&m, RM_CROSS, 0, &did, &chosen);

    /* START opens it; the race gets nothing while it is open, a held Cross (pedalling) is no press */
    assert(field(&m, RM_CROSS | RM_START, 1, &did, &chosen) == 0 && did == RM_OPENED && m.open && m.cursor == 0);
    /* the START that opened it, still held, does not close it */
    assert(field(&m, RM_CROSS | RM_START, 1, &did, &chosen) == 0 && did == RM_NOTHING && m.open);
    assert(field(&m, RM_CROSS, 1, &did, &chosen) == 0 && did == RM_NOTHING && m.open);
    field(&m, 0, 1, &did, &chosen);

    /* down / up wrap over the rows */
    field(&m, RM_DOWN, 1, &did, &chosen);
    assert(did == RM_MOVED && m.cursor == 1);
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_UP, 1, &did, &chosen);
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_UP, 1, &did, &chosen);
    assert(m.cursor == 3);
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_DOWN, 1, &did, &chosen);
    assert(m.cursor == 0);
    field(&m, 0, 1, &did, &chosen);

    /* RESUME with Cross: closed, and the Cross held through it never pedals until it is let go */
    assert(field(&m, RM_CROSS, 1, &did, &chosen) == 0 && did == RM_CLOSED && !m.open);
    assert(field(&m, RM_CROSS, 1, &did, &chosen) == 0);
    assert(field(&m, RM_CROSS | 0x0080, 1, &did, &chosen) == 0x0080);  /* a new press (left) goes through */
    assert(field(&m, 0, 1, &did, &chosen) == 0);
    assert(field(&m, RM_CROSS, 1, &did, &chosen) == RM_CROSS);         /* pressed again: it pedals */

    /* a direction held through RESUME (START this time) is no new press either */
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_START, 1, &did, &chosen);
    assert(m.open);
    field(&m, 0x0020, 1, &did, &chosen);
    assert(field(&m, 0x0020 | RM_START, 1, &did, &chosen) == 0 && did == RM_CLOSED);
    assert(field(&m, 0x0020, 1, &did, &chosen) == 0);
    assert(field(&m, 0, 1, &did, &chosen) == 0 && field(&m, 0x0020, 1, &did, &chosen) == 0x0020);

    /* Triangle closes the rows */
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_START, 1, &did, &chosen);
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_TRIANGLE, 1, &did, &chosen);
    assert(did == RM_CLOSED && !m.open);

    /* a room row asks on No: Cross on No goes back, Triangle too; Yes chooses it and closes */
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_START, 1, &did, &chosen);
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_DOWN, 1, &did, &chosen);
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_CROSS, 1, &did, &chosen);
    assert(did == RM_ASKED && m.asking == RM_ROOM && m.answer == 0 && m.open);
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_CROSS, 1, &did, &chosen);
    assert(did == RM_UNASKED && !m.asking && m.open && m.cursor == 1);
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_CROSS, 1, &did, &chosen);
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_TRIANGLE, 1, &did, &chosen);
    assert(did == RM_UNASKED && m.open);
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_DOWN, 1, &did, &chosen);  /* END THE RACE (host) */
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_CROSS, 1, &did, &chosen);
    assert(m.asking == RM_END);
    field(&m, 0, 1, &did, &chosen);
    field(&m, RM_DOWN, 1, &did, &chosen);
    assert(m.answer == 1);
    field(&m, 0, 1, &did, &chosen);
    assert(field(&m, RM_CROSS, 1, &did, &chosen) == 0 && did == RM_CHOSEN && chosen == RM_END && !m.open);
    assert(field(&m, RM_CROSS, 1, &did, &chosen) == 0);  /* the Yes's Cross never reaches the race */

    /* no rows (not set up): START is never taken */
    race_menu_reset(&m);
    assert(field(&m, RM_START, 1, &did, &chosen) == RM_START && !m.open);

    printf("test_race_menu: ok\n");
    return 0;
}
