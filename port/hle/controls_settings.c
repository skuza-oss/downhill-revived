/* CONTROLS (finding 157): a screen of the settings page (pc_settings.c's page, which PC SETTINGS
   opens with its Controls row), in the game's own popup style (menu_page.c), where the player
   presses the key or controller button that stands for each of the PS2 pad's buttons.
     Device:  < Keyboard >     the keyboard (player 1's), Controller 1, Controller 2
     Show:    < Buttons >      Cross to SELECT, or the d-pad and the sticks
     Cross:   Space   (also X)    the main; the dimmed alternate is launcher-only
     ...
     Reset Keyboard
   Cross on a button's row waits for a key (the keyboard) or a controller button (that player's
   controller, else any), which then takes the main slot; Esc, or five seconds without a press, gives
   up. While it waits, and until every key and button is let go after it, the game sees no input
   at all (pads_read), so the press is only the page's. Square clears the main slot. A controller's
   Show: D-pad and Sticks has Swap Sticks and an Invert for each of its sticks' axes.
   The rules are dhd_settings' (port/render/settings/src/controls.rs, through the host): a key or
   button another PS2 button had swaps with this button's old one (the hint says which), and a
   change that would leave this device without Cross, Triangle, START, R1 or a d-pad direction is
   refused (the hint says why: "Cross needs a button").
   Every change is saved to dhd.ini at once (keyboard, pad1, pad2) and the host plays with it at
   once. A layout the environment sets (DHD_KEYBOARD, DHD_PAD1, DHD_PAD2) is shown, not changed.
   Nothing here writes game state: the layouts live in the host and dhd.ini. */
#include "hle.h"
#include "menu_page.h"

#include <stdio.h>
#include <string.h>

#include "controls_table.h"  /* the targets, keys and controller inputs: generated from port/render/settings */

enum { GROUPS = 2, CAPTURE_SECONDS = 5, NOTE_SECONDS = 4 };
enum { CAPTURE_NONE, CAPTURE_WAIT, CAPTURE_RELEASE };  /* waiting for a press; for every input let go */
enum { ROW_DEVICE, ROW_GROUP, ROW_TARGET, ROW_OPTION, ROW_RESET };
enum { OPTION_SWAP, OPTION_LX, OPTION_LY, OPTION_RX, OPTION_RY, OPTIONS };

static const char *const option_label[OPTIONS] = {"Swap Sticks:", "Invert Left X:", "Invert Left Y:", "Invert Right X:",
                                                  "Invert Right Y:"};
static const uint8_t option_bit[OPTIONS] = {0, CONTROL_INVERT_LX, CONTROL_INVERT_LY, CONTROL_INVERT_RX, CONTROL_INVERT_RY};

static int device, group;             /* the layout and target group shown */
static ControlsLayout layout;          /* the device's, as the host plays with it */
static int kind;                       /* the controller's PAD_KIND_* (its buttons' words); 0 none */
static int row_kind[MENU_PAGE_ROWS], row_item[MENU_PAGE_ROWS];  /* each row's ROW_* and its target or option */
static int capture, capture_row;
static double capture_since;
static char note[96];                  /* the hint after a change */
static double note_since;
static char hint[96], reset_text[32];
static uint16_t held_before;           /* Square: a press, not a hold */

/* The game sees no pad input (pads_read): the page is waiting for a press, or for the last one to
   be let go. */
int port_controls_capturing(void) { return capture != CAPTURE_NONE; }

static int locked(void) { return host_service_host_setting_locked(GADDR(control_devices[device].env)); }

static void load(void)
{
    if (!host_service_controls_layout(device, GADDR(&layout))) layout = control_defaults[device];
    int pad = device == CONTROL_KEYBOARD ? 0 : host_service_controls_pad(device);
    kind = pad > 0 ? pad - 1 : 0;
}

/* The word for an input of the device's layout ("-": none). */
static const char *input_word(int code)
{
    if (!code) return "-";
    if (device == CONTROL_KEYBOARD) {
        for (int i = 0; i < CONTROL_KEYS; i++)
            if (control_keys[i].vk == code) return control_keys[i].label;
    } else {
        for (int i = 0; i < CONTROL_PAD_INPUTS; i++)
            if (control_pad_inputs[i].id == code)
                return control_pad_inputs[i].label[kind == PAD_KIND_PLAYSTATION ? 1 : kind == PAD_KIND_NINTENDO ? 2 : 0];
    }
    return "?";
}

/* The targets of the group shown: the keyboard's second has the d-pad and the sticks' directions,
   a controller's the d-pad (then its stick options). */
static int group_targets(int *first)
{
    if (group == 0) { *first = 0; return 12; }
    *first = 12;
    return device == CONTROL_KEYBOARD ? CONTROL_TARGETS - 12 : CONTROL_BUTTONS - 12;
}

