/* The update notice on Android (finding 143). A phone has no launcher to update the game, so at
   start the game itself asks whether a newer release with an Android build is out, and says so in
   one line over the game (DhdActivity.updateNotice) that opens the release's page in the browser
   when tapped. Nothing is downloaded or installed: the player installs the new APK as the first.

   - At most once a day (update-check.txt in the app's files folder holds when the last check
     started), never when dhd.ini says auto_update = off.
   - On a thread of its own, so the start never waits: the online server's copy of the newest
     latest.json first (GET /v1/latest, online/server/src/latest.ts), then GitHub's release asset
     (latest.json, through its redirect) whenever the server names no newer release: when it
     doesn't answer, and also when its answer is older or stale (it keeps its last good copy
     while it can't reach GitHub), so a stale copy never hides a release (update_find). The
     launcher asks GitHub's API first (update.rs); a phone network shares one address among many,
     and the API allows an address 60 checks an hour, so the phone starts with the server; the
     second request is once a day at most. Each GET gives up after
     the HTTPS client's timeouts (5 s to connect, 10 s without data); offline, the game just plays.
   - The rules (the version compare, the manifest, the day) are port/net/update_check.c's. */
#include <SDL3/SDL.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/version.h"
#include "../net/update_check.h"
#include "../net/ws.h"
#include "../render/include/dhd_settings.h"

/* latest.json's key for the APK (package_release.py --platform android). */
#define PLATFORM "android"
static const char *const sources[] = {UPDATE_SOURCE_SERVER, UPDATE_SOURCE_GITHUB};

static char ini[1200], stamp[1200];

static long long last_check(void)
{
    long long when = 0;
    FILE *f = fopen(stamp, "r");
    if (f) {
        if (fscanf(f, "%lld", &when) != 1) when = 0;
        fclose(f);
    }
    return when;
}

static void note_check(long long now)
{
    FILE *f = fopen(stamp, "w");
    if (f) {
        fprintf(f, "%lld\n", now);
        fclose(f);
    }
}

/* DhdActivity.updateNotice(version, page), on the UI thread there. */
static void show_notice(const char *version, const char *page)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!activity) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID m = cls ? (*env)->GetMethodID(env, cls, "updateNotice", "(Ljava/lang/String;Ljava/lang/String;)V") : NULL;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); m = NULL; }
    if (m) {
        jstring v = (*env)->NewStringUTF(env, version), p = (*env)->NewStringUTF(env, page);
        if (v && p) (*env)->CallVoidMethod(env, activity, m, v, p);
        if (v) (*env)->DeleteLocalRef(env, v);
        if (p) (*env)->DeleteLocalRef(env, p);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, activity);
}

/* update_find's fetch: one GET, logged when it fails. */
static const char *fetch(void *context, const char *url)
{
    static char body[64 * 1024];
    (void)context;
    if (http_get(url, body, sizeof body) < 0) {
        fprintf(stderr, "update: %s didn't answer\n", url);
        return NULL;
    }
    return body;
}

static int SDLCALL check(void *unused)
{
    char version[32], page[512];
    (void)unused;
    int from = update_find(fetch, NULL, sources, sizeof sources / sizeof *sources, PLATFORM, DHD_VERSION,
                           version, sizeof version, page, sizeof page);
    if (from) {
        fprintf(stderr, "update: %s is out (this is %s, said %s): %s\n", version, DHD_VERSION, sources[from - 1], page);
        show_notice(version, page);
    } else {
        fprintf(stderr, "update: no newer Android release than %s\n", DHD_VERSION);
    }
    return 0;
}

void android_update_start(const char *files)
{
    char auto_update[16] = "";
    long long now = (long long)time(NULL);
    snprintf(ini, sizeof ini, "%sdhd.ini", files);
    snprintf(stamp, sizeof stamp, "%supdate-check.txt", files);
    dhd_settings_read(ini, "auto_update", auto_update, sizeof auto_update);
    if (!update_due(auto_update, now, last_check())) return;
    note_check(now);
    SDL_Thread *t = SDL_CreateThread(check, "update check", NULL);
    if (t) SDL_DetachThread(t);
}
