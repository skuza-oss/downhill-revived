/* TOUCH CONTROLS (finding 141): three screens of the settings page (pc_settings.c's page, which
   PC SETTINGS opens with its Touch Controls row when the host has touch: DHD_TOUCH), in the game's
   own popup style (menu_page.c). Each change is saved to dhd.ini at once (touch_* keys) and the
   host's touch controls follow it (TOUCH_COMMAND_SETTINGS); Edit Layout opens the overlay's editor
   over the page, Show Hints Again and Set Tilt Centre are commands too (touch_field.c).
   Page 1: Layout, Hand, Steer With, Size, Opacity; Edit Layout, More, Back.
   Page 2: Stick, Stick Range, Trick Stick, Trick Labels, Attacks (the last three dimmed under
           Classic pad); Prev, More, Back.
   Page 3: Vibration, With A Controller, Show Hints Again, Tilt Strength and Set Tilt Centre (alive
           with tilt steering); Prev, Back.
   Left / right (and the mouse's or a tap's arrows) change a value; Cross steps it forward.
   Without a tilt sensor (the host's DHD_TOUCH_TILT_SENSOR=0) Steer With stays on Stick and its
   footer says so. */
#include "hle.h"
#include "../runtime/portable.h"
#include "menu_page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_table.h"

enum { VALUES = 5, PAGES = 3 };
enum { DIM_NEVER, DIM_CLASSIC, DIM_NO_TILT };
enum { ACT_NONE, ACT_EDIT, ACT_MORE, ACT_PREV, ACT_BACK, ACT_HINTS, ACT_TILT_CENTRE };

typedef struct { const char *label, *key; int dim, action; } Line;   /* key NULL: a button (action) */

static const Line lines[PAGES][VALUES] = {
    {{"Layout:", "touch_preset", DIM_NEVER, 0}, {"Hand:", "touch_hand", DIM_NEVER, 0}, {"Steer With:", "touch_steer", DIM_NEVER, 0},
     {"Size:", "touch_size", DIM_NEVER, 0}, {"Opacity:", "touch_opacity", DIM_NEVER, 0}},
    {{"Stick:", "touch_stick", DIM_NEVER, 0}, {"Stick Range:", "touch_stick_range", DIM_NEVER, 0},
     {"Trick Stick:", "touch_trick_stick", DIM_CLASSIC, 0}, {"Trick Labels:", "touch_labels", DIM_CLASSIC, 0},
     {"Attacks:", "touch_attacks", DIM_CLASSIC, 0}},
    {{"Vibration:", "touch_vibration", DIM_NEVER, 0}, {"With A Controller:", "touch_with_pad", DIM_NEVER, 0},
     {"Show Hints Again", NULL, DIM_NEVER, ACT_HINTS}, {"Tilt Strength:", "touch_tilt_strength", DIM_NO_TILT, 0},
     {"Set Tilt Centre", NULL, DIM_NO_TILT, ACT_TILT_CENTRE}},
};

static const int buttons[PAGES][3] = {{ACT_EDIT, ACT_MORE, ACT_BACK}, {ACT_PREV, ACT_MORE, ACT_BACK}, {ACT_PREV, ACT_BACK, ACT_NONE}};
static const char *const button_text[] = {"", "Edit Layout", "More " GLYPH_RIGHT, GLYPH_LEFT " Prev", "Back"};

static int row_action[MENU_PAGE_ROWS], row_line[MENU_PAGE_ROWS];
static char title[32];
static const char *note;     /* the footer after a command */

static const SettingInfo *info_of(const char *key)
{
    for (unsigned i = 0; i < sizeof setting_table / sizeof *setting_table; i++)
        if (!strcmp(setting_table[i].key, key)) return &setting_table[i];
    return NULL;
}

/* The value it runs with: the variable (dhd.ini's, or the environment's), else the built-in one. */
static int current(const SettingInfo *info)
{
    const char *v = host_getenv(info->env);
    if (!v || !*v) v = info->builtin;
    for (int i = 0; i < info->count; i++)
        if (!dhd_stricmp(info->choices[i].value, v)) return i;
    if (info->count && !strcmp(info->key, "touch_vibration")) return atoi(v) ? 1 : 0;
    return 0;
}

static int is(const char *key, const char *value)
{
    const SettingInfo *info = info_of(key);
    return info && !dhd_stricmp(info->choices[current(info)].value, value);
}

/* The host found no tilt sensor (DHD_TOUCH_TILT_SENSOR=0): Steer With refuses Tilt. */
static int no_tilt_sensor(void)
{
    const char *v = host_getenv("DHD_TOUCH_TILT_SENSOR");
    return v && !strcmp(v, "0");
}

