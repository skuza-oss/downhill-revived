/* ios_main.h - Native iOS Platform Host Header for Downhill Revived
   Provides sandbox paths, lifecycle management, ISO validation, and game thread startup.
   GPL-3.0-or-later. */
#ifndef DHD_IOS_MAIN_H
#define DHD_IOS_MAIN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Retrieve the app sandbox Documents directory path with trailing slash */
const char *ios_documents_dir(void);

/* Retrieve the path to dhd.ini inside the app sandbox */
const char *ios_settings_path(void);

/* Save or read a setting in Documents/dhd.ini */
int ios_setting_save(const char *key, const char *value);
int ios_setting_read(const char *key, char *out, int out_size);

/* Retrieve the path to the sandboxed ISO file (Documents/SCUS-97177.iso) */
const char *ios_iso_path(void);

/* Check if the sandboxed ISO exists and is valid */
bool ios_disc_is_ready(void);

/* Auto-detect any valid Downhill Domination ISO inside the Documents directory */
bool ios_disc_auto_detect(void);

/* Retrieve details of the currently recognized disc (filename, size in bytes) */
bool ios_disc_get_info(char *name_buf, size_t name_size, uint64_t *size_bytes);

/* Remove the currently active ISO from sandbox */
bool ios_disc_delete(void);

/* Validate an ISO file at a given filesystem path */
const char *ios_disc_validate(const char *iso_path);

/* Import and copy an ISO from a security-scoped URL (UIDocumentPickerViewController) */
typedef void (*iOSImportProgressCallback)(double progress, uint64_t bytes_copied, uint64_t total_bytes);
bool ios_disc_import_from_url(const char *source_url_path, iOSImportProgressCallback progress_cb, char *err_msg, size_t err_size);

/* Query available free storage on the device in bytes */
uint64_t ios_available_disk_space(void);

/* Check if the game session is actively running */
bool ios_is_game_running(void);
bool ios_game_is_active(void);

/* Launch Downhill Revived game core on background game thread */
int ios_start_game(int argc, char **argv);

/* Request game exit and shutdown */
void ios_request_exit(void);

/* Lifecycle notifications (called by AppDelegate / SceneDelegate) */
void ios_lifecycle_will_resign_active(void);
void ios_lifecycle_did_enter_background(void);
void ios_lifecycle_will_enter_foreground(void);
void ios_lifecycle_did_become_active(void);
void ios_lifecycle_memory_warning(void);

#ifdef __cplusplus
}
#endif

#endif /* DHD_IOS_MAIN_H */

