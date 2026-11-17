/* Online rooms in the game's popup style (menu_page.c), with physical keyboard text entry.
   The flow (finding 092): Create or Join waits on the ONLINE page until the server has the player
   in the room; then the game's own rider screen, once, its ride to the shop and the shop's own
   pages (BIKES / JERSEYS / BIKE COLOR, finding 128), whose START (port_online_shop_commit) keeps
   the choice and opens the RACE ROOM over the rider screen (the rider on its stand, Change Rider
   to choose again); the host starts; loading; the race. The host chooses the track and starts
   when everyone is ready. The existing online.c setup event carries each complete choice. */
#include "hle.h"
#include "menu_page.h"
#include "room_view.h"
#include <ctype.h>
#include "text_input.h"
#include "settings_table.h"  /* the rider name's rule (dhd_settings, port/render/settings) */
#include "../net/net.h"
#include "../net/lobby_row.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Downhill Domination (USA) SCUS-97177, redump disc 30393 (whole file 0fe2b777...): its game files
   hash, dhd_disc's USA_FILES_SHA256 (port/render/disc, finding 098). Only the files count, so a
   good dump with other sectors around them is taken too. */
static const char us_files_sha256[] = "c8c17bcae383daf9f2feadc7f6d7476c43d659ab8d66127a299b01311f2a3476";

/* The online tracks and the riders every save has (online.c regular): the game's indexes and
   names, as the online launcher lists them. Every PC races its own one-player race (finding 044),
   so a track needs only its one-player level: Salt City MX (17) and TD (18), which have no 2-4
   player level, are in. Not 21, DUAL: the career's one-on-one Dual Slalom, with two start places
   and no Single Event entry (func_001E3810 skips it), finding 166. */
/* Retail's full event names (also used by its Single Event and records pages).
   Read from the player's data image; omit country, keep FR/MX/TD suffixes intact. */
extern const char *D_0026EFE8[];
/* In the Super Career's order, first race to last: retail's D_0026F150 (func_001821E8's career
   type 3, SETUP+0x10C == 3), read from the player's data image, with Mosh Pit, which no career
   races, last as in the game's own track lists (D_00296F60). Only the order is ours: the ids,
   limits and pick-only arena are the game's, so the server and older games see the same tracks
   (finding 190). The picker, Left / Right on TRACK and the host's rotation (NEXT, RANDOM, the
   grid-lock switch) all walk it. Read, not copied: any copy of the list in the port's code is a
   copy of retail's .data that package_release.py's game-data check refuses, as ints and even as
   bytes, which the wasm compiler widened into vector constants of the same ints for its searches
   (the macOS build laid them out in one run). */
extern const int D_0026F150[];
enum { TRACK_COUNT = 25, MOSH_PIT = 23 };
static int track_at(int k)
{
    return k < TRACK_COUNT - 1 ? D_0026F150[k] : MOSH_PIT;
}
static const char *track_name(int index)
{
    static char names[26][64];
    if (index < 0 || index > 25) return "?";
    if (!names[index][0]) {
        const char *full = D_0026EFE8[index], *country = strchr(full, ',');
        const char *mode = strrchr(full, ' ');
        if (country && mode && mode > country)
            snprintf(names[index], sizeof names[index], "%.*s%s", (int)(country - full), full, mode);
        else snprintf(names[index], sizeof names[index], index == 23 ? "%s (Arena)" : "%s", full);
    }
    return names[index];
}
static const struct { int index; const char *name; } riders[] = {
    {0, "COSMO"}, {1, "MAI"}, {2, "AJAX"}, {3, "JONNIE"}, {4, "KALOLO"}, {5, "T-BAG"}, {8, "DRUD"},
    {10, "RAKEL"}, {13, "JEN"}, {14, "WOODY"}};
#define COUNT(a) ((int)(sizeof a / sizeof *a))
/* MOSH (23) is the Mosh Pit arena, where riders knock each other around: not a race. The host can
   pick it from the track list, but NEXT / RANDOM after a race (the server's pickOnly) and Left /
   Right on the room's track never land on it (finding 128). */
static int arena(int track) { return track == 23; }

