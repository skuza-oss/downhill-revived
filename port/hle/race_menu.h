/* The race menu online (finding 215): with another human still racing, START opens the room's
   menu over the running race instead of the game's pause, which would stop this game's race (its
   rider, the computer riders a host runs, its race clock) and so freeze them on every other screen.
   The race goes on under it: the rider gets a neutral pad (it coasts), the pad drives the menu.

   Rows: RESUME, BACK TO THE ROOM, END THE RACE (the host's), LEAVE THE ROOM; a race outside a
   room (the tests' DHD_NET_CREATE / DHD_NET_JOIN) has RESUME and LEAVE THE ROOM. The room's rows
   ask first (No / Yes, on No). OPTIONS is not offered: every settings page runs modally outside
   the race loop, so it would stop the race as the pause does.

   Alone (no other human still racing) START is the game's own pause as offline: nobody else's race
   depends on this game any more.

   Pure functions, no game memory: port/hle/test_race_menu.c tests them; online.c drives them once
   a field with the player's pad and draws the menu. */
#ifndef DHD_RACE_MENU_H
#define DHD_RACE_MENU_H
#include <stdint.h>
#include <string.h>

enum { RACE_MENU_ROWS = 4 };
enum { RM_RESUME = 1, RM_ROOM, RM_END, RM_LEAVE };  /* a row */
/* what a field did */
enum { RM_NOTHING, RM_OPENED, RM_CLOSED, RM_MOVED, RM_ASKED, RM_UNASKED, RM_CHOSEN };
/* the pad's buttons as pad.c has them (hle.h PAD_*) */
enum { RM_START = 0x0008, RM_UP = 0x0010, RM_DOWN = 0x0040, RM_TRIANGLE = 0x1000, RM_CROSS = 0x4000 };

typedef struct {
    int open;
    int rows, cursor;
    unsigned char row[RACE_MENU_ROWS];
    int asking;       /* the row the question asks about (0: the rows show) */
    int answer;       /* the question's cursor: 0 No, 1 Yes */
    uint16_t last;    /* the buttons last field: a press is a button down now and not then */
    uint16_t kept;    /* held when the menu closed: kept from the race until let go */
} RaceMenu;

static inline void race_menu_reset(RaceMenu *m) { memset(m, 0, sizeof *m); }

/* The rows for this race: a room's, the host's END THE RACE among them, or a race outside a room. */
static inline void race_menu_rows(RaceMenu *m, int room, int host)
{
    m->rows = 0;
    m->row[m->rows++] = RM_RESUME;
    if (room) m->row[m->rows++] = RM_ROOM;
    if (room && host) m->row[m->rows++] = RM_END;
    m->row[m->rows++] = RM_LEAVE;
}

/* A human's rider is still in the race: not finished, not left (gone: this game saw it go). */
static inline int race_menu_racing(int finished, int left, int gone) { return !finished && !left && !gone; }

/* This game's player is alone in the race: no other human (racing[j], j < humans, j != self) still
   racing. This game's own rider does not count: finished and watching, a host still runs the
   computer riders the others see. A race only loses racers (nobody joins mid-race), so alone stays
   alone until the race ends. */
static inline int race_menu_alone(const unsigned char *racing, int humans, int self)
{
    for (int j = 0; j < humans; j++)
        if (j != self && racing[j]) return 0;
    return 1;
}

/* Close: whatever is held now stays away from the race until it is let go (RESUME's Cross does
   not pedal, a held direction is no new press: finding 165's rule). */
static inline void race_menu_close(RaceMenu *m, uint16_t buttons)
{
    m->open = 0, m->asking = 0;
    m->kept = buttons;
}

/* One field with the player's buttons (the stick already folded into up / down by the caller).
   overlay: START opens this menu (else START reaches the game: its own pause, alone). Returns the
   buttons the race gets: none while the menu is open, else the pad's less those kept since it
   closed. *did says what happened; *chosen the row whose question was answered Yes (RM_CHOSEN). */
static inline uint16_t race_menu_field(RaceMenu *m, uint16_t buttons, int overlay, int *did, int *chosen)
{
    uint16_t pressed = (uint16_t)(buttons & ~m->last);
    m->last = buttons;
    m->kept &= buttons;
    *did = RM_NOTHING, *chosen = 0;
    if (!m->open) {
        if (overlay && (pressed & RM_START) && m->rows) {
            m->open = 1, m->cursor = 0, m->asking = 0, m->answer = 0;
            *did = RM_OPENED;
            return 0;
        }
        return (uint16_t)(buttons & ~m->kept);
    }
    if (m->asking) {
        if (pressed & (RM_TRIANGLE | RM_START)) {  /* No */
            m->asking = 0;
            *did = RM_UNASKED;
        } else if (pressed & (RM_UP | RM_DOWN)) {
            m->answer = !m->answer;
            *did = RM_MOVED;
        } else if (pressed & RM_CROSS) {
            if (m->answer) {
                *chosen = m->asking;
                *did = RM_CHOSEN;
                race_menu_close(m, buttons);
            } else {
                m->asking = 0;
                *did = RM_UNASKED;
            }
        }
        return 0;
    }
    if (pressed & (RM_START | RM_TRIANGLE)) {
        race_menu_close(m, buttons);
        *did = RM_CLOSED;
    } else if (pressed & RM_UP) {
        m->cursor = (m->cursor + m->rows - 1) % m->rows;
        *did = RM_MOVED;
    } else if (pressed & RM_DOWN) {
        m->cursor = (m->cursor + 1) % m->rows;
        *did = RM_MOVED;
    } else if (pressed & RM_CROSS) {
        if (m->row[m->cursor] == RM_RESUME) {
            race_menu_close(m, buttons);
            *did = RM_CLOSED;
        } else {
            m->asking = m->row[m->cursor], m->answer = 0;
            *did = RM_ASKED;
        }
    }
    return 0;
}

#endif
