/* Seekable Java document reader for the shared boot-disc check. GPL-3.0-or-later. */
#include <jni.h>
#include <stdio.h>
#include "disc_check.h"

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
JNIEXPORT jstring JNICALL Java_app_downhillrevived_DiscImage_checkNative(
    JNIEnv *env, jclass cls, jobject file, jlong size)
{
    jmethodID method = (*env)->GetStaticMethodID(env, cls, "readAt", "(Ljava/nio/channels/FileChannel;JLjava/nio/ByteBuffer;)Z");
    if (!method) return NULL;
    Reader reader = {env, cls, file, method};
    char id[32], out[64];
    int code = disc_check_image(read_at, &reader, (uint64_t)size, id, sizeof id, NULL);
    if ((*env)->ExceptionCheck(env)) return NULL;
    snprintf(out, sizeof out, "%d\n%s", code, id);
    return (*env)->NewStringUTF(env, out);
}