static const char code_letters[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";  /* the server's join codes */
static const char name_letters[] = SETTING_NAME_LETTERS;
/* CODE_LENGTH: an SFU room's code; a P2P room's has one more (port/net/net.h, finding 180). */
enum { NAME_MAX = SETTING_NAME_MAX, CODE_LENGTH = 6, P2P_CODE_LENGTH = 7, ROW_TEXT = 96, MAX_ROWS = 12, LIST_MAX = 8, MEMBER_ROWS = 10 };
/* HOME's rows, in this order, those the platform offers (host_service_net_offered: SFU rooms in
   every native build, phones included (finding 237), P2P rooms everywhere, finding 180);
   home_kind[] says what each shown row does. */
enum { HOME_NAME, HOME_QUICK, HOME_LIST, HOME_PUBLIC, HOME_PRIVATE, HOME_REGION, HOME_P2P, HOME_JOIN, HOME_UPDATE, HOME_KINDS };

enum { HOME, LIST, NAME, CODE, ROOM, QUICK };
/* The room's rows */
/* The room's options, carried in the server's "laps" setting (Downhill Domination has no laps):
   every PC builds the same race from them (online.c hle_online_ai_count, hle_cheat_code). Cheat
   codes are off unless the host turns them on (finding 117). */
enum { ROOM_NO_BOTS = 1, ROOM_CHEATS = 2 };
static const char *const rider_names[16] = {
    "COSMO", "MAI", "AJAX", "JONNIE", "KALOLO", "T-BAG", "ERIC CARTER", "TARA LLANES", "DRUD",
    "BRIAN LOPES", "RAKEL", "MISSY GIOVE", "RICHIE SCHLEY", "JEN", "WOODY", "KINETICLOPS"};
static MenuPage page;
static int screen, race;
static int choosing, selected, selection_requested;
static int room_closed;  /* the race room was left this frame: nothing more to draw */
/* The room over the rider screen (port_online_shell): in_shell while it runs; room_pending
   while the camera flies to the rider screen it opens over; hold_rider until the press that
   closed it is released; resume_home: back on the title, ONLINE opens again. */
static int in_shell, room_pending, hold_rider, resume_home;
/* shop_done: the shop's START kept the choice (port_online_shop_commit); its Back path is running */
static int shop_done;
/* connecting: Create or Join was chosen; the page waits for the server's room (connect_poll). */
static int connecting;
static int chosen_rider, chosen_bike;
static int chosen_spot;  /* the chosen rider's place on the rider screen's carousel (D_004C6910), 0 none */
static int public_room, room_settings_sent, room_ready_sent, room_track = 0, room_options;
extern int func_00182290(int track);
static int track_limit(int track) { int kind = func_00182290(track); return kind == 2 ? 10 : kind == 1 ? 8 : 6; }
static int rotation_sent;  /* the host's track list for the room's Then rule, once a room */
static int32_t chosen_outfit[3], chosen_cycle[3], chosen_colour;  /* colour: SETUP+0x254, the BIKE COLOR page's */
#define SHELL_WORD(a) (*(int32_t *)(uintptr_t)(0x08000000u + (a)))
#define SETUP_WORD(offset) SHELL_WORD(0x4B25A0 + (offset))
void port_online_choose_begin(int flight);
void port_online_choose_end(void);
void func_001F67F0(int enabled);
void func_001F6798(int enabled);
void func_001F5B08(int enabled);
void func_001F6940(void);
void func_0021E428(int enabled);
void func_001F65E0(int enabled);
void func_001F5578(void);
void func_001F60C8(int rider);
void func_00203590(void *animation, int resource, int bank, int index);
void func_00203FE0(void *animation, int enabled);
void func_002038E0(void *animation, int enabled);
void func_001F50F0(int resource, int index);
void func_001F57D0(int resource, int enabled);
void func_001C59D8(void *sound);
void *func_0023D040(int view);
void func_0023D020(int view, void *camera);
void func_0023C8D8(int mode, int view);
void func_0023D898(int view, float *distance, float *aspect);
void func_0023D8D8(int view, float distance, float aspect);
float func_00203C28(void *animation);
static float shell_distance, shell_aspect;
static int offline_mode, offline_submode, offline_players;
void hle_online_selected_ride(int rider, const int32_t *outfit, const int32_t *bike, int colour);

/* SharedMenu can change a one-view shell's render mode in place; the stock popup
   manager only restores its view descriptor when it began with multiple views. */
static void run_page(void)
{
    void *camera = func_0023D040(0);
    int mode = SHELL_WORD(0x75A9D8);
    float distance, aspect;
    func_0023D898(0, &distance, &aspect);
    menu_page_run(&page);
    func_0023D020(0, camera);
    func_0023C8D8(mode, 0);
    func_0023D8D8(0, distance, aspect);
    host_service_text_input_end();
}

/* The retail rider screen's Back cleanup (func_001F24B8), also needed when
   intercepting GET READY: changing only MENU bypasses all of these side effects. */
static void selection_end(void)
{
    void *animation = (void *)(uintptr_t)(0x08000000u + 0x4C6370);
    void *sound = (void *)(uintptr_t)(0x08000000u + 0x4B2A90);
    func_001F67F0(0);
    func_001F6798(0);
    func_001F5B08(0);
    func_001F6940();
    SHELL_WORD(0x29E024) = 0;
    func_0021E428(0);
    func_001F65E0(0);
    func_001F5578();
    func_001F60C8(0);
    for (int i = 0; i < 7; i++) SETUP_WORD(0x2A8 + 4 * i) = -1;
    SHELL_WORD(0x29DF68) = 0;
    SHELL_WORD(0x4C6910) = 1;
    func_00203590(animation, 0x2330, 0, 1);
    func_00203FE0(animation, 1);
    func_002038E0(animation, 0);
    func_001F50F0(0x2330, 1);
    func_001F57D0(0x232B, 0);
    func_001F57D0(0x233D, 0);
    func_001F57D0(0x235F, 0);
    func_001F57D0(0x2329, 1);
    func_001C59D8(sound);
    uintptr_t music = (uint32_t)SHELL_WORD(0x4B2A90 + 0x1310);
    if (music) *(float *)(music + 0xB8) = 1.0f;
    func_0023D8D8(0, shell_distance, shell_aspect);
    SHELL_WORD(0x29DED8) = 15;
    SHELL_WORD(0x29DEEC) = 3;
    port_online_choose_end();
}

/* Online everyone has every rider, bike and track, as retail's unlock-all code gives them
   (func_001F0780: D_004325A0.x2180 the 16 riders, x2188 the 18 bikes, x2114 the tracks but 21
   and 23, D_004225A0.x12190 23 more flags), and the rider screen's pros too: their statues come
   alive from the save's rider bits D_004225A0.x12110 (0-13), which func_001BB670 copies into the
   screen's table (D_004B25A0.x3C). The player's own progress comes back when they leave online
   play, and the memory card keeps it meanwhile: its writes are held (mc.c). */
enum { UNLOCK_PROS = 0x4346B0, UNLOCK_TRACKS = 0x4346B4, UNLOCK_RIDERS = 0x434720, UNLOCK_BIKES = 0x434728,
       UNLOCK_FLAGS = 0x434730 };
void func_001BB670(void *game);
static int unlocked;
static int32_t kept_pros, kept_tracks, kept_riders, kept_bikes;
static uint8_t kept_flags[23];

static void unlock_all(void)
{
    if (unlocked) return;
    uint8_t *flags = (uint8_t *)(uintptr_t)(0x08000000u + UNLOCK_FLAGS);
    kept_pros = SHELL_WORD(UNLOCK_PROS), kept_tracks = SHELL_WORD(UNLOCK_TRACKS);
    kept_riders = SHELL_WORD(UNLOCK_RIDERS), kept_bikes = SHELL_WORD(UNLOCK_BIKES);
    memcpy(kept_flags, flags, sizeof kept_flags);
    SHELL_WORD(UNLOCK_PROS) |= 0x3FFF;
    func_001BB670((void *)(uintptr_t)(0x08000000u + 0x4B25A0));
    SHELL_WORD(UNLOCK_TRACKS) |= 0x035FFFFF;
    SHELL_WORD(UNLOCK_RIDERS) |= 0xFFFF;
    SHELL_WORD(UNLOCK_BIKES) |= 0x3FFFF;
    memset(flags, 1, sizeof kept_flags);
    hle_mc_hold = unlocked = 1;
    hle_log("online menu: everything unlocked while online\n");
}

/* Every bike online (finding 128): the shop's BIKES page lists the save's bike garage (frame,
   fork, wheels of each bike; profile +0x11C68, a new save has 3), not the unlock bits. While
   online the garage's readers answer from PC SETTINGS Unlock All's list (patches_unlock.py,
   port_unlock_list): the save's bikes, then the 41 preset bikes the retail unlock-all code adds
   (D_0026DEA0: the entry and bonus bikes and every pro's), and online the single race's stock bike
   17 4 0 too (issue 18's default). Nothing is written to the save. */
int port_online_bikes(void) { return unlocked; }

/* Back to offline play: the player's own unlocks again. */
void port_online_relock(void)
{
    if (!unlocked) return;
    SHELL_WORD(UNLOCK_PROS) = kept_pros, SHELL_WORD(UNLOCK_TRACKS) = kept_tracks;
    SHELL_WORD(UNLOCK_RIDERS) = kept_riders, SHELL_WORD(UNLOCK_BIKES) = kept_bikes;
    func_001BB670((void *)(uintptr_t)(0x08000000u + 0x4B25A0));
    memcpy((uint8_t *)(uintptr_t)(0x08000000u + UNLOCK_FLAGS), kept_flags, sizeof kept_flags);
    hle_mc_hold = unlocked = 0;
    hle_log("online menu: the player's own unlocks again\n");
}

static void reset_selection(void)
{
    selected = selection_requested = 0;
    chosen_spot = 0;
    room_settings_sent = room_ready_sent = 0;
}

static char text[MAX_ROWS][ROW_TEXT], status[128];
static int outdated;  /* the server refused this release: HOME offers Update Now */
/* 44 characters: the footer fits about 45 at 640 wide (0.1.0 shows the server's copy there too) */
static const char outdated_text[] = "A new version is out. Update to play online.";

static const char *server(void)
{
    const char *s = getenv("DHD_NET_SERVER");
    return s && *s ? s : NULL;
}

/* ---- the disc image's check ---- */

static volatile int iso_state;  /* 0 not started, 1 hashing, 2 done, 3 failed */

/* A thread's result for the screen: a sequentially consistent store (the Interlocked exchanges were these). */
static void set_state(volatile int *state, int value) { __atomic_store_n(state, value, __ATOMIC_SEQ_CST); }
static char iso_hash[65];  /* the game files hash, the one the server gets */

/* The check's result in the log, once: a disc image that boots (its SCUS_971.77 is right,
   game_data.c) can still be a dump whose other files differ, and online needs the USA disc's.
   note: what differs (the first file that isn't the USA disc's), or "". */
static void log_iso(const char *how, const char *note)
{
    if (!strcmp(iso_hash, us_files_sha256))
        hle_log("disc check: the disc image has the USA disc's game files (files SHA-256 %s, %s)\n", iso_hash, how);
    else
        hle_log("disc check: the disc image has game files different from the USA disc's (redump 30393)%s%s: "
                "files SHA-256 %s, online needs %s (%s). A bad or modified dump: it can't be used online, and "
                "the game may stop where it reads a changed file\n",
                *note ? ": " : "", note, iso_hash, us_files_sha256, how);
}

/* A file of the player's data folder: the launcher's (DHD_DATA_DIR), else the game's own folder
   (on Android the app's files folder, host_exe_path). iso-check.txt, the check's cache (dhd_disc,
   shared with the launcher), and disc-notice.txt (disc_notice.c) live there. */
void disc_data_path(char *out, size_t size, const char *name)
{
    const char *data = host_getenv("DHD_DATA_DIR");  /* the launcher's folder for the player's files */
    if (data) { snprintf(out, size, "%s/%s", data, name); return; }
    char exe[HOST_PATH_MAX];
    host_exe_path(exe, sizeof exe);
    char *slash = strrchr(exe, '\\'), *posix = strrchr(exe, '/');  /* Windows, or the others' */
    if (!slash || (posix && posix > slash)) slash = posix;
    if (slash) slash[1] = 0;
    snprintf(out, size, "%s%s", exe, name);
}

static void iso_check_path(char *out, size_t size) { disc_data_path(out, size, "iso-check.txt"); }

/* 64-bit hosts check on a host thread (finding 082): the blocking part (the cache, else reading
   the image's files) runs on the host, and page_update picks up the answer, so the menu stays
   responsive. */
static void start_iso_check(void)
{
    const char *given = getenv("DHD_NET_ISO_SHA256");  /* a tool's files hash */
    const char *path = getenv("DHD_ISO");
    char cache[HOST_PATH_MAX];
    if (iso_state) return;
    if (given && strlen(given) == 64) {
        snprintf(iso_hash, sizeof iso_hash, "%s", given);
        iso_state = 2;
        log_iso("given by DHD_NET_ISO_SHA256", "");
        return;
    }
    if (!path || !host_path_exists(path)) {
        hle_log("disc check: the disc image can't be found for the online check\n");
        set_state(&iso_state, 3);
        return;
    }
    iso_check_path(cache, sizeof cache);
    hle_log("disc check: checking the disc image's game files\n");
    set_state(&iso_state, 1);
    if (!host_service_disc_check_start(GADDR(path), GADDR(cache))) {
        hle_log("disc check: the disc image's check couldn't start\n");
        set_state(&iso_state, 3);
    }
}

static char iso_note[256];  /* the check's note: what differs from the USA disc, or why no answer */

/* One poll of the checking host job: the hash lands in iso_hash. */
static void poll_iso(void)
{
    static uint8_t digest[32];
    int r;
    if (iso_state != 1) return;
    r = host_service_disc_check_poll(GADDR(digest), GADDR(iso_note), sizeof iso_note);
    if (!r) return;
    if (r == -1 || r == -3) {  /* -3: stopped (the game is ending) */
        hle_log("disc check: reading the disc image failed during its check: %s\n", iso_note);
        set_state(&iso_state, 3);
        return;
    }
    if (r == -2) {
        /* no file system to hash, or a cut dump: no hash, and not the USA disc's files */
        memset(iso_hash, '0', 64), iso_hash[64] = 0;
        set_state(&iso_state, 2);
        hle_log("disc check: the disc image can't have the USA disc's game files: %s. A bad or modified dump: "
                "it can't be used online\n", iso_note);
        return;
    }
    for (int i = 0; i < 32; i++) snprintf(iso_hash + 2 * i, 3, "%02x", digest[i]);
    set_state(&iso_state, 2);
    log_iso(r == 2 ? "from iso-check.txt" : "checked now", iso_note);
}

/* Every start, once, when the main menu first shows (disc_notice.c, from the patched main menu):
   the same check, so every dhd.log says whether the disc image's files are the USA disc's, and
   a changed image gets its notice. A disc image that boots can still be a dump with changed or
   damaged files, and the game then stops where it reads one: issue #78's Bike Shop crash read
   texture records that no file of the USA disc holds (finding 208). The answer comes from
   iso-check.txt when the launcher or an earlier start wrote it for this image; else the host
   hashes the image's files once on a low-priority thread (host_sdl.c disc_run). Never in timed,
   deterministic or traced runs (DHD_BENCH, DHD_FRAME_CLOCK, DHD_STATE_TRACE), nor when a tool
   gives the hash; a host without the check (the browser) refuses the start, quietly. The Online
   menu takes the answer (start_iso_check does nothing once a check ran). */
void disc_check_start(void)
{
    static int started;
    if (started) return;
    started = 1;
    const char *given = getenv("DHD_NET_ISO_SHA256"), *path = getenv("DHD_ISO");
    char cache[HOST_PATH_MAX];
    if (iso_state || host_getenv("DHD_BENCH") || host_getenv("DHD_FRAME_CLOCK") || host_getenv("DHD_STATE_TRACE") ||
        (given && strlen(given) == 64) || !path || !host_path_exists(path))
        return;
    iso_check_path(cache, sizeof cache);
    if (host_service_disc_check_start(GADDR(path), GADDR(cache))) set_state(&iso_state, 1);
}

/* Each field (kernel.c): a running check's answer, whenever it comes, on any screen. */
void disc_check_field(unsigned long long field)
{
    (void)field;
    if (iso_state == 1) poll_iso();
}

/* The check's answer: the image's game files hash (64 hex digits; zeros when its files can't be
   the USA disc's), with what differs in *why; NULL while there is none. usa: the USA disc's. */
const char *disc_check_answer(const char **why, const char **usa)
{
    if (why) *why = iso_note;
    if (usa) *usa = us_files_sha256;
    return iso_state == 2 ? iso_hash : NULL;
}

/* ---- the public list ---- */

static volatile int list_state;  /* 0 none, 1 asking, 2 done */
static NetLobbyInfo list[LIST_MAX];
static int list_count, list_shown;  /* list_shown: the rows have the answer */

/* One poll of the fetching host job: the rows land in list, as before. */
static void poll_list(void)
{
    int r;
    if (list_state != 1) return;
    r = host_service_net_list_lobbies_poll(GADDR(list), LIST_MAX);
    if (r == -2) return;
    list_count = r;
    if (r < 0) hle_log("online menu: the public list: the server %s didn't answer\n", server());
    set_state(&list_state, 2);
}

/* ---- the player ---- */

static char name[NAME_MAX + 1], version[64];
static int rider = 0;  /* index into riders[] */

static void load_player(void)
{
    const char *n = getenv("DHD_NET_NAME"), *r = getenv("DHD_NET_RIDER");
    if (!name[0]) {
        int k = 0;
        for (const char *c = n ? n : ""; *c && k < NAME_MAX; c++) {
            char up = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
            if (strchr(name_letters, up)) name[k++] = up;
        }
        name[k] = 0;
        if (!k) snprintf(name, sizeof name, "%s", SETTING_NAME_UNNAMED);
        for (int i = 0; r && *r && i < COUNT(riders); i++)
            if (riders[i].index == atoi(r)) rider = i;
    }
    if (!version[0]) hle_online_version(version, sizeof version);
}

/* Saved to dhd.ini and set for this run, as the online launcher does. */
static void save(const char *key, const char *env, const char *value)
{
    host_service_host_setting_save(GADDR(key), GADDR(value));
    host_putenv_s(env, value);
    hle_env_changed();
}

/* Where the rooms this player creates run (dhd.ini room_region, DHD_NET_ROOM_REGION; finding 181):
   the settings table's choices, Auto first. The server makes the room with that location hint. */
static const SettingInfo *region_setting(void)
{
    for (int i = 0; i < COUNT(setting_table); i++)
        if (!strcmp(setting_table[i].key, "room_region")) return &setting_table[i];
    return NULL;
}

static int region_index(void)
{
    const SettingInfo *s = region_setting();
    const char *v = getenv("DHD_NET_ROOM_REGION");
    for (int i = 0; s && v && i < s->count; i++)
        if (!strcmp(s->choices[i].value, v)) return i;
    return 0;  /* unset or unknown: Auto */
}

static int home_row_of(int kind);

static void region_step(MenuPage *p, int by)
{
    const SettingInfo *s = region_setting();
    if (!s) return;
    int i = (region_index() + by + s->count) % s->count;
    save("room_region", "DHD_NET_ROOM_REGION", s->choices[i].value);
    menu_page_value_set(p, home_row_of(HOME_REGION), "Room Region:", s->choices[i].label, MENU_VALUE_ARROWS);
    menu_page_sound(by > 0);
    hle_log("online menu: room region %s\n", s->choices[i].value);
}

/* What stops this PC from joining a race, or NULL. */
static const char *not_ready(void)
{
    const char *render = getenv("DHD_RENDER");
    if (!server()) return "No server: set server = ... in dhd.ini";
    if (!render || strcmp(render, "native")) return "Online races need the native renderer";
    if (iso_state == 1) return "Checking the disc image, a moment...";
    if (iso_state != 2) return "Can't read the disc image: choose it again";
    if (strcmp(iso_hash, us_files_sha256)) return "This disc's game files aren't the USA disc's";
    return NULL;
}

/* Why the disc image keeps this PC offline, as lines under ONLINE's rows (the footer holds one
   short line), NULL-ended; NULL when the image is fine. Each line fits the 640-wide screen. */
static const char *const *disc_note(void)
{
    static const char *const hashing[] = {"", "Checking the disc image for online...", NULL};
    static const char *const unreadable[] = {"", "The disc image couldn't be read for", "the online check. Choose it again",
                                             "in the launcher.", NULL};
    static const char *const different[] = {"", "The disc image's game files are", "different from the USA disc's (a bad",
                                            "or modified dump). Make a new dump or", "choose another image in the launcher.",
                                            NULL};
    if (iso_state == 1) return hashing;
    if (iso_state == 3) return unreadable;
    if (iso_state == 2 && strcmp(iso_hash, us_files_sha256)) return different;
    return NULL;
}

/* ---- the screens ---- */

static int entry_pick;           /* the letter on offer */
static char entry[16];
static int home_row;  /* HOME's row last chosen: the cursor goes back to it */

static int home_kind[HOME_KINDS], home_rows;
/* A P2P room's members reach its hub's game directly, so each sees the hub's network address and
   the hub every member's (findings 180, 186). Said on HOME's row and in the room when it opens. */
static const char p2p_notice[] = "Players in this room can see your network address.";
static const char p2p_row_notice[] = "Players in it can see your network address.";  /* the footer's 45 */
static double p2p_notice_until;
static int p2p_noticed;

static void home_add(MenuPage *p, int kind, const char *label)
{
    home_kind[home_rows++] = kind;
    menu_page_row(p, label, 1);
}

static int home_row_of(int kind)
{
    for (int i = 0; i < home_rows; i++)
        if (home_kind[i] == kind) return i;
    return 0;
}
static const char *const *disc_shown;  /* the disc_note HOME's rows have */

static void room_open(void);

static void go(int to)
{
    host_service_text_input_end();
    screen = to;
    if (to == NAME || to == CODE) host_service_text_input_begin();
    page.mouse = NULL, page.draw = NULL, page.title_y = 0;
    if (to == ROOM) room_open();
    status[0] = 0;
    menu_page_rebuild_at(&page, to == HOME ? home_row : 0);
}

static void start_list(void)
{
    if (list_state == 1) return;
    list_state = 1;
    host_service_net_init(GADDR(server()), GADDR(name), GADDR(version), GADDR(iso_hash));
    /* The fetch itself runs on a host thread; page_update picks up the rows. */
    if (!host_service_net_list_lobbies_start(LIST_MAX)) list_state = 2, list_count = -1;
}

/* A public race's row: its name, players, track and what it does now (racing, the time to its
   next race...), written again every frame as its times count (the game keeps the row's text by
   pointer). Racing rooms take players too: they race the next race. */
static int list_joinable(const NetLobbyInfo *l) { return !strcmp(l->version, version) && !l->full && l->players < l->max_players; }

static void list_row(int i)
{
    const NetLobbyInfo *l = &list[i];
    const char *track = "?";
    char now[24];
    double t = host_service_net_server_ms();
    for (int k = 0; k < TRACK_COUNT; k++)
        if (track_at(k) == l->track) track = track_name(track_at(k));
    if (strcmp(l->version, version)) snprintf(now, sizeof now, "OTHER VERSION");
    else if (l->full || l->players >= l->max_players) snprintf(now, sizeof now, "FULL");
    else net_lobby_now(l, t, now, sizeof now);  /* counted only on a known server clock (finding 135) */
    /* where the room runs (EU WEST, NA EAST...; finding 181; an older server's EU, NA...; finding
       135): a far room shows before joining it */
    const char *where = l->area[0] ? l->area : l->region;
    snprintf(text[i], ROW_TEXT, "%.14s  %s%s%d/%d  %s  %s", l->room_name[0] ? l->room_name : l->host_name,
             where, where[0] ? "  " : "", l->players, l->max_players, track, now);
}

/* create: 0 joins the room of `code`, else makes a room: HOME_PUBLIC, HOME_PRIVATE or HOME_P2P. */
static void enter_room(int create, const char *code)
{
    const char *why = not_ready();
    if (why) {
        snprintf(status, sizeof status, "%s", why);
        hle_log("online menu: can't %s: %s\n", create ? "create a room" : "join", why);
        return;
    }
    host_service_net_init(GADDR(server()), GADDR(name), GADDR(version), GADDR(iso_hash));
    if (create) host_service_net_create_room(create == HOME_P2P);
    else if (host_service_net_join_room(GADDR(code)) < 0) {
        snprintf(status, sizeof status, "That code can't be one");
        return;
    }
    reset_selection();
    choosing = 0;
    public_room = create == HOME_PUBLIC;
    p2p_noticed = 0;
    if (create) room_options &= ~ROOM_CHEATS;  /* a new room starts with cheats off */
    room_settings_sent = rotation_sent = !create;  /* a guest who becomes host keeps the room as it is */
    room_ready_sent = 0;
    connecting = 1;
    snprintf(status, sizeof status, "Connecting to the server...");
    hle_log("online menu: %s %s\n", create ? "creating a room" : "joining",
            create == HOME_P2P ? "(P2P)" : create ? (public_room ? "(public)" : "(private)") : net_log_code(code).text);
}

static void page_build(MenuPage *p)
{
    switch (screen) {
    case HOME:
        p->title = "ONLINE";
        home_rows = 0;
        snprintf(text[0], ROW_TEXT, "Name:  %s", name);
        home_add(p, HOME_NAME, text[0]);
        if (host_service_net_offered() & NET_OFFER_SFU) {
            home_add(p, HOME_QUICK, "Quick Race");
            home_add(p, HOME_LIST, "Public Races");
            home_add(p, HOME_PUBLIC, "Create A Public Race");
            home_add(p, HOME_PRIVATE, "Create A Private Race");
            /* where a new room runs (finding 181), a P2P one too: its rules and fallback run there (186) */
            const SettingInfo *s = region_setting();
            home_kind[home_rows++] = HOME_REGION;
            menu_page_value(p, "Room Region:", s ? s->choices[region_index()].label : "Auto", MENU_VALUE_ARROWS);
        }
        if (host_service_net_offered() & NET_OFFER_P2P) home_add(p, HOME_P2P, "Create A P2P Race");
        home_add(p, HOME_JOIN, "Join With A Code");
        if (outdated) home_add(p, HOME_UPDATE, "Update Now");
        disc_shown = disc_note();
        for (const char *const *line = disc_shown; line && *line; line++) menu_page_text(p, *line);
        break;
    case LIST:
        p->title = "PUBLIC RACES";
        list_shown = list_state == 2;
        if (list_state != 2) menu_page_row(p, "Looking for races...", 0);
        else if (list_count < 0) menu_page_row(p, "The server didn't answer", 0);
        else if (list_count == 0) menu_page_row(p, "No public races right now", 0);
        for (int i = 0; list_state == 2 && i < list_count; i++) {
            list_row(i);
            menu_page_row(p, text[i], list_joinable(&list[i]));
        }
        menu_page_row(p, "Look Again", list_state == 2);
        break;
    case QUICK:  /* the busiest public race with room, joined; none: asked before one is made */
        p->title = "QUICK RACE";
        list_shown = list_state == 2;
        if (list_state != 2) menu_page_row(p, "Looking for a race...", 0);
        else {
            menu_page_text(p, list_count < 0 ? "The server didn't answer" : "No public race to join right now");
            menu_page_row(p, "Create A Public Race", list_count >= 0);
            menu_page_row(p, "Back", 1);
        }
        break;
    case NAME:
    case CODE:
        p->title = screen == NAME ? "YOUR NAME" : "JOIN WITH A CODE";
        menu_page_row(p, text[0], 1);  /* written every frame */
        menu_page_row(p, screen == NAME ? "Done" : "Join", 1);
        break;
    case ROOM:  /* no rows: it lays itself out (room_draw) */
        p->title = "RACE ROOM";
        break;
    }
}

static void entry_text(void)
{
    const char *letters = screen == NAME ? name_letters : code_letters;
    int max = screen == NAME ? NAME_MAX : P2P_CODE_LENGTH;
    char shown = letters[entry_pick] == ' ' ? '-' : letters[entry_pick];
    if ((int)strlen(entry) < max)
        snprintf(text[0], ROW_TEXT, "%s  %s[%c]", screen == NAME ? "Name:" : "Code:", entry, shown);
    else
        snprintf(text[0], ROW_TEXT, "%s  %s", screen == NAME ? "Name:" : "Code:", entry);
}

/* The server refused or lost the room: its reason for the footer. */
static void refused(const NetLobby *l)
{
    int too_old = !strcmp(l->error_code, "client_outdated");
    if (too_old) outdated = 1;
    snprintf(status, sizeof status, "%s",
             too_old ? outdated_text : l->error[0] ? l->error : "The connection to the server failed");
    hle_log("online menu: refused: %s (%s)\n", l->error, l->error_code);
    host_service_net_leave();
    reset_selection();
    connecting = 0;
    if (too_old) home_row = home_row_of(HOME_UPDATE);
}

/* In the room: the host's settings, then this member's first (not ready) state, once each. */
static void room_sync(const NetLobby *l)
{
    if (l->is_host && !room_settings_sent) {
        char room_name[40];
        snprintf(room_name, sizeof room_name, "%s", name);
        /* tests (DHD_NET_SCRIPTED with DHD_NET_TEST_TRACK=<game track index>): the room starts there */
        const char *test_track = getenv("DHD_NET_SCRIPTED") ? getenv("DHD_NET_TEST_TRACK") : NULL;
        for (int k = 0; test_track && k < TRACK_COUNT; k++)
            if (track_at(k) == atoi(test_track)) room_track = k;
        /* tests (DHD_NET_SCRIPTED with DHD_NET_NO_AI=1): the room's computer riders off, as DHD_NET_CREATE's */
        if (getenv("DHD_NET_SCRIPTED") && getenv("DHD_NET_NO_AI")) room_options |= ROOM_NO_BOTS;
        NetRoomSettings s = {track_at(room_track), room_options, public_room, 10, -1, room_name};
        host_service_net_set_room(GADDR(&s));
        room_settings_sent = 1;
    }
    if (l->is_host && !rotation_sent) {
        /* the tracks in the list's order: the room's Then rule (next / random) walks them */
        static unsigned char ids[TRACK_COUNT];
        for (int k = 0; k < TRACK_COUNT; k++)
            ids[k] = (unsigned char)(track_at(k) | ((10 - track_limit(track_at(k))) / 2 << 5) | (arena(track_at(k)) ? 0x80 : 0));
        host_service_net_rotation(GADDR(ids), TRACK_COUNT);
        rotation_sent = 1;
        hle_log("online menu: the room's track list: %d tracks, MOSH PIT pick-only\n", TRACK_COUNT);
        /* tests (DHD_NET_SCRIPTED with DHD_NET_TEST_THEN=same|next|random): the host's Then rule */
        const char *test_then = getenv("DHD_NET_SCRIPTED") ? getenv("DHD_NET_TEST_THEN") : NULL;
        if (test_then) {
            int then = !strcmp(test_then, "same") ? 0 : !strcmp(test_then, "random") ? 2 : 1;
            host_service_net_rules(l->finish_window, l->next_race, then);
            hle_log("online menu: room rules for the test: then %s\n", test_then);
        }
    }
    if (!room_ready_sent) {
        hle_log("online menu: room %s%s, version %s\n", net_log_code(l->code).text, l->is_host ? " (host)" : "", version);
        host_service_net_set_ready(0);
        room_ready_sent = 1;
    }
}

/* Create or Join was chosen: once the server has this player in the room, everything is
   unlocked and the page closes for the game's rider screen (port_online_menu
   returns 2); a refusal stays here with its reason. Triangle gives up (page_back). */
static void connect_poll(MenuPage *p)
{
    static NetLobby l;
    host_service_net_get_lobby(GADDR(&l));
    if (l.state == NET_FAILED || l.state == NET_IDLE) {
        refused(&l);
        if (outdated && screen == HOME) menu_page_rebuild_at(p, home_row);
        return;
    }
    if (l.state != NET_LOBBY && l.state != NET_RACING) return;
    room_sync(&l);
    for (int k = 0; k < TRACK_COUNT; k++)
        if (track_at(k) == l.track) room_track = k;
    room_options = l.laps;
    connecting = 0;
    status[0] = 0;
    unlock_all();
    choosing = 1;
    race = 2;
    host_service_net_pick(-1, -1, 0);  /* "Choosing..." in the room meanwhile */
    hle_log("online menu: in room %s: choosing the rider\n", net_log_code(l.code).text);
    menu_page_close(p);
}

static void page_accept(MenuPage *p, int row);
static void page_step(MenuPage *p, int row, int by);

/* Over the rider screen the room closes (port_online_shell takes the rider screen's Back to
   the title and opens ONLINE there); over the title it is ONLINE's own page. */
static void leave_room(MenuPage *p)
{
    hle_log("online menu: leaving the room\n");
    room_closed = 1;
    host_service_net_leave();
    reset_selection();
    if (in_shell) {
        race = 0;
        menu_page_close(p);
    } else
        go(HOME);
}

/* ---- the race room (finding 115) ----
   Over the rider screen, laid out as the mock-up the players chose (layout B): the host's
   settings and the join code on the left, a card per player on the right, the actions in a bar
   below, what happens now under it, the buttons in the footer. The page has no rows: it draws
   itself (menu_draw_*) and reads the pads itself, so the cursor moves in two places, the
   settings (host) and the bar, as they are laid out. Colours mean one thing each: green ready,
   amber not ready yet, red a problem.

   The server owns what everyone must agree on (src/room.ts): a changed track or option
   unreadies everyone, a member on the rider screen shows as choosing, and once everyone is
   ready its countdown starts the race by itself (countdown_ms); Start starts it at once. Two or
   more players may ride the same rider (issue #17). */

/* The fields in the order the cursor meets them: the left panel's top to bottom (the track, the
   rules, computer riders, cheats, the room's kind), then the buttons. RF_READY is the main button
   (READY, or WATCH THE RACE), RF_START the third (the host's START NOW, or READY NEXT beside WATCH):
   what they do this frame is bar_now's (finding 271). */
enum { RF_TRACK, RF_RULES, RF_BOTS, RF_CHEATS, RF_PUBLIC, RF_RIDER, RF_READY, RF_START, RF_LEAVE, RF_COUNT };
enum { RF_LAST_SETTING = RF_PUBLIC };
enum { RULE_WINDOW, RULE_NEXT, RULE_THEN, RULE_PLAYERS, RULES };
/* The picker fills its columns top to bottom, so the list reads down the left column, then the
   right (finding 190): Up / Down step through the list, Left / Right go to the other column. */
enum { TOASTS = 3, TRACK_COLUMNS = 2, TRACK_ROWS = (TRACK_COUNT + TRACK_COLUMNS - 1) / TRACK_COLUMNS };
/* The layout, in the popup view's 640 x 224 (menu_draw_size), as the owner's pictures have it
   (finding 271; their units of 1/100 of the 4:3 page are 6.4 across and about 3 down here):
   the header (a small line, the game's title, the countdown bar), the information panel and the
   room code on the left, the list on the right, the buttons, the sentence, the game's legend. */
enum { PAGE_X = 45, PAGE_W = 550, PANEL_W = 147, LIST_X = 205, LIST_W = 390,
       SMALL_Y = 2, TITLE_Y = 13, COUNT_Y = 31,
       INFO_Y = 37, INFO_H = 100, CODE_Y = 140, CODE_H = 27,
       HEAD_Y = 38, ROW_Y = 48, ROW_STEP = 12, ROW_H = 11,
       BTN_Y = 171, BTN_H = 18, MAIN_X = 205, MAIN_W = 192, THIRD_X = 407, THIRD_W = 105, LEAVE_X = 522, LEAVE_W = 73,
       SENTENCE_Y = 192 };
/* Text colours (the font's 0-128) and rectangle colours (0-255) */
static const int green[3] = {0x30, 0x80, 0x28}, amber[3] = {0x80, 0x60, 0x10}, red[3] = {0x80, 0x30, 0x30},
                 blue[3] = {0x38, 0x60, 0x98}, lilac[3] = {0x70, 0x68, 0x98};
static const int t_white[3] = {0x76, 0x75, 0x7A}, t_dim[3] = {0x45, 0x48, 0x60}, t_amber[3] = {0x70, 0x58, 0x17},
                 t_green[3] = {0x2C, 0x68, 0x2E}, t_blue[3] = {0x37, 0x51, 0x75},
                 t_dark[3] = {0x06, 0x06, 0x08}, t_red[3] = {0x78, 0x2C, 0x2C};
static const int r_green[3] = {0x59, 0xD0, 0x5C}, r_chip[3] = {0x2E, 0x9A, 0x38}, r_amber[3] = {0xE0, 0xB1, 0x2E},
                 r_grey[3] = {0x3A, 0x3C, 0x5C}, r_yellow[3] = {0xF4, 0xE4, 0x00}, r_lilac[3] = {0xA3, 0xA5, 0xF2},
                 r_blue[3] = {0x6F, 0xA3, 0xEA}, r_black[3] = {0, 0, 0}, r_wait[3] = {0x7D, 0x7F, 0xA6},
                 r_dark[3] = {0x0C, 0x0C, 0x16}, r_white[3] = {0xEC, 0xEB, 0xF5};

typedef struct { short x, y, w, h; } Hit;
static NetLobby room;                       /* this frame's snapshot */
static int rf = RF_READY, rf_bar = RF_READY;  /* the cursor; the bar's place, kept meanwhile */
static int track_list, track_cursor;          /* all the tracks, open over the room */
static int leave_ask, leave_choice;           /* "Leave the room?": 0 Stay, 1 Leave */
static int rules_open, rules_cursor;          /* ROOM RULES, open over the room */
static Hit hits[RF_COUNT], code_hit, track_hits[TRACK_COUNT], track_back, leave_hits[2], rule_hits[RULES], rules_back;
static int room_buttons, repeat_buttons, repeat_frames;
static double copied_at;
static int copied_ok;
static struct { char text[48]; const int *colour; double until; } toast[TOASTS];
static int seen_id[NET_MAX_SLOTS], seen_ready[NET_MAX_SLOTS], seen_count = -1;
static char seen_name[NET_MAX_SLOTS][32];
static double ready_heard[NET_MAX_SLOTS];     /* per card: the last ready sound (no spam) */
static int countdown_shown = -1;
static char seen_race[NET_MAX_SLOTS][12];     /* per card: its race state last seen (news) */
static int seen_host = -1;
static int seen_track = -1;                   /* the room's track last seen (TRACK CHANGED) */
static int raced_track = -1;                  /* the track this PC's race was on, and its room: */
static char raced_code[8];                    /* back from it, a change made meanwhile is news */
static char seen_event[32];                   /* the server's last notice seen (start_cancelled) */
static char title_text[32], footer_text[96];

/* The buttons held on both pads, the left stick at its edge as the d-pad (menu_pads_held, as the
   game's menus: issue #103). -1 for a field whose word reads all ones as a press starts: that
   field changes nothing. */
static int pads_held(void) { return menu_pads_held(); }

/* ---- what the room is now ---- */

/* The room keeps going across races (finding 118, the server's session rooms): a race runs while
   the others wait here (phase "racing"), a timer counts to the next one ("intermission"), the
   5 s countdown runs for the ready ones ("starting"), or nothing runs ("open"). Ready means "put
   me in the next race". */
typedef struct {
    int in;            /* in the room (not reconnecting) */
    int self;          /* this PC's member, -1 none */
    int own_ready, own_choosing;
    int ready, can_start, host;
    int racing, starting, intermission;  /* the phase */
    int riding, finished;  /* in the running race: still riding, finished */
    const char *own_race;  /* this PC's state in the running race: "" not in it */
    int can_watch;         /* outside the running race, which this PC may watch (finding 269) */
} RoomNow;

static RoomNow room_now(void)
{
    RoomNow r = {0};
    const NetLobby *l = &room;
    r.in = l->state == NET_LOBBY || l->state == NET_RACING;
    r.self = -1;
    r.host = r.in && l->is_host;
    r.racing = !strcmp(l->phase, "racing");
    r.starting = l->countdown_ms >= 0;
    r.intermission = !strcmp(l->phase, "intermission");
    r.own_race = "";
    int own_in_race = 0;
    for (int i = 0; i < l->nmembers; i++) {
        const NetMember *m = &l->member[i];
        r.ready += m->ready;
        if (r.racing && !strcmp(m->race_state, "racing")) r.riding++;
        if (r.racing && !strcmp(m->race_state, "finished")) r.finished++;
        if (m->id != l->member_id) continue;
        r.self = i, r.own_ready = m->ready, r.own_choosing = m->choosing;
        if (r.racing) r.own_race = m->race_state;
        own_in_race = m->in_race;
    }
    /* WATCH (finding 269): a race runs that this PC is not in (joined during it, sat it out, left it),
       on a server that takes it, in an SFU room, not the arena's scored round (finding 204) */
    r.can_watch = r.in && r.racing && r.self >= 0 && !own_in_race && l->server_watch && !l->is_p2p && !arena(l->track) &&
                  l->state == NET_LOBBY;
    /* the host starts the next race with whoever is ready (the others wait here) */
    r.can_start = r.host && !r.racing && !r.starting && r.ready > 0;
    return r;
}

/* What the main and the third button do now (finding 271): READY, and the host's START NOW beside
   it (shown with why it can't while it can't: today's rules, finding 118); while a race runs that
   this PC may watch, WATCH THE RACE with READY NEXT beside it. A guest has no third button: the
   header says the host starts. */
enum { ACT_NONE, ACT_READY, ACT_START, ACT_WATCH };
typedef struct { int main, third; } BarNow;

static BarNow bar_now(const RoomNow *r)
{
    BarNow b = {ACT_READY, ACT_NONE};
    if (r->can_watch) b.main = ACT_WATCH, b.third = ACT_READY;
    else if (r->host) b.third = ACT_START;
    return b;
}

static int action_possible(int act, const RoomNow *r)
{
    return act == ACT_READY ? r->in && strcmp(r->own_race, "racing") != 0 : act == ACT_START ? r->can_start
         : act == ACT_WATCH ? r->can_watch : 0;
}

static int focusable(int f, const RoomNow *r)
{
    BarNow b = bar_now(r);
    switch (f) {
    case RF_TRACK: case RF_BOTS: case RF_CHEATS: return r->host && r->in;
    case RF_PUBLIC: return r->host && r->in && !room.is_p2p;  /* a P2P room is never listed */
    case RF_RULES: return r->in;  /* everyone reads them; the host changes them */
    case RF_RIDER: return r->in;
    case RF_READY: return action_possible(b.main, r);
    case RF_START: return action_possible(b.third, r);
    default: return 1;  /* Leave */
    }
}

/* The room's rules, as the server has them (NetLobby: finish_window s, 0 no limit; next_race 0
   auto, 1 host; then 0 same, 1 next, 2 random). */
static const int windows[] = {30, 60, 120, 0};
static const char *window_name(int s) { return s == 30 ? "30 S" : s == 60 ? "60 S" : s == 120 ? "2 MIN" : "NO LIMIT"; }
static const char *next_name(int n) { return n ? "HOST STARTS" : "AUTO 30 S"; }
static const char *then_name(int t) { return t == 0 ? "SAME TRACK" : t == 2 ? "RANDOM TRACK" : "NEXT TRACK"; }

static void change_rule(int rule, int by)
{
    if (rule == RULE_PLAYERS) {
        int size = room.max_players + by;
        if (size < 2) size = 10;
        if (size > 10) size = 2;
        NetRoomSettings s = {-1, -1, -1, size, -1, NULL};
        host_service_net_set_room(GADDR(&s));
        menu_page_sound(by > 0);
        return;
    }
    const NetLobby *l = &room;
    int window = l->finish_window, next = l->next_race, then = l->then;
    if (rule == RULE_WINDOW) {
        int k = 0;
        while (k < 3 && windows[k] != window) k++;
        window = windows[(k + by + 4) % 4];
    } else if (rule == RULE_NEXT)
        next ^= 1;
    else if (rule == RULE_THEN)
        then = (then + by + 3) % 3;
    else
        return;
    host_service_net_rules(window, next, then);
    hle_log("online menu: room rules: finish window %d s, next race %s, then %s\n", window, next_name(next), then_name(then));
    menu_page_sound(by > 0);
}

/* A place as the notes say it ("3RD"): room_view.h's, in a buffer of its own */
static const char *ordinal(int place)
{
    static char text[8];
    return room_ordinal(place, text, sizeof text);
}

/* The seconds from now to a server time (0 when absent or past). */
static int seconds_to(double at)
{
    double ms = at > 0 ? at - host_service_net_server_ms() : 0;
    return ms > 0 ? (int)((ms + 999) / 1000) : 0;
}

/* ---- what the buttons do ---- */

static int sent_track = -1;  /* the track this PC chose last: its own change isn't news */
static double sent_at;

static void send_settings(int track, int options, int is_public)
{
    NetRoomSettings s = {track, options, is_public, -1, -1, NULL};
    if (track >= 0) sent_track = track, sent_at = host_seconds();
    host_service_net_set_room(GADDR(&s));
}

static const char *input_by;  /* defined with the buttons below */

static void change_setting(int f, int by)
{
    static const char *const what[] = {"track", "rules", "computer riders", "cheats", "public"};
    hle_log("online menu: the room's %s changed by %s\n", what[f], input_by);
    if (f == RF_TRACK) {
        do room_track = (room_track + by + TRACK_COUNT) % TRACK_COUNT; while (arena(track_at(room_track)));
        send_settings(track_at(room_track), -1, -1);
    } else if (f == RF_BOTS)
        send_settings(-1, room_options = room.laps ^ ROOM_NO_BOTS, -1);
    else if (f == RF_CHEATS) {
        send_settings(-1, room_options = room.laps ^ ROOM_CHEATS, -1);
        hle_log("online menu: the room's cheats %s\n", room_options & ROOM_CHEATS ? "on" : "off");
    } else if (f == RF_PUBLIC)
        send_settings(-1, -1, public_room = !room.is_public);
    menu_page_sound(by > 0);
}

static void choose_rider(void)
{
    host_service_net_set_ready(0);
    host_service_net_pick(-1, -1, 0);  /* the others see "Choosing..." meanwhile */
    selection_requested = 1;
    menu_play("S_SELECT");
}

/* READY as the room hears it (finding 218): who pressed it, and whether the room's next lobbies say
   so. A READY the room doesn't show within READY_WAIT seconds is said on the page, never a button
   that silently does nothing (a phone's READY was lost that way, with no word from the server). */
enum { READY_WAIT = 4 };
static const char *input_by = "pad";   /* "tap" while room_mouse acts */
static int ready_wanted = -1, seen_own_ready = -1;
static double ready_asked_at;
static char seen_error[200];
static int error_primed;
static void note_for(const char *text, const int *colour, double seconds);

static void toggle_ready(const RoomNow *r)
{
    if (r->own_ready) {
        host_service_net_set_ready(0);
        menu_play("S_BACK");
    } else if (!selected) {
        choose_rider();
        hle_log("online menu: READY by %s: no rider chosen yet, the rider screen\n", input_by);
        return;
    } else {
        host_service_net_set_ready(1);
        menu_play("S_SELECT");
    }
    ready_wanted = !r->own_ready, ready_asked_at = host_seconds();
    hle_log("online menu: READY %s by %s\n", ready_wanted ? "on" : "off", input_by);
}

/* Each room update: the room's answer to READY, and its errors on the page (they were only logged). */
static void ready_news(const NetLobby *l, const RoomNow *r)
{
    static const int warn[3] = {0x80, 0x30, 0x30};
    if (r->self >= 0 && r->own_ready != seen_own_ready) {
        if (seen_own_ready >= 0) hle_log("online menu: the room has this player %s\n", r->own_ready ? "ready" : "not ready");
        seen_own_ready = r->own_ready;
    }
    if (ready_wanted >= 0 && r->own_ready == ready_wanted)
        ready_wanted = -1;
    else if (ready_wanted >= 0 && host_seconds() - ready_asked_at > READY_WAIT) {
        hle_log("online menu: the room didn't take READY %s in %d s\n", ready_wanted ? "on" : "off", READY_WAIT);
        note_for(ready_wanted ? "READY DIDN'T REACH THE ROOM: TRY AGAIN" : "THE ROOM STILL HAS YOU READY", warn, 6.0);
        ready_wanted = -1;
    }
    if (!error_primed) {   /* an error from before this page is old news */
        snprintf(seen_error, sizeof seen_error, "%s", l->error);
        error_primed = 1;
    } else if (l->error[0] && strcmp(l->error, seen_error)) {
        char text[48];
        snprintf(seen_error, sizeof seen_error, "%s", l->error);
        snprintf(text, sizeof text, "%s", l->error);
        for (char *c = text; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
        note_for(text, warn, 6.0);
    }
}

static void copy_code(void)
{
    copied_ok = room.code[0] && host_service_clipboard_set(GADDR(room.code));
    copied_at = host_seconds();
    hle_log("online menu: the code copied by %s: %s\n", input_by, copied_ok ? "ok" : "failed");
    menu_play(copied_ok ? "S_SELECT" : "S_CANCEL");
}

static void open_track_list(const RoomNow *r)
{
    if (r->own_ready) host_service_net_set_ready(0);  /* nothing counts down behind the list */
    track_list = 1, track_cursor = room_track;
    menu_play("S_SELECT");
}

static void close_track_list(int choose)
{
    track_list = 0;
    if (choose && track_cursor != room_track) {
        room_track = track_cursor;
        send_settings(track_at(room_track), -1, -1);
    }
    menu_play(choose ? "S_SELECT" : "S_BACK");
}

static void open_leave(const RoomNow *r, const char *by)
{
    hle_log("online menu: leave asked (%s)\n", by);
    if (r->own_ready) host_service_net_set_ready(0);
    leave_ask = 1, leave_choice = 0;
    menu_play("S_BACK");
}

static void leave_room(MenuPage *p);

static void act(int what, const RoomNow *r)
{
    if (what == ACT_READY) toggle_ready(r);
    else if (what == ACT_START) {
        hle_log("online menu: START NOW by %s\n", input_by);
        host_service_net_start_race();
        menu_play("S_ENTER_GAME");
    } else if (what == ACT_WATCH) {  /* finding 269: the room's answer (the race's start, "watch") opens the race */
        hle_log("online menu: WATCH by %s\n", input_by);
        host_service_net_watch();
        menu_play("S_ENTER_GAME");
    }
}

static void activate(MenuPage *p, int f, const RoomNow *r)
{
    if (!focusable(f, r)) {
        menu_play("S_CANCEL");
        return;
    }
    BarNow b = bar_now(r);
    switch (f) {
    case RF_TRACK: open_track_list(r); break;
    case RF_BOTS: case RF_CHEATS: case RF_PUBLIC: change_setting(f, 1); break;
    case RF_RULES: rules_open = 1, rules_cursor = 0, menu_play("S_SELECT"); break;
    case RF_RIDER: choose_rider(); break;
    case RF_READY: act(b.main, r); break;
    case RF_START: act(b.third, r); break;
    case RF_LEAVE: open_leave(r, "Leave"); break;
    }
    (void)p;
}

/* The next setting from f in direction d (-1 up, 1 down) that takes the cursor, or -1: a P2P
   room's ROOM row can't (focusable), and landing on it sent the cursor back to the bar on the
   next frame, so a pad could never reach TRACK, COMPUTER RIDERS or CHEATS there. */
static int setting_step(int f, int d, const RoomNow *r)
{
    for (f += d; f >= RF_TRACK && f <= RF_LAST_SETTING; f += d)
        if (focusable(f, r)) return f;
    return -1;
}

/* Up and down in the settings, across the bar; up from the bar to the last setting. */
static void move(int dx, int dy, const RoomNow *r)
{
    int was = rf, to = dy ? setting_step(rf, dy, r) : -1;
    if (rf <= RF_LAST_SETTING) {
        if (to >= 0) rf = to;
        else if (dy > 0) rf = focusable(rf_bar, r) ? rf_bar : RF_LEAVE;
    } else if (dx) {
        do rf = RF_RIDER + (rf - RF_RIDER + dx + 4) % 4; while (!focusable(rf, r));
    } else if (dy < 0 && r->in && (to = setting_step(RF_LAST_SETTING + 1, -1, r)) >= 0)
        rf = to;  /* up from the buttons: the panel's last row this PC can use */
    if (rf >= RF_RIDER) rf_bar = rf;
    if (rf != was) menu_page_sound(dx > 0 || dy > 0);
}

static void room_input(MenuPage *p, const RoomNow *r)
{
    int held = pads_held();
    if (held < 0) return;
    int pressed = held & ~room_buttons, dirs = held & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
    room_buttons = held;
    if (dirs && dirs == repeat_buttons) {  /* a held direction repeats, as the game's lists do */
        if (++repeat_frames > 24 && (repeat_frames - 24) % 6 == 0) pressed |= dirs;
    } else
        repeat_buttons = dirs, repeat_frames = 0;
    if (!p->armed) return;
    int dx = pressed & PAD_LEFT ? -1 : pressed & PAD_RIGHT ? 1 : 0, dy = pressed & PAD_UP ? -1 : pressed & PAD_DOWN ? 1 : 0;
    if (leave_ask) {
        if (dy) leave_choice ^= 1, menu_page_sound(dy > 0);
        if (pressed & PAD_CROSS) {
            if (leave_choice) leave_room(p);
            else leave_ask = 0, menu_play("S_SELECT");
        } else if (pressed & PAD_TRIANGLE)
            leave_ask = 0, menu_play("S_BACK");
        return;
    }
    if (rules_open) {
        int to = rules_cursor + dy;
        if (dy && to >= 0 && to <= RULE_PLAYERS) rules_cursor = to, menu_page_sound(dy > 0);
        if (dx && r->host) change_rule(rules_cursor, dx);
        if (pressed & (PAD_CROSS | PAD_TRIANGLE)) rules_open = 0, menu_play("S_BACK");
        return;
    }
    if (track_list) {
        int n = TRACK_COUNT, to = track_cursor + dx * TRACK_ROWS + dy;
        if ((dx || dy) && to >= 0 && to < n) track_cursor = to, menu_page_sound(dx > 0 || dy > 0);
        if (pressed & PAD_CROSS) close_track_list(1);
        else if (pressed & PAD_TRIANGLE) close_track_list(0);
        return;
    }
    if (!focusable(rf, r)) rf = focusable(rf_bar, r) ? rf_bar : focusable(RF_READY, r) ? RF_READY : RF_LEAVE;
    if (dx && rf <= RF_LAST_SETTING && rf != RF_RULES) change_setting(rf, dx);
    else if (dy || (dx && rf > RF_LAST_SETTING)) move(dx, dy, r);
    if (pressed & PAD_CROSS) activate(p, rf, r);
    else if (pressed & (PAD_SQUARE | PAD_START)) toggle_ready(r);
    else if (pressed & PAD_CIRCLE) copy_code();
    else if (pressed & PAD_TRIANGLE) open_leave(r, "Triangle");
}

static int inside(const Hit *h, int x, int y) { return h->w > 0 && x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h; }

/* The mouse: hover moves the cursor, a click does what Cross does there (on a setting's left
   half: the previous value), the wheel changes a setting or moves along the bar. */
static void room_mouse(MenuPage *p, int x, int y, int moved, int left, int wheel)
{
    RoomNow r = room_now();
    if (!p->armed) return;
    if (leave_ask) {
        for (int i = 0; i < 2; i++)
            if (inside(&leave_hits[i], x, y)) {
                if (moved && leave_choice != i) leave_choice = i, menu_page_sound(i);
                if (left) {
                    if (i) hle_log("online menu: Leave clicked\n"), leave_room(p);
                    else leave_ask = 0, menu_play("S_SELECT");
                }
            }
        return;
    }
    if (rules_open) {
        for (int i = 0; i <= RULE_PLAYERS; i++)
            if (inside(&rule_hits[i], x, y)) {
                if (moved && rules_cursor != i) rules_cursor = i, menu_page_sound(1);
                if (left && r.host) rules_cursor = i, change_rule(i, x < rule_hits[i].x + rule_hits[i].w / 2 ? -1 : 1);
            }
        if (left && inside(&rules_back, x, y)) rules_open = 0, menu_play("S_BACK");
        if (wheel && r.host) change_rule(rules_cursor, wheel > 0 ? -1 : 1);
        return;
    }
    if (track_list) {
        for (int i = 0; i < TRACK_COUNT; i++)
            if (inside(&track_hits[i], x, y)) {
                if (moved && track_cursor != i) track_cursor = i, menu_page_sound(1);
                if (left) track_cursor = i, close_track_list(1);
            }
        if (left && inside(&track_back, x, y)) close_track_list(0);
        for (; wheel > 0 && track_cursor > 0; wheel--) track_cursor--;
        for (; wheel < 0 && track_cursor < TRACK_COUNT - 1; wheel++) track_cursor++;
        return;
    }
    if (left && inside(&code_hit, x, y)) {
        input_by = "tap";
        copy_code();
        input_by = "pad";
        return;
    }
    for (int f = 0; f < RF_COUNT; f++) {
        if (!inside(&hits[f], x, y) || !focusable(f, &r)) continue;
        if (rf != f && (moved || left)) {
            rf = f;
            if (f >= RF_RIDER) rf_bar = f;
            menu_page_sound(1);
        }
        if (left) {
            input_by = "tap";
            if (f == RF_BOTS || f == RF_CHEATS || f == RF_PUBLIC) change_setting(f, 1);
            else activate(p, f, &r);
            input_by = "pad";
        }
    }
    input_by = "wheel";
    if (wheel && rf <= RF_LAST_SETTING && rf != RF_RULES) change_setting(rf, wheel > 0 ? -1 : 1);
    else if (wheel) move(wheel > 0 ? -1 : 1, 0, &r);
    input_by = "pad";
}

/* ---- news: who came, went or got ready, with a sound (each card's ready sound at most every
   3 s, so toggling Ready can't spam the room) and a note at the top for a moment ---- */

static void note_for(const char *text, const int *colour, double seconds)
{
    memmove(toast, toast + 1, sizeof toast[0] * (TOASTS - 1));
    snprintf(toast[TOASTS - 1].text, sizeof toast[0].text, "%s", text);
    toast[TOASTS - 1].colour = colour;
    toast[TOASTS - 1].until = host_seconds() + seconds;
}

static void note(const char *text, const int *colour) { note_for(text, colour, 3.4); }

static void room_news(void)
{
    const NetLobby *l = &room;
    if (l->state != NET_LOBBY) return;
    double now = host_seconds();
    char line[48];
    /* the room's results of each race, once, in the log (a scored round's points too, finding 204) */
    static int logged_race;
    if (l->last_race_id && l->last_race_id != logged_race) {
        char text[600], detail[40];
        const char *where = track_name(l->last_race_track);
        int n = snprintf(text, sizeof text, "online menu: the room's results of race %d (%s):", l->last_race_id,
                         where ? where : "?");
        logged_race = l->last_race_id;
        for (int k = 0; k < l->nresults && n < (int)sizeof text - 60; k++) {
            const NetResult *r = &l->result[k];
            if (r->ticks >= 0) room_finish(detail, sizeof detail, r->ticks, l->result_score[k]);
            else snprintf(detail, sizeof detail, "DNF %s", room_dnf_name(r->dnf_reason));
            n += snprintf(text + n, sizeof text - n, " %d. %.24s %s;", r->place, r->name, detail);
        }
        hle_log("%s\n", text);
    }
    if (seen_count < 0) {  /* the room as it was when this page opened: nothing new yet, */
        seen_count = l->nmembers;
        /* except a track changed during this PC's race (the Then rule, the host's pick) */
        if (raced_track >= 0 && !strcmp(raced_code, l->code) && l->track != raced_track) {
            snprintf(line, sizeof line, "TRACK CHANGED: %s", track_name(l->track));
            note_for(line, amber, 5.0);
            menu_play("S_NEXT");
            hle_log("online menu: %s (during the race)\n", line);
        }
        raced_track = -1;
        goto keep;
    }
    for (int i = 0; i < l->nmembers; i++) {
        const NetMember *m = &l->member[i];
        int k = 0;
        char who[ROOM_NAME_SIZE];
        room_name(m->name, who);
        while (k < seen_count && seen_id[k] != m->id) k++;
        if (k == seen_count) {
            snprintf(line, sizeof line, "%s JOINED", who);
            note(line, NULL);
            menu_play("S_SELECT");
        } else if (strcmp(m->race_state, seen_race[k]) && !strcmp(m->race_state, "finished")) {
            snprintf(line, sizeof line, "%s FINISHED %s", who, ordinal(m->place));
            note(line, green);
            menu_play("S_NEXT");
        } else if (strcmp(m->race_state, seen_race[k]) && !strcmp(m->race_state, "dnf") && m->id != l->member_id) {
            snprintf(line, sizeof line, "%s IS OUT (%s)", who, room_dnf_name(m->dnf_reason));
            note(line, red);
        } else if (m->ready && !seen_ready[k] && m->id != l->member_id && now - ready_heard[i] >= 3.0) {
            ready_heard[i] = now;
            snprintf(line, sizeof line, "%s IS READY", who);
            note(line, green);
            menu_play("S_NEXT");
        }
    }
    if (seen_host >= 0 && l->host_id != seen_host)
        for (int i = 0; i < l->nmembers; i++)
            if (l->member[i].id == l->host_id) {
                char who[ROOM_NAME_SIZE];
                room_name(l->member[i].name, who);
                snprintf(line, sizeof line, l->host_id == l->member_id ? "YOU ARE THE HOST NOW" : "%s IS THE HOST NOW", who);
                note(line, NULL);
            }
    int from, limit, to, switched = 0;
    if (strcmp(seen_event, l->last_event) && sscanf(l->last_event, "track_switch:%d:%d:%d", &from, &limit, &to) == 3) {
        snprintf(line, sizeof line, "%s TAKES %d: RACING %s", track_name(from), limit, track_name(to));
        note(line, amber);
        hle_log("online menu: %s\n", line);
        switched = 1;
    }
    /* Ready stays when the track changes (finding 128): the ready players hear and read it. Not
       this PC's own choice, nor the grid's switch just above. */
    if (seen_track >= 0 && l->track != seen_track && !switched && !(l->track == sent_track && now - sent_at < 10)) {
        snprintf(line, sizeof line, "TRACK CHANGED: %s", track_name(l->track));
        note_for(line, amber, 5.0);
        menu_play("S_NEXT");
        hle_log("online menu: %s\n", line);
    }
    if (!strcmp(l->last_event, "start_cancelled") && strcmp(seen_event, l->last_event))
        note("START CANCELLED: NOBODY WAS READY", amber);
    for (int k = 0; k < seen_count; k++) {
        int here = 0;
        for (int i = 0; i < l->nmembers; i++) here |= l->member[i].id == seen_id[k];
        if (!here) {
            char who[ROOM_NAME_SIZE];
            room_name(seen_name[k], who);
            snprintf(line, sizeof line, "%s LEFT", who);
            note(line, red);
            menu_play("S_BACK");
        }
    }
keep:
    seen_count = l->nmembers;
    seen_host = l->host_id;
    seen_track = l->track;
    snprintf(seen_event, sizeof seen_event, "%s", l->last_event);
    for (int j = 0; j < l->nmembers; j++) {
        seen_id[j] = l->member[j].id, seen_ready[j] = l->member[j].ready;
        snprintf(seen_name[j], sizeof seen_name[j], "%s", l->member[j].name);
        snprintf(seen_race[j], sizeof seen_race[j], "%s", l->member[j].race_state);
    }
}

static const uint16_t *track_preview(int track);

/* ---- drawing ---- */

static MenuStyle st_row, st_on, st_off;
static int vw = 640, vh = 224;
static int X(int x) { return x * vw / 640; }
static int Y(int y) { return y * vh / 224; }
/* every text of the room is solid: at the game's alpha its glyphs cover half (menu_solid) */
static MenuStyle tint(MenuStyle s, int percent, const int *c)
{
    return menu_solid(menu_style(s, percent, c ? c[0] : -1, c ? c[1] : 0, c ? c[2] : 0));
}
static void put(int align, int x, int y, const char *t, MenuStyle s) { menu_draw_text(align, X(x), Y(y), t, &s); }
static void box(int x, int y, int w, int h, int level) { menu_draw_box(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), level); }
static void rect(int x, int y, int w, int h, const int *c, int a) { menu_draw_rect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), c[0], c[1], c[2], a); }
static void bar(int x, int y, int w, int h) { menu_draw_bar(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y)); }
static void hit(Hit *h, int x, int y, int w, int h_) { h->x = (short)X(x), h->y = (short)Y(y), h->w = (short)(X(x + w) - X(x)), h->h = (short)(Y(y + h_) - Y(y)); }
/* A text's width in the layout's units (menu_text_width measures as capitals: an upper bound) */
static int width_of(const char *t, MenuStyle s) { return menu_text_width(t, &s) * 640 / (vw > 0 ? vw : 640); }

