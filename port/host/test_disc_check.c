/* The shared synthetic boot-disc corpus (disc_fixtures.py), no game data.
   Run through port/tools/test_runtime.py. GPL-3.0-or-later. */
#include "disc_check.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char image[64 * 2048];
static int read_image(void *context, uint64_t offset, void *buffer, size_t n)
{
    uint64_t length = *(uint64_t *)context;
    if (offset > length || n > length - offset) return 0;
    memcpy(buffer, image + offset, n);
    return 1;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *names[] = {"good", "no-volume", "no-cnf", "other", "europe", "japan", "cut", "tiny",
        "zero-volume", "empty-boot", "empty-path", "long-boot", "binary-cnf", "no-boot"};
    int codes[] = {DISC_GAME, DISC_NOT_ISO, DISC_NO_CNF, DISC_OTHER, DISC_OTHER, DISC_OTHER, DISC_CUT,
        DISC_NOT_ISO, DISC_CUT, DISC_OTHER, DISC_OTHER, DISC_OTHER, DISC_OTHER, DISC_OTHER};
    for (unsigned i = 0; i < sizeof codes / sizeof *codes; i++) {
        char path[4096], id[32];
        uint64_t volume;
        snprintf(path, sizeof path, "%s/%s.iso", argv[1], names[i]);
        FILE *f = fopen(path, "rb");
        assert(f);
        uint64_t size = fread(image, 1, sizeof image, f);
        fclose(f);
        assert(disc_check_image(read_image, &size, size, id, sizeof id, &volume) == codes[i]);
        if (!strcmp(names[i], "good") || !strcmp(names[i], "cut")) assert(volume == sizeof image);
        if (!strcmp(names[i], "other")) assert(!strcmp(id, "SLUS_203.12"));
        if (!strcmp(names[i], "long-boot")) assert(strlen(id) == 31);
        if (!strcmp(names[i], "empty-boot") || !strcmp(names[i], "empty-path")) assert(!id[0]);
    }
    puts("disc_check: shared synthetic corpus passed");
    return 0;
}
