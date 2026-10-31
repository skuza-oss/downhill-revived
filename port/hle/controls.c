/* Rebindable controls (controls.h, finding 124). Compiled into the game core (for the CONTROLS
   page and the DHD_KEYS scripts) and into the native host (keyboard.c, gamepad.c and host.c), so
   it uses no host API and no SDL: an SDL source is only a number and a name here. */
#include "controls.h"
#include "hle.h"   /* the PAD_* bits */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Case-insensitive comparisons of our own: strcasecmp lives in <strings.h>, which the MSVC
   headers the host build uses don't have. */
static int same_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return !*a && !*b;
}

static int starts_nocase(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
    return 1;
}

/* ---- the keyboard controls (the layout of keyboard.c, finding 059) ---- */

typedef struct {
    const char *name, *label;
    unsigned def;                     /* the default virtual-key code */
    unsigned vk;                      /* the bound one (0: unbound) */
    unsigned menu, riding;            /* the pad button in menus and while riding (0: none) */
    unsigned axis;                    /* while riding: the stick axis it moves (0: none) */
    int dir;
} KeyControl;

static KeyControl key_controls[] = {
    {"menu_left",    "Steer / Menu Left",    0x25, 0x25, PAD_LEFT,     0,            CONTROL_AXIS_LEFT_X, -1},
    {"menu_right",   "Steer / Menu Right",   0x27, 0x27, PAD_RIGHT,    0,            CONTROL_AXIS_LEFT_X,  1},
    {"menu_up",      "Lean / Menu Up",       0x26, 0x26, PAD_UP,       0,            CONTROL_AXIS_LEFT_Y, -1},
    {"menu_down",    "Lean / Menu Down",     0x28, 0x28, PAD_DOWN,     0,            CONTROL_AXIS_LEFT_Y,  1},
    {"accelerate",   "Accelerate / Confirm", 0x20, 0x20, PAD_CROSS,    PAD_CROSS,    0, 0},
    {"accelerate_x", "Accelerate (X)",       'X',  'X',  PAD_CROSS,    PAD_CROSS,    0, 0},
    {"brake",        "Brake / Back (S)",     'S',  'S',  PAD_TRIANGLE, PAD_TRIANGLE, 0, 0},
    {"brake_back",   "Brake (Backspace)",    0x08, 0x08, PAD_TRIANGLE, PAD_TRIANGLE, 0, 0},
    {"pause",        "Pause / Back (Esc)",   0x1B, 0x1B, PAD_TRIANGLE, PAD_START,    0, 0},
    {"start",        "Start (Enter)",        0x0D, 0x0D, PAD_START,    PAD_START,    0, 0},
    {"select",       "Camera / Bail (Tab)",  0x09, 0x09, PAD_SELECT,   PAD_SELECT,   0, 0},
    {"punch",        "Punch (Z)",            'Z',  'Z',  PAD_SQUARE,   PAD_SQUARE,   0, 0},
    {"kick",         "Kick (C)",             'C',  'C',  PAD_CIRCLE,   PAD_CIRCLE,   0, 0},
    {"trick_l1",     "Trick L1 (A)",         'A',  'A',  PAD_L1,       PAD_L1,       0, 0},
    {"trick_r1",     "Trick R1 (D)",         'D',  'D',  PAD_R1,       PAD_R1,       0, 0},
    {"trick_l2",     "Trick L2 (Q)",         'Q',  'Q',  PAD_L2,       PAD_L2,       0, 0},
    {"trick_r2",     "Trick R2 (E)",         'E',  'E',  PAD_R2,       PAD_R2,       0, 0},
    {"sprint",       "Sprint (W)",           'W',  'W',  PAD_R3,       PAD_R3,       0, 0},
    {"powerslide",   "Powerslide (F)",       'F',  'F',  PAD_L3,       PAD_L3,       0, 0},
};
enum { KEY_CONTROLS = sizeof key_controls / sizeof *key_controls };

