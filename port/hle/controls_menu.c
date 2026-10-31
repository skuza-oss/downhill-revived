/* CONTROLS: the page that rebinds the keyboard and the gamepad and keeps control profiles
   (finding 124), in the game's own popup style (menu_page.c). OPTIONS > PC SETTINGS opens it (its
   "Controls..." row); it pushes the hub over PC SETTINGS and the Keyboard / Gamepad pages over the
   hub, as the game's own sub-pages open.

   The bindings in force come from the host (host.c): DHD_CONTROLS_ACTIVE holds the active profile's
   text, DHD_CONTROLS names the profile and DHD_CONTROLS_PROFILES lists the ones dhd.ini holds. A
   change saves the profile's key (and "controls") through the settings crate and asks the host to
   apply it (host_service_controls_apply), which reads the file again and republishes
   DHD_CONTROLS_ACTIVE. So the keyboard and the controllers follow at once, without a restart.

   Touch Controls / Touch Layout are shown only where the on-screen controls are (Android,
   DHD_TOUCH_AVAILABLE). The layout is not drawn during play yet (finding 124, Open). */
#include "hle.h"
#include "menu_page.h"
#include "controls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static MenuPage hub, key_page, pad_page;
static int key_which, pad_which;      /* the control the keyboard / gamepad page shows */
static char profiles[8][32];
static int profile_count, profile;    /* the active profile */

enum { HUB_KEYBOARD, HUB_GAMEPAD, HUB_PROFILE, HUB_SAVE, HUB_RESET, HUB_TOUCH, HUB_TOUCH_LAYOUT };

static int touch_available(void)
{
    const char *v = host_getenv("DHD_TOUCH_AVAILABLE");
    return v && *v && atoi(v) != 0;
}

static int profile_index(const char *name)
{
    for (int i = 0; i < profile_count; i++)
        if (!strcmp(profiles[i], name)) return i;
    return 0;
}

/* The bindings and the profile list from the host, and the profile in force. */
static void state_load(void)
{
    const char *text = host_getenv("DHD_CONTROLS_ACTIVE"), *list = host_getenv("DHD_CONTROLS_PROFILES");
    const char *active = host_getenv("DHD_CONTROLS");
    controls_default();
    if (text && *text) controls_parse(text);
    profile_count = 0;
    for (const char *at = list; at && *at && profile_count < 8;) {
        const char *end = strchr(at, ',');
        size_t n = end ? (size_t)(end - at) : strlen(at);
        if (n && n < sizeof profiles[0]) snprintf(profiles[profile_count++], sizeof profiles[0], "%.*s", (int)n, at);
        at = end ? end + 1 : at + n;
    }
    if (!profile_count) snprintf(profiles[profile_count++], sizeof profiles[0], "default");
    profile = active && *active ? profile_index(active) : 0;
    if (key_which >= control_key_count()) key_which = 0;
    if (pad_which >= control_pad_count()) pad_which = 0;
}

/* Save what the page holds: the active profile's bindings, the profile's name, and (where they
   exist) the touch settings; then have the host apply it to the keyboard and the controllers. */
static void apply(void)
{
    char key[64], text[CONTROL_TEXT];
    control_profile_key(profiles[profile], key, sizeof key);
    if (controls_format(text, sizeof text) < 0) return;
    host_service_host_setting_save(GADDR(key), GADDR(text));
    host_service_host_setting_save(GADDR("controls"), GADDR(profiles[profile]));
    host_putenv_s("DHD_CONTROLS", profiles[profile]);
    if (touch_available()) {
        const char *layout = host_getenv("DHD_TOUCH_LAYOUT");
        host_service_host_setting_save(GADDR("touch"), GADDR(host_getenv("DHD_TOUCH") ? "1" : "0"));
        host_service_host_setting_save(GADDR("touch_layout"), GADDR(layout && *layout ? layout : "default"));
    }
    hle_env_changed();
    host_service_controls_apply();
}

/* ---- the hub ---- */

static void hub_build(MenuPage *p)
{
    menu_page_row(p, "Keyboard controls...", 1);
    menu_page_row(p, "Gamepad controls...", 1);
    menu_page_value(p, "Profile:", profiles[profile], 1);
    menu_page_row(p, "Save Profile", 1);
    menu_page_row(p, "Reset to Defaults", 1);
    if (touch_available()) {
        const char *on = host_getenv("DHD_TOUCH"), *layout = host_getenv("DHD_TOUCH_LAYOUT");
        menu_page_value(p, "Touch Controls:", on && !atoi(on) ? "OFF" : "ON", 1);
        menu_page_value(p, "Touch Layout:", layout && !strcmp(layout, "left-handed") ? "Left-handed" : "Default", 1);
    }
}

