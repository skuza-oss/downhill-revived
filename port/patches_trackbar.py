"""Track bar names (finding 129): after the HUD has moved each rider's dot along the track bar
(func_00179338), port/hle/trackbar.c draws a name tag right of each player's dot."""


def add(patches):
    patches.setdefault('unsorted/00176ED8.c', []).extend([
        ('text', 'extern void func_00179338(Obj *);', 'extern void func_00179338(Obj *);\nextern void hle_trackbar(Obj *);'),
        ('text', '                    func_00179338(obj);\n', '                    func_00179338(obj);\n                    hle_trackbar(obj);\n'),
    ])