/* A style whose text fits w: smaller steps down to 60 %. */
static MenuStyle fit(const char *t, MenuStyle s, int percent, int w)
{
    MenuStyle f = menu_style(s, percent, -1, 0, 0);
    while (percent > 60 && menu_text_width(t, &f) > X(w)) percent -= 5, f = menu_style(s, percent, -1, 0, 0);
    return f;
}

/* Whether t fits w at fit's smallest size or above (menu_text_width measures as capitals). */
static int fits(const char *t, MenuStyle s, int percent, int w)
{
    MenuStyle f = fit(t, s, percent, w);
    return menu_text_width(t, &f) <= X(w);
}

/* The first of the texts (a NULL ends them) that fits w; the last one if none does. */
static const char *first_fit(const char *const *texts, MenuStyle s, int percent, int w)
{
    int i = 0;
    while (texts[i + 1] && !fits(texts[i], s, percent, w)) i++;
    return texts[i];
}

/* The layout's own check (finding 271): every text the room draws into a box of its own is
   measured against it, and one that is wider says so in dhd.log, once per text (a capture run
   with the widest names must log none). */
static void layout_check(const char *what, const char *t, MenuStyle s, int w)
{
    static char said[24][40];
    static int nsaid;
    int width = width_of(t, s);
    if (width <= w) return;
    for (int i = 0; i < nsaid; i++) if (!strncmp(said[i], t, sizeof said[i] - 1)) return;
    if (nsaid < 24) snprintf(said[nsaid++], sizeof said[0], "%s", t);
    hle_log("online menu: layout: %s \"%s\" is %d wide in %d\n", what, t, width, w);
}

