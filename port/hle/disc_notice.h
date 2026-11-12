/* The disc notice's rules (finding 208), apart from the page so a plain test runs them
   (test_disc_notice.c): when the disc image's game files are not the USA disc's, the main menu
   shows one notice, once per image. An image is its game files hash, the one the disc check
   computes (dhd_disc, finding 098): a changed image has another. disc-notice.txt, next to
   iso-check.txt, lists the hashes whose notice was dismissed, one per line, newest first. */
#ifndef DHD_DISC_NOTICE_H
#define DHD_DISC_NOTICE_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum {
    DISC_NOTICE_HASH = 64,   /* hex digits of a files hash */
    DISC_NOTICE_KEPT = 16,   /* dismissed images remembered */
    DISC_NOTICE_SEEN = DISC_NOTICE_KEPT * (DISC_NOTICE_HASH + 1) + 1,  /* the file's size at most */
    DISC_NOTICE_WIDTH = 38,  /* a line of the game's popup at 640 wide */
    DISC_NOTICE_LINES = 12
};

/* Whether hash (64 hex digits) is one of seen's lines. */
static inline int disc_notice_seen(const char *seen, const char *hash)
{
    for (const char *p = seen; p && *p;) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n >= DISC_NOTICE_HASH && !strncmp(p, hash, DISC_NOTICE_HASH)) return 1;
        p = end ? end + 1 : p + n;
    }
    return 0;
}

/* Whether the notice is due: the check's answer (hash) is not the USA disc's (usa), and this
   image's notice was not dismissed before (seen: disc-notice.txt's text, or NULL). */
static inline int disc_notice_due(const char *hash, const char *usa, const char *seen)
{
    if (!hash || strlen(hash) != DISC_NOTICE_HASH || !strcmp(hash, usa)) return 0;
    return !disc_notice_seen(seen, hash);
}

/* disc-notice.txt after hash's notice was dismissed: hash first, then the others seen kept, at
   most DISC_NOTICE_KEPT lines. Returns the text's length. */
static inline size_t disc_notice_seen_add(char *out, size_t size, const char *seen, const char *hash)
{
    size_t n = 0;
    int lines = 1;
    if (size < DISC_NOTICE_HASH + 2) return 0;
    n += (size_t)snprintf(out, size, "%.*s\n", DISC_NOTICE_HASH, hash);
    for (const char *p = seen; p && *p && lines < DISC_NOTICE_KEPT;) {
        const char *end = strchr(p, '\n');
        size_t k = end ? (size_t)(end - p) : strlen(p);
        if (k == DISC_NOTICE_HASH && strncmp(p, hash, DISC_NOTICE_HASH) && n + k + 2 <= size) {
            memcpy(out + n, p, k);
            n += k;
            out[n++] = '\n';
            out[n] = 0;
            lines++;
        }
        p = end ? end + 1 : p + k;
    }
    return n;
}

/* The first file that differs, from the check's note ("R/CNOS.RTX isn't the USA disc's; ..."):
   copied into out, 1; 0 when the note names none. */
static inline int disc_notice_file(const char *note, char *out, size_t size)
{
    const char *end = note ? strstr(note, " isn't the USA disc's") : NULL;
    if (!end || end == note || size < 2) return 0;
    size_t n = (size_t)(end - note);
    if (n >= size) n = size - 1;
    memcpy(out, note, n);
    out[n] = 0;
    return 1;
}

/* text's words onto lines of at most DISC_NOTICE_WIDTH characters, from lines[at]; the next free
   line. A word longer than a line is cut. */
static inline int disc_notice_wrap(char lines[][DISC_NOTICE_WIDTH + 1], int at, int max, const char *text)
{
    const char *p = text;
    while (*p && at < max) {
        size_t n = 0, last = 0;
        while (*p == ' ') p++;
        if (!*p) break;
        while (p[n] && n <= DISC_NOTICE_WIDTH) {
            if (p[n] == ' ') last = n;
            n++;
        }
        size_t take = !p[n] && n <= DISC_NOTICE_WIDTH ? n : last ? last : DISC_NOTICE_WIDTH;
        memcpy(lines[at], p, take);
        lines[at][take] = 0;
        at++;
        p += take;
    }
    return at;
}

/* The notice's lines for the check's note: what differs, what it means, what fixes it. Returns
   how many. */
static inline int disc_notice_lines(const char *note, char lines[][DISC_NOTICE_WIDTH + 1], int max)
{
    char file[64], said[160];
    int at = disc_notice_wrap(lines, 0, max, "This disc image's game files are not the USA disc's (SCUS-97177).");
    if (disc_notice_file(note, file, sizeof file))
        snprintf(said, sizeof said, "First file that differs: %s", file);
    else
        snprintf(said, sizeof said, "%s", note && *note ? note : "It may be damaged or changed.");
    at = disc_notice_wrap(lines, at, max, said);
    if (at < max) lines[at++][0] = 0;
    at = disc_notice_wrap(lines, at, max, "The game may close unexpectedly.");
    at = disc_notice_wrap(lines, at, max, "A new copy of your own disc fixes it.");
    at = disc_notice_wrap(lines, at, max, "Online needs the USA disc's files.");
    return at;
}

#endif
