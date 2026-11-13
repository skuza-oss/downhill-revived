/* The iOS launcher's message handling (port/ios/host/launcher_calls.c, finding 227) driven by a fake
   page: each call's answer by id, the coded and plain errors, the once-only crash note, the link
   rule and the UIKit side's hooks. The shared code it calls (the settings crate, the disc checks,
   the update check) is stood in for here; their own tests are theirs. GPL-3.0-or-later. */
#include "../host/launcher_calls.h"
#include "net/json.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stand-ins for the shared code ---- */
static char ini_launcher[16] = "", ini_iso[256] = "", saved_key[64], saved_value[256];
int dhd_settings_read(const char *path, const char *key, char *out, size_t size)
{
    (void)path;
    const char *v = !strcmp(key, "launcher") ? ini_launcher : !strcmp(key, "iso") ? ini_iso : "";
    if (!*v) return 0;
    snprintf(out, size, "%s", v);
    return 1;
}
int dhd_settings_save(const char *path, const char *key, const char *value)
{
    (void)path;
    snprintf(saved_key, sizeof saved_key, "%s", key);
    snprintf(saved_value, sizeof saved_value, "%s", value);
    if (!strcmp(key, "iso")) snprintf(ini_iso, sizeof ini_iso, "%s", value);
    return strcmp(key, "readonly") != 0;
}
size_t dhd_settings_values_json(const char *path, char *out, size_t size)
{
    (void)path;
    return (size_t)snprintf(out, size, "{\"fps\":\"60\",\"launcher\":\"%s\"}", ini_launcher[0] ? ini_launcher : "1");
}
size_t dhd_settings_schema_json(char *out, size_t size) { return (size_t)snprintf(out, size, "[{\"key\":\"fps\"}]"); }
void dhd_settings_rider_name(const char *typed, char *out, size_t size) { snprintf(out, size, "%.8s", typed); }
static const char *disc_answer;
const char *host_disc_check(const char *path) { (void)path; return disc_answer; }
int dhd_disc_check(const char *iso, const char *cache, uint8_t digest[32], char *note, size_t size, const int *stop,
                   uint32_t pause_us)
{
    (void)iso; (void)cache; (void)digest; (void)stop; (void)pause_us;
    snprintf(note, size, "%s", "");
    return 2;
}
static int update_answer;
int phone_update_find(const char *platform, char *version, size_t vs, char *page, size_t ps)
{
    assert(!strcmp(platform, "ios"));
    if (update_answer > 0) {
        snprintf(version, vs, "9.9.9");
        snprintf(page, ps, "https://github.com/skuza-oss/downhill-revived/releases/tag/v9.9.9");
    }
    return update_answer;
}

/* ---- the fake page and the UIKit side ---- */
static char sent[8][4096];
static int sent_count, picked, played, edit_asked, shared;
static char opened[600];
static void send(void *c, const char *json) { (void)c; assert(sent_count < 8); snprintf(sent[sent_count++], sizeof *sent, "%s", json); }
static void pick(void *c, int id) { picked = id; (void)c; }
static void delete_original(void *c, int id, long long job) { (void)c; (void)id; (void)job; }
static void play(void *c, int edit, int checked) { (void)c; played++; edit_asked = edit; assert(checked); }
static void open_url(void *c, const char *url) { (void)c; snprintf(opened, sizeof opened, "%s", url); }
static void share_log(void *c) { (void)c; shared++; }
static uint64_t free_space(void *c) { (void)c; return 5000000000ull; }

static const char *call(const LauncherHost *h, const char *message)
{
    sent_count = 0;
    launcher_receive(h, message);
    assert(sent_count == 1);
    return sent[0];
}

