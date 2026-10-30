/* Where the controllers' input comes from, and online races (findings 041, 044).

   pad.c asks pads_read for player i's pad once per field. Offline that is a script (DHD_NAV,
   DHD_RACE_PAD<n>) or the host's controller i; DHD_PADS (1-4) plugs that many in, 3-4 through
   a multitap on port 0 (finding 040).

   Online (DHD_NET_CREATE=1 or DHD_NET_JOIN=code, with DHD_NET_SERVER) every PC runs a normal
   one-player race and the riders' states travel (state sync, finding 044, port/hle/puppet.c):
   - Boot: the PC waits in the lobby (the barrier) until the host starts. Then the built-in menu
     script goes to a single race; on the load screen (menu 9) the lobby's track and this
     player's rider go into the setup block D_004B25A0, its seed into DHD_RACE_SEED, and the PCs
     tell each other what their players ride, so each puppet wears its player's rider and bike
     (hle_online_riders, after the game chose the AI riders).
   - At the countdown every PC says it has loaded; then the host names a moment on the server's
     clock and every PC starts the countdown then, so the GOs land together (the frame clock
     slips by the wait, kernel_pace_slip).
   - In the race this PC's controller rides rider 0, from the grid place the host has for this
     player. About 20 times a second it sends a snapshot of the riders it runs (its own; the host
     also the AI riders) as STATE frames; the other humans, and on the other PCs the AI, are
     puppets in AI rider slots (each PC at its player's frame rate, 60-240: snapshots carry race
     time, and every count here is of race ticks, 1/60 s fields, or wall time, never of frames;
     finding 114). A hit this PC's rider deals to a puppet goes to the puppet's PC as
     an EVENT frame, which applies it.
   - The finish: each PC reports its rider's time, the host publishes the order once everyone has
     finished, and every PC's results screen waits for it. A player who leaves becomes the host's
     AI rider (findings 044, 047). */
#include "hle.h"
#include "menu_page.h"
#include "../gs/present.h"
#include "../net/net.h"
#include "../include/version.h"
#include "../include/fps.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EE32(a) (*(volatile int32_t *)(uintptr_t)(0x08000000u + (a)))
#define EE_PTR(a) ((void *)(uintptr_t)(0x08000000u + (a)))
enum { MENU = 0x29DED8, PHASE = 0x43F348, SETUP = 0x4B25A0, LEVEL_NAME = 0x4BD4DC, TRACK_NAMES = 0x26EE50 };
enum { LOAD_MENU = 9, FLYBY_PHASE = 5 };  /* on the race screen: 5 the flight over the track, 1 countdown, 2-4 race */
/* Retail func_0016B5A8: the flyby (5) is not a running race. */
static int race_running(void) { return EE32(PHASE) >= 2 && EE32(PHASE) <= 4; }

/* The race screen's menus: D_0029E290 is 1 while riding, 0 in a menu; D_003F52D0.x150 the open
   menu (a host pointer), each menu's first word its row. The pause menu's rows: RESUME, OPTIONS,
   TRICK LIST, NEW LEVEL, RE-START, MUSIC PLAYLIST, QUIT; RE-START and QUIT ask first ("No,
   Return" / "Yes"), NEW LEVEL opens the level list. */
enum { RIDING = 0x29E290, OPEN_MENU = 0x3F5420, PAUSE_MENU = 0x38BC00, RESTART_ASK = 0x388990,
       LEVEL_LIST = 0x386680, RESULTS_MENU = 0x386B90, PAUSE_NEW_LEVEL = 3, PAUSE_RESTART = 4 };

/* ---- the pads ---- */

static int npads = -1, online = -1;
static int selected_ride;
static int32_t selected_rider, selected_outfit[3], selected_bike[3];
void hle_online_selected_ride(int rider, const int32_t *outfit, const int32_t *bike)
{
    selected_ride = 1;
    selected_rider = rider;
    memcpy(selected_outfit, outfit, sizeof selected_outfit);
    memcpy(selected_bike, bike, sizeof selected_bike);
}

static int from_menu;  /* the ONLINE menu (online_menu.c) made the connection and the room */

static int online_mode(void)
{
    if (online < 0)
        online = getenv("DHD_NET_CREATE") != NULL || getenv("DHD_NET_JOIN") != NULL;
    return online;
}

int pads_online(void) { return online_mode(); }

/* The ONLINE menu's room started its race: from the next field this is an online race as with
   DHD_NET_*, past the connection and the lobby, which the menu did (settings, pick, ready). The
   main menu is on SINGLE PLAYER, where race_script's "15:cross" goes on. */
void hle_online_from_menu(void)
{
    online = 1;
    from_menu = 1;
}

/* Played by hand, the game finds a multitap with four pads at boot, as it only looks once, and
   each pad reads as connected while that player has a controller (pad.c), so the 2-4 player
   modes take controllers plugged in at any time (issue #7: with one pad the game said "TWO
   CONTROLLERS MUST BE CONNECTED" whatever SDL had found). Tools, scripts and DHD_NET_* races keep
   DHD_PADS (default 1), whose pads are always connected. */
int pads_live(void)
{
    static int live = -1;
    if (live < 0) {
        static const char *const scripted[] = {"DHD_PADS", "DHD_NAV", "DHD_AUTOPAD", "DHD_AUTOPAD_FRAMES",
                                               "DHD_RACE_PAD", "DHD_RACE_PAD1", "DHD_RACE_PAD2",
                                               "DHD_RACE_PAD3", "DHD_RACE_SYNC", "DHD_KEYS"};
        live = !online_mode();
        for (size_t i = 0; i < sizeof scripted / sizeof *scripted; i++)
            if (getenv(scripted[i])) live = 0;
    }
    return live;
}

int pads_connected(void)
{
    if (npads < 0) {
        const char *s = getenv("DHD_PADS");
        npads = online_mode() ? 1 : pads_live() ? 4 : s && *s ? atoi(s) : 1;
        if (npads < 1) npads = 1;
        if (npads > 4) npads = 4;
    }
    return npads;
}

int pads_multitap(void) { return pads_connected() > 2; }

/* The DualShock 2's pressure bytes follow the buttons (pressed = 255), so every PC derives the
   same ones from the buttons that travel. */
static const uint16_t pressure_order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                            PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
static void neutral(HostPad *out)
{
    memset(out, 0, sizeof *out);
    out->right_x = out->right_y = out->left_x = out->left_y = 128;
}

/* ---- the race setup (DHD_RACE_SETUP offline, the lobby online) ---- */

/* The level files of each track with 2-4 players (func_001BA3B8); NULL: no multiplayer level. */
static const char *const mp_level[26] = {
    "ALP2", "MOAB2", "JUNGLE2", "PERU2", "PERUTDH2", "GLAC2P", "ALPX2", "MOABMX2", "JUNGLEX2",
    "PERUMX2", "JUNGLET2", "AUBERST2", "FUJI2", "MOABTDH2", "GLACX2P", "AUBERMX2", "AUBTDH2", NULL,
    NULL, "BC2", "CITY2", NULL, "GLACT2P", "MOSH2", "ALPT2", "BCTDX2"};

typedef struct {
    int track;          /* -1: as the menus left it */
    int rider[4];       /* rider index 0-14 (id 0x20 * (index + 1)), -1: as the menus left it */
} RaceSetup;

/* Writes the setup block on the load screen, before the shell hands it to the race build: the
   track and its level name (as func_001BA3B8 would), and the riders. */
static void write_setup(const RaceSetup *s)
{
    int humans = EE32(SETUP + 0x288);
    if (s->track >= 0 && s->track < 26) {
        const char *name = humans >= 2 ? mp_level[s->track] : *(const char **)EE_PTR(TRACK_NAMES + 4 * s->track);
        if (name) {
            EE32(SETUP + 0x2A0) = s->track;
            strcpy((char *)EE_PTR(LEVEL_NAME), name);
        } else
            fprintf(stderr, "online: track %d has no %d-player level; keeping track %d\n", s->track, humans,
                    EE32(SETUP + 0x2A0));
    }
    for (int i = 0; i < 4 && i < humans; i++)
        if (s->rider[i] >= 0 && s->rider[i] < 16) {
            EE32(SETUP + 0x290 + 4 * i) = 0x20 * (s->rider[i] + 1);
            EE32(SETUP + 0x1B8 + 12 * i) = s->rider[i];  /* rider, outfit, ...: as the rider screen sets it */
            for (int k = 0; k < 16; k++) EE32(SETUP + 0x2A8 + 4 * k) = -1;  /* func_001B74E8 fills it */
        }
    fprintf(stderr, "online: race setup: %d players, track %d (%s), riders %X %X %X %X\n", humans,
            EE32(SETUP + 0x2A0), (const char *)EE_PTR(LEVEL_NAME), EE32(SETUP + 0x290), EE32(SETUP + 0x294),
            EE32(SETUP + 0x298), EE32(SETUP + 0x29C));
}

static void parse_setup(const char *text, RaceSetup *s)
{
    char *end;
    s->track = -1;
    for (int i = 0; i < 4; i++) s->rider[i] = -1;
    if (!text || !*text) return;
    s->track = (int)strtol(text, &end, 0);
    for (int i = 0; i < 4 && *end == ','; i++) s->rider[i] = (int)strtol(end + 1, &end, 0);
}

/* ---- state hashes ---- */

/* The game's writable static area (.data, .sdata, .sbss, .bss: 0x0026DD00-0x008A0400, which
   holds the rider world D_002AF2C0, the setup block, the RNG states and the pads), finding 040.
   Words that point into the EE threads' host stacks count as 0: the game keeps pointers to stack
   locals, whose values depend on where Windows placed the stacks (finding 041). */
static uint32_t *copy;
static const void *state_hash_copy(void) { return copy; }

static uint64_t state_hash(int whole)
{
    static uint32_t stacks[64][2];
    uint32_t start = whole ? 0 : 0x26DD00, end = whole ? 32u << 20 : 0x8A0400, n = (end - start) / 4;
    const uint32_t *ee = EE_PTR(start);
    int nstacks = host_stack_ranges_copy(GADDR(stacks), 64);
    uint32_t low = UINT32_MAX, high = 0;
    for (int k = 0; k < nstacks; k++) {
        if (stacks[k][0] < low) low = stacks[k][0];
        if (stacks[k][1] > high) high = stacks[k][1];
    }
    if (!copy && !(copy = malloc(32u << 20))) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = ee[i];
        if (v >= low && v < high)
            for (int k = 0; k < nstacks; k++)
            if (v >= stacks[k][0] && v < stacks[k][1]) { v = 0; break; }
        copy[i] = v;
    }
    NetRange r = {copy, (size_t)n * 4};
    return host_service_net_hash_ranges(GADDR(&r), 1);
}