/* The main binding, or the capture prompt. The alternate is drawn dimmed beside it. */
static void target_text(int row, char *value, size_t size)
{
    const uint8_t *in = layout.input[row_item[row]];
    const char *words[1] = {input_word(in[0])};
    if (capture == CAPTURE_WAIT && row == capture_row) words[0] = device == CONTROL_KEYBOARD ? "Press a key" : "Press a button";
    snprintf(value, size, "%s", words[0]);
}

static int option_on(int option)
{
    return option == OPTION_SWAP ? layout.swap_sticks != 0 : (layout.invert & option_bit[option]) != 0;
}

/* Each row's text again, in place. */
static void show(MenuPage *p)
{
    static const char *const group_word[GROUPS] = {"Buttons", "D-pad and Sticks"};
    char value[MENU_PAGE_TEXT];
    for (int row = 0; row < menu_page_rows(p); row++) {
        switch (row_kind[row]) {
        case ROW_DEVICE: menu_page_value_set(p, row, "Device:", control_devices[device].label, 1); break;
        case ROW_GROUP: menu_page_value_set(p, row, "Show:", group_word[group], 1); break;
        case ROW_TARGET: {
            char label[32];
            snprintf(label, sizeof label, "%s:", control_targets[row_item[row]].label);
            target_text(row, value, sizeof value);
            menu_page_value_set(p, row, label, value, MENU_VALUE_ACTION);
            break;
        }
        case ROW_OPTION:
            menu_page_value_set(p, row, option_label[row_item[row]], option_on(row_item[row]) ? "On" : "Off", !locked());
            break;
        default: break;
        }
    }
}

void controls_settings_draw(MenuPage *p)
{
    for (int row = 0; row < menu_page_rows(p); row++) {
        if (row_kind[row] != ROW_TARGET || !layout.input[row_item[row]][1] || (capture == CAPTURE_WAIT && row == capture_row)) continue;
        char text[48];
        snprintf(text, sizeof text, "(also %s)", input_word(layout.input[row_item[row]][1]));
        menu_page_suffix(p, row, text);
    }
}

const char *controls_settings_title(void) { return "CONTROLS"; }

void controls_settings_build(MenuPage *p)
{
    load();
    int first, count = group_targets(&first), row;
    memset(row_kind, 0, sizeof row_kind);
    row = menu_page_value(p, "Device:", "", 1), row_kind[row] = ROW_DEVICE;
    row = menu_page_value(p, "Show:", "", 1), row_kind[row] = ROW_GROUP;
    for (int t = first; t < first + count; t++) {
        row = menu_page_value(p, "", "", MENU_VALUE_ACTION);
        if (row >= 0) row_kind[row] = ROW_TARGET, row_item[row] = t;
    }
    if (device != CONTROL_KEYBOARD && group == 1)
        for (int o = 0; o < OPTIONS; o++) {
            row = menu_page_value(p, option_label[o], "", 1);
            if (row >= 0) row_kind[row] = ROW_OPTION, row_item[row] = o;
        }
    snprintf(reset_text, sizeof reset_text, "Reset %s", control_devices[device].label);
    row = menu_page_row(p, reset_text, !locked());
    if (row >= 0) row_kind[row] = ROW_RESET;
    menu_page_text(p, "Changes are saved as you make them");
    show(p);
}

/* The PS2 buttons players 1 and 2 hold now (their keyboard and controllers through their layouts):
   Square on the page. */
static uint16_t buttons_now(void)
{
    uint16_t held = 0;
    for (int player = 0; player < 2; player++) {
        HostPad pad;
        memset(&pad, 0, sizeof pad);
        host_service_host_pad_device(player, GADDR(&pad));
        held |= pad.buttons;
    }
    return held;
}

static void say(const char *text)
{
    snprintf(note, sizeof note, "%s", text);
    note_since = host_seconds();
}

/* A change through the host (dhd_settings' rules); the hint says what happened. */
static void change(MenuPage *p, int edit, int target, int which, int input)
{
    char message[96] = "";
    int done = host_service_controls_change(device, edit, target, which, input, GADDR(message), (int)sizeof message);
    if (done == CONTROL_REFUSED) menu_play("S_BACK");
    else if (done == CONTROL_CHANGED) menu_play("S_SELECT");
    if (*message) say(message);
    else if (done == CONTROL_CHANGED && edit != CONTROL_RESET) say("Saved");
    else if (done == CONTROL_CHANGED && edit == CONTROL_RESET) {
        char text[64];
        snprintf(text, sizeof text, "%s: back to the defaults", control_devices[device].label);
        say(text);
    }
    hle_log("controls: %s edit %d target %d slot %d input %d: %s%s%s\n", control_devices[device].key, edit, target, which, input,
            done == CONTROL_REFUSED ? "refused" : done == CONTROL_CHANGED ? "changed" : "unchanged", *message ? ": " : "", message);
    load();
    show(p);
}

