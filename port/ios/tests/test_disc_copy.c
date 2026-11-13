/* The iOS disc copy's rules (port/ios/host/disc_copy.c, finding 227), on a small made-up ISO 9660
   volume: the volume's size, the copy loop, cancelling, the completeness rule (Android's
   DiscImage.completeness). GPL-3.0-or-later. */
#include "../host/disc_copy.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int steps;
static void progress(void *context, uint64_t done) { (void)context; (void)done; steps++; }

int main(void)
{
    char why[256];
    /* a volume of 9000 blocks of 2048 bytes, 20 MiB of data written */
    FILE *in = tmpfile(), *out = tmpfile();
    assert(in && out);
    static unsigned char block[2048];
    for (int i = 0; i < 10240; i++) {
        memset(block, 0, sizeof block);
        if (i == 16) {
            block[0] = 1;
            memcpy(block + 1, "CD001", 5);
            block[80] = 9000 & 255, block[81] = 9000 >> 8;
            block[128] = 0, block[129] = 8;   /* 2048 */
        } else {
            block[0] = (unsigned char)i;
        }
        assert(fwrite(block, 1, sizeof block, in) == sizeof block);
    }
    assert(disc_volume_bytes(in) == 9000ull * 2048);
    rewind(in);
    int cancel = 0;
    int64_t copied = disc_copy(in, out, &cancel, progress, NULL);
    assert(copied == 10240ll * 2048 && steps == 1);
    assert(disc_volume_bytes(out) == 9000ull * 2048);
    assert(!disc_completeness((uint64_t)copied, (uint64_t)copied, disc_volume_bytes(out), why, sizeof why));
    /* a source that ended early, a size that changed, a volume larger than the copy */
    assert(disc_completeness(100, 200, 50, why, sizeof why) && strstr(why, "stopped early"));
    assert(disc_completeness(300, 200, 50, why, sizeof why) && strstr(why, "doesn't match"));
    assert(disc_completeness(100, 0, 0, why, sizeof why) && strstr(why, "no disc volume size"));
    assert(disc_completeness(100, 100, 200, why, sizeof why) && strstr(why, "incomplete"));
    /* cancelled before the first block */
    rewind(in);
    cancel = 1;
    assert(disc_copy(in, out, &cancel, NULL, NULL) == -1);
    /* not a disc image */
    FILE *plain = tmpfile();
    fputs("hello", plain);
    assert(disc_volume_bytes(plain) == 0);
    fclose(plain);
    fclose(in);
    fclose(out);

    /* the publication: an older import finishing after a newer pick lands nowhere */
    char older_file[64], newer_file[64], target[64];
    snprintf(older_file, sizeof older_file, "dc-older-%d.part", (int)(uintptr_t)&older_file % 9973);
    snprintf(newer_file, sizeof newer_file, "dc-newer-%d.part", (int)(uintptr_t)&older_file % 9973);
    snprintf(target, sizeof target, "dc-target-%d.iso", (int)(uintptr_t)&older_file % 9973);
    remove(target);
    FILE *a = fopen(older_file, "w"); fputs("older", a); fclose(a);
    FILE *b = fopen(newer_file, "w"); fputs("newer", b); fclose(b);
    long long first = disc_publish_begin(), second = disc_publish_begin();
    assert(!disc_publish_current(first) && disc_publish_current(second));
    assert(disc_publish(first, older_file, target) == 1);   /* superseded: not moved */
    FILE *t = fopen(target, "r");
    assert(!t);
    assert(disc_publish(second, newer_file, target) == 0);
    char got[16] = "";
    t = fopen(target, "r");
    assert(t && fgets(got, sizeof got, t) && !strcmp(got, "newer"));
    fclose(t);
    remove(target);
    remove(older_file);
    puts("disc_copy: volume, copy, cancel, completeness, and a superseded import never lands");
    return 0;
}
