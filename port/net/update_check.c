/* update_check.h: whether a newer release is out (finding 143). */
#include "update_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

/* Release pages the notice may open: this repository's. */
static const char repository[] = "https://github.com/skuza-oss/downhill-revived/";

int update_parse_version(const char *text, unsigned out[3])
{
    const char *s = text;
    if (!s)
        return 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == 'v' || *s == 'V')
        s++;
    for (int i = 0; i < 3; i++) {
        unsigned long value = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            value = value * 10 + (unsigned long)(*s++ - '0');
            if (++digits > 9)
                return 0;
        }
        if (!digits)
            return 0;
        out[i] = (unsigned)value;
        if (i < 2 && *s++ != '.')
            return 0;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    return *s == 0;
}

int update_newer(const char *candidate, const char *current)
{
    unsigned a[3], b[3];
    if (!update_parse_version(candidate, a) || !update_parse_version(current, b))
        return 0;
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i])
            return a[i] > b[i];
    return 0;
}

int update_from_manifest(const char *json, const char *platform, const char *current,
                         char *version, size_t version_size, char *page, size_t page_size)
{
    char path[64], url[512];
    if (!json || !platform || !*platform || strlen(platform) > 32)
        return 0;
    if (!json_string(json, "version", version, version_size) || !update_newer(version, current))
        return 0;
    snprintf(path, sizeof path, "platforms.%s.url", platform);
    if (!json_string(json, path, url, sizeof url) || strncmp(url, "https://", 8))
        return 0;
    if (!json_string(json, "page", page, page_size) || strncmp(page, repository, sizeof repository - 1) ||
        strpbrk(page, " \"'<>\\"))
        snprintf(page, page_size, "%s", UPDATE_RELEASES_PAGE);
    return 1;
}

int update_due(const char *auto_update, long long now, long long last)
{
    if (auto_update && !strcmp(auto_update, "off"))
        return 0;
    return last <= 0 || now < last || now - last >= UPDATE_INTERVAL_SECONDS;
}
