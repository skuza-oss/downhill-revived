/* The disc image's copy into the iOS app (the launcher's pick_disc, port/ios/app/disc_import.m;
   finding 227): the copy loop and the completeness rule of Android's DiscImage.java, in plain C so
   port/ios/tests/test_disc_copy.c runs them anywhere. Whether a file is the game is host.c's
   host_disc_check, as on Android. GPL-3.0-or-later. */
#ifndef DHD_IOS_DISC_COPY_H
#define DHD_IOS_DISC_COPY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Progress is reported every DISC_COPY_STEP bytes. */
#define DISC_COPY_STEP (16ull << 20)
/* Room kept free besides the copy (saves, dhd.log, the texture cache): DiscImport.MARGIN. */
#define DISC_COPY_MARGIN (128ull << 20)

/* The bytes the ISO 9660 volume in f declares (the primary volume descriptor's volume space size
   times its logical block size), 0 when there is no such descriptor. */
uint64_t disc_volume_bytes(FILE *f);

/* Whether a finished copy is complete: NULL if it is, else why not (into why). copied: the bytes
   written; size: the source's size (0 unknown); volume: the copy's disc_volume_bytes. The copy
   must hold the whole volume and as many bytes as the source has. */
const char *disc_completeness(uint64_t copied, uint64_t size, uint64_t volume, char *why, size_t why_size);

/* Copies in to out until in ends; *cancel is asked before every block (nonzero: stop, -1 is
   returned), progress(context, bytes so far) every DISC_COPY_STEP. Returns the bytes copied, or -1
   on a read or write error or a cancel. */
int64_t disc_copy(FILE *in, FILE *out, const volatile int *cancel,
                  void (*progress)(void *context, uint64_t done), void *context);

/* The import's publication (finding 227): each pick or retry begins a generation, which makes every
   older import superseded; an import puts its file in place (rename, or the move of a file already
   in Documents) only while its generation is still the newest, checked and done under one lock, so a
   superseded import can never land over a newer choice. */
long long disc_publish_begin(void);
int disc_publish_current(long long generation);
/* rename(from, to) when generation is the newest: 0 done, 1 superseded (nothing done), -1 failed. */
int disc_publish(long long generation, const char *from, const char *to);

#endif
