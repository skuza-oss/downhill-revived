/* The launcher's link rule (launcher_link.h), one fixture set for every native bridge.
   GPL-3.0-or-later. */
#include "launcher_link.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    /* every link the page sends (port/launcher/ui/app.js, disc.rs's More link, update results) */
    const char *yes[] = {"discord", "kofi",
        "https://github.com/skuza-oss/downhill-revived",
        "https://github.com/skuza-oss/downhill-revived/releases",
        "https://github.com/skuza-oss/downhill-revived/releases/latest",
        "https://github.com/skuza-oss/downhill-revived/releases/tag/v0.8.2",
        "https://github.com/skuza-oss/downhill-revived/blob/main/docs/findings/100-other-releases.md",
        "https://github.com/skuza-oss/downhill-revived#readme"};
    const char *no[] = {"", "Discord", "KOFI", "javascript:alert(1)", "https://example.com/",
        "https://discord.gg/XrSeTyzfr2", "https://discord.gg/other", "https://ko-fi.com/skuza",
        "https://ko-fi.com/skuzaevil", "https://downhill-revived.pages.dev/",
        "http://github.com/skuza-oss/downhill-revived", "https://github.com/skuza-oss/",
        "https://github.com/skuza-oss/other", "https://github.com/skuza-oss/downhill-revived.evil/",
        "https://github.com/skuza-oss/downhill-revived-other", "https://github.com/skuza-oss.evil/downhill-revived",
        "https://github.com/skuza-oss/downhill-revived/../other",
        "https://github.com/skuza-oss/downhill-revived/%2E%2e/other",
        "https://github.com/skuza-oss/downhill-revived/%2e./other",
        "https://github.com/skuza-oss/downhill-revived\\evil", "https://github.com/skuza-oss/downhill-revived/a b",
        "https://github.com/skuza-oss/downhill-revived/a\t", "https://github.com/skuza-oss/downhill-revived/a\n",
        "https://github.com/skuza-oss/downhill-revived/a\r", "https://github.com/skuza-oss/downhill-revived/a\x7f",
        "https://github.com/skuza-oss/downhill-revived/a\"", "https://github.com/skuza-oss/downhill-revived/a'",
        "https://github.com/skuza-oss/downhill-revived/a<", "https://github.com/skuza-oss/downhill-revived/a>"};
    for (size_t i = 0; i < sizeof yes / sizeof *yes; ++i) assert(launcher_link(yes[i]));
    for (size_t i = 0; i < sizeof no / sizeof *no; ++i) assert(!launcher_link(no[i]));
    assert(!launcher_link(NULL));
    assert(!strcmp(launcher_link("discord"), "https://discord.gg/XrSeTyzfr2"));
    assert(!strcmp(launcher_link("kofi"), "https://ko-fi.com/skuza"));
    assert(launcher_link(yes[2]) == yes[2]);
    puts("launcher links: PASS");
    return 0;
}
