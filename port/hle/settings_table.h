/* The settings' choices for the game's PC SETTINGS page (pc_settings.c): generated from the one
   table of dhd.ini's settings, port/render/settings/src/table.rs (dhd_settings), whose test
   keeps this file equal to it (DHD_SETTINGS_BLESS=1 cargo test -p dhd_settings writes it).
   Don't edit it here. */
#ifndef DHD_SETTINGS_TABLE_H
#define DHD_SETTINGS_TABLE_H

typedef struct { const char *value, *label; } SettingChoice;
/* key: the dhd.ini key; env: the variable ("" none); builtin: the value without one. */
typedef struct { const char *key, *env, *builtin; const SettingChoice *choices; int count; } SettingInfo;

static const SettingChoice setting_choices_scale[] = {{"1", "1x"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {"5", "5x"}, {"6", "6x"}, {"7", "7x"}, {"8", "8x"}, {"window", "Match window"}};
static const SettingChoice setting_choices_window[] = {{"windowed", "Window"}, {"borderless", "Borderless"}, {"fullscreen", "Fullscreen"}};
static const SettingChoice setting_choices_size[] = {{"monitor", "Monitor size"}, {"896x672", "896x672"}, {"1280x960", "1280x960"}, {"1440x1080", "1440x1080"}, {"1280x720", "1280x720"}, {"1920x1080", "1920x1080"}, {"2560x1440", "2560x1440"}};
static const SettingChoice setting_choices_widescreen[] = {{"0", "OFF"}, {"1", "ON"}};
static const SettingChoice setting_choices_dhd_aspect[] = {{"4:3", "4:3"}, {"wide", "Wide"}, {"16:9", "16:9"}, {"stretch", "Stretch"}, {"pixels", "Pixels"}};
static const SettingChoice setting_choices_dhd_present_filter[] = {{"linear", "Smooth"}, {"point", "Sharp"}};
static const SettingChoice setting_choices_renderer[] = {{"native", "Native"}, {"exact", "Exact"}};
static const SettingChoice setting_choices_skip_movies[] = {{"0", "OFF"}, {"1", "ON"}};
static const SettingChoice setting_choices_vibration[] = {{"0", "OFF"}, {"1", "ON"}};
static const SettingChoice setting_choices_stick_dead_zone[] = {{"game", "Game (36%)"}, {"5", "5%"}, {"10", "10%"}, {"15", "15%"}, {"20", "20%"}, {"25", "25%"}, {"30", "30%"}, {"40", "40%"}, {"50", "50%"}};
static const SettingChoice setting_choices_prompts[] = {{"auto", "Auto"}, {"playstation", "PlayStation"}, {"xbox", "Xbox"}, {"nintendo", "Nintendo"}};
static const SettingChoice setting_choices_textures[] = {{"off", "OFF"}, {"on", "ON"}};
static const SettingChoice setting_choices_font[] = {{"original", "Original"}, {"sharp", "Sharp"}};
static const SettingChoice setting_choices_fps[] = {{"60", "60 fps"}, {"120", "120 fps"}, {"144", "144 fps"}, {"165", "165 fps"}, {"240", "240 fps"}, {"monitor", "Monitor"}};
static const SettingChoice setting_choices_vsync[] = {{"0", "OFF"}, {"1", "ON"}};
static const SettingChoice setting_choices_perf_counter[] = {{"0", "OFF"}, {"1", "ON"}};
static const SettingChoice setting_choices_unlock_all[] = {{"0", "OFF"}, {"1", "ON"}};
static const SettingChoice setting_choices_touch_preset[] = {{"thumb", "Thumb zones"}, {"classic", "Classic pad"}};
static const SettingChoice setting_choices_touch_hand[] = {{"right", "Right"}, {"left", "Left"}};
static const SettingChoice setting_choices_touch_steer[] = {{"stick", "Stick"}, {"tilt", "Tilt"}};
static const SettingChoice setting_choices_touch_size[] = {{"auto", "Auto"}, {"70", "70%"}, {"75", "75%"}, {"80", "80%"}, {"85", "85%"}, {"90", "90%"}, {"95", "95%"}, {"100", "100%"}, {"105", "105%"}};
static const SettingChoice setting_choices_touch_opacity[] = {{"25", "25%"}, {"30", "30%"}, {"35", "35%"}, {"40", "40%"}, {"45", "45%"}, {"50", "50%"}, {"55", "55%"}, {"60", "60%"}, {"65", "65%"}, {"70", "70%"}, {"75", "75%"}, {"80", "80%"}, {"85", "85%"}, {"90", "90%"}, {"95", "95%"}, {"100", "100%"}};
static const SettingChoice setting_choices_touch_stick[] = {{"floating", "Floating"}, {"fixed", "Fixed"}};
static const SettingChoice setting_choices_touch_stick_range[] = {{"8", "8 mm"}, {"10", "10 mm"}, {"12", "12 mm"}, {"14", "14 mm"}, {"16", "16 mm"}, {"18", "18 mm"}, {"20", "20 mm"}};
static const SettingChoice setting_choices_touch_trick_stick[] = {{"25", "25 mm"}, {"28", "28 mm"}, {"31", "31 mm"}, {"34", "34 mm"}, {"37", "37 mm"}};
static const SettingChoice setting_choices_touch_labels[] = {{"auto", "Auto"}, {"always", "Always"}, {"off", "Off"}};
static const SettingChoice setting_choices_touch_attacks[] = {{"flick", "Flick"}, {"buttons", "Buttons"}};
static const SettingChoice setting_choices_touch_vibration[] = {{"0", "Off"}, {"1", "On"}};
static const SettingChoice setting_choices_touch_with_pad[] = {{"hide", "Hide"}, {"show", "Show"}};
static const SettingChoice setting_choices_touch_tilt_strength[] = {{"0", "0%"}, {"10", "10%"}, {"20", "20%"}, {"30", "30%"}, {"40", "40%"}, {"50", "50%"}, {"60", "60%"}, {"70", "70%"}, {"80", "80%"}, {"90", "90%"}, {"100", "100%"}};
static const SettingChoice setting_choices_names[] = {{"off", "Off"}, {"small", "Small"}, {"medium", "Medium"}, {"large", "Large"}};
static const SettingChoice setting_choices_auto_update[] = {{"ask", "Ask"}, {"on", "Automatic"}, {"off", "Off"}};
static const SettingChoice setting_choices_discord[] = {{"off", "OFF"}, {"on", "ON"}};

static const SettingInfo setting_table[] = {
    {"scale", "DHD_SCALE", "2", setting_choices_scale, 9},
    {"window", "DHD_WINDOW", "windowed", setting_choices_window, 3},
    {"size", "", "896x672", setting_choices_size, 7},
    {"widescreen", "DHD_WIDESCREEN", "0", setting_choices_widescreen, 2},
    {"DHD_ASPECT", "DHD_ASPECT", "4:3", setting_choices_dhd_aspect, 5},
    {"DHD_PRESENT_FILTER", "DHD_PRESENT_FILTER", "linear", setting_choices_dhd_present_filter, 2},
    {"renderer", "DHD_RENDER", "exact", setting_choices_renderer, 2},
    {"skip_movies", "DHD_SKIP_MOVIES", "0", setting_choices_skip_movies, 2},
    {"vibration", "DHD_VIBRATION", "1", setting_choices_vibration, 2},
    {"stick_dead_zone", "DHD_STICK_DEAD_ZONE", "game", setting_choices_stick_dead_zone, 9},
    {"prompts", "DHD_PROMPTS", "playstation", setting_choices_prompts, 4},
    {"textures", "DHD_TEXTURES", "on", setting_choices_textures, 2},
    {"font", "DHD_FONT", "sharp", setting_choices_font, 2},
    {"fps", "DHD_FPS", "60", setting_choices_fps, 6},
    {"vsync", "DHD_VSYNC", "0", setting_choices_vsync, 2},
    {"perf_counter", "DHD_PERF_COUNTER", "0", setting_choices_perf_counter, 2},
    {"unlock_all", "DHD_UNLOCK_ALL", "0", setting_choices_unlock_all, 2},
    {"touch_preset", "DHD_TOUCH_PRESET", "classic", setting_choices_touch_preset, 2},
    {"touch_hand", "DHD_TOUCH_HAND", "right", setting_choices_touch_hand, 2},
    {"touch_steer", "DHD_TOUCH_STEER", "stick", setting_choices_touch_steer, 2},
    {"touch_size", "DHD_TOUCH_SIZE", "auto", setting_choices_touch_size, 9},
    {"touch_opacity", "DHD_TOUCH_OPACITY", "60", setting_choices_touch_opacity, 16},
    {"touch_stick", "DHD_TOUCH_STICK", "floating", setting_choices_touch_stick, 2},
    {"touch_stick_range", "DHD_TOUCH_STICK_RANGE", "12", setting_choices_touch_stick_range, 7},
    {"touch_trick_stick", "DHD_TOUCH_TRICK_STICK", "31", setting_choices_touch_trick_stick, 5},
    {"touch_labels", "DHD_TOUCH_LABELS", "auto", setting_choices_touch_labels, 3},
    {"touch_attacks", "DHD_TOUCH_ATTACKS", "flick", setting_choices_touch_attacks, 2},
    {"touch_vibration", "DHD_TOUCH_VIBRATION", "1", setting_choices_touch_vibration, 2},
    {"touch_with_pad", "DHD_TOUCH_WITH_PAD", "hide", setting_choices_touch_with_pad, 2},
    {"touch_tilt_strength", "DHD_TOUCH_TILT_STRENGTH", "50", setting_choices_touch_tilt_strength, 11},
    {"names", "DHD_NAMES", "small", setting_choices_names, 4},
    {"auto_update", "", "ask", setting_choices_auto_update, 3},
    {"discord", "DHD_DISCORD", "off", setting_choices_discord, 2},
};

/* The rider name ("name", DHD_NET_NAME): up to SETTING_NAME_MAX of SETTING_NAME_LETTERS, upper
   case; SETTING_NAME_UNNAMED while none is set. Up to 12 letters, digits, spaces or dashes. */
#define SETTING_NAME_LETTERS "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -"
#define SETTING_NAME_MAX 12
#define SETTING_NAME_UNNAMED "RIDER"

#endif