/* ---- the gamepad controls (the layout of gamepad.c, finding 091) ---- */

typedef struct {
    const char *name, *label;
    unsigned button;                  /* the PAD_* bit it sets (0: none) */
    unsigned axis;                    /* the stick byte it drives (0: none) */
    unsigned def;                     /* the default SDL source (PADSRC_*) */
    unsigned source;                  /* the bound one (0: unbound) */
} PadControl;

static PadControl pad_controls[] = {
    {"south",     "Accelerate (bottom)",  PAD_CROSS,    0,                    PADSRC_SOUTH,           PADSRC_SOUTH},
    {"east",      "Kick (right)",         PAD_CIRCLE,   0,                    PADSRC_EAST,            PADSRC_EAST},
    {"west",      "Punch (left)",         PAD_SQUARE,   0,                    PADSRC_WEST,            PADSRC_WEST},
    {"north",     "Brake (top)",          PAD_TRIANGLE, 0,                    PADSRC_NORTH,           PADSRC_NORTH},
    {"back",      "Select / Camera",      PAD_SELECT,   0,                    PADSRC_BACK,            PADSRC_BACK},
    {"start",     "Start",                PAD_START,    0,                    PADSRC_START,           PADSRC_START},
    {"l1",        "Trick L1",             PAD_L1,       0,                    PADSRC_LEFT_SHOULDER,   PADSRC_LEFT_SHOULDER},
    {"r1",        "Trick R1",             PAD_R1,       0,                    PADSRC_RIGHT_SHOULDER,  PADSRC_RIGHT_SHOULDER},
    {"l2",        "Trick L2 (trigger)",   PAD_L2,       0,                    PADSRC_LEFT_TRIGGER,    PADSRC_LEFT_TRIGGER},
    {"r2",        "Trick R2 (trigger)",   PAD_R2,       0,                    PADSRC_RIGHT_TRIGGER,   PADSRC_RIGHT_TRIGGER},
    {"l3",        "Powerslide (L3)",      PAD_L3,       0,                    PADSRC_L3,              PADSRC_L3},
    {"r3",        "Sprint (R3)",          PAD_R3,       0,                    PADSRC_R3,              PADSRC_R3},
    {"dpad_up",   "Menu up (d-pad)",      PAD_UP,       0,                    PADSRC_DPAD_UP,         PADSRC_DPAD_UP},
    {"dpad_down", "Menu down (d-pad)",    PAD_DOWN,     0,                    PADSRC_DPAD_DOWN,       PADSRC_DPAD_DOWN},
    {"dpad_left", "Menu left (d-pad)",    PAD_LEFT,     0,                    PADSRC_DPAD_LEFT,       PADSRC_DPAD_LEFT},
    {"dpad_right", "Menu right (d-pad)",  PAD_RIGHT,    0,                    PADSRC_DPAD_RIGHT,      PADSRC_DPAD_RIGHT},
    {"left_x",    "Left stick X (steer)", 0,            CONTROL_AXIS_LEFT_X,  PADSRC_LEFT_X,          PADSRC_LEFT_X},
    {"left_y",    "Left stick Y (lean)",  0,            CONTROL_AXIS_LEFT_Y,  PADSRC_LEFT_Y,          PADSRC_LEFT_Y},
    {"right_x",   "Right stick X",        0,            CONTROL_AXIS_RIGHT_X, PADSRC_RIGHT_X,         PADSRC_RIGHT_X},
    {"right_y",   "Right stick Y (pedal)", 0,           CONTROL_AXIS_RIGHT_Y, PADSRC_RIGHT_Y,         PADSRC_RIGHT_Y},
};
enum { PAD_CONTROLS = sizeof pad_controls / sizeof *pad_controls };

/* ---- the SDL sources: their names, and which of them a control may take ---- */

