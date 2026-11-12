/* The disc notice's rules (disc_notice.h, finding 208): match, no notice; differs, once, naming
   the file; dismissed, not again for that image; another image, again. Built and run by
   port/tools/test_runtime.py (check.py port). */
#include "disc_notice.h"
#include <assert.h>
#include <stdio.h>

static const char usa[] = "c8c17bcae383daf9f2feadc7f6d7476c43d659ab8d66127a299b01311f2a3476";
static const char one[] = "605a55da82099a37d6a5fe04a5ac5878635cb4bf77ea10dfa2f346a62863b04c";
static const char two[] = "da60cdc6e7d904d8e65c35865c572fcf10ef9c6ae91fda4014eb75ec66e1f9a6";

static int mentions(char lines[][DISC_NOTICE_WIDTH + 1], int n, const char *what)
{
    for (int i = 0; i < n; i++)
        if (strstr(lines[i], what)) return 1;
    return 0;
}

int main(void)
{
    char seen[DISC_NOTICE_SEEN] = "", next[DISC_NOTICE_SEEN];
    char lines[DISC_NOTICE_LINES][DISC_NOTICE_WIDTH + 1];

    /* the USA disc's files: never a notice, nothing remembered or not */
    assert(!disc_notice_due(usa, usa, NULL) && !disc_notice_due(usa, usa, seen));
    /* no answer (still checking, unreadable): no notice */
    assert(!disc_notice_due(NULL, usa, seen) && !disc_notice_due("", usa, seen) && !disc_notice_due("abc", usa, seen));

    /* differs: due, and the page names the first file that differs */
    assert(disc_notice_due(one, usa, NULL) && disc_notice_due(one, usa, seen));
    int n = disc_notice_lines("R/CNOS.RTX isn't the USA disc's", lines, DISC_NOTICE_LINES);
    assert(n > 4 && n <= DISC_NOTICE_LINES);
    for (int i = 0; i < n; i++) assert(strlen(lines[i]) <= DISC_NOTICE_WIDTH);
    assert(mentions(lines, n, "R/CNOS.RTX") && mentions(lines, n, "SCUS-97177") && mentions(lines, n, "close")
           && mentions(lines, n, "new copy") && mentions(lines, n, "Online"));
    /* a note without a file (a cut dump): said as it is */
    n = disc_notice_lines("BIG.DAT lies past the image's end", lines, DISC_NOTICE_LINES);
    assert(mentions(lines, n, "past the image's end"));
    char file[64];
    assert(disc_notice_file("R/CNOS.RTX isn't the USA disc's; 2337 files, the USA disc has 2338", file, sizeof file)
           && !strcmp(file, "R/CNOS.RTX"));
    assert(!disc_notice_file("", file, sizeof file) && !disc_notice_file(NULL, file, sizeof file));

    /* dismissed: not again for that image */
    disc_notice_seen_add(next, sizeof next, seen, one);
    strcpy(seen, next);
    assert(!disc_notice_due(one, usa, seen));
    /* another image (or the same one changed: another hash): again, and the first stays seen */
    assert(disc_notice_due(two, usa, seen));
    disc_notice_seen_add(next, sizeof next, seen, two);
    strcpy(seen, next);
    assert(!disc_notice_due(two, usa, seen) && !disc_notice_due(one, usa, seen));
    assert(!strncmp(seen, two, DISC_NOTICE_HASH));  /* newest first */
    /* dismissing again keeps one line per image */
    disc_notice_seen_add(next, sizeof next, seen, one);
    assert(!strncmp(next, one, DISC_NOTICE_HASH) && strlen(next) == 2 * (DISC_NOTICE_HASH + 1));

    /* at most DISC_NOTICE_KEPT images: the oldest goes */
    seen[0] = 0;
    char hash[DISC_NOTICE_HASH + 1];
    for (int i = 0; i < DISC_NOTICE_KEPT + 3; i++) {
        snprintf(hash, sizeof hash, "%064x", i + 1);
        disc_notice_seen_add(next, sizeof next, seen, hash);
        strcpy(seen, next);
    }
    snprintf(hash, sizeof hash, "%064x", DISC_NOTICE_KEPT + 3);
    assert(!disc_notice_due(hash, usa, seen));
    snprintf(hash, sizeof hash, "%064x", 1);
    assert(disc_notice_due(hash, usa, seen));
    assert(strlen(seen) == DISC_NOTICE_KEPT * (DISC_NOTICE_HASH + 1));

    /* the wrap cuts a word longer than a line and keeps every line in width */
    char long_note[200];
    memset(long_note, 'A', 120), long_note[120] = 0;
    n = disc_notice_lines(long_note, lines, DISC_NOTICE_LINES);
    for (int i = 0; i < n; i++) assert(strlen(lines[i]) <= DISC_NOTICE_WIDTH);
    printf("PASS: disc notice\n");
    return 0;
}