void controls_settings_update(MenuPage *p, int cursor)
{
    p->footer = GLYPH_CROSS " Change  " GLYPH_SQUARE " Clear  " GLYPH_TRIANGLE " Back";
    p->hint = NULL;
    if (capture == CAPTURE_WAIT) {
        int got = host_service_controls_capture(device, 0);
        double waited = host_seconds() - capture_since;
        if (got > 0) {
            capture = CAPTURE_RELEASE;
            change(p, CONTROL_ASSIGN, row_item[capture_row], 0, got);
        } else if (got == -2 || waited >= CAPTURE_SECONDS) {
            capture = CAPTURE_RELEASE;
            menu_play("S_BACK");
            show(p);
        } else {
            snprintf(hint, sizeof hint, "%s for %s (Esc cancels) %d", device == CONTROL_KEYBOARD ? "Press a key" : "Press a button",
                     control_targets[row_item[capture_row]].label, CAPTURE_SECONDS - (int)waited);
            p->hint = hint;
            return;
        }
    }
    if (capture == CAPTURE_RELEASE && !host_service_controls_capture(device, 2)) {
        capture = CAPTURE_NONE;
        p->armed = 0;  /* the page acts again once the game has seen every button up */
    }
    /* Square on a button's row: the slot cleared (a press, not a hold) */
    uint16_t held = capture ? 0 : buttons_now();
    int square = held & PAD_SQUARE && !(held_before & PAD_SQUARE);
    held_before = held;
    if (square && p->armed && cursor >= 0 && cursor < MENU_PAGE_ROWS && row_kind[cursor] == ROW_TARGET && !locked())
        change(p, CONTROL_CLEAR, row_item[cursor], 0, 0);
    if (*note && host_seconds() - note_since < NOTE_SECONDS) {
        p->hint = note;
        return;
    }
    *note = 0;
    if (cursor < 0 || cursor >= MENU_PAGE_ROWS) return;
    if (locked()) {
        snprintf(hint, sizeof hint, "Set by %s outside the game", control_devices[device].env);
        p->hint = hint;
    } else if (row_kind[cursor] == ROW_DEVICE)
        p->hint = device == CONTROL_KEYBOARD ? "Esc always goes back in menus and pauses a race"
                                             : "Every controller of this player uses this layout";
    else if (row_kind[cursor] == ROW_TARGET) {
        const char *race = control_targets[row_item[cursor]].race;
        const char *also = input_word(layout.input[row_item[cursor]][1]);
        snprintf(hint, sizeof hint, "%s%s%s", race, layout.input[row_item[cursor]][1] ? "   also: " : "",
                 layout.input[row_item[cursor]][1] ? also : "");
        p->hint = hint;
    } else if (row_kind[cursor] == ROW_RESET)
        p->hint = "Restores this device to its default bindings";
    else if (row_kind[cursor] == ROW_OPTION)
        p->hint = row_item[cursor] == OPTION_SWAP ? "The right stick steers, the left pedals" : "Reverses this axis before Swap Sticks";
}

void controls_settings_change(MenuPage *p, int row, int step)
{
    if (row < 0 || row >= menu_page_rows(p) || capture) return;
    switch (row_kind[row]) {
    case ROW_DEVICE:
        device = (device + step + CONTROL_DEVICES) % CONTROL_DEVICES, *note = 0;
        menu_page_sound(step > 0);
        menu_page_rebuild_at(p, row);  /* a controller's directions have the stick options */
        break;
    case ROW_GROUP:
        group = (group + step + GROUPS) % GROUPS;
        menu_page_sound(step > 0);
        menu_page_rebuild_at(p, row);
        break;
    case ROW_TARGET: break;
    case ROW_OPTION:
        if (locked()) return;
        if (row_item[row] == OPTION_SWAP) change(p, CONTROL_SWAP_STICKS, 0, 0, !option_on(OPTION_SWAP));
        else change(p, CONTROL_INVERT, option_bit[row_item[row]], 0, !option_on(row_item[row]));
        break;
    default: break;
    }
}

/* Cross: wait for the slot's key or button, step a value, or reset. Returns 1 (the page stays). */
int controls_settings_accept(MenuPage *p, int row)
{
    if (row < 0 || row >= menu_page_rows(p) || capture) return 1;
    switch (row_kind[row]) {
    case ROW_DEVICE: case ROW_GROUP: case ROW_OPTION:
        controls_settings_change(p, row, 1);
        break;
    case ROW_TARGET:
        if (locked()) break;
        host_service_controls_capture(device, 1);
        capture = CAPTURE_WAIT, capture_row = row, capture_since = host_seconds(), *note = 0;
        menu_play("S_SELECT");
        show(p);
        break;
    case ROW_RESET:
        if (!locked()) change(p, CONTROL_RESET, 0, 0, 0);
        break;
    default: break;
    }
    return 1;
}

/* Opening the screen: the device and group shown last, the main slot; a button held from the row
   that opened it is no Square press. */
void controls_settings_open(void)
{
    capture = CAPTURE_NONE, *note = 0;
    held_before = 0xFFFF;
}
