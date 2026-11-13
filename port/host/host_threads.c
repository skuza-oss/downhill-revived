/* The port's own OS threads by name (game, vu, gs, native, movie, texpack, sdl): the perf log
   (port/gs/perf_log.c, finding 129), the bench and the timeline read each one's CPU time.

   A thread that has ended is never asked for anything by handle (finding 217). bionic aborts
   ("invalid pthread_t ... passed to pthread_getcpuclockid") when a pthread_t names a thread that
   has been joined or has exited; glibc may read freed memory; macOS's pthread_mach_thread_np reads
   the same freed struct. So a named thread leaves the list on its way out: SDL runs its TLS
   destructors on the thread itself after the thread function returns (SDL_RunThread), and ours
   takes the list's lock, keeps the thread's final CPU time and marks the entry ended. A reader
   asks a running thread's clock under the same lock, so the thread cannot get past its destructor
   (and end) while it is being asked. An ended entry answers its kept time, so a snapshot after a
   join (the exit's `perf: session` lines, HOST_END_REPORT, after the workers and the render thread
   are gone) still counts each thread's whole life. A new thread of the same name (the next movie)
   takes the ended entry and adds to its time.

   Every named thread of the port is an SDL thread (SDL_CreateThread); the one that is not, the
   process's main thread ("sdl", or "game" where the game runs on it), lives until the process
   ends. A thread made some other way that named itself and then ended would be asked again: name
   only SDL threads, or the main thread. */
#define _GNU_SOURCE  /* gettid, pthread_setname_np */
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_thread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <TargetConditionals.h>
#endif

void perf_thread(const char *name);  /* port/gs/perf_log.c: its waits */
#ifdef __ANDROID__
void android_perf_thread(const char *name);  /* port/host/android_perf.c: placement and hints */
void android_perf_thread_ended(void);
#endif

enum { NAMED_THREADS = 16 };
enum { EMPTY, RUNNING, ENDED };
static struct {
    const char *name;
    int state;
    double ended_seconds;   /* the CPU time of this name's threads that have ended */
    uint64_t ended_cycles;  /* the same in cycles (Windows) */
#ifdef _WIN32
    HANDLE handle;          /* a duplicate, closed when the thread ends */
#else
    pthread_t handle;
#endif
} named[NAMED_THREADS];
static SDL_SpinLock lock;     /* named[]; never destroyed */
static SDL_TLSID slots_tls;   /* the calling thread's entries, a bit each */

double host_current_thread_cpu(void)
{
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    return (((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
            ((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime)) * 1e-7;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) return 0;
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
#endif
}

/* SDL's TLS destructor, on the ending thread itself: its entries keep its final time and end. */
static void SDLCALL thread_ended(void *value)
{
    unsigned slots = (unsigned)(uintptr_t)value;
    double seconds = host_current_thread_cpu();
    uint64_t cycles = 0;
#ifdef _WIN32
    ULONG64 value64;
    if (QueryThreadCycleTime(GetCurrentThread(), &value64)) cycles = value64;
#endif
#ifdef __ANDROID__
    android_perf_thread_ended();
#endif
    SDL_LockSpinlock(&lock);
    for (int i = 0; i < NAMED_THREADS; i++) {
        if (!(slots & 1u << i) || named[i].state != RUNNING) continue;
        named[i].ended_seconds += seconds;
        named[i].ended_cycles += cycles;
        named[i].state = ENDED;
#ifdef _WIN32
        CloseHandle(named[i].handle);
        named[i].handle = NULL;
#endif
    }
    SDL_UnlockSpinlock(&lock);
}

void host_thread_name(const char *name)
{
#ifdef _WIN32  /* the OS's name too, for debuggers and profilers */
    WCHAR wide[64];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64)) SetThreadDescription(GetCurrentThread(), wide);
#elif defined(__APPLE__)
    pthread_setname_np(name);  /* the calling thread's own */
#if TARGET_OS_IPHONE
    /* iOS: the game's own threads at the interactive QoS class, so the scheduler keeps them on
       the performance cores (the iOS port, finding 224) */
    if (!strcmp(name, "game") || !strcmp(name, "vu") || !strcmp(name, "gs") || !strcmp(name, "native"))
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
#else
    /* Linux and Android: the main thread's name is the process's own (ps, top, pgrep, a desktop's
       system monitor), so that thread keeps "dhd" (a crash report then says thread dhd) */
    if (gettid() != getpid()) pthread_setname_np(pthread_self(), name);
#endif
#ifdef __ANDROID__
    android_perf_thread(name);
#endif
    perf_thread(name);
#ifdef _WIN32
    HANDLE handle = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return;
#else
    pthread_t handle = pthread_self();
#endif
    int at = -1;
    SDL_LockSpinlock(&lock);
    for (int i = 0; i < NAMED_THREADS && at < 0; i++)  /* an ended thread of this name: its time goes on */
        if (named[i].state == ENDED && !strcmp(named[i].name, name)) at = i;
    for (int i = 0; i < NAMED_THREADS && at < 0; i++)
        if (named[i].state == EMPTY) at = i;
    if (at >= 0) {
        named[at].name = name;
        named[at].handle = handle;
        named[at].state = RUNNING;
    }
    SDL_UnlockSpinlock(&lock);
    if (at < 0) {  /* full: not counted, as before */
#ifdef _WIN32
        CloseHandle(handle);
#endif
        return;
    }
    unsigned slots = (unsigned)(uintptr_t)SDL_GetTLS(&slots_tls) | 1u << at;
    SDL_SetTLS(&slots_tls, (void *)(uintptr_t)slots, thread_ended);
}