/* The state that decides the race, hashed every NET_HASH_PERIOD fields for DHD_HASH_LOG: the
   rider world D_002AF2C0 (10 riders, the AI and human controllers), the four random generators
   (libc rand's state behind D_0028191C, the twister D_00663140, the particle and weather LCGs,
   D_0029E2C0), the menu, race phase and race clock, and the pads as the game read them. The rest
   of the static area isn't comparable across PCs: the game copies uninitialised host stack
   (stale host pointers, which ASLR moves) into some of it (finding 041). */
static uint64_t game_hash(void)
{
    NetRange r[] = {
        {EE_PTR(0x2AF2C0), 0x501C8},  /* rider world */
        {EE_PTR(0x663140), 0x9C8},    /* twister */
        {(const char *)*(void **)EE_PTR(0x28191C) + 0x58, 8},  /* rand */
        {EE_PTR(0x29E2C0), 4}, {EE_PTR(0x29DAF8), 4}, {EE_PTR(0x29D6D0), 4},
        {EE_PTR(MENU), 4}, {EE_PTR(PHASE), 4}, {EE_PTR(0x43EEC8), 4},
        {EE_PTR(0x660B80), 8 * 32},   /* pads */
    };
    return host_service_net_hash_ranges(GADDR(r), (int)(sizeof r / sizeof *r));
}

/* DHD_HASH_LOG=file: offline, the hashes of every NET_HASH_PERIOD-th vsync from boot, for
   comparing two runs (E4). */
static void log_hash(uint64_t field)
{
    static FILE *f;
    static int tried;
    if (!tried++) {
        const char *path = getenv("DHD_HASH_LOG");
        if (path) f = fopen(path, "w");
    }
    if (!f || field % NET_HASH_PERIOD) return;
    fprintf(f, "%llu menu=%d phase=%d game=%016llx static=%016llx all=%016llx\n", (unsigned long long)field,
            EE32(MENU), EE32(PHASE), (unsigned long long)game_hash(), (unsigned long long)state_hash(0),
            (unsigned long long)state_hash(1));
    /* DHD_HASH_DUMP=field: the hashed (masked) 32 MB at that field, and the stacks masked */
    const char *dump = getenv("DHD_HASH_DUMP");
    if (dump && strtoull(dump, NULL, 10) == field) {
        static uint32_t stacks[64][2];
        int n = host_stack_ranges_copy(GADDR(stacks), 64);
        char name[64];
        snprintf(name, sizeof name, "hashdump-%llu.bin", (unsigned long long)field);
        FILE *d = fopen(name, "wb");
        if (d) fwrite(state_hash_copy(), 32u << 20, 1, d), fclose(d);
        snprintf(name, sizeof name, "rawdump-%llu.bin", (unsigned long long)field);
        if ((d = fopen(name, "wb"))) fwrite(EE_PTR(0), 32u << 20, 1, d), fclose(d);
        for (int i = 0; i < n; i++) fprintf(f, "stack %08lx-%08lx\n", (unsigned long)stacks[i][0], (unsigned long)stacks[i][1]);
    }
    fflush(f);
}

/* ---- online ---- */

void hle_puppet_online(int local, int nslots, int host);
void hle_puppet_offline(void);
void hle_puppet_drop(int lobby, int host, int finished);
int hle_puppet_pack(int slot, uint8_t *out, int max);
void hle_puppet_receive(int sender, const uint8_t *in, int n);
int hle_puppet_take_event(uint8_t *out, int max);
void hle_puppet_event(int sender, const uint8_t *in, int n);
int hle_puppet_lag(char *out, int size);
int hle_puppet_hits_taken(void);
int hle_puppet_global(int slot), hle_puppet_local(int global), hle_puppet_owned(int slot);
void hle_puppet_rematch(void);
extern int func_001FA7C0(int bike);                       /* a bike's sound kind */
extern void func_001B45F8(void *timer, int kind, int ticks);  /* the race-end countdown */

/* EVENT payloads (finding 047): byte 0 the version, byte 1 the type. Type 1 (a hit) is
   port/hle/puppet.c's. */
enum {
    EVENT_VERSION = 2, EVENT_HIT = 1, EVENT_LOADED = 2, EVENT_SETUP = 3, EVENT_START = 4, EVENT_GO = 5,
    EVENT_FINISH = 6, EVENT_RESULTS = 7, EVENT_REMATCH = 8
};
enum { SNAPSHOT_TICKS = 3 };  /* a snapshot every 3 race ticks: 20 a second at any frame rate */
enum { NRIDERS = 10, START_LEAD_MS = 1500 };
#define RACE_CLOCK EE32(0x43EEC8)
/* The race object D_0043EEC0 (func_0019F210): +8 the race clock, +0x1C finish times (race ticks)
   and +0x58 finished flags by rider slot, +0x62 finishes to apply on the next step. */
#define FINISH_TIME(slot) EE32(0x43EEDC + 4 * (slot))
#define FINISHED(slot) (*(volatile uint8_t *)EE_PTR(0x43EF18 + (slot)))
#define FINISH_NOW(slot) (*(volatile uint8_t *)EE_PTR(0x43EF22 + (slot)))
#define RIDER(slot) ((uint8_t *)EE_PTR(0x2AF2C0 + 0x7BD0 * (slot)))
#define PLACE(slot) (*(int32_t *)(RIDER(slot) + 0x798C))

/* The game's riders by index (rider id = 0x20 * (index + 1)); the first ten can be picked on a
   new save, the others are unlocked by the career (func_001B74E8 never gives them to the AI). */
static const char *const rider_name[16] = {
    "Cosmo", "Mai", "Ajax", "Jonnie", "Kalolo", "T-bag", "Eric Carter", "Tara Llanes", "Drud",
    "Brian Lopes", "Rakel", "Missy Giove", "Richie Schley", "Jen", "Woody", "Kineticlops"};
static const int regular[10] = {0, 1, 2, 3, 4, 5, 8, 10, 13, 14};

/* What a player rides, as their PC's setup block has it after the menus: rider id (+0x290),
   rider/outfit (+0x1B8, 3 words), bike (+0x11C, 3 words). */
typedef struct {
    int have;
    int32_t rider, outfit[3], bike[3];
} Ride;

static struct {
    int released;               /* past the barrier */
    int setup_done, started;    /* the setup block written; past the start wait */
    RaceSetup setup;
    int nslots, local_slot, host_slot;
    char code[8];
    char name[NET_MAX_SLOTS][32];
    int pick[NET_MAX_SLOTS];    /* rider index each player rides (lobby picks, no two alike) */
    Ride ride[NET_MAX_SLOTS];   /* EVENT_SETUP from that slot */
    int loaded[NET_MAX_SLOTS];  /* EVENT_LOADED from that slot */
    int gone[NET_MAX_SLOTS];
    double go_at;               /* EVENT_START: server ms of the countdown's start, 0 until known */
    double go_server;           /* our GO on the server's clock */
    double their_go[4];         /* the others' */
    int go_sent;
    /* the start (start_step): when this PC came to it, the last resend, whether the host named the
       moment; the players it still waits for (start_waiting: the first) */
    double start_entered, start_said;
    int start_announced, start_missing, start_waiting;
    int last_sent;              /* race clock of the last snapshot sent */
    unsigned sent, received;
    /* the finish (global rider ids: humans 0..nslots-1, AI riders their slot) */
    int finish[NRIDERS];        /* race ticks, -1: not finished */
    int reported[NRIDERS];
    int results[NRIDERS], nresults, have_results;
    double first_finish;        /* host_wall_seconds of the first human finish */
    int end_held, end_done;     /* the race-end countdown, held for the results */
    int no_bots;                /* the room's computer riders off (its "laps" option 1) */
    int cheats;                 /* the room's cheat codes on (option 2; off: hle_cheat_code) */
    /* the host's next race after the results (EVENT_REMATCH): 1 TRY AGAIN, 2 NEW LEVEL on
       rematch_track; rematch_fields: 1/60 s fields (not frames) since this PC began choosing it */
    int rematch_want, rematch_track, rematch_fields;
    int restart_accepted;        /* a retail menu action, never inferred from a phase */
    double rematch_at;
    double end_done_at;
    double end_held_at;
} on;

