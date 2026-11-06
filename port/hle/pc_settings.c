/* PC SETTINGS: the page the front end's OPTIONS screen opens (item 9, port/patches.py patches
   func_001F0160), a page in the game's own popup menu style (menu_page.c, finding 050) as its
   AUDIO / OTHER page is; up/down, the cursor, the font, the panel and the sounds are the game's:
   - left/right change the row under the cursor; Cross (SELECT) keeps the changes and closes,
     Triangle (BACK) puts the values the page opened with back and closes, as AUDIO / OTHER does
     (on Android Back keeps them, as a phone's Back does); a click or tap on the legend's ✕ SELECT
     or △ BACK presses it (menu_page.c).
   Every change is saved to dhd.ini at once (host_setting_save keeps the other lines). Display,
   window size, picture shape, scaling, skip movies (the startup movies), vibration, the sticks'
   dead zone, the button symbols, unlock all and FPS & Ping apply at once; resolution,
   widescreen, the texture pack, the font, the frame rate and V-Sync at the next start ("*" and
   the row's hint, a line above the legend, say so). A setting the environment sets (DHD_*) is
   shown but can't be changed here.
   Nothing here writes game state: the page lives in host memory and the settings in dhd.ini,
   never in the memory card's save.
   A host that can do only some of the rows says which in DHD_PC_SETTINGS_ROWS, a comma list of
   the rows' variables; unset, every row is shown. A host that applies more rows at once than the
   ones marked live below lists their variables in DHD_PC_SETTINGS_LIVE (the row then sets its
   variable at once and the host follows it), and DHD_PC_SETTINGS_RESTART replaces the hint of
   a row that waits for the next start. The browser sets all three (port/web/www/settings.js);
   x64 sets none of them, so its page is as described above.
   After the rows, Controls opens the CONTROLS screen (controls_settings.c, finding 157: the keys
   and controller buttons for the PS2 pad's buttons; a host lists DHD_KEYBOARD in its
   DHD_PC_SETTINGS_ROWS to show it). With touch (the host sets DHD_TOUCH: android, or mouse for a
   desktop test) the page ends with a Touch Controls row whose three screens are
   touch_settings.c's (finding 141); on Android the page is called SETTINGS. */
#include "hle.h"
#include "../runtime/portable.h"
#include "menu_page.h"
#include "../include/fps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controls_table.h"
#include "settings_tabs.h"
#include "settings_table.h"  /* the settings' choices: generated from port/render/settings */

void gs_window_apply(void);           /* port/gs/present.c */
/* touch_settings.c: the TOUCH CONTROLS screens 1-3 of this page */
const char *touch_settings_title(int page);
void touch_settings_build(MenuPage *p, int page);
void touch_settings_update(MenuPage *p, int page, int cursor);
void touch_settings_change(MenuPage *p, int page, int row, int step);
int touch_settings_accept(MenuPage *p, int page, int row);
/* controls_settings.c: the CONTROLS screen of this page */
const char *controls_settings_title(void);
void controls_settings_open(void);
void controls_settings_build(MenuPage *p);
void controls_settings_draw(MenuPage *p);
void controls_settings_update(MenuPage *p, int cursor);
void controls_settings_change(MenuPage *p, int row, int step);
int controls_settings_accept(MenuPage *p, int row);
void gs_present_options_apply(void);
int texpack_available(void);          /* port/gs/texpack.c */

enum { ROWS = 17, MAX_CHOICES = 16 };

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
       VSYNC, COUNTER, UNLOCK, NAMES };
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
    {"Unlock All:", "unlock_all", 1},
    {"Player Names:", "names", 1},
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
   hint says "This GPU's" and the rest), else NULL. */
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
static char hint[64];
static const char *restart_hint = "* Applies when the game starts again";
enum { TAB_DISPLAY, TAB_GRAPHICS, TAB_CONTROLS, TAB_ONLINE, TAB_GAME, TABS };
static const char *const tab_labels[TABS] = {"Display", "Graphics", "Controls", "Online", "Game"};
static const int row_tab[ROWS] = {TAB_GRAPHICS, TAB_DISPLAY, TAB_DISPLAY, TAB_DISPLAY, TAB_GRAPHICS,
    TAB_DISPLAY, TAB_GAME, TAB_CONTROLS, TAB_CONTROLS, TAB_CONTROLS, TAB_GRAPHICS, TAB_GRAPHICS,
    TAB_DISPLAY, TAB_DISPLAY, TAB_DISPLAY, TAB_GAME, TAB_ONLINE};