static const struct { unsigned source; const char *name; } pad_sources[] = {
    {PADSRC_SOUTH, "south"}, {PADSRC_EAST, "east"}, {PADSRC_WEST, "west"}, {PADSRC_NORTH, "north"},
    {PADSRC_BACK, "back"}, {PADSRC_START, "start"},
    {PADSRC_LEFT_SHOULDER, "left_shoulder"}, {PADSRC_RIGHT_SHOULDER, "right_shoulder"},
    {PADSRC_DPAD_UP, "dpad_up"}, {PADSRC_DPAD_DOWN, "dpad_down"},
    {PADSRC_DPAD_LEFT, "dpad_left"}, {PADSRC_DPAD_RIGHT, "dpad_right"},
    {PADSRC_L3, "l3"}, {PADSRC_R3, "r3"},
    {PADSRC_LEFT_TRIGGER, "left_trigger"}, {PADSRC_RIGHT_TRIGGER, "right_trigger"},
    {PADSRC_LEFT_X, "left_x"}, {PADSRC_LEFT_Y, "left_y"},
    {PADSRC_RIGHT_X, "right_x"}, {PADSRC_RIGHT_Y, "right_y"},
};
enum { PAD_SOURCES = sizeof pad_sources / sizeof *pad_sources };

/* A stick source (an axis) rather than a button or a trigger. */
static int source_is_axis(unsigned source)
{
    return source == PADSRC_LEFT_X || source == PADSRC_LEFT_Y || source == PADSRC_RIGHT_X || source == PADSRC_RIGHT_Y;
}

int control_pad_accepts(int i, unsigned source)
{
    if (i < 0 || i >= PAD_CONTROLS) return 0;
    if (!source) return 1;                          /* unbound is always allowed */
    return pad_controls[i].axis ? source_is_axis(source) : !source_is_axis(source);
}

/* ---- the key catalog the page cycles (US virtual-key codes) ---- */

typedef struct { const char *name; unsigned vk; } KeyName;

static const KeyName key_catalog[] = {
    {"a", 'A'}, {"b", 'B'}, {"c", 'C'}, {"d", 'D'}, {"e", 'E'}, {"f", 'F'}, {"g", 'G'}, {"h", 'H'},
    {"i", 'I'}, {"j", 'J'}, {"k", 'K'}, {"l", 'L'}, {"m", 'M'}, {"n", 'N'}, {"o", 'O'}, {"p", 'P'},
    {"q", 'Q'}, {"r", 'R'}, {"s", 'S'}, {"t", 'T'}, {"u", 'U'}, {"v", 'V'}, {"w", 'W'}, {"x", 'X'},
    {"y", 'Y'}, {"z", 'Z'},
    {"0", '0'}, {"1", '1'}, {"2", '2'}, {"3", '3'}, {"4", '4'}, {"5", '5'}, {"6", '6'}, {"7", '7'},
    {"8", '8'}, {"9", '9'},
    {"left", 0x25}, {"up", 0x26}, {"right", 0x27}, {"down", 0x28},
    {"space", 0x20}, {"enter", 0x0D}, {"escape", 0x1B}, {"tab", 0x09}, {"backspace", 0x08},
};
enum { KEY_NAMES = sizeof key_catalog / sizeof *key_catalog };

/* ---- the keyboard controls ---- */

