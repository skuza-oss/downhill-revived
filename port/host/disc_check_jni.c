/* Android's DiscImage natives (findings 234, 242): the shared boot-disc check over a seekable Java
   document (positional FileChannel reads), and the phones' import rules (disc_import.c) the iOS
   launcher calls directly. Compiled into libmain.so and, for port/android/test_disc_image.py, into
   a desktop JVM's library. GPL-3.0-or-later. */
#include <jni.h>
#include <stdio.h>
#include "disc_check.h"
#include "disc_import.h"

#define EXPORT(type, name) JNIEXPORT type JNICALL Java_app_downhillrevived_DiscImage_##name

typedef struct { JNIEnv *env; jclass cls; jobject file; jmethodID read; } Reader;
static int read_at(void *context, uint64_t offset, void *buffer, size_t n)
{
    Reader *r = context;
    JNIEnv *env = r->env;
    if ((*env)->ExceptionCheck(env)) return 0;
    jobject bytes = (*env)->NewDirectByteBuffer(env, buffer, (jlong)n);
    if (!bytes) return 0;
    jboolean ok = (*env)->CallStaticBooleanMethod(env, r->cls, r->read, r->file, (jlong)offset, bytes);
    (*env)->DeleteLocalRef(env, bytes);
    return !(*env)->ExceptionCheck(env) && ok;
}

/* "code\nid\nvolume": disc_check_image's kind, another game's boot file, the declared volume. */
EXPORT(jstring, checkNative)(JNIEnv *env, jclass cls, jobject file, jlong size)
{
    jmethodID method = (*env)->GetStaticMethodID(env, cls, "readAt", "(Ljava/nio/channels/FileChannel;JLjava/nio/ByteBuffer;)Z");
    if (!method) return NULL;
    Reader reader = {env, cls, file, method};
    char id[32], out[96];
    uint64_t volume = 0;
    int code = disc_check_image(read_at, &reader, (uint64_t)size, id, sizeof id, &volume);
    if ((*env)->ExceptionCheck(env)) return NULL;
    snprintf(out, sizeof out, "%d\n%s\n%llu", code, id, (unsigned long long)volume);
    return (*env)->NewStringUTF(env, out);
}

/* disc_import.h's constants: block, progress step, margin, low. */
EXPORT(jlongArray, rules)(JNIEnv *env, jclass cls)
{
    (void)cls;
    jlong values[4] = {DISC_COPY_BLOCK, (jlong)DISC_COPY_STEP, (jlong)DISC_COPY_MARGIN, (jlong)DISC_COPY_LOW};
    jlongArray a = (*env)->NewLongArray(env, 4);
    if (a) (*env)->SetLongArrayRegion(env, a, 0, 4, values);
    return a;
}

EXPORT(jlong, roomNeeded)(JNIEnv *env, jclass cls, jlong size, jlong free_bytes)
{
    (void)env; (void)cls;
    return (jlong)disc_room_needed(size > 0 ? (uint64_t)size : 0, free_bytes > 0 ? (uint64_t)free_bytes : 0);
}

EXPORT(jstring, verdictNative)(JNIEnv *env, jclass cls, jint kind, jlong copied, jlong size, jboolean exact, jlong volume)
{
    (void)cls;
    char why[400];
    const char *bad = disc_copy_verdict(kind, copied > 0 ? (uint64_t)copied : 0, size > 0 ? (uint64_t)size : 0,
                                        exact ? 1 : 0, volume > 0 ? (uint64_t)volume : 0, why, sizeof why);
    return bad ? (*env)->NewStringUTF(env, bad) : NULL;
}

EXPORT(jlong, publishBegin)(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jlong)disc_publish_begin();
}

EXPORT(jint, publish)(JNIEnv *env, jclass cls, jlong generation, jstring from, jstring to)
{
    (void)cls;
    const char *f = from ? (*env)->GetStringUTFChars(env, from, NULL) : NULL;
    const char *t = to ? (*env)->GetStringUTFChars(env, to, NULL) : NULL;
    int r = f && t ? disc_publish(generation, f, t) : -1;
    if (f) (*env)->ReleaseStringUTFChars(env, from, f);
    if (t) (*env)->ReleaseStringUTFChars(env, to, t);
    return r;
}
