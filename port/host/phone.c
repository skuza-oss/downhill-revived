/* What the two phone hosts share: phone.h. GPL-3.0-or-later. */
#include "phone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/version.h"
#include "../net/update_check.h"
#include "../net/ws.h"

const char PHONE_SETTINGS_ROWS[] =
    "DHD_SCALE,DHD_ASPECT,DHD_PRESENT_FILTER,DHD_WIDESCREEN,DHD_SKIP_MOVIES,DHD_VIBRATION,"
    "DHD_STICK_DEAD_ZONE,DHD_PROMPTS,DHD_TEXTURES,DHD_FONT,DHD_FPS,DHD_VSYNC,DHD_PERF_COUNTER,DHD_UNLOCK_ALL,"
    "DHD_SHADOWS,DHD_LENS_FLARE,DHD_PARTICLES,DHD_NATIVE_VU1,DHD_LAUNCHER_AT_START";

void phone_settings_rows(void)
{
    setenv("DHD_PC_SETTINGS_ROWS", PHONE_SETTINGS_ROWS, 0);
}

void phone_quote_last_exit(const char *dir)
{
    char path[1200], line[600];
    snprintf(path, sizeof path, "%sdhd-exit.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        fprintf(stderr, "previous run: %s\n", line);
    }
    fclose(f);
}

/* update_find's fetch: one GET; context counts the sources that answered. */
static const char *fetch(void *context, const char *url)
{
    static _Thread_local char body[64 * 1024];
    if (http_get(url, body, sizeof body) < 0) {
        fprintf(stderr, "launcher: %s didn't answer\n", url);
        return NULL;
    }
    ++*(int *)context;
    return body;
}

int phone_update_find(const char *platform, char *version, size_t version_size, char *page, size_t page_size)
{
    static const char *const sources[] = {UPDATE_SOURCE_SERVER, UPDATE_SOURCE_GITHUB};
    int answered = 0;
    if (update_find(fetch, &answered, sources, sizeof sources / sizeof *sources, platform, DHD_VERSION,
                    version, version_size, page, page_size))
        return 1;
    return answered ? 0 : -1;
}