static void title(const char *fmt, ...)
{
    static char last[300];
    char text[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (!strcmp(text, last)) return;
    strcpy(last, text);
    host_service_gs_window_title(GADDR(text));
    fprintf(stderr, "online: %s\n", text);
}

static void fail(const char *why)
{
    char text[400];
    snprintf(text, sizeof text, "Online play stopped: %s", why);
    fprintf(stderr, "online: %s\n", text);
    host_error_box("Downhill Revived online", text);
    exit(1);
}

/* The module's own hash: build_wasm.py writes the first 8 bytes of the finished module's SHA-256
   (in hex, taken with these 16 bytes as they are here) over them. Every platform runs the same
   module (wasm2c's C of it), so a Windows, Mac, Linux and Android build of one core agree; an
   exe's hash differs per platform. Not const: the bytes must stay in the module's data. */
char hle_core_id[17] = "@@DHD_CORE_ID@@@";

/* The build's identity for the lobby: players must run the same game (the module: the game's
   code, the patches and the online code). The release (version.h) is in it so the server's
   "the host runs ..." message names it; the hello also sends the release alone (clientVersion)
   for the version gate. */
static void build_version(char *out, size_t size)
{
    snprintf(out, size, "dhd-port " DHD_VERSION " %.16s", hle_core_id);
}

void hle_online_version(char *out, size_t size) { build_version(out, size); }

unsigned hle_env_epoch;  /* hle.h's hle_env */
static void set_env(const char *name, const char *value)
{
    char text[600];
    snprintf(text, sizeof text, "%s=%s", name, value);
    host_putenv(text);
    hle_env_changed();
}

/* The menus to a single race, by the screen shown (port/hle/menu_nav.h; race_compare.py's NAV):
   title, memory card dialog, single race, the track and rider screens, the load screen, and START
   skips the camera's flight over the track (race phase 5), on every PC. The flight's length
   doesn't matter: the countdown waits for everyone and starts on the host's moment. */
static const char race_script[] = "0:triangle|cross,15:cross,3:cross,16:cross,1:cross,11:cross,"
                                  "18@4C7F38=0:start,18@4C7F38=1:cross,9/5:start";

/* What the race's script and seed replaced, put back when the race is over ("" when unset). */
static char saved_nav[1200], saved_seed[32];
static void save_env(const char *name, char *out, size_t size)
{
    const char *v = getenv(name);
    snprintf(out, size, "%s", v ? v : "");
}

static void connect_lobby(void)
{
    char version[64];
    const char *server = getenv("DHD_NET_SERVER"), *name = getenv("DHD_NET_NAME");
    const char *iso = getenv("DHD_NET_ISO_SHA256"), *join = getenv("DHD_NET_JOIN");
    if (!server || !*server) fail("no server address (DHD_NET_SERVER: the launcher's server field or dhd.ini).");
    const char *render = getenv("DHD_RENDER");
    if (!render || strcmp(render, "native")) fail("online races use the native renderer (DHD_RENDER=native).");
    if (!iso || strlen(iso) != 64) fail("the disc image's hash is missing (DHD_NET_ISO_SHA256: 64 hex digits).");
    build_version(version, sizeof version);
    host_service_net_init(GADDR(server), GADDR(name && *name ? name : "Rider"), GADDR(version), GADDR(iso));
    if (join && *join) {
        if (host_service_net_join_room(GADDR(join)) < 0) fail("that join code can't be one (6 letters and digits).");
    } else
        host_service_net_create_room();
    fprintf(stderr, "online: %s, version %s\n", join && *join ? "joining" : "creating a room", version);
}

static int pressed_start(void)
{
    static int was;
    HostPad in;
    host_service_host_pad_device(0, GADDR(&in));
    int now = (in.buttons & PAD_START) != 0, edge = now && !was;
    was = now;
    return edge;
}

/* Every player rides the rider they picked in the lobby, two or more on the same one too (issue
   #17); a player without a pick, or with one not of the ten, gets a default the others don't ride.
   The same on every PC. */
static void choose_riders(const NetLobby *lobby)
{
    int taken[16] = {0};
    for (int j = 0; j < on.nslots; j++) {
        int want = lobby->racer[j].rider - 1;
        if (want >= 0 && want < 16) taken[want] = 1;
    }
    for (int j = 0; j < on.nslots; j++) {
        int want = lobby->racer[j].rider - 1, k = 0;
        snprintf(on.name[j], sizeof on.name[j], "%s", lobby->racer[j].name);
        if ((selected_ride || getenv("DHD_NET_SCRIPTED")) && want >= 0 && want < 16) {
            on.pick[j] = want;
            fprintf(stderr, "online: player %d %s rides %s (preview choice)\n", j, on.name[j], rider_name[want]);
            continue;
        }
        while (k < 10 && regular[k] != want) k++;
        if (k == 10) {  /* none or not one of the ten: a default */
            k = j % 10;
            for (int tries = 0; tries < 10 && taken[regular[k]]; tries++) k = (k + 1) % 10;
            taken[regular[k]] = 1;
        }
        on.pick[j] = regular[k];
        fprintf(stderr, "online: player %d %s rides %s\n", j, on.name[j], rider_name[on.pick[j]]);
    }
}

/* Round-trips to the relay for the log: this PC's own measure, then the server's for each
   player (-1 not measured yet). */
static void log_pings(const NetLobby *l)
{
    char line[640];
    int n = snprintf(line, sizeof line, "online: ping %d ms; players:", l->ping_ms);
    for (int i = 0; i < l->nmembers && n < (int)sizeof line - 120; i++)
        n += snprintf(line + n, sizeof line - n, "%s %s %d ms", i ? "," : "", l->member[i].name, l->member[i].rtt_ms);
    fprintf(stderr, "%s\n", line);
}

/* The barrier: at boot every PC waits in the lobby until the host starts the race. */
static void barrier(void)
{
    NetLobby lobby;
    double entered = host_wall_seconds();
    const char *want = getenv("DHD_NET_PLAYERS");
    int expected = want && *want ? atoi(want) : 0, settings_sent = from_menu, start_sent = from_menu,
        ready_sent = from_menu;
    double last_change = 0, last_beat = 0;
    unsigned seq = ~0u;
    for (;;) {
        host_service_net_get_lobby(GADDR(&lobby));
        if (lobby.state == NET_FAILED)
            fail(lobby.error[0] ? lobby.error : "the connection to the server failed.");
        if (lobby.state == NET_LOBBY || lobby.state == NET_RACING) {
            if (!ready_sent) fprintf(stderr, "online: room %s%s\n", lobby.code, lobby.is_host ? " (host)" : "");
            if (lobby.is_host && !settings_sent) {
                const char *track = getenv("DHD_NET_TRACK"), *room = getenv("DHD_NET_ROOM_NAME");
                /* DHD_NET_CHEATS=1 (tests): the room's cheats on (options bit 2) */
                NetRoomSettings s = {track && *track ? atoi(track) : 0, getenv("DHD_NET_CHEATS") ? 2 : -1,
                                     getenv("DHD_NET_PUBLIC") != NULL, 4, -1, room && *room ? room : NULL};
                host_service_net_set_room(GADDR(&s));
                settings_sent = 1;
            }
            if (!ready_sent) {
                const char *rider = getenv("DHD_NET_RIDER");  /* rider index; the lobby keeps index + 1 */
                host_service_net_pick(-1, rider && *rider ? atoi(rider) + 1 : 0, 1);
                host_service_net_set_ready(1);
                ready_sent = 1;
            }
        }
        int ready = 0;
        for (int i = 0; i < lobby.nmembers; i++) ready += lobby.member[i].ready;
        /* who is in and ready (seq also moves with every ping) */
        unsigned roster = (unsigned)lobby.nmembers << 8 | (unsigned)ready;
        if (roster != seq) seq = roster, last_change = host_service_net_seconds();
        if (host_service_net_seconds() - last_beat > 2) {  /* the log shows the barrier is alive */
            last_beat = host_service_net_seconds();
            fprintf(stderr, "online: lobby state %d, %d members, %d ready, seq %u, ping %d ms\n", lobby.state,
                    lobby.nmembers, ready, lobby.seq, lobby.ping_ms);
        }
        int all_ready = lobby.nmembers >= 2 && ready == lobby.nmembers;
        if (lobby.state == NET_RACING || lobby.started) {
            double left = lobby.start_at - host_service_net_seconds();
            title("Downhill Revived online - room %s - starting in %.1f s", lobby.code, left > 0 ? left : 0);
            if (left <= 0) break;
        } else if (lobby.state == NET_LOBBY) {
            if (lobby.is_host && all_ready && !start_sent &&
                ((expected >= 2 && lobby.nmembers >= expected) || pressed_start())) {
                /* a moment after the last change, so a player who just joined shows up first */
                if (host_service_net_seconds() - last_change > 0.5) {
                    fprintf(stderr, "online: starting the race (%d players)\n", lobby.nmembers);
                    host_service_net_start_race();
                    start_sent = 1;
                }
            }
            title("Downhill Revived online - room %s - %d player%s, %d ready - %s", lobby.code, lobby.nmembers,
                  lobby.nmembers == 1 ? "" : "s", ready,
                  !lobby.is_host ? "waiting for the host to start"
                  : lobby.nmembers < 2 ? "waiting for players to join"
                  : !all_ready ? "waiting for everyone to be ready"
                               : "press START (or Enter) to begin");
        } else
            title("Downhill Revived online - connecting...");
        if (lobby.state == NET_LOBBY && lobby.is_host) pressed_start();
        host_service_gs_pump_messages();
        host_sleep_ms(10);
    }
    kernel_pace_slip(host_wall_seconds() - entered);  /* the lobby wait isn't game time to catch up */
    on.nslots = lobby.nslots < 2 ? 2 : lobby.nslots > 4 ? 4 : lobby.nslots;
    on.local_slot = lobby.local_slot;
    on.host_slot = 0;
    for (int i = 0; i < lobby.nslots; i++)
        if (lobby.racer[i].member_id == lobby.host_id) on.host_slot = i;
    memcpy(on.code, lobby.code, sizeof on.code);
    choose_riders(&lobby);
    on.setup.track = lobby.race_track;
    on.no_bots = lobby.race_laps & 1;
    on.cheats = (lobby.race_laps & 2) != 0;
    on.setup.rider[0] = on.pick[on.local_slot];
    for (int i = 1; i < 4; i++) on.setup.rider[i] = -1;
    for (int i = 0; i < NRIDERS; i++) on.finish[i] = -1;
    char seed[16];
    save_env("DHD_NAV", saved_nav, sizeof saved_nav), save_env("DHD_RACE_SEED", saved_seed, sizeof saved_seed);
    set_env("DHD_NAV", race_script);
    snprintf(seed, sizeof seed, "%u", lobby.race_seed ? lobby.race_seed : 1u);
    set_env("DHD_RACE_SEED", seed);  /* hle_race_seed: never 0, which falls back to the RTC */
    hle_puppet_online(on.local_slot, on.nslots, on.host_slot);
    fprintf(stderr, "online: race starts: player %d of %d, track %d, seed %s%s, cheats %s\n", on.local_slot, on.nslots,
            on.setup.track, seed, on.no_bots ? ", no computer riders" : "", on.cheats ? "on" : "off");
    log_pings(&lobby);
}

static void send_event(const void *payload, int n)
{
    if (!host_service_net_send_frame(NET_FRAME_EVENT, GADDR(payload), (size_t)n))
        fprintf(stderr, "online: an event (type %d) could not be sent\n", ((const uint8_t *)payload)[1]);
}

static void put32(uint8_t *o, int32_t v) { memcpy(o, &v, 4); }
static int32_t get32(const uint8_t *i) { int32_t v; memcpy(&v, i, 4); return v; }

/* DHD_NET_RECORD=file: tests, this PC's rider's snapshots as sent (a 2-byte length, then the
   payload), for nettool --replay's bots. Keep it under build/ or a temp folder. */
static void record(const uint8_t *payload, int n)
{
    static FILE *f;
    static int opened;
    if (!opened++ && getenv("DHD_NET_RECORD")) f = fopen(getenv("DHD_NET_RECORD"), "wb");
    if (!f) return;
    uint16_t size = (uint16_t)n;
    fwrite(&size, 2, 1, f), fwrite(payload, 1, (size_t)n, f), fflush(f);
}

static void go_skew(int player)
{
    fprintf(stderr, "online: player %d went at %+.1f ms from us (GO on the server clock)\n", player,
            on.their_go[player] - on.go_server);
}

static void read_finish(int global, int time);
static void read_results(const uint8_t *in, int n);

/* The relayed frames: snapshots to the puppets, events (a hit, the start, a finish, the results). */
static void receive_frames(void)
{
    uint8_t frame[NET_FRAME_MAX];
    int n;
    while ((n = host_service_net_receive_frame(GADDR(frame), sizeof frame)) > 0) {
        int sender = frame[1];
        const uint8_t *e = frame + 2;
        if (frame[0] == NET_FRAME_STATE) {  /* the sender's riders: [size][snapshot]... */
            for (const uint8_t *end = frame + n; e < end && e + 1 + e[0] <= end; e += 1 + e[0]) {
                hle_puppet_receive(sender, e + 1, e[0]);
                on.received++;
            }
            continue;
        }
        if (frame[0] != NET_FRAME_EVENT || n < 4 || e[0] != EVENT_VERSION || sender >= NET_MAX_SLOTS) continue;
        switch (e[1]) {
        case EVENT_LOADED:
            if (!on.loaded[sender]++) fprintf(stderr, "online: player %d has loaded (at %.1f s)\n", sender, host_wall_seconds());
            break;
        case EVENT_SETUP:
            if (n - 2 >= 30) {
                Ride *r = &on.ride[sender];
                r->rider = get32(e + 2);
                for (int k = 0; k < 3; k++) r->outfit[k] = get32(e + 6 + 4 * k), r->bike[k] = get32(e + 18 + 4 * k);
                r->have = 1;
                fprintf(stderr, "online: player %d rides %X (%d %d %d), bike %d %d %d\n", sender, r->rider,
                        r->outfit[0], r->outfit[1], r->outfit[2], r->bike[0], r->bike[1], r->bike[2]);
            }
            break;
        case EVENT_START:
            if (n - 2 >= 10) memcpy(&on.go_at, e + 2, 8);
            break;
        case EVENT_GO:
            if (n - 2 >= 10) {
                memcpy(&on.their_go[sender & 3], e + 2, 8);
                if (on.go_server) go_skew(sender & 3);
            }
            break;
        case EVENT_FINISH:
            if (n - 2 >= 7) read_finish(e[2], get32(e + 3));
            break;
        case EVENT_RESULTS:
            if (sender == on.host_slot) read_results(e, n - 2);
            break;
        case EVENT_REMATCH:
            if (sender == on.host_slot && sender != on.local_slot && n - 2 >= 4 && e[3] < 26 &&
                !on.rematch_want && !on.restart_accepted) {
                on.rematch_want = e[2] ? 2 : 1, on.rematch_track = e[3], on.rematch_fields = 0;
                on.rematch_at = host_wall_seconds();
                fprintf(stderr, "online: the host chose %s (track %d) at %.1f s\n", e[2] ? "NEW LEVEL" : "TRY AGAIN", e[3],
                        on.rematch_at);
            }
            break;
        default:  /* EVENT_HIT */
            hle_puppet_event(sender, e, n - 2);
        }
    }
}

/* ---- the riders: who wears what ---- */

/* A rider's bike online (issue #18): the room skips the shop, which would write it, so the setup
   holds a stale one, and a pro rider on it rides a mix of two bikes. The five pros get theirs from
   the game's own single-race rule (func_001B7C08, three words); the others the stock bike, 17 4 0,
   as func_001B99B8 gives them. */
extern void func_001B7C08(void *obj, int rider, int slot);
static void default_bike(int rider, int32_t bike[3])
{
    if (rider == 0xE0 || rider == 0x100 || rider == 0x140 || rider == 0x180 || rider == 0x1A0) {
        int32_t scratch[0x300 / 4] = {0};
        func_001B7C08(scratch, rider, 0);
        for (int k = 0; k < 3; k++) bike[k] = scratch[0x14C / 4 + k];
    } else {
        bike[0] = 0x11; bike[1] = 4; bike[2] = 0;
    }
}

/* On the load screen, before the race build: the setup block as this PC's menus left it, with
   the lobby's track and this player's rider; then every PC tells the others what its player
   rides, and waits for theirs (at most 30 s). */
static void exchange_rides(void)
{
    uint8_t e[30] = {EVENT_VERSION, EVENT_SETUP};
    Ride *own = &on.ride[on.local_slot];
    double entered = host_wall_seconds();
    own->rider = EE32(SETUP + 0x290);
    for (int k = 0; k < 3; k++) own->outfit[k] = EE32(SETUP + 0x1B8 + 4 * k), own->bike[k] = EE32(SETUP + 0x11C + 4 * k);
    own->have = 1;
    put32(e + 2, own->rider);
    for (int k = 0; k < 3; k++) put32(e + 6 + 4 * k, own->outfit[k]), put32(e + 18 + 4 * k, own->bike[k]);
    send_event(e, sizeof e);
    for (;;) {
        NetLobby lobby;
        host_service_net_get_lobby(GADDR(&lobby));
        receive_frames();
        int missing = 0;
        for (int i = 0; i < on.nslots; i++)
            if (!lobby.racer[i].left && !on.ride[i].have) missing++;
        if (!missing || !host_service_net_racing() || host_wall_seconds() - entered > 30) break;
        title("Downhill Revived online - room %s - waiting for %d player%s' riders", on.code, missing,
              missing == 1 ? "" : "s");
        host_service_gs_pump_messages();
        host_sleep_ms(5);
    }
    kernel_pace_slip(host_wall_seconds() - entered);
}

/* func_001B89C8's AI count for this PC's one-player race (patched there): online the other
   players are puppets in AI slots, so there is one at least for each; with the room's computer
   riders off, only those. Every PC gets the same count from the same room. */
int hle_online_ai_count(int count)
{
    if (!online_mode() || !on.released) return count;
    int others = on.nslots - 1;
    return on.no_bots || count < others ? others : count;
}

/* The riders in the online race (the human, then the AI count func_001B74E8 kept), all ten offline. */
int hle_online_race_riders(void)
{
    if (!online_mode() || !on.released) return NRIDERS;
    int n = 1 + EE32(SETUP + 0x28C);
    return n < 1 ? 1 : n > NRIDERS ? NRIDERS : n;
}

/* After func_001B74E8 chose the AI riders (patched into func_001B7198): the AI slots that are
   puppets of the other players wear what those players ride, and the host's AI riders are the
   same on every PC (the remaining ones of the ten, in the game's order from +0x368). A ride not
   received falls back to the lobby pick with its default bike. */
void hle_online_riders(void *game)
{
    if (!online_mode() || !on.released) return;
    uint8_t *g = game;
    int used[16] = {0}, next = 0;
    for (int j = 0; j < on.nslots; j++) used[on.ride[j].have ? (on.ride[j].rider / 32 - 1) & 15 : on.pick[j]] = 1;
    while (next < 10 && regular[next] != *(int32_t *)(g + 0x368) / 32 - 1) next++;
    next %= 10;
    for (int i = 0; i < 16; i++) *(int32_t *)(g + 0x2A8 + 4 * i) = -1;
    *(int32_t *)(g + 0x2A8 + 4 * ((*(int32_t *)(g + 0x290) / 32 - 1) & 15)) = 0;
    for (int slot = 1; slot < NRIDERS && slot <= *(int32_t *)(g + 0x28C); slot++) {
        int k = slot - 1, global = hle_puppet_global(slot), id, outfit[3] = {0, 0, 0}, bike[3];
        if (global < on.nslots) {
            const Ride *r = &on.ride[global];
            id = r->have ? r->rider : 0x20 * (on.pick[global] + 1);
            for (int w = 0; w < 3; w++) outfit[w] = r->have ? r->outfit[w] : 0;
            if (r->have) for (int w = 0; w < 3; w++) bike[w] = r->bike[w];
            else default_bike(id, bike);
        } else {
            for (int tries = 0; tries < 10 && used[regular[next]]; tries++) next = (next + 1) % 10;
            id = 0x20 * (regular[next] + 1);
            used[regular[next]] = 1;
            default_bike(id, bike);
        }
        outfit[0] = id / 32 - 1;
        *(int32_t *)(g + 0x32C + 4 * k) = id;
        for (int w = 0; w < 3; w++) {
            *(int32_t *)(g + 0x1E8 + 12 * k + 4 * w) = outfit[w];
            *(int32_t *)(g + 0x14C + 12 * k + 4 * w) = bike[w];
        }
        *(int32_t *)(g + 0x264 + 4 * k) = func_001FA7C0(bike[0]);
        /* +0x2A8 is per rider: the slot riding it, -1 if none. The AI choice (func_001B74E8) and
           the rider screens only ask whether it is free, so a puppet on the human's rider leaves
           the human's entry */
        int32_t *kind = (int32_t *)(g + 0x2A8 + 4 * ((id / 32 - 1) & 15));
        if (*kind == -1) *kind = 4;
        fprintf(stderr, "online: slot %d (%s %d) rides %s, bike %d %d %d\n", slot, global < on.nslots ? "player" : "AI",
                global, rider_name[(id / 32 - 1) & 15], bike[0], bike[1], bike[2]);
    }
}

/* ---- the start ---- */

/* At the countdown: every PC says it has loaded; when all have (at most 60 s), the host names a
   moment on the server's clock and every PC holds the countdown until then, so the GOs land
   within a frame or two of each other on every PC. Both are said again every half second: after
   a TRY AGAIN or NEW LEVEL the PC that loads first is here before the other has reset for the next
   race (rematch_reset), which would drop a single message, and the start would wait 60 s.
   One step a field (pads_begin_field): the game goes on drawing while it waits, its countdown held
   where it is (hle_online_countdown_hold) and the wait on the screen (hle_online_hud). A PC that
   skipped the flight over the track waits so for one still watching it. The step whose field the
   moment falls in waits for it there, as closely as the whole wait did before. 1: the countdown
   runs from now. */
static int start_step(void)
{
    static const uint8_t loaded[2] = {EVENT_VERSION, EVENT_LOADED};
    int host = on.local_slot == on.host_slot;
    if (!on.start_entered) {
        on.start_entered = host_wall_seconds(), on.start_said = -1;
        fprintf(stderr, "online: loaded at %.1f s, waiting for the others\n", on.start_entered);
    }
    NetLobby lobby;
    host_service_net_get_lobby(GADDR(&lobby));
    if (host_wall_seconds() - on.start_said >= 0.5) {
        on.start_said = host_wall_seconds();
        send_event(loaded, sizeof loaded);
        if (on.start_announced) {
            uint8_t e[10] = {EVENT_VERSION, EVENT_START};
            memcpy(e + 2, &on.go_at, 8);
            send_event(e, sizeof e);
        }
    }
    receive_frames();
    int missing = 0;
    for (int i = 0; i < lobby.nslots && i < NET_MAX_SLOTS; i++)
        if (i != on.local_slot && !lobby.racer[i].left && !on.loaded[i]) {
            if (!missing++) on.start_waiting = i;
        }
    on.start_missing = missing;
    double waited = host_wall_seconds() - on.start_entered;
    int go = !host_service_net_racing() || waited > 75;
    if (!go && host && !on.start_announced && (!missing || waited > 60)) {
        uint8_t e[10] = {EVENT_VERSION, EVENT_START};
        on.go_at = host_service_net_server_ms() + START_LEAD_MS;
        memcpy(e + 2, &on.go_at, 8);
        send_event(e, sizeof e);
        on.start_announced = 1;
        if (missing) fprintf(stderr, "online: %d player%s never said they loaded: starting without waiting\n",
                             missing, missing == 1 ? "" : "s");
    }
    if (!go && on.go_at && on.go_at - host_service_net_server_ms() < 1000 * port_frame_period() + 5) {
        while (host_service_net_server_ms() < on.go_at) host_sleep_ms(0);
        go = 1;
    }
    if (!go) {
        if (missing) title("Downhill Revived online - room %s - waiting for %d player%s to load", on.code, missing,
                           missing == 1 ? "" : "s");
        else title("Downhill Revived online - room %s - get ready", on.code);
        return 0;
    }
    on.start_missing = 0;
    kernel_pace_shared(1);  /* from the agreed moment the fields keep to it */
    hle_puppet_rematch();  /* snapshots from before this start (of the race before, after a rematch) */
    fprintf(stderr, "online: countdown starts (waited %.3f s, %+.1f ms from the agreed moment)\n",
            host_wall_seconds() - on.start_entered, on.go_at ? host_service_net_server_ms() - on.go_at : 0.0);
    return 1;
}

/* func_0019EE98, each frame of the race's start sequence (state: the race phase): 1 keeps the
   countdown's timer where it is, from the countdown's first frame until start_step lets it run. */
int hle_online_countdown_hold(int state)
{
    return state == 1 && online_mode() && on.setup_done && !on.started && EE32(MENU) == LOAD_MENU;
}

/* func_00176ED8, each view's HUD every frame: while the countdown waits for players, who it waits
   for, as the game writes its own prompts there (centred, x 256 of the view's 512). */
extern char D_004F3570[];  /* the fonts: 0x20 bytes each (menu_page.c) */
void func_0020F588(int view, int unused, int r, int g, int b, int a);
void func_0020FB80(int font, int x, int y, int width, int height);
void func_0020F9F0(int font, int view, int x, int y, const char *text);

static void race_chat_draw(int view);

void hle_online_hud(int view)
{
    if (!online_mode()) return;
    race_chat_draw(view);
    if (on.started || !on.start_entered || !on.start_missing) return;
    char text[64], name[32];
    if (on.start_missing > 1) snprintf(text, sizeof text, "WAITING FOR THE OTHERS...");
    else {
        snprintf(name, sizeof name, "%s", on.name[on.start_waiting]);
        for (char *c = name; *c; c++) *c = toupper((unsigned char)*c);
        snprintf(text, sizeof text, "WAITING FOR %s...", name);
    }
    char saved[0x20];
    memcpy(saved, D_004F3570, sizeof saved);
    func_0020F588(view, 0, 128, 128, 128, 128);
    func_0020FB80(0, 256, 192, -91, -32);
    func_0020F9F0(0, view, 256, 150, text);
    memcpy(D_004F3570, saved, sizeof saved);
}

/* ---- the finish ---- */

static const char *rider_label(int global, char *out, int size)
{
    if (global < on.nslots) snprintf(out, size, "%s", on.name[global]);
    else {
        int id = EE32(SETUP + 0x32C + 4 * (hle_puppet_local(global) - 1));
        snprintf(out, size, "%s (AI)", rider_name[(id / 32 - 1) & 15]);
    }
    return out;
}

static void read_finish(int global, int time)
{
    char name[48];
    if (global >= NRIDERS || on.finish[global] >= 0) return;
    on.finish[global] = time;
    if (global < on.nslots && !on.first_finish) on.first_finish = host_wall_seconds();
    fprintf(stderr, "online: %s finished in %d:%05.2f\n", rider_label(global, name, sizeof name), time / 3600,
            time % 3600 / 60.0);
}

/* The host's final order: a count, then each rider (global id) and finish time (-1: not finished),
   first place first. */
static void read_results(const uint8_t *in, int n)
{
    char name[48];
    int count = n >= 3 ? in[2] : 0;
    if (on.have_results || count > NRIDERS || n < 3 + 5 * count) return;
    for (int i = 0; i < count; i++) {
        int global = in[3 + 5 * i], time = get32(in + 4 + 5 * i);
        on.results[i] = global;
        if (time >= 0) read_finish(global, time);
    }
    on.nresults = count, on.have_results = 1;
    char text[300];
    int len = snprintf(text, sizeof text, "Downhill Revived online - results:");
    for (int i = 0; i < count; i++) {
        int global = on.results[i], time = on.finish[global];
        rider_label(global, name, sizeof name);
        if (time >= 0)
            fprintf(stderr, "online: result %d. %s %d:%05.2f\n", i + 1, name, time / 3600, time % 3600 / 60.0);
        else
            fprintf(stderr, "online: result %d. %s (did not finish)\n", i + 1, name);
        if (global < on.nslots && len < (int)sizeof text - 40)
            len += snprintf(text + len, sizeof text - len, "  %d. %s %d:%05.2f", i + 1, name, time >= 0 ? time / 3600 : 0,
                            time >= 0 ? time % 3600 / 60.0 : 0.0);
    }
    title("%s", text);
}

/* func_0019F9B0 (this PC's rider finished): the game counts 8 s down to the results screen.
   Online that waits for the host's results, so every PC's screen shows the same order. */
int hle_online_race_end(int ticks)
{
    (void)ticks;
    if (!online_mode() || !on.started || on.have_results) return 0;
    on.end_held = 1, on.end_held_at = host_wall_seconds();
    fprintf(stderr, "online: finished; the results screen waits for the others\n");
    return 1;
}

/* Each field: finishes of the riders this PC runs go out; finishes the others reported become the
   times here (the game ranks by them every step); a puppet whose player finished more than a
   second ago but hasn't crossed here yet crosses now. The host publishes the order once every
   human has finished or left (or 90 s after the first finished). */
static void finish_step(void)
{
    if (!race_running()) return;
    static double test_finish = -1;  /* DHD_NET_TEST_FINISH=seconds: tests, this PC's rider finishes then */
    if (test_finish < 0) test_finish = getenv("DHD_NET_TEST_FINISH") ? atof(getenv("DHD_NET_TEST_FINISH")) : 0;
    if (test_finish > 0 && RACE_CLOCK > test_finish * 60 && !FINISHED(0) && !FINISH_NOW(0)) FINISH_NOW(0) = 1;
    int riders = hle_online_race_riders();
    for (int slot = 0; slot < riders; slot++) {
        int global = hle_puppet_global(slot);
        if (hle_puppet_owned(slot)) {
            if (FINISHED(slot) && !on.reported[global]) {
                uint8_t e[7] = {EVENT_VERSION, EVENT_FINISH, (uint8_t)global};
                put32(e + 3, FINISH_TIME(slot));
                send_event(e, sizeof e);
                on.reported[global] = 1;
                read_finish(global, FINISH_TIME(slot));
            }
        } else if (on.finish[global] >= 0) {
            if (FINISHED(slot)) FINISH_TIME(slot) = on.finish[global];
            else if (RACE_CLOCK > on.finish[global] + 60 && !FINISH_NOW(slot)) FINISH_NOW(slot) = 1;
        }
    }
    if (on.local_slot != on.host_slot || on.have_results || !on.first_finish) return;
    NetLobby lobby;
    host_service_net_get_lobby(GADDR(&lobby));
    int waiting = 0;
    for (int j = 0; j < on.nslots; j++)
        if (on.finish[j] < 0 && !lobby.racer[j].left && !on.gone[j]) waiting++;
    if (waiting && host_wall_seconds() - on.first_finish < 90) return;
    /* the order: the finished by time, then the others by the game's places */
    uint8_t e[3 + 5 * NRIDERS] = {EVENT_VERSION, EVENT_RESULTS, (uint8_t)riders};
    int order[NRIDERS];
    for (int i = 0; i < riders; i++) order[i] = i;
    for (int i = 0; i < riders; i++)
        for (int j = i + 1; j < riders; j++) {
            int a = order[i], b = order[j], ta = on.finish[a], tb = on.finish[b];
            int a_first = ta >= 0 && tb >= 0 ? ta <= tb : ta >= 0 ? 1 : tb >= 0 ? 0
                          : PLACE(hle_puppet_local(a)) <= PLACE(hle_puppet_local(b));
            if (!a_first) order[i] = b, order[j] = a;
        }
    for (int i = 0; i < riders; i++) e[3 + 5 * i] = (uint8_t)order[i], put32(e + 4 + 5 * i, on.finish[order[i]]);
    send_event(e, 3 + 5 * riders);
    read_results(e, 3 + 5 * riders);
}

/* ---- disconnects ---- */

/* A player who left: their rider goes on as the host's AI (the lowest lobby slot still here is
   the host; it runs the AI riders), unless they had finished: then it stays where it is, finished
   (hle_puppet_drop). If this PC lost the connection, it runs every rider itself. */
static void check_left(void)
{
    NetLobby lobby;
    host_service_net_get_lobby(GADDR(&lobby));
    for (int j = 0; j < on.nslots; j++) {
        int gone = j != on.local_slot && (lobby.racer[j].left || lobby.offline);
        if (!gone || on.gone[j]) continue;
        on.gone[j] = 1;
        int host = on.host_slot;
        if (on.gone[host]) {
            host = 0;
            while (host < on.nslots && on.gone[host]) host++;
            if (lobby.offline) host = on.local_slot;
            on.host_slot = host;
        }
        fprintf(stderr, "online: player %d (%s) left at race tick %d; host now player %d\n", j, on.name[j], RACE_CLOCK,
                host);
        hle_puppet_drop(j, host, on.finish[j] >= 0);
    }
}

/* Each vsync of the race: the snapshots and events in, ours out. */
static void state_sync(void)
{
    uint8_t payload[NET_FRAME_MAX - 2];
    int n, clock = RACE_CLOCK;
    receive_frames();
    if (EE32(MENU) != LOAD_MENU || on.restart_accepted) return;
    if (race_running() && !on.go_sent) {  /* GO: when it was, on the server's clock */
        uint8_t e[10] = {EVENT_VERSION, EVENT_GO};
        on.go_server = host_service_net_server_ms() - port_field_seconds(clock) * 1000;
        memcpy(e + 2, &on.go_server, 8);
        send_event(e, sizeof e);
        on.go_sent = 1;
        for (int j = 0; j < 4; j++)
            if (on.their_go[j]) go_skew(j);
    }
    static int checked;
    if (++checked % 15 == 0) check_left();
    finish_step();
    if (on.end_held && !on.end_done &&
        (on.have_results || host_wall_seconds() - on.end_held_at > 120)) {
        func_001B45F8(EE_PTR(0x4B1F40), 1, 4 * 60);  /* the results screen in 4 s */
        on.end_done = 1;
        on.end_done_at = host_wall_seconds();
        /* tests (DHD_NET_SCRIPTED): DHD_NET_AFTER_RACE is the menu script from here, e.g. through
           the results and their menu to QUIT */
        const char *after = getenv("DHD_NET_AFTER_RACE");
        if (after && *after && getenv("DHD_NET_SCRIPTED")) {
            char script[1200];
            snprintf(script, sizeof script, "%s", after);
            set_env("DHD_NAV", script);
        }
    }
    if (!race_running() || clock < on.last_sent + SNAPSHOT_TICKS && clock >= on.last_sent) return;
    on.last_sent = clock - clock % SNAPSHOT_TICKS;
    /* All our riders in one frame (the host's AI riders too): one message to the server a
       snapshot, not one a rider. Each is [size][snapshot]; a snapshot stays within 255 bytes. */
    int at = 0, riders = 0;
    for (int slot = 0; slot < hle_online_race_riders(); slot++) {
        int room = (int)sizeof payload - at - 1;
        if ((n = hle_puppet_pack(slot, payload + at + 1, room < 255 ? room : 255)) > 0) {
            payload[at] = (uint8_t)n;
            if (slot == 0) record(payload + at + 1, n);
            at += 1 + n, riders++;
        }
    }
    if (at && host_service_net_send_frame(NET_FRAME_STATE, GADDR(payload), at)) on.sent += riders;
    while ((n = hle_puppet_take_event(payload, sizeof payload)) > 0) send_event(payload, n);
    static double last_title;
    if (host_wall_seconds() - last_title > 1) {
        NetLobby lobby;
        char lag[160];
        host_service_net_get_lobby(GADDR(&lobby));
        last_title = host_wall_seconds();
        hle_puppet_lag(lag, sizeof lag);
        if (!on.have_results)
            title("Downhill Revived online - room %s - player %d of %d - ping %d ms%s%s", on.code, on.local_slot + 1,
                  on.nslots, lobby.ping_ms, lobby.offline ? " - offline" : "",
                  on.end_held ? " - finished, waiting for the others" : "");
        extern unsigned kernel_pace_extra_fields, kernel_pace_late_ms;
        fprintf(stderr, "online: race clock %d (%d fps), %u snapshots sent, %u received, hits taken %d, %u ms late, %u "
                "extra fields, lag (ms):%s\n", clock, port_fps(), on.sent, on.received, hle_puppet_hits_taken(),
                kernel_pace_late_ms, kernel_pace_extra_fields, lag);
        static double last_ping;
        if (last_title - last_ping >= 10) last_ping = last_title, log_pings(&lobby);
    }
}

/* DHD_VIEW=0-3 shows that player's quarter full screen during 3-4 player races ("all": the
   split screen); online the default is this PC's player. Menus and loads stay whole. */
static void choose_view(void)
{
    static HleEnv view_env;
    const char *v = hle_env(&view_env, "DHD_VIEW");
    int want = v && *v ? (*v >= '0' && *v <= '3' ? *v - '0' + 1 : 0) : online_mode() && on.released ? on.local_slot + 1 : 0;
    int phase = EE32(PHASE), racing = EE32(MENU) == LOAD_MENU && phase >= 1 && phase <= 3;
    host_service_gs_native_view(racing && EE32(SETUP + 0x288) >= 3 ? want : 0);
}

/* ---- the other players' names over their riders ---- */

void func_00222D48(int view, float *pos, float *screen);  /* world point to the view's GS coordinates */
void func_0023D618(int view, int *x, int *y);             /* the view's centre */
void func_0023D650(int view, int *w, int *h);             /* its size */
void func_0020FB80(int pen, int w, int h, int dw, int dh);
void func_0020F518(int pen, int r, int g, int b, int a);
void func_0020F420(int pen, int view);
void func_0020F9F0(int pen, int view, int x, int y, const char *text);

/* The stock rider markers (func_001797A0) establish world Z as up. Use the body
   scene node with a lower offset for a label just above the head. Puppets update
   that same node through func_0012DDC0.
   Draw while the local rider's race text is built, then restore pen 0 and its
   queued colour so later HUD text keeps its own size, alignment and colour. */
enum { NAME_FAR = 150 };
int func_0020F860(int pen, const char *text);  /* text width in sixteenths */
extern float port_fov_x;  /* widescreen.c: glyph width scale, 1 when off */
void hle_online_names(void *rider, int view)
{
    if (!online_mode() || !on.started || rider != RIDER(0) || !race_running()) return;
    uint8_t *pen = EE_PTR(0x4F3570), kept[0x20];
    int cx, cy, w, h, drawn = 0;
    func_0023D618(view, &cx, &cy);
    func_0023D650(view, &w, &h);
    memcpy(kept, pen, sizeof kept);
    for (int slot = 1; slot < hle_online_race_riders(); slot++) {
        int global = hle_puppet_global(slot);
        if (global < 0 || global >= on.nslots || on.gone[global] || !on.name[global][0]) continue;
        uintptr_t body = (uint32_t)*(int32_t *)(RIDER(slot) + 0x792C);
        if (!body) continue;
        float at[4] __attribute__((aligned(16))), screen[4] __attribute__((aligned(16)));
        float base[4] __attribute__((aligned(16)));
        memcpy(at, (void *)(body + 0x10), sizeof at);
        func_00222D48(view, at, base);
        at[2] += 3.25f;
        func_00222D48(view, at, screen);
        if (!(base[2] >= 1 && base[2] <= NAME_FAR && screen[2] >= 1)) continue;
        /* Keep the label above the rider on screen: raising the world point also
           shifts X outward in a pitched camera, especially at the start gate. */
        float local_x = base[0] - (cx - w / 2), local_y = screen[1] - (cy - h / 2);
        float base_y = base[1] - (cy - h / 2);
        /* Check before converting, also rejecting non-finite projections. */
        if (!(local_x >= 0 && local_x < w && base_y >= 0 && base_y < h && local_y >= 0 && local_y < h)) continue;
        /* Text normally stays inside the middle 4:3 area. Undo that mapping for
           this world anchor, while keeping the glyphs themselves in proportion. */
        int x = (int)(w / 2 + (local_x - w / 2) / port_fov_x), y = (int)local_y - 16;
        func_0020FB80(0, 320, 240, -102, -24);
        int half_width = (func_0020F860(0, on.name[global]) + 31) / 32;
        if (local_x - half_width * port_fov_x < 1 || local_x + half_width * port_fov_x >= w - 1 || y < 1) continue;
        func_0020F518(0, 0, 0, 0, 128);
        func_0020F9F0(0, view, x - 1, y, on.name[global]);
        func_0020F9F0(0, view, x + 1, y, on.name[global]);
        func_0020F9F0(0, view, x, y - 1, on.name[global]);
        func_0020F9F0(0, view, x, y + 1, on.name[global]);
        func_0020F518(0, 128, 128, 128, 128);
        func_0020F9F0(0, view, x, y, on.name[global]);
        drawn = 1;
    }
    memcpy(pen, kept, sizeof kept);
    for (int i = 0; drawn && i < 4; i++) func_0020F420(0, i);
}

/* ---- text chat in the race (finding 125) ----
   On the race screen Y opens a line to type (Enter says it, Esc closes; the controller keeps
   riding meanwhile, and text_input.c keeps the typed keys from the pad), and in the pause menu
   L1, R1, L2 and R2 say the quick lines (port_chat_presets) without a keyboard. The last lines
   said show at the left of this player's view for a few seconds, fading, in the HUD's font as
   hle_online_hud's own prompt is drawn. */
enum { RACE_CHAT_SHOWN = 5, RACE_CHAT_SECONDS = 8, RACE_CHAT_KEY = 'Y', RACE_CHAT_TEXT = 56 };
enum { QUICK_CHAT = PAD_L1 | PAD_R1 | PAD_L2 | PAD_R2 };
static struct {
    unsigned seen;          /* the newest chat line taken */
    int seen_valid, key_held, typing, quick_held;
    char entry[NET_CHAT_MAX + 1];
    struct { char text[RACE_CHAT_TEXT]; int own, note; double until; } shown[RACE_CHAT_SHOWN];  /* oldest first */
} race_chat;
static int menu_open(int menu);
void func_0020F9A8(int pen, int view, int x, int y, const char *text);  /* left-aligned */

static void race_chat_close(void)
{
    if (!race_chat.typing) return;
    race_chat.typing = 0;
    host_service_text_input_end();
}

/* Every field online: the lines that came, and the line being typed. */
static void race_chat_step(void)
{
    static NetChat lines;
    static NetLobby lobby;
    if (EE32(MENU) != LOAD_MENU || !on.setup_done) {
        race_chat_close();
        return;
    }
    host_service_net_get_chat(GADDR(&lines));
    if (!race_chat.seen_valid) race_chat.seen = lines.seq, race_chat.seen_valid = 1;  /* the room's are old */
    if (lines.seq != race_chat.seen) {
        host_service_net_get_lobby(GADDR(&lobby));
        for (int i = 0; i < lines.count; i++) {
            const NetChatLine *line = &lines.line[i];
            if (line->seq <= race_chat.seen) continue;
            memmove(race_chat.shown, race_chat.shown + 1, sizeof race_chat.shown[0] * (RACE_CHAT_SHOWN - 1));
            char shown[NET_CHAT_NAME + NET_CHAT_TEXT + 4];
            port_chat_show(shown, sizeof shown, line->name, line->text, line->member_id);
            if (strlen(shown) >= RACE_CHAT_TEXT) memcpy(shown + RACE_CHAT_TEXT - 3, "..", 3);
            snprintf(race_chat.shown[RACE_CHAT_SHOWN - 1].text, RACE_CHAT_TEXT, "%s", shown);
            race_chat.shown[RACE_CHAT_SHOWN - 1].own = line->member_id == lobby.member_id;
            race_chat.shown[RACE_CHAT_SHOWN - 1].note = line->member_id < 0;
            race_chat.shown[RACE_CHAT_SHOWN - 1].until = host_wall_seconds() + RACE_CHAT_SECONDS;
        }
        race_chat.seen = lines.seq;
    }
    int key = host_service_key_held(RACE_CHAT_KEY);
    if (key && !race_chat.key_held && !race_chat.typing) {
        race_chat.typing = 1, race_chat.entry[0] = 0;
        host_service_text_input_begin();
    }
    race_chat.key_held = key;
    if (!race_chat.typing) return;
    int action = host_service_text_input_edit(GADDR(race_chat.entry), sizeof race_chat.entry, GADDR(port_chat_letters));
    if (action > 0 && race_chat.entry[0]) host_service_net_send_chat(GADDR(race_chat.entry));
    if (action) race_chat_close();
}

/* In the pause menu the shoulder buttons say the quick lines; they do nothing else there. */
static void race_chat_quick(HostPad *out)
{
    static const uint16_t buttons[PORT_CHAT_PRESETS] = {PAD_L1, PAD_R1, PAD_L2, PAD_R2};
    int held = out->buttons & QUICK_CHAT, pressed = held & ~race_chat.quick_held;
    race_chat.quick_held = held;  /* held as the menu opens (a trick): not a press */
    if (!menu_open(PAUSE_MENU)) return;
    for (int i = 0; i < PORT_CHAT_PRESETS; i++)
        if (pressed & buttons[i] && host_service_net_send_chat(GADDR(port_chat_presets[i]))) menu_play("S_SELECT");
    out->buttons &= ~QUICK_CHAT;
}

/* One line with a black edge, as the names over the riders are drawn. */
static void race_chat_line(int view, int x, int y, const char *text, int r, int g, int b, int a)
{
    func_0020F588(view, 0, 0, 0, 0, a);
    func_0020F9A8(0, view, x - 1, y, text);
    func_0020F9A8(0, view, x + 1, y, text);
    func_0020F9A8(0, view, x, y - 1, text);
    func_0020F9A8(0, view, x, y + 1, text);
    func_0020F588(view, 0, r, g, b, a);
    func_0020F9A8(0, view, x, y, text);
}

/* The last lines at the left of this player's view (the local rider's), mid-height, clear of
   the HUD's corners; the line being typed under them; in the pause menu the quick lines. */
static void race_chat_draw(int view)
{
    if (EE32(MENU) != LOAD_MENU || !on.setup_done || view != *(int32_t *)(RIDER(0) + 0x7988)) return;
    double now = host_wall_seconds();
    int pause = menu_open(PAUSE_MENU), alive = 0, w, h;
    for (int i = 0; i < RACE_CHAT_SHOWN; i++) alive |= race_chat.shown[i].until > now;
    if (!alive && !race_chat.typing && !pause) return;
    func_0023D650(view, &w, &h);
    if (w <= 0 || h <= 0) w = 512, h = 224;
    char saved[0x20];
    memcpy(saved, D_004F3570, sizeof saved);
    func_0020FB80(0, 205, 154, -73, -26);  /* the HUD prompt's size (hle_online_hud), 80 % */
    int x = 24, step = h * 11 / 224, y = h * 78 / 224;
    for (int i = 0; i < RACE_CHAT_SHOWN; i++, y += step) {
        double left = race_chat.shown[i].until - now;
        if (left <= 0) continue;
        int a = left < 1 ? (int)(128 * left) : 128;
        if (race_chat.shown[i].note) race_chat_line(view, x, y, race_chat.shown[i].text, 128, 64, 64, a);
        else if (race_chat.shown[i].own) race_chat_line(view, x, y, race_chat.shown[i].text, 128, 120, 64, a);
        else race_chat_line(view, x, y, race_chat.shown[i].text, 128, 128, 128, a);
    }
    if (race_chat.typing) {
        char typed[NET_CHAT_MAX + 8];
        size_t n = strlen(race_chat.entry);
        /* a long line shows its end */
        snprintf(typed, sizeof typed, "SAY: %s_", n > 44 ? race_chat.entry + n - 44 : race_chat.entry);
        race_chat_line(view, x, y + step / 2, typed, 128, 120, 64, 128);
        race_chat_line(view, x, y + step / 2 + step, "ENTER SAY   ESC CANCEL", 96, 96, 96, 128);
    } else if (pause) {
        char quick[96];
        snprintf(quick, sizeof quick, "CHAT:  L1 %s   R1 %s   L2 %s   R2 %s", port_chat_presets[0], port_chat_presets[1],
                 port_chat_presets[2], port_chat_presets[3]);
        race_chat_line(view, x, h * 200 / 224, quick, 128, 128, 128, 128);
    }
    memcpy(D_004F3570, saved, sizeof saved);
    func_0020F420(0, view);  /* pen 0's own colour again, for the HUD's text after this */
}

/* ---- cheat codes (finding 117) ----
   func_001E6F58, the human rider's pad reader at rider +0x1420, matches the last presses against
   the code table (D_00272448) and leaves the code's number (1-26) at rider +0x1E3C for one frame;
   in the race (phases 2-4) func_0012C9E0 applies it, codes 2-26 only once code 1 ("Codes") has
   turned code entry on (rider +0x1E3A, func_001E7460). Their effects are the bytes at rider
   +0x7BA8..+0x7BB6, which func_0012C450 clears at each race's load but for +0x7BB2 (Always
   Stoked), +0x7BB4 and +0x7BB5, and Extra Smack Time's global copy D_0043EF2C. With the room's
   cheats off (online_menu.c ROOM_CHEATS) a code does nothing online, and the race starts with
   none of them on. Offline, and in a room with cheats on, the game's own rules. */
enum { CHEAT_CODE = 0x1E3C, CHEAT_ENTRY = 0x1E3A, CHEAT_FLAGS = 0x7BA8, CHEAT_FLAG_COUNT = 15,
       CHEAT_NAMES = 0x272818, SMACK_TIME = 0x43EF2C };
enum { SPEED_FREAK = 0x7BAA, INFINITE_BOTTLES = 0x7BAD };
static int cheat_entered;  /* this frame's code, logged after func_0012C9E0 applied it */

static int cheats_blocked(void) { return online_mode() && on.released && !on.cheats; }

static const char *cheat_name(int code)
{
    return code > 0 && code < 27 ? *(const char **)EE_PTR(CHEAT_NAMES + 4 * code) : "?";
}

static void cheat_flags(const uint8_t *rider, char *out, int size)
{
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < CHEAT_FLAG_COUNT && n < size - 8; i++)
        if (rider[CHEAT_FLAGS + i]) n += snprintf(out + n, size - n, " +%X", CHEAT_FLAGS + i);
    if (!n) snprintf(out, size, " none");
}