/* Text into a box w wide (align as put's), checked */
static void put_in(const char *what, int align, int x, int y, int w, const char *t, MenuStyle s)
{
    layout_check(what, t, s, w);
    put(align, x, y, t, s);
}

/* An outline of t units round the inside of a rectangle */
static void outline(int x, int y, int w, int h, int t, const int *c)
{
    rect(x, y, w, t, c, 128);
    rect(x, y + h - t, w, t, c, 128);
    rect(x, y + t, t, h - 2 * t, c, 128);
    rect(x + w - t, y + t, t, h - 2 * t, c, 128);
}

/* A check mark (the font has none): two slanted strokes of the font's weight, each a strip of two
   triangles (menu_draw_strip), so the edges stay clean at every scale (lane 272). Its points in
   square units (one x unit both ways; a y unit of the popup view is 480 / 224 of them tall on the
   4:3 page): the short stroke p0 p1 down to the crotch p2 and the point p5, the long one up to
   p3 p4; both 4.2 wide across their own direction (the letters' strokes at 125 %). Drawn h units
   tall, its top left at x, y. */
static int check_width(int h) { return h * 480 * 1872 / 224 / 1692; }
static void check_mark(int x, int y, int h, const int *c)
{
    static const short p[6][2] = {{0, 950}, {310, 666}, {667, 1055}, {1550, 0}, {1872, 270}, {681, 1692}};
    static const char strips[2][4] = {{1, 0, 2, 5}, {3, 2, 4, 5}};
    for (int s = 0; s < 2; s++) {
        int xy[8];
        for (int i = 0; i < 4; i++) {
            const short *v = p[(int)strips[s][i]];
            /* square units (1/100) to layout units, then to 1/16 of the view's pixels */
            double lx = x + v[0] / 100.0 * h * 480 / 224 / 16.92, ly = y + v[1] / 100.0 * h / 16.92;
            xy[2 * i] = (int)(lx * vw * 16 / 640 + 0.5), xy[2 * i + 1] = (int)(ly * vh * 16 / 224 + 0.5);
        }
        menu_draw_strip(xy, 4, c[0], c[1], c[2]);
    }
}

