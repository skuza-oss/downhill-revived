/* The Android start (finding 091). SDL's Java activity (port/android: DhdActivity over SDL's
   SDLActivity) loads libSDL3.so and libmain.so, then calls SDL_main here on its SDL thread. That
   thread becomes SDL's loop (port/host/sdl_host.c) and the game gets a thread of its own, as on
   every platform, once the game's own main runs (port/host64/launcher.c, compiled as
   dhd_launcher_main). Before it:
   - the log (stdout, stderr) goes to logcat (tag DownhillRevived) and to dhd.log (host.c's
     host_console_init, the log of every platform, started here before anything prints; the one
     before becomes dhd-previous.log), which first says how the run before ended (quote_last_exit);
   - the app's files folder (internal storage, SDL_GetAndroidInternalStoragePath) stands for the
     folder of dhd.exe: host_exe_path names dhd64 there, so dhd.ini, saves, dhd.log and the disc
     image's copy live there;
   - the disc image: SCUS-97177.iso in that folder. The first start's screen (port/android's
     SetupActivity.java, finding 143) asks for the player's .iso, checks it and the room for it, and
     copies it there before the game starts. Here it is checked again; when it is missing or not the
     game (a start without that screen), the player picks it through Android's document picker
     (SDL's file dialog) and it is copied here (the content URI is read once; Android gives no path);
   - a new dhd.ini names the copy (iso); the player defaults (the native renderer at 2x,
     widescreen, movies skipped) come from host.c as on Windows;
   - the update notice: once a day at most (not with auto_update = off) a thread asks whether a
     newer release has an APK and says so over the game (android_update.c, finding 143). An APK is
     updated by installing the new one; nothing is downloaded here;
   - SDL hints: landscape only, the Back button kept from closing the game.
   The game ends with exit() (the close of the activity, gs_pump_messages, on the game's thread);
   SDL_main never returns to a second start in the same process. */
#include <SDL3/SDL.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../render/include/dhd_settings.h"

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

static SDL_Semaphore *picked;
static char chosen[4096];

static void SDLCALL on_pick(void *userdata, const char *const *list, int filter)
{
    (void)userdata; (void)filter;
    chosen[0] = 0;
    if (list && list[0]) snprintf(chosen, sizeof chosen, "%s", list[0]);
    SDL_SignalSemaphore(picked);
}

/* The document the player picks (a content:// URI), or "" when cancelled. */
static const char *pick(void)
{
    if (!picked) picked = SDL_CreateSemaphore(0);
    SDL_ShowOpenFileDialog(on_pick, NULL, NULL, NULL, 0, NULL, false);  /* no filter: any file */
    SDL_WaitSemaphore(picked);
    return chosen;
}

/* Copies the document at uri to path (through a .part file); 1 when done. */
static int copy_disc(const char *uri, const char *path)
{
    char part[1300];
    snprintf(part, sizeof part, "%s.part", path);
    SDL_IOStream *in = SDL_IOFromFile(uri, "rb");
    if (!in) { fprintf(stderr, "android: %s can't be opened (%s)\n", uri, SDL_GetError()); return 0; }
    FILE *out = fopen(part, "wb");
    if (!out) { SDL_CloseIO(in); fprintf(stderr, "android: %s can't be written\n", part); return 0; }
    static char block[1 << 20];
    uint64_t total = 0, next = 0;
    size_t n;
    int ok = 1;
    while ((n = SDL_ReadIO(in, block, sizeof block)) > 0) {
        if (fwrite(block, 1, n, out) != n) { ok = 0; break; }
        total += n;
        if (total >= next) { fprintf(stderr, "android: copying the disc image, %llu MB\n", (unsigned long long)(total >> 20)); next += 256u << 20; }
    }
    if (SDL_GetIOStatus(in) == SDL_IO_STATUS_ERROR) ok = 0;
    SDL_CloseIO(in);
    ok &= fclose(out) == 0;
    if (ok && total && rename(part, path) == 0) {
        fprintf(stderr, "android: disc image copied, %llu MB\n", (unsigned long long)(total >> 20));
        return 1;
    }
    unlink(part);
    return 0;
}

/* The copy of the disc image in the files folder, made on the first start; 0 if the player gave up. */
static int disc_ready(const char *path)
{
    struct stat st;
    const char *problem = stat(path, &st) == 0 ? host_disc_check(path) : "none";
    if (!problem) return 1;
    if (strcmp(problem, "none")) unlink(path);  /* not the game: ask again */
    char message[1024];
    snprintf(message, sizeof message, "%s%sChoose your Downhill Domination (USA) disc image: the game plays from "
             "your own copy of the disc, an .iso file (SCUS-97177). It is copied into the app once (a few GB "
             "of storage; this takes a minute or two with nothing on the screen).",
             strcmp(problem, "none") ? problem : "", strcmp(problem, "none") ? "\n\n" : "");
    for (;;) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Downhill Revived", message, NULL);
        const char *uri = pick();
        if (!*uri) return 0;
        fprintf(stderr, "android: disc image chosen: %s\n", uri);
        if (!copy_disc(uri, path)) {
            snprintf(message, sizeof message, "The file couldn't be copied (is there room on the phone?).\n\n"
                     "Choose your Downhill Domination (USA) disc image (SCUS-97177).");
            continue;
        }
        if (!(problem = host_disc_check(path))) return 1;
        unlink(path);
        snprintf(message, sizeof message, "%s\n\nChoose your Downhill Domination (USA) disc image (SCUS-97177).", problem);
    }
}

/* How the run before ended, as Android recorded it (the reason, and a native crash's tombstone in
   short): the app's Java side writes dhd-exit.txt before the game starts (LastExit.java, finding
   152); each of its lines goes into the new dhd.log as "previous run: ...", so a log always says how
   the one before it (now dhd-previous.log) ended. */
static void quote_last_exit(void)
{
    char path[1200], line[600];
    snprintf(path, sizeof path, "%sdhd-exit.txt", files);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        fprintf(stderr, "previous run: %s\n", line);
    }
    fclose(f);
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
    quote_last_exit();
    {   /* a profile-collecting build (build_host64.py --target android-arm64 --pgo-generate): its counters go to the
           files folder (host_exit writes them; the process ends with _exit, so no atexit runs) */
        extern void __llvm_profile_set_filename(const char *) __attribute__((weak));
        static char profile[1100];
        snprintf(profile, sizeof profile, "%sdhd-%%p.profraw", files);
        if (__llvm_profile_set_filename) __llvm_profile_set_filename(profile);
    }
    /* SETTINGS without the window rows (a phone has one screen), which makes room for Touch
       Controls (finding 141; the popup holds 16 rows) */
    setenv("DHD_PC_SETTINGS_ROWS", "DHD_SCALE,DHD_ASPECT,DHD_PRESENT_FILTER,DHD_WIDESCREEN,DHD_SKIP_MOVIES,DHD_VIBRATION,"
           "DHD_STICK_DEAD_ZONE,DHD_PROMPTS,DHD_TEXTURES,DHD_FONT,DHD_FPS,DHD_VSYNC,DHD_PERF_COUNTER,DHD_UNLOCK_ALL", 0);
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    char disc[1200];
    snprintf(disc, sizeof disc, "%s" DISC_NAME, files);
    if (!disc_ready(disc)) {
        fprintf(stderr, "android: no disc image; the game ends\n");
        exit(0);
    }
    settings_start(disc);
    android_update_start(files);
    char *args[] = {exe, NULL};
    (void)argc; (void)argv;
    int code = dhd_launcher_main(1, args);
    fprintf(stderr, "android: the game ended (%d)\n", code);
    exit(code);
}