/* Patched into func_0012C9E0 before it reads the code. */
void hle_cheat_code(void *rider)
{
    int32_t *code = (int32_t *)((uint8_t *)rider + CHEAT_CODE);
    cheat_entered = *code;
    if (!*code || !cheats_blocked()) return;
    hle_log("cheats: code %d (%s) blocked: the room's cheats are off\n", *code, cheat_name(*code));
    *code = cheat_entered = 0;
}

/* After func_0012C9E0: what the code left on. */
void hle_cheat_code_done(void *rider)
{
    if (!cheat_entered) return;
    const uint8_t *r = rider;
    char flags[96];
    cheat_flags(r, flags, sizeof flags);
    hle_log("cheats: code %d (%s)%s, code entry %s; on:%s\n", cheat_entered, cheat_name(cheat_entered),
            online_mode() ? " online, the room's cheats on" : "", r[CHEAT_ENTRY] ? "on" : "off", flags);
    cheat_entered = 0;
}

/* GO online: with the room's cheats off, whatever was on (from offline play before) goes off,
   the way typing its code again would turn it off. */
static void cheats_start(void)
{
    uint8_t *r = RIDER(0);
    char flags[96];
    if (on.cheats) {
        hle_log("cheats: the room's cheats are on\n");
        return;
    }
    cheat_flags(r, flags, sizeof flags);
    if (r[SPEED_FREAK]) *(float *)(r + 0x4214) = 1.0f;
    if (r[INFINITE_BOTTLES]) *(int32_t *)(r + 0x7ADC) = 3;
    memset(r + CHEAT_FLAGS, 0, CHEAT_FLAG_COUNT);
    uint8_t *smack = EE_PTR(SMACK_TIME);
    hle_log("cheats: the room's cheats are off; cleared:%s%s\n", flags, *smack ? " and Extra Smack Time's copy" : "");
    *smack = 0;
}

