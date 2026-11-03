/* Whether a newer release is out, for a game with no launcher to say so (Android, finding 143).
   The rules only; port/host/android_update.c fetches the answer and shows the notice.

   The answer is a release's latest.json (port/tools/package_release.py), as the online server's
   GET /v1/latest serves it (online/server/src/latest.ts) or as GitHub's release asset: "version",
   "page" and "platforms": {"<platform>": {"url", "sha256", "size"}}. A release counts only when it
   has a build for this platform and its version is newer than this one, compared as the launcher
   compares them (update.rs parse_version, newer): three numbers, an optional leading v. */
#ifndef NET_UPDATE_CHECK_H
#define NET_UPDATE_CHECK_H

#include <stddef.h>

/* The releases page, for an answer whose page is not this repository's. */
#define UPDATE_RELEASES_PAGE "https://github.com/skuza-oss/downhill-revived/releases/latest"
/* At most one check a day. */
#define UPDATE_INTERVAL_SECONDS (24 * 60 * 60)

/* "1.2.3" or "v1.2.3" into out; 1 when it is one. */
int update_parse_version(const char *text, unsigned out[3]);
/* 1 when candidate is a version newer than current (both must parse). */
int update_newer(const char *candidate, const char *current);
/* 1 when the latest.json text names a release newer than current with a build for platform:
   its version and its page (the repository's release page, else UPDATE_RELEASES_PAGE) copied out. */
int update_from_manifest(const char *json, const char *platform, const char *current,
                         char *version, size_t version_size, char *page, size_t page_size);
/* 1 when a check is due: auto_update (dhd.ini's value, "" when unset) is not "off", and the last
   check (seconds since the epoch, 0 never) is a day ago or more, or in the future (a clock set
   back). */
int update_due(const char *auto_update, long long now, long long last);

#endif
