#ifndef DHD_GS_QUEUE_H
#define DHD_GS_QUEUE_H
#include <stdint.h>
/* VIF-owned provenance travels with copied GIF data; GS never reads it live. */
typedef struct {
    unsigned entry, top, itop, pc, clip, qword, overlay, program;
    unsigned shadow;  /* kicked by a VU1 batch the native renderer draws from its mesh item (vu1_mesh.inc) */
    double seconds;
} GraphicsProvenance;
extern GraphicsProvenance graphics_provenance;
void graphics_gif_feed(unsigned path, const uint32_t *words, unsigned count);
int graphics_xgkick(uint32_t memory, unsigned qword);  /* memory: guest address */
void graphics_gs_wait(void);
enum { GS_PATH_MARK = 0x100 };  /* a marker the VIF/VU worker passed on (graphics_queue.c) */
enum { GS_PATH_MESH = 0x101 };  /* a translated VU1 mesh batch (port/gs/gs_mesh.h, gs_mesh) */
#endif
