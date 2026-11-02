/* The game's font drawn sharp at high resolutions (font_sharp.c, finding 058). */
#ifndef PORT_GS_FONT_SHARP_H
#define PORT_GS_FONT_SHARP_H
#include <stdint.h>
#include "replace_texture.h"

/* DHD_FONT (dhd.ini "font"): sharp (the default) or original. Read once. */
int font_sharp_on(void);

/* Whether a context's TEX0 is the text engine's font texture: the one its text packets set
   (D_004F35B0, func_00210068 and func_00210250). Reads EE memory, writes none. */
int font_sharp_match(uint64_t tex0);

/* A high-resolution version of a decoded font texture (w x h GS colours, alpha 0x80 opaque), made
   in memory only, by device: a new reference to it (device->release when done), or NULL. key
   names the decoded texture; the last few are kept, so decoding the same font again costs
   nothing. One device per process. */
void *font_sharp_view(const ReplaceDevice *device, const uint32_t *pixels, unsigned w, unsigned h, uint64_t key);

/* The button icons (button_icons.c, finding 134; native builds only): style says which icons the
   menus' atlas gets now (0 the game's own), paint puts them into the copy. Without them (the
   browser) the copy is the font's alone. */
typedef void FontSharpPaint(uint8_t *big, unsigned f, const uint32_t *pixels, unsigned w, unsigned h, unsigned solid,
                            int style);
void font_sharp_icons(int (*style)(void), FontSharpPaint *paint);
int font_sharp_icon_style(void);  /* 0 without icons */

/* A copy is made: the sharp font, or other icons than the game's (the copy then keeps the font as
   it is, only bigger). */
int font_sharp_active(void);

#endif
