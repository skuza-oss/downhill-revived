/* The disc image's online check: the C interface of the Rust crate dhd_disc (port/render/disc,
   linked into dhd_render's static library). Online races need the USA disc's game files: the
   hash covers every file of the image's ISO 9660 file system (path, size, contents), not the
   sectors around them (finding 098). The crate also keeps the answer in iso-check.txt, as the
   launcher does with the same crate. Strings are UTF-8 and NUL-terminated. */
#ifndef DHD_DISC_H
#define DHD_DISC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The check of the disc image at iso, with iso-check.txt at cache (NULL: none). It reads the
   whole image unless cache holds this file's answer, so it belongs on a thread. On an answer,
   digest gets the game files hash (32 bytes) and note (size bytes, cut to fit) what differs from
   the USA disc ("" for its files); else note says why there is none. 1 hashed now, 2 from
   iso-check.txt, -1 the image can't be read, -2 its game files can't be the USA disc's (no
   ISO 9660 file system, or a cut dump), -3 stopped. After each 1 MiB piece read it rests
   pause_us microseconds, and stops (nothing cached) once *stop is nonzero (stop NULL: never),
   so a game can play while it hashes (finding 208). */
int dhd_disc_check(const char *iso, const char *cache, uint8_t digest[32], char *note, size_t size,
                   const int *stop, uint32_t pause_us);

#ifdef __cplusplus
}
#endif

#endif