/* ---- the left: what the next race is, and the room's code ---- */

/* A track's name in capitals, as the room writes every heading (finding 271) */
static const char *track_caps(int index)
{
    static char caps[2][64];
    static int k;
    char *out = caps[k ^= 1];
    snprintf(out, sizeof caps[0], "%s", track_name(index));
    for (char *c = out; *c; c++) *c = (char)toupper((unsigned char)*c);
    return out;
}

static const char *next_short(int n) { return n ? "HOST" : "AUTO"; }
static const char *then_short(int t) { return t == 0 ? "SAME" : t == 2 ? "RANDOM" : "NEXT"; }

/* One label / value row of the panel; the host's cursor shows its arrows on the value. */
static void info_row(int f, int k, const char *label, const char *value, const char *value_short, const RoomNow *r)
{
    int x = PAGE_X + 6, w = PANEL_W - 12, y = INFO_Y + 51 + k * 8, on = rf == f && r->host && f != RF_RULES;
    /* on the cursor's bar: the game's selected style, as its menus draw a focused row */
    MenuStyle ls = rf == f ? tint(st_on, 62, NULL) : tint(st_row, 62, t_dim), vs = rf == f ? tint(st_on, 62, NULL) : tint(st_row, 62, t_white);
    char v[40];
    /* a long label with a long value ("COMPUTER RIDERS" "OFF") steps both down together */
    for (int pct = 58; pct >= 50 && width_of(label, ls) + 6 + width_of(value_short ? value_short : value, vs) > w; pct -= 4)
        ls = rf == f ? tint(st_on, pct, NULL) : tint(st_row, pct, t_dim), vs = rf == f ? tint(st_on, pct, NULL) : tint(st_row, pct, t_white);
    int room_w = w - width_of(label, ls) - 6;
    /* the host's arrows when they fit (beside COMPUTER RIDERS they don't: the yellow says it) */
    snprintf(v, sizeof v, on ? "< %s >" : "%s", value);
    if (width_of(v, vs) > room_w && value_short) snprintf(v, sizeof v, on ? "< %s >" : "%s", value_short);
    if (width_of(v, vs) > room_w && on) snprintf(v, sizeof v, "%s", value_short && width_of(value, vs) > room_w ? value_short : value);
    put_in("info label", MENU_LEFT, x, y, w, label, ls);
    put_in("info value", MENU_RIGHT, x + w, y, room_w, v, vs);
    if (f != RF_RULES) hit(&hits[f], PAGE_X, y - 1, PANEL_W, 8);
}

static void draw_info(const RoomNow *r)
{
    const NetLobby *l = &room;
    int x = PAGE_X + 6, w = PANEL_W - 12, live = r->racing;
    box(PAGE_X, INFO_Y, PANEL_W, INFO_H, 90);
    box(PAGE_X, CODE_Y, PANEL_W, CODE_H, 90);
    put(MENU_LEFT, x, INFO_Y + 3, live ? "THIS RACE" : "NEXT RACE", tint(st_row, 60, t_dim));
    /* the cursor's bar (one a frame) behind the focused row */
    if (rf == RF_TRACK && r->host) bar(PAGE_X + 3, INFO_Y + 38, PANEL_W - 6, 11);
    else if (rf == RF_RULES) bar(PAGE_X + 3, INFO_Y + 50, PANEL_W - 6, 24);
    else if (rf >= RF_BOTS && rf <= RF_PUBLIC) bar(PAGE_X + 3, INFO_Y + 50 + (rf - RF_BOTS + 3) * 8, PANEL_W - 6, 8);
    /* the track's picture, as the picker shows it (finding 132), and its name */
    const uint16_t *picture = track_preview(l->track);
    rect(x - 1, INFO_Y + 10, w + 2, 28, r_black, 128);
    if (picture) menu_draw_picture(X(x), Y(INFO_Y + 11), X(x + w) - X(x), Y(INFO_Y + 37) - Y(INFO_Y + 11), picture);
    const char *track = track_caps(l->track);
    int on = rf == RF_TRACK && r->host;
    MenuStyle ts = fit(track, on ? st_on : tint(st_row, 100, t_white), 100, on ? w - 24 : w);
    if (on) {
        put(MENU_LEFT, x, INFO_Y + 38, "<", ts);
        put(MENU_RIGHT, x + w, INFO_Y + 38, ">", ts);
        put_in("track", MENU_CENTRE, x + w / 2, INFO_Y + 38, w - 24, track, ts);
    } else
        put_in("track", MENU_LEFT, x, INFO_Y + 38, w, track, ts);
    hit(&hits[RF_TRACK], PAGE_X, INFO_Y + 9, PANEL_W, 41);
    /* the rules (everyone reads them, Cross opens them; a change made during a race counts from the
       next one: *), then the room's own settings */
    const char *pending = l->rules_pending ? " *" : "";
    char window[24], next[24], then[24], next_s[16], then_s[16];
    snprintf(window, sizeof window, "%s%s", window_name(l->finish_window), pending);
    snprintf(next, sizeof next, "%s%s", l->next_race ? "HOST STARTS" : "AUTO, 30 S", pending);
    snprintf(next_s, sizeof next_s, "%s%s", next_short(l->next_race), pending);
    snprintf(then, sizeof then, "%s%s", then_name(l->then), pending);
    snprintf(then_s, sizeof then_s, "%s%s", then_short(l->then), pending);
    info_row(RF_RULES, 0, "FINISH WINDOW", window, NULL, r);
    info_row(RF_RULES, 1, "NEXT RACE", next, next_s, r);
    info_row(RF_RULES, 2, "THEN", then, then_s, r);
    hit(&hits[RF_RULES], PAGE_X, INFO_Y + 50, PANEL_W, 24);
    info_row(RF_BOTS, 3, "COMPUTER RIDERS", l->laps & ROOM_NO_BOTS ? "OFF" : "ON", NULL, r);
    info_row(RF_CHEATS, 4, "CHEATS", l->laps & ROOM_CHEATS ? "ON" : "OFF", NULL, r);
    info_row(RF_PUBLIC, 5, "ROOM", l->is_p2p ? "P2P, PRIVATE" : l->is_public ? "PUBLIC" : "PRIVATE", l->is_p2p ? "P2P" : NULL, r);
    /* the join code, large; Circle (or a click on it) copies it */
    int copied = copied_at > 0 && host_seconds() - copied_at < 1.5;
    {
        static const char *const labels[] = {"ROOM CODE - INVITE FRIENDS", "ROOM CODE - INVITE", "ROOM CODE", NULL};
        MenuStyle cs = tint(st_row, 100, t_dim);  /* fit and first_fit take the size from 100 % */
        const char *label = first_fit(labels, cs, 55, w);
        put_in("code label", MENU_LEFT, x, CODE_Y + 3, w, label, fit(label, cs, 55, w));
    }
    put_in("code", MENU_LEFT, x, CODE_Y + 9, w, l->code[0] ? l->code : "------", tint(st_on, 125, t_white));
    put_in("copy", MENU_LEFT, x, CODE_Y + 20, w, copied ? (copied_ok ? "COPIED" : "COULDN'T COPY") : GLYPH_CIRCLE " COPY CODE",
           tint(st_row, 60, !copied ? t_dim : copied_ok ? t_green : t_red));
    hit(&code_hit, PAGE_X, CODE_Y, PANEL_W, CODE_H);
}

/* ---- the list ---- */

static void draw_bars(int x, int y, int rtt)
{
    static const int unlit[3] = {0x30, 0x30, 0x40};
    int level = rtt < 0 ? 0 : rtt < 80 ? 3 : rtt < 150 ? 2 : 1;
    for (int i = 0; i < 3; i++) {
        int h = 2 + 2 * i;
        rect(x + i * 3, y - h, 2, h, i >= level ? unlit : level == 1 ? r_amber : r_lilac, 128);
    }
}

/* The list's columns (units from the row's left): the place, the name, the middle column, the
   state's right edge and the bars. Sized for "10TH", "WWWWWWWWWW.." with HOST, and "TARA LLANES";
   the layout check measures them (finding 271). */
enum { COL_PLACE = 8, COL_NAME = 46, COL_MIDDLE = 222, COL_STATE = 22, COL_BARS = 13, ROW_PERCENT = 80 };
static const int POOR_MS = 150;  /* draw_bars' one red bar: the ping's number shows too */

static void draw_row(int k, const RoomRow *w)
{
    const NetLobby *l = &room;
    int x = LIST_X, y = ROW_Y + k * ROW_STEP, right = LIST_X + LIST_W;
    MenuStyle rs = tint(st_row, ROW_PERCENT, NULL);
    box(x, y, LIST_W, ROW_H, w->member < 0 ? 60 : 90);
    if (w->member < 0) {
        put(MENU_LEFT, x + COL_PLACE, y + 1, w->place, tint(rs, 100, t_dim));
        put(MENU_LEFT, x + COL_NAME, y + 1, "OPEN", tint(rs, 100, t_dim));
        return;
    }
    const NetMember *m = &l->member[w->member];
    if (w->own) {
        rect(x, y, LIST_W, ROW_H, (const int[]){0x3C, 0x38, 0x00}, 60);
        outline(x - 1, y - 1, LIST_W + 2, ROW_H + 2, 1, r_yellow);
    }
    rect(x, y, 2, ROW_H, w->own ? r_yellow : w->edge == EDGE_GREEN ? r_green : w->edge == EDGE_AMBER ? r_amber : r_grey, 128);
    const int *place_c = w->dim ? t_dim : w->state_kind == RS_DONE ? t_amber : NULL;
    put_in("place", MENU_LEFT, x + COL_PLACE, y + 1, COL_NAME - COL_PLACE - 3, w->place, tint(rs, 100, place_c));
    MenuStyle ns = tint(rs, 100, w->dim ? t_dim : t_white);
    int name_w = width_of(w->name, ns), mark_x = x + COL_NAME + name_w + 5;
    put(MENU_LEFT, x + COL_NAME, y + 1, w->name, ns);
    const char *mark = w->host ? "HOST" : w->own ? "YOU" : NULL;
    int name_end = mark_x;
    if (mark) {
        MenuStyle bs = tint(st_row, 50, t_dark);
        int bw = width_of(mark, bs) + 10;
        rect(mark_x, y + 2, bw, 7, w->host ? r_lilac : r_yellow, 128);
        put(MENU_CENTRE, mark_x + bw / 2, y + 3, mark, bs);
        name_end = mark_x + bw;
        if (w->host && w->own) {  /* the host is this PC: both */
            int yw = width_of("YOU", bs) + 10;
            rect(name_end + 3, y + 2, yw, 7, r_yellow, 128);
            put(MENU_CENTRE, name_end + 3 + yw / 2, y + 3, "YOU", bs);
            name_end += 3 + yw;
        }
    }
    /* the ping: bars; its number too when it is poor */
    char ping[16] = "";
    int state_right = right - COL_STATE;
    draw_bars(right - COL_BARS, y + 9, m->rtt_ms);
    if (m->rtt_ms >= POOR_MS) {
        MenuStyle ps = tint(rs, 75, t_amber);
        snprintf(ping, sizeof ping, "%d MS", m->rtt_ms);
        put(MENU_RIGHT, right - COL_BARS - 3, y + 2, ping, ps);
        state_right = right - COL_BARS - 3 - width_of(ping, ps) - 6;
    }
    /* the state at the right: a chip (READY, WAIT, CHOOSING) or words */
    int state_left;
    if (w->state_kind == RS_READY || w->state_kind == RS_WAIT || w->state_kind == RS_CHOOSING) {
        MenuStyle cs = tint(st_row, 65, w->state_kind == RS_READY ? t_white : t_dim);
        int cw = width_of(w->state, cs) + 12;
        if (cw < 44) cw = 44;
        state_left = state_right - cw;
        if (w->state_kind == RS_READY) rect(state_left, y + 2, cw, 7, r_chip, 128);
        else rect(state_left, y + 2, cw, 7, r_wait, 128), rect(state_left + 1, y + 3, cw - 2, 5, r_dark, 128);
        put(MENU_CENTRE, state_left + cw / 2, y + 2, w->state, cs);
    } else {
        const int *c = w->state_kind == RS_DONE ? t_amber : w->state_kind == RS_RACING || w->state_kind == RS_READY_NEXT ? t_green
                     : t_dim;
        MenuStyle ss = tint(rs, w->state_kind == RS_DONE ? 100 : 85, c);
        state_left = state_right - width_of(w->state, ss);
        put(MENU_RIGHT, state_right, y + 2, w->state, ss);
        if (w->state_kind == RS_RACING) rect(state_left - 6, y + 4, 3, 3, r_green, 128);
    }
    /* the middle: the rider, or the last race's time / DNF */
    if (w->middle[0]) {
        MenuStyle ms = w->middle_kind == MID_RESULT ? tint(rs, 90, t_amber) : tint(rs, 72, t_dim);
        int mx = x + COL_MIDDLE > name_end + 6 ? x + COL_MIDDLE : name_end + 6;
        put_in("middle", MENU_LEFT, mx, y + 2, state_left - 8 - mx, w->middle, ms);
    }
    layout_check("name", w->name, ns, COL_MIDDLE - COL_NAME - 6 - (mark ? width_of(mark, tint(st_row, 50, NULL)) + 15 : 0));
    if (name_end > state_left - 8) hle_log("online menu: layout: row %d's name runs into its state\n", k);
}

