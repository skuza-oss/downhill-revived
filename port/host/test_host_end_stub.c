/* host_end.h for the test programs that link a unit which registers end handlers (the graphics
   queues, the VIF's mesh counters, the pads, the native renderer, the SDL host) but never ends the
   process through host_end: a plain program, so the handlers run at exit() as atexit's did. The
   real host_end.c is tested by test_host_end.c. */
#include <stdlib.h>
#include "host_end.h"

void host_end_add(int stage, const char *name, void (*fn)(void))
{
    (void)stage;
    (void)name;
    atexit(fn);
}