/* ---- the next race in the same room ---- */

/* Someone else still in the race, so the host's next race is theirs too. */
static int others_here(void)
{
    NetLobby lobby;
    host_service_net_get_lobby(GADDR(&lobby));
    if (!host_service_net_racing()) return 0;
    for (int j = 0; j < lobby.nslots && j < NET_MAX_SLOTS; j++)
        if (j != on.local_slot && !lobby.racer[j].left && !on.gone[j]) return 1;
    return 0;
}

/* The host's TRY AGAIN or NEW LEVEL after the results: the game starts the next race on the race
   screen (race phase back to the countdown, or to the flight over a new track), on every PC. The
   race's own online state starts again; the room, the players and their rides stay. The new start
   is agreed again (start_step), and the puppets drop what they had (hle_puppet_rematch). */
static int rematch_trace, rematch_menu, rematch_phase;
static int rematches;  /* rematch_reset's so far (DHD_NET_HOST_NEXT's list) */
static void rematch_reset(int track)
{
    fprintf(stderr, "online: the next race in room %s: track %d (at %.1f s)\n", on.code, track, host_wall_seconds());
    on.started = on.go_sent = 0;
    on.go_at = on.go_server = 0;
    on.start_entered = on.start_said = 0;
    on.start_announced = on.start_missing = 0;
    memset(on.their_go, 0, sizeof on.their_go);
    memset(on.loaded, 0, sizeof on.loaded);
    on.last_sent = 0;
    for (int i = 0; i < NRIDERS; i++) on.finish[i] = -1;
    memset(on.reported, 0, sizeof on.reported);
    on.nresults = on.have_results = 0;
    on.first_finish = 0;
    on.end_held = on.end_done = 0;
    on.end_held_at = on.end_done_at = 0;
    on.rematch_want = on.rematch_fields = on.restart_accepted = 0;
    on.rematch_at = 0;
    on.setup.track = track;
    rematch_trace = 1;
    rematch_menu = rematch_phase = -1;
    rematches++;
    kernel_pace_shared(0);
    hle_puppet_rematch();
}

