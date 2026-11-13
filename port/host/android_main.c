/* The Android start (finding 091). SDL's Java activity (port/android: DhdActivity over SDL's
   SDLActivity) loads libSDL3.so and libmain.so, then calls SDL_main here on its SDL thread. That
   thread becomes SDL's loop (port/host/sdl_host.c) and the game gets a thread of its own, as on
   every platform, once the game's own main runs (port/host64/launcher.c, compiled as
   dhd_launcher_main). Before it:
   - the log (stdout, stderr) goes to logcat (tag DownhillRevived) and to dhd.log (host.c's
     host_console_init, the log of every platform, started here before anything prints; the one
     before becomes dhd-previous.log), which first says how the run before ended (phone_quote_last_exit);
   - the app's files folder (internal storage, SDL_GetAndroidInternalStoragePath) stands for the
     folder of dhd.exe: host_exe_path names dhd64 there, so dhd.ini, saves, dhd.log and the disc
     image's copy live there;
   - the disc image: SCUS-97177.iso in that folder. The launcher (port/launcher's page in
     LauncherActivity, finding 225) asks for the player's .iso, checks it and the room for it, and
     copies it there before the game starts. Here it is checked again; when it is missing or not the
     game, the game ends (DhdActivity checks it before SDL starts and opens the launcher instead), so
     the disc has one place to be chosen;
   - a new dhd.ini names the copy (iso); the player defaults (the native renderer at 2x,
     widescreen, movies skipped) come from host.c as on Windows;
   - the update notice: when the launcher didn't show the update line this start (a direct start,
     DHD_LAUNCHER_CHECKED unset), once a day at most (not with auto_update = off) a thread asks
     whether a newer release has an APK and says so over the game (android_update.c, finding 143).
     An APK is updated by installing the new one; nothing is downloaded here;
   - SDL hints: landscape only, the Back button kept from closing the game.
   The game ends with host_end (the close of the activity, gs_pump_messages, on the game's thread:
   the work threads and the render thread stop, then the process leaves without the C library's
   exit handlers and static destructors, finding 210); SDL_main never returns to a second start in
   the same process. */
#include <SDL3/SDL.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../render/include/dhd_settings.h"
#include "host_end.h"
#include "phone.h"

#define TAG "DownhillRevived"
#define DISC_NAME "SCUS-97177.iso"

int dhd_launcher_main(int argc, char **argv);   /* launcher.c's main */
void host_console_init(void);                   /* host.c: the log */
void android_update_start(const char *files);   /* android_update.c: the update notice */
const char *host_disc_check(const char *path);  /* host.c: NULL when path is the game */

static char files[1024];      /* the app's files folder, with a trailing slash */
static char exe[1100];        /* files + "dhd64": what host_exe_path answers */

/* host_posix.c's host_exe_path on Android. */
const char *android_exe_path(void) { return exe; }

/* ---- the disc image ---- */

/* The copy of the disc image in the files folder, which the launcher made (DhdActivity checks it
   before the game starts and opens the launcher without it); when it is missing or not the game
   here all the same, a copy that isn't the game is removed and 0 is returned: the game ends, and the
   next start's launcher asks for the disc. */
static int disc_ready(const char *path)
{
    struct stat st;
    const char *problem = stat(path, &st) == 0 ? host_disc_check(path) : "none";
    if (!problem) return 1;
    if (strcmp(problem, "none")) {
        fprintf(stderr, "android: the disc image's copy isn't the game (%s); removed\n", problem);
        unlink(path);
    }
    fprintf(stderr, "android: no disc image; the game ends, the launcher asks for it at the next start\n");
    return 0;
}

/* dhd.ini names the copy of the disc (host_disc_ready checks it in the core; DHD_ISO would skip
   that check): a dhd.ini without a disc gets it (dhd_settings, which starts a missing file). The
   player defaults apply as soon as dhd.ini exists (host_settings_load). */
static void settings_start(const char *disc)
{
    char path[1200], iso[8];
    snprintf(path, sizeof path, "%sdhd.ini", files);
    if (!dhd_settings_read(path, "iso", iso, sizeof iso)) dhd_settings_save(path, "iso", disc);
}

__attribute__((visibility("default"))) int SDL_main(int argc, char **argv)
{
    const char *storage = SDL_GetAndroidInternalStoragePath();
    if (!storage) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "no internal storage (%s)", SDL_GetError());
        return 1;
    }
    snprintf(files, sizeof files, "%s/", storage);
    snprintf(exe, sizeof exe, "%sdhd64", files);
    host_console_init();
    fprintf(stderr, "android: Downhill Revived in %s\n", files);
    phone_quote_last_exit(files);  /* LastExit.java's dhd-exit.txt (finding 152) */
    {   /* a profile-collecting build (build_host64.py --target android-arm64 --pgo-generate): its counters go to the
           files folder (host_exit writes them; the process ends with _exit, so no atexit runs) */
        extern void __llvm_profile_set_filename(const char *) __attribute__((weak));
        static char profile[1100];
        snprintf(profile, sizeof profile, "%sdhd-%%p.profraw", files);
        if (__llvm_profile_set_filename) __llvm_profile_set_filename(profile);
    }
    phone_settings_rows();  /* SETTINGS: a phone's rows (phone.c, the same list as iOS) */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    char disc[1200];
    snprintf(disc, sizeof disc, "%s" DISC_NAME, files);
    if (!disc_ready(disc)) {
        host_end(0);
    }
    settings_start(disc);
    if (!getenv("DHD_LAUNCHER_CHECKED")) android_update_start(files);  /* the launcher's line said it */
    char *args[] = {exe, NULL};
    (void)argc; (void)argv;
    int code = dhd_launcher_main(1, args);
    fprintf(stderr, "android: the game ended (%d)\n", code);
    host_end(code);
}
