/* ios_main.m - the iOS app's host side (ios_main.h; findings 224, 227).
   GPL-3.0-or-later. */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>
#include "ios_main.h"
#include "metal_bridge.h"
#include "run_record.h"
#include "launcher_calls.h"
#include "host/phone.h"   /* port/, the project's header search path */

/* From libdhd_ios.a */
extern void host_console_init(void);
extern int dhd_settings_save(const char *path, const char *key, const char *value);
extern int dhd_settings_read(const char *path, const char *key, char *out, size_t size);
extern int dhd_main(int argc, char **argv);
extern const char *host_disc_check(const char *path);
extern void host_end_add(int stage, const char *name, void (*fn)(void));
extern int host_end_code(void);
enum { HOST_END_LOG_STAGE = 4 };   /* host_end.h's HOST_END_LOG */

static char s_documents_dir[1024];
static char s_settings_path[1100];
static _Atomic(bool) s_game_started = false;
static _Atomic(bool) s_game_active = true;
static pthread_once_t s_directories_once = PTHREAD_ONCE_INIT;

static void initialize_directories(void) {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject
                     ?: NSTemporaryDirectory();
    snprintf(s_documents_dir, sizeof s_documents_dir, "%s/", docs.fileSystemRepresentation);
    snprintf(s_settings_path, sizeof s_settings_path, "%sdhd.ini", s_documents_dir);
    char saves[1200];
    snprintf(saves, sizeof saves, "%ssaves/card1", s_documents_dir);
    [NSFileManager.defaultManager createDirectoryAtPath:[NSString stringWithUTF8String:saves]
                            withIntermediateDirectories:YES attributes:nil error:nil];
}

const char *ios_documents_dir(void) { pthread_once(&s_directories_once, initialize_directories); return s_documents_dir; }
const char *ios_settings_path(void) { pthread_once(&s_directories_once, initialize_directories); return s_settings_path; }

int ios_setting_read(const char *key, char *out, int out_size) {
    if (!key || !out || out_size <= 0) return 0;
    return dhd_settings_read(ios_settings_path(), key, out, (size_t)out_size);
}

uint64_t ios_available_disk_space(void) {
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:ios_documents_dir()]];
    NSDictionary *r = [url resourceValuesForKeys:@[NSURLVolumeAvailableCapacityForImportantUsageKey] error:nil];
    NSNumber *n = r[NSURLVolumeAvailableCapacityForImportantUsageKey];
    if (n) return n.unsignedLongLongValue;
    NSDictionary *a = [NSFileManager.defaultManager attributesOfFileSystemForPath:url.path error:nil];
    return [a[NSFileSystemFreeSize] unsignedLongLongValue];
}

/* ---- the run record (run_record.h): Documents/dhd-run.txt at each change of state ---- */