static void hub_change(MenuPage *p, int row, int step)
{
    if (row == HUB_PROFILE) {
        profile = (profile + step + profile_count) % profile_count;
        host_putenv_s("DHD_CONTROLS", profiles[profile]);
        hle_env_changed();
        host_service_host_setting_save(GADDR("controls"), GADDR(profiles[profile]));
        host_service_controls_apply();   /* the host reads the new profile and republishes it */
        state_load();
        menu_page_rebuild_at(p, row);
        menu_page_sound(step > 0);
    } else if (row == HUB_TOUCH || row == HUB_TOUCH_LAYOUT) {
        char value[24];
        const char *on = host_getenv("DHD_TOUCH"), *layout = host_getenv("DHD_TOUCH_LAYOUT");
        int touch_on = on && !atoi(on);
        int left = layout && !strcmp(layout, "left-handed");
        if (row == HUB_TOUCH) touch_on = !touch_on; else left = !left;
        snprintf(value, sizeof value, "%s", touch_on ? "1" : "0");
        host_putenv_s("DHD_TOUCH", value);
        host_putenv_s("DHD_TOUCH_LAYOUT", left ? "left-handed" : "default");
        hle_env_changed();
        apply();                          /* saves touch and touch_layout too */
        menu_page_rebuild_at(p, row);
        menu_page_sound(step > 0);
    }
}

static void hub_accept(MenuPage *p, int row)
{
    if (row == HUB_KEYBOARD) menu_page_push(&key_page);
    else if (row == HUB_GAMEPAD) menu_page_push(&pad_page);
    else if (row == HUB_SAVE) { apply(); menu_page_sound(1); }
    else if (row == HUB_RESET) { controls_default(); apply(); menu_page_rebuild_at(p, row); }
}

static void hub_left(MenuPage *p, int row) { hub_change(p, row, -1); }
static void hub_right(MenuPage *p, int row) { hub_change(p, row, 1); }

/* ---- the keyboard page ---- */

static const char *key_shown(void)
{
    const char *name = control_key_name_of_vk(control_key_vk(key_which));
    return *name ? name : "(none)";
}

static void key_build(MenuPage *p)
{
    if (key_which >= control_key_count()) key_which = 0;
    menu_page_value(p, "Control:", control_key_label(key_which), 1);
    menu_page_value(p, "Key:", key_shown(), 1);
}

static void key_change(MenuPage *p, int row, int step)
{
    if (row == HUB_KEYBOARD) {  /* row 0: the control */
        int n = control_key_count();
        key_which = (key_which + step + n) % n;
    } else {
        int n = control_key_catalog_count();
        int cur = control_key_catalog_index(control_key_vk(key_which));
        int index = cur < 0 ? (step > 0 ? 0 : n) : cur;
        index = (index + step + n + 1) % (n + 1);
        control_key_set(key_which, index < n ? control_key_catalog_vk(index) : 0);
        apply();
    }
    menu_page_rebuild_at(p, row);
    menu_page_sound(step > 0);
}

static void key_left(MenuPage *p, int row) { key_change(p, row, -1); }
static void key_right(MenuPage *p, int row) { key_change(p, row, 1); }

/* ---- the gamepad page ---- */

/* The catalog entries this control accepts (control_pad_accepts), for cycling. */
static int pad_candidates(unsigned *out, int max)
{
    int n = 0;
    for (int j = 0; j < control_pad_catalog_count() && n < max; j++) {
        unsigned source = control_pad_catalog_source(j);
        if (control_pad_accepts(pad_which, source)) out[n++] = source;
    }
    return n;
}

static const char *pad_shown(void)
{
    const char *name = control_pad_source_name(control_pad_source(pad_which));
    return *name ? name : "(none)";
}

static void pad_build(MenuPage *p)
{
    if (pad_which >= control_pad_count()) pad_which = 0;
    menu_page_value(p, "Control:", control_pad_label(pad_which), 1);
    menu_page_value(p, "Source:", pad_shown(), 1);
}

static void pad_change(MenuPage *p, int row, int step)
{
    if (row == HUB_KEYBOARD) {  /* row 0: the control */
        int n = control_pad_count();
        pad_which = (pad_which + step + n) % n;
    } else {
        unsigned list[PADSRC_COUNT];
        int n = pad_candidates(list, PADSRC_COUNT), cur = -1;
        for (int i = 0; i < n; i++)
            if (list[i] == control_pad_source(pad_which)) cur = i;
        int index = cur < 0 ? (step > 0 ? 0 : n) : cur;
        index = (index + step + n + 1) % (n + 1);
        control_pad_set(pad_which, index < n ? list[index] : 0);
        apply();
    }
    menu_page_rebuild_at(p, row);
    menu_page_sound(step > 0);
}

static void pad_left(MenuPage *p, int row) { pad_change(p, row, -1); }
static void pad_right(MenuPage *p, int row) { pad_change(p, row, 1); }

/* PC SETTINGS' "Controls..." row (pc_settings.c): the hub over the page that is open. */
void port_controls_menu(void)
{
    state_load();
    hub.title = "CONTROLS";
    hub.build = hub_build;
    hub.update = NULL;
    hub.accept = hub_accept;
    hub.back = NULL;
    hub.left = hub_left;
    hub.right = hub_right;
    key_page.title = "KEYBOARD CONTROLS";
    key_page.build = key_build;
    key_page.update = NULL;
    key_page.accept = NULL;
    key_page.back = NULL;
    key_page.left = key_left;
    key_page.right = key_right;
    pad_page.title = "GAMEPAD CONTROLS";
    pad_page.build = pad_build;
    pad_page.update = NULL;
    pad_page.accept = NULL;
    pad_page.back = NULL;
    pad_page.left = pad_left;
    pad_page.right = pad_right;
    menu_page_push(&hub);
}