static void draw_list(const RoomNow *r)
{
    const NetLobby *l = &room;
    RoomRow rows[NET_MAX_SLOTS];
    int n = room_rows(l, rider_names, rows);
    char left[64], right[32];
    const int *rc = t_dim;
    MenuStyle hs = tint(st_row, 65, t_dim);
    if (r->racing) {
        snprintf(left, sizeof left, "LIVE RESULTS");
        if (r->riding) snprintf(right, sizeof right, "%d STILL RACING", r->riding);
        else snprintf(right, sizeof right, "ALL IN");
        rc = t_blue;
    } else if (l->last_race_id) {
        snprintf(left, sizeof left, "LAST RACE - %s", track_caps(l->last_race_track));
        snprintf(right, sizeof right, "%d READY", r->ready);
        rc = r->ready ? t_green : t_dim;
    } else {
        /* where the room runs (findings 135, 181): its area, else its region */
        const char *area = l->room_area[0] ? l->room_area : l->room_region;
        snprintf(left, sizeof left, "PLAYERS %d/%d", l->nmembers, l->max_players);
        snprintf(right, sizeof right, "%s", area);
    }
    MenuStyle rs = tint(st_row, 70, rc);
    int right_w = width_of(right, rs);
    put_in("list head", MENU_RIGHT, LIST_X + LIST_W, HEAD_Y, LIST_W / 2, right, rs);
    if (!fits(left, hs, 65, LIST_W - right_w - 10) && l->last_race_id && !r->racing) snprintf(left, sizeof left, "LAST RACE");
    put_in("list head", MENU_LEFT, LIST_X, HEAD_Y, LIST_W - right_w - 10, left, hs);
    for (int k = 0; k < n && k < MEMBER_ROWS; k++) draw_row(k, &rows[k]);
}

/* ---- the buttons ---- */

/* ink colours (menu_draw_text_ink): all of what is under taken away (black), white added */
static const int ink_full[3] = {0x80, 0x80, 0x80}, ink_white[3] = {0xFF, 0xFF, 0xFF};

/* kind: 0 a plain button, 1 the main one (yellow), 2 the main one pressed (green, a check) */
static void draw_button(int f, int x, int w, const char *label, const char *why, int kind, const RoomNow *r)
{
    int on = rf == f && !track_list && !leave_ask && !rules_open, can = focusable(f, r);
    if (kind == 0) {
        box(x, BTN_Y, w, BTN_H, 90);
        if (on) bar(x + 3, BTN_Y + 3, w - 6, BTN_H - 6);
        /* on the cursor's yellow bar the words are dark: in ink, as the main button's (lane 272) */
        MenuStyle s = on ? tint(st_row, 100, ink_full) : tint(st_row, 100, can ? NULL : t_dim);
        int ly = why ? BTN_Y + 2 : BTN_Y + 4;
        MenuStyle f = fit(label, s, why ? 90 : 100, w - 14);
        layout_check("button", label, f, w - 14);
        if (on) menu_draw_text_ink(MENU_CENTRE, X(x + w / 2), Y(ly), label, &f, MENU_INK_DARK);
        else put(MENU_CENTRE, x + w / 2, ly, label, f);
        if (why) put_in("button why", MENU_CENTRE, x + w / 2, BTN_Y + 10, w - 8, why, tint(st_row, 55, t_amber));
    } else {
        rect(x, BTN_Y, w, BTN_H, kind == 1 ? r_yellow : r_chip, 128);
        if (on || kind == 2) outline(x - 1, BTN_Y - 1, w + 2, BTN_H + 2, 1, kind == 2 ? r_yellow : r_white);
        if (on && kind == 1) outline(x + 1, BTN_Y + 1, w - 2, BTN_H - 2, 1, r_black);
        /* the list's names' style at 125 %, in ink (lane 272): the font's dark edge round each glyph
           ran with the dark words on yellow into one blob (the selected style is the same size) */
        int ch = 8, cw = kind == 2 ? check_width(ch) + 6 : 0;  /* the check, 6 before the words */
        MenuStyle s = fit(label, tint(st_row, 125, kind == 1 ? ink_full : t_white), 125, w - 30 - cw);
        int left = x + w / 2 - (width_of(label, s) + cw) / 2;
        if (kind == 2) check_mark(left, BTN_Y + 5, ch, ink_white);
        layout_check("main button", label, s, w - 30 - cw);
        /* the words' middle (5.4 units under y at 100 %) on the button's, at the size fit chose */
        int mid10 = (BTN_Y + BTN_H / 2) * 10 - 54 * s.size[1] / (st_row.size[1] ? st_row.size[1] : 1);
        menu_draw_text_ink(MENU_LEFT, X(left + cw), mid10 * vh / 2240, label, &s, kind == 1 ? MENU_INK_DARK : MENU_INK_LIGHT);
    }
    hit(&hits[f], x, BTN_Y, w, BTN_H);
}

static void draw_buttons(const RoomNow *r)
{
    const NetLobby *l = &room;
    BarNow b = bar_now(r);
    char why[24];
    draw_button(RF_RIDER, PAGE_X, PANEL_W, selected ? "CHANGE RIDER" : "CHOOSE RIDER", NULL, 0, r);
    if (b.main == ACT_WATCH) draw_button(RF_READY, MAIN_X, MAIN_W, "WATCH THE RACE", NULL, 1, r);
    else draw_button(RF_READY, MAIN_X, MAIN_W, "READY", NULL, r->own_ready ? 2 : 1, r);
    int third_x = THIRD_X, third_w = THIRD_W;
    if (b.third == ACT_READY)
        draw_button(RF_START, third_x, third_w, r->own_ready ? "READY NEXT" : "READY NEXT", r->own_ready ? "YOU'RE IN" : NULL, 0, r);
    else if (b.third == ACT_START) {  /* the host's: why it can't start now, as before (finding 118) */
        const char *reason = NULL;
        if (r->racing) reason = "RACE LIVE";
        else if (r->starting) reason = "STARTING";
        else if (r->ready == 0) reason = "NOBODY READY";
        else if (r->ready < l->nmembers) snprintf(why, sizeof why, "%d NOT READY", l->nmembers - r->ready), reason = why;
        draw_button(RF_START, third_x, third_w, "START NOW", reason, 0, r);
    }
    draw_button(RF_LEAVE, LEAVE_X, LEAVE_W, "LEAVE", NULL, 0, r);
}

/* ---- the words: the header's small line, the sentence under the buttons ---- */

/* The newest note still showing (who came, went, got ready, finished...): one at a time, in the
   header's small line (finding 271: three stacked ones covered the header). NULL none. */
static const char *note_now(const int **colour)
{
    double now = host_seconds();
    for (int i = TOASTS - 1; i >= 0; i--)
        if (toast[i].until > now) {
            *colour = toast[i].colour;
            return toast[i].text;
        }
    return NULL;
}

/* What is happening, small, over the title. */
static void small_line(const RoomNow *r, char *out, int size)
{
    const NetLobby *l = &room;
    if (l->state == NET_RECONNECTING && l->notice[0]) snprintf(out, size, "%s", l->notice);
    else if (!r->in) snprintf(out, size, "CONNECTING...");
    else if (r->racing) snprintf(out, size, "RACE LIVE - %d FINISHED, %d RACING", r->finished, r->riding);
    else if (r->starting || r->intermission) snprintf(out, size, "%d OF %d READY", r->ready, l->nmembers);
    else if (l->nmembers < 2) snprintf(out, size, "WAITING FOR PLAYERS");
    else if (l->waiting_for_rider) snprintf(out, size, "WAITING FOR A RIDER");
    else if (l->next_race && !r->host) snprintf(out, size, "WAITING FOR THE HOST TO START");
    else if (l->next_race) snprintf(out, size, "YOU START THE RACE");
    else snprintf(out, size, "THE RACE STARTS WHEN EVERYONE IS READY");
}

/* One sentence under the buttons: what happens next and what this player can do. */
static void sentence(const RoomNow *r, char *line, int size, const int **colour)
{
    const NetLobby *l = &room;
    int waiting = 0, choosing_n = 0, who = -1, riding = -1;
    *colour = NULL;
    for (int i = 0; i < l->nmembers; i++) {
        if (l->member[i].choosing) choosing_n++, who = who < 0 ? i : who;
        else if (!l->member[i].ready) waiting++, who = who < 0 ? i : who;
        if (r->riding == 1 && !strcmp(l->member[i].race_state, "racing")) riding = i;
    }
    char name[ROOM_NAME_SIZE] = "";
    if (status[0]) snprintf(line, size, "%s", status);
    else if (!r->in) snprintf(line, size, "CONNECTING TO THE SERVER...");
    else if (l->is_p2p && host_seconds() < p2p_notice_until) snprintf(line, size, "%s", p2p_notice), *colour = t_amber;
    else if (r->starting && r->own_ready) snprintf(line, size, "YOU'RE IN THIS RACE. PRESS READY AGAIN TO STAY OUT."), *colour = t_green;
    else if (r->starting) snprintf(line, size, "YOU WEREN'T READY: READY NOW TO RACE THE ONE AFTER.");
    else if (r->racing && r->own_race[0]) {
        char wait[40];
        int most = seconds_to(l->finish_ends_at);
        if (riding >= 0) room_name(l->member[riding].name, name), snprintf(wait, sizeof wait, "WAITING FOR %s", name);
        else snprintf(wait, sizeof wait, "WAITING FOR %d RIDERS", r->riding);
        if (!strcmp(r->own_race, "finished")) {
            char place[8];
            room_ordinal(l->member[r->self].place, place, sizeof place);
            snprintf(line, size, most ? "YOU FINISHED %s. %s, %d S AT MOST." : "YOU FINISHED %s. %s.", place, wait, most);
            *colour = t_green;
        } else snprintf(line, size, "YOU'RE OUT OF THIS RACE. %s.", wait);
    } else if (r->racing && r->can_watch) snprintf(line, size, "THE RACE IS ON. WATCH IT, OR GET READY FOR THE NEXT ONE.");
    else if (r->racing && r->own_ready) snprintf(line, size, "YOU'RE IN THE NEXT RACE. PRESS AGAIN TO SIT IT OUT."), *colour = t_green;
    else if (r->racing) snprintf(line, size, selected ? "A RACE IS ON. PRESS READY TO RACE THE NEXT ONE."
                                                      : "A RACE IS ON. CHOOSE YOUR RIDER FOR THE NEXT ONE.");
    else if (!selected) snprintf(line, size, "CHOOSE YOUR RIDER FIRST.");
    else if (r->own_ready && (r->intermission || l->last_race_id))
        snprintf(line, size, l->next_race && !r->intermission ? "YOU ARE IN THE NEXT RACE. THE HOST STARTS IT."
                                                               : "YOU ARE IN THE NEXT RACE. PRESS AGAIN TO SIT IT OUT."), *colour = t_green;
    else if (r->intermission) snprintf(line, size, "PRESS READY TO RACE THE NEXT ONE. ONLY READY RIDERS START.");
    else if (l->waiting_for_rider) snprintf(line, size, "WAITING FOR A RIDER: THE FIRST READY STARTS A NEW 30 S.");
    else if (l->last_race_id && l->next_race && r->host && r->ready)
        snprintf(line, size, "START NOW WHEN YOU LIKE: ONLY READY RIDERS RACE.");
    else if (l->nmembers < 2) snprintf(line, size, "WAITING FOR PLAYERS: SHARE THE CODE %s.", l->code);
    else if (!r->own_ready)
        snprintf(line, size, l->next_race && !r->host ? "PRESS READY TO RACE. THE HOST STARTS THE RACE."
                           : r->host ? "PRESS READY TO RACE. START IT WHEN YOU LIKE." : "PRESS READY TO RACE. IT STARTS WHEN ALL ARE READY.");
    else if (waiting + choosing_n == 1 && who >= 0) {
        room_name(l->member[who].name, name);
        snprintf(line, size, l->member[who].choosing ? "YOU'RE READY. %s IS CHOOSING A RIDER." : "YOU'RE READY. WAITING FOR %s.", name);
    } else if (waiting + choosing_n) snprintf(line, size, "YOU'RE READY. WAITING FOR %d PLAYERS.", waiting + choosing_n);
    else snprintf(line, size, l->next_race && !r->host ? "EVERYONE IS READY. THE HOST STARTS THE RACE." : "EVERYONE IS READY.");
    for (char *k = line; *k; k++) if (*k >= 'a' && *k <= 'z') *k -= 32;
}

static void draw_words(const RoomNow *r)
{
    char line[120];
    const int *c;
    small_line(r, line, sizeof line);
    const int *note_c = NULL;
    const char *note_text = note_now(&note_c);
    if (note_text) {
        MenuStyle s = tint(st_row, 100, note_c ? (note_c == green ? t_green : note_c == red ? t_red : note_c == amber ? t_amber : NULL)
                                                : t_white);
        put_in("note", MENU_CENTRE, 320, SMALL_Y, PAGE_W, note_text, fit(note_text, s, 62, PAGE_W));
    } else
        put_in("small line", MENU_CENTRE, 320, SMALL_Y, PAGE_W, line, fit(line, tint(st_row, 100, t_dim), 62, PAGE_W));
    sentence(r, line, sizeof line, &c);
    MenuStyle s = fit(line, tint(st_row, 100, c ? c : t_white), 80, PAGE_W);
    put_in("sentence", MENU_CENTRE, 320, SENTENCE_Y, PAGE_W, line, s);
}

/* Loading-page landscape from the player's disc, not a repository asset. TEX has
   the 512-square indexed image; RTX has its 16-square CSM1 palette. Use the upper
   340 lines after rotating the loading mesh's upside-down UVs, excluding its cutout.
   Cache the ten destinations; variants retain their event name beside the picture. */
static const uint16_t *track_preview(int track)
{
    static const char *const pages[] = {"LALPSE", "LMOASE", "LJUNSE", "LPERSE", "LGLASE", "LAUBSE", "LFUJSE", "LBCSE", "LCITSE", "LMOSHSE"};
    static const unsigned char destination[26] = {0,1,2,3,3,4,0,1,2,3,2,5,6,1,4,5,5,8,8,7,8,8,4,9,0,7};
    static uint16_t *pictures[10];
    static int attempted[10];
    int d = destination[track];
    if (!attempted[d]) {
        attempted[d] = 1;
        unsigned char *tex = malloc(264192), *rtx = malloc(133120);
        char path[64]; uint32_t sector, size;
        int ok = tex && rtx;
        snprintf(path, sizeof path, "LOADBAR/%s.TEX", pages[d]);
        ok = ok && cdvd_find(path, &sector, &size) && size == 262512 && cdvd_read(sector, (size + 2047) / 2048, tex);
        snprintf(path, sizeof path, "LOADBAR/%s.RTX", pages[d]);
        ok = ok && cdvd_find(path, &sector, &size) && size == (d == 9 ? 1136 : 132336) && cdvd_read(sector, (size + 2047) / 2048, rtx);
        if (ok) {
            /* 256 x 160 for the picture's bilinear texture: each pixel the mean of its 2 x 2-3
               source pixels, then 5 bits a channel with a 4 x 4 ordered dither (PSMCT16) */
            static const unsigned char bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
            enum { W = MENU_PICTURE_W, H = MENU_PICTURE_H };
            pictures[d] = menu_picture_new();
            if (pictures[d]) for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                unsigned sum[3] = {0, 0, 0}, n = 0, pixel = 0x8000;
                for (int sy = y * 340 / H; sy < (y + 1) * 340 / H; sy++) for (int sx = x * 512 / W; sx < (x + 1) * 512 / W; sx++, n++) {
                    unsigned k = tex[0x170 + (511 - sy - (d == 9 ? 80 : 0)) * 512 + 511 - sx];
                    k = (k & ~24u) | ((k & 8) << 1) | ((k & 16) >> 1);
                    for (int c = 0; c < 3; c++) sum[c] += rtx[(d == 9 ? 0x10 : 0x60) + k * 4 + c];
                }
                for (int c = 0; c < 3; c++) {
                    unsigned v = (sum[c] + n / 2) / n + bayer[y & 3][x & 3] / 2;
                    pixel |= (v > 255 ? 31 : v >> 3) << (5 * c);
                }
                pictures[d][y * W + x] = (uint16_t)pixel;
            }
        }
        hle_log("online menu: preview %s %s\n", pages[d], pictures[d] ? "loaded from disc" : "unavailable");
        free(tex); free(rtx);
    }
    return pictures[d];
}

