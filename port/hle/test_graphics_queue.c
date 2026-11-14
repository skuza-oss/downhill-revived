#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>
#include "hle.h"
#include "../gs/gs.h"

Gs gs;
static uint32_t actual, expected, calls, zero_calls;
static DWORD main_thread, consumer_thread;
static int failures;
static int inline_mode;
void host_thread_name(const char *name) { (void)name; }
void hle_log(const char *fmt, ...) { (void)fmt; }
static uint32_t append(uint32_t hash, unsigned unit, uint32_t word)
{
    return (hash ^ word ^ unit * 0x9e3779b9u) * 16777619u;
}
/* Finding 236: the drawing stuck while a phone's app is away (the held renderer), and the
   game thread's wait for it. The consumer stays in its packet until the away hook (present.c's
   app_away_wait) lets it go; it gives up after 3 s, a failure: the wait never reached the hook. */
static volatile LONG stuck;
static unsigned away_calls;
static void away_wait(const char *where)
{
    if (!stuck) return;
    away_calls++;
    failures += GetCurrentThreadId() != main_thread || !where || !*where;
    InterlockedExchange(&stuck, 0);   /* the return: the renderer goes again */
}
static void stuck_packet(void)
{
    for (DWORD start = GetTickCount(); stuck;)
        if (GetTickCount() - start > 3000) { failures++; InterlockedExchange(&stuck, 0); }
}
static void receive(unsigned unit, const uint32_t *words, unsigned count)
{
    DWORD thread = GetCurrentThreadId();
    stuck_packet();
    if (!consumer_thread) consumer_thread = thread;
    failures += consumer_thread != thread || (inline_mode ? thread != main_thread : thread == main_thread);
    failures += (_mm_getcsr() & 0xFFC0u) != 0x1F80u;  /* masked, nearest, no FTZ/DAZ (not the EE mode) */
    volatile float one = 1, zero = 0;
    float infinity = one / zero; /* intentional host IEEE result, even under the EE trap */
    failures += !__builtin_isinf(infinity);
    for (unsigned i = 0; i < count; i++) actual = append(actual, unit, words[i]);
    calls++;
    if (!(calls % 37)) Sleep(1); /* force producer backpressure */
}
void vif_feed(int unit, const uint32_t *words, uint32_t count)
{
    if (!unit) {
        failures += GetCurrentThreadId() != main_thread || (_mm_getcsr() & 0x680u) != 0;
        zero_calls++;
    }
    else receive((unsigned)unit, words, count);
}
void gs_feed(Gs *state, unsigned path, const uint32_t *words, size_t count)
{
    failures += state != &gs || path != 3;
    receive(2, words, count);
}
int main(int argc, char **argv)
{
    uint32_t words[4097], random = 0x12345678;
    main_thread = GetCurrentThreadId();
    (void)argv;
    inline_mode = argc > 1;
    _putenv(inline_mode ? "DHD_MTVU=0" : "DHD_MTVU=1");
    _mm_setcsr((_mm_getcsr() & ~(0x680u | 63u)) | 0xE040u);  /* the EE mode: toward zero, FTZ, DAZ */
    for (unsigned packet = 0; packet < 3000; packet++) {
        unsigned count = packet % 17 == 0 ? 4097 : packet % 271 + 1;
        unsigned unit = packet % 3 == 0 ? 2 : 1;
        for (unsigned i = 0; i < count; i++) {
            random = random * 1664525u + 1013904223u;
            words[i] = random;
            expected = append(expected, unit, words[i]);
        }
        graphics_feed((int)unit, GADDR(words), count);
        memset(words, 0xCD, sizeof words); /* DMA source may be reused immediately */
        if (!(packet % 19)) {
            graphics_wait();
            failures += actual != expected || (_mm_getcsr() & 0x680u) != 0;
            graphics_feed(0, GADDR(words), 1);
        }
    }
    if (!inline_mode) {
        /* stuck behind a held renderer: a fence, then a queue filled past its size */
        graphics_away_wait = away_wait;
        for (int round = 0; round < 2; round++) {
            InterlockedExchange(&stuck, 1);
            unsigned before = away_calls;
            for (unsigned packet = 0; packet < (round ? 400u : 1u); packet++) {
                for (unsigned i = 0; i < 4097; i++) words[i] = i, expected = append(expected, 1, i);
                graphics_feed(1, GADDR(words), 4097);
            }
            graphics_wait();
            failures += away_calls != before + 1 || stuck;
        }
        graphics_away_wait = NULL;
    }
    graphics_shutdown(); /* drains the last packets before joining */
    failures += actual != expected || !consumer_thread || !zero_calls || calls < 3000;
    printf("graphics queue: %u chunks, copied packets, wrap, fences, %u waits behind a held renderer: %d failures\n", calls,
           away_calls, failures);
    return failures != 0;
}
