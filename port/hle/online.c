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
     AI rider (findings 044, 047). Room finishers stay to watch until the server closes the race
     or they choose BACK TO THE ROOM / LEAVE THE ROOM (finding 122). */
#include "hle.h"
#include "menu_page.h"
#include "../gs/present.h"
#include "../net/net.h"
#include "../include/version.h"
#include "../include/fps.h"
#include "voice_rules.h"

#include <ctype.h>
#include <math.h>
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
void hle_puppet_start(void);
extern int func_001FA7C0(int bike);                       /* a bike's sound kind */
extern void func_001B45F8(void *timer, int kind, int ticks);  /* the race-end countdown */

/* EVENT payloads (finding 047): byte 0 the version, byte 1 the type. Type 1 (a hit) is
   port/hle/puppet.c's. */
enum {
    EVENT_VERSION = 2, EVENT_HIT = 1, EVENT_LOADED = 2, EVENT_SETUP = 3, EVENT_START = 4, EVENT_GO = 5,
    EVENT_FINISH = 6, EVENT_RESULTS = 7,  /* 8 was 0.7.3's rematch (TRY AGAIN / NEW LEVEL in the race) */
    /* 9: the taunts branch's EVENT_TAUNT. 10: [length, text]: a race chat line (finding 125), so chat
       works on a server without a chat message; older clients drop it (puppet.c takes EVENT_HIT alone) */
    EVENT_CHAT = 10,
    EVENT_VOICE = 11 /* [sequence 16 bits, Opus]: voice chat (finding 131, port/host/voice.c's VOICE_EVENT) */
};
enum { SNAPSHOT_TICKS = 3 };  /* a snapshot every 3 race ticks: 20 a second at any frame rate */
enum { NRIDERS = 10, START_LEAD_MS = 1500 };
_Static_assert(2 + NRIDERS * 256 <= NET_FRAME_MAX, "all owned riders fit one STATE frame");
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
    double their_go[NET_MAX_SLOTS];         /* the others' */
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
    /* a room's race (from_menu): the room's last race when it started; when this PC leaves it for
       the room, and why (shown until then) */
    int watch_slot, watch_paused, watch_direction;
    int watched;                /* the camera has left this PC's rider: its HUD stays hidden */
    double watch_finished_at;
    unsigned ai_sent;
    int last_race_id;
    double leave_at;
    const char *leave_reason;
    int end_held, end_done;     /* the race-end countdown, held for the results */
    int no_bots;                /* the room's computer riders off (its "laps" option 1) */
    int cheats;                 /* the room's cheat codes on (option 2; off: hle_cheat_code) */
    unsigned away;              /* bit s: racer s lost its connection, the server holds its slot
                                   (NetLobby.racers_away, finding 126); its name tag says so */
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
        /* DHD_NET_JOIN: a bare code, or a whole join link (net_join_room takes the code out). */
        if (host_service_net_join_room(GADDR(join)) < 0) fail("that join value holds no code (a code or a link).");
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
        } else if (lobby.state == NET_RECONNECTING)
            /* the socket dropped and the net client is reconnecting into the same slot (finding
               126): keep waiting, the server holds it a moment */
            title("Downhill Revived online - room %s - reconnecting...", lobby.code);
        else
            title("Downhill Revived online - connecting...");
        if (lobby.state == NET_LOBBY && lobby.is_host) pressed_start();
        host_service_gs_pump_messages();
        host_sleep_ms(10);
    }
    kernel_pace_slip(host_wall_seconds() - entered);  /* the lobby wait isn't game time to catch up */
    /* a room's race can have one rider (START NOW with only them ready): no other player then */
    on.nslots = lobby.nslots < 1 ? 1 : lobby.nslots > NET_MAX_SLOTS ? NET_MAX_SLOTS : lobby.nslots;
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
static void race_chat_heard(int sender, const uint8_t *in, int n);
static int voice_command(const char *text);
static void voice_step(void);

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
                memcpy(&on.their_go[sender], e + 2, 8);
                if (on.go_server) go_skew(sender);
            }
            break;
        case EVENT_FINISH:
            if (n - 2 >= 7) read_finish(e[2], get32(e + 3));
            break;
        case EVENT_RESULTS:
            if (sender == on.host_slot) read_results(e, n - 2);
            break;
        case EVENT_CHAT:
            race_chat_heard(sender, e, n - 2);
            break;
        case EVENT_VOICE:   /* to the host's decoder, as the slot the server stamped */
            if (sender < on.nslots && sender != on.local_slot && !on.gone[sender])
                host_service_voice_frame(sender, GADDR(e), n - 2);
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
        title("Downhill Revived online - room %s - waiting for %d player%s riders", on.code, missing,
              missing == 1 ? "'s" : "s'");
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
   within a frame or two of each other on every PC. Both are said again every half second: the PC
   that loads first can be here before another has set up its race, which would drop a single
   message, and the start would wait 60 s.
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
    hle_puppet_start();  /* snapshots from before this start */
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
/* Out of the race without the player (finished, TIME UP, the host ended it): as START does
   (func_001B4900: the race loop ends with 2 and the race screen opens its pause menu), whose
   builder then leaves the race screen at once as QUIT's Yes does (port_online_pause_rows). While
   the race loop runs, after a finish too, no menu opens: func_0015E0C0 alone or a pushed menu
   does nothing there. Paused already, the menu opens again when the player resumes. */
