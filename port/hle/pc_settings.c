/* PC SETTINGS: the page the front end's OPTIONS screen opens (item 9, port/patches.py patches
   func_001F0160), a page in the game's own popup menu style (menu_page.c, finding 050) as its
   AUDIO / OTHER page is; up/down, the cursor, the font, the panel and the sounds are the game's:
   - left/right change the row under the cursor; Cross (SELECT) keeps the changes and closes,
     Triangle (BACK) puts the values the page opened with back and closes, as AUDIO / OTHER does.
   Every change is saved to dhd.ini at once (host_setting_save keeps the other lines). Display,
   window size, picture shape, scaling, skip movies (the startup movies), vibration, the sticks'
   dead zone, the button symbols, unlock all and FPS & Ping apply at once; resolution,
   widescreen, the texture pack, the font, the frame rate and V-Sync at the next start ("*" and
   the footer say so). A setting the environment sets (DHD_*) is shown but can't be changed here.
   Nothing here writes game state: the page lives in host memory and the settings in dhd.ini,
   never in the memory card's save.
   A host that can do only some of the rows says which in DHD_PC_SETTINGS_ROWS, a comma list of
   the rows' variables; unset, every row is shown. A host that applies more rows at once than the
   ones marked live below lists their variables in DHD_PC_SETTINGS_LIVE (the row then sets its
   variable at once and the host follows it), and DHD_PC_SETTINGS_RESTART replaces the footer of
   a row that waits for the next start. The browser sets all three (port/web/www/settings.js);
   x64 sets none of them, so its page is as described above.
   With touch (the host sets DHD_TOUCH: android, or mouse for a desktop test) the page ends with a
   Touch Controls row whose three screens are touch_settings.c's (finding 138); on Android the
   page is called SETTINGS. */
#include "hle.h"
#include "../runtime/portable.h"
#include "menu_page.h"
#include "../include/fps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_table.h"  /* the settings' choices: generated from port/render/settings */

void gs_window_apply(void);           /* port/gs/present.c */
/* touch_settings.c: the TOUCH CONTROLS screens 1-3 of this page */
const char *touch_settings_title(int page);
void touch_settings_build(MenuPage *p, int page);
void touch_settings_update(MenuPage *p, int page, int cursor);
void touch_settings_change(MenuPage *p, int page, int row, int step);
int touch_settings_accept(MenuPage *p, int page, int row);
void gs_present_options_apply(void);
int texpack_available(void);          /* port/gs/texpack.c */

enum { ROWS = 16, MAX_CHOICES = 16 };

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

enum { RESOLUTION, DISPLAY, SIZE, SHAPE, SCALING, WIDESCREEN, MOVIES, VIBRATION, DEAD_ZONE, PROMPTS, TEXTURES, FONT, RATE,
       VSYNC, COUNTER, UNLOCK };