static void marker_write(int state, int code) {
    char path[1200], text[128];
    RunMarker m = {state, (long long)time(NULL), code};
    run_marker_format(&m, text, sizeof text);
    snprintf(path, sizeof path, "%sdhd-run.txt", ios_documents_dir());
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

/* host_end's last stage (the log's): the game ended itself, with its code. */
static void marker_ended(void) { marker_write(RUN_ENDED, host_end_code()); }

/* A file's last max bytes as text, malloc'd (NULL: none); *mtime its last write. */
static char *file_tail(const char *path, long max, long long *mtime) {
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    *mtime = (long long)st.st_mtime;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    long size = (long)st.st_size, n = size < max ? size : max;
    char *text = malloc((size_t)n + 1);
    if (text) {
        fseek(f, size - n, SEEK_SET);
        n = (long)fread(text, 1, (size_t)n, f);
        text[n] = 0;
        for (long i = 0; i < n; i++) if (!text[i]) text[i] = ' ';   /* a cut line, binary garbage */
    }
    fclose(f);
    return text;
}

const char *ios_run_record_start(void) {
    char path[1200], exit_path[1200];
    long long log_time = 0, unused;
    snprintf(path, sizeof path, "%sdhd-run.txt", ios_documents_dir());
    char *marker_text = file_tail(path, 4096, &unused);
    RunMarker m;
    run_marker_parse(marker_text, &m);
    free(marker_text);
    snprintf(path, sizeof path, "%sdhd.log", ios_documents_dir());
    char *log = file_tail(path, 256 << 10, &log_time);
    static char text[2048];
    const char *why = run_judge(m.state == RUN_NONE ? NULL : &m, log, log_time, text, sizeof text);
    free(log);
    snprintf(exit_path, sizeof exit_path, "%sdhd-exit.txt", ios_documents_dir());
    if (text[0]) {
        FILE *f = fopen(exit_path, "w");
        if (f) { fputs(text, f); fclose(f); }
    } else {
        remove(exit_path);
    }
    NSLog(@"Downhill Revived: the run before: %s", text[0] ? text : "none\n");
    marker_write(RUN_LAUNCHER, 0);
    return why;
}

/* ---- the game ---- */

/* dhd.ini names this run's disc copy (in the settings' order, after the launcher's saves). */
static void correct_disc_path(void *disc) {
    char named[1200];
    if (!dhd_settings_read(s_settings_path, "iso", named, sizeof named) || strcmp(named, disc))
        dhd_settings_save(s_settings_path, "iso", disc);
}

int ios_start_game(int edit_touch_layout, int checked) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_game_started, &expected, true)) return 4;
    const char *dir = ios_documents_dir();
    char disc[1200];
    snprintf(disc, sizeof disc, "%sSCUS-97177.iso", dir);
    struct stat st;
    if (stat(disc, &st) != 0 || host_disc_check(disc)) {   /* the launcher asks for it */
        atomic_store(&s_game_started, false);
        return 3;
    }
    /* dhd.ini names the copy (host_disc_ready checks it in the core; DHD_ISO would skip that). The
       app's folder moves when iOS installs an update (a new container), so the name is the copy's
       path of this run, written again whenever it differs (finding 227) */
    launcher_settings_run(correct_disc_path, disc);   /* after the launcher's last save (launcher_calls.h) */
    setenv("DHD_SETTINGS", s_settings_path, 1);
    setenv("DHD_DATA_DIR", dir, 1);
    setenv("DHD_IOS_DISPLAY_AUTO", "1", 1);
    if (edit_touch_layout) setenv("DHD_TOUCH_EDIT_AT_START", "1", 1);
    if (checked) setenv("DHD_LAUNCHER_CHECKED", "1", 1);
    phone_settings_rows();   /* SETTINGS: Android's rows (phone.c) */
    marker_write(atomic_load(&s_game_active) ? RUN_GAME : RUN_INACTIVE, 0);
    host_end_add(HOST_END_LOG_STAGE, "run record", marker_ended);
    ios_metal_set_game_active(atomic_load(&s_game_active));
    host_console_init();
    fprintf(stderr, "ios: Downhill Revived in the app's Documents folder\n");
    phone_quote_last_exit(dir);   /* run_record.c's dhd-exit.txt, written at the app's start */
    char *argv[] = {(char *)"dhd64", NULL};
    int rc = dhd_main(1, argv);
    atomic_store(&s_game_started, false);
    ios_metal_set_game_active(false);
    return rc ? rc : 5;
}

bool ios_is_game_running(void) { return atomic_load(&s_game_started); }
bool ios_game_is_active(void) { return atomic_load(&s_game_active); }

/* The game stopped for the app's absence (present.c's app_away_wait): its drawing reached the GPU
   and ended, and the surface is gone, so nothing is submitted or presented any more. UIKit's
   callback waits for that, bounded, before it returns and iOS suspends the app (iOS ends an app
   that draws in the background). The game thread gets there at its next field without the main
   thread (nothing on its way waits for it), so the wait can't deadlock; a long load that doesn't
   reach a field in time is logged and the game stops at its next field (finding 227). */
extern int gs_app_stopped(void);   /* present.c */
static void wait_for_the_game(const char *why) {
    if (!atomic_load(&s_game_started)) return;
    enum { MOST_MS = 1500 };
    double start = CACurrentMediaTime();
    int ms = 0;
    while (!gs_app_stopped() && ms < MOST_MS) {
        usleep(2000);
        ms = (int)((CACurrentMediaTime() - start) * 1000);
    }
    if (gs_app_stopped())
        fprintf(stderr, "ios: %s: the game and its renderer stopped in %d ms\n", why, ms);
    else
        fprintf(stderr, "ios: %s: the game hasn't stopped after %d ms (a load); it stops at its next field\n", why, ms);
}

/* The game waits while the app is not active (present.c's app_away_wait, finding 227); the marker
   says where it was, for the next start's record. */
void ios_lifecycle_will_resign_active(void) {
    atomic_store(&s_game_active, false);
    ios_metal_set_game_active(false);   /* also lets go of every finger (metal_bridge.m) */
    if (atomic_load(&s_game_started)) marker_write(RUN_INACTIVE, 0);
    wait_for_the_game("off the screen");
}
void ios_lifecycle_did_enter_background(void) {
    atomic_store(&s_game_active, false);
    ios_metal_set_game_active(false);
    if (atomic_load(&s_game_started)) marker_write(RUN_BACKGROUND, 0);
    wait_for_the_game("in the background");
}
void ios_lifecycle_will_enter_foreground(void) {}
void ios_lifecycle_did_become_active(void) {
    atomic_store(&s_game_active, true);
    bool running = atomic_load(&s_game_started);
    ios_metal_set_game_active(running);
    if (running) marker_write(RUN_GAME, 0);
}
