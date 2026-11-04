/* Width immediates in the VU1 packet uploaded by the chain at 0x269C50.
   GPL-3.0-or-later. */
#ifndef DHD_WIDESCREEN_SPRITES_H
#define DHD_WIDESCREEN_SPRITES_H
#include <stdint.h>
static const struct {
    uint32_t ee, vu, upper;
} ws_sprite_widths[] = {
    {0x26C448, 0x27C8, 0x81F51E8A}, /* vf7.w: X-only half extent */
    {0x26C5C0, 0x2938, 0x81F508BC}, /* tree sprites */
    {0x26C700, 0x2A78, 0x81F410BD},
    {0x26C758, 0x2AD0, 0x8020965B},
    {0x26C7D8, 0x2B50, 0x80400218}, /* vf8.x: X scale; vf8.y still comes from 138.0 */
    {0x26C8E8, 0x2C60, 0x80400218},
};
#endif