static int quit_on_pause;
static void leave_race(void)
{
    quit_on_pause = 1;
    EE32(0x3F52D0 + 0x130) = 0;  /* the pad that paused (D_003F52D0.x130): this PC's */
    func_001B45F8(EE_PTR(0x4B1F40), 2, 0);
}

/* A race started outside a room (DHD_NET_CREATE / DHD_NET_JOIN, the tests' races): the host
   publishes the order once every human has finished or left, or this long after the first. */
enum { RESULTS_WAIT = 60 };

static void hud_text(int view, int y, int size, int r, int g, int b, const char *text)
{
    char saved[0x20];
    memcpy(saved, D_004F3570, sizeof saved);
    func_0020F588(view, 0, r, g, b, 128);
    func_0020FB80(0, 256 * size, 192 * size, -91 * size, -32 * size);
    func_0020F9F0(0, view, 256, y, text);
    memcpy(D_004F3570, saved, sizeof saved);
}

/* This PC's member of the room, or NULL. */
static const NetMember *self_member(const NetLobby *l)
{
    for (int i = 0; i < l->nmembers; i++)
        if (l->member[i].id == l->member_id) return &l->member[i];
    return NULL;
}

/* A room's race over each view's HUD while this PC's rider rides (finding 118): from the first
   human finish, who finished and the room's finish window counting down (the server's
   finishEndsAt), its last 10 s large; then why this PC's race ends (on.leave_reason). */
static void race_banner(int view)
{
    if (!on.started || !from_menu) return;
    if (on.leave_reason) {
        hud_text(view, 96, 2, 128, 40, 40, on.leave_reason);
        return;
    }
    NetLobby lobby;
    host_service_net_get_lobby(GADDR(&lobby));
    int finished = 0, first = -1;
    for (int i = 0; i < lobby.nmembers; i++) {
        const NetMember *m = &lobby.member[i];
        if (strcmp(m->race_state, "finished")) continue;
        finished++;
        if (first < 0 || m->finish_ticks < lobby.member[first].finish_ticks) first = i;
    }
    if (!finished) return;
    char text[64], who[32];
    if (finished > 1) snprintf(who, sizeof who, "%d FINISHED", finished);
    else snprintf(who, sizeof who, "%.12s FINISHED", lobby.member[first].name);
    for (char *c = who; *c; c++) *c = toupper((unsigned char)*c);
    if (!lobby.finish_ends_at) {
        snprintf(text, sizeof text, "%s - NO TIME LIMIT", who);
        hud_text(view, 40, 1, 128, 128, 128, text);
        return;
    }
    double ms = lobby.finish_ends_at - host_service_net_server_ms();
    int left = ms > 0 ? (int)((ms + 999) / 1000) : 0;
    if (left <= 0) {
        hud_text(view, 96, 2, 128, 40, 40, FINISHED(0) ? "TIME UP" : "TIME UP - DNF");
        return;
    }
    snprintf(text, sizeof text, "%s - RACE ENDS IN %d:%02d", who, left / 60, left % 60);
    hud_text(view, 40, 1, 128, 128, 128, text);
    if (left <= 10) {
        snprintf(text, sizeof text, "%d", left);
        hud_text(view, 96, 3, 128, 110, 40, text);
    }
}

static const char *rider_label(int global, char *out, int size);

/* The view-0 HUD describes this PC's rider: hidden once the camera watches another (its finish
   place still shows for the first seconds). */
int hle_online_watch_hud(void) { return online_mode() && from_menu && on.end_held && on.watched; }

static void race_chat_draw(int view);
static void voice_draw(int view);

