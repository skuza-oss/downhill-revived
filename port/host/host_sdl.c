/* The host of host_api.h on every platform (port/host): the module's OS services that SDL3 and the
   C library give each one alike. Files, folders, the wall clock and waiting, the error box, the
   display, threads started and left, the console and the async services (the file hash and the
   lobby list). Paths are UTF-8, as SDL's are.

   What has no portable answer stays in the OS files, host_win32.c and host_posix.c: guest memory
   protection and the fault guard, the FP control word, the cycle counter, the program's path, code
   range and memory regions, the environment's writes, the heartbeat and the process's end; what
   this file takes from them is in host_os.h. Fibers, guest RAM, the FP mode, the guest-safe
   callback contracts and the thread name live in the host64 glue (fiber_*.c, guest_memory*.c,
   fp_mode.c, the generated imports_gen.c, port/hle/host.c). The module sees only host_api.h. */
#include "host_api.h"
#include "host_os.h"
#include "../hle/hle.h"
#include "../include/guest.h"
#include "../net/net.h"
#include "../render/include/dhd_disc.h"  /* the disc image's online check (port/render/disc) */

#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- environment, errors and the program ---- */
const char *host_getenv(const char *name) { return getenv(name); }

typedef struct { const char *title, *message; } ErrorBox;

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

static void show_error_box(void *box)
{
    const ErrorBox *b = box;
#if defined(__APPLE__) && TARGET_OS_IPHONE
    (void)b;
#else
    if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, b->title, b->message, NULL))
        fprintf(stderr, "(no error box: %s)\n", SDL_GetError());
#endif
}

/* The log carries it too; tool runs (DHD_HEADLESS) never wait on a box. The box is shown on the
   main thread (sdl_host_run), the one that runs SDL's events, on every platform: macOS shows a box
   only there, and asked from the game's thread SDL waited for a main thread that never ran it (a
   game with no disc image stayed up with no box until killed). Before that thread's loop is up the
   box is shown where it is asked for. With no display SDL shows nothing and returns at once, and the
   caller goes on to end the game. */
void host_error_box(const char *title, const char *message)
{
    fprintf(stderr, "%s: %s\n", title, message);
    fflush(stderr);
    if (getenv("DHD_HEADLESS")) return;
    ErrorBox box = {title, message};
    if (!sdl_host_run(show_error_box, &box)) show_error_box(&box);
}

void host_console_write(uint32_t address, uint32_t bytes)
{
    fwrite(GUEST(address), 1, bytes, stderr);
    fflush(stderr);
}

/* ---- display (fps.c, widescreen.c) ----
   The primary display's desktop mode, read on the SDL thread, which owns the video subsystem (the
   window's, present.c; started here when the window isn't there yet). */
typedef struct {
    int hz;
    double aspect;
} DisplayFacts;

static void display_read(void *out)
{
    DisplayFacts *d = out;
#if defined(__APPLE__) && TARGET_OS_IPHONE
    void ios_metal_get_drawable_size(int *out_w, int *out_h) __attribute__((weak));
    bool ios_metal_has_promotion_support(void) __attribute__((weak));
    int w = 0, h = 0;
    if (ios_metal_get_drawable_size) ios_metal_get_drawable_size(&w, &h);
    d->hz = (ios_metal_has_promotion_support && ios_metal_has_promotion_support()) ? 120 : 60;
    if (w > 0 && h > 0) d->aspect = (double)w / (double)h;
    else d->aspect = 16.0 / 9.0;
#else
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return;
    const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (!m) return;
    d->hz = (int)m->refresh_rate;  /* the whole hertz below, as the mode's own number was (143 at 143.86) */
    if (m->w > 0 && m->h > 0) d->aspect = (double)m->w / m->h;
#endif
}

static DisplayFacts display_facts(void)
{
    DisplayFacts d = {0, 0};
    sdl_host_run(display_read, &d);
    return d;
}

int host_display_hz(void)
{
#if defined(__APPLE__) && TARGET_OS_IPHONE
    int ios_metal_effective_fps(void);
    return ios_metal_effective_fps();
#else
    return display_facts().hz;
#endif
}

