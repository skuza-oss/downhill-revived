/* The button icons for the player's controller (button_icons.c, finding 134). */
#ifndef PORT_GS_BUTTON_ICONS_H
#define PORT_GS_BUTTON_ICONS_H
#include <stdint.h>

enum { BUTTON_ICONS_GAME = 0, BUTTON_ICONS_XBOX = 1, BUTTON_ICONS_NINTENDO = 2 };

/* Each field on the game thread (host.c): the setting (DHD_PROMPTS: auto, playstation, xbox,
   nintendo; NULL the game's own) and what auto means now (player 1's controller: BUTTON_ICONS_*). */
void button_icons_choose(const char *setting, int auto_style);

/* BUTTON_ICONS_*: the icons the font copy gets now (any thread). */
int button_icons_style(void);

/* Paints the style's icons over the game's button icons in a copy of the menus' font atlas
   (font_sharp.c): big is the copy, W x H RGBA (alpha `solid` where the font is solid, colours as
   the game's palette gives them), f times the decoded atlas `pixels` (w x h). Nothing unless the
   atlas is the menus' (512 x 512) and a style is chosen. */
void button_icons_paint(uint8_t *big, unsigned f, const uint32_t *pixels, unsigned w, unsigned h, unsigned solid, int style);

#endif
