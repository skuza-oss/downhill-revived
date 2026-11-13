/* ios_main.h - the iOS app's host side: the app's folders and settings, the game's start on its
   own thread, the app's lifecycle and the run record (findings 224, 227).
   GPL-3.0-or-later. */
#ifndef DHD_IOS_MAIN_H
#define DHD_IOS_MAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The app's Documents folder with a trailing slash: it stands for the executable's folder
   (dhd.ini, dhd.log, the saves, the disc copy SCUS-97177.iso; visible in the Files app). */
const char *ios_documents_dir(void);
/* Documents/dhd.ini */
const char *ios_settings_path(void);
int ios_setting_read(const char *key, char *out, int out_size);
/* Free room on the app's volume, bytes. */
uint64_t ios_available_disk_space(void);

/* At the app's start, before anything else: how the run before ended (run_record.h) into
   dhd-exit.txt, for the game's "previous run:" lines; returns the launcher's few words when it
   closed unexpectedly, else NULL. Marks this run as in the launcher. */
const char *ios_run_record_start(void);

/* Starts the game on the calling thread (the app's game thread): dhd_main with the app's folders.
   edit_touch_layout: the launcher's "Edit the touch layout"; checked: the launcher showed the
   update line. Returns only when the game could not start (nonzero); the game ends the process
   itself (host_end, finding 210). */
int ios_start_game(int edit_touch_layout, int checked);
bool ios_is_game_running(void);
bool ios_game_is_active(void);

/* Lifecycle notifications (the app delegate). */
void ios_lifecycle_will_resign_active(void);
void ios_lifecycle_did_enter_background(void);
void ios_lifecycle_will_enter_foreground(void);
void ios_lifecycle_did_become_active(void);

#ifdef __cplusplus
}
#endif

#endif /* DHD_IOS_MAIN_H */