static int tab, visible[ROWS], tabs[TABS], tab_count;
static unsigned held_before;
static int tab_left[TABS], tab_right[TABS];
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
    if (restart && *restart) restart_hint = restart;
    for (int i = 0; i < ROWS; i++) {
        visible[i] = !list || listed(list, rows[i].env);
        if (live && listed(live, rows[i].env)) rows[i].live = 1;
    }
    const char *touch = host_getenv("DHD_TOUCH");
    int controls = !list || listed(list, "DHD_KEYBOARD") || (touch && *touch);
    tab_count = settings_tabs_choose(tabs, TABS, row_tab, visible, ROWS, TAB_CONTROLS, controls);
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
enum { CONTROLS = 4 };      /* the screen of controls_settings.c */
static int screen;          /* 0 the PC settings, 1-3 TOUCH CONTROLS, CONTROLS */
static int touch_row = -1;  /* the Touch Controls row, -1 without touch */
static int controls_row = -1;  /* the Controls row, -1 when the host doesn't offer it */

static const char *page_title(void)
{
    const char *touch = host_getenv("DHD_TOUCH");
    if (screen == CONTROLS) return controls_settings_title();
    if (screen) return touch_settings_title(screen);
    return touch && !strcmp(touch, "android") ? "SETTINGS" : "REVIVED SETTINGS";
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
    p->title_y = screen ? 0 : 35;
    p->footer = NULL;
    if (screen == CONTROLS) { controls_settings_build(p); return; }
    if (screen) { touch_settings_build(p, screen); return; }
    shown_count = 0;
    for (int i = 0; i < ROWS; i++)
        if (visible[i] && row_tab[i] == tabs[tab]) shown[shown_count++] = i;
    const char *touch_mode = host_getenv("DHD_TOUCH");
    p->footer = touch_mode && !strcmp(touch_mode, "android")
        ? GLYPH_LEFT " Change " GLYPH_RIGHT "  " GLYPH_CROSS " Keep  " GLYPH_TRIANGLE " Back"
        : GLYPH_LEFT " Change " GLYPH_RIGHT "  " GLYPH_CROSS " Keep  " GLYPH_TRIANGLE " Undo";
    for (int i = 0; i < shown_count; i++) {
        menu_page_value(p, rows[shown[i]].label, "", 1);
        format(&rows[shown[i]]);
    }
    const char *list = host_getenv("DHD_PC_SETTINGS_ROWS");
    controls_row = tabs[tab] == TAB_CONTROLS && (!list || listed(list, "DHD_KEYBOARD")) ? menu_page_row(p, "Controls", 1) : -1;
    const char *touch = host_getenv("DHD_TOUCH");
    touch_row = tabs[tab] == TAB_CONTROLS && touch && *touch ? menu_page_row(p, "Touch Controls", 1) : -1;
}

static void page_update(MenuPage *p, int cursor)  /* the hint for the row (the legend stays) */
{
    if (screen == CONTROLS) { controls_settings_update(p, cursor); return; }
    if (screen) { touch_settings_update(p, screen, cursor); return; }
    unsigned held = 0;
    for (int player = 0; player < 2; player++) {
        HostPad pad = {0};
        host_service_host_pad_device(player, GADDR(&pad));
        held |= pad.buttons;
    }
    unsigned pressed = held & ~held_before;
    held_before = held;
    if (p->armed && tab_count > 1 && (pressed & (PAD_L1 | PAD_R1))) {
        int step = pressed & PAD_R1 ? 1 : -1;
        tab = settings_tab_step(tab, step, tab_count);
        menu_page_sound(step > 0);
        menu_page_rebuild_at(p, 0);
        return;
    }
    const Row *r = row_at(cursor);
    p->hint = NULL;
    if (cursor >= 0 && cursor == controls_row) p->hint = "Your keys and controller buttons";
    if (r && locked(r)) {
        snprintf(hint, sizeof hint, "Set by %s outside the game", r == &rows[SIZE] ? "DHD_WINDOW_WIDTH" : r->env);
        p->hint = hint;
    } else if (r && r->selected != r->running) p->hint = restart_hint;
    else if (r == &rows[TEXTURES] && !host_service_texpack_available()) p->hint = "No pack: see texture_pack in dhd.ini";
    else if (r == &rows[RESOLUTION] && scale_limit && scale_limit < 8) {
        if (scale_phone) snprintf(hint, sizeof hint, "Phones go up to %dx", scale_limit);
        else if (scale_why) snprintf(hint, sizeof hint, "This GPU's %s allows up to %dx", scale_why + 4, scale_limit);
        else snprintf(hint, sizeof hint, "This GPU allows up to %dx", scale_limit);
        p->hint = hint;
    }
}