/* Called only when retail accepts TRY AGAIN / RE-START or a level selection.
   Announce before the loader yields; phase 1 -> 1 is a valid countdown restart.
   The initial level selection and the flyby's skip never announce a restart. */
void hle_online_restart(void)
{
    if (!online_mode() || !on.started || EE32(MENU) != LOAD_MENU || on.restart_accepted) return;
    if (on.rematch_want == 2) {
        RaceSetup s = {on.rematch_track, {-1, -1, -1, -1}};
        write_setup(&s);  /* the level list has just written its own cursor's track */
    }
    /* Test only: exercise a specific destination after the host selects NEW LEVEL. */
    if (on.local_slot == on.host_slot && getenv("DHD_NET_TEST_NEXT_TRACK")) {
        RaceSetup s = {atoi(getenv("DHD_NET_TEST_NEXT_TRACK")), {-1, -1, -1, -1}};
        write_setup(&s);
    }
    on.restart_accepted = 1;
    on.rematch_want = on.rematch_fields = 0;
    on.rematch_at = 0;
    if (on.local_slot == on.host_slot && others_here()) {
        int track = EE32(SETUP + 0x2A0);
        uint8_t e[4] = {EVENT_VERSION, EVENT_REMATCH, (uint8_t)(track != on.setup.track), (uint8_t)track};
        send_event(e, sizeof e);
        fprintf(stderr, "online: accepted %s for everyone (track %d) at %.1f s\n",
                track != on.setup.track ? "NEW LEVEL" : "RE-START", track, host_wall_seconds());
    }
}