void hle_online_hud(int view)
{
    if (online_mode()) {
        race_chat_draw(view);
        voice_draw(view);
        race_banner(view);
    }
    if (online_mode() && from_menu && on.end_held && on.watch_slot) {
        char name[48], text[96];
        rider_label(hle_puppet_global(on.watch_slot), name, sizeof name);
        snprintf(text, sizeof text, "WATCHING %s", name);
        for (char *c = text; *c; c++) *c = toupper((unsigned char)*c);
        hud_text(view, 164, 1, 128, 128, 128, text);
        hud_text(view, 180, 1, 128, 128, 128, "LEFT / RIGHT - CHANGE RIDER");
    }
    if (!online_mode() || on.started || !on.start_entered || !on.start_missing) return;
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
    if (!online_mode() || !on.started || (!from_menu && on.have_results)) return 0;
    on.end_held = 1, on.end_held_at = host_wall_seconds();
    fprintf(stderr, "online: finished; the results screen waits for the others (camera mode %d)\n",
            EE32(0x37EA60 + 0x2B8));
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
                /* this PC's own rider, in a room: its time and place for the room's results */
                if (slot == 0 && from_menu) host_service_net_finish((uint32_t)FINISH_TIME(0), PLACE(0));
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
    if (waiting && host_wall_seconds() - on.first_finish < RESULTS_WAIT) return;
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

/* Camera 0 is the 0x380-byte view, NOT the RiderCamera at rider +0x3530.
   Its +0xC0 is a direction vector. Leave the rider's animation camera links alone. */
extern void func_00148560(void *, int);
extern void func_00148860(void *);

void hle_online_watch_resume(void) { on.watch_paused = on.watch_direction = 0; }

static void watch_cut(int slot)
{
    if (slot == on.watch_slot) return;
    on.watch_slot = slot;
    on.watch_finished_at = 0;
    if (slot) on.watched = 1;
    char name[48];
    fprintf(stderr, "online: watching %s (slot %d)\n",
            rider_label(hle_puppet_global(slot), name, sizeof name), slot);
}

/* Run immediately before the view update: finish/crash states may have changed it. */
void hle_online_watch_camera(void *camera)
{
    static HleEnv test_camera, test_slot;
    int slot = on.watch_slot;
    const char *test = hle_env(&test_camera, "DHD_PUPPET_CAMERA");
    if (!slot && test && *test == '1') {
        const char *value = hle_env(&test_slot, "DHD_PUPPET_SLOT");
        slot = value ? atoi(value) : 1;
    }
    if (camera != EE_PTR(0x37EA60) || slot < 0 || slot >= hle_online_race_riders()) return;
    if (!slot && !(from_menu && on.end_held)) return;
    if (on.watch_paused || EE32(0x398C40 + 0x5BE4)) return; /* the race loop's pause state */
    int cut = *(uint8_t **)((char *)camera + 0x2B4) != RIDER(slot);
    *(uint8_t **)((char *)camera + 0x2B4) = RIDER(slot);
    if (cut) fprintf(stderr, "online: camera cut slot %d, mode %d -> 1\n", slot, EE32(0x37EA60 + 0x2B8));
    if (cut || EE32(0x37EA60 + 0x2B8) != 1) {
        /* +0x358 selects the existing view node at reset. Use the rider-relative
           branch for this cut, then restore the normal follow setting. */
        int use_view_node = EE32(0x37EA60 + 0x358);
        EE32(0x37EA60 + 0x358) = 0;
        func_00148560(camera, 1);
        func_00148860(camera);
        EE32(0x37EA60 + 0x358) = use_view_node;
    }
}

/* A human's room state wins over the delayed puppet finish flag. */
static int watch_state(const NetLobby *lobby, int slot)
{
    int global = hle_puppet_global(slot);
    if (global < on.nslots) {
        for (int i = 0; i < lobby->nmembers; i++) {
            const NetMember *m = &lobby->member[i];
            if (m->id != lobby->racer[global].member_id) continue;
            if (!m->in_race || !strcmp(m->race_state, "dnf")) return -1;
            return !strcmp(m->race_state, "racing") ? 1 : 0;
        }
        return -1;
    }
    return !FINISHED(slot);
}

static void watch_cycle(const NetLobby *lobby, int direction)
{
    int order[NRIDERS], count = 0, current = -1;
    for (int slot = 1; slot < hle_online_race_riders(); slot++)
        if (watch_state(lobby, slot) == 1) order[count++] = slot;
    for (int i = 1; i < count; i++) {
        int slot = order[i], j = i;
        while (j && PLACE(order[j - 1]) > PLACE(slot)) { order[j] = order[j - 1]; j--; }
        order[j] = slot;
    }
    for (int i = 0; i < count; i++) if (order[i] == on.watch_slot) current = i;
    if (count) watch_cut(order[current < 0 ? (direction > 0 ? 0 : count - 1)
                                     : (current + direction + count) % count]);
    else watch_cut(0);
}

static void watch_step(const NetLobby *lobby)
{
    if (!on.end_held || host_wall_seconds() - on.end_held_at < 3) return;
    int state = on.watch_slot ? watch_state(lobby, on.watch_slot) : -1;
    if (state == 1) return;
    double now = host_wall_seconds();
    if (state == 0) {
        if (!on.watch_finished_at) on.watch_finished_at = now;
        if (now - on.watch_finished_at < 3) return;
    }
    int best = 0;
    for (int slot = 1; slot < hle_online_race_riders(); slot++)
        if (hle_puppet_global(slot) < on.nslots && watch_state(lobby, slot) == 1 &&
            (!best || PLACE(slot) < PLACE(best))) best = slot;
    watch_cut(best);
}

/* ---- disconnects ---- */

/* A player who left: their rider goes on as the host's AI (the lowest lobby slot still here is
   the host; it runs the AI riders), unless they had finished: then it stays where it is, finished
   (hle_puppet_drop). If this PC lost the connection, it runs every rider itself. */
static void check_left(void)
{
    NetLobby lobby;
    host_service_net_get_lobby(GADDR(&lobby));
    on.away = lobby.racers_away;
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
    if (EE32(MENU) != LOAD_MENU) return;
    if (race_running() && !on.go_sent) {  /* GO: when it was, on the server's clock */
        uint8_t e[10] = {EVENT_VERSION, EVENT_GO};
        on.go_server = host_service_net_server_ms() - port_field_seconds(clock) * 1000;
        memcpy(e + 2, &on.go_server, 8);
        send_event(e, sizeof e);
        on.go_sent = 1;
        for (int j = 0; j < on.nslots; j++)
            if (on.their_go[j]) go_skew(j);
    }
    static double checked;
    if (host_wall_seconds() - checked >= 0.25) { checked = host_wall_seconds(); check_left(); }
    finish_step();
    if (from_menu && !on.end_done) {
        /* Finishers stay to watch; the server closes the race for everyone. */
        double now = host_wall_seconds();
        NetLobby lobby;
        host_service_net_get_lobby(GADDR(&lobby));
        const NetMember *me = self_member(&lobby);
        int out = lobby.state != NET_RACING || !me || !me->in_race;
        if (!on.leave_at && out) {
            on.leave_reason = FINISHED(0) ? "RACE OVER" : lobby.last_race_id != on.last_race_id
                              ? "TIME UP - DNF" : "THE HOST ENDED THE RACE";
            fprintf(stderr, "online: race closed: back to the room\n");
            on.leave_at = now + 2;
        }
        watch_step(&lobby);
        if (on.leave_at && now >= on.leave_at) {
            on.end_done = 1, on.end_done_at = now;
            leave_race();
        }
    } else if (on.end_held && !on.end_done &&
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
    int at = 0, riders = 0, ai = 0;
    for (int slot = 0; slot < hle_online_race_riders(); slot++) {
        int room = (int)sizeof payload - at - 1;
        if ((n = hle_puppet_pack(slot, payload + at + 1, room < 255 ? room : 255)) > 0) {
            payload[at] = (uint8_t)n;
            if (slot == 0) record(payload + at + 1, n);
            at += 1 + n, riders++;
            if (slot && hle_puppet_owned(slot)) ai++;
        }
    }
    if (at && host_service_net_send_frame(NET_FRAME_STATE, GADDR(payload), at)) {
        on.sent += riders;
        on.ai_sent += ai;
    }
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
        if (on.end_held) fprintf(stderr, "online: watching: %u owned AI snapshots sent, phase %d\n", on.ai_sent, EE32(PHASE));
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
   queued colour so later HUD text keeps its own size, alignment and colour.
   A tag is the player's colour (by race slot, the same on every PC), full size and opaque up
   to NAME_NEAR, then smaller and fainter to nothing at NAME_FAR, so the riders far down the
   hill don't cover the track with names. A player whose connection dropped (the server holds
   the slot) reads "NAME - AWAY", grey, until it is back or has left. */
enum { NAME_NEAR = 45, NAME_FAR = 150, NAME_SMALLEST = 70 /* % at NAME_FAR */ };
static const uint8_t name_colour[NET_MAX_SLOTS][3] = {
    {80, 112, 128}, {128, 96, 48}, {88, 128, 80}, {128, 84, 116},
    {128, 120, 64}, {64, 124, 128}, {108, 88, 128}, {128, 76, 68}};
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
        if (slot == on.watch_slot) continue;
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
        /* nearer than NAME_NEAR as it was; beyond, smaller and fainter */
        float far = base[2] <= NAME_NEAR ? 0 : (base[2] - NAME_NEAR) / (float)(NAME_FAR - NAME_NEAR);
        int size = 100 - (int)((100 - NAME_SMALLEST) * far), alpha = (int)(128 * (1 - far));
        int away = global < 32 && on.away >> global & 1;
        if (away) alpha /= 2;
        if (alpha < 8) continue;
        char label[48];
        int talking = host_service_voice_state(VOICE_STATE_TALKING, global);  /* voice chat (finding 131) */
        snprintf(label, sizeof label, away ? "%s - AWAY" : talking ? "%s - TALKING" : "%s", on.name[global]);
        func_0020FB80(0, 320 * size / 100, 240 * size / 100, -102 * size / 100, -24 * size / 100);
        int half_width = (func_0020F860(0, label) + 31) / 32;
        if (local_x - half_width * port_fov_x < 1 || local_x + half_width * port_fov_x >= w - 1 || y < 1) continue;
        const uint8_t *c = name_colour[global % NET_MAX_SLOTS];
        func_0020F518(0, 0, 0, 0, alpha);
        func_0020F9F0(0, view, x - 1, y, label);
        func_0020F9F0(0, view, x + 1, y, label);
        func_0020F9F0(0, view, x, y - 1, label);
        func_0020F9F0(0, view, x, y + 1, label);
        if (away) func_0020F518(0, 96, 96, 96, alpha);
        else func_0020F518(0, c[0], c[1], c[2], alpha);
        func_0020F9F0(0, view, x, y, label);
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
   hle_online_hud's own prompt is drawn.

   A race's lines travel as EVENT_CHAT frames, which every server relays to the race as they are
   (the official one has no chat message of its own), with the sender's slot the server stamps
   in the frame: the name shown is that slot's, never one the frame claims. The server does not
   send a frame back to its sender, so this PC shows its own line as it sends it. At most
   CHAT_BURST lines each CHAT_WINDOW seconds a player, sent or taken. Outside a race (the room)
   lines go by the server's chat message, where it has one. */
enum { RACE_CHAT_SHOWN = 5, RACE_CHAT_SECONDS = 8, RACE_CHAT_KEY = 'Y', RACE_CHAT_TEXT = 56 };
enum { QUICK_CHAT = PAD_L1 | PAD_R1 | PAD_L2 | PAD_R2 };
enum { CHAT_BURST = 5, CHAT_WINDOW = 10 };
static struct {
    unsigned seen;          /* the newest chat line taken */
    int seen_valid, key_held, typing, quick_held;
    char entry[NET_CHAT_MAX + 1];
    struct { char text[RACE_CHAT_TEXT]; int own, note; double until; } shown[RACE_CHAT_SHOWN];  /* oldest first */
} race_chat;
/* The menu the race screen has open (OPEN_MENU holds its object; RIDING 0 while a menu is up):
   race chat's quick lines belong to the pause menu. */
enum { RIDING_WORD = 0x29E290, OPEN_MENU = 0x3F5420, PAUSE_MENU = 0x38BC00 };
static int menu_open(int menu)
{
    return EE32(RIDING_WORD) == 0 && EE32(OPEN_MENU) == (int32_t)(uintptr_t)EE_PTR(menu);
}
void func_0020F9A8(int pen, int view, int x, int y, const char *text);  /* left-aligned */

/* A line at the bottom of the shown ones, the oldest gone. */
static void race_chat_add(const char *name, const char *text, int member, int own)
{
    char shown[NET_CHAT_NAME + NET_CHAT_TEXT + 4];
    memmove(race_chat.shown, race_chat.shown + 1, sizeof race_chat.shown[0] * (RACE_CHAT_SHOWN - 1));
    port_chat_show(shown, sizeof shown, name, text, member);
    if (strlen(shown) >= RACE_CHAT_TEXT) memcpy(shown + RACE_CHAT_TEXT - 3, "..", 3);
    snprintf(race_chat.shown[RACE_CHAT_SHOWN - 1].text, RACE_CHAT_TEXT, "%s", shown);
    race_chat.shown[RACE_CHAT_SHOWN - 1].own = own;
    race_chat.shown[RACE_CHAT_SHOWN - 1].note = member < 0;
    race_chat.shown[RACE_CHAT_SHOWN - 1].until = host_wall_seconds() + RACE_CHAT_SECONDS;
}

/* 1 when slot (NET_MAX_SLOTS: this PC) may say one more line now: CHAT_BURST in CHAT_WINDOW s. */
static int chat_allowed(int slot)
{
    static double said[NET_MAX_SLOTS + 1][CHAT_BURST];
    double now = host_wall_seconds(), *t = said[slot], *oldest = t;
    for (int i = 1; i < CHAT_BURST; i++)
        if (t[i] < *oldest) oldest = &t[i];
    if (*oldest && now - *oldest < CHAT_WINDOW) return 0;
    *oldest = now;
    return 1;
}

/* This PC's line: in a race to the race as an EVENT_CHAT (shown here at once), else by the
   server's chat message. 1 when it went. */
static int race_chat_say(const char *text)
{
    if (voice_command(text)) return 1;
    if (!on.started || !host_service_net_racing()) return host_service_net_send_chat(GADDR(text));
    if (!chat_allowed(NET_MAX_SLOTS)) {
        race_chat_add("", "TOO MANY LINES: WAIT A MOMENT", -1, 0);
        return 0;
    }
    uint8_t e[3 + NET_CHAT_MAX] = {EVENT_VERSION, EVENT_CHAT};
    int n = (int)strlen(text);
    if (n > NET_CHAT_MAX) n = NET_CHAT_MAX;
    e[2] = (uint8_t)n;
    memcpy(e + 3, text, (size_t)n);
    send_event(e, 3 + n);
    race_chat_add(on.local_slot >= 0 && on.local_slot < NET_MAX_SLOTS ? on.name[on.local_slot] : "", text, 0, 1);
    return 1;
}

/* Another player's line (EVENT_CHAT): the name is the slot's the server stamped; the text is
   cleaned to the game font's letters when shown (port_chat_show). */
static void race_chat_heard(int sender, const uint8_t *in, int n)
{
    char text[NET_CHAT_MAX + 1];
    if (n < 3 || sender < 0 || sender >= on.nslots || sender == on.local_slot) return;
    int len = in[2];
    if (len < 1 || len > NET_CHAT_MAX || len > n - 3) return;   /* malformed: dropped */
    if (!chat_allowed(sender)) return;                          /* a flood: the extra lines dropped */
    memcpy(text, in + 3, (size_t)len);
    text[len] = 0;
    race_chat_add(on.name[sender], text, sender, 0);
    fprintf(stderr, "online: chat from player %d (%s): %s\n", sender, on.name[sender], text);
}

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
    if (action > 0 && race_chat.entry[0]) race_chat_say(race_chat.entry);
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
        if (pressed & buttons[i] && race_chat_say(port_chat_presets[i])) menu_play("S_SELECT");
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

/* ---- voice chat (finding 131, port/host/voice.c) ----
   The host captures, encodes, sends, decodes and mixes; here each frame decides who is heard and
   how, from this PC's view of the race (the server relays a race's frames to all of it: it can't
   choose by distance, as it has no positions):
   - racers hear racers within the voice range (VOICE_RANGE_M, DHD_VOICE_RANGE metres; a race's
     world is about UNITS_PER_METRE units a metre: a rider at 38 mph moves about 91 units a second),
     full near (a third of the range), quieter to VOICE_EDGE at the range; one heard stays heard
     until VOICE_HYSTERESIS times the range, so a voice at the edge doesn't cut in and out;
   - finished players (watching the rest) hear each other at full level, wherever they are, and
     neither group hears the other;
   - a player who left, or this PC's own voice, is never heard;
   - the pan is the side of this player's view the speaker is on (behind it, the middle).
   "/MUTE NAME" and "/UNMUTE NAME" in chat mute a player for this PC (race_chat_say). */
enum { VOICE_RANGE_M = 50 };
static struct { int heard[NET_MAX_SLOTS], active; } voice;

static float voice_range_units(void)
{
    static HleEnv range_env;
    const char *v = hle_env(&range_env, "DHD_VOICE_RANGE");
    int m = v && *v ? atoi(v) : VOICE_RANGE_M;
    if (m < 5) m = 5;
    if (m > 1000) m = 1000;
    return m * VOICE_UNITS_PER_METRE;
}

static void voice_step(void)
{
    int racing = online_mode() && on.started && EE32(MENU) == LOAD_MENU && on.setup_done;
    if (!racing) {
        if (voice.active) host_service_voice_drop(-1), memset(&voice, 0, sizeof voice);
        return;
    }
    voice.active = 1;
    float range = voice_range_units();
    uintptr_t mine = (uint32_t)*(int32_t *)(RIDER(0) + 0x792C);
    int me_done = on.local_slot >= 0 && on.local_slot < NRIDERS && on.finish[on.local_slot] >= 0;
    int cx, cy, w, h;
    func_0023D618(0, &cx, &cy);
    func_0023D650(0, &w, &h);
    for (int g = 0; g < on.nslots && g < NET_MAX_SLOTS; g++) {
        float gain = 0, pan = 0;
        int slot = hle_puppet_local(g);
        int done = g < NRIDERS && on.finish[g] >= 0, group = voice_group(me_done, done);
        if (g == on.local_slot || on.gone[g] || slot <= 0 || slot >= hle_online_race_riders() || !group) {
            voice.heard[g] = 0;
        } else if (group == 2) {   /* the finished: one group, no distance */
            voice.heard[g] = 1, gain = 1;
        } else {
            uintptr_t theirs = (uint32_t)*(int32_t *)(RIDER(slot) + 0x792C);
            if (theirs && mine) {
                float at[4] __attribute__((aligned(16))), me[4], screen[4] __attribute__((aligned(16)));
                memcpy(at, (void *)(theirs + 0x10), sizeof at);
                memcpy(me, (void *)(mine + 0x10), sizeof me);
                float dx = at[0] - me[0], dy = at[1] - me[1], dz = at[2] - me[2], d2 = dx * dx + dy * dy + dz * dz;
                voice.heard[g] = voice_in_range(d2, range, voice.heard[g]);   /* squared: no root to decide */
                if (voice.heard[g]) {
                    gain = voice_gain(sqrtf(d2), range);   /* in the hysteresis band: as at the edge */
                    func_00222D48(0, at, screen);
                    if (screen[2] >= 1 && w > 0) {
                        pan = (screen[0] - cx) / (w / 2.0f);
                        pan = (pan < -1 ? -1 : pan > 1 ? 1 : pan) * 0.8f;
                    }
                }
            } else voice.heard[g] = 0;
        }
        host_service_voice_place(g, (int)(gain * 1000), (int)(pan * 1000));
    }
}

/* a starts with b, in any case */
static int starts_with(const char *a, const char *b)
{
    for (; *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
    return 1;
}

/* "/MUTE NAME" or "/UNMUTE NAME" (any case, a name's start is enough): 1 when the line was one. */
static int voice_command(const char *text)
{
    int mute = starts_with(text, "/MUTE "), unmute = starts_with(text, "/UNMUTE ");
    if (!mute && !unmute) return 0;
    const char *who = text + (mute ? 6 : 8);
    char note[NET_CHAT_TEXT];
    for (int g = 0; g < on.nslots && g < NET_MAX_SLOTS; g++)
        if (g != on.local_slot && on.name[g][0] && *who && starts_with(on.name[g], who)) {
            host_service_voice_mute(g, mute);
            snprintf(note, sizeof note, "%s %s", on.name[g], mute ? "MUTED" : "UNMUTED");
            race_chat_add("", note, -1, 0);
            return 1;
        }
    snprintf(note, sizeof note, "NO PLAYER %s", who);
    race_chat_add("", note, -1, 0);
    return 1;
}

/* The microphone in this player's view: MIC while it sends, NO MICROPHONE when the talk key finds
   none, at the top left under the race's own HUD text. */
static void voice_draw(int view)
{
    if (!host_service_voice_state(VOICE_STATE_ENABLED, 0) || !voice.active) return;
    int sending = host_service_voice_state(VOICE_STATE_TRANSMITTING, 0);
    int failed = host_service_voice_state(VOICE_STATE_MIC, 0) < 0 &&
                 host_service_key_held(host_service_voice_state(VOICE_STATE_KEY, 0));
    if (!sending && !failed) return;
    int w, h;
    func_0023D650(view, &w, &h);
    if (w <= 0 || h <= 0) w = 512, h = 224;
    char saved[0x20];
    memcpy(saved, D_004F3570, sizeof saved);
    func_0020FB80(0, 205, 154, -73, -26);
    if (sending) race_chat_line(view, 24, h * 64 / 224, "MIC", 64, 128, 64, 128);
    else race_chat_line(view, 24, h * 64 / 224, "NO MICROPHONE", 128, 64, 64, 128);
    memcpy(D_004F3570, saved, sizeof saved);
    func_0020F420(0, view);
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

/* DHD_NET_TEST_SKIP=seconds (tests): START that long into each flight over the track (race phase
   5) still on the screen, as a player skipping it would. One press of 0.08 s. */
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

/* Leaving the race screen and the room returns this PC to offline control. Restore the previous
   script and seed so the online boot script cannot swallow title-menu input. */
static int connected;
static void race_over(void)
{
    fprintf(stderr, "online: the race is over; back to offline play\n");
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

/* ---- between races: the room again (finding 118) ----
   A race started from the ONLINE menu's room ends for this PC when it leaves the race screen (the
   server closes the race, BACK TO THE ROOM, or END THE RACE): the race's state goes as race_over's does, but the
   connection and the online unlocks stay, and the shell opens the room again over the rider screen
   (online_menu.c port_online_back_to_room). LEAVE THE ROOM leaves as QUIT did (race_over). */
void port_online_back_to_room(void);
static int leaving_room;  /* LEAVE THE ROOM chosen: off the race screen, out of the room too */

static void back_to_room(void)
{
    fprintf(stderr, "online: this PC's race is over; back to the room\n");
    host_service_net_back();  /* out of the race: unless it finished, DNF (gave up) */
    hle_puppet_offline();
    kernel_pace_shared(0);
    set_env("DHD_NAV", saved_nav);
    set_env("DHD_RACE_SEED", saved_seed);
    memset(&on, 0, sizeof on);
    online = 0, from_menu = 0, connected = 0;
    selected_ride = 0;
    host_service_gs_window_title(GADDR("Downhill Revived"));
    port_online_back_to_room();
}

static void race_left(void)
{
    host_service_voice_drop(-1);   /* the race's voices go with it */
    int leave = leaving_room || !from_menu;
    leaving_room = 0;
    quit_on_pause = 0;  /* left by the player's own row first: not the next race's first pause */
    if (leave) race_over();
    else back_to_room();
}

/* The race's pause menu in a room (patched func_0015E238 / func_0015E448): RESUME, BACK TO THE
   ROOM, END THE RACE (the host's), OPTIONS, LEAVE THE ROOM. The game's ids for RESUME (0x19) and
   OPTIONS (0x48) keep their own accept; the room's rows are strings, accepted here by index. Each
   of the room's rows asks first in the game's own quit question (D_00387F90: No / Yes, patched
   func_0015DE40 / func_0015DED0), whose Yes leaves the race screen as QUIT's does. */
enum { PAUSE_RESUME, PAUSE_ROOM, PAUSE_END, PAUSE_OPTIONS, PAUSE_LEAVE, PAUSE_KINDS };
static unsigned char pause_kind[16];
static int pause_count, pause_asked;  /* pause_asked: the row the question asks about */
extern char D_0029DE68[], D_003F52D0[], D_00387F90[];
void func_00190660(void *manager, void *screen);  /* open a popup over the others */
void func_0018E3A8(void *menu, int id, void *table, int enabled);
void func_0018E408(void *menu, const char *text, int enabled);

static void pause_row(void *menu, int kind)
{
    static const char *const text[PAUSE_KINDS] = {NULL, "BACK TO THE ROOM", "END THE RACE", NULL, "LEAVE THE ROOM"};
    if (pause_count >= (int)sizeof pause_kind) return;
    pause_kind[pause_count++] = (unsigned char)kind;
    if (kind == PAUSE_RESUME) func_0018E3A8(menu, 0x19, D_0029DE68, 1);
    else if (kind == PAUSE_OPTIONS) func_0018E3A8(menu, 0x48, D_0029DE68, 1);
    else func_0018E408(menu, text[kind], 1);
}

void func_0015E0C0(void);  /* QUIT's Yes (D_00388E90's builder): leave the race screen */

int port_online_pause_rows(void *menu)
{
    on.watch_paused = 1;
    pause_count = pause_asked = 0;
    if (!online_mode() || !from_menu) return 0;
    if (quit_on_pause) {  /* leave_race: no rows, out at once */
        quit_on_pause = 0;
        func_0015E0C0();
        return 1;
    }
    NetLobby lobby;
    host_service_net_get_lobby(GADDR(&lobby));
    pause_row(menu, PAUSE_RESUME);
    pause_row(menu, PAUSE_ROOM);
    if (lobby.is_host) pause_row(menu, PAUSE_END);
    pause_row(menu, PAUSE_OPTIONS);
    pause_row(menu, PAUSE_LEAVE);
    return 1;
}

int port_online_pause_accept(int index)
{
    if (!pause_count || index < 0 || index >= pause_count) return 0;
    int kind = pause_kind[index];
    if (kind == PAUSE_RESUME || kind == PAUSE_OPTIONS) return 0;  /* the game's own */
    pause_asked = kind;
    func_00190660(D_003F52D0, D_00387F90);
    return 1;
}

/* The quit question's title and the line under it (MenuItem, include/menu.h) for the asked row. */
typedef struct { short x, y, id; char enabled, placed; const char *string; void *table; } MenuText;

void port_online_question(void *menu)
{
    if (!pause_asked) return;
    MenuText *title = (MenuText *)((char *)menu + 0x438), *line = (MenuText *)((char *)menu + 0x448);
    const char *ask = pause_asked == PAUSE_END ? "END THE RACE FOR EVERYONE?"
                    : pause_asked == PAUSE_LEAVE ? "LEAVE THE ROOM?" : "BACK TO THE ROOM?";
    const char *why = pause_asked == PAUSE_END ? "NO RESULTS FROM THIS RACE WILL COUNT"
                    : pause_asked == PAUSE_LEAVE ? "YOU LEAVE THE ROOM AND THIS RACE"
                    : FINISHED(0) ? "THE OTHERS ARE STILL RACING" : "YOU WON'T FINISH THIS RACE (DNF)";
    title->id = 1, title->string = ask, title->table = D_0029DE68;
    line->id = 1, line->string = why, line->table = D_0029DE68;
}

void port_online_question_yes(void)
{
    if (!pause_asked) return;
    fprintf(stderr, "online: pause menu: %s\n", pause_asked == PAUSE_ROOM ? "back to the room"
            : pause_asked == PAUSE_END ? "the host ends the race" : "leave the room");
    if (pause_asked == PAUSE_END) host_service_net_end_race();
    if (pause_asked == PAUSE_LEAVE) leaving_room = 1;
    pause_asked = 0;
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
        /* the room's last race now, before loading: a race that closes later (TIME UP) changes
           it, a voided one (the host ended it) doesn't. Not at GO: a late GO can come after. */
        NetLobby lobby;
        host_service_net_get_lobby(GADDR(&lobby));
        on.last_race_id = lobby.last_race_id;
    }
    if (!on.started && on.setup_done && EE32(MENU) == LOAD_MENU && EE32(PHASE) == 1) {
        if (start_step()) {
            on.started = 1;
            cheats_start();
        }
    }
    if (on.started) state_sync();
    race_chat_step();
    voice_step();
    if (on.started && EE32(MENU) != LOAD_MENU) race_left();
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
        if (player == 0 && race && test_skip_pad(out)) {
        } else if (player == 0 && race && !hle_env(&scripted_env, "DHD_NET_SCRIPTED")) {
            host_service_host_pad_device(0, GADDR(out));
            menu_mouse_merge(0, out);  /* the pause menu by mouse, under the same rules */
            race_chat_quick(out);
            /* no pause while the countdown waits for the others: it would hold this PC's GO */
            if (hle_online_countdown_hold(EE32(PHASE))) out->buttons &= ~PAD_START;
        }
        else if (!on.released || !host_pad_script(player, out)) neutral(out);
        if (player == 0 && from_menu && on.end_held) {
            int direction = (out->buttons & PAD_RIGHT || out->left_x > 176) ? 1
                          : (out->buttons & PAD_LEFT || out->left_x < 80) ? -1 : 0;
            if (!on.watch_paused && !EE32(0x398C40 + 0x5BE4) && direction && !on.watch_direction && !(out->buttons & PAD_START)) {
                NetLobby lobby;
                host_service_net_get_lobby(GADDR(&lobby));
                watch_cycle(&lobby, direction);
            }
            on.watch_direction = direction;
        }
        for (int i = 0; i < 12; i++) out->pressure[i] = out->buttons & pressure_order[i] ? 255 : 0;
        return;
    }
    if (!host_pad_script(player, out)) {
        host_service_host_pad_device(player, GADDR(out));
        menu_mouse_merge(player, out);
    }
}
