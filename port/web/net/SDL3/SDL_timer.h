/* The browser's network module: the clock is the worker's (performance.now, net.js). */
#ifndef DHD_WEB_SDL_TIMER_H
#define DHD_WEB_SDL_TIMER_H

#include <stdint.h>

__attribute__((import_module("env"), import_name("web_now_ns"))) uint64_t SDL_GetTicksNS(void);

#endif
