/* The game's side of the player launcher (port/launcher, finding 093). The launcher holds the
   settings, the rider name and the updates on every platform; the game holds none of them.
   - dhd.exe started straight (a shortcut, or an older release's own update) with the launcher
     beside it opens the launcher and ends; the launcher starts the game with DHD_LAUNCHER=1.
     Tools driving the game never get the launcher.
   - The online menu's Update Now ends the game with UPDATE_EXIT_CODE; the launcher installs the
     update when it sees that code (port/launcher/src-tauri/src/game.rs). */
#include "hle.h"
#include "../host/host_api.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define LAUNCHER_NAME "DownhillRevived.exe"
#else
#define LAUNCHER_NAME "DownhillRevived"
#endif

/* 'U': under 256, so it survives a POSIX exit status as it is. */
#define UPDATE_EXIT_CODE 85

static int automated(void)
{
    static const char *const tools[] = {"DHD_NAV", "DHD_AUTOPAD_FRAMES", "DHD_RACE_SYNC", "DHD_RACE_PAD",
                                        "DHD_NET_CREATE", "DHD_NET_JOIN", "DHD_HEADLESS", "DHD_JOIN"};
    for (unsigned i = 0; i < sizeof tools / sizeof *tools; i++)
        if (getenv(tools[i])) return 1;
    return 0;
}

/* Before dhd.ini and the log, so the launcher gets the environment the game was started with.
   Nonzero: the launcher opened, end. */
int launcher_open(void)
{
    char dir[1024], path[1100];
    if (getenv("DHD_LAUNCHER") || automated()) return 0;
    unsigned n = host_exe_path(dir, sizeof dir);
    while (n && dir[n - 1] != '\\' && dir[n - 1] != '/') n--;
    dir[n] = 0;
    snprintf(path, sizeof path, "%s%s", dir, LAUNCHER_NAME);
    if (!SDL_GetPathInfo(path, NULL)) return 0;
    /* In the background (none of the game's standard handles), with its environment. The
       launcher finds its folder by its own path (game.rs), whatever the working folder. */
    const char *args[] = {path, NULL};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *launcher = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!launcher) return 0;
    SDL_DestroyProcess(launcher);  /* it runs on; only our handle to it goes */
    return 1;
}

void update_request(void)
{
    fprintf(stderr, "update: asked for from the online menu; the launcher installs it\n");
    fflush(stdout), fflush(stderr);
    host_exit(UPDATE_EXIT_CODE);
}

/* Guest-address-only module boundary; native device code stays above. */
int host_service_launcher_open(void) { return launcher_open(); }
void host_service_update_request(void) { update_request(); }