int control_key_count(void) { return KEY_CONTROLS; }
const char *control_key_id(int i) { return i >= 0 && i < KEY_CONTROLS ? key_controls[i].name : ""; }
const char *control_key_label(int i) { return i >= 0 && i < KEY_CONTROLS ? key_controls[i].label : ""; }
unsigned control_key_vk(int i) { return i >= 0 && i < KEY_CONTROLS ? key_controls[i].vk : 0; }
unsigned control_key_default_vk(int i) { return i >= 0 && i < KEY_CONTROLS ? key_controls[i].def : 0; }
unsigned control_key_menu(int i) { return i >= 0 && i < KEY_CONTROLS ? key_controls[i].menu : 0; }
unsigned control_key_riding(int i) { return i >= 0 && i < KEY_CONTROLS ? key_controls[i].riding : 0; }
unsigned control_key_axis(int i) { return i >= 0 && i < KEY_CONTROLS ? key_controls[i].axis : 0; }
int control_key_dir(int i) { return i >= 0 && i < KEY_CONTROLS ? key_controls[i].dir : 0; }
void control_key_set(int i, unsigned vk) { if (i >= 0 && i < KEY_CONTROLS) key_controls[i].vk = vk; }

int control_key_index(const char *id)
{
    if (!id) return -1;
    for (int i = 0; i < KEY_CONTROLS; i++)
        if (same_nocase(key_controls[i].name, id)) return i;
    return -1;
}

/* ---- the gamepad controls ---- */

int control_pad_count(void) { return PAD_CONTROLS; }
const char *control_pad_id(int i) { return i >= 0 && i < PAD_CONTROLS ? pad_controls[i].name : ""; }
const char *control_pad_label(int i) { return i >= 0 && i < PAD_CONTROLS ? pad_controls[i].label : ""; }
unsigned control_pad_button(int i) { return i >= 0 && i < PAD_CONTROLS ? pad_controls[i].button : 0; }
unsigned control_pad_axis(int i) { return i >= 0 && i < PAD_CONTROLS ? pad_controls[i].axis : 0; }
unsigned control_pad_source(int i) { return i >= 0 && i < PAD_CONTROLS ? pad_controls[i].source : 0; }
unsigned control_pad_default_source(int i) { return i >= 0 && i < PAD_CONTROLS ? pad_controls[i].def : 0; }
void control_pad_set(int i, unsigned source) { if (i >= 0 && i < PAD_CONTROLS && control_pad_accepts(i, source)) pad_controls[i].source = source; }

int control_pad_index(const char *id)
{
    if (!id) return -1;
    for (int i = 0; i < PAD_CONTROLS; i++)
        if (same_nocase(pad_controls[i].name, id)) return i;
    return -1;
}

/* ---- the catalogs ---- */

int control_key_catalog_count(void) { return KEY_NAMES; }
const char *control_key_catalog_name(int j) { return j >= 0 && j < KEY_NAMES ? key_catalog[j].name : ""; }
unsigned control_key_catalog_vk(int j) { return j >= 0 && j < KEY_NAMES ? key_catalog[j].vk : 0; }

int control_key_catalog_index(unsigned vk)
{
    for (int j = 0; j < KEY_NAMES; j++)
        if (key_catalog[j].vk == vk) return j;
    return -1;
}

int control_pad_catalog_count(void) { return PAD_SOURCES; }
const char *control_pad_catalog_name(int j) { return j >= 0 && j < PAD_SOURCES ? pad_sources[j].name : ""; }
unsigned control_pad_catalog_source(int j) { return j >= 0 && j < PAD_SOURCES ? pad_sources[j].source : 0; }

int control_pad_catalog_index(unsigned source)
{
    for (int j = 0; j < PAD_SOURCES; j++)
        if (pad_sources[j].source == source) return j;
    return -1;
}

const char *control_pad_source_name(unsigned source)
{
    int j = control_pad_catalog_index(source);
    return j < 0 ? "" : pad_sources[j].name;
}

unsigned control_pad_source_of_name(const char *name)
{
    if (!name || !*name) return 0;
    for (int j = 0; j < PAD_SOURCES; j++)
        if (same_nocase(pad_sources[j].name, name)) return pad_sources[j].source;
    return 0;
}

unsigned control_key_vk_of_name(const char *name)
{
    if (!name || !*name) return 0;
    for (int j = 0; j < KEY_NAMES; j++)
        if (same_nocase(key_catalog[j].name, name)) return key_catalog[j].vk;
    return 0;
}

