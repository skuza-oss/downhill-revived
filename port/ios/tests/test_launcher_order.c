/* The iOS launcher's settings order (port/ios/host/launcher_calls.c, finding 227): the page's
   save_setting calls and PLAY, ticketed where they arrive and run on several threads at once, as the
   app's web view handler dispatches them. The settings writer here reads, changes and writes the
   whole file with a pause in the middle, as a read-modify-write does: without the order, saves made
   at once lose each other's keys. Every key must be in the file at the end, PLAY must see every save
   sent before it, and a save after PLAY is refused. GPL-3.0-or-later. */
#include "../host/launcher_calls.h"
#include "net/json.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static void nap(int ms) { Sleep((DWORD)ms); }
#else
#include <pthread.h>
#include <unistd.h>
static void nap(int ms) { usleep((useconds_t)ms * 1000); }
#endif

enum { SAVES = 24 };
static char ini_path[600];

/* ---- the shared code, a read-modify-write writer with a pause in the middle ---- */
static int count_keys(void)
{
    FILE *f = fopen(ini_path, "r");
    int n = 0;
    char line[256];
    while (f && fgets(line, sizeof line, f)) n += strncmp(line, "key", 3) == 0;
    if (f) fclose(f);
    return n;
}
int dhd_settings_save(const char *path, const char *key, const char *value)
{
    static char old[16384];
    char before[16384] = "";
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(before, 1, sizeof before - 1, f) : 0;
    if (f) fclose(f);
    before[n] = 0;
    nap(3);   /* another writer's turn, if it may have one */
    snprintf(old, sizeof old, "%s%s = %s\n", before, key, value);
    f = fopen(path, "w");
    if (!f) return 0;
    fputs(old, f);
    fclose(f);
    return 1;
}
int dhd_settings_read(const char *path, const char *key, char *out, size_t size) { (void)path; (void)key; (void)out; (void)size; return 0; }
size_t dhd_settings_values_json(const char *path, char *out, size_t size) { (void)path; return (size_t)snprintf(out, size, "{}"); }
size_t dhd_settings_schema_json(char *out, size_t size) { return (size_t)snprintf(out, size, "[]"); }
void dhd_settings_rider_name(const char *typed, char *out, size_t size) { (void)typed; snprintf(out, size, "R"); }
const char *host_disc_check(const char *path) { (void)path; return NULL; }
int dhd_disc_check(const char *iso, const char *cache, uint8_t digest[32], char *note, size_t size, const int *stop,
                   uint32_t pause_us) { (void)iso; (void)cache; (void)digest; (void)note; (void)size; (void)stop; (void)pause_us; return 2; }
int phone_update_find(const char *platform, char *v, size_t vs, char *p, size_t ps) { (void)platform; (void)v; (void)vs; (void)p; (void)ps; return 0; }

/* ---- the UIKit side ---- */
static volatile int keys_at_play = -1, refused;
static void page_send(void *c, const char *json)
{
    (void)c;
    char text[128];
    if (json_string(json, "error", text, sizeof text) && strstr(text, "the game is starting")) refused++;
}
static void play(void *c, int edit, int checked) { (void)c; (void)edit; (void)checked; keys_at_play = count_keys(); }
static void none(void *c, int id) { (void)c; (void)id; }
static void none3(void *c, int id, long long job) { (void)c; (void)id; (void)job; }
static void open_url(void *c, const char *url) { (void)c; (void)url; }
static void share(void *c) { (void)c; }
static uint64_t free_space(void *c) { (void)c; return 0; }

static LauncherHost host;
typedef struct { unsigned long ticket; char message[200]; int ordered; } Job;

#ifdef _WIN32
static DWORD WINAPI run(void *arg)
#else
static void *run(void *arg)
#endif
{
    Job *j = arg;
    nap(rand() % 5);   /* the threads start in any order */
    if (j->ordered) launcher_receive_ordered(&host, j->ticket, j->message);
    else launcher_receive(&host, j->message);
    return 0;
}

int main(int argc, char **argv)
{
    assert(argc > 1);
    char dir[512];
    snprintf(dir, sizeof dir, "%s/", argv[1]);
    snprintf(ini_path, sizeof ini_path, "%sdhd.ini", dir);
    remove(ini_path);
    LauncherHost h = {dir, "0.8.0", NULL, page_send, none, none, none3, play, open_url, share, NULL, free_space};
    host = h;
    srand(227);

    static Job jobs[SAVES + 2];
    int count = 0;
    /* the page's messages as they arrive: tickets in this order (the main thread) */
    for (int i = 0; i < SAVES; i++, count++) {
        snprintf(jobs[count].message, sizeof jobs[count].message,
                 "{\"id\":%d,\"call\":\"save_setting\",\"args\":{\"key\":\"key%02d\",\"value\":\"%d\"}}", i + 1, i, i);
        assert(launcher_settings_call(jobs[count].message));
        jobs[count].ordered = 1;
        jobs[count].ticket = launcher_order_ticket();
    }
    snprintf(jobs[count].message, sizeof jobs[count].message, "{\"id\":100,\"call\":\"play\",\"args\":{}}");
    assert(launcher_settings_call(jobs[count].message));
    jobs[count].ordered = 1, jobs[count].ticket = launcher_order_ticket(), count++;
    snprintf(jobs[count].message, sizeof jobs[count].message,
             "{\"id\":101,\"call\":\"save_setting\",\"args\":{\"key\":\"late\",\"value\":\"1\"}}");
    jobs[count].ordered = 1, jobs[count].ticket = launcher_order_ticket(), count++;
    assert(!launcher_settings_call("{\"id\":1,\"call\":\"check_update\"}"));   /* the long calls take no ticket */
    assert(!launcher_settings_call("{\"id\":1,\"call\":\"check_disc\"}"));

    /* run on as many threads as there are messages, started in reverse */
#ifdef _WIN32
    HANDLE threads[SAVES + 2];
    for (int i = count - 1; i >= 0; i--) threads[i] = CreateThread(NULL, 0, run, &jobs[i], 0, NULL);
    WaitForMultipleObjects((DWORD)count, threads, TRUE, INFINITE);
    for (int i = 0; i < count; i++) CloseHandle(threads[i]);
#else
    pthread_t threads[SAVES + 2];
    for (int i = count - 1; i >= 0; i--) pthread_create(&threads[i], NULL, run, &jobs[i]);
    for (int i = 0; i < count; i++) pthread_join(threads[i], NULL);
#endif
    assert(count_keys() == SAVES);          /* no save lost */
    assert(keys_at_play == SAVES);          /* PLAY ran after every save sent before it */
    assert(refused == 1);                   /* the save after PLAY: refused, the file is the game's */
    remove(ini_path);
    puts("launcher order: 24 saves on 26 threads, none lost; PLAY after them; a later save refused");
    return 0;
}
