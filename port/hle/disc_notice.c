/* The disc notice (finding 208): when the disc check (online_menu.c, dhd_disc) finds that the
   disc image's game files are not the USA disc's, the main menu shows one notice in the game's
   own popup (menu_page.c), once per image: what differs, that the game may close unexpectedly,
   that a new copy of the player's own disc fixes it, and that online needs the USA disc's files.
   OK (or Triangle) dismisses it, and disc-notice.txt in the player's data folder remembers the
   image (its game files hash; disc_notice.h has the rules). The main menu only: never over a race
   or a movie; a check that finishes elsewhere waits for it. Offline play goes on; nothing is sent. */
#include "hle.h"
#include "menu_page.h"
#include "disc_notice.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void disc_check_start(void);                                       /* online_menu.c */
const char *disc_check_answer(const char **why, const char **usa);  /* online_menu.c */
void disc_data_path(char *out, size_t size, const char *name);      /* online_menu.c */

static MenuPage page;
static char lines[DISC_NOTICE_LINES][DISC_NOTICE_WIDTH + 1];
static int line_count;
static int settled;  /* shown, or not due, this run: nothing more to do */

static void seen_path(char *out, size_t size) { disc_data_path(out, size, "disc-notice.txt"); }

/* disc-notice.txt's text into out ("" when there is none). */
static void seen_read(char *out, size_t size)
{
    char path[HOST_PATH_MAX];
    out[0] = 0;
    seen_path(path, sizeof path);
    HostFile f = host_file_open(path, HOST_FILE_READ);
    if (f == HOST_FILE_NONE) return;
    int64_t n = host_file_read(f, out, size - 1);
    host_file_close(f);
    out[n > 0 ? n : 0] = 0;
}

static void seen_write(const char *text)
{
    char path[HOST_PATH_MAX];
    seen_path(path, sizeof path);
    HostFile f = host_file_open(path, HOST_FILE_WRITE | HOST_FILE_TRUNCATE);
    if (f == HOST_FILE_NONE) {
        hle_log("disc notice: %s can't be written: the notice comes again at the next start\n", "disc-notice.txt");
        return;
    }
    host_file_write(f, text, strlen(text));
    host_file_close(f);
}

static void page_build(MenuPage *p)
{
    for (int i = 0; i < line_count; i++) menu_page_text(p, lines[i]);
    menu_page_row(p, "OK", 1);
}

/* The patched main menu, every frame (port/patches_exit.py): starts the disc check once, and
   says whether the notice is due now (the check has answered, the image differs, and its notice
   was never dismissed). */
int port_disc_notice_due(void)
{
    const char *why, *usa, *hash;
    static char seen[DISC_NOTICE_SEEN];
    disc_check_start();
    if (settled || !(hash = disc_check_answer(&why, &usa))) return 0;
    seen_read(seen, sizeof seen);
    if (!disc_notice_due(hash, usa, seen)) {
        settled = 1;
        return 0;
    }
    return 1;
}

/* The notice, modally in the main menu; then remembered. */
void port_disc_notice(void)
{
    const char *why, *usa, *hash = disc_check_answer(&why, &usa);
    static char seen[DISC_NOTICE_SEEN], next[DISC_NOTICE_SEEN];
    settled = 1;
    if (!hash) return;
    line_count = disc_notice_lines(why, lines, DISC_NOTICE_LINES);
    page.title = "DISC IMAGE";
    page.build = page_build;
    page.accept = NULL;  /* OK: close */
    page.back = NULL;    /* Triangle: close too */
    hle_log("disc notice: shown (the disc image's game files aren't the USA disc's: %s)\n", why);
    menu_page_run(&page);
    seen_read(seen, sizeof seen);
    disc_notice_seen_add(next, sizeof next, seen, hash);
    seen_write(next);
    hle_log("disc notice: dismissed; not shown again for this image\n");
}
