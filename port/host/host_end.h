/* How the game's process ends (finding 210). Every way the game ends by itself (EXIT GAME, the
   window's close button, the activity closing on Android, the lost-device message, a fatal error
   box, the main thread returning) calls host_end(code) instead of exit(): the end handlers run in
   stages on the calling thread, the open files are flushed, and the process leaves with
   host_exit (no C library exit handlers, no static destructors). exit() with the renderer's
   thread still drawing let a library's destructor destroy a mutex that thread then locked, and
   bionic aborts on that (the render thread's SIGABRT on a Mi 11i). A handler that hangs is
   cut off by a watchdog (DHD_END_TIMEOUT_MS, 3000 by default): logged, then the process leaves
   anyway. A plain exit() still runs the same handlers once (host_end_add hooks atexit), so a
   path that was missed ends in the same order, then proceeds to the destructors.

   Stages run in this order; inside a stage the last added runs first (as atexit). Handlers run on
   the thread that ends the process and must not wait for a thread that depends on it. */
#ifndef PORT_HOST_END_H
#define PORT_HOST_END_H

enum {
    HOST_END_WORKERS,  /* the work threads stop and are joined (the VU and GS queues); the renderer still takes frames */
    HOST_END_RENDER,   /* the render thread stops (bounded wait) */
    HOST_END_REPORT,   /* the closing log lines (perf: session, native VU1 counters); nothing draws any more */
    HOST_END_DEVICES,  /* audio, presence, vibration: what talks to the OS */
    HOST_END_LOG,      /* the log's last lines reach dhd.log */
    HOST_END_STAGES
};

/* Registers fn for stage; name is a string literal for the log. Any thread, before or while running. */
#ifdef __wasm__
/* The browser's modules have no host_end.c (one thread each, the page ends them): atexit. */
#include <stdlib.h>
static inline void host_end_add(int stage, const char *name, void (*fn)(void)) { (void)stage; (void)name; atexit(fn); }
#else
void host_end_add(int stage, const char *name, void (*fn)(void));
#endif

/* Runs the handlers, flushes the open files and ends the process with host_exit(code). Never
   returns. Called twice (another thread, or a handler): the second call waits for the first. */
void host_end(int code) __attribute__((noreturn));

#endif
