/* The chip check (chip.h, finding 273). This file is part of the Xcode target's own sources (App/),
   which are built without build_host64.py's -mcpu=apple-a12: for the base arm64 (clang's default
   for arm64 iOS, the apple-a7), so it runs on an A10 itself. It calls only libc's sysctlbyname and
   getenv. GPL-3.0-or-later.

   The features: what LLVM's apple-a12 adds to the A10's (ARMv8.0 with crypto and CRC) that a
   compiler uses by itself or the game's code asks for: LSE atomics (ARMv8.1: CAS, LDADD, SWP for
   every atomic operation), FP16 arithmetic (ARMv8.2), RCPC (ARMv8.3: LDAPR for acquire loads),
   JSCVT and FCMA (ARMv8.3). Each has its hw.optional.arm.FEAT_* name (iOS 15 and later) and, where
   one exists, the older name. Every A12 and later reports all five; an A10 reports 0 for each. */
#include "chip.h"

#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

static const struct { const char *name, *flag, *older; } FEATURES[] = {
    {"LSE", "hw.optional.arm.FEAT_LSE", "hw.optional.armv8_1_atomics"},
    {"FP16", "hw.optional.arm.FEAT_FP16", "hw.optional.neon_fp16"},
    {"RCPC", "hw.optional.arm.FEAT_LRCPC", NULL},
    {"JSCVT", "hw.optional.arm.FEAT_JSCVT", NULL},
    {"FCMA", "hw.optional.arm.FEAT_FCMA", "hw.optional.armv8_3_compnum"},
};

int chip_missing_from(ChipFlagReader read, char *out, size_t size)
{
    size_t used = 0;
    if (size) out[0] = 0;
    for (size_t i = 0; i < sizeof FEATURES / sizeof *FEATURES; i++) {
        int value = 0;
        if (!read(FEATURES[i].flag, &value) && !(FEATURES[i].older && read(FEATURES[i].older, &value))) continue;
        if (value) continue;
        size_t n = strlen(FEATURES[i].name);
        if (used + n + 3 > size) return 1;
        if (used) { out[used++] = ','; out[used++] = ' '; }
        memcpy(out + used, FEATURES[i].name, n + 1);
        used += n;
    }
    return used > 0;
}

#ifdef __APPLE__
static int sysctl_flag(const char *name, int *value)
{
    int v = 0;
    size_t size = sizeof v;
    if (sysctlbyname(name, &v, &size, NULL, 0) != 0) return 0;
    *value = v;
    return 1;
}

const char *ios_chip_missing(void)
{
    static char missing[64];
    const char *forced = getenv("DHD_IOS_CHIP_TOO_OLD");
    if (forced && atoi(forced)) return "forced by DHD_IOS_CHIP_TOO_OLD";
    return chip_missing_from(sysctl_flag, missing, sizeof missing) ? missing : NULL;
}
#endif
