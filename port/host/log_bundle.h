/* The phones' logs for a report (finding 242): launcher.log's lines, and Share log's one text file,
   written the same way on Android (Native.logBundle, android_launcher.c) and iOS (launcher.m).
   The platform keeps its share sheet and what only it knows: the device's words, whether a game is
   running in this process now (Android: a dhd.log written since the process started), and the
   files it writes into the logs folder itself (Android's dhd-exit.txt from LastExit.java with its
   startup record, iOS's from run_record.c).

   The file: "Downhill Revived VERSION log, shared DATE", "device: DEVICE", then each of these that
   exists, as "==== TITLE (NAME), last written DATE ====" and its text: dhd-exit.txt (how the last
   run ended), launcher.log (the launcher's steps: the disc's import), the log of the run that ended,
   then the other one. Which is which: with a game running, dhd.log is that run and
   dhd-previous.log the one that ended (host.c moved it at the start); otherwise dhd.log ended.
   A file longer than LOG_BUNDLE_HEAD + LOG_BUNDLE_TAIL keeps its start (the settings and device
   lines) and its end (a crash), with "[... N bytes left out ...]" between; it is copied through one
   64 KiB buffer, so a big log costs no memory. GPL-3.0-or-later. */
#ifndef DHD_LOG_BUNDLE_H
#define DHD_LOG_BUNDLE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_BUNDLE_HEAD (256u << 10)
#define LOG_BUNDLE_TAIL (1536u << 10)
#define LAUNCHER_LOG_MAX (256u << 10)   /* launcher.log starts again past this */

typedef struct LogBundle {
    const char *version;   /* "0.8.3" */
    const char *device;    /* the platform's words: maker, model, OS and version */
    int running;           /* a game runs in this process now: dhd.log is its log */
} LogBundle;

/* Writes the shared file into out_dir from the logs in logs_dir (no trailing '/' needed), named
   downhill-revived-VERSION-log-YYYY-MM-DD.txt, its path to path. 0: written; 1: none of the files
   exists, nothing written; -1: it can't be written (errno says why). */
int log_bundle_write(const char *logs_dir, const char *out_dir, const LogBundle *b, char *path, size_t path_size);

/* One line, stamped with the local time, appended to logs_dir's launcher.log (started again once it
   is past LAUNCHER_LOG_MAX). Safe from several threads. */
void launcher_log_line(const char *logs_dir, const char *line);

#ifdef __cplusplus
}
#endif

#endif
