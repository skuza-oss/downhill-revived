/* The previous run's record on iOS (port/ios/host/run_record.c, finding 227): every way a run can
   end, as the next start words it. Run by port/tools/test_runtime.py on the build machine and by
   port/ios/tests/test_port.py where a compiler named cc is. GPL-3.0-or-later. */
#include "../host/run_record.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *judge(int state, int code, const char *log, char *text)
{
    RunMarker m = {state, 1763030000, code};
    return run_judge(&m, log, 1763030100, text, 2048);
}

int main(void)
{
    char text[2048], buffer[256];
    RunMarker m = {RUN_ENDED, 1763030000, 3}, back;
    run_marker_format(&m, buffer, sizeof buffer);
    run_marker_parse(buffer, &back);
    assert(back.state == RUN_ENDED && back.since == 1763030000 && back.code == 3);
    for (int s = RUN_LAUNCHER; s <= RUN_BACKGROUND; s++) {
        RunMarker a = {s, 42, 0}, b;
        run_marker_format(&a, buffer, sizeof buffer);
        run_marker_parse(buffer, &b);
        assert(b.state == s && b.since == 42);
    }
    run_marker_parse("garbage", &back);
    assert(back.state == RUN_NONE);
    run_marker_parse(NULL, &back);
    assert(back.state == RUN_NONE);

    /* the first start: nothing to say */
    assert(!run_judge(NULL, NULL, 0, text, sizeof text) && !text[0]);
    /* a log from a build that kept no marker */
    assert(!run_judge(NULL, "x\n", 5, text, sizeof text) && strstr(text, "no record"));

    const char *log = "gs: frame\ncrash: SIGSEGV at 1234ABCD in thread game (fault address 0x10)\n  #00 ...\n";
    /* EXIT GAME, the lost-device message's end */
    assert(!judge(RUN_ENDED, 0, "plain log\n", text) && !strncmp(text, "how it ended: normally (", 24));
    assert(strstr(text, "system record: none (iOS keeps none an app can read); the app's own: ended"));
    assert(!judge(RUN_ENDED, 1, NULL, text) && strstr(text, "ended itself with code 1"));
    /* ended cleanly: a crash line from earlier in the log is not this end */
    assert(!judge(RUN_ENDED, 0, log, text));
    /* closed in the launcher, before any game */
    assert(!judge(RUN_LAUNCHER, 0, log, text) && strstr(text, "normally, in the launcher"));
    /* killed in the background or from the app switcher: as runs end */
    assert(!judge(RUN_BACKGROUND, 0, "x", text) && strstr(text, "normally, in the background"));
    assert(!judge(RUN_INACTIVE, 0, "x", text) && strstr(text, "app switcher"));
    /* the game's crash handler: crashed, wherever it was */
    const char *why = judge(RUN_GAME, 0, log, text);
    assert(why && !strcmp(why, "It crashed."));
    assert(strstr(text, "how it ended: closed unexpectedly, it crashed ("));
    assert(strstr(text, "the game's crash report: crash: SIGSEGV at 1234ABCD in thread game (fault address 0x10)\n"));
    assert(judge(RUN_BACKGROUND, 0, log, text) && strstr(text, "it crashed"));
    why = judge(RUN_GAME, 0, "host64 memory: access of uncommitted guest address 0x9000000\nmore\n", text);
    assert(why && strstr(text, "access of uncommitted guest address 0x9000000\n"));
    /* gone while on the screen with no report: memory or the watchdog */
    why = judge(RUN_GAME, 0, "gs: frame\n", text);
    assert(why && strstr(why, "without a crash report") && strstr(text, "closed unexpectedly"));
    /* a SIGABRT line is not one of the crash handler's reports (Android's rule) */
    assert(!run_crash_line("crash: SIGABRT at 1\n", buffer, sizeof buffer));
    puts("run_record: every end worded");
    return 0;
}