/* The named threads' CPU times in seconds, each name as often as it has entries; an ended
   thread's time is the one it had when it ended. */
unsigned host_thread_cpu(const char **names, double *seconds, unsigned max)
{
    unsigned n = 0;
    SDL_LockSpinlock(&lock);
    for (int i = 0; i < NAMED_THREADS && n < max; i++) {
        if (named[i].state == EMPTY) continue;
        double now = 0;
        if (named[i].state == RUNNING) {
#ifdef _WIN32
            FILETIME created, exited, kernel, user;
            if (!GetThreadTimes(named[i].handle, &created, &exited, &kernel, &user)) continue;
            now = (((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                   ((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime)) * 1e-7;
#elif defined(__APPLE__)  /* no per-thread CPU clocks: Mach's thread times */
            thread_basic_info_data_t info;
            mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
            if (thread_info(pthread_mach_thread_np(named[i].handle), THREAD_BASIC_INFO, (thread_info_t)&info,
                            &count) != KERN_SUCCESS)
                continue;
            now = info.user_time.seconds + info.system_time.seconds +
                  (info.user_time.microseconds + info.system_time.microseconds) / 1e6;
#else
            clockid_t id;
            struct timespec t;
            if (pthread_getcpuclockid(named[i].handle, &id) != 0 || clock_gettime(id, &t) != 0) continue;
            now = (double)t.tv_sec + (double)t.tv_nsec / 1e9;
#endif
        }
        names[n] = named[i].name;
        seconds[n++] = named[i].ended_seconds + now;
    }
    SDL_UnlockSpinlock(&lock);
    return n;
}

/* Raw scheduled CPU cycles, complementary to GetThreadTimes (coarse on Windows). */
unsigned host_thread_cycles(const char **names, uint64_t *cycles, unsigned max)
{
    unsigned n = 0;
#ifdef _WIN32
    SDL_LockSpinlock(&lock);
    for (int i = 0; i < NAMED_THREADS && n < max; i++) {
        ULONG64 value = 0;
        if (named[i].state == EMPTY ||
            (named[i].state == RUNNING && !QueryThreadCycleTime(named[i].handle, &value)))
            continue;
        names[n] = named[i].name;
        cycles[n++] = named[i].ended_cycles + value;
    }
    SDL_UnlockSpinlock(&lock);
#else
    (void)names; (void)cycles; (void)max;
#endif
    return n;
}

/* How many threads of this name are running now (ended ones not counted). */
int host_thread_running(const char *name)
{
    int count = 0;
    SDL_LockSpinlock(&lock);
    for (int i = 0; i < NAMED_THREADS; i++)
        count += named[i].state == RUNNING && !strcmp(named[i].name, name);
    SDL_UnlockSpinlock(&lock);
    return count;
}
