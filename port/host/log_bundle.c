/* The phones' Share log file and launcher.log (log_bundle.h, finding 242). GPL-3.0-or-later. */
#include "log_bundle.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static void local_time(time_t t, struct tm *tm)
{
#ifdef _WIN32
    localtime_s(tm, &t);
#else
    localtime_r(&t, tm);
#endif
}

/* "2026-11-14 18:02:11 +0100" (Android's LastExit.date) */
static void date_text(time_t t, char *out, size_t n)
{
    struct tm tm;
    local_time(t, &tm);
    if (!strftime(out, n, "%Y-%m-%d %H:%M:%S %z", &tm)) snprintf(out, n, "?");
}

static void join(char *out, size_t n, const char *dir, const char *name)
{
    size_t len = strlen(dir);
    snprintf(out, n, "%s%s%s", dir, len && (dir[len - 1] == '/' || dir[len - 1] == '\\') ? "" : "/", name);
}

/* n bytes of in from offset at to out; 0, or -1 on a write error. A short read ends early. */
static int copy_range(FILE *in, long long at, long long n, FILE *out, char *buf, size_t size)
{
#ifdef _WIN32
    if (_fseeki64(in, at, SEEK_SET) != 0) return 0;
#else
    if (fseeko(in, (off_t)at, SEEK_SET) != 0) return 0;
#endif
    while (n > 0) {
        size_t want = n < (long long)size ? (size_t)n : size;
        size_t got = fread(buf, 1, want, in);
        if (!got) break;
        if (fwrite(buf, 1, got, out) != got) return -1;
        n -= (long long)got;
    }
    return 0;
}

/* One section: its title line and the file's head and tail. 0 when the file is not there or was
   copied, -1 on a write error. */
static int section(FILE *out, const char *dir, const char *name, const char *title, char *buf, size_t size)
{
    char path[1024], when[40];
    struct stat st;
    join(path, sizeof path, dir, name);
    if (stat(path, &st) != 0 || !(st.st_mode & S_IFREG)) return 0;
    FILE *in = fopen(path, "rb");
    if (!in) return 0;
    date_text(st.st_mtime, when, sizeof when);
    int r = fprintf(out, "\n==== %s (%s), last written %s ====\n", title, name, when) < 0 ? -1 : 0;
    long long length = (long long)st.st_size, head = LOG_BUNDLE_HEAD, tail = LOG_BUNDLE_TAIL;
    if (!r && length <= head + tail) {
        r = copy_range(in, 0, length, out, buf, size);
    } else if (!r) {
        r = copy_range(in, 0, head, out, buf, size);
        if (!r && fprintf(out, "\n[... %lld bytes left out ...]\n", length - head - tail) < 0) r = -1;
        if (!r) r = copy_range(in, length - tail, tail, out, buf, size);
    }
    fclose(in);
    return r;
}

static int exists(const char *dir, const char *name)
{
    char path[1024];
    struct stat st;
    join(path, sizeof path, dir, name);
    return stat(path, &st) == 0 && (st.st_mode & S_IFREG);
}

int log_bundle_write(const char *logs_dir, const char *out_dir, const LogBundle *b, char *path, size_t path_size)
{
    static const char *const names[] = {"dhd-exit.txt", "launcher.log", "dhd.log", "dhd-previous.log"};
    int any = 0;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) any |= exists(logs_dir, names[i]);
    if (!any) return 1;
    time_t now = time(NULL);
    struct tm tm;
    char day[16], name[160], when[40];
    local_time(now, &tm);
    strftime(day, sizeof day, "%Y-%m-%d", &tm);
    snprintf(name, sizeof name, "downhill-revived-%s-log-%s.txt", b->version, day);
    join(path, path_size, out_dir, name);
    FILE *out = fopen(path, "wb");
    if (!out) return -1;
    const size_t size = 64u << 10;
    char *buf = malloc(size);
    if (!buf) { fclose(out); errno = ENOMEM; return -1; }
    date_text(now, when, sizeof when);
    int r = fprintf(out, "Downhill Revived %s log, shared %s\ndevice: %s\n", b->version, when, b->device) < 0 ? -1 : 0;
    const char *ended = b->running ? "dhd-previous.log" : "dhd.log", *other = b->running ? "dhd.log" : "dhd-previous.log";
    if (!r) r = section(out, logs_dir, "dhd-exit.txt", "how the last run ended", buf, size);
    if (!r) r = section(out, logs_dir, "launcher.log", "the launcher's steps", buf, size);
    if (!r) r = section(out, logs_dir, ended, "the last run", buf, size);
    if (!r) r = section(out, logs_dir, other, b->running ? "this run, still going" : "the run before it", buf, size);
    free(buf);
    int saved = errno;
    if (fclose(out) != 0 && !r) { r = -1; saved = errno; }
    if (r) { remove(path); errno = saved; }
    return r;
}

static atomic_flag s_log_lock = ATOMIC_FLAG_INIT;

void launcher_log_line(const char *logs_dir, const char *line)
{
    char path[1024], stamp[32];
    join(path, sizeof path, logs_dir, "launcher.log");
    time_t now = time(NULL);
    struct tm tm;
    local_time(now, &tm);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    while (atomic_flag_test_and_set_explicit(&s_log_lock, memory_order_acquire)) {}
    struct stat st;
    FILE *f = fopen(path, stat(path, &st) == 0 && st.st_size > (long long)LAUNCHER_LOG_MAX ? "wb" : "ab");
    if (f) {
        fprintf(f, "%s %s\n", stamp, line);
        fclose(f);
    }
    atomic_flag_clear_explicit(&s_log_lock, memory_order_release);
}
