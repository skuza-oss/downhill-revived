/* The disc image's own check, before the game's data (finding 227): host.c's disc_problem asks it
   on every platform (the desktop's start, Android's and iOS's launchers, imports and starts through
   host_disc_check), so a phone checks a disc with the same code as the desktop. Plain C over a
   reader: port/host/test_disc_check.c runs it on small made-up images (no game data).
   GPL-3.0-or-later. */
#ifndef PORT_HOST_DISC_CHECK_H
#define PORT_HOST_DISC_CHECK_H

#include <stddef.h>
#include <stdint.h>

/* Reads n bytes at offset into buffer; 1 when all of them were read. */
typedef int (*DiscRead)(void *context, uint64_t offset, void *buffer, size_t n);

enum {
    DISC_GAME,     /* an ISO 9660 volume whose SYSTEM.CNF boots SCUS_971.77, held whole */
    DISC_NOT_ISO,  /* no primary volume descriptor */
    DISC_NO_CNF,   /* no SYSTEM.CNF in the root */
    DISC_OTHER,    /* another game: *id its boot file's name ("" when the line names none) */
    DISC_CUT       /* the right game, but the file is shorter than the volume it declares */
};

/* What the image is. file_size: the file's length in bytes; id (id_size, at least 1) gets the boot
   file's name of another game, cut to fit; *volume (may be NULL) the bytes the volume declares. */
int disc_check_image(DiscRead read, void *context, uint64_t file_size, char *id, size_t id_size, uint64_t *volume);

#endif