static void page_back(MenuPage *p)  /* Triangle: the values the page opened with, close */
{
    if (screen) {  /* TOUCH CONTROLS and CONTROLS save as they go: back to the PC settings, on their row */
        int row = screen == CONTROLS ? controls_row : touch_row;
        screen = 0;
        menu_play("S_BACK");
        menu_page_rebuild_at(p, row >= 0 ? row : 0);
        return;
    }
    /* A phone's Back (and △) leaves SETTINGS as its other screens do: with what was chosen, which
       is already saved (every change is). Putting the opened values back is the PC's △, where
       ✕ SELECT is the way to keep them. */
    const char *touch = host_getenv("DHD_TOUCH");
    if (touch && !strcmp(touch, "android")) {
        menu_page_close(p);
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

static void page_step(MenuPage *p, int index, int step)
{
    if (screen == CONTROLS) controls_settings_change(p, index, step);
    else if (screen) touch_settings_change(p, screen, index, step);
    else change(index, step);
}
static void page_left(MenuPage *p, int index) { page_step(p, index, -1); }
static void page_right(MenuPage *p, int index) { page_step(p, index, 1); }

/* Cross: keep and close; on Touch Controls (and inside it) the touch screens, on Controls its screen. */
static void page_accept(MenuPage *p, int index)
{
    if (screen == CONTROLS) { controls_settings_accept(p, index); return; }
    if (!screen && controls_row >= 0 && index == controls_row) {
        menu_play("S_SELECT");
        controls_settings_open();
        screen = CONTROLS;
        menu_page_rebuild_at(p, 0);
        return;
    }
    if (!screen && (touch_row < 0 || index != touch_row)) { menu_page_close(p); return; }
    int next = screen ? touch_settings_accept(p, screen, index) : 1;
    if (!screen) menu_play("S_SELECT");
    if (next == screen) return;
    int back_to_row = next == 0 && touch_row >= 0 ? touch_row : 0;
    screen = next;
    menu_page_rebuild_at(p, back_to_row);
}

static void page_draw(MenuPage *p)
{
    if (screen == CONTROLS) { controls_settings_draw(p); return; }
    if (screen) return;
    MenuStyle style = menu_page_style(p, MENU_STYLE_OFF);
    char legend[96];
    int width = 0;
    for (int i = 0; i < tab_count; i++) width += menu_text_width(tab_labels[tabs[i]], &style) + 16;
    int x = 256 - width / 2;
    for (int i = 0; i < tab_count; i++) {
        int w = menu_text_width(tab_labels[tabs[i]], &style) + 16;
        tab_left[i] = x, tab_right[i] = x + w;
        char label[24];
        snprintf(label, sizeof label, i == tab ? "[%s]" : "%s", tab_labels[tabs[i]]);
        MenuStyle shown = menu_page_style(p, i == tab ? MENU_STYLE_SELECTED : MENU_STYLE_OFF);
        menu_draw_text(MENU_CENTRE, x + w / 2, 62, label, &shown);
        x += w;
    }
    ControlsLayout layout;
    int pad = host_service_controls_pad(CONTROL_PAD1);
    int device = pad > 0 ? CONTROL_PAD1 : CONTROL_KEYBOARD;
    if (!host_service_controls_layout(device, GADDR(&layout))) layout = control_defaults[device];
    const char *words[2] = {"-", "-"};
    for (int i = 0; i < 2; i++) {
        int code = layout.input[4 + i][0];
        if (device == CONTROL_KEYBOARD) {
            for (int k = 0; k < CONTROL_KEYS; k++) if (control_keys[k].vk == code) words[i] = control_keys[k].label;
        } else {
            for (int k = 0; k < CONTROL_PAD_INPUTS; k++) if (control_pad_inputs[k].id == code)
                words[i] = control_pad_inputs[k].label[pad - 1 == PAD_KIND_PLAYSTATION ? 1 : pad - 1 == PAD_KIND_NINTENDO ? 2 : 0];
        }
    }
    snprintf(legend, sizeof legend, "%s / %s  Tabs", words[0], words[1]);
    menu_draw_text(MENU_CENTRE, 256, 180, legend, &style);
}

/* A tap or click on a tab keeps the normal row and legend mouse handling. */
static int page_header_mouse(MenuPage *p, int x, int y)
{
    if (screen || y < 55 || y > 73) return 0;
    for (int i = 0; i < tab_count; i++) {
        if (x < tab_left[i] || x >= tab_right[i]) continue;
        if (i != tab) {
            menu_page_sound(i > tab);
            tab = i;
            menu_page_rebuild_at(p, 0);
        }
        return 1;
    }
    return 0;
}

/* OPTIONS, first item (patched func_001F0160): run the page until it closes. */
void port_pc_settings(void)
{
    screen = 0;
    held_before = 0xFFFF;
    page.title = page_title();
    page.build = page_build;
    page.update = page_update;
    page.draw = page_draw;
    page.header_mouse = page_header_mouse;
    page.hints = 1;   /* the row's hint above the legend (menu_page.h) */
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

/* OPTIONS' rows (patched func_001F0160): row 0 is this page; the game's table has an unused
   "UNLOCK RIDERS/BIKES(test)" there, which stays as it is. */
char *port_options_label(char *label, int index)
{
    if (index != 0) return label;
    const char *touch = host_getenv("DHD_TOUCH");  /* the page's own name (page_title): SETTINGS on a phone */
    return (char *)(touch && !strcmp(touch, "android") ? "SETTINGS" : "REVIVED SETTINGS");
}
