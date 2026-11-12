/* The crash report's line of the disc files the game looked up last (finding 208): cdvd.c writes
   them into the HLE window (memory.h PS2_DISC_RECENT); the host's crash report (port/hle/host.c)
   and Windows' guest memory fault report (port/host64/guest_memory_win32.c) print this line.
   at: the host address of PS2_DISC_RECENT, readable. Writes the line, oldest lookup first, into
   out; its length, 0 before the first lookup. No allocation: it runs in fault handlers. */
#ifndef DHD_DISC_RECENT_H
#define DHD_DISC_RECENT_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "memory.h"

static inline int disc_recent_line(const uint8_t *at, char *out, size_t size)
{
    uint32_t count;
    memcpy(&count, at, sizeof count);
    if (!count || size < 64) return 0;
    size_t n = (size_t)snprintf(out, size, "disc files looked up last (%u in all):", count);
    for (uint32_t k = count > PS2_DISC_RECENT_SLOTS ? count - PS2_DISC_RECENT_SLOTS : 0; k < count && n < size; k++) {
        const char *path = (const char *)at + 8 + k % PS2_DISC_RECENT_SLOTS * PS2_DISC_RECENT_PATH;
        n += (size_t)snprintf(out + n, size - n, " %.*s%s", (int)PS2_DISC_RECENT_PATH - 1, path,
                              k + 1 < count ? "," : "\n");
    }
    return (int)(n < size ? n : size - 1);
}
#endif
