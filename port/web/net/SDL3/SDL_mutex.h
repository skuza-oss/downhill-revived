/* The browser's network module (port/web/build_net.py): port/net's client runs on the game's one
   worker thread there, so its locks and its condition do nothing. */
#ifndef DHD_WEB_SDL_MUTEX_H
#define DHD_WEB_SDL_MUTEX_H

typedef struct SDL_Mutex SDL_Mutex;
typedef struct SDL_Condition SDL_Condition;

static inline SDL_Mutex *SDL_CreateMutex(void) { return (SDL_Mutex *)1; }
static inline void SDL_DestroyMutex(SDL_Mutex *m) { (void)m; }
static inline void SDL_LockMutex(SDL_Mutex *m) { (void)m; }
static inline void SDL_UnlockMutex(SDL_Mutex *m) { (void)m; }
static inline SDL_Condition *SDL_CreateCondition(void) { return (SDL_Condition *)1; }
static inline void SDL_DestroyCondition(SDL_Condition *c) { (void)c; }
static inline void SDL_BroadcastCondition(SDL_Condition *c) { (void)c; }

#endif