/* Retail's race initialization has actually run. A phase observation can miss
   a same-phase restart, and GO/results flags describe the previous race. */
void hle_online_restart_ready(void)
{
    if (online_mode() && on.restart_accepted) rematch_reset(EE32(SETUP + 0x2A0));
}

/* Count fields once, independently of pad reads (above 60 a field has several frames, so whole
   fields: port_whole_step). A stale request, a closed room, or an unrecognised menu must release
   the controller instead of pressing forever. */
static int rematch_step(void)
{
    if (!on.rematch_want) return 0;
    on.rematch_fields += port_fps_high() ? port_whole_step() : 1;
    if (!others_here() || on.rematch_fields > 1800 || host_wall_seconds() - on.rematch_at > 30) {
        fprintf(stderr, "online: stopped following the host's stale restart request (at %.1f s, %d fields, menu %08X)\n",
                host_wall_seconds(), on.rematch_fields, EE32(OPEN_MENU));
        on.rematch_want = on.rematch_fields = 0;
        on.rematch_at = 0;
        return 1;
    }
    return 0;
}

static int menu_open(int menu);

/* Results stay under the host's control. Read the real row and submenu instead
   of repeating DOWN/X on a timer, which can drift into SAVE or QUIT after a fade. */
static int rematch_pad(HostPad *out)
{
    static uint16_t press;
    if (on.local_slot == on.host_slot || !on.end_done || !others_here()) return 0;
    neutral(out);
    if (!on.rematch_want || on.restart_accepted || host_wall_seconds() - on.end_done_at < 6) return 1;
    /* 11: no press before the first whole field (above 60 that takes a few frames) */
    int f = on.rematch_fields > 0 ? (on.rematch_fields - 1) % 12 : 11, row = on.rematch_want == 1 ? 0 : 1;
    if (f == 0) {
        if (menu_open(RESULTS_MENU))
            press = EE32(RESULTS_MENU) < row ? PAD_DOWN : EE32(RESULTS_MENU) > row ? PAD_UP : PAD_CROSS;
        else if (menu_open(LEVEL_LIST)) press = PAD_CROSS;
        else press = PAD_TRIANGLE;
    }
    if (f < 4) out->buttons = press;
    return 1;
}

