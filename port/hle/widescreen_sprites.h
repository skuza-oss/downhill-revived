/* Sprite widths in the VU1 package uploaded by the DMA chain at 0x269C50 (overlay B, finding 019),
   for widescreen (finding 192). GPL-3.0-or-later.

   Each is a lower instruction word that is a LOI immediate (the upper word's I bit set): 277.0,
   the 1P race's focal length in pixels at 4:3. The programs multiply a sprite's world size by it
   and by 1/w for a half width in screen pixels, in X only; Y comes from elsewhere. The scene's X
   focal length is times s in widescreen (finding 043), so these are too, or the sprites are drawn
   1/s wider than the scene around them. The PCSX2 widescreen patch for the disc changes the three
   tree widths; the other three are the same pattern in the program at VU 0x2738 and the two
   rectangle programs at 0x2B10 and 0x2C20 (whose half height is LOI 138.0, left alone).

   EE address of the lower word, its retail value, and the upper word it pairs with (both are
   checked before the lower word is changed, so a second run or another package leaves it). */
#ifndef DHD_WIDESCREEN_SPRITES_H
#define DHD_WIDESCREEN_SPRITES_H
#include <stdint.h>
static const struct {
    uint32_t ee, retail, upper;
} ws_sprite_widths[] = {
    {0x26C448, 0x438A8000, 0x81F51E8A}, /* VU 0x27C8: vf7.w, the X half width (program 0x2738) */
    {0x26C5C0, 0x438A8000, 0x81F508BC}, /* VU 0x2938: trees (program 0x28D0; PCSX2 patch) */
    {0x26C700, 0x438A8000, 0x81F410BD}, /* VU 0x2A78 (PCSX2 patch) */
    {0x26C758, 0x438A8000, 0x8020965B}, /* VU 0x2AD0 (PCSX2 patch) */
    {0x26C7D8, 0x438A8000, 0x80400218}, /* VU 0x2B50: vf8.x, the X half extent (program 0x2B10) */
    {0x26C8E8, 0x438A8000, 0x80400218}, /* VU 0x2C60: vf8.x (program 0x2C20) */
};
#endif
