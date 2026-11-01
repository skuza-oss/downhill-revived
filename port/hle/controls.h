/* Rebindable controls (finding 141): which keyboard key and which gamepad button or axis drive
   each of the game's controls, and the profile text dhd.ini keeps.

   Two tables, one per device, since the port reads them in different places:
   - the keyboard (keyboard.c) has one entry per key it reads, each with the pad button it makes in
     menus and while riding (the arrows move the left stick while riding: the ramp);
   - the controllers (gamepad.c, SDL) have one entry per pad input the port reads (a button, a
     trigger or a stick axis).
   A player rebinds an entry to another key, or to another SDL button/axis. The defaults are the
   layout findings 059 and 091 shipped, so nothing changes until a player changes it.

   The text ("bindings" in dhd.ini, the crate's setting, DHD_BINDINGS): a comma-separated list of
   "key.<control>=<key name>" and "pad.<control>=<source name>" assignments, e.g.
   "key.accelerate=space,pad.south=south". An empty value unbinds. Unknown ids, keys and sources
   are skipped, so an old or hand-edited file never breaks the game.

   Also here: the catalogs the CONTROLS page cycles (a fixed list of keys, and of SDL sources), so
   the page needs no keyboard or controller of its own. */
#ifndef PORT_CONTROLS_H
#define PORT_CONTROLS_H

#include <stdint.h>

/* The stick axis a control drives (the same numbers keyboard.c always used). */
enum { CONTROL_AXIS_NONE, CONTROL_AXIS_LEFT_X, CONTROL_AXIS_LEFT_Y, CONTROL_AXIS_RIGHT_X, CONTROL_AXIS_RIGHT_Y };

/* An SDL gamepad source (gamepad.c maps these to SDL's enums). */
enum { PADSRC_NONE, PADSRC_SOUTH, PADSRC_EAST, PADSRC_WEST, PADSRC_NORTH,
       PADSRC_BACK, PADSRC_START, PADSRC_LEFT_SHOULDER, PADSRC_RIGHT_SHOULDER,
       PADSRC_DPAD_UP, PADSRC_DPAD_DOWN, PADSRC_DPAD_LEFT, PADSRC_DPAD_RIGHT,
       PADSRC_L3, PADSRC_R3, PADSRC_LEFT_TRIGGER, PADSRC_RIGHT_TRIGGER,
       PADSRC_LEFT_X, PADSRC_LEFT_Y, PADSRC_RIGHT_X, PADSRC_RIGHT_Y, PADSRC_COUNT };

/* The greatest number of controls of one kind (the page's arrays). */
enum { CONTROL_KEY_MAX = 32, CONTROL_PAD_MAX = 32, CONTROL_TEXT = 2048 };

/* ---- the keyboard controls (keyboard.c) ---- */
int control_key_count(void);
const char *control_key_id(int i);       /* the dhd.ini id, e.g. "accelerate" */
const char *control_key_label(int i);    /* the words the CONTROLS page shows */
unsigned control_key_vk(int i);          /* the bound virtual-key code (0: unbound) */
unsigned control_key_default_vk(int i);
unsigned control_key_menu(int i);        /* the pad button in menus (0: none) */
unsigned control_key_riding(int i);      /* the pad button while riding (0: none) */
unsigned control_key_axis(int i);        /* while riding: the stick axis it moves (0: none) */
int control_key_dir(int i);              /* that axis' direction: -1 up/left, 1 down/right */
void control_key_set(int i, unsigned vk);
int control_key_index(const char *id);   /* -1 unknown */

/* ---- the gamepad controls (gamepad.c) ---- */
int control_pad_count(void);
const char *control_pad_id(int i);
const char *control_pad_label(int i);
unsigned control_pad_button(int i);      /* the PAD_* bit it sets (0: none) */
unsigned control_pad_axis(int i);        /* the stick byte it drives (CONTROL_AXIS_*, 0: none) */
unsigned control_pad_source(int i);      /* the bound PADSRC_* (0: unbound) */
unsigned control_pad_default_source(int i);
void control_pad_set(int i, unsigned source);
int control_pad_index(const char *id);
/* Whether `source` suits control `i`: a stick control takes an axis, a button control a button or
   a trigger. */
int control_pad_accepts(int i, unsigned source);

/* ---- the catalogs the page cycles ---- */
int control_key_catalog_count(void);
const char *control_key_catalog_name(int j);     /* "a", "left", "space", ... */
unsigned control_key_catalog_vk(int j);
int control_key_catalog_index(unsigned vk);      /* -1 not in the catalog */

int control_pad_catalog_count(void);
const char *control_pad_catalog_name(int j);     /* "south", "left_trigger", "left_x", ... */
unsigned control_pad_catalog_source(int j);
int control_pad_catalog_index(unsigned source);

const char *control_pad_source_name(unsigned source);  /* "south", "" for none */
unsigned control_pad_source_of_name(const char *name);
unsigned control_key_vk_of_name(const char *name);
const char *control_key_name_of_vk(unsigned vk);       /* "" for a key outside the catalog */

/* ---- the profiles ---- */
void controls_default(void);            /* the shipped layout again */
int controls_parse(const char *text);   /* apply a bindings text; the settings it took */
int controls_format(char *out, int size);  /* the tables as the text a save writes */
int controls_customised(void);          /* any control differs from its default */

/* The dhd.ini key a profile's bindings live under: "bindings" for the default profile, else
   "bindings_<name>" (a name of letters, digits, '_' and '-'). */
void control_profile_key(const char *name, char *out, int size);

#endif
