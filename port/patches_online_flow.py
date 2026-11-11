"""The race's pause menu in an online room (port/hle/online.c, finding 118): its rows are the
room's (RESUME, BACK TO THE ROOM, END THE RACE for the host, OPTIONS, LEAVE THE ROOM) instead of
NEW LEVEL, RE-START and QUIT. The builder (func_0015E238) asks online.c first and the accept
(func_0015E448) lets it handle its own rows; offline both return 0 and the menu is retail's. The
room's rows ask in the game's quit question (func_0015DE40 / func_0015DED0), which offline is
retail's too.

The arena's scored round (finding 204, port/hle/online.c "the arena's scored round"): an online
race on the Mosh Pit arena runs in the game's Mosh Bowl mode. Its end on this PC (func_001B4718,
the timer out) waits for every rider's final score before the game's own end runs on; its COMBAT
RESULTS (func_00160A48) list the riders in the round's order (the same on every PC) with the
players' names, and in a room its only row is BACK TO THE ROOM (func_00160980, func_00161050).
Offline every hook returns the game's own value.
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
    # The arena's round: this PC's two minutes ran out; the game's own end waits for the scores.
    PATCHES['unsorted/001B4718.c'] = PATCHES.get('unsorted/001B4718.c', []) + [
        ('text', 'extern void func_001B45F8(Obj *, int, int);',
         'extern void func_001B45F8(Obj *, int, int);\nextern int hle_online_round_end(void);'),
        ('text', '                func_001B45F8(obj, 1, 120);',
         '                if (!hle_online_round_end()) func_001B45F8(obj, 1, 120);'),
    ]
    # COMBAT RESULTS: its rows (func_00160980), its order and names (func_00160A48), its accept.
    PATCHES['unsorted/0015FF38.c'] = PATCHES.get('unsorted/0015FF38.c', []) + [
        ('text', 'void func_00160980(MenuObj *p) {',
         'extern int port_online_round_rows(void *);\nextern int port_online_round_rider(int, int);\n'
         'extern const char *port_online_round_name(void *, const char *);\nvoid func_00160980(MenuObj *p) {'),
        ('text', '    func_0018E3A8((struct SharedMenu *)(void *)(p), 0x28, &D_0029DE68, 1);\n'
                 '    func_0018E3A8((struct SharedMenu *)(void *)(p), 0x23, &D_0029DE68, 1);\n'
                 '    func_0018E3A8((struct SharedMenu *)(void *)(p), 0x1A, &D_0029DE68, 1);\n',
         '    if (!port_online_round_rows(p)) {\n'
         '    func_0018E3A8((struct SharedMenu *)(void *)(p), 0x28, &D_0029DE68, 1);\n'
         '    func_0018E3A8((struct SharedMenu *)(void *)(p), 0x23, &D_0029DE68, 1);\n'
         '    func_0018E3A8((struct SharedMenu *)(void *)(p), 0x1A, &D_0029DE68, 1);\n'
         '    }\n'),
        ('text', '            if (bestIdx != -1) {\n',
         '            bestIdx = port_online_round_rider(i, bestIdx);\n            if (bestIdx != -1) {\n'),
        ('text', '                name = func_00158650(r->x796C);\n',
         '                name = (char *)port_online_round_name(r, func_00158650(r->x796C));\n'),
    ]
    PATCHES['unsorted/00161050.cpp'] = PATCHES.get('unsorted/00161050.cpp', []) + [
        ('text', 'void func_00161050(Menu *menu, int index)\n{\n',
         'extern "C" int port_online_round_accept(int index);\n'
         'void func_00161050(Menu *menu, int index)\n{\n    if (port_online_round_accept(index)) return;\n'),
    ]