/* DHD_MONITOR_ASPECT where the starter knows better (Android's activity: the long side over the
   short), else the primary display's shape; 16 / 9 when unknown. */
double host_monitor_aspect(void)
{
#if defined(__APPLE__) && TARGET_OS_IPHONE
    double ios_metal_game_aspect(void);
    return ios_metal_game_aspect();
#endif
    const char *named = getenv("DHD_MONITOR_ASPECT");
    if (named && atof(named) >= 1.0 && atof(named) < 4.0) return atof(named);
    double aspect = display_facts().aspect;
    return aspect > 0 ? aspect : 16.0 / 9.0;
}

/* SDL's video declares the process per-monitor DPI aware (v2 on Windows) when it starts, before
   any window, and the display is read through it: nothing to ask for here. */
void host_dpi_aware(void) {}

/* ---- time and waiting: the wall clock (the game's own clock is virtual, kernel.c) ---- */
double host_wall_seconds(void)
{
    static Uint64 start, freq;
    if (!freq) {
        freq = SDL_GetPerformanceFrequency();
        start = SDL_GetPerformanceCounter();
    }
    return (double)(SDL_GetPerformanceCounter() - start) / (double)freq;
}

void host_sleep_ms(unsigned ms) { SDL_Delay(ms); }

/* Until a host_wall_seconds time: a sleep to a millisecond before it (SDL_DelayNS: a
   high-resolution waitable timer on Windows, one per thread, which woke up to 0.85 ms late,
   finding 052; nanosleep on POSIX; macOS: host_os_sleep_ns), then a pause-instruction spin. SDL_DelayPrecise ends with a
   short sleep of its own instead: 0.6 ms late at worst against 0.006 here (2026-10-27, 200 waits).
   A thread with a wait sink (host_wait_sink: port/gs/perf_log.c's game and render threads) adds the
   time it waited there, in nanoseconds: pacing is waiting, not work (finding 129). */
static _Thread_local _Atomic uint64_t *wait_sink;
void host_wait_sink(_Atomic uint64_t *nanoseconds) { wait_sink = nanoseconds; }

void host_wait_wall(double seconds)
{
    double start = host_wall_seconds(), now = start, left = seconds - start;
    if (left > 0.001) host_os_sleep_ns((Uint64)((left - 0.001) * 1e9));
    while ((now = host_wall_seconds()) < seconds)
        SDL_CPUPauseInstruction();
    if (wait_sink && now > start)
        atomic_fetch_add_explicit(wait_sink, (uint64_t)((now - start) * 1e9), memory_order_relaxed);
}

/* ---- threads: a fire-and-forget job (online_menu.c, the async services) ---- */
typedef struct {
    void (*fn)(void *);
    void *arg;
} DetachedStart;

static int detached_run(void *p)
{
    DetachedStart start = *(DetachedStart *)p;
    free(p);
    start.fn(start.arg);
    return 0;
}

int host_thread_detach(void (*fn)(void *), void *arg)
{
    DetachedStart *start = malloc(sizeof *start);
    SDL_Thread *t;
    if (!start) return 0;
    start->fn = fn;
    start->arg = arg;
    t = SDL_CreateThread(detached_run, "job", start);
    if (!t) {
        free(start);
        return 0;
    }
    SDL_DetachThread(t);
    return 1;
}

/* ---- files: the disc image and the memory card's files ----
   A file is a C library FILE behind a small handle (1..FILE_SLOTS, 0 is none); the disc image is
   read through it as the C library reads. A discarding handle (HOST_FILE_DISCARD) has no FILE:
   its writes go nowhere and it reads as empty. Any thread may open and close files and folders (the
   game thread, the texture pack's loaders): slots are taken and given back under slot_lock; a
   handle is then its owner's alone. */
#define FILE_SLOTS 32
static FILE *file_slot[FILE_SLOTS + 1];
static SDL_SpinLock slot_lock;
static char discard_file;  /* the slot's FILE for a discarding handle */
#define DISCARD ((FILE *)&discard_file)

static FILE *file_of(HostFile h) { return h > 0 && h <= FILE_SLOTS ? file_slot[h] : NULL; }

