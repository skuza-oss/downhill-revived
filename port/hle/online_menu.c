/* Online rooms in the game's popup style (menu_page.c), with physical keyboard text entry.
   The flow (finding 092): Create or Join waits on the ONLINE page until the server has the player
   in the room; then the game's own rider screen, once, its ride to the shop and the shop's own
   pages (BIKES / JERSEYS / BIKE COLOR, finding 128), whose START (port_online_shop_commit) keeps
   the choice and opens the RACE ROOM over the rider screen (the rider on its stand, Change Rider
   to choose again); the host starts; loading; the race. The host chooses the track and starts
   when everyone is ready. The existing online.c setup event carries each complete choice. */
#include "hle.h"
#include "menu_page.h"
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
static const struct { int index; } tracks[] = {
    {0}, {1}, {2}, {3}, {4}, {5}, {6}, {7}, {8}, {9}, {10}, {11}, {12},
    {13}, {14}, {15}, {16}, {17}, {18}, {19}, {20}, {22}, {23}, {24}, {25}};
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
enum { NAME_MAX = SETTING_NAME_MAX, CODE_LENGTH = 6, ROW_TEXT = 96, MAX_ROWS = 12, LIST_MAX = 8, MEMBER_ROWS = 10 };

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
int func_00220230(int port);  /* the buttons held on a pad */
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
static int outdated;  /* the server refused this release: HOME offers Update Now (row 5) */
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
    const char *path = getenv("DHD_ISO");
    if (!strcmp(iso_hash, us_files_sha256))
        hle_log("online menu: the disc image has the USA disc's game files (files SHA-256 %s, %s)\n", iso_hash, how);
    else
        hle_log("online menu: the disc image %s has game files different from the USA disc's (redump 30393)%s%s: "
                "files SHA-256 %s, online needs %s (%s). A bad or modified dump: it can't be used online\n",
                path ? path : "(none)", *note ? ": " : "", note, iso_hash, us_files_sha256, how);
}

/* iso-check.txt, the check's cache (dhd_disc, shared with the launcher). */
static void iso_check_path(char *out, size_t size)
{
    const char *data = host_getenv("DHD_DATA_DIR");  /* the launcher's folder for the player's files */
    if (data) { snprintf(out, size, "%s/iso-check.txt", data); return; }
    char exe[HOST_PATH_MAX];
    host_exe_path(exe, sizeof exe);
    char *slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0;
    snprintf(out, size, "%siso-check.txt", exe);
}

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
        hle_log("online menu: the disc image %s can't be found for the online check\n", path ? path : "(none)");
        set_state(&iso_state, 3);
        return;
    }
    iso_check_path(cache, sizeof cache);
    hle_log("online menu: checking the disc image's game files\n");
    set_state(&iso_state, 1);
    if (!host_service_disc_check_start(GADDR(path), GADDR(cache))) {
        hle_log("online menu: the disc image's check couldn't start\n");
        set_state(&iso_state, 3);
    }
}

/* One poll of the checking host job: the hash lands in iso_hash. */
static void poll_iso(void)
{
    static uint8_t digest[32];
    static char note[256];
    const char *path = getenv("DHD_ISO");
    int r;
    if (iso_state != 1) return;
    r = host_service_disc_check_poll(GADDR(digest), GADDR(note), sizeof note);
    if (!r) return;
    if (r == -1) {
        hle_log("online menu: reading the disc image %s failed during its online check: %s\n", path, note);
        set_state(&iso_state, 3);
        return;
    }
    if (r == -2) {
        /* no file system to hash, or a cut dump: no hash, and not the USA disc's files */
        memset(iso_hash, '0', 64), iso_hash[64] = 0;
        set_state(&iso_state, 2);
        hle_log("online menu: the disc image %s can't have the USA disc's game files: %s. A bad or modified dump: "
                "it can't be used online\n", path, note);
        return;
    }
    for (int i = 0; i < 32; i++) snprintf(iso_hash + 2 * i, 3, "%02x", digest[i]);
    set_state(&iso_state, 2);
    log_iso(r == 2 ? "from iso-check.txt" : "checked now", note);
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
    for (int k = 0; k < COUNT(tracks); k++)
        if (tracks[k].index == l->track) track = track_name(tracks[k].index);
    if (strcmp(l->version, version)) snprintf(now, sizeof now, "OTHER VERSION");
    else if (l->full || l->players >= l->max_players) snprintf(now, sizeof now, "FULL");
    else net_lobby_now(l, t, now, sizeof now);  /* counted only on a known server clock (finding 135) */
    /* where the room runs (EU, NA, ASIA...; finding 135): a far room shows before joining it */
    snprintf(text[i], ROW_TEXT, "%.14s  %s%s%d/%d  %s  %s", l->room_name[0] ? l->room_name : l->host_name,
             l->region, l->region[0] ? "  " : "", l->players, l->max_players, track, now);
}

static void enter_room(int create, const char *code)
{
    const char *why = not_ready();
    if (why) {
        snprintf(status, sizeof status, "%s", why);
        hle_log("online menu: can't %s: %s\n", create ? "create a room" : "join", why);
        return;
    }
    host_service_net_init(GADDR(server()), GADDR(name), GADDR(version), GADDR(iso_hash));
    if (create) host_service_net_create_room();
    else if (host_service_net_join_room(GADDR(code)) < 0) {
        snprintf(status, sizeof status, "That code can't be one");
        return;
    }
    reset_selection();
    choosing = 0;
    public_room = create == 2;
    if (create) room_options &= ~ROOM_CHEATS;  /* a new room starts with cheats off */
    room_settings_sent = rotation_sent = !create;  /* a guest who becomes host keeps the room as it is */
    room_ready_sent = 0;
    connecting = 1;
    snprintf(status, sizeof status, "Connecting to the server...");
    hle_log("online menu: %s %s\n", create ? "creating a room" : "joining", create ? (public_room ? "(public)" : "(private)") : code);
}

