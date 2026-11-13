/* How the process ends: host_end.h (finding 210). */
#include "host_end.h"
#include "host_api.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_HANDLERS 32
#define DEFAULT_TIMEOUT_MS 3000

static struct { int stage; const char *name; void (*fn)(void); } handlers[MAX_HANDLERS];
static SDL_AtomicInt ready[MAX_HANDLERS];  /* 1 once handlers[i] is filled */
static SDL_AtomicInt added;                /* slots taken */
static SDL_AtomicInt hooked;               /* atexit(end_from_exit) is registered */
static SDL_AtomicInt began;                /* the handlers have started */
static SDL_AtomicInt finished;             /* the handlers have all returned */
static SDL_ThreadID runner;                /* the thread running them */
static const char *volatile running;       /* the handler in progress, for the watchdog's line */
static int end_code;

static void end_from_exit(void);

void host_end_add(int stage, const char *name, void (*fn)(void))
{
    int i = SDL_AddAtomicInt(&added, 1);
    if (i >= MAX_HANDLERS) {
        fprintf(stderr, "end: too many handlers; %s is not registered\n", name);
        return;
    }
    handlers[i].stage = stage, handlers[i].name = name, handlers[i].fn = fn;
    SDL_SetAtomicInt(&ready[i], 1);
    if (SDL_CompareAndSwapAtomicInt(&hooked, 0, 1)) atexit(end_from_exit);
}

static unsigned timeout_ms(void)
{
    const char *text = getenv("DHD_END_TIMEOUT_MS");
    int ms = text ? atoi(text) : 0;
    return ms > 0 ? (unsigned)ms : DEFAULT_TIMEOUT_MS;
}

/* A handler that does not return, a thread that will not join: after the time is up the process
   leaves as it is. The log says which handler it was waiting in. */
static int SDLCALL watchdog(void *start)
{
    Uint64 deadline = *(Uint64 *)start;
    while (SDL_GetTicks() < deadline) {
        if (SDL_GetAtomicInt(&finished) == 2) return 0;  /* the handlers and the flush are done */
        SDL_Delay(10);
    }
    const char *where = running;
    fprintf(stderr, "end: %s still running after %u ms; the process leaves now\n", where ? where : "the end", timeout_ms());
    fflush(stderr);
    host_exit(end_code);
    return 0;
}

/* Runs the handlers once. 1: this call ran them; 0: they were running or done already (the same
   thread, or another one that is waited for here). */
static int run_handlers(int code, int wait_for_other)
{
    if (!SDL_CompareAndSwapAtomicInt(&began, 0, 1)) {
        if (SDL_GetCurrentThreadID() == runner) return 0;  /* a handler ended the process itself */
        if (wait_for_other)
            for (;;) SDL_Delay(100);  /* the first caller ends the process, or its watchdog does */
        for (unsigned waited = 0; !SDL_GetAtomicInt(&finished) && waited < timeout_ms() + 500; waited += 10) SDL_Delay(10);
        return 0;
    }
    runner = SDL_GetCurrentThreadID();
    end_code = code;
    static Uint64 deadline;
    deadline = SDL_GetTicks() + timeout_ms();
    SDL_Thread *dog = SDL_CreateThread(watchdog, "end watchdog", &deadline);
    if (dog) SDL_DetachThread(dog);
    Uint64 start = SDL_GetTicks();
    fprintf(stderr, "end: closing (code %d)\n", code);
    int n = SDL_GetAtomicInt(&added);
    if (n > MAX_HANDLERS) n = MAX_HANDLERS;
    for (int stage = 0; stage < HOST_END_STAGES; stage++)
        for (int i = n - 1; i >= 0; i--)
            if (SDL_GetAtomicInt(&ready[i]) && handlers[i].stage == stage) {
                running = handlers[i].name;
                handlers[i].fn();
            }
    running = "flushing";
    SDL_SetAtomicInt(&finished, 1);
    fprintf(stderr, "end: handlers done in %u ms\n", (unsigned)(SDL_GetTicks() - start));
    return 1;
}

/* exit() or a return from main, from a path that did not call host_end: the same handlers, once
   (the watchdog watches them, not the C library's teardown after them). */
static void end_from_exit(void)
{
    run_handlers(0, 0);
    SDL_SetAtomicInt(&finished, 2);
}

int host_end_code(void) { return end_code; }

void host_end(int code)
{
    run_handlers(code, 1);
    fflush(NULL);
    SDL_SetAtomicInt(&finished, 2);
    host_exit(code);
    for (;;) {}
}
