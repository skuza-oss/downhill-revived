"""The streamed sound slots (port/hle/sound_slots.c, finding 222): a positional sound whose emitter
is none (the front end's chatter for the rider screen's empty place 0) is out of range instead of
read from address 0, so the game stops it as it stops any sound out of earshot. Where an emitter is
real nothing changes. Two test switches make the front end's chatter fire (the repeat)."""


def add(PATCHES):
    PATCHES['unsorted/001BBF30.c'] = PATCHES.get('unsorted/001BBF30.c', []) + [
        ('text', 'void func_001BBF30(Object *obj, int id, Info *info)\n{\n',
         'extern int port_sound_no_emitter(int slot, unsigned emitter, int key);\n'
         'void func_001BBF30(Object *obj, int id, Info *info)\n{\n'),
        # x2C + 4 is the slot's key (func_001BC480)
        ('text', '        if (D_004B25A0.x118 == 0) {\n',
         '        if (port_sound_no_emitter(id, (unsigned)info->x8, *(int *)(info->x2C + 4))) {\n'
         '            amount = 0;\n'
         '            angle = 0;\n'
         '        } else if (D_004B25A0.x118 == 0) {\n'),
    ]
    # The start's Doppler distance (one screen) reads the emitter when a race world is set; with none
    # it gives what no world gives (0).
    PATCHES['unsorted/001BC868.c'] = PATCHES.get('unsorted/001BC868.c', []) + [
        ('text', 'extern void func_001C2280(void *, float, float, float, float);',
         'extern void func_001C2280(void *, float, float, float, float);\n'
         'extern int port_sound_emitter_null(unsigned emitter);'),
        ('text', '    if (D_004B2828[0] == 1) {\n',
         '    if (D_004B2828[0] == 1 && !port_sound_emitter_null((unsigned)x8)) {\n'),
    ]
    PATCHES['unsorted/002337C0.cpp'] = PATCHES.get('unsorted/002337C0.cpp', []) + [
        ('text', 'int EventQueue::func_002337C0(int kind, int arg)\n{',
         'extern "C" int port_test_sound_event(int index);\n'
         'int EventQueue::func_002337C0(int kind, int arg)\n{'),
        ('text', '} else if (Random() % 100 < info->x2) {',
         '} else if ((Random() % 100 < info->x2) | port_test_sound_event(list[i]->index)) {'),
    ]
    PATCHES['unsorted/001C4E68.c'] = PATCHES.get('unsorted/001C4E68.c', []) + [
        ('text', 'void func_001C4E68(Obj *obj) {',
         'extern int port_test_shell_voice(int rider);\nvoid func_001C4E68(Obj *obj) {'),
        ('text', 'func_001C5040(manager+0x17C,obj->x6C)',
         'func_001C5040(manager+0x17C,port_test_shell_voice(obj->x6C))'),
    ]
