/* The disc image's copy into the iOS app: disc_copy.h. GPL-3.0-or-later. */
#include "disc_copy.h"

#include <stdlib.h>
#include <string.h>

uint64_t disc_volume_bytes(FILE *f)
{
    unsigned char s[2048];
    if (fseek(f, 16L * 2048, SEEK_SET) || fread(s, 1, sizeof s, f) != sizeof s) return 0;
    if (s[0] != 1 || memcmp(s + 1, "CD001", 5)) return 0;
    uint64_t blocks = (uint64_t)s[80] | (uint64_t)s[81] << 8 | (uint64_t)s[82] << 16 | (uint64_t)s[83] << 24;
    unsigned block = (unsigned)s[128] | (unsigned)s[129] << 8;
    return blocks * block;
}

static double mb(uint64_t bytes) { return bytes / (double)(1 << 20); }

const char *disc_completeness(uint64_t copied, uint64_t size, uint64_t volume, char *why, size_t why_size)
{
    if (size && copied < size)
        snprintf(why, why_size, "The copy stopped early: %.1f MB of %.1f MB arrived. Try again.", mb(copied), mb(size));
    else if (size && copied != size)
        snprintf(why, why_size, "The copy doesn't match the file: %.1f MB copied, the file has %.1f MB. Try again.",
                 mb(copied), mb(size));
    else if (!volume)
        snprintf(why, why_size, "The copy has no disc volume size, so it can't be checked as complete.");
    else if (copied < volume)
        snprintf(why, why_size, "The copy is incomplete: the disc image declares %.1f MB, and only %.1f MB arrived. "
                 "Try again, or copy the file onto the device first.", mb(volume), mb(copied));
    else
        return NULL;
    return why;
}

int64_t disc_copy(FILE *in, FILE *out, const volatile int *cancel,
                  void (*progress)(void *context, uint64_t done), void *context)
{
    enum { BLOCK = 1 << 20 };
    char *block = malloc(BLOCK);
    if (!block) return -1;
    uint64_t done = 0, shown = 0;
    int64_t result = -1;
    for (;;) {
        size_t n = fread(block, 1, BLOCK, in);
        if (!n) {
            if (!ferror(in)) result = (int64_t)done;
            break;
        }
        if (cancel && *cancel) break;
        if (fwrite(block, 1, n, out) != n) break;
        done += n;
        if (progress && done - shown >= DISC_COPY_STEP) progress(context, shown = done);
    }
    free(block);
    return result;
}

/* ---- the import's publication (disc_copy.h) ---- */
#ifdef _WIN32   /* the unit test on Windows */
#include <windows.h>
static SRWLOCK publish_lock = SRWLOCK_INIT;
#define PUBLISH_LOCK() AcquireSRWLockExclusive(&publish_lock)
#define PUBLISH_UNLOCK() ReleaseSRWLockExclusive(&publish_lock)
#else
#include <pthread.h>
#include <unistd.h>
static pthread_mutex_t publish_lock = PTHREAD_MUTEX_INITIALIZER;
#define PUBLISH_LOCK() pthread_mutex_lock(&publish_lock)
#define PUBLISH_UNLOCK() pthread_mutex_unlock(&publish_lock)
#endif
static long long publish_newest;   /* under publish_lock */

long long disc_publish_begin(void)
{
    PUBLISH_LOCK();
    long long generation = ++publish_newest;
    PUBLISH_UNLOCK();
    return generation;
}

int disc_publish_current(long long generation)
{
    PUBLISH_LOCK();
    int current = generation == publish_newest;
    PUBLISH_UNLOCK();
    return current;
}

int disc_publish(long long generation, const char *from, const char *to)
{
    PUBLISH_LOCK();
    int result = 1;
    if (generation == publish_newest) {
#ifdef DISC_PUBLISH_TEST_PAUSE   /* test_disc_copy: a pause between the check and the rename, so that
                                    without the lock a newer import would slip in */
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
#endif
#ifdef _WIN32   /* the unit test on Windows: rename over the target as POSIX does */
        result = MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
        result = rename(from, to) == 0 ? 0 : -1;
#endif
    }
    PUBLISH_UNLOCK();
    return result;
}