/* The game's own files (the memory card's saves\cardN folders, mc.c) are named with Windows
   backslashes: each path below has them turned into slashes, which every host takes. */
static const char *slashed(const char *path, char *buf, size_t size)
{
    if (!path || !strchr(path, '\\')) return path;
    snprintf(buf, size, "%s", path);
    for (char *c = buf; *c; c++)
        if (*c == '\\') *c = '/';
    return buf;
}
#define SLASHED(path) char path##_buf[4096]; path = slashed(path, path##_buf, sizeof path##_buf)

HostFile host_file_open(const char *path, unsigned mode)
{
    FILE *f;
    int slot;
    if ((mode & HOST_FILE_WRITE) && (mode & HOST_FILE_DISCARD)) f = DISCARD;
    else {
        SLASHED(path);
        const char *kind = !(mode & HOST_FILE_WRITE) ? "rb" : mode & HOST_FILE_TRUNCATE ? "w+b" : "r+b";
        f = host_os_fopen(path, kind);
        /* The module's WASI open shim has no preopened directories, so an absolute POSIX path
           arrives with its leading slash removed ("/tmp/x" as "tmp/x", wasi_adapter.c). A path
           that fails relative to the process directory is retried from the root; the retry only
           runs after the plain open already failed, so working opens are untouched. */
        if (!f && path[0] != '/') {
            char rooted[4097];
            snprintf(rooted, sizeof rooted, "/%s", path);
            f = host_os_fopen(rooted, kind);
        }
    }
    if (!f) return HOST_FILE_NONE;
    SDL_LockSpinlock(&slot_lock);
    for (slot = 1; slot <= FILE_SLOTS && file_slot[slot]; slot++)
        ;
    if (slot <= FILE_SLOTS) file_slot[slot] = f;
    SDL_UnlockSpinlock(&slot_lock);
    if (slot > FILE_SLOTS) {
        if (f != DISCARD) fclose(f);
        return HOST_FILE_NONE;
    }
    return slot;
}

void host_file_close(HostFile h)
{
    FILE *f = file_of(h);
    if (f) {
        if (f != DISCARD) fclose(f);
        SDL_LockSpinlock(&slot_lock);
        file_slot[h] = NULL;
        SDL_UnlockSpinlock(&slot_lock);
    }
}

int64_t host_file_read(HostFile h, void *dst, size_t size)
{
    FILE *f = file_of(h);
    size_t n;
    if (!f) return -1;
    if (f == DISCARD) return 0;
    static int profile = -1;
    if (profile < 0) profile = getenv("DHD_SPIKES") != NULL;
    double start = profile ? host_wall_seconds() : 0;
    n = fread(dst, 1, size, f);
    if (profile) fprintf(stderr, "spike_io: start=%.9f ms=%.6f bytes=%zu\n", start,
                         (host_wall_seconds() - start) * 1e3, n);
    return n < size && ferror(f) ? -1 : (int64_t)n;
}

size_t host_file_write(HostFile h, const void *src, size_t size)
{
    FILE *f = file_of(h);
    return !f ? 0 : f == DISCARD ? size : fwrite(src, 1, size, f);
}

int host_file_flush(HostFile h)
{
    FILE *f = file_of(h);
    return !f ? -1 : f == DISCARD ? 0 : fflush(f);
}

int host_file_seek(HostFile h, uint64_t offset)
{
    FILE *f = file_of(h);
    return !f ? -1 : f == DISCARD ? 0 : host_fseek(f, (int64_t)offset, SEEK_SET);
}

uint64_t host_file_size(HostFile h)
{
    FILE *f = file_of(h);
    int64_t here, end;
    if (!f || f == DISCARD || (here = host_ftell(f)) < 0) return 0;
    if (host_fseek(f, 0, SEEK_END) != 0) return 0;
    end = host_ftell(f);
    host_fseek(f, here, SEEK_SET);
    return end < 0 ? 0 : (uint64_t)end;
}

