/* Is this iPhone's or iPad's chip at the game's CPU floor (finding 273)? The game's code is built
   for the A12 (build_host64.py's IOS_CPU, finding 267): an A10 or A10X (iPad 6 and 7, iPad Pro
   2017, iPadOS 17) ends with SIGILL as soon as that code runs. The app asks before any of it runs
   and shows a plain page instead. GPL-3.0-or-later. */
#ifndef DHD_IOS_CHIP_H
#define DHD_IOS_CHIP_H
#include <stddef.h>

/* Reads one sysctl flag (hw.optional...): 1 and *value set when the name exists, else 0. */
typedef int (*ChipFlagReader)(const char *name, int *value);

/* The A12 features the build may use that this chip lacks, comma-separated in out ("" when none).
   A feature counts as missing only when the system says 0: a name the system does not know is never
   a refusal. Returns 1 when something is missing. */
int chip_missing_from(ChipFlagReader read, char *out, size_t size);

/* This device (sysctlbyname); DHD_IOS_CHIP_TOO_OLD=1 forces the answer for the Simulator's check.
   NULL when the chip has everything, else the missing features' names (static storage). */
const char *ios_chip_missing(void);

#endif
