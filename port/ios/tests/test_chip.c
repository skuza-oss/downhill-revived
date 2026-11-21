/* The iOS chip check's rule (port/ios/app/chip.c, finding 273) against the flags an A12, an A10 and
   a system without the names report. Run by port/tools/test_runtime.py. GPL-3.0-or-later. */
#include "../app/chip.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *const *flags;   /* name, value, name, value, ..., NULL */

static int fake(const char *name, int *value)
{
    for (const char *const *f = flags; *f; f += 2) {
        if (!strcmp(*f, name)) { *value = f[1][0] - '0'; return 1; }
    }
    return 0;
}

static const char *ask(const char *const *with, char *out, size_t size, int *missing)
{
    flags = with;
    *missing = chip_missing_from(fake, out, size);
    return out;
}

int main(void)
{
    char out[64];
    int missing;
    static const char *const a12[] = {"hw.optional.arm.FEAT_LSE", "1", "hw.optional.arm.FEAT_FP16", "1",
        "hw.optional.arm.FEAT_LRCPC", "1", "hw.optional.arm.FEAT_JSCVT", "1", "hw.optional.arm.FEAT_FCMA", "1", NULL};
    assert(!strcmp(ask(a12, out, sizeof out, &missing), "") && !missing);
    /* an A10 / A10X on iPadOS 17: ARMv8.0, every A12 feature 0 */
    static const char *const a10[] = {"hw.optional.arm.FEAT_LSE", "0", "hw.optional.arm.FEAT_FP16", "0",
        "hw.optional.arm.FEAT_LRCPC", "0", "hw.optional.arm.FEAT_JSCVT", "0", "hw.optional.arm.FEAT_FCMA", "0", NULL};
    assert(!strcmp(ask(a10, out, sizeof out, &missing), "LSE, FP16, RCPC, JSCVT, FCMA") && missing);
    /* only the older names */
    static const char *const older[] = {"hw.optional.armv8_1_atomics", "0", "hw.optional.neon_fp16", "1",
        "hw.optional.armv8_3_compnum", "1", NULL};
    assert(!strcmp(ask(older, out, sizeof out, &missing), "LSE") && missing);
    /* no names at all: nothing can be told, nothing is refused */
    static const char *const none[] = {NULL};
    assert(!strcmp(ask(none, out, sizeof out, &missing), "") && !missing);
    /* a short buffer still says that something is missing */
    assert((ask(a10, out, 8, &missing), missing));
    puts("ios chip: PASS");
    return 0;
}