/* ---- paths and folders: the memory card's folders ---- */
int host_path_is_dir(const char *path)
{
    SDL_PathInfo info;
    SLASHED(path);
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

int host_path_exists(const char *path)
{
    SLASHED(path);
    return SDL_GetPathInfo(path, NULL);
}

/* Parents that are missing are made too. */
int host_dir_create(const char *path)
{
    SLASHED(path);
    return SDL_CreateDirectory(path) ? 1 : 0;
}

/* A folder's entries are named when it is opened (SDL's listing, then "." and ".." first, as the
   card and Windows list them) and looked at one by one. */
#define DIR_SLOTS 16
typedef struct {
    char path[4096];
    char **names;
    int count, at, cap;
} DirList;
static DirList *dir_slot[DIR_SLOTS + 1];

static SDL_EnumerationResult SDLCALL dir_name(void *user, const char *dirname, const char *name)
{
    DirList *d = user;
    (void)dirname;
    if (d->count == d->cap) {
        int cap = d->cap ? d->cap * 2 : 32;
        char **names = realloc(d->names, (size_t)cap * sizeof *names);
        if (!names) return SDL_ENUM_FAILURE;
        d->names = names, d->cap = cap;
    }
    if (!(d->names[d->count] = SDL_strdup(name))) return SDL_ENUM_FAILURE;
    d->count++;
    return SDL_ENUM_CONTINUE;
}

static void dir_free(DirList *d)
{
    for (int i = 0; i < d->count; i++) SDL_free(d->names[i]);
    free(d->names);
    free(d);
}

/* An SDL time (ns since 1970, UTC) as local time, as the card's directory entries show it. */
static void host_time_of(SDL_Time t, HostTime *out)
{
    SDL_DateTime dt;
    memset(out, 0, sizeof *out);
    if (!SDL_TimeToDateTime(t, &dt, true)) return;
    out->year = (uint16_t)dt.year, out->month = (uint16_t)dt.month, out->day = (uint16_t)dt.day;
    out->hour = (uint16_t)dt.hour, out->minute = (uint16_t)dt.minute, out->second = (uint16_t)dt.second;
}

/* Entry `at` of the list: -2 ".", -1 "..", then SDL's names. */
static void dir_entry(const DirList *d, int at, HostDirEntry *e)
{
    const char *name = at == -2 ? "." : at == -1 ? ".." : d->names[at];
    char full[4096 + HOST_PATH_MAX + 2];
    SDL_PathInfo info;
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name);
    if (at == -2) snprintf(full, sizeof full, "%s", d->path);
    else snprintf(full, sizeof full, "%s/%s", d->path, name);
    if (!SDL_GetPathInfo(full, &info)) return;
    e->is_dir = info.type == SDL_PATHTYPE_DIRECTORY;
    e->size = (uint32_t)info.size;
    host_time_of(info.create_time, &e->created);
    host_time_of(info.modify_time, &e->modified);
}

HostDir host_dir_open(const char *path, HostDirEntry *first)
{
    DirList *d;
    int slot;
    SLASHED(path);
    if (!host_path_is_dir(path) || !(d = calloc(1, sizeof *d))) return HOST_DIR_NONE;
    snprintf(d->path, sizeof d->path, "%s", path);
    if (!SDL_EnumerateDirectory(path, dir_name, d)) {
        dir_free(d);
        return HOST_DIR_NONE;
    }
    d->at = -2;
    dir_entry(d, d->at++, first);
    SDL_LockSpinlock(&slot_lock);
    for (slot = 1; slot <= DIR_SLOTS && dir_slot[slot]; slot++)
        ;
    if (slot <= DIR_SLOTS) dir_slot[slot] = d;
    SDL_UnlockSpinlock(&slot_lock);
    if (slot > DIR_SLOTS) {
        dir_free(d);
        return HOST_DIR_NONE;
    }
    return slot;
}

int host_dir_next(HostDir h, HostDirEntry *e)
{
    DirList *d = h > 0 && h <= DIR_SLOTS ? dir_slot[h] : NULL;
    if (!d || d->at >= d->count) return 0;
    dir_entry(d, d->at++, e);
    return 1;
}

