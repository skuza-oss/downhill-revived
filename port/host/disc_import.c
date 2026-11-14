/* The phones' disc import rules: disc_import.h. GPL-3.0-or-later. */
#include "disc_import.h"
#include "disc_check.h"

#include <stdlib.h>
#include <string.h>

uint64_t disc_room_needed(uint64_t size, uint64_t free_bytes)
{
    return size && size + DISC_COPY_MARGIN > free_bytes ? size + DISC_COPY_MARGIN : 0;
}

static int file_read(void *context, uint64_t offset, void *buffer, size_t n)
{
    FILE *f = context;
#ifdef _WIN32
    if (_fseeki64(f, (long long)offset, SEEK_SET)) return 0;
#else
    if (fseeko(f, (off_t)offset, SEEK_SET)) return 0;
#endif
    return fread(buffer, 1, n, f) == n;
}

int disc_check_file(FILE *f, uint64_t *volume)
{
    char id[32];
    *volume = 0;
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END)) return DISC_NOT_ISO;
    long long size = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END)) return DISC_NOT_ISO;
    long long size = (long long)ftello(f);
#endif
    return size < 0 ? DISC_NOT_ISO : disc_check_image(file_read, f, (uint64_t)size, id, sizeof id, volume);
}

static double mb(uint64_t bytes) { return bytes / (double)(1 << 20); }

const char *disc_completeness(uint64_t copied, uint64_t size, int exact, uint64_t volume, char *why, size_t why_size)
{
    if (size && copied < size)
        snprintf(why, why_size, "The copy stopped early: %.1f MB of %.1f MB arrived. Try again.", mb(copied), mb(size));
    else if (size && exact && copied != size)
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

const char *disc_copy_verdict(int kind, uint64_t copied, uint64_t size, int exact, uint64_t volume,
                              char *why, size_t why_size)
{
    if (kind != DISC_GAME && kind != DISC_CUT) return NULL;
    return disc_completeness(copied, size, exact, volume, why, why_size);
}

int64_t disc_copy(FILE *in, FILE *out, const volatile int *cancel,
                  void (*progress)(void *context, uint64_t done), void *context)
{
    char *block = malloc(DISC_COPY_BLOCK);
    if (!block) return -1;
    uint64_t done = 0, shown = 0;
    int64_t result = -1;
    for (;;) {
        size_t n = fread(block, 1, DISC_COPY_BLOCK, in);
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

/* ---- the import's publication (disc_import.h) ---- */
#ifdef _WIN32   /* the unit tests on Windows */
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
#ifdef DISC_PUBLISH_TEST_PAUSE   /* test_disc_import: a pause between the check and the rename, so that
                                    without the lock a newer import would slip in */
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
#endif
#ifdef _WIN32   /* the unit tests on Windows: rename over the target as POSIX does */
        result = MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
        result = rename(from, to) == 0 ? 0 : -1;
#endif
    }
    PUBLISH_UNLOCK();
    return result;
}
