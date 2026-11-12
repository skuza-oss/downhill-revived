/* host_end.c (finding 210). One scenario per argument; port/tools/test_runtime.py runs them and
   checks the exit code and the output.

     order     handlers added in any order run by stage; the last added runs first within a stage;
               host_end(7) leaves with 7 and no atexit handler runs
     exit      a plain exit(4) runs the same handlers once, then the atexit handler runs
     thread    a thread still using a lock when the process ends: its stop handler joins it before
               anything is destroyed, then host_end(0) leaves (the fix; exit 0)
     unjoined  the same thread with no stop handler and a plain exit(): the "static destructor"
               (an atexit handler that destroys the lock) runs under the thread, which sees it;
               the process ends with 42. That is the Android abort in miniature; it is what the
               fix prevents.
     hang      a handler that never returns: the watchdog (DHD_END_TIMEOUT_MS) ends the process
               with the code host_end was given
     nested    a handler that calls host_end itself ends the process at once with its code */
#include "host_end.h"
#include "host_api.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* host_api.h's host_exit, for a program without the host */
void host_exit(int code)
{
    fflush(NULL);
    _exit(code);
}

static void say(const char *what) { printf("%s\n", what); fflush(stdout); }

static void devices_a(void) { say("devices a"); }
static void devices_b(void) { say("devices b"); }
static void workers(void) { say("workers"); }
static void render(void) { say("render"); }
static void report(void) { say("report"); }
static void log_last(void) { say("log"); }
static void after_exit(void) { say("atexit ran"); }

/* ---- a thread that uses a lock until it is told to stop ---- */
static SDL_Mutex *lock;
static SDL_AtomicInt destroyed, stop, violated;
static SDL_Thread *user;

static int SDLCALL use_lock(void *unused)
{
    (void)unused;
    while (!SDL_GetAtomicInt(&stop)) {
        SDL_LockMutex(lock);
        if (SDL_GetAtomicInt(&destroyed))  /* bionic aborts here; other systems go on silently */
            SDL_SetAtomicInt(&violated, 1);
        SDL_UnlockMutex(lock);
        SDL_Delay(1);
    }
    return 0;
}

static void stop_user(void)
{
    SDL_SetAtomicInt(&stop, 1);
    SDL_WaitThread(user, NULL);
    say("thread joined");
}

/* a library's static destructor: destroys the lock, then takes a moment before the process is gone */
static void destructor(void)
{
    SDL_SetAtomicInt(&destroyed, 1);
    SDL_Delay(200);
    if (SDL_GetAtomicInt(&violated)) {
        say("VIOLATION: the lock was used after it was destroyed");
        _exit(42);
    }
    say("destructor done");
}

static void hang(void) { for (;;) SDL_Delay(100); }
static void nested(void) { host_end(9); }

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "order")) {
        host_end_add(HOST_END_DEVICES, "devices a", devices_a);
        host_end_add(HOST_END_LOG, "log", log_last);
        host_end_add(HOST_END_RENDER, "render", render);
        host_end_add(HOST_END_DEVICES, "devices b", devices_b);
        host_end_add(HOST_END_REPORT, "report", report);
        host_end_add(HOST_END_WORKERS, "workers", workers);
        atexit(after_exit);
        host_end(7);
    }
    if (!strcmp(mode, "exit")) {
        atexit(after_exit);  /* before the first host_end_add: it runs after the hook */
        host_end_add(HOST_END_RENDER, "render", render);
        host_end_add(HOST_END_LOG, "log", log_last);
        exit(4);
    }
    if (!strcmp(mode, "thread") || !strcmp(mode, "unjoined")) {
        lock = SDL_CreateMutex();
        user = SDL_CreateThread(use_lock, "user", NULL);
        if (!strcmp(mode, "thread")) host_end_add(HOST_END_RENDER, "user thread", stop_user);
        else host_end_add(HOST_END_LOG, "nothing", log_last);  /* the hook exists, the thread's stop does not */
        atexit(destructor);
        SDL_Delay(50);  /* the thread is in its loop */
        if (!strcmp(mode, "thread")) host_end(0);
        exit(0);
    }
    if (!strcmp(mode, "hang")) {
        host_end_add(HOST_END_RENDER, "stuck", hang);
        host_end(5);
    }
    if (!strcmp(mode, "nested")) {
        host_end_add(HOST_END_RENDER, "nested", nested);
        host_end(1);
    }
    fprintf(stderr, "usage: test_host_end order|exit|thread|unjoined|hang|nested\n");
    return 2;
}
