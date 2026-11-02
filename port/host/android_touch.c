/* The touch controls on Android (finding 139; port/host/touch_host.c): what only the activity
   knows, through JNI on the SDL thread (DhdActivity.java): the screen's density and the system
   gesture and cutout insets, the phone's vibrator, and its roll for tilt steering (the gravity
   sensor, with the screen locked to its landscape side while tilt is on). */
#include <SDL3/SDL.h>
#include <jni.h>

/* The activity's method `name` with `signature`; NULL if missing. */
static jmethodID method(JNIEnv *env, jobject activity, const char *name, const char *signature)
{
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID id = cls ? (*env)->GetMethodID(env, cls, name, signature) : NULL;
    if (cls) (*env)->DeleteLocalRef(env, cls);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return NULL; }
    return id;
}

static int begin(JNIEnv **env, jobject *activity)
{
    *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    *activity = *env ? (jobject)SDL_GetAndroidActivity() : NULL;
    return *env && *activity;
}

static void end(JNIEnv *env, jobject activity)
{
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, activity);
}

/* DhdActivity.touchScreen(): xdpi and ydpi (times 1000), the gesture insets and the cutout's (px). */
int android_touch_screen(float *xdpi, float *ydpi, int insets[8])
{
    JNIEnv *env;
    jobject activity;
    if (!begin(&env, &activity)) return 0;
    int ok = 0;
    jmethodID m = method(env, activity, "touchScreen", "()[I");
    jintArray result = m ? (jintArray)(*env)->CallObjectMethod(env, activity, m) : NULL;
    if (result && !(*env)->ExceptionCheck(env) && (*env)->GetArrayLength(env, result) >= 10) {
        jint v[10];
        (*env)->GetIntArrayRegion(env, result, 0, 10, v);
        *xdpi = v[0] / 1000.0f, *ydpi = v[1] / 1000.0f;
        for (int i = 0; i < 8; i++) insets[i] = v[2 + i];
        ok = 1;
    }
    if (result) (*env)->DeleteLocalRef(env, result);
    end(env, activity);
    return ok;
}

/* DhdActivity.touchBuzz(kind): 1 a tick, 2 a double tick, 3 a strong one. */
void android_touch_buzz(int kind)
{
    JNIEnv *env;
    jobject activity;
    if (!begin(&env, &activity)) return;
    jmethodID m = method(env, activity, "touchBuzz", "(I)V");
    if (m) (*env)->CallVoidMethod(env, activity, m, (jint)kind);
    end(env, activity);
}

/* DhdActivity.touchTilt(): the roll in degrees (turned right positive); 0 before the sensor answers. */
int android_touch_tilt(float *degrees)
{
    JNIEnv *env;
    jobject activity;
    if (!begin(&env, &activity)) return 0;
    jmethodID m = method(env, activity, "touchTilt", "()F");
    jfloat v = m ? (*env)->CallFloatMethod(env, activity, m) : 0;
    end(env, activity);
    if (v != v) return 0;   /* NaN: not known yet */
    *degrees = v;
    return 1;
}

/* DhdActivity.touchTiltOn(on): the sensor, and the screen locked to its side while on. */
void android_touch_tilt_on(int on)
{
    JNIEnv *env;
    jobject activity;
    if (!begin(&env, &activity)) return;
    jmethodID m = method(env, activity, "touchTiltOn", "(Z)V");
    if (m) (*env)->CallVoidMethod(env, activity, m, (jboolean)(on != 0));
    end(env, activity);
}
