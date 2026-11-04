/* Bounded frame statistics; no allocation or sorting on the render thread.
   GPL-3.0-or-later. */
#include "performance_stats.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum { SAMPLES = 240 };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static double intervals[SAMPLES], previous;
static unsigned count, next;

void ios_perf_reset(void) {
    pthread_mutex_lock(&lock);
    previous = 0; count = next = 0;
    pthread_mutex_unlock(&lock);
}

void ios_perf_record_present(double seconds) {
    if (!isfinite(seconds) || seconds <= 0) return;
    pthread_mutex_lock(&lock);
    double dt = seconds - previous;
    if (previous && dt > 0 && isfinite(dt)) {
        intervals[next] = dt;
        next = (next + 1) % SAMPLES;
        if (count < SAMPLES) count++;
    } else if (previous) {
        count = next = 0; /* first frame or a backwards clock; lifecycle resets explicitly */
    }
    previous = seconds;
    pthread_mutex_unlock(&lock);
}

static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

void ios_perf_snapshot(double *fps, double *p99_ms) {
    double copy[SAMPLES], sum = 0;
    pthread_mutex_lock(&lock);
    unsigned n = count;
    memcpy(copy, intervals, n * sizeof *copy);
    pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < n; i++) sum += copy[i];
    if (fps) *fps = sum > 0 ? n / sum : 0;
    if (p99_ms) {
        qsort(copy, n, sizeof *copy, compare);
        *p99_ms = n ? copy[(99 * n + 99) / 100 - 1] * 1000 : 0;
    }
}