static void draw_track_list(void)
{
    rect(0, 0, 640, 224, (const int[]){0, 0, 0}, 90);
    box(36, 24, 568, 176, 110);
    put(MENU_CENTRE, 320, 30, "CHOOSE A TRACK", menu_style(st_row, 115, -1, 0, 0));
    /* Two columns of names, filled top to bottom in the list's (career) order; the rider limit goes
       beside the picture, so each name has room. 13 rows of 10.5 units fit between the title and
       BACK. */
    for (int i = 0; i < TRACK_COUNT; i++) {
        int x = 44 + (i / TRACK_ROWS) * 192, y = 45 + (i % TRACK_ROWS) * 21 / 2, on = i == track_cursor;
        const char *name = track_name(track_at(i));
        MenuStyle s = fit(name, on || i == room_track ? st_on : st_row, 90, 178);
        if (on) bar(x + 2, y - 1, 186, 10);
        put(MENU_CENTRE, x + 95, y, name, s);
        if (i == room_track) {  /* the current one: underlined under its name's own width, below the letters */
            int u = menu_text_width(name, &s) * 640 / vw;
            rect(x + 95 - u / 2, y + 9, u, 1, (const int[]){0xC0, 0xB8, 0x40}, 128);
        }
        hit(&track_hits[i], x, y - 1, 190, 10);
    }
    int at = track_at(track_cursor);
    char limit[32];
    snprintf(limit, sizeof limit, "UP TO %d RIDERS", track_limit(at));
    const uint16_t *picture = track_preview(at);
    box(430, 58, 164, 100, 110);
    if (picture) menu_draw_picture(438 * vw / 640, 66 * vh / 224, 148 * vw / 640, 49 * vh / 224, picture);
    put(MENU_CENTRE, 512, 124, track_name(at), fit(track_name(at), st_on, 95, 150));
    put(MENU_CENTRE, 512, 139, limit, fit(limit, st_row, 80, 150));
    put(MENU_CENTRE, 320, 184, "BACK", st_row);
    hit(&track_back, 270, 181, 100, 14);
}

static void draw_rules(const RoomNow *r)
{
    const NetLobby *l = &room;
    char line[64], about[96], capacity[8];
    rect(0, 0, 640, 224, (const int[]){0, 0, 0}, 90);
    box(110, 30, 420, 152, 110);
    put(MENU_CENTRE, 320, 36, "ROOM RULES", menu_style(st_row, 115, -1, 0, 0));
    for (int i = 0; i < RULES; i++) {
        int y = 56 + i * 28, on = i == rules_cursor;
        const char *label = i == RULE_WINDOW ? "FINISH WINDOW" : i == RULE_NEXT ? "NEXT RACE" : i == RULE_THEN ? "THEN" : "PLAYERS";
        const char *value = i == RULE_WINDOW ? window_name(l->finish_window) : i == RULE_NEXT ? next_name(l->next_race)
                          : i == RULE_THEN ? then_name(l->then) : (snprintf(capacity, sizeof capacity, "%d", l->max_players), capacity);
        snprintf(line, sizeof line, r->host ? "%s:  < %s >" : "%s:  %s", label, value);
        if (i == RULE_WINDOW && l->finish_window)
            snprintf(about, sizeof about, "AFTER THE FIRST PLAYER FINISHES, THE OTHERS HAVE %s. THEN DNF.", window_name(l->finish_window));
        else if (i == RULE_WINDOW) snprintf(about, sizeof about, "THE RACE WAITS FOR EVERYONE TO FINISH OR GIVE UP.");
        else if (i == RULE_NEXT && !l->next_race)
            snprintf(about, sizeof about, "30 S AFTER EACH RACE. ONLY READY RIDERS START; THE OTHERS WAIT HERE.");
        else if (i == RULE_NEXT) snprintf(about, sizeof about, "THE HOST STARTS EACH RACE (OR EVERYONE READY DOES).");
        else if (i == RULE_THEN) snprintf(about, sizeof about, "THE TRACK AFTER EACH RACE: SAME, NEXT IN THE LIST, OR RANDOM.");
        else snprintf(about, sizeof about, "ROOM SIZE: 2 TO 10 PLAYERS");
        if (on) bar(130, y - 1, 380, 12);
        put(MENU_CENTRE, 320, y, line, on ? st_on : st_row);
        put(MENU_CENTRE, 320, y + 12, about, fit(about, st_off, 70, 400));
        hit(&rule_hits[i], 120, y - 3, 400, 26);
    }
    const char *note = l->rules_pending ? "CHANGED DURING A RACE: THE NEW RULES COUNT FROM THE NEXT RACE"
                                        : "CHANGED DURING A RACE? IT COUNTS FROM THE NEXT RACE.";
    put(MENU_CENTRE, 320, 166, note, fit(note, l->rules_pending ? tint(st_row, 75, amber) : st_off, 75, 400));
    hit(&rules_back, 110, 160, 420, 20);
}

static void draw_leave(const RoomNow *r)
{
    const NetLobby *l = &room;
    rect(0, 0, 640, 224, (const int[]){0, 0, 0}, 90);
    box(170, 68, 300, 86, 110);
    put(MENU_CENTRE, 320, 76, "LEAVE THE ROOM?", menu_style(st_row, 115, -1, 0, 0));
    const char *why = r->host && l->nmembers > 1 ? "THE PLAYER HERE LONGEST BECOMES THE HOST"
                    : l->nmembers > 1 ? "YOU CAN COME BACK WITH THE CODE" : "YOU ARE THE LAST ONE: THE ROOM CLOSES";
    put(MENU_CENTRE, 320, 94, why, fit(why, st_off, 80, 280));
    for (int i = 0; i < 2; i++) {
        int y = 112 + i * 16;
        if (leave_choice == i) bar(240, y - 1, 160, 12);
        put(MENU_CENTRE, 320, y, i ? "LEAVE" : "STAY", leave_choice == i ? st_on : st_row);
        hit(&leave_hits[i], 230, y - 3, 180, 15);
    }
}

/* The title (the header's large line) and the footer, which the game draws: before its update. */
static void room_texts(MenuPage *p)
{
    const NetLobby *l = &room;
    RoomNow r = room_now();
    if (r.starting)
        snprintf(title_text, sizeof title_text, r.own_ready ? "RACE STARTS IN  %d" : "STARTING  %d", (l->countdown_ms + 999) / 1000);
    else if (r.racing && l->finish_ends_at) {
        int left = seconds_to(l->finish_ends_at);
        snprintf(title_text, sizeof title_text, "RACE ENDS IN  %d:%02d", left / 60, left % 60);
    } else if (r.racing) {
        double ms = l->race_started_at ? host_service_net_server_ms() - l->race_started_at : 0;
        int so_far = ms > 0 ? (int)(ms / 1000) : 0;
        snprintf(title_text, sizeof title_text, "RACE LIVE  %d:%02d", so_far / 60, so_far % 60);
    } else if (r.intermission) {
        int left = seconds_to(l->next_race_at);
        snprintf(title_text, sizeof title_text, "NEXT RACE IN  %d:%02d", left / 60, left % 60);
    } else if (r.in && l->nmembers >= 1)
        snprintf(title_text, sizeof title_text, "%d OF %d READY", r.ready, l->nmembers);
    else
        snprintf(title_text, sizeof title_text, "RACE ROOM");
    p->title = title_text;
    if (l->state == NET_RECONNECTING && l->notice[0])  /* the server restarts the room (finding 184) */
        snprintf(footer_text, sizeof footer_text, "%s", l->notice);
    else if (rules_open)
        snprintf(footer_text, sizeof footer_text, r.host ? GLYPH_LEFT GLYPH_RIGHT " CHANGE   " GLYPH_TRIANGLE " BACK"
                                                         : GLYPH_TRIANGLE " BACK");
    else if (leave_ask)
        snprintf(footer_text, sizeof footer_text, GLYPH_CROSS " CHOOSE   " GLYPH_TRIANGLE " STAY");
    else if (track_list)
        snprintf(footer_text, sizeof footer_text, GLYPH_CROSS " CHOOSE   " GLYPH_TRIANGLE " BACK");
    else if (rf == RF_TRACK)
        snprintf(footer_text, sizeof footer_text, GLYPH_LEFT GLYPH_RIGHT " TRACK   " GLYPH_CROSS " ALL TRACKS   " GLYPH_SQUARE
                 " READY   " GLYPH_TRIANGLE " LEAVE");
    else
        snprintf(footer_text, sizeof footer_text, GLYPH_CROSS " SELECT   " GLYPH_SQUARE " READY   " GLYPH_CIRCLE " COPY CODE   "
                 GLYPH_TRIANGLE " LEAVE");
    p->footer = footer_text;
}

/* The rest: after the game's update (MenuPage.draw). */
static void room_draw(MenuPage *p)
{
    const NetLobby *l = &room;
    RoomNow now = room_now(), *r = &now;
    if (screen != ROOM || room_closed) return;
    menu_draw_size(&vw, &vh);
    if (vw <= 0 || vh <= 0) vw = 640, vh = 224;
    st_row = menu_page_style(p, MENU_STYLE_ROW);
    st_on = menu_page_style(p, MENU_STYLE_SELECTED);
    st_off = menu_page_style(p, MENU_STYLE_OFF);
    menu_draw_begin();
    memset(hits, 0, sizeof hits);
    /* the rider screen behind, darker: its 3D rider read through the header and the list */
    rect(0, 0, 640, 224, r_black, 112);
    /* the header's bar: the countdown running out (green to the start, lilac to the next race, blue
       to the race's end), else before a race how many are ready (green) */
    {
        double left = 0, whole = 1;
        const int *c = r_green;
        if (l->countdown_ms >= 0) left = l->countdown_ms, whole = 5000;
        else if (r->intermission) left = l->next_race_at - host_service_net_server_ms(), whole = 30000, c = r_lilac;
        else if (r->racing && l->finish_ends_at && l->finish_window)
            left = l->finish_ends_at - host_service_net_server_ms(), whole = l->finish_window * 1000.0, c = r_blue;
        else if (!r->racing && r->in && l->nmembers > 0) left = r->ready, whole = l->nmembers;
        rect(PAGE_X, COUNT_Y, PAGE_W, 2, r_black, 128);
        if (left > 0) rect(PAGE_X, COUNT_Y, (int)(PAGE_W * (left > whole ? 1 : left / whole)), 2, c, 128);
    }
    /* The game draws all text over every box: a list or a question over the room would show the
       room's text through it, so it is drawn alone. */
    if (track_list || leave_ask || rules_open) {
        if (track_list) draw_track_list();
        else if (rules_open) draw_rules(r);
        else draw_leave(r);
        return;
    }
    draw_info(r);
    draw_list(r);
    draw_buttons(r);
    draw_words(r);
}

static void room_countdown_sounds(void)
{
    int ms = room.countdown_ms, shown = ms < 0 ? -1 : (ms + 999) / 1000;
    if (shown != countdown_shown) {
        if (countdown_shown < 0 && shown >= 0) menu_play("S_ENTER_GAME");  /* everyone is ready */
        else if (shown > 0 && shown < countdown_shown) menu_page_sound(shown > 3 ? 0 : 1);
        countdown_shown = shown;
    }
}

static double room_opened_at;  /* host_seconds when the room page last opened */

static void room_open(void)
{
    room_opened_at = host_seconds();
    rf = rf_bar = selected ? RF_READY : RF_RIDER;
    track_list = leave_ask = rules_open = room_closed = 0;
    seen_host = seen_track = -1;
    memset(seen_race, 0, sizeof seen_race);
    room_buttons = 0xFFFF, repeat_buttons = repeat_frames = 0;
    seen_count = -1, countdown_shown = -1;
    memset(toast, 0, sizeof toast);
    memset(ready_heard, 0, sizeof ready_heard);
    ready_wanted = seen_own_ready = -1, error_primed = 0;
    page.title_y = TITLE_Y;
    page.mouse = room_mouse;
    page.draw = room_draw;
}

static void room_update(MenuPage *p)
{
    NetLobby *l = &room;
    host_service_net_get_lobby(GADDR(l));
    if (l->is_p2p && !p2p_noticed) {  /* once per room, when it first shows */
        p2p_noticed = 1;
        p2p_notice_until = host_seconds() + 15;
        hle_log("online menu: P2P room: the address notice shown\n");
    }
    if (l->state == NET_FAILED || l->state == NET_IDLE) {
        refused(l);
        if (in_shell) {  /* the rider screen's Back, then ONLINE again with the reason */
            race = 0;
            menu_page_close(p);
            return;
        }
        screen = HOME;
        page.mouse = NULL, page.draw = NULL, page.title_y = 0;   /* HOME's taps and title again (not go: the reason stays) */
        menu_page_rebuild_at(p, home_row);
        return;
    }
    int in = l->state == NET_LOBBY || l->state == NET_RACING;
    if (in) room_sync(l);
    if (in)
        for (int k = 0; k < TRACK_COUNT; k++)
            if (track_at(k) == l->track) room_track = k;
    if (l->started || l->state == NET_RACING) {
        hle_log("online menu: room %s: %s\n", net_log_code(l->code).text, l->watching ? "watching the race" : "the race starts");
        race = 1;
        raced_track = l->track;
        snprintf(raced_code, sizeof raced_code, "%s", l->code);
        track_list = leave_ask = rules_open = 0;
        hle_online_selected_ride(chosen_rider, chosen_outfit, chosen_cycle, chosen_colour);
        hle_online_from_menu();
        menu_page_close(p);
        return;
    }
    RoomNow r = room_now();
    /* Wait for the server to acknowledge unready before handing input to the picker.
       If a start won the race, the earlier started branch loads the committed selection. */
    if (in && selection_requested && !r.own_ready) {
        selection_requested = 0;
        choosing = 1;
        unlock_all();
        race = 2;
        menu_page_close(p);
        return;
    }
    if (in) {
        static int seen_cheats = -1;  /* every PC logs the room's cheats as it sees them */
        if (seen_cheats != (l->laps & ROOM_CHEATS))
            hle_log("online menu: room %s: cheats %s\n", net_log_code(l->code).text, l->laps & ROOM_CHEATS ? "ON" : "OFF");
        seen_cheats = l->laps & ROOM_CHEATS;
        room_options = l->laps, public_room = l->is_public;
    }
    room_news();
    if (in) ready_news(l, &r);
    room_countdown_sounds();
    room_input(p, &r);
    /* tests (DHD_NET_SCRIPTED with DHD_NET_TEST_READY): ready again for the next race, as a player
       who presses READY in the room after each one, so a scripted run races on (at most 1 a second);
       a number of seconds above 1: that long after the room opens each time (pictures of a player
       not ready yet, finding 271) */
    static int test_ready = -1;
    static double test_ready_at, test_ready_after;
    if (test_ready < 0) {
        test_ready = getenv("DHD_NET_TEST_READY") && getenv("DHD_NET_SCRIPTED");
        test_ready_after = test_ready ? atof(getenv("DHD_NET_TEST_READY")) : 0;
    }
    if (test_ready && in && selected && !r.own_ready && !r.riding && !leave_ask && !room_closed &&
        host_seconds() - test_ready_at > 1 && (test_ready_after <= 1 || host_seconds() - room_opened_at > test_ready_after)) {
        test_ready_at = host_seconds();
        host_service_net_set_ready(1);
    }
    /* tests (DHD_NET_SCRIPTED with DHD_NET_TEST_WATCH=1): WATCH each race this PC is outside of, once,
       two seconds after the room shows it (finding 269) */
    static int test_watch = -1;
    static double test_watch_seen, test_watch_race;
    if (test_watch < 0) test_watch = getenv("DHD_NET_TEST_WATCH") && getenv("DHD_NET_SCRIPTED");
    if (test_watch && r.can_watch && !leave_ask && !room_closed && test_watch_race != l->race_started_at) {
        if (!test_watch_seen) test_watch_seen = host_seconds();
        if (host_seconds() - test_watch_seen > 2) {
            test_watch_race = l->race_started_at, test_watch_seen = 0;
            hle_log("online menu: WATCH by test\n");
            host_service_net_watch();
        }
    }
    if (!room_closed) room_texts(p);
}

static void page_update(MenuPage *p, int cursor)
{
    if (getenv("DHD_ONLINE_MENU_TRACE")) {
        static char last[160];
        char trace[160];
        snprintf(trace, sizeof trace, "view=%d row=%d entry=%s", screen, cursor,
                 screen == NAME || screen == CODE ? entry : "");
        if (strcmp(last, trace)) {
            snprintf(last, sizeof last, "%s", trace);
            hle_log("online menu: %s\n", trace);
        }
    }
    p->footer = status[0] ? status : NULL;
    poll_iso();   /* the hashing host job: its digest lands in iso_hash */
    poll_list();  /* the fetching host job: its rows land in list */
    if (connecting) {
        connect_poll(p);
        p->footer = status[0] ? status : NULL;
        return;
    }
    switch (screen) {
    case HOME:
        if (disc_note() != disc_shown) {  /* the check has ended: its lines under the rows */
            menu_page_rebuild_at(p, cursor);
            break;
        }
        {
            int kind = cursor < home_rows ? home_kind[cursor] : HOME_NAME;
            /* what keeps this PC out of a room; the list and the name need no disc */
            if (!status[0] && ((kind != HOME_NAME && kind != HOME_LIST) || disc_shown) && not_ready()) p->footer = not_ready();
            else if (!status[0] && kind == HOME_P2P) p->footer = p2p_row_notice;
        }
        break;
    case LIST:
        if (list_state == 2 && !list_shown) menu_page_rebuild(p);  /* the answer came: the rows again */
        for (int i = 0; list_shown && i < list_count; i++) list_row(i);
        break;
    case QUICK:
        if (list_state == 2 && !list_shown) {
            /* the server lists the busiest first */
            list_shown = 1;
            for (int i = 0; i < list_count; i++)
                if (list_joinable(&list[i])) {
                    hle_log("online menu: quick race: room %s (%d/%d, %s, region %s)\n", list[i].code, list[i].players,
                            list[i].max_players, list[i].phase, list[i].region[0] ? list[i].region : "unknown");
                    enter_room(0, list[i].code);
                    return;
                }
            hle_log("online menu: quick race: no public race to join\n");
            menu_page_rebuild_at(p, 1);
        }
        break;
    case NAME:
    case CODE:
        {
            int action = host_service_text_input_edit(GADDR(entry), (screen == NAME ? NAME_MAX : P2P_CODE_LENGTH) + 1, GADDR(screen == NAME ? name_letters : code_letters));
            if (action < 0) { go(HOME); return; }
            if (action > 0) { page_accept(p, 1); return; }
        }
        entry_text();
        if (!status[0])
            p->footer = screen == CODE ? "TYPE/CTRL+V - ENTER JOIN - ESC BACK" : "TYPE OR PAD - ENTER DONE - ESC CANCEL";
        break;
    case ROOM:
        room_update(p);
        break;
    }
}

