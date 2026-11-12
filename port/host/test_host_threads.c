/* host_threads.c (finding 217): a named thread that has ended is never asked for anything by
   handle, and its CPU time is kept for the session's lines. port/tools/test_runtime.py runs it.

   - joined: a thread names itself, burns CPU, ends and is joined (the VU and GS workers at the
     exit, a movie's decoder after its clip); the list no longer counts it as running (so no handle
     of it is used: only running entries are asked), and the snapshots after the join still
     report its time, the same each time, at least what it burned.
   - again: a second thread of the same name (the next movie) takes the ended entry; its time adds.
   - churn: short threads start, name themselves and are joined while another thread takes
     snapshots all the time (the render thread's perf_presented while a movie ends). Before the fix
     this aborted on Android (bionic checks every pthread_t) and read freed memory on glibc;
     Windows kept its handles valid, so there the check is the running count.
   Exit code 0 and "ok" when everything holds; a failed check prints FAIL and exits 1. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void host_thread_name(const char *name);
unsigned host_thread_cpu(const char **names, double *seconds, unsigned max);
int host_thread_running(const char *name);
double host_current_thread_cpu(void);

void perf_thread(const char *name) { (void)name; }  /* port/gs/perf_log.c's, not linked here */

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

/* The CPU time the list reports for name (the first entry), -1 when absent. */
static double cpu_of(const char *name)
{
    const char *names[16];
    double seconds[16];
    unsigned n = host_thread_cpu(names, seconds, 16);
    for (unsigned i = 0; i < n; i++)
        if (!strcmp(names[i], name)) return seconds[i];
    return -1;
}

static SDL_AtomicInt named_ok, release;
static double burned[2];

static int SDLCALL burner(void *which)
{
    int at = (int)(intptr_t)which;
    host_thread_name("burner");
    SDL_SetAtomicInt(&named_ok, 1);
    double start = host_current_thread_cpu();
    volatile double x = 1;
    while (host_current_thread_cpu() - start < 0.12) x = x * 1.0000001 + 1e-9;  /* about 120 ms of CPU */
    while (!SDL_GetAtomicInt(&release)) SDL_Delay(1);
    burned[at] = host_current_thread_cpu();
    return 0;
}

static void joined(void)
{
    SDL_SetAtomicInt(&named_ok, 0);
    SDL_SetAtomicInt(&release, 0);
    SDL_Thread *t = SDL_CreateThread(burner, "burner", (void *)(intptr_t)0);
    while (!SDL_GetAtomicInt(&named_ok)) SDL_Delay(1);
    CHECK(host_thread_running("burner") == 1, "a running thread counts as running");
    CHECK(cpu_of("burner") >= 0, "a running thread is listed");
    SDL_SetAtomicInt(&release, 1);
    SDL_WaitThread(t, NULL);
    CHECK(host_thread_running("burner") == 0, "a joined thread is no longer running in the list");
    double a = cpu_of("burner"), b = cpu_of("burner");
    CHECK(a >= 0.06, "a joined thread's time is kept (%.3f s, burned %.3f s)", a, burned[0]);
    CHECK(a == b, "a joined thread's kept time does not move (%.6f, %.6f)", a, b);
    printf("joined: kept %.3f s of %.3f s burned\n", a, burned[0]);
}

static void again(void)
{
    double before = cpu_of("burner");
    SDL_SetAtomicInt(&named_ok, 0);
    SDL_SetAtomicInt(&release, 1);
    SDL_Thread *t = SDL_CreateThread(burner, "burner", (void *)(intptr_t)1);
    SDL_WaitThread(t, NULL);
    double after = cpu_of("burner");
    const char *names[16];
    double seconds[16];
    unsigned n = host_thread_cpu(names, seconds, 16), entries = 0;
    for (unsigned i = 0; i < n; i++) entries += !strcmp(names[i], "burner");
    CHECK(entries == 1, "the second thread of a name takes the ended entry (%u entries)", entries);
    CHECK(after >= before + 0.06, "the second thread's time adds (%.3f -> %.3f)", before, after);
    CHECK(host_thread_running("burner") == 0, "the second thread, joined, is not running");
    printf("again: %.3f s -> %.3f s\n", before, after);
}

static SDL_AtomicInt watching;
static SDL_AtomicInt snapshots;

static int SDLCALL watcher(void *unused)
{
    (void)unused;
    host_thread_name("watcher");
    while (SDL_GetAtomicInt(&watching)) {
        const char *names[16];
        double seconds[16];
        host_thread_cpu(names, seconds, 16);
        SDL_AddAtomicInt(&snapshots, 1);
    }
    return 0;
}

static int SDLCALL short_one(void *unused)
{
    (void)unused;
    host_thread_name("short");
    volatile int x = 0;
    for (int i = 0; i < 20000; i++) x += i;
    return 0;
}

static void churn(void)
{
    SDL_SetAtomicInt(&watching, 1);
    SDL_Thread *w = SDL_CreateThread(watcher, "watcher", NULL);
    for (int round = 0; round < 150; round++) {
        SDL_Thread *t[4];
        for (int i = 0; i < 4; i++) t[i] = SDL_CreateThread(short_one, "short", NULL);
        for (int i = 0; i < 4; i++) SDL_WaitThread(t[i], NULL);
    }
    CHECK(host_thread_running("short") == 0, "600 joined threads: none running in the list (%d)",
          host_thread_running("short"));
    CHECK(host_thread_running("watcher") == 1, "the watcher still runs");
    SDL_SetAtomicInt(&watching, 0);
    SDL_WaitThread(w, NULL);
    CHECK(host_thread_running("watcher") == 0, "the watcher, joined, is not running");
    CHECK(cpu_of("short") >= 0, "the short threads' time is kept");
    printf("churn: 600 threads joined under %d snapshots\n", SDL_GetAtomicInt(&snapshots));
}

int main(void)
{
    host_thread_name("main");  /* the main thread: never ends before the process */
    joined();
    again();
    churn();
    CHECK(host_thread_running("main") == 1, "the main thread runs");
    if (failures) return 1;
    printf("ok\n");
    return 0;
}
