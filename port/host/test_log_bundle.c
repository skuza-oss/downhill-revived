/* Share log's file and launcher.log (log_bundle.c) on fixture logs: the header, the sections'
   order with and without a running game, a long log's head and tail, nothing to share, and
   launcher.log starting again. Usage: test_log_bundle DIR (an empty scratch folder).
   GPL-3.0-or-later. */
#include "log_bundle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_dir[512];

static void path_of(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", g_dir, name); }

static void put(const char *name, const char *text, size_t repeat)
{
    char path[640];
    path_of(path, sizeof path, name);
    FILE *f = fopen(path, "wb");
    assert(f);
    for (size_t i = 0; i < repeat; i++) fputs(text, f);
    fclose(f);
}

static void drop(const char *name)
{
    char path[640];
    path_of(path, sizeof path, name);
    remove(path);
}

static char *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    assert(s && fread(s, 1, (size_t)n, f) == (size_t)n);
    s[n] = 0;
    fclose(f);
    if (size) *size = (size_t)n;
    return s;
}

/* The section titles in s, in order, as one string "a|b|..." */
static void titles(const char *s, char *out, size_t n)
{
    out[0] = 0;
    for (const char *p = s; (p = strstr(p, "\n==== ")); p += 6) {
        const char *open = strchr(p + 6, '('), *close = open ? strchr(open, ')') : NULL;
        assert(open && close);
        size_t len = strlen(out);
        snprintf(out + len, n - len, "%s%.*s", len ? "|" : "", (int)(close - open - 1), open + 1);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    char path[640], order[256];
    LogBundle b = {"9.9.9", "Maker Model, Android 15 (API 35)", 0};

    /* nothing at all: nothing written */
    drop("dhd-exit.txt"); drop("launcher.log"); drop("dhd.log"); drop("dhd-previous.log");
    assert(log_bundle_write(g_dir, g_dir, &b, path, sizeof path) == 1);

    /* every file, no game running: dhd.log is the run that ended */
    put("dhd-exit.txt", "previous run: it crashed\n", 1);
    put("launcher.log", "2026-11-14 10:00:00 import: start\n", 1);
    put("dhd.log", "dhd: ended run\n", 1);
    put("dhd-previous.log", "dhd: older run\n", 1);
    assert(log_bundle_write(g_dir, g_dir, &b, path, sizeof path) == 0);
    assert(strstr(path, "downhill-revived-9.9.9-log-"));
    char *s = slurp(path, NULL);
    assert(!strncmp(s, "Downhill Revived 9.9.9 log, shared ", 35));
    assert(strstr(s, "\ndevice: Maker Model, Android 15 (API 35)\n"));
    titles(s, order, sizeof order);
    assert(!strcmp(order, "dhd-exit.txt|launcher.log|dhd.log|dhd-previous.log"));
    assert(strstr(s, "==== the last run (dhd.log), last written "));
    assert(strstr(s, "==== the run before it (dhd-previous.log), last written "));
    assert(strstr(s, "dhd: ended run\n") < strstr(s, "dhd: older run\n"));
    free(s);

    /* a game running: dhd-previous.log ended, dhd.log is this run's, after it */
    b.running = 1;
    assert(log_bundle_write(g_dir, g_dir, &b, path, sizeof path) == 0);
    s = slurp(path, NULL);
    titles(s, order, sizeof order);
    assert(!strcmp(order, "dhd-exit.txt|launcher.log|dhd-previous.log|dhd.log"));
    assert(strstr(s, "==== this run, still going (dhd.log)"));
    free(s);
    b.running = 0;

    /* only some files: the others are left out */
    drop("dhd-exit.txt"); drop("launcher.log"); drop("dhd-previous.log");
    assert(log_bundle_write(g_dir, g_dir, &b, path, sizeof path) == 0);
    s = slurp(path, NULL);
    titles(s, order, sizeof order);
    assert(!strcmp(order, "dhd.log"));
    free(s);

    /* a long log: its first LOG_BUNDLE_HEAD bytes, the gap's size, its last LOG_BUNDLE_TAIL bytes */
    const size_t line = 64, lines = (LOG_BUNDLE_HEAD + LOG_BUNDLE_TAIL) / line + 1000;
    char text[65];
    memset(text, 'x', line - 1);
    text[line - 1] = '\n';
    text[line] = 0;
    put("dhd.log", "START\n", 1);
    {
        snprintf(path, sizeof path, "%s/dhd.log", g_dir);
        FILE *f = fopen(path, "ab");
        for (size_t i = 0; i < lines; i++) fputs(text, f);
        fputs("THE END\n", f);
        fclose(f);
    }
    size_t total = 6 + lines * line + 8, size = 0;
    assert(log_bundle_write(g_dir, g_dir, &b, path, sizeof path) == 0);
    s = slurp(path, &size);
    char gap[80];
    snprintf(gap, sizeof gap, "\n[... %llu bytes left out ...]\n",
             (unsigned long long)(total - LOG_BUNDLE_HEAD - LOG_BUNDLE_TAIL));
    char *start = strstr(s, "START\n"), *cut = strstr(s, gap);
    assert(start && cut && (size_t)(cut - start) == LOG_BUNDLE_HEAD);
    assert(size - (size_t)(cut + strlen(gap) - s) == LOG_BUNDLE_TAIL);
    assert(!strcmp(s + size - 8, "THE END\n"));
    free(s);

    /* launcher.log: lines appended with a stamp; started again past LAUNCHER_LOG_MAX */
    drop("launcher.log");
    launcher_log_line(g_dir, "picker: shown");
    launcher_log_line(g_dir, "picker: cancelled");
    path_of(path, sizeof path, "launcher.log");
    s = slurp(path, &size);
    assert(size == (20 + 13 + 1) + (20 + 17 + 1));  /* "YYYY-MM-DD HH:MM:SS " + text + '\n' each */
    assert(strstr(s, " picker: shown\n") && strstr(s, " picker: cancelled\n"));
    free(s);
    put("launcher.log", text, LAUNCHER_LOG_MAX / line + 1);
    launcher_log_line(g_dir, "import: start");
    s = slurp(path, &size);
    assert(size == 20 + 14 && strstr(s, " import: start\n"));
    free(s);

    drop("launcher.log"); drop("dhd.log");
    printf("log_bundle: ok\n");
    return 0;
}