static void page_accept(MenuPage *p, int row)
{
    if (connecting) return;  /* Triangle gives up; nothing else meanwhile */
    status[0] = 0;
    switch (screen) {
    case HOME:
        home_row = row;
        switch (row < home_rows ? home_kind[row] : -1) {
        case HOME_NAME:
            snprintf(entry, sizeof entry, "%s", name);
            entry_pick = 0;
            go(NAME);
            break;
        case HOME_QUICK:
        case HOME_LIST: {
            int quick = home_kind[row] == HOME_QUICK;
            const char *why = quick ? not_ready() : !server() ? "No server: set server = ... in dhd.ini" : NULL;
            if (why) snprintf(status, sizeof status, "%s", why);
            else {
                start_list();
                go(quick ? QUICK : LIST);
            }
            break;
        }
        case HOME_PUBLIC:
        case HOME_PRIVATE:
        case HOME_P2P:
            enter_room(home_kind[row], NULL);
            break;
        case HOME_REGION:
            region_step(p, 1);
            break;
        case HOME_JOIN: {
            /* tests (DHD_NET_SCRIPTED): DHD_NET_TEST_CODE is the code, as if typed (cross_race.py) */
            const char *test_code = getenv("DHD_NET_SCRIPTED") ? getenv("DHD_NET_TEST_CODE") : NULL;
            snprintf(entry, sizeof entry, "%.*s", P2P_CODE_LENGTH, test_code ? test_code : "");
            entry_pick = 0;
            go(CODE);
            break;
        }
        case HOME_UPDATE:
            hle_log("online menu: Update Now\n");
            host_service_update_request();  /* ends the game; the launcher updates it */
            break;
        }
        break;
    case LIST:
        if (list_state == 2 && row < list_count && list_count > 0) enter_room(0, list[row].code);
        else if (list_state == 2) {
            start_list();
            menu_page_rebuild(p);
        }
        break;
    case NAME:
    case CODE: {
        const char *letters = screen == NAME ? name_letters : code_letters;
        int max = screen == NAME ? NAME_MAX : P2P_CODE_LENGTH;
        size_t n = strlen(entry);
        if (row == 0) {
            if ((int)n < max) entry[n] = letters[entry_pick], entry[n + 1] = 0;
            /* an SFU code is whole at 6 (Up adds a P2P code's seventh), a P2P one at 7: Join */
            if (screen == CODE && ((int)n + 1 == CODE_LENGTH || (int)n + 1 == max)) menu_page_set_cursor(p, 1);
        } else if (screen == NAME) {
            while (n && entry[n - 1] == ' ') entry[--n] = 0;
            if (n) {
                snprintf(name, sizeof name, "%s", entry);
                save("name", "DHD_NET_NAME", name);
            }
            go(HOME);
        } else if ((int)n == CODE_LENGTH || (int)n == P2P_CODE_LENGTH)
            enter_room(0, entry);
        else
            snprintf(status, sizeof status, "A code has %d or %d letters and digits", CODE_LENGTH, P2P_CODE_LENGTH);
        break;
    }
    case QUICK:
        if (list_state == 2 && row == 1) enter_room(HOME_PUBLIC, NULL);  /* Quick Race found none: a public room */
        else if (list_state == 2 && row == 2) go(HOME);
        break;
    case ROOM:  /* the room reads the pads itself (room_input) */
        break;
    }
}

static void page_back(MenuPage *p)
{
    if (connecting) {
        hle_log("online menu: connecting given up\n");
        host_service_net_leave();
        reset_selection();
        connecting = 0;
        status[0] = 0;
        return;
    }
    switch (screen) {
    case HOME:
        menu_page_cancel(p);
        break;
    case NAME:
    case CODE: {
        size_t n = strlen(entry);
        if (n) entry[n - 1] = 0;
        else go(HOME);
        break;
    }
    case LIST:
    case QUICK:
        go(HOME);
        break;
    case ROOM:  /* Triangle asks first (room_input) */
        break;
    }
}

static void page_step(MenuPage *p, int row, int by)
{
    if (connecting) return;
    if (screen == HOME && row < home_rows && home_kind[row] == HOME_REGION) {
        region_step(p, by);
        return;
    }
    if ((screen == NAME || screen == CODE) && row == 0) {
        int n = screen == NAME ? (int)sizeof name_letters - 1 : (int)sizeof code_letters - 1;
        entry_pick = (entry_pick + by + n) % n;
        menu_page_sound(by > 0);
    }
}

static void page_left(MenuPage *p, int row) { page_step(p, row, -1); }
static void page_right(MenuPage *p, int row) { page_step(p, row, 1); }

/* The main menu's ONLINE row (patched func_001ED5B8), also when port_online_resume opens it
   again: the page until it closes; 2 when a room was joined (the rider screen next, as SINGLE
   PLAYER's), 0 when the player backed out. */
int port_online_menu(void)
{
    int resumed = resume_home;
    resume_home = 0;
    choosing = 0;
    reset_selection();
    load_player();
    offline_mode = SETUP_WORD(0x10C);
    offline_submode = SETUP_WORD(0x110);
    offline_players = SETUP_WORD(0x288);
    func_0023D898(0, &shell_distance, &shell_aspect);
    start_iso_check();
    page.build = page_build;
    page.update = page_update;
    page.accept = page_accept;
    page.back = page_back;
    page.left = page_left;
    page.right = page_right;
    page.title = "ONLINE";
    screen = HOME;
    /* the room's own drawing and taps (room_open) are not HOME's: a room left over the rider screen
       closes the page without go(HOME), and its tap handler took every tap on ONLINE after it
       (finding 218: no row could be tapped; the pads still worked) */
    page.mouse = NULL, page.draw = NULL, page.title_y = 0;
    if (!resumed) {
        home_row = 0;
        status[0] = 0;  /* a room that closed leaves its reason for HOME */
    }
    race = 0;
    in_shell = shop_done = 0;
    connecting = 0;
    run_page();
    if (!race) { host_service_net_leave(); reset_selection(); connecting = 0; }
    return race;
}

/* The main menu (patched func_001ED5B8), every frame: 1 once to choose ONLINE again after a
   room closed, when the camera is back on the title. */
int port_online_resume(void)
{
    void *animation = (void *)(uintptr_t)(0x08000000u + 0x4C6370);
    if (!resume_home) return 0;
    func_00203590(animation, 0x2330, 0, 1);
    float t = func_00203C28(animation);
    return t <= 0.0f || t >= 1.0f;
}

/* The main menu's labels (patched func_001ED5B8): its table has three; the fourth is ONLINE. */
int port_main_label(const int *table, int index)
{
    return index < 3 ? table[index] : (int)(uintptr_t)"ONLINE";
}

/* The main menu's rows as shown: the game's three, ONLINE, EXIT GAME (exit_menu.c). A build
   without online play (a browser page without its online part: host_service_net_offered, finding 143) shows no
   ONLINE row, so its fourth row is EXIT GAME; port_main_choice turns a shown row into the row the
   patched menu acts on. */
int port_main_rows(void)
{
    return host_service_net_offered() ? 5 : 4;
}

int port_main_choice(int shown)
{
    return shown >= 3 && !host_service_net_offered() ? shown + 1 : shown;
}

/* The choice as the setup block has it: after the shop's START (retail's func_0013A170 commits
   the bike there), or at GET READY. */
static void keep_selection(void)
{
    chosen_rider = SETUP_WORD(0x290);
    chosen_spot = SHELL_WORD(0x4C6910 + 4 * SHELL_WORD(0x29DF68));
    chosen_bike = SETUP_WORD(0x11C);
    for (int i = 0; i < 3; i++) {
        chosen_outfit[i] = SETUP_WORD(0x1B8 + 4 * i);
        chosen_cycle[i] = SETUP_WORD(0x11C + 4 * i);
    }
    chosen_colour = SETUP_WORD(0x254);  /* func_0013A170 commits the BIKE COLOR page's cursor there */
    selected = 1;
    host_service_net_pick(-1, chosen_rider / 32, chosen_bike);
    host_service_net_set_ready(0);
    hle_log("online menu: selection rider %d (carousel place %d, player %d), outfit %d %d %d, bike %d %d %d, colour %d\n",
            chosen_rider, chosen_spot, SHELL_WORD(0x29DF68), chosen_outfit[0], chosen_outfit[1], chosen_outfit[2],
            chosen_cycle[0], chosen_cycle[1], chosen_cycle[2], chosen_colour);
}

/* The shop's START online (patched func_0013BA60, right after retail's own commit, func_0013A170,
   and func_00140C38): the bike, jersey and bike colour the player chose are in the setup block now.
   Keep them, and 1: the shop then leaves as its Back does (the shell's own way back to the rider
   screen, camera included), not to the level chooser; port_online_shell opens the room. Finding
   092 section 3 read the bike too early (before this commit); here it is retail's committed one. */
int port_online_shop_commit(void)
{
    if (!choosing) return 0;
    keep_selection();
    hle_log("online menu: the shop's choice kept\n");
    shop_done = 1;
    return 1;
}

/* Scripted test runs (DHD_NET_SCRIPTED) press nothing in the shop: its START on its first frame,
   so they go on to the room as before. DHD_NET_TEST_SHOP=1 leaves the shop to the pads. */
int port_online_shop_auto(void)
{
    static int automatic = -1;
    if (automatic < 0) automatic = getenv("DHD_NET_SCRIPTED") && !getenv("DHD_NET_TEST_SHOP");
    return choosing && automatic;
}

void hle_online_default_bike(int rider, int32_t bike[3]);

/* The ride to the shop (menu 18), once, before the shop opens (func_001F1500 runs it when the
   camera arrives): the setup's bike, where the shop's BIKES page starts online (patched
   func_00139BF8: func_0013A270 finds it in the list), is this rider's last choice, else the
   rider's default (a pro's own bike, the others the stock one, issue 18): START without a change
   keeps it. */
static void shop_prepare(void)
{
    int32_t bike[3];
    int rider = SETUP_WORD(0x290);
    if (selected && chosen_rider == rider) memcpy(bike, chosen_cycle, sizeof bike);
    else hle_online_default_bike(rider, bike);
    for (int k = 0; k < 3; k++) SETUP_WORD(0x11C + 4 * k) = bike[k];
    hle_log("online menu: the shop opens on bike %d %d %d (rider %d)\n", bike[0], bike[1], bike[2], rider);
}

/* The shell's frame, before its menu runs; 1 skips that menu this frame. Only online selection
   enters here. The room is a page over the rider screen (menu 3):
   - the shop's START (port_online_shop_commit) keeps the choice; its Back path back on the shell,
     the rider screen's Back cleanup and the camera to the rider's stand, as below;
   - GET READY (menu 9), which the shop no longer reaches online, keeps the choice and flies the
     camera back to the rider screen, as the main menu's SINGLE PLAYER does, instead of loading a
     race before everyone is ready;
   - before a selection is committed, the rider screen's Back returns to ONLINE;
   - changing an existing selection, Back opens the room;
   - Change Selection closes the room on the rider screen itself;
   - leaving the room (or losing it) takes the rider screen's own Back to the title, where the
     main menu opens ONLINE again (port_online_resume). */
int port_online_shell(void)
{
    if (!choosing) return 0;
    int menu = SHELL_WORD(0x29DED8);
    if (!shop_done && menu == 11 && SHELL_WORD(0x29DEEC) == 18) {
        /* The shop cancelled to Single Race, the page we auto-accept going
           forward. Restore the carousel before that shortcut selects again. */
        chosen_spot = SHELL_WORD(0x4C6910);
        port_online_choose_begin(0);
        hold_rider = 1;
        hle_log("online menu: shop cancelled: choosing the rider\n");
        return 1;
    }
    if (menu == 16) {
        /* Single races only: bypass the career's NEW / CONTINUE and mode menus. */
        SHELL_WORD(0x29DED8) = 11;
        SHELL_WORD(0x29DEEC) = 1;
        SETUP_WORD(0x10C) = 12;
        SETUP_WORD(0x110) = 0;
        return 0;
    }
    NetLobby connection;
    host_service_net_get_lobby(GADDR(&connection));
    int lost = connection.state == NET_FAILED || connection.state == NET_IDLE;
    static int shop_prepared;
    if (menu != 18) shop_prepared = 0;
    else if (!shop_prepared && !lost) shop_prepared = 1, shop_prepare();
    if (shop_done && menu != 18) {
        /* The shop has closed by its Back path (func_00139BF8's cancel, func_001F1500's way back):
           leave as the rider screen's Back does; the room (with the host's track) opens over the
           rider screen, on the player's rider. */
        hle_log("online menu: the shop closed (menu %d): back to the room\n", menu);
        shop_done = 0;
        selection_end();
        return 1;  /* the main menu next frame: the flight to the rider screen below */
    }
    if (room_pending && (menu == 15 || menu == 0)) room_pending = 0;  /* backed out meanwhile */
    if (room_pending) {
        /* The rider screen's own frames put the camera on it; the room's page freezes it. */
        if (menu != 3 || ++room_pending < 45) return 0;
        room_pending = 0;
    } else if (menu == 9 || menu == 15 || menu == 0) {
        if (menu == 9 && !lost) keep_selection();
        func_0023D8D8(0, shell_distance, shell_aspect);
        port_online_choose_begin(0);  /* to the rider screen without the title's flight */
        room_pending = 1;
        return 0;
    } else if (menu == 3) {
        /* Read the pads, not func_001F4A98: its press latch would take the rider screen's own. */
        static int was_held;
        int held = pads_held();
        if (held < 0) held = was_held;
        int back = held & 0x1000 && !(was_held & 0x1000);  /* Triangle */
        was_held = held;
        if (hold_rider) {
            if (held) return 1;
            hold_rider = 0;
        }
        if (!back && !lost) return 0;
        if (back && !selected && !lost) {
            hle_log("online menu: rider choice cancelled: ONLINE\n");
            race = 0;
            goto leave_selection;
        }
    } else
        return 0;

    in_shell = 1;
    func_001F57D0(0x232B, 0);  /* the rider screen's name plates, which would show through */
    func_001F57D0(0x233D, 0);
    func_001F57D0(0x235F, 0);
    if (!lost) host_service_net_pick(-1, selected ? chosen_rider / 32 : 0, selected ? chosen_bike : 0);
    go(ROOM);
    race = 0;
    run_page();
    in_shell = 0;
    if (race == 2) {
        hold_rider = 1;  /* the rider screen is here: choose on it */
        return 1;
    }
leave_selection:
    choosing = 0;
    if (race == 1) {
        SHELL_WORD(0x29DED8) = 9;
        SHELL_WORD(0x29DEEC) = 8;
        SHELL_WORD(0x29DF40) = 120;
        SHELL_WORD(0x29DF44) = 0;
        return 1;
    }
    reset_selection();
    host_service_net_leave();
    selection_end();
    SETUP_WORD(0x10C) = offline_mode;
    SETUP_WORD(0x110) = offline_submode;
    SETUP_WORD(0x288) = offline_players;
    resume_home = 1;
    port_online_relock();
    return 1;
}

/* This PC's race is over and the room stays (online.c back_to_room): the shell opens the room
   again over the rider screen once the front end is up, as when the room was first made. */
void port_online_back_to_room(void)
{
    hle_log("online menu: back to the room after the race (menu %d)\n", SHELL_WORD(0x29DED8));
    choosing = 1;
    room_pending = 0;
}

/* Back from the shop the rider screen shows the chosen rider (port_online_choose_begin), not the
   carousel's first place, which the rider screen's Back cleanup (selection_end) leaves. */
int port_online_rider_spot(void)
{
    int spot = selected && chosen_spot >= 1 && chosen_spot <= 14 ? chosen_spot : 1;
    hle_log("online menu: the rider screen on carousel place %d\n", spot);
    return spot;
}

/* Guests use the same retail handoff into the shop, without choosing a competing track. */
int port_online_guest_track(void)
{
    if (!choosing) return 0;
    NetLobby lobby;
    host_service_net_get_lobby(GADDR(&lobby));
    return !lobby.is_host;
}

int port_online_single_race(void) { return choosing; }

/* A track's name as the online launcher lists it (presence.c); NULL for a track not in the list. */
const char *port_track_name(int index)
{
    for (int k = 0; k < TRACK_COUNT; k++)
        if (track_at(k) == index) return track_name(track_at(k));
    return NULL;
}
