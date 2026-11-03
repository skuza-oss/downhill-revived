/* Tests of update_check.c: the version compare (as the launcher's update.rs tests it), reading a
   latest.json answer (the Android entry's url only one of this repository's release assets), the
   order the sources are asked in (a stale or older answer never hides a newer one) and the
   once-a-day rule. */
#include "update_check.h"

#include <stdio.h>
#include <string.h>

static int failures;

/* update_find's sources in a test: each answer by index, NULL for one that doesn't answer; the
   number of requests made. */
typedef struct {
    const char *answers[2];
    int asked;
} Fake;

static const char *fake_fetch(void *context, const char *url)
{
    Fake *f = context;
    f->asked++;
    return !strcmp(url, "server") ? f->answers[0] : f->answers[1];
}
#define CHECK(c) do { if (!(c)) { printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    unsigned v[3];
    CHECK(update_parse_version("0.7.8", v) && v[0] == 0 && v[1] == 7 && v[2] == 8);
    CHECK(update_parse_version("v1.10.200", v) && v[0] == 1 && v[1] == 10 && v[2] == 200);
    CHECK(!update_parse_version("0.7", v) && !update_parse_version("0.7.8.1", v) && !update_parse_version("junk", v));
    CHECK(!update_parse_version("0.7.x", v) && !update_parse_version("", v) && !update_parse_version(NULL, v));
    CHECK(!update_parse_version("1234567890.0.0", v));

    CHECK(update_newer("0.5.1", "0.5.0"));
    CHECK(update_newer("v1.0.0", "0.9.9"));
    CHECK(update_newer("0.7.10", "0.7.9"));   /* numbers, not text */
    CHECK(update_newer("0.8.0", "0.7.99"));
    CHECK(!update_newer("0.5.0", "0.5.0"));
    CHECK(!update_newer("0.6.9", "0.7.0"));
    CHECK(!update_newer("junk", "0.5.0") && !update_newer("0.5.1", "junk"));

    static const char manifest[] =
        "{\"name\":\"Downhill Revived\",\"version\":\"0.7.8\",\"url\":\"https://github.com/skuza-oss/downhill-revived/"
        "releases/download/v0.7.8/DownhillRevived-0.7.8-win.zip\",\"sha256\":\"00\",\"size\":1,"
        "\"page\":\"https://github.com/skuza-oss/downhill-revived/releases/tag/v0.7.8\",\"platforms\":{"
        "\"android\":{\"url\":\"https://github.com/skuza-oss/downhill-revived/releases/download/v0.7.8/"
        "DownhillRevived-0.7.8-android-experimental.apk\",\"sha256\":\"00\",\"size\":2},"
        "\"win\":{\"url\":\"https://github.com/skuza-oss/downhill-revived/releases/download/v0.7.8/"
        "DownhillRevived-0.7.8-win.zip\",\"sha256\":\"00\",\"size\":1}}}";
    char version[32], page[256];
    CHECK(update_from_manifest(manifest, "android", "0.7.7", version, sizeof version, page, sizeof page));
    CHECK(!strcmp(version, "0.7.8"));
    CHECK(!strcmp(page, "https://github.com/skuza-oss/downhill-revived/releases/tag/v0.7.8"));
    CHECK(!update_from_manifest(manifest, "android", "0.7.8", version, sizeof version, page, sizeof page));
    CHECK(!update_from_manifest(manifest, "android", "0.8.0", version, sizeof version, page, sizeof page));
    CHECK(!update_from_manifest(manifest, "mac", "0.7.7", version, sizeof version, page, sizeof page));  /* no build */
    CHECK(!update_from_manifest("{\"version\":\"0.7.8\"}", "android", "0.7.7", version, sizeof version, page, sizeof page));
    CHECK(!update_from_manifest("not json", "android", "0.7.7", version, sizeof version, page, sizeof page));
    CHECK(!update_from_manifest(NULL, "android", "0.7.7", version, sizeof version, page, sizeof page));

    /* a page elsewhere (or none): the repository's releases page instead */
    static const char elsewhere[] =
        "{\"version\":\"v0.9.0\",\"page\":\"https://example.org/evil\",\"platforms\":{\"android\":"
        "{\"url\":\"https://github.com/skuza-oss/downhill-revived/releases/download/v0.9.0/x.apk\"}}}";
    CHECK(update_from_manifest(elsewhere, "android", "0.7.7", version, sizeof version, page, sizeof page));
    CHECK(!strcmp(version, "v0.9.0") && !strcmp(page, UPDATE_RELEASES_PAGE));
    static const char no_page[] =
        "{\"version\":\"0.9.0\",\"platforms\":{\"android\":{\"url\":\"https://github.com/skuza-oss/downhill-revived/"
        "releases/download/v0.9.0/x.apk\"}}}";
    CHECK(update_from_manifest(no_page, "android", "0.7.7", version, sizeof version, page, sizeof page));
    CHECK(!strcmp(page, UPDATE_RELEASES_PAGE));
    static const char plain_url[] =
        "{\"version\":\"0.9.0\",\"platforms\":{\"android\":{\"url\":\"http://github.com/x.apk\"}}}";
    CHECK(!update_from_manifest(plain_url, "android", "0.7.7", version, sizeof version, page, sizeof page));
    /* the APK's url: this repository's release assets only (as the launcher's from_backup) */
    static const char *const bad_urls[] = {
        "https://example.org/x.apk",
        "https://github.com/x.apk",
        "https://github.com/someone/downhill-revived/releases/download/v0.9.0/x.apk",
        "https://github.com/skuza-oss/downhill-revived/releases/tag/v0.9.0",
        "https://github.com/skuza-oss/downhill-revived/releases/download/",
        "https://objects.githubusercontent.com/github-production-release-asset/x.apk",
        "https://github.com/skuza-oss/downhill-revived/releases/download/v0.9.0/x y.apk",
    };
    for (size_t i = 0; i < sizeof bad_urls / sizeof *bad_urls; i++) {
        char json[512];
        snprintf(json, sizeof json, "{\"version\":\"0.9.0\",\"platforms\":{\"android\":{\"url\":\"%s\"}}}", bad_urls[i]);
        CHECK(!update_from_manifest(json, "android", "0.7.7", version, sizeof version, page, sizeof page));
    }

    /* the sources in order: the server first, GitHub whenever the server names nothing newer */
    static const char *const sources[] = {"server", "github"};
    static const char older[] =   /* the server's stale copy: the release this phone runs */
        "{\"version\":\"0.7.7\",\"platforms\":{\"android\":{\"url\":\"https://github.com/skuza-oss/"
        "downhill-revived/releases/download/v0.7.7/x.apk\"}}}";
    Fake fake = {{manifest, older}, 0};   /* the server knows 0.7.8: GitHub isn't asked */
    CHECK(update_find(fake_fetch, &fake, sources, 2, "android", "0.7.7", version, sizeof version, page, sizeof page) == 1);
    CHECK(fake.asked == 1 && !strcmp(version, "0.7.8"));
    fake = (Fake){{older, manifest}, 0};   /* a stale server: GitHub's newer release still counts */
    CHECK(update_find(fake_fetch, &fake, sources, 2, "android", "0.7.7", version, sizeof version, page, sizeof page) == 2);
    CHECK(fake.asked == 2 && !strcmp(version, "0.7.8"));
    fake = (Fake){{NULL, manifest}, 0};   /* no server */
    CHECK(update_find(fake_fetch, &fake, sources, 2, "android", "0.7.7", version, sizeof version, page, sizeof page) == 2);
    fake = (Fake){{"not json", older}, 0};   /* a broken answer, then nothing newer */
    CHECK(!update_find(fake_fetch, &fake, sources, 2, "android", "0.7.7", version, sizeof version, page, sizeof page));
    CHECK(fake.asked == 2);
    fake = (Fake){{NULL, NULL}, 0};   /* offline */
    CHECK(!update_find(fake_fetch, &fake, sources, 2, "android", "0.7.7", version, sizeof version, page, sizeof page));
    fake = (Fake){{manifest, manifest}, 0};   /* this is 0.7.8 already */
    CHECK(!update_find(fake_fetch, &fake, sources, 2, "android", "0.7.8", version, sizeof version, page, sizeof page));
    CHECK(fake.asked == 2);

    const long long day = UPDATE_INTERVAL_SECONDS, now = 1793750400;  /* 2026-11-03 */
    CHECK(update_due("", now, 0));
    CHECK(update_due("ask", now, now - day));
    CHECK(update_due("on", now, now - day - 1));
    CHECK(!update_due("ask", now, now - day + 1));
    CHECK(!update_due("", now, now));
    CHECK(!update_due("off", now, 0));
    CHECK(update_due(NULL, now, now + 3600));   /* a clock set back */

    if (failures)
        printf("test_update_check: %d failures\n", failures);
    else
        printf("test_update_check: all passed\n");
    return failures != 0;
}
