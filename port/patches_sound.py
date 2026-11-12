"""A sound's angle to the listener with no position (port/hle/sound_slots.c, finding 222).

func_00208C60 (the angle from one point to another, read with lqc2) takes a null point as a point at
address 0: on the PS2 the kernel's memory, in the port a fault. Every distance the sound code takes
(func_00222330) reads a null point as the origin; the angle now does the same. The front end's chatter
for the rider screen's empty place 0 has such a point: its distance to the listener puts it out of
range, so the game stops it at once, as the PS2 did (it read the angle from the kernel's words and
dropped it). Where both points are real nothing changes. Two test switches make that chatter fire
(the repeat)."""


def add(PATCHES):
    PATCHES['unsorted/00208C60.c'] = PATCHES.get('unsorted/00208C60.c', []) + [
        ('text', 'extern float func_00248D90(float, float);',
         'extern float func_00248D90(float, float);\n'
         'extern float *port_sound_point(float *point);'),
        ('text', '    heading = heading * 57.29578f;\n',
         '    a = port_sound_point(a);\n'
         '    b = port_sound_point(b);\n'
         '    heading = heading * 57.29578f;\n'),
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
