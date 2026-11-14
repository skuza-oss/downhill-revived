/* Phone update lookup, including the notice's source attribution. GPL-3.0-or-later. */
#include "phone.h"
#include "../net/update_check.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* phone.c runs on POSIX phones; the PC fixture needs only a no-op rows setter. */
#ifdef _WIN32
static int test_setenv(const char *name, const char *value, int overwrite)
{ (void)name; (void)value; (void)overwrite; return 0; }
#define setenv test_setenv
#endif
#include "phone.c"
static const char *answers[2];
static int asked;
int http_get(const char *url, char *out, size_t cap)
{
    int index = !strcmp(url, UPDATE_SOURCE_SERVER) ? 0 : 1;
    assert(!strcmp(url, index ? UPDATE_SOURCE_GITHUB : UPDATE_SOURCE_SERVER));
    ++asked;
    if (!answers[index]) return -1;
    return snprintf(out, cap, "%s", answers[index]);
}
int main(void)
{
    const char *newer = "{\"version\":\"99.0.0\",\"platforms\":{\"android\":{\"url\":\"https://github.com/skuza-oss/downhill-revived/releases/download/v99.0.0/x.apk\"},\"ios\":{\"url\":\"https://github.com/skuza-oss/downhill-revived/releases/download/v99.0.0/x.ipa\"}}}";
    char version[32], page[512];
    const char *source;
    for (int platform = 0; platform < 2; ++platform) {
        const char *key = platform ? "ios" : "android";
        for (int winner = 0; winner < 2; ++winner) {
            answers[0] = winner ? "{}" : newer; answers[1] = newer; asked = 0;
            assert(phone_update_find_source(key, version, sizeof version, page, sizeof page, &source, "update") == 1);
            assert(asked == winner + 1);
            assert(!strcmp(source, winner ? UPDATE_SOURCE_GITHUB : UPDATE_SOURCE_SERVER));
            assert(!strcmp(version, "99.0.0") && !strcmp(page, UPDATE_RELEASES_PAGE));
        }
        answers[0] = "{}"; answers[1] = NULL; source = "stale";
        assert(phone_update_find_source(key, version, sizeof version, page, sizeof page, &source, "update") == 0);
        assert(source == NULL);
        answers[0] = NULL;
        assert(phone_update_find(key, version, sizeof version, page, sizeof page) == -1);
    }
    puts("phone update: PASS");
}
