/* What the two phone hosts share (Android: android_main.c, android_launcher.c; iOS:
   port/ios/host/ios_main.m, port/ios/app/launcher.m; findings 225, 227): one list, one wording,
   one update check, so a phone is a phone whichever system it runs. GPL-3.0-or-later. */
#ifndef PORT_HOST_PHONE_H
#define PORT_HOST_PHONE_H

#include <stddef.h>

/* SETTINGS on a phone: the rows a phone can do, without the window rows (a phone has one screen),
   which makes room for Touch Controls (finding 141; the popup holds 16 rows). pc_settings.c reads
   it as DHD_PC_SETTINGS_ROWS. */
extern const char PHONE_SETTINGS_ROWS[];
/* Sets DHD_PC_SETTINGS_ROWS to PHONE_SETTINGS_ROWS (unless the environment already names rows). */
void phone_settings_rows(void);

/* How the run before ended: dir's dhd-exit.txt (Android: LastExit.java; iOS: run_record.c), each
   of its lines into the new dhd.log as "previous run: ...", so a log always says how the one before
   it (now dhd-previous.log) ended. dir ends with a separator. */
void phone_quote_last_exit(const char *dir);

/* The launcher's update line (BRIDGE.md, check_update): update_find over the game's own sources
   (update_check.h) for platform's key in latest.json. 1 with version and page when a newer
   release has a build for platform, 0 when none does, -1 when no source answered. */
int phone_update_find(const char *platform, char *version, size_t version_size, char *page, size_t page_size);

#endif
