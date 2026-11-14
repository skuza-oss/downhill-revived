/* Android's direct-start update notice (finding 143). The shared launcher checks
   before PLAY; direct starts reach this daily worker when DHD_LAUNCHER_CHECKED is
   absent. phone_update_find_source owns fetching and finding a release. Keep the
   daily stamp, auto_update policy and JNI notice here; nothing is installed. */
#include <SDL3/SDL.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/version.h"
#include "../net/update_check.h"
#include "phone.h"
#include "../render/include/dhd_settings.h"

/* latest.json's key for the APK (package_release.py --platform android). */
#define PLATFORM "android"

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

static int SDLCALL check(void *unused)
{
    char version[32], page[512];
    (void)unused;
    const char *source = NULL;
    int from = phone_update_find_source(PLATFORM, version, sizeof version, page, sizeof page, &source, "update");
    if (from > 0) {
        fprintf(stderr, "update: %s is out (this is %s, said %s): %s\n", version, DHD_VERSION, source, page);
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
