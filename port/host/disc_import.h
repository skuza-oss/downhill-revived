/* The phones' disc import rules, once for Android and iOS (findings 227, 242). The launchers' import
   jobs (Android's DiscImport.java through DiscImage's JNI, disc_check_jni.c; iOS's launcher.m
   job_run) keep only what their OS does: open the document (a content URI's stream on Android; a
   security-scoped URL under file coordination on iOS), the free-space query, the copy's reads and
   writes, and the page's events. Here: the constants, the room rule, the copy's verdict
   (completeness against the source's size and the volume's), the copy loop over FILEs (iOS's) and
   which import may put its file in place. Whether a file is the game is disc_check.c's
   (disc_check_image), as everywhere. Plain C: port/host/test_disc_import.c runs it.
   GPL-3.0-or-later. */
#ifndef DHD_DISC_IMPORT_H
#define DHD_DISC_IMPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The copy loop's block; progress is reported every DISC_COPY_STEP bytes. */
#define DISC_COPY_BLOCK (1u << 20)
#define DISC_COPY_STEP (16ull << 20)
/* Room kept free besides the copy (saves, dhd.log, the texture cache). */
#define DISC_COPY_MARGIN (128ull << 20)
/* A copy that failed with less than this free: "out of space" rather than "the copy failed". */
#define DISC_COPY_LOW (64ull << 20)

/* The bytes the import needs free for a source of size bytes (0: unknown, nothing checked): 0 when
   free_bytes holds it, else size + DISC_COPY_MARGIN (the page's "need"). */
uint64_t disc_room_needed(uint64_t size, uint64_t free_bytes);

/* What disc_check_image says of f (DISC_GAME, ...), and the bytes its volume declares in *volume
   (0 without a primary volume descriptor). */
int disc_check_file(FILE *f, uint64_t *volume);

/* Whether a finished copy is complete: NULL if it is, else why not (into why). copied: the bytes
   written; size: the source's size (0 unknown); exact: size is the file's own (stat) rather than a
   document provider's word (Android's OpenableColumns.SIZE), which a copy may run past but never
   stop short of; volume: the copy's declared volume. The copy must hold the whole volume. */
const char *disc_completeness(uint64_t copied, uint64_t size, int exact, uint64_t volume, char *why, size_t why_size);

/* The copy's verdict after its check: kind and volume are disc_check_image's for the copy. A copy
   of the game, or one cut short of its volume, says the copy's own reason first when it is not
   complete ("the copy stopped early: try again", finding 234); NULL when it is complete. Any other
   kind is the caller's own message (NULL here). */
const char *disc_copy_verdict(int kind, uint64_t copied, uint64_t size, int exact, uint64_t volume,
                              char *why, size_t why_size);

/* Copies in to out until in ends; *cancel is asked before every block (nonzero: stop, -1 is
   returned), progress(context, bytes so far) every DISC_COPY_STEP. Returns the bytes copied, or -1
   on a read or write error or a cancel. */
int64_t disc_copy(FILE *in, FILE *out, const volatile int *cancel,
                  void (*progress)(void *context, uint64_t done), void *context);

/* The import's publication (finding 227): each pick or retry begins a generation, which makes every
   older import superseded; an import puts its file in place (rename, or the move of a file already
   in the app's folder) only while its generation is still the newest, checked and done under one
   lock, so a superseded import can never land over a newer choice. */
long long disc_publish_begin(void);
int disc_publish_current(long long generation);
/* rename(from, to) when generation is the newest: 0 done, 1 superseded (nothing done), -1 failed. */
int disc_publish(long long generation, const char *from, const char *to);

#ifdef __cplusplus
}
#endif

#endif