static void page_build(MenuPage *p)
{
    switch (screen) {
    case HOME:
        p->title = "ONLINE";
        snprintf(text[0], ROW_TEXT, "Name:  %s", name);
        menu_page_row(p, text[0], 1);
        menu_page_row(p, "Quick Race", 1);
        menu_page_row(p, "Public Races", 1);
        menu_page_row(p, "Create A Public Race", 1);
        menu_page_row(p, "Create A Private Race", 1);
        menu_page_row(p, "Join With A Code", 1);
        if (outdated) menu_page_row(p, "Update Now", 1);
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
    int max = screen == NAME ? NAME_MAX : CODE_LENGTH;
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
    if (too_old) home_row = 6;
}

/* In the room: the host's settings, then this member's first (not ready) state, once each. */
static void room_sync(const NetLobby *l)
{
    if (l->is_host && !room_settings_sent) {
        char room_name[40];
        snprintf(room_name, sizeof room_name, "%s", name);
        /* tests (DHD_NET_SCRIPTED with DHD_NET_TEST_TRACK=<game track index>): the room starts there */
        const char *test_track = getenv("DHD_NET_SCRIPTED") ? getenv("DHD_NET_TEST_TRACK") : NULL;
        for (int k = 0; test_track && k < COUNT(tracks); k++)
            if (tracks[k].index == atoi(test_track)) room_track = k;
        NetRoomSettings s = {tracks[room_track].index, room_options, public_room, 10, -1, room_name};
        host_service_net_set_room(GADDR(&s));
        room_settings_sent = 1;
    }
    if (l->is_host && !rotation_sent) {
        /* the tracks in the list's order: the room's Then rule (next / random) walks them */
        static unsigned char ids[COUNT(tracks)];
        for (int k = 0; k < COUNT(tracks); k++)
            ids[k] = (unsigned char)(tracks[k].index | ((10 - track_limit(tracks[k].index)) / 2 << 5) | (arena(tracks[k].index) ? 0x80 : 0));
        host_service_net_rotation(GADDR(ids), COUNT(tracks));
        rotation_sent = 1;
        hle_log("online menu: the room's track list: %d tracks, MOSH PIT pick-only\n", COUNT(tracks));
        /* tests (DHD_NET_SCRIPTED with DHD_NET_TEST_THEN=same|next|random): the host's Then rule */
        const char *test_then = getenv("DHD_NET_SCRIPTED") ? getenv("DHD_NET_TEST_THEN") : NULL;
        if (test_then) {
            int then = !strcmp(test_then, "same") ? 0 : !strcmp(test_then, "random") ? 2 : 1;
            host_service_net_rules(l->finish_window, l->next_race, then);
            hle_log("online menu: room rules for the test: then %s\n", test_then);
        }
    }
    if (!room_ready_sent) {
        hle_log("online menu: room %s%s, version %s\n", l->code, l->is_host ? " (host)" : "", version);
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
    for (int k = 0; k < COUNT(tracks); k++)
        if (tracks[k].index == l.track) room_track = k;
    room_options = l.laps;
    connecting = 0;
    status[0] = 0;
    unlock_all();
    choosing = 1;
    race = 2;
    host_service_net_pick(-1, -1, 0);  /* "Choosing..." in the room meanwhile */
    hle_log("online menu: in room %s: choosing the rider\n", l.code);
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

enum { RF_TRACK, RF_BOTS, RF_CHEATS, RF_PUBLIC, RF_RULES, RF_RIDER, RF_READY, RF_START, RF_LEAVE, RF_COUNT };
enum { RULE_WINDOW, RULE_NEXT, RULE_THEN, RULE_PLAYERS, RULES };
enum { TOASTS = 3, TRACK_COLUMNS = 2 };
/* The layout, in the popup view's 640 x 224 (menu_draw_size) */
enum { L_X = 48, L_W = 166, TOP = 40, CARD_X = 224, CARD_W = 368, CARD_Y = 52, CARD_H = 11, CARD_GAP = 0,
       MID_H = 117, BAR_Y = 164, BAR_H = 18, BAR_X = 48, BAR_W = 544, STATUS_Y = 189, TITLE_Y = 14 };
static const int green[3] = {0x30, 0x80, 0x28}, amber[3] = {0x80, 0x60, 0x10}, red[3] = {0x80, 0x30, 0x30},
                 blue[3] = {0x38, 0x60, 0x98}, lilac[3] = {0x70, 0x68, 0x98};

typedef struct { short x, y, w, h; } Hit;
static NetLobby room;                       /* this frame's snapshot */
static int rf = RF_READY, rf_bar = RF_READY;  /* the cursor; the bar's place, kept meanwhile */
static int track_list, track_cursor;          /* all the tracks, open over the room */
static int leave_ask, leave_choice;           /* "Leave the room?": 0 Stay, 1 Leave */
static int rules_open, rules_cursor;          /* ROOM RULES, open over the room */
static Hit hits[RF_COUNT], code_hit, track_hits[COUNT(tracks)], track_back, leave_hits[2], rule_hits[RULES], rules_back;
static int room_buttons, repeat_buttons, repeat_frames;
static double copied_at;
static int copied_ok;
static struct { char text[48]; const int *colour; double until; } toast[TOASTS];
static int seen_id[NET_MAX_SLOTS], seen_ready[NET_MAX_SLOTS], seen_count = -1;
static char seen_name[NET_MAX_SLOTS][16];
static double ready_heard[NET_MAX_SLOTS];     /* per card: the last ready sound (no spam) */
static int countdown_shown = -1;
static char seen_race[NET_MAX_SLOTS][12];     /* per card: its race state last seen (news) */
static int seen_host = -1;
static int seen_track = -1;                   /* the room's track last seen (TRACK CHANGED) */
static int raced_track = -1;                  /* the track this PC's race was on, and its room: */
static char raced_code[8];                    /* back from it, a change made meanwhile is news */
static char seen_event[32];                   /* the server's last notice seen (start_cancelled) */
static char title_text[32], footer_text[96];

/* The buttons held on both pads. A pad's word can read all ones for one field as a press starts
   (its entry is 0 before the next read, func_00220230): -1 then, so that field changes nothing. */
static int pads_held(void)
{
    int a = func_00220230(0) & 0xFFFF, b = func_00220230(1) & 0xFFFF;
    return a == 0xFFFF || b == 0xFFFF ? -1 : a | b;
}

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
    for (int i = 0; i < l->nmembers; i++) {
        const NetMember *m = &l->member[i];
        r.ready += m->ready;
        if (r.racing && !strcmp(m->race_state, "racing")) r.riding++;
        if (r.racing && !strcmp(m->race_state, "finished")) r.finished++;
        if (m->id != l->member_id) continue;
        r.self = i, r.own_ready = m->ready, r.own_choosing = m->choosing;
        if (r.racing) r.own_race = m->race_state;
    }
    /* the host starts the next race with whoever is ready (the others wait here) */
    r.can_start = r.host && !r.racing && !r.starting && r.ready > 0;
    return r;
}

static int focusable(int f, const RoomNow *r)
{
    switch (f) {
    case RF_TRACK: case RF_BOTS: case RF_CHEATS: case RF_PUBLIC: return r->host && r->in;
    case RF_RULES: return r->in;  /* everyone reads them; the host changes them */
    case RF_RIDER: return r->in;
    case RF_READY: return r->in && strcmp(r->own_race, "racing") != 0;
    case RF_START: return r->can_start;
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

/* A time in race ticks (60 a second) as M:SS.CC */
static void race_time(char *out, int size, long long ticks)
{
    snprintf(out, size, "%d:%02d.%02d", (int)(ticks / 3600), (int)(ticks / 60 % 60), (int)(ticks % 60 * 100 / 60));
}

static const char *ordinal(int place)
{
    static char text[8];
    snprintf(text, sizeof text, "%d%s", place, place % 100 / 10 == 1 ? "TH" : place % 10 == 1 ? "ST" : place % 10 == 2 ? "ND"
             : place % 10 == 3 ? "RD" : "TH");
    return text;
}

static const char *dnf_name(const char *reason)
{
    return !strcmp(reason, "time_up") ? "TIME UP" : !strcmp(reason, "gave_up") ? "GAVE UP" : !strcmp(reason, "left") ? "LEFT" : "";
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

static void change_setting(int f, int by)
{
    if (f == RF_TRACK) {
        do room_track = (room_track + by + COUNT(tracks)) % COUNT(tracks); while (arena(tracks[room_track].index));
        send_settings(tracks[room_track].index, -1, -1);
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

static void toggle_ready(const RoomNow *r)
{
    if (r->own_ready) {
        host_service_net_set_ready(0);
        menu_play("S_BACK");
    } else if (!selected) {
        choose_rider();
    } else {
        host_service_net_set_ready(1);
        menu_play("S_SELECT");
    }
}

static void copy_code(void)
{
    copied_ok = room.code[0] && host_service_clipboard_set(GADDR(room.code));
    copied_at = host_seconds();
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
        send_settings(tracks[room_track].index, -1, -1);
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

static void activate(MenuPage *p, int f, const RoomNow *r)
{
    if (!focusable(f, r)) {
        menu_play("S_CANCEL");
        return;
    }
    switch (f) {
    case RF_TRACK: open_track_list(r); break;
    case RF_BOTS: case RF_CHEATS: case RF_PUBLIC: change_setting(f, 1); break;
    case RF_RULES: rules_open = 1, rules_cursor = 0, menu_play("S_SELECT"); break;
    case RF_RIDER: choose_rider(); break;
    case RF_READY: toggle_ready(r); break;
    case RF_START: host_service_net_start_race(); menu_play("S_ENTER_GAME"); break;
    case RF_LEAVE: open_leave(r, "Leave"); break;
    }
    (void)p;
}

/* Up and down in the settings, across the bar; up from the bar to the last setting. */
static void move(int dx, int dy, const RoomNow *r)
{
    int was = rf;
    if (rf <= RF_RULES) {
        if (dy < 0 && rf > RF_TRACK && r->host) rf--;
        else if (dy > 0 && rf < RF_RULES) rf++;
        else if (dy > 0) rf = focusable(rf_bar, r) ? rf_bar : RF_LEAVE;
    } else if (dx) {
        do rf = RF_RIDER + (rf - RF_RIDER + dx + 4) % 4; while (!focusable(rf, r));
    } else if (dy < 0 && r->in)
        rf = RF_RULES;
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
        int n = COUNT(tracks), to = track_cursor + dx + dy * TRACK_COLUMNS;
        if ((dx || dy) && to >= 0 && to < n) track_cursor = to, menu_page_sound(dx > 0 || dy > 0);
        if (pressed & PAD_CROSS) close_track_list(1);
        else if (pressed & PAD_TRIANGLE) close_track_list(0);
        return;
    }
    if (!focusable(rf, r)) rf = focusable(rf_bar, r) ? rf_bar : focusable(RF_READY, r) ? RF_READY : RF_LEAVE;
    if (dx && rf <= RF_PUBLIC) change_setting(rf, dx);
    else if (dx || dy) move(dx, dy, r);
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
        for (int i = 0; i < COUNT(tracks); i++)
            if (inside(&track_hits[i], x, y)) {
                if (moved && track_cursor != i) track_cursor = i, menu_page_sound(1);
                if (left) track_cursor = i, close_track_list(1);
            }
        if (left && inside(&track_back, x, y)) close_track_list(0);
        for (; wheel > 0 && track_cursor > 0; wheel--) track_cursor--;
        for (; wheel < 0 && track_cursor < COUNT(tracks) - 1; wheel++) track_cursor++;
        return;
    }
    if (left && inside(&code_hit, x, y)) {
        copy_code();
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
            if (f == RF_BOTS || f == RF_CHEATS || f == RF_PUBLIC) change_setting(f, 1);
            else activate(p, f, &r);
        }
    }
    if (wheel && rf <= RF_PUBLIC) change_setting(rf, wheel > 0 ? -1 : 1);
    else if (wheel) move(wheel > 0 ? -1 : 1, 0, &r);
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
        while (k < seen_count && seen_id[k] != m->id) k++;
        if (k == seen_count) {
            snprintf(line, sizeof line, "%.12s JOINED", m->name);
            note(line, NULL);
            menu_play("S_SELECT");
        } else if (strcmp(m->race_state, seen_race[k]) && !strcmp(m->race_state, "finished")) {
            snprintf(line, sizeof line, "%.12s FINISHED %s", m->name, ordinal(m->place));
            note(line, green);
            menu_play("S_NEXT");
        } else if (strcmp(m->race_state, seen_race[k]) && !strcmp(m->race_state, "dnf") && m->id != l->member_id) {
            snprintf(line, sizeof line, "%.12s IS OUT (%s)", m->name, dnf_name(m->dnf_reason));
            note(line, red);
        } else if (m->ready && !seen_ready[k] && m->id != l->member_id && now - ready_heard[i] >= 3.0) {
            ready_heard[i] = now;
            snprintf(line, sizeof line, "%.12s IS READY", m->name);
            note(line, green);
            menu_play("S_NEXT");
        }
    }
    if (seen_host >= 0 && l->host_id != seen_host)
        for (int i = 0; i < l->nmembers; i++)
            if (l->member[i].id == l->host_id) {
                snprintf(line, sizeof line, l->host_id == l->member_id ? "YOU ARE THE HOST NOW" : "%.12s IS THE HOST NOW",
                         l->member[i].name);
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
            snprintf(line, sizeof line, "%.12s LEFT", seen_name[k]);
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

/* ---- drawing ---- */

static MenuStyle st_row, st_on, st_off;
static int vw = 640, vh = 224;
static int X(int x) { return x * vw / 640; }
static int Y(int y) { return y * vh / 224; }
static MenuStyle tint(MenuStyle s, int percent, const int *c) { return menu_style(s, percent, c ? c[0] : -1, c ? c[1] : 0, c ? c[2] : 0); }
static void put(int align, int x, int y, const char *t, MenuStyle s) { menu_draw_text(align, X(x), Y(y), t, &s); }
static void box(int x, int y, int w, int h, int level) { menu_draw_box(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), level); }
static void rect(int x, int y, int w, int h, const int *c, int a) { menu_draw_rect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), c[0], c[1], c[2], a); }
static void bar(int x, int y, int w, int h) { menu_draw_bar(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y)); }
static void hit(Hit *h, int x, int y, int w, int h_) { h->x = (short)X(x), h->y = (short)Y(y), h->w = (short)(X(x + w) - X(x)), h->h = (short)(Y(y + h_) - Y(y)); }

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

static void draw_setting(int f, int y, const char *label, const char *value, const RoomNow *r)
{
    char line[64];
    int on = rf == f && !track_list && !leave_ask;
    snprintf(line, sizeof line, r->host ? "%s  < %s >" : "%s  %s", label, value);
    if (on) bar(L_X + 6, y - 1, L_W - 12, 11);
    put(MENU_CENTRE, L_X + L_W / 2, y, line, fit(line, on ? st_on : st_row, 90, L_W - 16));
    hit(&hits[f], L_X, y - 2, L_W, 13);
}

static void draw_left(const RoomNow *r)
{
    const NetLobby *l = &room;
    box(L_X, TOP, L_W, MID_H, 90);
    /* the track: a big name; the host changes it here (left/right) or from the list (Cross) */
    int cx = L_X + L_W / 2, on = rf == RF_TRACK && !track_list && !leave_ask;
    put(MENU_CENTRE, cx, TOP + 6, "TRACK", menu_style(st_off, 80, -1, 0, 0));
    if (on) bar(L_X + 6, TOP + 17, L_W - 12, 14);
    /* the host's arrows at the highlight's ends; the name fitted between them with a gap (mixed
       case draws up to a tenth wider than the capitals measure) */
    const char *track = track_name(l->track);
    MenuStyle ts = fit(track, on ? st_on : st_row, 125, r->host ? L_W - 62 : L_W - 24);
    put(MENU_CENTRE, cx, TOP + 18, track, ts);
    if (r->host) {
        put(MENU_LEFT, L_X + 10, TOP + 18, "<", ts);
        put(MENU_RIGHT, L_X + L_W - 10, TOP + 18, ">", ts);
    }
    hit(&hits[RF_TRACK], L_X, TOP + 14, L_W, 20);
    draw_setting(RF_BOTS, TOP + 39, "COMPUTER RIDERS", l->laps & ROOM_NO_BOTS ? "OFF" : "ON", r);
    draw_setting(RF_CHEATS, TOP + 52, "CHEATS", l->laps & ROOM_CHEATS ? "ON" : "OFF", r);
    draw_setting(RF_PUBLIC, TOP + 65, "ROOM", l->is_public ? "PUBLIC" : "PRIVATE", r);
    /* the room's rules in one line; Cross opens them (everyone reads them, the host changes them) */
    on = rf == RF_RULES && !track_list && !leave_ask && !rules_open;
    if (on) bar(L_X + 6, TOP + 77, L_W - 12, 11);
    put(MENU_CENTRE, cx, TOP + 78, "ROOM RULES", on ? st_on : st_row);
    /* short: the left column is narrow beside two columns of cards (AUTO is always 30 s) */
    /* the longest form that fits the panel: "HOST STARTS" -> "HOST", then "NEXT TRACK" -> "NEXT" */
    char forms[3][48];
    const char *pending = l->rules_pending ? " *" : "", *then_short = l->then == 0 ? "SAME" : l->then == 2 ? "RANDOM" : "NEXT";
    snprintf(forms[0], 48, "%s - %s - %s%s", window_name(l->finish_window), l->next_race ? "HOST STARTS" : "AUTO",
             then_name(l->then), pending);
    snprintf(forms[1], 48, "%s - %s - %s%s", window_name(l->finish_window), l->next_race ? "HOST" : "AUTO",
             then_name(l->then), pending);
    snprintf(forms[2], 48, "%s - %s - %s%s", window_name(l->finish_window), l->next_race ? "HOST" : "AUTO", then_short, pending);
    const char *const texts[] = {forms[0], forms[1], forms[2], NULL};
    const char *rules = first_fit(texts, st_off, 70, L_W - 16);
    put(MENU_CENTRE, cx, TOP + 89, rules, fit(rules, st_off, 70, L_W - 16));
    hit(&hits[RF_RULES], L_X, TOP + 75, L_W, 24);
    rect(L_X + 8, TOP + 99, L_W - 16, 1, (const int[]){0, 0, 0}, 128);
    /* the join code; Circle (or a click) copies it */
    int copied = copied_at > 0 && host_seconds() - copied_at < 1.5;
    put(MENU_LEFT, L_X + 10, TOP + 105, copied ? (copied_ok ? "COPIED" : "FAILED") : "CODE",
         !copied ? menu_style(st_off, 80, -1, 0, 0) : copied_ok ? tint(st_row, 80, green) : tint(st_row, 80, red));
    put(MENU_RIGHT, L_X + L_W - 10, TOP + 102, l->code[0] ? l->code : "------", menu_style(st_on, 120, -1, 0, 0));
    hit(&code_hit, L_X, TOP + 100, L_W, 17);
}

static void draw_bars(int x, int y, int rtt)
{
    static const int lilac[3] = {0x70, 0x68, 0x98}, unlit[3] = {0x30, 0x30, 0x30};
    int level = rtt < 0 ? 0 : rtt < 80 ? 3 : rtt < 150 ? 2 : 1;
    for (int i = 0; i < 3; i++) {
        int h = 3 + 2 * i;
        rect(x + i * 3, y - h, 2, h, i >= level ? unlit : level == 1 ? red : lilac, 128);
    }
}

static void draw_card(int i)
{
    const NetLobby *l = &room;
    int x = CARD_X + (i / 5) * 188, y = CARD_Y + (i % 5) * 22, w = 180;
    char line[64], detail[40], ping[20];
    box(x, y, w, 20, i < l->nmembers ? 90 : 60);
    if (i >= l->nmembers) {
        snprintf(line, sizeof line, "%d  %s", i + 1, i < l->max_players ? "OPEN" : "");
        put(MENU_LEFT, x + 6, y + 6, line, st_off);
        return;
    }
    const NetMember *m = &l->member[i];
    RoomNow now = room_now();
    const char *tag = m->choosing ? "CHOOSING" : m->ready ? "READY" : "WAIT";
    if (now.racing && !strcmp(m->race_state, "racing")) tag = "RACING";
    else if (now.racing && !strcmp(m->race_state, "finished")) tag = "DONE";
    else if (now.racing && !strcmp(m->race_state, "dnf")) tag = "OUT";
    else if (now.starting && m->ready) tag = "STARTING";
    snprintf(line, sizeof line, "%d %.9s%s", i + 1, m->name, m->id == l->host_id ? " HOST" : m->id == l->member_id ? " YOU" : "");
    for (char *c = line; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    rect(x, y + 1, 2, 18, m->ready ? green : amber, 128);
    put(MENU_LEFT, x + 5, y + 2, line, fit(line, st_row, 85, 112));
    put(MENU_RIGHT, x + w - 5, y + 2, tag, menu_style(st_on, 70, -1, 0, 0));
    detail[0] = 0;
    if (now.racing && !strcmp(m->race_state, "finished")) {
        race_time(line, sizeof line, m->finish_ticks);
        snprintf(detail, sizeof detail, "%s %s", ordinal(m->place), line);
    } else if (now.racing && !strcmp(m->race_state, "dnf"))
        snprintf(detail, sizeof detail, "DNF %s", dnf_name(m->dnf_reason));
    else if (!now.racing) {
        for (int k = 0; k < l->nresults; k++) if (l->result[k].id == m->id) {
            const NetResult *last = &l->result[k];
            if (last->ticks >= 0) {
                race_time(line, sizeof line, last->ticks);
                snprintf(detail, sizeof detail, "%s %s", ordinal(last->place), line);
            } else snprintf(detail, sizeof detail, "DNF %s", dnf_name(last->dnf_reason));
        }
    }
    if (!detail[0]) snprintf(detail, sizeof detail, "%s", m->choosing ? "..." : m->rider >= 1 && m->rider <= 16 ? rider_names[m->rider-1] : "NO RIDER");
    put(MENU_LEFT, x + 5, y + 11, detail, fit(detail, st_off, 75, 115));
    if (m->rtt_ms >= 0) snprintf(ping, sizeof ping, "%d MS", m->rtt_ms);
    else snprintf(ping, sizeof ping, "-");
    MenuStyle ps = menu_style(st_row, 70, -1, 0, 0);
    put(MENU_RIGHT, x + w - 5, y + 11, ping, ps);
    draw_bars(x + w - 18 - menu_text_width(ping, &ps) * 640 / vw, y + 18, m->rtt_ms);
}

static void draw_bar_cell(int f, int k, const char *label, const char *why, const RoomNow *r, int green_cell)
{
    int w = BAR_W / 4, x = BAR_X + k * w, on = rf == f && !track_list && !leave_ask, can = focusable(f, r);
    if (k) rect(x, BAR_Y + 1, 1, BAR_H - 2, (const int[]){0, 0, 0}, 128);
    if (on) bar(x + 3, BAR_Y + 3, w - 6, BAR_H - 6);
    if (green_cell) rect(x + (on ? 6 : 3), BAR_Y + (on ? 5 : 3), w - (on ? 12 : 6), BAR_H - (on ? 10 : 6), green, 128);
    MenuStyle s = on && !green_cell ? st_on : can ? st_row : st_off;
    if (why) {
        put(MENU_CENTRE, x + w / 2, BAR_Y + 2, label, s);
        put(MENU_CENTRE, x + w / 2, BAR_Y + 11, why, tint(st_row, 65, amber));
    } else
        put(MENU_CENTRE, x + w / 2, BAR_Y + 5, label, fit(label, s, 100, w - 10));
    hit(&hits[f], x, BAR_Y, w, BAR_H);
}

static void draw_bar(const RoomNow *r)
{
    const NetLobby *l = &room;
    char why[24];
    box(BAR_X, BAR_Y, BAR_W, BAR_H, 90);
    draw_bar_cell(RF_RIDER, 0, selected ? "CHANGE RIDER" : "CHOOSE RIDER", NULL, r, 0);
    draw_bar_cell(RF_READY, 1, "READY", NULL, r, r->own_ready);
    if (!r->host) draw_bar_cell(RF_START, 2, "HOST STARTS", NULL, r, 0);
    else if (r->racing) draw_bar_cell(RF_START, 2, "START", "RACE LIVE", r, 0);
    else if (r->starting) draw_bar_cell(RF_START, 2, "STARTING", NULL, r, 0);
    else if (r->ready == 0) draw_bar_cell(RF_START, 2, "START", "NOBODY READY", r, 0);
    else if (r->ready < l->nmembers) {
        snprintf(why, sizeof why, "%d NOT READY", l->nmembers - r->ready);
        draw_bar_cell(RF_START, 2, "START NOW", why, r, 0);
    } else
        draw_bar_cell(RF_START, 2, l->last_race_id ? "START NOW" : "START THE RACE", NULL, r, 0);
    draw_bar_cell(RF_LEAVE, 3, "LEAVE", NULL, r, 0);
}

/* One line: what happens now and what you can do. */
static void draw_status(const RoomNow *r)
{
    const NetLobby *l = &room;
    char line[96];
    const int *c = NULL;
    int waiting = 0, choosing_n = 0, who = -1;
    for (int i = 0; i < l->nmembers; i++) {
        if (l->member[i].choosing) choosing_n++, who = who < 0 ? i : who;
        else if (!l->member[i].ready) waiting++, who = who < 0 ? i : who;
    }
    int riding = -1;  /* one rider still in the race: who */
    for (int i = 0; i < l->nmembers; i++)
        if (r->riding == 1 && !strcmp(l->member[i].race_state, "racing")) riding = i;
    if (status[0]) snprintf(line, sizeof line, "%s", status);
    else if (!r->in) snprintf(line, sizeof line, "CONNECTING TO THE SERVER...");
    else if (r->starting && r->own_ready) snprintf(line, sizeof line, "YOU'RE IN THIS RACE: UNREADY TO STAY OUT"), c = green;
    else if (r->starting) snprintf(line, sizeof line, "YOU WEREN'T READY: READY NOW TO RACE THE ONE AFTER");
    else if (r->racing && r->own_race[0]) {
        char wait[48];
        int most = seconds_to(l->finish_ends_at);
        if (riding >= 0) snprintf(wait, sizeof wait, "WAITING FOR %.12s", l->member[riding].name);
        else snprintf(wait, sizeof wait, "WAITING FOR %d RIDERS", r->riding);
        if (!strcmp(r->own_race, "finished"))
            snprintf(line, sizeof line, most ? "YOU FINISHED %s: %s, %d S AT MOST" : "YOU FINISHED %s: %s",
                     ordinal(l->member[r->self].place), wait, most), c = green;
        else snprintf(line, sizeof line, "YOU'RE OUT OF THIS RACE: %s", wait);
    } else if (r->racing && r->own_ready) snprintf(line, sizeof line, "RACE LIVE: YOU'RE READY FOR THE NEXT ONE"), c = green;
    else if (r->racing) snprintf(line, sizeof line, selected ? "RACE LIVE: PRESS READY TO RACE THE NEXT ONE"
                                                             : "RACE LIVE: CHOOSE YOUR RIDER FOR THE NEXT ONE");
    else if (!selected) snprintf(line, sizeof line, "CHOOSE YOUR RIDER FIRST");
    else if (r->intermission)
        snprintf(line, sizeof line, r->own_ready ? "NEXT RACE ON %s: ONLY READY RIDERS START" : "NEXT RACE ON %s: PRESS READY TO RACE IT",
                 track_name(l->track)), c = r->own_ready ? green : NULL;
    else if (l->waiting_for_rider) snprintf(line, sizeof line, "WAITING FOR A RIDER: THE FIRST READY STARTS A NEW 30 S");
    else if (l->last_race_id && l->next_race && !r->host)
        snprintf(line, sizeof line, r->own_ready ? "WAITING FOR THE HOST TO START THE NEXT RACE" : "PRESS READY TO RACE THE NEXT ONE");
    else if (l->last_race_id && l->next_race && r->ready)
        snprintf(line, sizeof line, "START NOW WHEN YOU LIKE: ONLY READY RIDERS RACE");
    else if (l->nmembers < 2) snprintf(line, sizeof line, "WAITING FOR PLAYERS: SHARE THE CODE %s", l->code);
    else if (!r->own_ready) snprintf(line, sizeof line, "PRESS READY WHEN YOU LIKE YOUR RIDER");
    else if (waiting + choosing_n == 1 && who >= 0)
        snprintf(line, sizeof line, l->member[who].choosing ? "%.12s IS CHOOSING A RIDER" : "WAITING FOR %.12s", l->member[who].name);
    else snprintf(line, sizeof line, "WAITING FOR %d PLAYERS TO GET READY", waiting + choosing_n);
    for (char *k = line; *k; k++) if (*k >= 'a' && *k <= 'z') *k -= 32;
    put(MENU_CENTRE, 320, STATUS_Y, line, fit(line, c ? tint(st_row, 100, c) : st_row, 100, 600));
}

/* The notes, newest first, top right in three rows. The middle row is beside the title (the
   game draws it, centred at TITLE_Y: about 17.5 units a letter, so 9 each side plus a gap), so a
   note goes there only if it fits beside it; otherwise into the next free row. */
static void draw_toasts(void)
{
    double now = host_seconds();
    static const int row_y[3] = {3, 16, 29};
    int used[3] = {0, 0, 0};
    int beside = 632 - 14 - (320 + (int)strlen(title_text) * 9 + 6);
    for (int i = TOASTS - 1; i >= 0; i--) {
        if (toast[i].until <= now) continue;
        MenuStyle s = toast[i].colour ? tint(st_row, 75, toast[i].colour) : menu_style(st_on, 75, -1, 0, 0);
        int row = 0;
        while (row < 3 && (used[row] || (row == 1 && !fits(toast[i].text, s, 100, beside)))) row++;
        if (row == 3) continue;  /* no room now: it shows when a row is free, while it lasts */
        used[row] = 1;
        s = fit(toast[i].text, s, 100, row == 1 ? beside : 610);
        int w = menu_text_width(toast[i].text, &s) * 640 / vw + 14;
        box(632 - w, row_y[row], w, 11, 90);
        put(MENU_RIGHT, 625, row_y[row] + 2, toast[i].text, s);
    }
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
    /* Two columns of names; the rider limit goes beside the picture, so each name has room. 13 rows
       of 10.5 units fit between the title and BACK. */
    for (int i = 0; i < COUNT(tracks); i++) {
        int x = 44 + (i % TRACK_COLUMNS) * 192, y = 45 + (i / TRACK_COLUMNS) * 21 / 2, on = i == track_cursor;
        const char *name = track_name(tracks[i].index);
        MenuStyle s = fit(name, on || i == room_track ? st_on : st_row, 90, 178);
        if (on) bar(x + 2, y - 1, 186, 10);
        put(MENU_CENTRE, x + 95, y, name, s);
        if (i == room_track) {  /* the current one: underlined under its name's own width, below the letters */
            int u = menu_text_width(name, &s) * 640 / vw;
            rect(x + 95 - u / 2, y + 9, u, 1, (const int[]){0xC0, 0xB8, 0x40}, 128);
        }
        hit(&track_hits[i], x, y - 1, 190, 10);
    }
    int at = tracks[track_cursor].index;
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

/* The title and the footer, which the game draws: before its update. */
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
    } else
        snprintf(title_text, sizeof title_text, "RACE ROOM");
    p->title = title_text;
    if (rules_open)
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
    /* the countdown: under the title counting, a bar runs out */
    {
        double left = 0, whole = 1;
        const int *c = green;
        if (l->countdown_ms >= 0) left = l->countdown_ms, whole = 5000;
        else if (r->intermission) left = l->next_race_at - host_service_net_server_ms(), whole = 30000, c = lilac;
        else if (r->racing && l->finish_ends_at && l->finish_window)
            left = l->finish_ends_at - host_service_net_server_ms(), whole = l->finish_window * 1000.0, c = blue;
        if (left > 0) {
            rect(L_X, 33, BAR_W, 2, (const int[]){0, 0, 0}, 128);
            rect(L_X, 33, (int)(BAR_W * (left > whole ? 1 : left / whole)), 2, c, 128);
        }
    }
    /* The game draws all text over every box: a list or a question over the room would show the
       room's text through it, so it is drawn alone. */
    if (track_list || leave_ask || rules_open) {
        if (track_list) draw_track_list();
        else if (rules_open) draw_rules(r);
        else draw_leave(r);
        return;
    }
    draw_left(r);
    {
        char head[64];
        int all = l->nmembers >= 2 && r->ready == l->nmembers;
        if (r->racing) {
            put(MENU_LEFT, CARD_X + 3, TOP + 1, "RACE RESULTS - LIVE", menu_style(st_row, 80, -1, 0, 0));
            snprintf(head, sizeof head, "%d FINISHED - %d RACING", r->finished, r->riding);
            put(MENU_RIGHT, CARD_X + CARD_W - 3, TOP + 1, head, tint(st_row, 80, blue));
        } else {
            /* the ready count on the right; on the left the longest form that fits beside it */
            MenuStyle st = menu_style(st_row, 80, -1, 0, 0);
            snprintf(head, sizeof head, "%d/%d READY", r->ready, l->nmembers);
            put(MENU_RIGHT, CARD_X + CARD_W - 3, TOP + 1, head, all ? tint(st_row, 80, green) : st);
            /* the left text's room, less a quarter: mixed-case track names draw wider than
               menu_text_width measures them (as capitals); a tenth was not enough (finding 132) */
            int room_w = (CARD_W - 6 - menu_text_width(head, &st) * 640 / vw - 10) * 3 / 4;
            char forms[4][64];
            const char *what = l->last_race_id ? track_name(l->last_race_track) : NULL, *region = l->room_region;
            char players[16];
            snprintf(players, sizeof players, "PLAYERS %d/%d", l->nmembers, l->max_players);
            /* before a race: the players and where the room runs (finding 135), "- EU SERVER", then
               "- EU", then left out; after a race: the last race's track only (the region is in the
               list and was in this header before the race) */
            if (what) {
                snprintf(forms[0], 64, "LAST RACE - %s", what);
                snprintf(forms[1], 64, "%s", what);
                forms[2][0] = forms[3][0] = 0;
            } else {
                snprintf(forms[0], 64, "%s%s%s%s", players, region[0] ? " - " : "", region, region[0] ? " SERVER" : "");
                snprintf(forms[1], 64, "%s%s%s", players, region[0] ? " - " : "", region);
                snprintf(forms[2], 64, "%s", players);
                forms[3][0] = 0;
            }
            const char *const texts[] = {forms[0], forms[1], forms[2][0] ? forms[2] : NULL, forms[3][0] ? forms[3] : NULL, NULL};
            const char *left = first_fit(texts, st, 80, room_w);
            put(MENU_LEFT, CARD_X + 3, TOP + 1, left, fit(left, st_row, 80, room_w));
        }
    }
    for (int i = 0; i < MEMBER_ROWS; i++) draw_card(i);
    draw_bar(r);
    draw_status(r);
    draw_toasts();
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

static void room_open(void)
{
    rf = rf_bar = selected ? RF_READY : RF_RIDER;
    track_list = leave_ask = rules_open = room_closed = 0;
    seen_host = seen_track = -1;
    memset(seen_race, 0, sizeof seen_race);
    room_buttons = 0xFFFF, repeat_buttons = repeat_frames = 0;
    seen_count = -1, countdown_shown = -1;
    memset(toast, 0, sizeof toast);
    memset(ready_heard, 0, sizeof ready_heard);
    page.title_y = TITLE_Y;
    page.mouse = room_mouse;
    page.draw = room_draw;
}

static void room_update(MenuPage *p)
{
    NetLobby *l = &room;
    host_service_net_get_lobby(GADDR(l));
    if (l->state == NET_FAILED || l->state == NET_IDLE) {
        refused(l);
        if (in_shell) {  /* the rider screen's Back, then ONLINE again with the reason */
            race = 0;
            menu_page_close(p);
            return;
        }
        screen = HOME;
        menu_page_rebuild_at(p, home_row);
        return;
    }
    int in = l->state == NET_LOBBY || l->state == NET_RACING;
    if (in) room_sync(l);
    if (in)
        for (int k = 0; k < COUNT(tracks); k++)
            if (tracks[k].index == l->track) room_track = k;
    if (l->started || l->state == NET_RACING) {
        hle_log("online menu: room %s: the race starts\n", l->code);
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
            hle_log("online menu: room %s: cheats %s\n", l->code, l->laps & ROOM_CHEATS ? "ON" : "OFF");
        seen_cheats = l->laps & ROOM_CHEATS;
        room_options = l->laps, public_room = l->is_public;
    }
    room_news();
    room_countdown_sounds();
    room_input(p, &r);
    /* tests (DHD_NET_SCRIPTED with DHD_NET_TEST_READY): ready again for the next race, as a player
       who presses READY in the room after each one, so a scripted run races on (at most 1 a second) */
    static int test_ready = -1;
    static double test_ready_at;
    if (test_ready < 0) test_ready = getenv("DHD_NET_TEST_READY") && getenv("DHD_NET_SCRIPTED");
    if (test_ready && in && selected && !r.own_ready && !r.riding && !leave_ask && !room_closed &&
        host_seconds() - test_ready_at > 1) {
        test_ready_at = host_seconds();
        host_service_net_set_ready(1);
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
        if (!status[0] && (cursor == 1 || cursor >= 3 || disc_shown) && not_ready()) p->footer = not_ready();
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
            int action = host_service_text_input_edit(GADDR(entry), (screen == NAME ? NAME_MAX : CODE_LENGTH) + 1, GADDR(screen == NAME ? name_letters : code_letters));
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
        if (row == 0) {
            snprintf(entry, sizeof entry, "%s", name);
            entry_pick = 0;
            go(NAME);
        } else if (row == 1 || row == 2) {
            const char *why = row == 1 ? not_ready() : !server() ? "No server: set server = ... in dhd.ini" : NULL;
            if (why) snprintf(status, sizeof status, "%s", why);
            else {
                start_list();
                go(row == 1 ? QUICK : LIST);
            }
        } else if (row == 3 || row == 4)
            enter_room(row == 3 ? 2 : 1, NULL);
        else if (row == 5) {
            entry[0] = 0;
            entry_pick = 0;
            go(CODE);
        } else if (row == 6 && outdated) {
            hle_log("online menu: Update Now\n");
            host_service_update_request();  /* ends the game; the launcher updates it */
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
        int max = screen == NAME ? NAME_MAX : CODE_LENGTH;
        size_t n = strlen(entry);
        if (row == 0) {
            if ((int)n < max) entry[n] = letters[entry_pick], entry[n + 1] = 0;
            if (screen == CODE && (int)n + 1 == max) menu_page_set_cursor(p, 1);  /* the code is whole: Join */
        } else if (screen == NAME) {
            while (n && entry[n - 1] == ' ') entry[--n] = 0;
            if (n) {
                snprintf(name, sizeof name, "%s", entry);
                save("name", "DHD_NET_NAME", name);
            }
            go(HOME);
        } else if ((int)n == CODE_LENGTH)
            enter_room(0, entry);
        else
            snprintf(status, sizeof status, "A code has %d letters and digits", CODE_LENGTH);
        break;
    }
    case QUICK:
        if (list_state == 2 && row == 1) enter_room(2, NULL);
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
    if ((screen == NAME || screen == CODE) && row == 0) {
        int n = screen == NAME ? (int)sizeof name_letters - 1 : (int)sizeof code_letters - 1;
        entry_pick = (entry_pick + by + n) % n;
        menu_page_sound(by > 0);
    }
    (void)p;
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
   without online play (Android and the browser: host_service_net_offered, finding 143) shows no
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
   - the rider screen's Back (Triangle) opens the room instead of leaving for the title;
   - Change Selection closes the room on the rider screen itself;
   - leaving the room (or losing it) takes the rider screen's own Back to the title, where the
     main menu opens ONLINE again (port_online_resume). */
int port_online_shell(void)
{
    if (!choosing) return 0;
    int menu = SHELL_WORD(0x29DED8);
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
    for (int k = 0; k < COUNT(tracks); k++)
        if (tracks[k].index == index) return track_name(tracks[k].index);
    return NULL;
}
