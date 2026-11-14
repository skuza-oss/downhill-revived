/* SHA-256 (FIPS 180-4), portable C. The online lobby hashes the disc image and the executable with
   it (hle/online_menu.c, hle/online.c). */
#ifndef DHD_SHA256_H
#define DHD_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t length;      /* bytes hashed so far */
    uint8_t block[64];
    unsigned fill;        /* bytes waiting in block */
} Sha256;

void sha256_init(Sha256 *c);
void sha256_update(Sha256 *c, const void *data, size_t size);
void sha256_final(Sha256 *c, uint8_t digest[32]);

#endif