static int menu_open(int menu)
{
    return EE32(RIDING) == 0 && EE32(OPEN_MENU) == (int32_t)(uintptr_t)EE_PTR(menu);
}

/* The host's RE-START or NEW LEVEL from the pause menu during the race: a guest's pause menu makes
   the same choice (START, the row, X; "Yes" on RE-START's question; X on the level list, whose
   track is the host's, pads_begin_field). Every 12 fields one press of 4, chosen from what is on
   the screen then; another menu (options, the quit question) goes back. 1: out is this field's pad. */
static int follow_pad(HostPad *out)
{
    static uint16_t press;
    if (on.local_slot == on.host_slot || on.end_done || !on.rematch_want || on.restart_accepted || !others_here()) return 0;
    neutral(out);
    int f = on.rematch_fields > 0 ? (on.rematch_fields - 1) % 12 : 11, row = on.rematch_want == 1 ? PAUSE_RESTART : PAUSE_NEW_LEVEL;
    if (f == 0) {
        if (EE32(RIDING)) press = PAD_START;
        else if (menu_open(PAUSE_MENU)) press = EE32(PAUSE_MENU) < row ? PAD_DOWN : EE32(PAUSE_MENU) > row ? PAD_UP : PAD_CROSS;
        else if (menu_open(RESTART_ASK)) press = EE32(RESTART_ASK) ? PAD_CROSS : PAD_DOWN;
        else if (menu_open(LEVEL_LIST)) press = PAD_CROSS;
        else press = PAD_TRIANGLE;
    }
    if (f < 4) out->buttons = press;
    return 1;
}

/* DHD_NET_HOST_NEXT (tests, online_race.py --next): the host's own choice on the results menu,
   pressed as a player would: "again" TRY AGAIN, "new" NEW LEVEL and another track on the level
   list (DOWN, else RIGHT, until the cursor moves); a list ("new,new,again") one race after
   another, its last for the rest. One press of 0.08 s every 0.25 s of wall time,
   chosen from what is on the screen then. */
static int host_next_pad(HostPad *out)
{
    static HleEnv env;
    static uint16_t press;
    static long period = -1;
    static int moved, first;
    const char *next = hle_env(&env, "DHD_NET_HOST_NEXT");
    if (!next || on.local_slot != on.host_slot || !on.end_done || on.restart_accepted ||
        host_wall_seconds() - on.end_done_at < 8) return 0;
    neutral(out);
    double t = host_wall_seconds();
    const char *pick = next;
    for (int k = 0; k < rematches && strchr(pick, ','); k++) pick = strchr(pick, ',') + 1;
    int row = strncmp(pick, "again", 5) ? 1 : 0;
    if ((long)(t / 0.25) != period) {
        period = (long)(t / 0.25);
        if (menu_open(RESULTS_MENU))
            press = EE32(RESULTS_MENU) < row ? PAD_DOWN : EE32(RESULTS_MENU) > row ? PAD_UP : PAD_CROSS, moved = 0;
        else if (menu_open(LEVEL_LIST)) {
            if (!moved) first = EE32(LEVEL_LIST);
            press = EE32(LEVEL_LIST) != first || moved >= 8 ? PAD_CROSS : moved < 4 ? PAD_DOWN : PAD_RIGHT;
            fprintf(stderr, "online: test: level list cursor %d, press %04X\n", EE32(LEVEL_LIST), press);
            moved++;
        } else press = 0;
    }
    if (t - period * 0.25 < 0.08) out->buttons = press;
    return 1;
}

/* DHD_NET_TEST_SKIP=seconds (tests): START that long into each flight over the track (race phase
   5) still on the screen, as a player skipping it would (the first race's is skipped by the menu
   script at once; after a TRY AGAIN or NEW LEVEL it is the player's). One press of 0.08 s. */
static int test_skip_pad(HostPad *out)
{
    static HleEnv env;
    static double began = -1;
    const char *v = hle_env(&env, "DHD_NET_TEST_SKIP");
    if (!v || on.started || EE32(PHASE) != FLYBY_PHASE) { began = -1; return 0; }
    if (began < 0) began = host_wall_seconds();
    double t = host_wall_seconds() - began - atof(v);
    if (t < 0 || t >= 0.08) return 0;
    static double said;
    if (began != said) said = began, fprintf(stderr, "online: test: START on the flight over the track at %.1f s\n", host_wall_seconds());
    neutral(out);
    out->buttons = PAD_START;
    return 1;
}

/* A guest with others in the room can't start the race again alone (the others ride on in the old
   one): X on the pause menu's NEW LEVEL and RE-START does nothing. The host's choice is everyone's. */
static void guest_pad(HostPad *out)
{
    if (on.local_slot == on.host_slot || !others_here() || !menu_open(PAUSE_MENU)) return;
    if (EE32(PAUSE_MENU) == PAUSE_NEW_LEVEL || EE32(PAUSE_MENU) == PAUSE_RESTART) out->buttons &= ~PAD_CROSS;
}

/* Leaving the race screen (QUIT), or giving up an unreachable host restart,
   returns this PC to offline control. Restore the previous script and seed so
   the online boot script cannot swallow title-menu input. Accepted restarts
   stay in the room and use rematch_reset instead. */
static int connected;
static void race_over(void)
{
    fprintf(stderr, "online: the race is over; back to offline play\n");
    rematch_trace = 0;
    host_service_net_leave();
    hle_puppet_offline();
    port_online_relock();
    kernel_pace_shared(0);
    set_env("DHD_NAV", saved_nav);  /* "" unsets it */
    set_env("DHD_RACE_SEED", saved_seed);
    memset(&on, 0, sizeof on);
    race_chat_close();
    memset(&race_chat, 0, sizeof race_chat);
    online = 0, from_menu = 0, connected = 0;
    selected_ride = 0;
    host_service_gs_window_title(GADDR("Downhill Revived"));
}

void pads_begin_field(void)
{
    choose_view();
    extern uint64_t kernel_vsync_count;
    if (!online_mode()) {
        static int setup = -1;
        static RaceSetup offline_setup;
        if (setup < 0) {
            setup = getenv("DHD_RACE_SETUP") != NULL;
            parse_setup(getenv("DHD_RACE_SETUP"), &offline_setup);
        }
        if (setup == 1 && EE32(MENU) == LOAD_MENU) write_setup(&offline_setup), setup = 2;
        log_hash(kernel_vsync_count);
        return;
    }
    if (!connected++ && !from_menu) connect_lobby();
    if (!on.released) {
        barrier();
        on.released = 1;
    }
    if (!on.setup_done && EE32(MENU) == LOAD_MENU) {
        write_setup(&on.setup);
        if (selected_ride) {
            EE32(SETUP + 0x290) = selected_rider;
            for (int k = 0; k < 3; k++) {
                EE32(SETUP + 0x1B8 + 4 * k) = selected_outfit[k];
                EE32(SETUP + 0x11C + 4 * k) = selected_bike[k];
            }
            /* the outfit's first word is the rider (as write_setup and hle_online_riders have it): the
               selection can leave the rider screen's first rider there, who would then ride */
            EE32(SETUP + 0x1B8) = selected_rider / 32 - 1;
        }
        {
            int32_t bike[3];
            default_bike(EE32(SETUP + 0x290), bike);
            for (int k = 0; k < 3; k++) EE32(SETUP + 0x11C + 4 * k) = bike[k];
        }
        exchange_rides();
        on.setup_done = 1;
    }
    if (rematch_trace && (rematch_menu != EE32(MENU) || rematch_phase != EE32(PHASE))) {
        rematch_menu = EE32(MENU), rematch_phase = EE32(PHASE);
        fprintf(stderr, "online: rematch transition menu %d phase %d at %.1f s\n",
                rematch_menu, rematch_phase, host_wall_seconds());
    }
    if (!on.started && on.setup_done && EE32(MENU) == LOAD_MENU && EE32(PHASE) == 1) {
        if (start_step()) rematch_trace = 0, on.started = 1, cheats_start();
    }
    if (on.started) state_sync();
    /* the host's new level, in the setup block once this PC is in its menus (not mid-race) */
    if (on.rematch_want == 2 && EE32(SETUP + 0x2A0) != on.rematch_track &&
        (EE32(RIDING) == 0 || EE32(PHASE) < 2 || EE32(PHASE) == FLYBY_PHASE)) {
        RaceSetup s = {on.rematch_track, {-1, -1, -1, -1}};
        write_setup(&s);
    }
    race_chat_step();
    if (rematch_step()) { race_over(); return; }
    if (on.started && EE32(MENU) != LOAD_MENU) race_over();
}

/* Online: the menu script, then in the race this PC's controller (DHD_NET_SCRIPTED=1, tests:
   still the script, DHD_RACE_PAD). */
void pads_read(int player, HostPad *out)
{
    static HleEnv scripted_env;
    if (online_mode()) {
        /* the race, and after it the results and their menu (Try Again ... Quit, race phase 0) */
        /* Once setup has been exchanged, the flyby and its skip prompt belong to the
           player too. The boot script may already be spent after an earlier race. */
        int race = EE32(MENU) == LOAD_MENU && on.setup_done;
        if (player == 0 && race && (rematch_pad(out) || follow_pad(out) || host_next_pad(out) || test_skip_pad(out))) {
        } else if (player == 0 && race && !hle_env(&scripted_env, "DHD_NET_SCRIPTED")) {
            host_service_host_pad_device(0, GADDR(out));
            menu_mouse_merge(0, out);  /* the pause menu by mouse, under the same rules */
            guest_pad(out);
            race_chat_quick(out);
            /* no pause while the countdown waits for the others: it would hold this PC's GO */
            if (hle_online_countdown_hold(EE32(PHASE))) out->buttons &= ~PAD_START;
        }
        else if (!on.released || !host_pad_script(player, out)) neutral(out);
        for (int i = 0; i < 12; i++) out->pressure[i] = out->buttons & pressure_order[i] ? 255 : 0;
        return;
    }
    if (!host_pad_script(player, out)) {
        host_service_host_pad_device(player, GADDR(out));
        menu_mouse_merge(player, out);
    }
}
