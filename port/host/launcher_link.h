/* Which links the launcher page may open: one rule for every native bridge (desktop main.rs
   through launcher_link.c, Android LauncherBridge through android_launcher.c, iOS
   launcher_calls.c; finding 235). GPL-3.0-or-later.

   The page's message is untrusted, so the rule stays native; the OS call that opens the link
   stays per platform. Accepted: the aliases "discord" and "kofi" (resolved to the project's
   invite and page) and this repository's pages, which is every link the page sends (Help, the
   release page, the notes fallback, the disc problem's More link). Refused: anything else,
   whitespace and control characters, quotes, angle brackets, backslashes and dot segments,
   plain or percent-encoded. The release manifest's URLs keep their own, narrower rule
   (net/update_check.c, the launcher's update.rs). */
#ifndef DHD_LAUNCHER_LINK_H
#define DHD_LAUNCHER_LINK_H
#include <string.h>

#define LAUNCHER_LINK_DISCORD "https://discord.gg/XrSeTyzfr2"
#define LAUNCHER_LINK_KOFI "https://ko-fi.com/skuza"
#define LAUNCHER_LINK_REPOSITORY "https://github.com/skuza-oss/downhill-revived"

/* The URL to open (url itself or an alias's target, static storage), or NULL when refused. */
static inline const char *launcher_link(const char *url)
{
    static const char repository[] = LAUNCHER_LINK_REPOSITORY;
    if (!url) return NULL;
    if (!strcmp(url, "discord")) return LAUNCHER_LINK_DISCORD;
    if (!strcmp(url, "kofi")) return LAUNCHER_LINK_KOFI;
    if (strncmp(url, repository, sizeof repository - 1)) return NULL;
    const char next = url[sizeof repository - 1];   /* not downhill-revived.evil or -other */
    if (next && next != '/' && next != '?' && next != '#') return NULL;
    if (strstr(url, "..")) return NULL;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p) {
        if (*p <= ' ' || *p == 0x7f || strchr("\"'<>\\", *p)) return NULL;
        if (p[0] == '%' && p[1] == '2' && (p[2] == 'e' || p[2] == 'E')) return NULL;
    }
    return url;
}
#endif
