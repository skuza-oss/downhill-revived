/* The disc image's own check (port/host/disc_check.c, finding 227) on small made-up ISO 9660
   images, no game data: the right game whole, the same cut short with its header kept, a
   SYSTEM.CNF with "BOOT2 =" and nothing after it, an over-long boot name, another game, no
   SYSTEM.CNF, no volume at all. Each answer must be a clean one. GPL-3.0-or-later. */
#include "disc_check.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { SECTOR = 2048, BLOCKS = 64 };
static unsigned char image[BLOCKS * SECTOR];

static int read_image(void *context, uint64_t offset, void *buffer, size_t n)
{
    uint64_t length = *(uint64_t *)context;
    if (offset + n > length) return 0;
    memcpy(buffer, image + offset, n);
    return 1;
}

static void le32(unsigned char *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }

/* A volume of BLOCKS blocks: the primary descriptor at 16, the root at 20, SYSTEM.CNF at 21. */
static void make(const char *cnf, int with_cnf)
{
    memset(image, 0, sizeof image);
    unsigned char *pvd = image + 16 * SECTOR;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    le32(pvd + 80, BLOCKS);
    pvd[128] = SECTOR & 255, pvd[129] = SECTOR >> 8;
    le32(pvd + 156 + 2, 20);
    le32(pvd + 156 + 10, SECTOR);
    unsigned char *r = image + 20 * SECTOR;
    if (with_cnf) {
        r[0] = 33 + 12;
        le32(r + 2, 21);
        le32(r + 10, (uint32_t)strlen(cnf));
        r[32] = 12;
        memcpy(r + 33, "SYSTEM.CNF;1", 12);
    }
    memcpy(image + 21 * SECTOR, cnf, strlen(cnf));
}

static int check(uint64_t length, char *id, uint64_t *volume)
{
    return disc_check_image(read_image, &length, length, id, 32, volume);
}

int main(void)
{
    char id[32];
    uint64_t volume, whole = sizeof image;
    make("BOOT2 = cdrom0:\\SCUS_971.77;1\r\nVER = 1.00\r\n", 1);
    assert(check(whole, id, &volume) == DISC_GAME && volume == whole);
    /* a download cut short that kept its header and SYSTEM.CNF */
    assert(check(30 * SECTOR, id, &volume) == DISC_CUT && volume == whole);
    /* BOOT2 with nothing after it: another disc, no name, nothing read past the line */
    make("BOOT2 =", 1);
    assert(check(whole, id, NULL) == DISC_OTHER && !id[0]);
    make("BOOT2 = cdrom0:\\", 1);
    assert(check(whole, id, NULL) == DISC_OTHER && !id[0]);
    /* an over-long boot name: cut to fit */
    char longer[400] = "BOOT2 = cdrom0:\\";
    memset(longer + strlen(longer), 'A', 300);
    make(longer, 1);
    assert(check(whole, id, NULL) == DISC_OTHER && strlen(id) == 31);
    /* another game, named */
    make("BOOT2 = cdrom0:\\SLUS_123.45;1\r\n", 1);
    assert(check(whole, id, NULL) == DISC_OTHER && !strcmp(id, "SLUS_123.45"));
    /* bytes that aren't text in SYSTEM.CNF */
    make("BOOT2\x01\x02 = \x7f", 1);
    assert(check(whole, id, NULL) == DISC_OTHER);
    /* no SYSTEM.CNF; no volume; too short for a descriptor */
    make("", 0);
    assert(check(whole, id, NULL) == DISC_NO_CNF);
    memset(image, 0, sizeof image);
    assert(check(whole, id, NULL) == DISC_NOT_ISO);
    assert(check(100, id, NULL) == DISC_NOT_ISO);
    puts("disc_check: whole, cut, empty and long boot names, another game, no SYSTEM.CNF, no volume");
    return 0;
}
