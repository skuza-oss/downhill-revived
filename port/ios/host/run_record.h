/* How the run before ended, on iOS (finding 227). iOS keeps no record of an app's end that the app
   can read (Android's ApplicationExitInfo, LastExit.java, has no counterpart; MetricKit's crash
   reports come a day late or not at all on a sideloaded build). So the app keeps its own marker,
   Documents/dhd-run.txt, written at each change of state (the launcher, the game on the screen,
   inactive, in the background, ended by host_end with its code), and the game's own crash report
   at the end of dhd.log is the sign of a crash, as on Android 10 (CrashReport.logCrash). At the
   next start run_judge turns the two into dhd-exit.txt's lines in Android's wording, which the
   game quotes into its new dhd.log as "previous run: ..." (phone_quote_last_exit), and the few
   words the launcher's note shows after a run that closed unexpectedly.
   Plain C, no Foundation: port/ios/tests/test_run_record.c runs it on any machine.
   GPL-3.0-or-later. */
#ifndef DHD_IOS_RUN_RECORD_H
#define DHD_IOS_RUN_RECORD_H

#include <stddef.h>

/* The marker's states. */
enum {
    RUN_NONE,        /* no marker (the first start, or a build before it) */
    RUN_LAUNCHER,    /* the launcher's page, no game yet */
    RUN_GAME,        /* the game on the screen, active */
    RUN_INACTIVE,    /* the game off the front (the app switcher, a call, Control Center) */
    RUN_BACKGROUND,  /* the game in the background */
    RUN_ENDED        /* the game ended itself (host_end) with code */
};

typedef struct {
    int state;
    long long since;   /* seconds since the epoch */
    int code;          /* RUN_ENDED: host_end's code */
} RunMarker;

/* dhd-run.txt's text ("state: game\nsince: 1763030000\n", "code: 0" when ended). */
void run_marker_format(const RunMarker *m, char *out, size_t size);
/* Parses that text; an unknown or missing text is RUN_NONE. */
void run_marker_parse(const char *text, RunMarker *m);

/* The game's crash report in a log's end (host.c's crash handler, guest memory's fault handler):
   its first line copied to out, 1 when there is one. */
int run_crash_line(const char *log_tail, char *out, size_t size);

/* How the run before ended. m: the marker as the run left it; log_tail: the end of that run's
   dhd.log (NULL: no log), log_time its last write. Writes dhd-exit.txt's text to text ("" when
   there is nothing to say: the first start) and returns the launcher's few words when the run
   closed unexpectedly, else NULL. */
const char *run_judge(const RunMarker *m, const char *log_tail, long long log_time, char *text, size_t size);

#endif