static int dimmed(const Line *l)
{
    return (l->dim == DIM_CLASSIC && is("touch_preset", "classic")) || (l->dim == DIM_NO_TILT && !is("touch_steer", "tilt"));
}

static void show(MenuPage *p, int row)
{
    const Line *l = &lines[0][0] + row_line[row];
    const SettingInfo *info = info_of(l->key);
    if (!info) return;
    menu_page_value_set(p, row, l->label, info->choices[current(info)].label,
                        !dimmed(l) && !host_service_host_setting_locked(GADDR(info->env)));
}

const char *touch_settings_title(int page)
{
    snprintf(title, sizeof title, "TOUCH CONTROLS %d/%d", page, PAGES);
    return title;
}

void touch_settings_build(MenuPage *p, int page)
{
    int index = page - 1;
    for (int i = 0; i < MENU_PAGE_ROWS; i++) row_action[i] = ACT_NONE, row_line[i] = -1;
    for (int i = 0; i < VALUES; i++) {
        const Line *l = &lines[index][i];
        int row = l->key ? menu_page_value(p, l->label, "", 1) : menu_page_row(p, l->label, !dimmed(l));
        if (row < 0) continue;
        row_line[row] = index * VALUES + i;
        row_action[row] = l->action;
        if (l->key) {
            show(p, row);
            if (dimmed(l)) menu_page_enable(p, row, 0);
        }
    }
    for (int i = 0; i < 3; i++) {
        if (buttons[index][i] == ACT_NONE) continue;
        int row = menu_page_row(p, button_text[buttons[index][i]], 1);
        if (row >= 0) row_action[row] = buttons[index][i];
    }
    note = NULL;
}

void touch_settings_update(MenuPage *p, int page, int cursor)
{
    (void)page;
    p->footer = note;
    if (!note && cursor >= 0 && cursor < MENU_PAGE_ROWS && row_line[cursor] >= 0) {
        const Line *l = &lines[0][0] + row_line[cursor];
        if (l->dim == DIM_CLASSIC && dimmed(l)) p->footer = "Thumb zones only";
        else if (l->dim == DIM_NO_TILT && dimmed(l)) p->footer = no_tilt_sensor() ? "No tilt sensor on this device" : "With Steer With: Tilt";
        else if (l->key && !strcmp(l->key, "touch_steer") && no_tilt_sensor()) p->footer = "No tilt sensor on this device";
    }
}

/* Left / right on a value row. */
void touch_settings_change(MenuPage *p, int page, int row, int step)
{
    (void)page;
    if (row < 0 || row >= MENU_PAGE_ROWS || row_line[row] < 0) return;
    const Line *l = &lines[0][0] + row_line[row];
    const SettingInfo *info = l->key ? info_of(l->key) : NULL;
    if (!info || dimmed(l) || host_service_host_setting_locked(GADDR(info->env))) return;
    int i = (current(info) + step + info->count) % info->count;
    const char *value = info->choices[i].value;
    if (!strcmp(info->key, "touch_steer") && !strcmp(value, "tilt") && no_tilt_sensor()) return;   /* the row's footer says why */
    host_service_host_setting_save(GADDR(info->key), GADDR(value));
    host_putenv_s(info->env, value);
    hle_env_changed();
    touch_command(TOUCH_COMMAND_SETTINGS);
    menu_page_sound(step > 0);
    hle_log("touch settings: %s %s\n", info->key, value);
    if (!strcmp(info->key, "touch_preset") || !strcmp(info->key, "touch_steer")) menu_page_rebuild_at(p, row);  /* dimmed rows change */
    else show(p, row);
}

/* Cross: a button's action, or the next value. Returns the page to show next (0: PC SETTINGS). */
int touch_settings_accept(MenuPage *p, int page, int row)
{
    if (row < 0 || row >= MENU_PAGE_ROWS) return page;
    if (row_line[row] >= 0 && (&lines[0][0] + row_line[row])->key) {
        touch_settings_change(p, page, row, 1);
        return page;
    }
    switch (row_action[row]) {
    case ACT_EDIT: touch_command(TOUCH_COMMAND_EDIT); menu_play("S_SELECT"); return page;
    case ACT_MORE: menu_play("S_SELECT"); return page + 1;
    case ACT_PREV: menu_play("S_BACK"); return page - 1;
    case ACT_BACK: menu_play("S_BACK"); return 0;
    case ACT_HINTS: touch_command(TOUCH_COMMAND_HINTS); note = "The hints will show again"; menu_play("S_SELECT"); return page;
    case ACT_TILT_CENTRE:
        if (!is("touch_steer", "tilt")) return page;
        touch_command(TOUCH_COMMAND_TILT_CENTRE);
        note = "Tilt centre set: the phone as it is now";
        menu_play("S_SELECT");
        return page;
    default: return page;
    }
}
