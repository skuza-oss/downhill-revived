/* The browser's network module (port/web/build_net.py): the little of SDL3 that port/host/voice.c
   uses there (its hearing side and capture_frame; the recording thread is native only). Everything
   runs on the game worker's one thread, so the atomics are plain ints and the spinlocks do
   nothing. */
#ifndef DHD_WEB_SDL_H
#define DHD_WEB_SDL_H

#include <ctype.h>
#include <stdint.h>
#include <strings.h>
#include "SDL_mutex.h"
#include "SDL_timer.h"

typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef struct { int value; } SDL_AtomicInt;
typedef int SDL_SpinLock;

static inline void SDL_SetAtomicInt(SDL_AtomicInt *a, int v) { a->value = v; }
static inline int SDL_GetAtomicInt(SDL_AtomicInt *a) { return a->value; }
static inline void SDL_LockSpinlock(SDL_SpinLock *l) { (void)l; }
static inline void SDL_UnlockSpinlock(SDL_SpinLock *l) { (void)l; }
static inline Uint64 SDL_GetTicks(void) { return SDL_GetTicksNS() / 1000000u; }
#define SDL_strcasecmp strcasecmp
#define SDL_isalnum isalnum
#define SDL_toupper toupper

#endif
