/* The launcher page's calls on iOS: launcher_calls.h. GPL-3.0-or-later. */
#include "launcher_calls.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef S_ISREG   /* the unit test on Windows */
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif

/* the shared code, from port/ (the Xcode project's header search path, build_ios.py) */
#include "net/json.h"
#include "net/update_check.h"
#include "host/phone.h"
#include "render/include/dhd_settings.h"
#include "render/include/dhd_disc.h"

const char *host_disc_check(const char *path);  /* host.c: NULL when path is the game */

const char LAUNCHER_IOS_CAPS[] =
    "[\"touch-settings\",\"disc-import\",\"direct-start\",\"share-log\",\"help\",\"touch-editor\"]";

/* The project's pages, the only ones a link opens (the desktop's main.rs open_link, Android's
   LauncherBridge.ALLOWED: the same list). */
static const char DISCORD[] = "https://discord.gg/XrSeTyzfr2", KOFI[] = "https://ko-fi.com/skuza";
static const char *const allowed[] = {"https://discord.gg/", "https://ko-fi.com/skuza", "https://github.com/skuza-oss/",
                                      "https://downhill-revived.pages.dev/"};

static char last_exit[256];   /* said once (launcher_set_last_exit) */

void launcher_set_last_exit(const char *why)
{
    snprintf(last_exit, sizeof last_exit, "%s", why ? why : "");
}

static void path_in(const LauncherHost *h, const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s%s", h->dir, name);
}

int launcher_disc_path(const LauncherHost *h, char *out, size_t size)
{
    struct stat st;
    path_in(h, "SCUS-97177.iso", out, size);
    return stat(out, &st) == 0 && S_ISREG(st.st_mode);
}

int launcher_starts_directly(const LauncherHost *h)
{
    char ini[1200], value[16] = "", disc[1200];
    path_in(h, "dhd.ini", ini, sizeof ini);
    if (!dhd_settings_read(ini, "launcher", value, sizeof value)) return 0;
    if (strcmp(value, "0") && strcmp(value, "off")) return 0;
    return launcher_disc_path(h, disc, sizeof disc) && !host_disc_check(disc);
}

/* A JSON string literal of s (quotes included), in a buffer of its own (freed by the caller). */
static char *quoted(const char *s)
{
    size_t need = json_escape(NULL, 0, s ? s : "") + 3;
    char *out = malloc(need);
    if (!out) return NULL;
    out[0] = '"';
    json_escape(out + 1, need - 2, s ? s : "");
    strcat(out, "\"");
    return out;
}

void launcher_answer(const LauncherHost *h, int id, int ok, const char *value_json)
{
    size_t size = strlen(value_json ? value_json : "null") + 64;
    char *m = malloc(size);
    if (!m) return;
    snprintf(m, size, "{\"id\":%d,\"ok\":%s,\"%s\":%s}", id, ok ? "true" : "false", ok ? "value" : "error",
             value_json ? value_json : "null");
    h->send(h->context, m);
    free(m);
}

void launcher_event(const LauncherHost *h, const char *event, const char *payload_json)
{
    size_t size = strlen(payload_json ? payload_json : "{}") + strlen(event) + 32;
    char *m = malloc(size);
    if (!m) return;
    snprintf(m, size, "{\"event\":\"%s\",\"payload\":%s}", event, payload_json ? payload_json : "{}");
    h->send(h->context, m);
    free(m);
}

void launcher_coded(char *out, size_t size, const char *code, ...)
{
    size_t at = (size_t)snprintf(out, size, "{\"code\":\"%s\",\"params\":{", code);
    va_list ap;
    va_start(ap, code);
    int first = 1;
    for (const char *name; (name = va_arg(ap, const char *)) != NULL && at < size;) {
        const char *value = va_arg(ap, const char *);
        char *v = quoted(value);
        at += (size_t)snprintf(out + at, size - at, "%s\"%s\":%s", first ? "" : ",", name, v ? v : "\"\"");
        free(v);
        first = 0;
    }
    va_end(ap);
    if (at < size) snprintf(out + at, size - at, "}}");
}

