"""Widescreen 2D and HUD (finding 056): the game's 2D draw code maps its X through
port/hle/widescreen.c, so menus, popups and the race HUD keep their shape in a wider picture.
With widescreen off every helper returns its input, so nothing changes. Merged into
port/patches.py's PATCHES (same entry kinds, same checks).
"""

WS = 'extern int port_ws_point(int), port_ws_size(int), port_ws_hud_point(int, int, int);\n' \
     'extern void port_ws_span(int *, int *);\n'

# The HUD's own code draws its text through port_hud_text_c / _l (func_0020F9F0 centred,
# func_0020F9A8 left-aligned), which tell the font to anchor each string to its side. The
# counts are the calls (and declarations) in each unit.
HUD_TEXT = {'00175730.c': (5, 0), '00176ED8.c': (3, 0), '001772B0.c': (0, 7), '0017A328.cpp': (12, 0),
            '0017A678.cpp': (33, 0), '0017B520.c': (2, 0), '0017B5C8.c': (0, 2)}


def add(PATCHES):
    def more(unit, *entries):
        PATCHES['unsorted/' + unit] = PATCHES.get('unsorted/' + unit, []) + list(entries)

    for unit, (centred, left) in HUD_TEXT.items():
        if centred:
            more(unit, ('re', (r'\bfunc_0020F9F0\b', centred), 'port_hud_text_c'))
        if left:
            more(unit, ('re', (r'\bfunc_0020F9A8\b', left), 'port_hud_text_l'))

    # The loading card's setup (the card and its text, drawn for eight frames, then sent again by
    # the loading thread): its 2D unmapped, as the card is shown 4:3 (finding 140).
    more('00182D78.c',
         ('text', 'extern void func_0014DE90(Entry *);', 'extern void func_0014DE90(Entry *);\nextern void port_card_setup(int);'),
         ('text', '    on = 1;\n    func_00238B20();', '    on = 1;\n    port_card_setup(1);\n    func_00238B20();'),
         ('text', '    func_0025AF98(2);\n}', '    func_0025AF98(2);\n    port_card_setup(0);\n}'))

    # The HUD's 3D pieces, placed in front of the camera: a piece at a side moves out with it.
    more('00176930.c',
         ('text', 'extern Game D_004B25A0;', 'extern Game D_004B25A0;\nextern void port_ws_hud_offset(int, float *);'),
         ('text', '    mat = &obj->x40;', '    port_ws_hud_offset(owner->x1280, &local.x);\n    mat = &obj->x40;'))

    # Text: each string's side (func_0020FBB0), then each glyph's left side and width.
    more('0020FBB0.c',
         ('text', 'extern int func_0020FDB0(int, int, int);',
          'extern int func_0020FDB0(int, int, int);\nextern void port_ws_text_begin(int, int), port_ws_text_end(void);'),
         ('text', '    int adjust = 0;\n', '    int adjust = 0;\n    port_ws_text_begin(arg1, (short)font->x14);\n'),
         ('text', '    font->x1C = 0;\n}', '    font->x1C = 0;\n    port_ws_text_end();\n}'))
    more('0020FDB0.c',
         ('text', 'extern int func_0020F6C0(int,int *);',
          'extern int func_0020F6C0(int,int *);\nextern int port_ws_glyph_x(int,int), port_ws_glyph_w(int);'),
         ('text', 'px=font->x10+dx;', 'px=port_ws_glyph_x(w,(short)font->x10)+dx;'),
         ('text', 'lx=font->x8+px;', 'lx=port_ws_glyph_w(font->x8)+px;'))

    # Popup and message boxes (x and width relative to the view's left side, made absolute here).
    for unit in ('00190E48.c', '001E4950.c'):
        more(unit, ('text', 'extern void func_0023D618(int,int *,int *),func_0023D650(int,int *,int *);',
                    'extern void func_0023D618(int,int *,int *),func_0023D650(int,int *,int *);\n' + WS),
             ('text', 'x+=vals[0]-(vals[2]>>1); y+=vals[1]-(vals[3]>>1);',
              'x+=vals[0]-(vals[2]>>1); y+=vals[1]-(vals[3]>>1); port_ws_span(&x,&width);'))
    for unit in ('00151AD0.c', '00169BD0.c'):
        more(unit, ('text', 'extern void func_0023D650(int,int *,int *);', 'extern void func_0023D650(int,int *,int *);\n' + WS),
             ('text', '    top += y-(h>>1);\n', '    top += y-(h>>1);\n    port_ws_span(&left,&width);\n'))
    # The engine's 2D vertex (popup highlight and value bars, the front end's fades).
    more('00210578.c', ('text', 'void func_00210578(int a, struct Quad *q, int x, int y)\n{\n    q->x0 = x << 4;',
                        WS + 'void func_00210578(int a, struct Quad *q, int x, int y)\n{\n    q->x0 = port_ws_point(x) << 4;'))
    # A HUD marker drawn in 2D at a fixed place (a diamond), and the markers over the other
    # riders' heads (at their projected place, which is right already: only their width).
    more('0017B0D8.c',
         ('text', 'extern int func_0023DED0(void);', 'extern int func_0023DED0(void);\n' + WS),
         ('text', '    obj->xC80->d[0] = ((unsigned long)0x8109 << 46) | 0x8005;',
          '    int port_d = port_ws_size(14);\n    cx = port_ws_hud_point(obj->x1280, cx, cx);\n'
          '    obj->xC80->d[0] = ((unsigned long)0x8109 << 46) | 0x8005;'),
         ('text', 'obj->xC80->w[0] = (cx + 14) << 4;', 'obj->xC80->w[0] = (cx + port_d) << 4;'),
         ('text', 'obj->xC80->w[0] = (cx - 14) << 4;', 'obj->xC80->w[0] = (cx - port_d) << 4;'))
    more('0017B848.c',
         ('text', 'extern State D_004B25A0;', 'extern State D_004B25A0;\n' + WS),
         ('text', '{ w=12;h=6; }', '{ w=12;h=6; }\n    w=port_ws_size(w);'))