void host_dir_close(HostDir h)
{
    if (h > 0 && h <= DIR_SLOTS && dir_slot[h]) {
        DirList *d = dir_slot[h];
        SDL_LockSpinlock(&slot_lock);
        dir_slot[h] = NULL;
        SDL_UnlockSpinlock(&slot_lock);
        dir_free(d);
    }
}

/* ---- async services: the disc image's online check and the lobby list (online_menu.c) ----
   The blocking part runs on a host thread; only results cross, as guest-address buffers the game
   thread copies at poll time. One job of each kind runs at a time. The atomic state is the whole
   protocol: the worker publishes its result before marking itself done, and the poller copies it
   before idling. */
enum { JOB_IDLE, JOB_WORKING, JOB_DONE, JOB_FAILED };
static SDL_AtomicInt disc_job;
static char disc_iso[1024], disc_cache[1024], disc_note[256];
static uint8_t disc_digest[32];
static int disc_answer;

static void disc_run(void *unused)
{
    (void)unused;
    disc_answer = dhd_disc_check(disc_iso, disc_cache, disc_digest, disc_note, sizeof disc_note);
    SDL_SetAtomicInt(&disc_job, JOB_DONE);
}

int host_service_disc_check_start(uint32_t iso, uint32_t cache)
{
    if (!iso || !cache) return 0;
    const char *at = GUEST(iso), *cache_at = GUEST(cache);
    if (strlen(at) >= sizeof disc_iso || strlen(cache_at) >= sizeof disc_cache) return 0;
    if (!SDL_CompareAndSwapAtomicInt(&disc_job, JOB_IDLE, JOB_WORKING)) return 0;
    snprintf(disc_iso, sizeof disc_iso, "%s", at);
    snprintf(disc_cache, sizeof disc_cache, "%s", cache_at);
    if (!host_thread_detach(disc_run, NULL)) {
        SDL_SetAtomicInt(&disc_job, JOB_IDLE);
        return 0;
    }
    return 1;
}

int host_service_disc_check_poll(uint32_t digest, uint32_t note, int size)
{
    int s = SDL_GetAtomicInt(&disc_job);
    if (s == JOB_WORKING) return 0;
    if (s != JOB_DONE) return -1;
    memcpy(GUEST(digest), disc_digest, sizeof disc_digest);
    if (size > 0) snprintf(GUEST(note), (size_t)size, "%s", disc_note);
    SDL_SetAtomicInt(&disc_job, JOB_IDLE);
    return disc_answer;
}

/* The lobby fetch: entries published here, copied to the guest buffer at poll time. */
#define ASYNC_LOBBY_MAX 64
static SDL_AtomicInt lobby_job;
static NetLobbyInfo lobby_list[ASYNC_LOBBY_MAX];
static int lobby_max, lobby_count;

static void lobby_run(void *unused)
{
    int n;
    (void)unused;
    n = net_list_lobbies(lobby_list, lobby_max);
    if (n >= 0) lobby_count = n;
    SDL_SetAtomicInt(&lobby_job, n < 0 ? JOB_FAILED : JOB_DONE);
}

int host_service_net_list_lobbies_start(int max)
{
    if (max <= 0 || max > ASYNC_LOBBY_MAX) return 0;
    if (!SDL_CompareAndSwapAtomicInt(&lobby_job, JOB_IDLE, JOB_WORKING)) return 0;
    lobby_max = max;
    lobby_count = -1;
    if (!host_thread_detach(lobby_run, NULL)) {
        SDL_SetAtomicInt(&lobby_job, JOB_IDLE);
        return 0;
    }
    return 1;
}

int host_service_net_list_lobbies_poll(uint32_t out, int max)
{
    int s = SDL_GetAtomicInt(&lobby_job), n;
    if (s == JOB_WORKING) return -2;
    if (s != JOB_DONE) {
        SDL_SetAtomicInt(&lobby_job, JOB_IDLE);
        return -1;
    }
    n = lobby_count < max ? lobby_count : max;
    if (n > 0) memcpy(GUEST(out), lobby_list, (size_t)n * sizeof lobby_list[0]);
    SDL_SetAtomicInt(&lobby_job, JOB_IDLE);
    return n;
}