static Row rows[ROWS] = {
    {"Resolution:", "scale", 0},
    {"Display:", "window", 1},
    {"Window Size:", "size", 1},  /* width and height */
    {"Picture:", "DHD_ASPECT", 1},
    {"Scaling:", "DHD_PRESENT_FILTER", 1},
    {"Widescreen:", "widescreen", 0},
    {"Skip Movies:", "skip_movies", 1},  /* the startup movies */
    {"Vibration:", "vibration", 1},
    {"Dead Zone:", "stick_dead_zone", 1},  /* the sticks' (stick.h; host.c follows it each field) */
    {"Button Icons:", "prompts", 1},  /* prompts.c */
    {"Texture Pack:", "textures", 0},  /* port/gs/texpack.c */
    {"Font:", "font", 0},  /* port/gs/font_sharp.c */
    {"Frame Rate:", "fps", 0},
    {"V-Sync:", "vsync", 0},  /* no tearing (port/gs/gpu_rust.c) */
    {"FPS & Ping:", "perf_counter", 1},
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

/* The largest scale the renderer draws on this GPU: DHD_SCALE_MAX, which the Rust native renderer
   sets (8, or less on a GPU with little memory or small textures); 0 without it (the exact
   renderer). scale_why: what holds it below 8 (DHD_SCALE_MAX_REASON) in the row's words ("GPU ..."; the
   footer says "This GPU's" and the rest), else NULL. */
static int scale_limit;
static const char *scale_why;
static int scale_phone;  /* the limit is the phones' (DHD_SCALE_MAX_REASON "phone") */

/* Each row's list: the setting's choices, but Resolution with every scale when the native renderer
   says what it draws (scale_limit: the ones over it are marked, not hidden: the limit follows the
   memory the system reports for this GPU now) and up to 4 without it, and Frame Rate up to
   port_fps_max (240; the browser sets DHD_FPS_MAX 144), each then Window or Monitor; Window Size
   with the monitor's own size (DHD_MONITOR_MODE, present.c) in place of "monitor" when it isn't one
   of them. */
static void choices_build(void)
{
    const char *most = host_getenv("DHD_SCALE_MAX"), *mode = host_getenv("DHD_MONITOR_MODE");
    const char *why = host_getenv("DHD_SCALE_MAX_REASON");
    scale_limit = most && atoi(most) > 0 ? atoi(most) : 0;
    scale_why = why && !strcmp(why, "memory") ? "GPU memory" : why && !strcmp(why, "texture") ? "GPU texture size" : NULL;
    /* a phone's limit (NATIVE_PHONE_SCALE_MAX) is the same on every phone: the scales over it aren't offered */
    scale_phone = why && !strcmp(why, "phone");
    int max_scale = scale_phone ? scale_limit : scale_limit ? 99 : 4, w = 0, h = 0;
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
static int screen;         /* 0 the PC settings, 1-3 TOUCH CONTROLS */
static int touch_row = -1; /* the Touch Controls row, -1 without touch */

static const char *page_title(void)
{
    const char *touch = host_getenv("DHD_TOUCH");
    if (screen) return touch_settings_title(screen);
    return touch && !strcmp(touch, "android") ? "SETTINGS" : "PC SETTINGS";
}

/* Resolution's value as the GPU draws it (scale_limit, host_service_gs_scale), after its label:
   a scale over the limit "8x (running 4x: GPU memory)" while it runs, "(runs at 4x: ...)"
   while it waits for the next start; the window's match with the scale it runs at, "(3x)", or
   "(4x: GPU memory)" when the limit holds it. */
static void scale_note(const Row *r, char *value, size_t size)
{
    if (r->selected < 0 || !scale_limit) return;
    const char *choice = r->choices[r->selected].value;
    int asked = atoi(choice), used = host_service_gs_scale(0), running = r->selected == r->running;
    char why[24] = "";
    if (scale_why) snprintf(why, sizeof why, ": %s", scale_why);
    size_t at = strlen(value);
    if (asked > scale_limit)
        snprintf(value + at, size - at, running ? " (running %dx%s)" : " (runs at %dx%s)",
                 running && used ? used : scale_limit, why);
    else if (!asked && running && used)
        snprintf(value + at, size - at, " (%dx%s)", used, host_service_gs_scale(1) > used ? why : "");
}

/* A row on the page: "Label:  < Value >", "*" after it while it waits for the next start; no
   arrows when the environment sets it. */
static void format(Row *r)
{
    char value[48];
    int index = -1;
    for (int i = 0; i < shown_count; i++)
        if (&rows[shown[i]] == r) index = i;
    if (index < 0) return;
    if (r->selected >= 0) snprintf(value, sizeof value, "%s", r->choices[r->selected].label);
    else current(r, value, sizeof value);
    if (r == &rows[RESOLUTION]) scale_note(r, value, sizeof value);
    if (r->selected != r->running) strncat(value, " *", sizeof value - strlen(value) - 1);
    menu_page_value_set(&page, index, r->label, value, !locked(r));
}

/* Saves the row's choice and, for a live row, applies it. */
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
    else if (r == &rows[SHAPE] || r == &rows[SCALING] || r == &rows[COUNTER]) host_service_gs_present_options_apply();
    else if (r == &rows[VIBRATION]) host_service_host_pad_vibrate(0, 0, 0);
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

static void page_build(MenuPage *p)
{
    p->title = page_title();
    if (screen) { touch_settings_build(p, screen); return; }
    for (int i = 0; i < shown_count; i++) {
        menu_page_value(p, rows[shown[i]].label, "", 1);
        format(&rows[shown[i]]);
    }
    const char *touch = host_getenv("DHD_TOUCH");
    touch_row = touch && *touch ? menu_page_row(p, "Touch Controls", 1) : -1;
}

static void page_update(MenuPage *p, int cursor)  /* the footer for the row */
{
    if (screen) { touch_settings_update(p, screen, cursor); return; }
    const Row *r = row_at(cursor);
    p->footer = NULL;
    if (r && locked(r)) {
        snprintf(footer, sizeof footer, "Set by %s outside the game", r == &rows[SIZE] ? "DHD_WINDOW_WIDTH" : r->env);
        p->footer = footer;
    } else if (r && r->selected != r->running) p->footer = restart_footer;
    else if (r == &rows[TEXTURES] && !host_service_texpack_available()) p->footer = "No pack: see texture_pack in dhd.ini";
    else if (r == &rows[RESOLUTION] && scale_limit && scale_limit < 8) {
        if (scale_phone) snprintf(footer, sizeof footer, "Phones go up to %dx", scale_limit);
        else if (scale_why) snprintf(footer, sizeof footer, "This GPU's %s allows up to %dx", scale_why + 4, scale_limit);
        else snprintf(footer, sizeof footer, "This GPU allows up to %dx", scale_limit);
        p->footer = footer;
    }
}

static void page_back(MenuPage *p)  /* Triangle: the values the page opened with, close */
{
    if (screen) {  /* TOUCH CONTROLS saves as it goes: back to the PC settings, on its row */
        screen = 0;
        menu_play("S_BACK");
        menu_page_rebuild_at(p, touch_row >= 0 ? touch_row : 0);
        return;
    }
    for (int i = 0; i < ROWS; i++) {
        Row *r = &rows[i];
        if (r->selected == r->opened || r->opened < 0) continue;
        r->selected = r->opened;
        commit(r, r->live || r->opened == r->running);
    }
    menu_page_cancel(p);
}

static void page_left(MenuPage *p, int index) { if (screen) touch_settings_change(p, screen, index, -1); else change(index, -1); }
static void page_right(MenuPage *p, int index) { if (screen) touch_settings_change(p, screen, index, 1); else change(index, 1); }

/* Cross: keep and close; on Touch Controls (and inside it) the touch screens. */
static void page_accept(MenuPage *p, int index)
{
    if (!screen && (touch_row < 0 || index != touch_row)) { menu_page_close(p); return; }
    int next = screen ? touch_settings_accept(p, screen, index) : 1;
    if (!screen) menu_play("S_SELECT");
    if (next == screen) return;
    int back_to_row = next == 0 && touch_row >= 0 ? touch_row : 0;
    screen = next;
    menu_page_rebuild_at(p, back_to_row);
}

/* OPTIONS, item 9 (patched func_001F0160): run the page until it closes. */
void port_pc_settings(void)
{
    screen = 0;
    page.title = page_title();
    page.build = page_build;
    page.update = page_update;
    page.accept = page_accept;  /* Cross: keep, close (Touch Controls: its screens) */
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
    menu_page_run(&page);
}

/* OPTIONS' rows (patched func_001F0160): row 9 is this page; the game's table has an unused
   "UNLOCK RIDERS/BIKES(test)" there, which stays as it is. */
char *port_options_label(char *label, int index)
{
    return index == 9 ? (char *)"PC SETTINGS" : label;
}