int main(int argc, char **argv)
{
    /* argv[1]: an existing folder of its own (test_runtime.py's), with a stand-in disc file in it */
    assert(argc > 1);
    char dir[512], disc[600];
    snprintf(dir, sizeof dir, "%s/", argv[1]);
    snprintf(disc, sizeof disc, "%sSCUS-97177.iso", dir);
    remove(disc);
    LauncherHost h = {dir, "0.8.0", NULL, send, pick, NULL, delete_original, play, open_url, share_log, NULL, free_space};

    /* the first start: no disc, defaults */
    const char *a = call(&h, "{\"id\":1,\"call\":\"snapshot\",\"args\":{}}");
    long long n;
    int b;
    char text[512];
    assert(json_int(a, "id", &n) && n == 1 && json_bool(a, "ok", &b) && b);
    assert(json_string(a, "value.version", text, sizeof text) && !strcmp(text, "0.8.0"));
    assert(json_count(a, "value.caps") == 6 && json_string(a, "value.caps[5]", text, sizeof text) && !strcmp(text, "touch-editor"));
    assert(json_is_null(a, "value.disc_problem") && json_is_null(a, "value.last_exit"));
    assert(json_int(a, "value.scale_max", &n) && n == 6 && json_int(a, "value.disc_size", &n) && n == 0);
    assert(json_string(a, "value.settings.fps", text, sizeof text) && !strcmp(text, "60"));
    assert(!launcher_starts_directly(&h));

    /* a disc in place (and named in dhd.ini by the snapshot), the crash note once */
    FILE *f = fopen(disc, "wb");
    fputs("disc", f);
    fclose(f);
    launcher_set_last_exit("It crashed.");
    a = call(&h, "{\"id\":2,\"call\":\"snapshot\"}");
    assert(json_string(a, "value.last_exit.why", text, sizeof text) && !strcmp(text, "It crashed."));
    assert(json_int(a, "value.disc_size", &n) && n == 4 && strstr(ini_iso, "SCUS-97177.iso"));
    a = call(&h, "{\"id\":3,\"call\":\"snapshot\"}");
    assert(json_is_null(a, "value.last_exit"));
    disc_answer = "That disc is SLUS_123.45, not Downhill Domination (USA), SCUS-97177.";
    a = call(&h, "{\"id\":4,\"call\":\"snapshot\"}");
    assert(json_string(a, "value.disc_problem", text, sizeof text) && strstr(text, "SLUS_123.45"));
    disc_answer = NULL;

    /* the setting "launcher": off starts the game at once when the disc is ready */
    snprintf(ini_launcher, sizeof ini_launcher, "0");
    assert(launcher_starts_directly(&h));
    snprintf(ini_launcher, sizeof ini_launcher, "1");
    assert(!launcher_starts_directly(&h));

    a = call(&h, "{\"id\":5,\"call\":\"settings_schema\"}");
    assert(json_string(a, "value[0].key", text, sizeof text) && !strcmp(text, "fps"));
    a = call(&h, "{\"id\":6,\"call\":\"save_setting\",\"args\":{\"key\":\"fps\",\"value\":\"120\"}}");
    assert(json_bool(a, "ok", &b) && b && json_is_null(a, "value") && !strcmp(saved_key, "fps") && !strcmp(saved_value, "120"));
    a = call(&h, "{\"id\":7,\"call\":\"save_setting\",\"args\":{\"key\":\"readonly\",\"value\":\"1\"}}");
    assert(json_bool(a, "ok", &b) && !b && json_string(a, "error", text, sizeof text) && !strcmp(text, "can't save dhd.ini"));
    a = call(&h, "{\"id\":8,\"call\":\"rider_name\",\"args\":{\"text\":\"A \\\"long\\\" name\"}}");
    assert(json_string(a, "value", text, sizeof text) && !strcmp(text, "A \"long\""));

    /* the update line: none answered (a coded error), up to date, a newer release */
    update_answer = -1;
    a = call(&h, "{\"id\":9,\"call\":\"check_update\"}");
    assert(json_string(a, "error.code", text, sizeof text) && !strcmp(text, "footer.checkFailed"));
    assert(json_string(a, "error.params.version", text, sizeof text) && !strcmp(text, "0.8.0"));
    update_answer = 0;
    a = call(&h, "{\"id\":10,\"call\":\"check_update\"}");
    assert(json_bool(a, "value.newer", &b) && !b && json_string(a, "value.release.page", text, sizeof text) && strstr(text, "releases/latest"));
    update_answer = 1;
    a = call(&h, "{\"id\":11,\"call\":\"check_update\"}");
    assert(json_bool(a, "value.newer", &b) && b && json_string(a, "value.release.version", text, sizeof text) && !strcmp(text, "9.9.9"));

    a = call(&h, "{\"id\":12,\"call\":\"check_disc\"}");
    assert(json_bool(a, "value.usa", &b) && b);

    /* links: the project's pages only */
    a = call(&h, "{\"id\":13,\"call\":\"open_link\",\"args\":{\"url\":\"discord\"}}");
    assert(json_bool(a, "ok", &b) && b && !strcmp(opened, "https://discord.gg/XrSeTyzfr2"));
    a = call(&h, "{\"id\":14,\"call\":\"open_link\",\"args\":{\"url\":\"https://example.com/\"}}");
    assert(json_bool(a, "ok", &b) && !b);
    a = call(&h, "{\"id\":15,\"call\":\"open_link\",\"args\":{\"url\":\"https://github.com/skuza-oss/x\\\"y\"}}");
    assert(json_bool(a, "ok", &b) && !b);

    /* the UIKit side: the picker answers later itself; PLAY, Share log */
    sent_count = 0;
    launcher_receive(&h, "{\"id\":16,\"call\":\"pick_disc\"}");
    assert(picked == 16 && sent_count == 0);
    a = call(&h, "{\"id\":17,\"call\":\"play\",\"args\":{\"edit_touch_layout\":true}}");
    assert(played == 1 && edit_asked == 1 && json_bool(a, "ok", &b) && b);
    a = call(&h, "{\"id\":18,\"call\":\"share_log\"}");
    assert(shared == 1);
    a = call(&h, "{\"id\":19,\"call\":\"licences\"}");
    assert(json_string(a, "value", text, sizeof text) && strstr(text, "GNU General Public License"));
    a = call(&h, "{\"id\":20,\"call\":\"no_such\"}");
    assert(json_string(a, "error", text, sizeof text) && strstr(text, "no such call"));
    /* not a call: nothing is answered */
    sent_count = 0;
    launcher_receive(&h, "not json");
    launcher_receive(&h, "{\"event\":\"x\"}");
    assert(sent_count == 0);

    /* events and coded errors as the page reads them */
    sent_count = 0;
    char coded[256];
    launcher_coded(coded, sizeof coded, "disc.problem.noRoom", "need", "2.5 GB", "free", "1.0 GB", (const char *)NULL);
    launcher_event(&h, "disc-import", "{\"job\":1,\"stage\":\"checking\"}");
    assert(json_string(sent[0], "event", text, sizeof text) && !strcmp(text, "disc-import"));
    assert(json_string(sent[0], "payload.stage", text, sizeof text) && !strcmp(text, "checking"));
    assert(json_string(coded, "params.free", text, sizeof text) && !strcmp(text, "1.0 GB"));
    remove(disc);
    puts("launcher_calls: every call answered as the page expects");
    return 0;
}