/* An error in the page's own words (a plain string). */
static void error_text(const LauncherHost *h, int id, const char *text)
{
    char *q = quoted(text);
    launcher_answer(h, id, 0, q);
    free(q);
}

/* A JSON text from one of dhd_settings' calls that say their whole length (NULL: out of memory). */
static char *settings_json(size_t (*make)(const char *, char *, size_t), const char *path)
{
    size_t size = 64 * 1024;
    for (int tries = 0; tries < 3; tries++) {
        char *out = malloc(size);
        if (!out) return NULL;
        size_t n = make(path, out, size);
        if (n < size) return out;
        free(out);
        size = n + 1;
    }
    return NULL;
}

static size_t schema(const char *unused, char *out, size_t size) { (void)unused; return dhd_settings_schema_json(out, size); }

static void snapshot(const LauncherHost *h, int id)
{
    char ini[1200], disc[1200];
    path_in(h, "dhd.ini", ini, sizeof ini);
    int has_disc = launcher_disc_path(h, disc, sizeof disc);
    /* the game reads the copy dhd.ini names (ios_main.m): the launcher that made it names it */
    char named[1200] = "";
    dhd_settings_read(ini, "iso", named, sizeof named);
    if (has_disc && strcmp(named, disc)) dhd_settings_save(ini, "iso", disc);
    else if (!has_disc && named[0]) dhd_settings_save(ini, "iso", "");
    char *settings = settings_json(dhd_settings_values_json, ini);
    const char *problem = has_disc ? host_disc_check(disc) : NULL;
    char *problem_json = problem ? quoted(problem) : NULL;
    char *why = last_exit[0] ? quoted(last_exit) : NULL;
    struct stat st;
    long long disc_size = has_disc && stat(disc, &st) == 0 ? (long long)st.st_size : 0;
    size_t size = (settings ? strlen(settings) : 2) + 2048;
    char *out = malloc(size);
    if (!out) { free(settings); free(problem_json); free(why); return; }
    snprintf(out, size,
             "{\"version\":\"%s\",\"caps\":%s,\"native_window_controls\":false,\"settings\":%s,\"game_found\":true,"
             "\"running\":false,\"disc_problem\":%s,\"texture_pack\":false,\"played\":0,\"scale\":null,"
             "\"scale_max\":%d,\"disc_size\":%lld,\"free_space\":%llu,\"last_exit\":%s%s%s}",
             h->version, LAUNCHER_IOS_CAPS, settings ? settings : "{}", problem_json ? problem_json : "null",
             LAUNCHER_PHONE_SCALE_MAX, disc_size, (unsigned long long)(h->free_space ? h->free_space(h->context) : 0),
             why ? "{\"why\":" : "null", why ? why : "", why ? "}" : "");
    last_exit[0] = 0;   /* said once */
    launcher_answer(h, id, 1, out);
    free(out);
    free(settings);
    free(problem_json);
    free(why);
}

static void check_update(const LauncherHost *h, int id)
{
    char version[32], page[512], out[800];
    int found = phone_update_find("ios", version, sizeof version, page, sizeof page);
    if (found < 0) {
        launcher_coded(out, sizeof out, "footer.checkFailed", "version", h->version, (const char *)NULL);
        launcher_answer(h, id, 0, out);
        return;
    }
    char *v = quoted(found ? version : h->version), *p = quoted(found ? page : UPDATE_RELEASES_PAGE);
    snprintf(out, sizeof out, "{\"release\":{\"version\":%s,\"page\":%s},\"newer\":%s}", v ? v : "\"\"", p ? p : "\"\"",
             found ? "true" : "false");
    free(v);
    free(p);
    launcher_answer(h, id, 1, out);
}

