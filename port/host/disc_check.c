/* The disc image's own check: disc_check.h. GPL-3.0-or-later. */
#include "disc_check.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

enum { SECTOR = 2048 };

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int same_name(const uint8_t *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (toupper(a[i]) != b[i]) return 0;
    return 1;
}

int disc_check_image(DiscRead read, void *context, uint64_t file_size, char *id, size_t id_size, uint64_t *volume)
{
    uint8_t sector[SECTOR];
    char text[SECTOR + 1];
    if (id_size) id[0] = 0;
    if (volume) *volume = 0;
#define READ(lsn) read(context, (uint64_t)(lsn) * SECTOR, sector, SECTOR)
    if (!READ(16) || sector[0] != 1 || memcmp(sector + 1, "CD001", 5)) return DISC_NOT_ISO;
    /* the volume's size (blocks at 80, block size at 128) and its root directory record (at 156:
       extent at +2, size at +10) */
    uint64_t declared = (uint64_t)le32(sector + 80) * (uint32_t)(sector[128] | sector[129] << 8);
    if (volume) *volume = declared;
    uint32_t dir = le32(sector + 156 + 2), dir_size = le32(sector + 156 + 10), cnf = 0, cnf_size = 0;
    for (uint32_t off = 0; off < dir_size && off < 16 * SECTOR && !cnf; off += SECTOR) {
        if (!READ(dir + off / SECTOR)) break;
        for (unsigned i = 0; i + 33 < SECTOR && sector[i]; i += sector[i]) {
            const uint8_t *r = sector + i;
            if (r[32] >= 10 && i + 33 + r[32] <= SECTOR && same_name(r + 33, "SYSTEM.CNF", 10) &&
                (r[32] == 10 || r[33 + 10] == ';')) {
                cnf = le32(r + 2);
                cnf_size = le32(r + 10);
                break;
            }
        }
    }
    if (!cnf || !READ(cnf)) return DISC_NO_CNF;
#undef READ
    memcpy(text, sector, SECTOR);
    text[cnf_size < SECTOR ? cnf_size : SECTOR] = 0;
    for (size_t i = 0; i < SECTOR && text[i]; i++)
        if ((unsigned char)text[i] < 32 && text[i] != '\r' && text[i] != '\n' && text[i] != '\t') text[i] = ' ';
    if (strstr(text, "SCUS_971.77")) return !declared || file_size < declared ? DISC_CUT : DISC_GAME;
    /* name the game it is: BOOT2 = cdrom0:\SLUS_123.45;1 (none when the line names nothing) */
    const char *boot = strstr(text, "BOOT2");
    if (boot && (boot = strpbrk(boot, "\\:")) && id_size) {
        while (*boot == '\\' || *boot == ':') boot++;
        size_t n = strcspn(boot, ";\r\n ");
        if (n >= id_size) n = id_size - 1;
        memcpy(id, boot, n);
        id[n] = 0;
    }
    return DISC_OTHER;
}
