/* host_widescreen_flat_limit's rule, read from guest RAM: by the core (port/hle/widescreen.c) and by
   the browser's GS reactor (port/web/gpu_rust_web_native.inc), which shares the core's memory. */
#ifndef WIDESCREEN_FLAT_H
#define WIDESCREEN_FLAT_H

#include <stdint.h>
#include "../runtime/memory.h"   /* PS2_RAM_BASE */

/* Maximum PATH1 count of a full-screen loading picture in this state, or zero
   where a small 3D race view must never be mistaken for a card. The tiled
   cards draw 144..170 (or none); everything else in these states draws
   thousands. The goggles before a level (the shop exit, 18, and screen 9 phase
   0) draw 3,648 and up: they are 3D, Hor+ like the rest, so they are shown
   wide (finding 140; finding 072 had counted them as a card, which squashed
   them at 4:3). */
static inline int widescreen_flat_limit(void)
{
    int menu = *(volatile int32_t *)(uintptr_t)(PS2_RAM_BASE + 0x29DED8);
    int phase = *(volatile int32_t *)(uintptr_t)(PS2_RAM_BASE + 0x43F348);
    if (menu == 0 || menu == 18 || (menu == 9 && (phase == 0 || phase == 5))) return 1000;
    return 0;
}

#endif
