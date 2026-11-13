/* How the run before ended, on iOS: run_record.h. GPL-3.0-or-later. */
#include "run_record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const names[] = {"none", "launcher", "game", "inactive", "background", "ended"};

void run_marker_format(const RunMarker *m, char *out, size_t size)
{
    int state = m->state >= RUN_NONE && m->state <= RUN_ENDED ? m->state : RUN_NONE;
    if (state == RUN_ENDED)
        snprintf(out, size, "state: %s\nsince: %lld\ncode: %d\n", names[state], m->since, m->code);
    else
        snprintf(out, size, "state: %s\nsince: %lld\n", names[state], m->since);
}

void run_marker_parse(const char *text, RunMarker *m)
{
    memset(m, 0, sizeof *m);
    if (!text) return;
    const char *s = strstr(text, "state: ");
    if (s) {
        s += 7;
        for (int i = RUN_LAUNCHER; i <= RUN_ENDED; i++) {
            size_t n = strlen(names[i]);
            if (!strncmp(s, names[i], n) && (s[n] == '\n' || s[n] == '\r' || s[n] == 0)) m->state = i;
        }
    }
    const char *t = strstr(text, "since: ");
    if (t) m->since = strtoll(t + 7, NULL, 10);
    const char *c = strstr(text, "code: ");
    if (c) m->code = atoi(c + 6);
}

/* A line of text that starts at p, up to its end, into out. */
static void line_at(const char *p, char *out, size_t size)
{
    size_t n = strcspn(p, "\r\n");
    if (n >= size) n = size - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

int run_crash_line(const char *log_tail, char *out, size_t size)
{
    static const char *const signals[] = {"crash: SIGSEGV at ", "crash: SIGBUS at ", "crash: SIGILL at ",
                                          "crash: SIGFPE at "};
    if (!log_tail || !size) return 0;
    const char *first = NULL;
    for (size_t i = 0; i < sizeof signals / sizeof *signals; i++) {
        const char *p = strstr(log_tail, signals[i]);
        if (p && (!first || p < first)) first = p;
    }
    const char *memory = strstr(log_tail, "host64 memory: access of uncommitted guest address 0x");
    if (memory && (!first || memory < first)) first = memory;
    if (!first) return 0;
    line_at(first, out, size);
    return 1;
}

static void date(long long seconds, char *out, size_t size)
{
    time_t t = (time_t)seconds;
    struct tm local;
#ifdef _WIN32   /* the unit test on Windows */
    int ok = seconds > 0 && !localtime_s(&local, &t);
#else
    int ok = seconds > 0 && localtime_r(&t, &local);
#endif
    if (!ok || !strftime(out, size, "%Y-%m-%d %H:%M:%S %z", &local))
        snprintf(out, size, "an unknown time");
}

const char *run_judge(const RunMarker *m, const char *log_tail, long long log_time, char *text, size_t size)
{
    static const char crashed[] = "It crashed.";
    static const char stopped[] = "It stopped while on the screen without a crash report (iOS closes an app "
                                  "that runs out of memory or stops responding).";
    char when[64], crash[400], record[160];
    text[0] = 0;
    int state = m ? m->state : RUN_NONE;
    if (state == RUN_NONE) {
        if (log_tail) snprintf(text, size, "how it ended: no record (the run before kept none)\n");
        return NULL;
    }
    date(m->since, when, sizeof when);
    snprintf(record, sizeof record, "system record: none (iOS keeps none an app can read); the app's own: %s since %s\n",
             names[state], when);
    /* the game's crash handler wrote its report and ended the process: no host_end, so the marker
       says where the game was */
    if (state != RUN_ENDED && state != RUN_LAUNCHER && run_crash_line(log_tail, crash, sizeof crash)) {
        char last[64];
        date(log_time, last, sizeof last);
        snprintf(text, size, "how it ended: closed unexpectedly, it crashed (%s, the log's last write)\n%s"
                 "the game's crash report: %s\n", last, record, crash);
        return crashed;
    }
    switch (state) {
    case RUN_ENDED:
        if (m->code) snprintf(text, size, "how it ended: the game ended itself with code %d (%s)\n%s", m->code, when, record);
        else snprintf(text, size, "how it ended: normally (%s)\n%s", when, record);
        return NULL;
    case RUN_LAUNCHER:
        snprintf(text, size, "how it ended: normally, in the launcher (%s)\n%s", when, record);
        return NULL;
    case RUN_BACKGROUND:   /* iOS ends suspended apps for memory all the time; the player too */
        snprintf(text, size, "how it ended: normally, in the background (iOS or the player closed it; there since %s)\n%s",
                 when, record);
        return NULL;
    case RUN_INACTIVE:     /* the app switcher: the player swiped it away */
        snprintf(text, size, "how it ended: normally, off the screen (closed from the app switcher, or by iOS; "
                 "inactive since %s)\n%s", when, record);
        return NULL;
    default:               /* on the screen, and no crash report */
        snprintf(text, size, "how it ended: closed unexpectedly, it stopped while on the screen without a crash "
                 "report: iOS closes an app that runs out of memory or stops responding (on the screen since %s)\n%s",
                 when, record);
        return stopped;
    }
}
