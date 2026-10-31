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
static const SettingChoice setting_choices_textures[] = {{"off", "OFF"}, {"on", "ON"}};
static const SettingChoice setting_choices_font[] = {{"original", "Original"}, {"sharp", "Sharp"}};
static const SettingChoice setting_choices_fps[] = {{"60", "60 fps"}, {"120", "120 fps"}, {"144", "144 fps"}, {"165", "165 fps"}, {"240", "240 fps"}, {"monitor", "Monitor"}};
static const SettingChoice setting_choices_vsync[] = {{"0", "OFF"}, {"1", "ON"}};
static const SettingChoice setting_choices_perf_counter[] = {{"0", "OFF"}, {"1", "ON"}};
static const SettingChoice setting_choices_voice[] = {{"0", "OFF"}, {"1", "ON"}};
static const SettingChoice setting_choices_voice_mode[] = {{"ptt", "Push to Talk"}, {"open", "Open Mic"}};
static const SettingChoice setting_choices_voice_volume[] = {{"0", "0%"}, {"20", "20%"}, {"40", "40%"}, {"60", "60%"}, {"80", "80%"}, {"100", "100%"}};
static const SettingChoice setting_choices_voice_mic[] = {{"50", "50%"}, {"75", "75%"}, {"100", "100%"}, {"150", "150%"}, {"200", "200%"}};
static const SettingChoice setting_choices_voice_range[] = {{"25", "25 m"}, {"50", "50 m"}, {"100", "100 m"}, {"200", "200 m"}};
static const SettingChoice setting_choices_voice_bitrate[] = {{"8000", "8 kbps"}, {"16000", "16 kbps"}, {"24000", "24 kbps"}, {"32000", "32 kbps"}, {"48000", "48 kbps"}, {"64000", "64 kbps"}};
static const SettingChoice setting_choices_unlock_all[] = {{"0", "OFF"}, {"1", "ON"}};
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
    {"textures", "DHD_TEXTURES", "on", setting_choices_textures, 2},
    {"font", "DHD_FONT", "sharp", setting_choices_font, 2},
    {"fps", "DHD_FPS", "60", setting_choices_fps, 6},
    {"vsync", "DHD_VSYNC", "0", setting_choices_vsync, 2},
    {"perf_counter", "DHD_PERF_COUNTER", "0", setting_choices_perf_counter, 2},
    {"voice", "DHD_VOICE", "1", setting_choices_voice, 2},
    {"voice_mode", "DHD_VOICE_MODE", "ptt", setting_choices_voice_mode, 2},
    {"voice_volume", "DHD_VOICE_VOLUME", "100", setting_choices_voice_volume, 6},
    {"voice_mic", "DHD_VOICE_MIC", "100", setting_choices_voice_mic, 5},
    {"voice_range", "DHD_VOICE_RANGE", "50", setting_choices_voice_range, 4},
    {"voice_bitrate", "DHD_VOICE_BITRATE", "24000", setting_choices_voice_bitrate, 6},
    {"unlock_all", "DHD_UNLOCK_ALL", "0", setting_choices_unlock_all, 2},
    {"auto_update", "", "ask", setting_choices_auto_update, 3},
    {"discord", "DHD_DISCORD", "off", setting_choices_discord, 2},
};

/* The rider name ("name", DHD_NET_NAME): up to SETTING_NAME_MAX of SETTING_NAME_LETTERS, upper
   case; SETTING_NAME_UNNAMED while none is set. Up to 12 letters, digits, spaces or dashes. */
#define SETTING_NAME_LETTERS "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -"
#define SETTING_NAME_MAX 12
#define SETTING_NAME_UNNAMED "RIDER"

#endif