const char *control_key_name_of_vk(unsigned vk)
{
    int j = control_key_catalog_index(vk);
    return j < 0 ? "" : key_catalog[j].name;
}

/* ---- the profiles ---- */

void controls_default(void)
{
    for (int i = 0; i < KEY_CONTROLS; i++) key_controls[i].vk = key_controls[i].def;
    for (int i = 0; i < PAD_CONTROLS; i++) pad_controls[i].source = pad_controls[i].def;
}

int controls_customised(void)
{
    for (int i = 0; i < KEY_CONTROLS; i++)
        if (key_controls[i].vk != key_controls[i].def) return 1;
    for (int i = 0; i < PAD_CONTROLS; i++)
        if (pad_controls[i].source != pad_controls[i].def) return 1;
    return 0;
}

/* Commas, semicolons, spaces and line endings all separate one assignment from the next. */
static const char *skip_space(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return s;
}

int controls_parse(const char *text)
{
    int applied = 0;
    if (!text) return 0;
    while (*text) {
        const char *end = text;
        while (*end && *end != ',' && *end != ';' && *end != '\r' && *end != '\n') end++;
        const char *eq = memchr(text, '=', (size_t)(end - text));
        if (eq) {
            char id[48], value[48];
            const char *k = skip_space(text), *k_end = eq;
            const char *v = skip_space(eq + 1), *v_end = end;
            while (k_end > k && (k_end[-1] == ' ' || k_end[-1] == '\t')) k_end--;
            while (v_end > v && (v_end[-1] == ' ' || v_end[-1] == '\t')) v_end--;
            if ((size_t)(k_end - k) < sizeof id && (size_t)(v_end - v) < sizeof value) {
                snprintf(id, sizeof id, "%.*s", (int)(k_end - k), k);
                snprintf(value, sizeof value, "%.*s", (int)(v_end - v), v);
                if (starts_nocase(id, "key.")) {
                    int i = control_key_index(id + 4);
                    unsigned vk = *value ? control_key_vk_of_name(value) : 0;
                    if (i >= 0 && (!*value || vk)) { control_key_set(i, vk); applied++; }
                } else if (starts_nocase(id, "pad.")) {
                    int i = control_pad_index(id + 4);
                    unsigned source = *value ? control_pad_source_of_name(value) : 0;
                    if (i >= 0 && (!*value || (source && control_pad_accepts(i, source)))) {
                        control_pad_set(i, source);
                        applied++;
                    }
                }
            }
        }
        text = *end ? end + 1 : end;
    }
    return applied;
}

int controls_format(char *out, int size)
{
    int n = 0;
    if (size <= 0) return -1;
    out[0] = 0;
    for (int i = 0; i < KEY_CONTROLS; i++) {
        int w = snprintf(out + n, (size_t)(size - n), "%skey.%s=%s", n ? "," : "", key_controls[i].name,
                         control_key_name_of_vk(key_controls[i].vk));
        if (w < 0 || n + w >= size) return -1;
        n += w;
    }
    for (int i = 0; i < PAD_CONTROLS; i++) {
        int w = snprintf(out + n, (size_t)(size - n), ",pad.%s=%s", pad_controls[i].name,
                         control_pad_source_name(pad_controls[i].source));
        if (w < 0 || n + w >= size) return -1;
        n += w;
    }
    return n;
}

void control_profile_key(const char *name, char *out, int size)
{
    char clean[48];
    int n = 0;
    if (size <= 0) return;
    if (!name || !*name || same_nocase(name, "default")) { snprintf(out, (size_t)size, "bindings"); return; }
    for (const char *p = name; *p && n < (int)sizeof clean - 1; p++)
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')
            clean[n++] = *p;
    clean[n] = 0;
    if (!n) snprintf(out, (size_t)size, "bindings");
    else snprintf(out, (size_t)size, "bindings_%s", clean);
}
