/* dhd.ini, the player's settings: the C interface of the Rust crate dhd_settings
   (port/render/settings, linked into dhd_render's static library). The crate holds every key, its
   default and choices, how the file is read and how a change is saved (finding 101); the game's
   host (port/hle/host.c) and the Android start call it. Strings are UTF-8 and NUL-terminated. */
#ifndef DHD_SETTINGS_H
#define DHD_SETTINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef const char *(*DhdSettingsGet)(const char *name);          /* a variable's value, or NULL */
typedef void (*DhdSettingsSet)(const char *name, const char *value);
typedef void (*DhdSettingsUnknown)(const char *key);

/* dhd.ini at path into the environment: each setting's DHD_* variable (dhd.ini's value, else the
   player's default) through set, unless get finds it set already (the environment overrides the
   file), and DHD_ENV_OVERRIDES, the list (",DHD_SCALE,DHD_FPS,") of the settings' variables the
   environment had. Without the file only the built-in ones the game always takes (the race
   server). unknown (may be NULL) hears of each line that isn't a setting. 1 when the file is there. */
int dhd_settings_load(const char *path, DhdSettingsGet get, DhdSettingsSet set, DhdSettingsUnknown unknown);

/* Saves key = value into dhd.ini at path: in place of the key's first line, its other lines gone,
   every other line as it was; a new file starts with the crate's header. Key "size" ("1280x960" or
   "monitor") writes width and height. 1 when saved; 0 also when DHD_SETTINGS is none. */
int dhd_settings_save(const char *path, const char *key, const char *value);

/* The value dhd.ini at path gives key, as written, into out (size bytes, cut to fit). 1 when it has
   one. */
int dhd_settings_read(const char *path, const char *key, char *out, size_t size);

/* The install id (finding 161): a random id of 32 lowercase hex digits that dhd.ini at path keeps
   as install_id, made and saved there the first time (without the file: one for this run only),
   into out (size bytes, at least 33). 1 when out has it. Not a setting: never a variable from
   dhd_settings_load, never in dhd.log; the online hello sends it (port/net/client.c). */
int dhd_settings_install_id(const char *path, char *out, size_t size);

/* ---- keyboard and controller bindings (finding 157; port/render/settings/src/controls.rs) ----
   A layout as port/hle/controls_table.h's ControlsLayout (52 bytes): each of the 24 targets' main
   and alternate input (the keyboard: a key's virtual-key code; a controller: an input's id; 0
   nothing), a controller's Swap Sticks and Invert bits. */
typedef struct DhdControlsLayout {
    unsigned char input[24][2];
    unsigned char swap_sticks, invert, unused[2];
} DhdControlsLayout;

/* The keyboard's, Controller 1's and Controller 2's layouts the three values give (dhd.ini
   keyboard, pad1, pad2; NULL or "": the defaults) into out[3]; problem (may be NULL) hears of each
   line that couldn't be read as written, with what the game did instead. */
void dhd_controls_layouts(const char *keyboard, const char *pad1, const char *pad2, DhdControlsLayout out[3],
                          DhdSettingsUnknown problem);

/* One change to device's layout (0 the keyboard, 1 and 2 the controllers) by the bindings' rules
   (an input another button had moves; no player is left without Cross, Triangle, START or a d-pad
   direction): edit 0 assign input to target's slot, 1 clear, 2 reset, 3 swap sticks (input 1 on),
   4 invert (target the bit, input 1 on); kind the controller's PAD_KIND_* (the words). The
   device's new value into value, the player's message into message (cut to fit). -1 refused, 0
   nothing to do, 1 changed. */
int dhd_controls_edit(const char *keyboard, const char *pad1, const char *pad2, int device, int edit, int target, int slot,
                      int input, int kind, char *value, size_t value_size, char *message, size_t message_size);

#ifdef __cplusplus
}
#endif

#endif
