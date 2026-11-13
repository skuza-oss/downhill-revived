/* The Android launcher's native calls (finding 225): app.downhillrevived.Native, which the launcher
   activity (LauncherActivity, LauncherBridge) loads from libmain.so before any game runs. Each is
   the shared code the desktop launcher uses too, not a copy in Java:
   - the settings: dhd_settings (port/render/settings) for the page's schema and values, saves and
     the rider name's rule, in the files folder's dhd.ini (the game's own);
   - the disc: host.c's host_disc_check (the boot check) and dhd_disc's online check with the game's
     iso-check.txt (port/render/disc);
   - the update check: phone.c's phone_update_find (port/net/update_check.c's update_find over the
     same sources as the game's own notice, android_update.c, with the HTTPS client of port/net),
     which the iOS launcher calls too (finding 227).
   Nothing here touches SDL or starts the game: loading libmain.so runs only its constructors (the
   renderer's version line, the VU1 register pointer), and SDL_main is the game activity's. */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/version.h"
#include "phone.h"
#include "../render/include/dhd_settings.h"
#include "../render/include/dhd_disc.h"

const char *host_disc_check(const char *path);  /* host.c: NULL when path is the game */

#define EXPORT(type, name) JNIEXPORT type JNICALL Java_app_downhillrevived_Native_##name

/* A Java string's UTF-8 (released by done), or NULL. */
static const char *utf(JNIEnv *env, jstring s) { return s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL; }
static void done(JNIEnv *env, jstring s, const char *c) { if (s && c) (*env)->ReleaseStringUTFChars(env, s, c); }

/* A JSON text from one of dhd_settings' calls that say their whole length. */
static jstring json(JNIEnv *env, size_t (*make)(const char *, char *, size_t), const char *path)
{
    size_t size = 64 * 1024;
    for (int tries = 0; tries < 3; tries++) {
        char *out = malloc(size);
        if (!out) return NULL;
        size_t n = make(path, out, size);
        if (n < size) {
            jstring r = (*env)->NewStringUTF(env, out);
            free(out);
            return r;
        }
        free(out);
        size = n + 1;
    }
    return NULL;
}

static size_t schema(const char *unused, char *out, size_t size) { (void)unused; return dhd_settings_schema_json(out, size); }

/* String schema(): every setting's default, choices and rule (the page's settings_schema). */
EXPORT(jstring, schema)(JNIEnv *env, jclass cls)
{
    (void)cls;
    return json(env, schema, NULL);
}

/* String values(String ini): every setting's value in dhd.ini, else its default. */
EXPORT(jstring, values)(JNIEnv *env, jclass cls, jstring ini)
{
    (void)cls;
    const char *path = utf(env, ini);
    jstring r = path ? json(env, dhd_settings_values_json, path) : NULL;
    done(env, ini, path);
    return r;
}

/* String read(String ini, String key): the value dhd.ini gives key as written, or null. */
EXPORT(jstring, read)(JNIEnv *env, jclass cls, jstring ini, jstring key)
{
    (void)cls;
    const char *path = utf(env, ini), *k = utf(env, key);
    char value[4096];
    int has = path && k && dhd_settings_read(path, k, value, sizeof value);
    done(env, ini, path);
    done(env, key, k);
    return has ? (*env)->NewStringUTF(env, value) : NULL;
}

/* boolean save(String ini, String key, String value): key = value in dhd.ini, the other lines kept. */
EXPORT(jboolean, save)(JNIEnv *env, jclass cls, jstring ini, jstring key, jstring value)
{
    (void)cls;
    const char *path = utf(env, ini), *k = utf(env, key), *v = utf(env, value);
    int ok = path && k && v && dhd_settings_save(path, k, v);
    done(env, ini, path);
    done(env, key, k);
    done(env, value, v);
    return ok ? JNI_TRUE : JNI_FALSE;
}

/* String riderName(String typed): the rider name saved for it ("" none). */
EXPORT(jstring, riderName)(JNIEnv *env, jclass cls, jstring typed)
{
    (void)cls;
    const char *t = utf(env, typed);
    char name[64] = "";
    if (t) dhd_settings_rider_name(t, name, sizeof name);
    done(env, typed, t);
    return (*env)->NewStringUTF(env, name);
}

/* String discProblem(String path): null when path is Downhill Domination (USA), else why not. */
EXPORT(jstring, discProblem)(JNIEnv *env, jclass cls, jstring path)
{
    (void)cls;
    const char *p = utf(env, path);
    const char *problem = p ? host_disc_check(p) : "No disc image.";
    jstring r = problem ? (*env)->NewStringUTF(env, problem) : NULL;
    done(env, path, p);
    return r;
}

/* String onlineCheck(String iso, String cache): dhd_disc_check's answer as "<code>\n<note>": 1 or 2
   the USA disc's game files are known (note "" for them, else what differs), negative none. */
EXPORT(jstring, onlineCheck)(JNIEnv *env, jclass cls, jstring iso, jstring cache)
{
    (void)cls;
    const char *i = utf(env, iso), *c = utf(env, cache);
    unsigned char digest[32];
    char note[512] = "", out[600];
    int code = i ? dhd_disc_check(i, c, digest, note, sizeof note, NULL, 0) : -1;
    snprintf(out, sizeof out, "%d\n%s", code, note);
    done(env, iso, i);
    done(env, cache, c);
    return (*env)->NewStringUTF(env, out);
}

/* String update(): "<version>\n<page>" when a newer release has an Android build (update_find over
   the game's own sources and rules: android_update.c, update_check.c), "" when none does, null when
   no source answered. */
EXPORT(jstring, update)(JNIEnv *env, jclass cls)
{
    (void)cls;
    char version[32], page[512], out[600];
    int found = phone_update_find("android", version, sizeof version, page, sizeof page);
    if (found > 0) {
        snprintf(out, sizeof out, "%s\n%s", version, page);
        return (*env)->NewStringUTF(env, out);
    }
    return found == 0 ? (*env)->NewStringUTF(env, "") : NULL;
}

/* String version(): this build's version (version.h). */
EXPORT(jstring, version)(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, DHD_VERSION);
}
