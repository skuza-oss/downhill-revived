/* The PS2 address space on the PC (memory.c). */
#ifndef PS2_MEMORY_H
#define PS2_MEMORY_H

#include <stdint.h>

#define PS2_RAM_BASE 0x08000000u   /* host address of PS2 address 0 (port/tools/data.py) */
#define PS2_RAM_SIZE 0x02000000u
#define PS2_IMAGE_START 0x263BC0u  /* .vutext, the first byte of the data image */
#define PS2_IMAGE_END 0x29DCF0u    /* the end of .sdata: .sbss and .bss after it are zero */
#define PS2_SCRATCHPAD 0x70000000u
/* Guest-visible memory the HLE owns, right above the scratchpad at the same address in every
   build. A pointer the HLE gives the game must not depend on where the core's linker put its own
   data: the game keeps it in its RAM, and the state trace compares that RAM between builds
   (finding 205). [PS2_HLE_WINDOW, +0x800): the SIF RPC packets (sif.c). */
#ifndef PS2_HLE_WINDOW  /* a unit test (test_sif.c) puts it inside its own small memory */
#define PS2_HLE_WINDOW 0x70004000u
#endif
#define PS2_HLE_WINDOW_SIZE 0x4000u
/* The RAM from here up is also at its own PS2 addresses (and | 0x20000000, | 0x30000000): the
   game's data files are linked for where it loads them (0xA00000 up) and hold absolute PS2
   pointers. Below it Windows has its own memory before the program starts. */
#define PS2_LOW_VIEW 0x00400000u

/* Host pointer of a PS2 RAM address (cached, uncached or a DMA address). */
#define PS2_PTR(addr) ((void *)(uintptr_t)(PS2_RAM_BASE + ((uint32_t)(addr) & 0x01FFFFFFu)))

/* Maps the RAM, its mirrors, the scratchpad and the hardware registers; nonzero on success.
   First thing at startup. */
int ps2_memory_init(void);
/* The data image in place: the retail bytes of PS2_IMAGE_START..PS2_IMAGE_END (read from the
   player's disc, port/hle/game_data.c) copied in, and the words the PC build changes (pointers
   to host addresses and recompiled functions, port/tools/data.py) set. Then the VU kernels take
   their microcode from it. */
void ps2_image_load(const uint8_t *retail);

#endif