static void check_disc(const LauncherHost *h, int id)
{
    char disc[1200], cache[1200], note[512] = "";
    if (!launcher_disc_path(h, disc, sizeof disc)) { error_text(h, id, "No disc image is chosen."); return; }
    path_in(h, "iso-check.txt", cache, sizeof cache);
    unsigned char digest[32];
    int code = dhd_disc_check(disc, cache, digest, note, sizeof note, NULL, 0);
    if (code < 0) {
        char text[700];
        snprintf(text, sizeof text, "The disc image couldn't be read for the online check: %s", note);
        error_text(h, id, text);
        return;
    }
    char *path = quoted(disc), out[1500];
    snprintf(out, sizeof out, "{\"path\":%s,\"usa\":%s}", path ? path : "\"\"", note[0] ? "false" : "true");
    free(path);
    launcher_answer(h, id, 1, out);
}

static void open_link(const LauncherHost *h, int id, const char *asked)
{
    const char *url = !strcmp(asked, "discord") ? DISCORD : !strcmp(asked, "kofi") ? KOFI : asked;
    int ok = 0;
    for (size_t i = 0; i < sizeof allowed / sizeof *allowed; i++) ok |= !strncmp(url, allowed[i], strlen(allowed[i]));
    if (!ok || strpbrk(url, " \"'<>\\")) {
        char text[700];
        snprintf(text, sizeof text, "not opening %s", url);
        error_text(h, id, text);
        return;
    }
    h->open_url(h->context, url);
    launcher_answer(h, id, 1, NULL);
}

void launcher_receive(const LauncherHost *h, const char *message)
{
    long long id_value;
    char call[64], ini[1200];
    if (!message || !json_int(message, "id", &id_value) || !json_string(message, "call", call, sizeof call)) return;
    int id = (int)id_value;
    path_in(h, "dhd.ini", ini, sizeof ini);
    if (!strcmp(call, "snapshot")) {
        snapshot(h, id);
    } else if (!strcmp(call, "settings_schema")) {
        char *s = settings_json(schema, NULL);
        launcher_answer(h, id, s != NULL, s ? s : "\"out of memory\"");
        free(s);
    } else if (!strcmp(call, "save_setting")) {
        char key[128], value[2048];
        if (!json_string(message, "args.key", key, sizeof key) || !json_string(message, "args.value", value, sizeof value))
            error_text(h, id, "save_setting needs a key and a value");
        else if (!dhd_settings_save(ini, key, value)) error_text(h, id, "can't save dhd.ini");
        else launcher_answer(h, id, 1, NULL);
    } else if (!strcmp(call, "rider_name")) {
        char typed[256] = "", name[64] = "";
        json_string(message, "args.text", typed, sizeof typed);
        dhd_settings_rider_name(typed, name, sizeof name);
        char *q = quoted(name);
        launcher_answer(h, id, 1, q);
        free(q);
    } else if (!strcmp(call, "check_update")) {
        check_update(h, id);
    } else if (!strcmp(call, "check_disc")) {
        check_disc(h, id);
    } else if (!strcmp(call, "pick_disc")) {
        h->pick_disc(h->context, id);
    } else if (!strcmp(call, "retry_import")) {
        h->retry_import(h->context, id);
    } else if (!strcmp(call, "delete_original")) {
        long long job = 0;
        json_int(message, "args.job", &job);
        h->delete_original(h->context, id, job);
    } else if (!strcmp(call, "play")) {
        int edit = 0;
        json_bool(message, "args.edit_touch_layout", &edit);
        launcher_answer(h, id, 1, NULL);
        h->play(h->context, edit, 1);
    } else if (!strcmp(call, "open_link")) {
        char url[600] = "";
        json_string(message, "args.url", url, sizeof url);
        open_link(h, id, url);
    } else if (!strcmp(call, "share_log")) {
        h->share_log(h->context);
        launcher_answer(h, id, 1, NULL);
    } else if (!strcmp(call, "licences")) {
        const char *text = h->licences ? h->licences(h->context) : NULL;
        char *q = quoted(text ? text : "Downhill Revived is free software under the GNU General Public License, version 3 "
                                       "or later. This build carries no licence texts; a release has them.");
        launcher_answer(h, id, q != NULL, q ? q : "\"out of memory\"");
        free(q);
    } else {
        char text[128];
        snprintf(text, sizeof text, "no such call: %s", call);
        error_text(h, id, text);
    }
}
