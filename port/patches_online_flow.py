"""The race's pause menu in an online room (port/hle/online.c, finding 118): its rows are the
room's (RESUME, BACK TO THE ROOM, END THE RACE for the host, OPTIONS, LEAVE THE ROOM) instead of
NEW LEVEL, RE-START and QUIT. The builder (func_0015E238) asks online.c first and the accept
(func_0015E448) lets it handle its own rows; offline both return 0 and the menu is retail's. The
room's rows ask in the game's quit question (func_0015DE40 / func_0015DED0), which offline is
retail's too.
"""


def add(PATCHES):
    PATCHES['unsorted/0015E238.c'] = PATCHES.get('unsorted/0015E238.c', []) + [
        ('text', 'void func_0015E238(Menu *menu) {',
         'extern int port_online_pause_rows(void *menu);\nvoid func_0015E238(Menu *menu) {'),
        ('text', ' menu->x418=0;\n',
         ' menu->x418=0;\n'
         ' if(port_online_pause_rows(menu)) { menu->x470=0; menu->x46C=1; return; }\n'),
    ]
    PATCHES['unsorted/0015E448.cpp'] = PATCHES.get('unsorted/0015E448.cpp', []) + [
        ('text', 'void func_0015E448(Menu *obj,int index) {\n',
         'extern "C" int port_online_pause_accept(int index);\n'
         'void func_0015E448(Menu *obj,int index) {\n'
         ' if (port_online_pause_accept(index)) return;\n'),
    ]
    # The quit question (D_00387F90) asks about the room's row instead; its Yes acts on it, then
    # leaves the race screen as QUIT's Yes does (D_00388E90, func_0015E0C0).
    PATCHES['unsorted/0015DE40.c'] = PATCHES.get('unsorted/0015DE40.c', []) + [
        ('text', 'void func_0015DE40(struct Obj *p) {',
         'extern void port_online_question(void *menu);\nvoid func_0015DE40(struct Obj *p) {'),
        ('text', '    p->x4A8 = 5;\n}', '    p->x4A8 = 5;\n    port_online_question(p);\n}'),
    ]
    PATCHES['unsorted/0015DED0.c'] = PATCHES.get('unsorted/0015DED0.c', []) + [
        ('text', 'void func_0015DED0(Obj *obj, int index) {',
         'extern void port_online_question_yes(void);\nvoid func_0015DED0(Obj *obj, int index) {'),
        ('text', '        func_00190660(D_003F52D0, D_00388E90);',
         '        port_online_question_yes();\n        func_00190660(D_003F52D0, D_00388E90);'),
    ]
