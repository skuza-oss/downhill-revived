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

/* The keys dhd.ini at path holds whose key starts with prefix (without case), joined by commas,
   into out (size bytes, cut to fit). 1 when it found one. The game lists the control profiles with
   it: "bindings_" (finding 141). */
int dhd_settings_keys(const char *path, const char *prefix, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
