/* The game's PACKED triangle strips, read where the packet holds them (finding 197).

   Every strip tag the game's VU1 programs kick has one register list: ST, RGBAQ, XYZF2 (GIF REGS
   0x412), three qwords a vertex. gs_feed hands such a tag's vertices to the backend as they lie
   in the packet (gs_strip_hook), so the backend decodes and records each in one pass; any other
   list, and a tag that continues a strip, goes through gs.c's vertex list (Gs draw_strip). */
#ifndef PORT_GS_STRIP_H
#define PORT_GS_STRIP_H
#include "gs.h"
#include <string.h>

enum { GS_STRIP_REGS = 0x412, GS_STRIP_WORDS = 12 };  /* the register list; words a vertex */

/* The vertex at w is written with ADC (XYZF3): its kick draws nothing. */
static inline int gs_strip_adc(const uint32_t *w) { return w[11] >> 15 & 1; }

/* The vertex register as the three qwords at w leave it (gs.c's packed()): ST's Q reaches the
   vertex with the RGBAQ after it; UV is not written, so u and v stay as `before` has them. */
static inline void gs_strip_vertex(const uint32_t *w, const GsVertex *before, GsVertex *v)
{
    v->x = w[8] & 65535; v->y = w[9] & 65535; v->z = w[10] >> 4 & 0xFFFFFF;
    v->rgba = (w[4] & 255) | (w[5] & 255) << 8 | (w[6] & 255) << 16 | (w[7] & 255) << 24;
    v->fog = w[11] >> 4 & 255;
    v->u = before->u; v->v = before->v;
    memcpy(&v->s, w, 12);  /* s, t, q */
}

/* Set by the backend, beside Gs draw and draw_strip: the triangles of `count` (three or more)
   such vertices kicked into an empty strip window. Vertex k is words + k * GS_STRIP_WORDS;
   triangle k is vertices k to k + 2, and draw would have been called for it unless vertex k + 2
   has ADC. The same draws, in the same order, with the same GS state but for the vertex
   registers, which already hold the strip's last values. Outside struct Gs, whose size the core's
   state trace holds. */
extern void (*gs_strip_hook)(Gs *gs, const uint32_t *words, unsigned count);

#endif
