/* PC SETTINGS: the page the front end's OPTIONS screen opens (item 9, port/patches.py patches
   func_001F0160), a page in the game's own popup menu style (menu_page.c, finding 050) as its
   AUDIO / OTHER page is; up/down, the cursor, the font, the panel and the sounds are the game's:
   - left/right change the row under the cursor; Cross (SELECT) keeps the changes and closes,
     Triangle (BACK) puts the values the page opened with back and closes, as AUDIO / OTHER does.
   Every change is saved to dhd.ini at once (host_setting_save keeps the other lines). Display,
   window size, resolution, picture shape, scaling, skip movies (the startup movies), vibration,
   V-Sync, the frame rate (above 60 at once, else at the next screen) and unlock all apply at once
   (finding 124); widescreen, the texture pack and the font at the next start ("*" and the footer
   say so). A setting the environment
   sets (DHD_*) is shown but can't be changed here. Nothing here writes game state: the page
   lives in host memory and the settings in dhd.ini, never in the memory card's save.
   A host that can do only some of the rows says which in DHD_PC_SETTINGS_ROWS, a comma list of
   the rows' variables; unset, every row is shown. A host that applies more rows at once than the
   ones marked live below lists their variables in DHD_PC_SETTINGS_LIVE (the row then sets its
   variable at once and the host follows it), and DHD_PC_SETTINGS_RESTART replaces the footer of
   a row that waits for the next start. The browser sets all three (port/web/www/settings.js) and
   lists DHD_SCALE live, which the x64 page now marks live itself; x64 sets none of them. */
#include "hle.h"
#include "../runtime/portable.h"
#include "menu_page.h"
#include "pad_preview.h"
#include "../include/fps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_table.h"  /* the settings' choices: generated from port/render/settings */

void gs_window_apply(void);           /* port/gs/present.c */
void gs_present_options_apply(void);
int texpack_available(void);          /* port/gs/texpack.c */

enum { ROWS = 13, MAX_CHOICES = 16 };

/* A row: its label and dhd.ini key; the setting's variable, built-in value and choices come from
   setting_table (dhd_settings), the list from choices_build. */
typedef struct {
    const char *label, *key;
    int live;
    const char *env, *builtin;
    SettingChoice choices[MAX_CHOICES];
    int count;
    int selected, running, opened;    /* choice indexes; -1: a value not in the list */
} Row;

enum { RESOLUTION, DISPLAY, SIZE, SHAPE, SCALING, WIDESCREEN, MOVIES, VIBRATION, TEXTURES, FONT, RATE, VSYNC, UNLOCK };
static Row rows[ROWS] = {
    {"Resolution:", "scale", 1},  /* the native renderer's DHD_SCALE, at once (finding 124) */
    {"Display:", "window", 1},
    {"Window Size:", "size", 1},  /* width and height */
    {"Picture:", "DHD_ASPECT", 1},
    {"Scaling:", "DHD_PRESENT_FILTER", 1},
    {"Widescreen:", "widescreen", 0},
    {"Skip Movies:", "skip_movies", 1},  /* the startup movies */
    {"Vibration:", "vibration", 1},
    {"Texture Pack:", "textures", 0},  /* port/gs/texpack.c */
    {"Font:", "font", 0},  /* port/gs/font_sharp.c */
    {"Frame Rate:", "fps", 1},  /* fps.c: at once above 60, else at the next screen */
    {"V-Sync:", "vsync", 1},  /* no tearing (port/gs/gpu_rust.c), at once (finding 124) */
    {"Unlock All:", "unlock_all", 1},  /* port_unlock_all */
};
static char monitor_size[24];

/* Unlock All (dhd.ini "unlock_all", DHD_UNLOCK_ALL): the game's unlock checks answer as if
   everything were unlocked, while it is on: tracks, riders and pro riders, sponsor rows, the
   shop's items and the preset bikes (patches.py's track mask, patches_unlock.py; finding 095).
   The save's own unlocks are never written, so turning it off brings them back. */
