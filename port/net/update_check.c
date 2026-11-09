/* update_check.h: whether a newer release is out (finding 143). */
#include "update_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

/* Release pages the notice may open: this repository's. */
static const char repository[] = "https://github.com/skuza-oss/downhill-revived/";
/* The platform's download: one of this repository's release assets (the launcher's from_backup,
   update.rs, takes only these too). */
static const char release_assets[] = "https://github.com/skuza-oss/downhill-revived/releases/download/";

/* "X.Y.Z" (a leading "v"; a prerelease's "-suffix" of letters, digits, dots and dashes): its numbers,
   and in *release whether it is a release. A prerelease comes before its release (the launcher's
   rule, update.rs version_key). */
static int parse_key(const char *text, unsigned out[3], int *release)
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
    *release = *s != '-';
    if (*s == '-') {
        const char *suffix = ++s;
        while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '.' || *s == '-')
            s++;
        if (s == suffix)
            return 0;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    return *s == 0;
}

int update_parse_version(const char *text, unsigned out[3])
{
    int release;
    return parse_key(text, out, &release);
}

int update_newer(const char *candidate, const char *current)
{
    unsigned a[3], b[3];
    int a_release, b_release;
    if (!parse_key(candidate, a, &a_release) || !parse_key(current, b, &b_release))
        return 0;
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i])
            return a[i] > b[i];
    return a_release > b_release;
}

/* An address that starts with prefix and has nothing after it that could take a browser or a
   download out of it: no dot segment, plain or percent-encoded (%2e, any case), no backslash
   (browsers read it as a slash), no space, quote or angle bracket. The launcher's rule (update.rs
   under()), and the server's (latest.ts isRepositoryPage). */
static int under(const char *address, const char *prefix, size_t prefix_size)
{
    size_t n = prefix_size - 1;
    if (strncmp(address, prefix, n) || strpbrk(address, " \"'<>\\") || strstr(address + n, ".."))
        return 0;
    for (const char *s = address + n; *s; s++)
        if (s[0] == '%' && s[1] == '2' && (s[2] == 'e' || s[2] == 'E'))
            return 0;
    return 1;
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
    if (!json_string(json, path, url, sizeof url) || !under(url, release_assets, sizeof release_assets) ||
        !url[sizeof release_assets - 1])
        return 0;
    if (!json_string(json, "page", page, page_size) || !under(page, repository, sizeof repository))
        snprintf(page, page_size, "%s", UPDATE_RELEASES_PAGE);
    return 1;
}

int update_find(update_fetch fetch, void *context, const char *const *sources, size_t count,
                const char *platform, const char *current,
                char *version, size_t version_size, char *page, size_t page_size)
{
    for (size_t i = 0; i < count; i++) {
        const char *body = fetch(context, sources[i]);
        if (body && update_from_manifest(body, platform, current, version, version_size, page, page_size))
            return (int)i + 1;
    }
    return 0;
}

int update_due(const char *auto_update, long long now, long long last)
{
    if (auto_update && !strcmp(auto_update, "off"))
        return 0;
    return last <= 0 || now < last || now - last >= UPDATE_INTERVAL_SECONDS;
}