int port_unlock_all(void)
{
    static HleEnv env;
    const char *v = hle_env(&env, "DHD_UNLOCK_ALL");
    return v && atoi(v) != 0;
}

static int loaded;

/* Each row's list: the setting's choices, but Resolution up to what the renderer draws
   (DHD_SCALE_MAX, which the Rust native renderer sets: 8, or less on a card with little memory; 4
   on the others) and Frame Rate up to port_fps_max (240; the browser sets DHD_FPS_MAX 144), each
   then Window or Monitor; Window Size with the monitor's own size (DHD_MONITOR_MODE, present.c) in
   place of "monitor" when it isn't one of them. */
static void choices_build(void)
{
    const char *most = host_getenv("DHD_SCALE_MAX"), *mode = host_getenv("DHD_MONITOR_MODE");
    int max_scale = most && atoi(most) > 4 ? atoi(most) : 4, w = 0, h = 0;
    if (mode && sscanf(mode, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) snprintf(monitor_size, sizeof monitor_size, "%dx%d", w, h);
    for (int i = 0; i < ROWS; i++) {
        Row *r = &rows[i];
        const SettingInfo *info = NULL;
        for (unsigned j = 0; j < sizeof setting_table / sizeof *setting_table && !info; j++)
            if (!strcmp(setting_table[j].key, r->key)) info = &setting_table[j];
        if (!info) continue;
        r->env = i == SIZE ? "DHD_WINDOW_WIDTH" : info->env;
        r->builtin = info->builtin;
        r->count = 0;
        for (int j = 0; j < info->count && r->count < MAX_CHOICES; j++) {
            SettingChoice c = info->choices[j];
            int number = atoi(c.value);
            if (i == RESOLUTION && number > max_scale) continue;
            if (i == RATE && number > port_fps_max()) continue;
            if (i == SIZE && !strcmp(c.value, "monitor")) continue;
            r->choices[r->count++] = c;
        }
        if (i == SIZE && *monitor_size) {
            int known = 0;
            for (int j = 0; j < r->count; j++) known |= !strcmp(r->choices[j].value, monitor_size);
            if (!known && r->count < MAX_CHOICES) r->choices[r->count++] = (SettingChoice){monitor_size, monitor_size};
        }
    }
}
static char footer[64];
static const char *restart_footer = "* Applies when the game starts again";
static int shown[ROWS], shown_count;  /* the page's rows: indexes into rows[] */

/* Whether env is one of list's comma-separated names. */
static int listed(const char *list, const char *env)
{
    size_t n = strlen(env);
    for (const char *at = list; at && (at = strstr(at, env)); at += n)
        if ((at == list || at[-1] == ',') && (at[n] == 0 || at[n] == ',')) return 1;
    return 0;
}

/* The rows the host can do (DHD_PC_SETTINGS_ROWS, else all), the ones it applies at once besides
   the live ones (DHD_PC_SETTINGS_LIVE) and its words for the others (DHD_PC_SETTINGS_RESTART). */
static void choose_rows(void)
{
    const char *list = host_getenv("DHD_PC_SETTINGS_ROWS"), *live = host_getenv("DHD_PC_SETTINGS_LIVE");
    const char *restart = host_getenv("DHD_PC_SETTINGS_RESTART");
    if (restart && *restart) restart_footer = restart;
    shown_count = 0;
    for (int i = 0; i < ROWS; i++) {
        if (!list || listed(list, rows[i].env)) shown[shown_count++] = i;
        if (live && listed(live, rows[i].env)) rows[i].live = 1;
    }
}

/* The button row "Controls..." (finding 124): after the rows this host shows. */
static int controls_row(void) { return shown_count; }

static Row *row_at(int index) { return index >= 0 && index < shown_count ? &rows[shown[index]] : NULL; }

/* The value the program runs with now: the variable (set from dhd.ini, in a choice's own spelling,
   or the environment), else the game's built-in one. */
static void current(const Row *r, char *out, size_t size)
{
    if (r == &rows[SIZE]) {
        const char *w = host_getenv("DHD_WINDOW_WIDTH"), *h = host_getenv("DHD_WINDOW_HEIGHT");
        snprintf(out, size, "%dx%d", w && atoi(w) > 0 ? atoi(w) : 896, h && atoi(h) > 0 ? atoi(h) : 672);
        return;
    }
    const char *v = host_getenv(r->env);
    snprintf(out, size, "%s", v && *v ? v : r->builtin);
}

static int find(const Row *r, const char *value)
{
    for (int i = 0; i < r->count; i++)
        if (!dhd_stricmp(r->choices[i].value, value)) return i;
    return -1;
}

static int locked(const Row *r)
{
    return host_service_host_setting_locked(GADDR(r->env)) || (r == &rows[SIZE] && host_service_host_setting_locked(GADDR("DHD_WINDOW_HEIGHT")));
}

static MenuPage page;

/* A row on the page: "Label:  < Value >", "*" after it while it waits for the next start; no
   arrows when the environment sets it. */
static void format(Row *r)
{
    char value[40];
    int index = -1;
    for (int i = 0; i < shown_count; i++)
        if (&rows[shown[i]] == r) index = i;
    if (index < 0) return;
    if (r->selected >= 0) snprintf(value, sizeof value, "%s", r->choices[r->selected].label);
    else current(r, value, sizeof value);
    if (r->selected != r->running) strncat(value, " *", sizeof value - strlen(value) - 1);
    menu_page_value_set(&page, index, r->label, value, !locked(r));
}

/* Saves the row's choice and, for a live row, applies it. */
/* Switching Vibration on gives player 1's pad a short buzz, so the player feels what it does
   (pad_preview.h): page_update stops it PAD_PREVIEW_FIELDS fields later, and the preview's own
   window ends it either way; switching it off stops at once. */
static int buzz;

static void commit(Row *r, int apply)
{
    const SettingChoice *c = &r->choices[r->selected];
    host_service_host_setting_save(GADDR(r->key), GADDR(c->value));  /* "size": width and height */
    if (apply && r == &rows[SIZE]) {
        char w[16], h[16];
        if (sscanf(c->value, "%15[0-9]x%15[0-9]", w, h) == 2) { host_putenv_s("DHD_WINDOW_WIDTH", w); host_putenv_s("DHD_WINDOW_HEIGHT", h); }
    } else if (apply) host_putenv_s(r->env, c->value);
    if (apply) hle_env_changed();
    if (!apply) return;
    r->running = r->selected;
    if (r == &rows[DISPLAY] || r == &rows[SIZE]) host_service_gs_window_apply();
    else if (r == &rows[RESOLUTION] || r == &rows[SHAPE] || r == &rows[SCALING] || r == &rows[VSYNC])
        host_service_gs_present_options_apply();  /* DHD_SCALE, DHD_ASPECT, DHD_PRESENT_FILTER, DHD_VSYNC read again */
    else if (r == &rows[RATE]) port_fps_changed();
    else if (r == &rows[VIBRATION]) {
        int on = strcmp(c->value, "0") != 0;
        host_service_host_pad_vibrate(PAD_PREVIEW_BIT, on, on ? 160 : 0);
        buzz = on ? PAD_PREVIEW_FIELDS : 0;
    }
}

static void change(int index, int step)
{
    Row *r = row_at(index);
    if (!r) return;
    if (locked(r)) return;
    r->selected = r->selected < 0 ? (step > 0 ? 0 : r->count - 1) : (r->selected + step + r->count) % r->count;
    menu_page_sound(step > 0);
    commit(r, r->live);
    /* Widescreen brings its picture shape (the monitor's, "wide"); saved for the next start,
       when the wider view comes, not shown stretched now. */
    if (r == &rows[WIDESCREEN] && !locked(&rows[SHAPE])) {
        rows[SHAPE].selected = find(&rows[SHAPE], r->selected ? "wide" : "4:3");
        commit(&rows[SHAPE], 0);
        format(&rows[SHAPE]);
    }
    format(r);
    hle_log("pc settings: %s %s\n", r->label, r->choices[r->selected].value);
}

/* ---- the page ---- */

void port_controls_menu(void);  /* controls_menu.c: the rebindable controls (finding 124) */

static void page_build(MenuPage *p)
{
    for (int i = 0; i < shown_count; i++) {
        menu_page_value(p, rows[shown[i]].label, "", 1);
        format(&rows[shown[i]]);
    }
    menu_page_row(p, "Controls...", 1);  /* Cross opens the CONTROLS hub (page_accept) */
}

static void page_update(MenuPage *p, int cursor)  /* the footer for the row */
{
    const Row *r = row_at(cursor);
    if (buzz && --buzz == 0) host_service_host_pad_vibrate(PAD_PREVIEW_BIT, 0, 0);  /* the buzz is over */
    p->footer = NULL;
    if (r && locked(r)) {
        snprintf(footer, sizeof footer, "Set by %s outside the game", r == &rows[SIZE] ? "DHD_WINDOW_WIDTH" : r->env);
        p->footer = footer;
    } else if (r && r->selected != r->running) p->footer = restart_footer;
    else if (r == &rows[RATE] && port_fps_pending()) p->footer = "Applies from the next screen";
    else if (r == &rows[TEXTURES] && !host_service_texpack_available()) p->footer = "No pack: see texture_pack in dhd.ini";
}

static void page_back(MenuPage *p)  /* Triangle: the values the page opened with, close */
{
    for (int i = 0; i < ROWS; i++) {
        Row *r = &rows[i];
        if (r->selected == r->opened || r->opened < 0) continue;
        r->selected = r->opened;
        commit(r, r->live || r->opened == r->running);
    }
    menu_page_cancel(p);
}

static void page_left(MenuPage *p, int index) { (void)p; change(index, -1); }
static void page_right(MenuPage *p, int index) { (void)p; change(index, 1); }

/* Cross: on "Controls..." the CONTROLS hub over this page (finding 124), else keep and close. */
static void page_accept(MenuPage *p, int row)
{
    if (row == controls_row()) {
        port_controls_menu();
        menu_page_rebuild_at(p, controls_row());
        return;
    }
    menu_page_close(p);
}

static void open_page(void)
{
    page.title = "PC SETTINGS";
    page.build = page_build;
    page.update = page_update;
    page.accept = page_accept;  /* Cross: keep and close, or open CONTROLS on its row */
    page.back = page_back;
    page.left = page_left;
    page.right = page_right;
    if (!loaded) {  /* what runs now; restart rows keep their pending choice across visits */
        char value[32];
        choices_build();
        choose_rows();
        for (int i = 0; i < ROWS; i++) {
            current(&rows[i], value, sizeof value);
            rows[i].selected = rows[i].running = find(&rows[i], value);
        }
        loaded = 1;
    }
    for (int i = 0; i < ROWS; i++) rows[i].opened = rows[i].selected;
}

/* OPTIONS, item 9 (patched func_001F0160): run the page until it closes. */
void port_pc_settings(void)
{
    open_page();
    menu_page_run(&page);
}

/* The race's pause menu (patched func_0015E238 and func_0015E448): the page opens over it, as
   its OPTIONS does, and closing goes back to it. */
const char port_pause_pc_settings_label[] = "PC SETTINGS";
void port_pc_settings_push(void)
{
    open_page();
    menu_page_push(&page);
}

/* OPTIONS' rows (patched func_001F0160): row 9 is this page; the game's table has an unused
   "UNLOCK RIDERS/BIKES(test)" there, which stays as it is. */
char *port_options_label(char *label, int index)
{
    return index == 9 ? (char *)"PC SETTINGS" : label;
}
