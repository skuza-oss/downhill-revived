"""PC SETTINGS "Unlock All" (port_unlock_all, port/hle/pc_settings.c): while it is on, the game's
unlock checks read as the retail unlock-all code (func_001F0780) leaves them, without that code's
writes. Only readers are patched: the profile D_004225A0 (the memory card's 0x12240 bytes) is never
written, so turning the option off brings the save's own state back and a save made with it on
stores none of the extras. The writers (career rewards func_0014E6A0/func_00169920/func_0014F868,
the shop func_0013F730, func_00194290) keep reading the real fields, so real progress is still
recorded. Finding 095 lists each field and its readers.

The tracks (+0x12114, func_00193F38) are patched in patches.py next to its accessor."""

DECL = 'extern int port_unlock_all(void);\n'
ONLINE = 'extern int port_online_bikes(void);\n'  # online: every bike in the garage too (finding 128)
DECL_CPP = 'extern "C" int port_unlock_all(void);\n'


def _head(patches, unit, first_line, decl=DECL):
    patches.setdefault(unit, []).append(('text', first_line + '\n', first_line + '\n' + decl))


def _sub(patches, unit, pairs):
    patches.setdefault(unit, []).extend(('text', a, b) for a, b in pairs)


# Bike garage (profile +0x11C68, a 63-entry list of {frame, part, colour} that all six
# D_004C8508 groups point at, func_001FA590): the cheat appends each of the 41 preset bikes
# D_0026DEA0 (count D_0029C3D4) it lacks. Its readers take it through func_001F9B98 (entries)
# and func_001F9BF0 (count); with the option on both answer from a list of the port's own,
# the save's bikes first and then the missing presets, as the cheat would order them, up to its
# 63 (func_001953B8). Additions (shop, pro riders, career) still go to the real list
# through func_001F9C78. Online rooms (port_online_bikes, finding 128) use the same list, with the
# single race's stock bike 17 4 0 after the save's own: online's default for the ten riders.
_LIST = r'''
typedef struct PortBike { int x0, x4, x8; } PortBike;
extern PortBike D_0026DEA0[];
extern int D_0029C3D4;
static PortBike port_unlock_bikes[63];
int port_unlock_list(char *real, char **items)
{
    PortBike *have = (PortBike *)real;
    int count = *(int *)(real + 0x4A4), i, j;
    if (count > 63) count = 63;
    if (count < 0) count = 0;
    for (i = 0; i < count; i++) port_unlock_bikes[i] = have[i];
    for (i = -1; i < D_0029C3D4 && count < 63; i++) {
        static PortBike stock = {0x11, 4, 0};
        PortBike *p = i < 0 ? &stock : &D_0026DEA0[i];
        if (i < 0 && !port_online_bikes()) continue;
        for (j = 0; j < count; j++)
            if (port_unlock_bikes[j].x0 == p->x0 && port_unlock_bikes[j].x4 == p->x4 &&
                port_unlock_bikes[j].x8 == p->x8) break;
        if (j == count) port_unlock_bikes[count++] = *p;
    }
    *items = (char *)port_unlock_bikes;
    return count;
}
'''


def add(patches):
    # Riders' career bonus (+0x12180, 16 bits; the cheat sets all): func_00193FC8, read by the
    # rider select (func_001F0160's unit, 001EB1D8.cpp) and the rider list in func_001F9118.
    _sub(patches, 'unsorted/00193FC8.c', [(
        'int func_00193FC8(struct Obj *obj, int bit)\n{\n    return ((1 << bit) & obj->x12180) != 0;',
        DECL + 'int func_00193FC8(struct Obj *obj, int bit)\n{\n'
        '    return ((1 << bit) & (obj->x12180 | (port_unlock_all() ? 0xFFFF : 0))) != 0;')])
    # Pro riders (+0x12110, func_00193FE8; read by func_001BB670's rider-select statues and
    # func_0018B3A8's list). Not in the retail cheat, but online's "everything unlocked"
    # (online_menu.c) sets the same 0x3FFF: without it the pros stay locked on the rider screen.
    _sub(patches, 'unsorted/00193FE8.c', [(
        'int func_00193FE8(struct Obj *obj, int bit)\n{\n    return ((1 << bit) & obj->x12110) != 0;',
        DECL + 'int func_00193FE8(struct Obj *obj, int bit)\n{\n'
        '    return ((1 << bit) & (obj->x12110 | (port_unlock_all() ? 0x3FFF : 0))) != 0;')])

    # Sponsors / bike catalogue rows (+0x12188, 18 bits; career rewards func_0014F868): read
    # directly by the bike menus.
    _head(patches, 'unsorted/00139BF8.c',
          '/* UNIT 0x00139BF8 0x0013B5E8 .sdata=0x29C468:0x29C46C .sbss=0x29DD00:0x29DD10 */')
    _sub(patches, 'unsorted/00139BF8.c', [
        ('func_00218498(obj, (D_004325A0.x2188 & bit) != 0);',
         'func_00218498(obj, ((D_004325A0.x2188 | (port_unlock_all() ? 0x3FFFF : 0)) & bit) != 0);'),
        ('            if (D_004325A0.x2188 & bit) {',
         '            if ((D_004325A0.x2188 | (port_unlock_all() ? 0x3FFFF : 0)) & bit) {')])
    _head(patches, 'unsorted/0018B3A8.cpp',
          '/* UNIT 0x0018B3A8 0x0018CF38 .sbss=0x29DE38:0x29DE60 .sdata=0x29CE30:0x29CE68 */', DECL_CPP)
    _sub(patches, 'unsorted/0018B3A8.cpp', [
        ('if (D_004325A0__c9.x2188 & bit) {',
         'if ((D_004325A0__c9.x2188 | (port_unlock_all() ? 0x3FFFF : 0)) & bit) {')])

    # Bike garage list (above).
    _sub(patches, 'unsorted/001F9B98.c', [
        ('char *func_001F9B98(struct Obj *arg0) {',
         DECL + ONLINE + _LIST + 'char *func_001F9B98(struct Obj *arg0) {'),
        ('    return arg0->x0;\n}',
         '    if (port_unlock_all() || port_online_bikes()) {\n        char *items;\n'
         '        port_unlock_list(arg0->x0, &items);\n        return items;\n    }\n'
         '    return arg0->x0;\n}')])
    _sub(patches, 'unsorted/001F9BF0.c', [
        ('unsigned int func_001F9BF0(struct Ref *ref) {',
         DECL + ONLINE + 'extern int port_unlock_list(char *real, char **items);\n'
         'unsigned int func_001F9BF0(struct Ref *ref) {'),
        ('    return ref->x0->x4A4;\n}',
         '    if (port_unlock_all() || port_online_bikes()) {\n        char *items;\n'
         '        return port_unlock_list((char *)ref->x0, &items);\n    }\n'
         '    return ref->x0->x4A4;\n}')])

    # The 23 shop flags (+0x12190..+0x121A6; the cheat sets every one): extras, cheats and
    # options bought in the shop (func_0013F730) or won. Each reader also answers 1.
    _head(patches, 'unsorted/0013F600.c', '/* UNIT 0x0013F600 0x0013F6F8 */')
    _sub(patches, 'unsorted/0013F600.c', [   # the shop's rows: shown bought (price 0, no purchase)
        ('set = D_004225A0.x12190[index] != 0;',
         'set = D_004225A0.x12190[index] != 0 || port_unlock_all();')])
    _head(patches, 'unsorted/0015E238.c', '/* UNIT 0x0015E238 0x0015E448 */')
    _sub(patches, 'unsorted/0015E238.c', [
        ('if(D_004225A0.x12199)', 'if(D_004225A0.x12199 || port_unlock_all())'),
        ('if(D_004225A0.x1219F && state', 'if((D_004225A0.x1219F || port_unlock_all()) && state')])
    _head(patches, 'unsorted/0015F0C8.c', '/* UNIT 0x0015F0C8 0x0015F410 */')
    _sub(patches, 'unsorted/0015F0C8.c', [
        ('if (D_004225A0.x121A0 != 0) {', 'if (D_004225A0.x121A0 != 0 || port_unlock_all()) {'),
        ('if (D_004225A0.x1219A != 0) {', 'if (D_004225A0.x1219A != 0 || port_unlock_all()) {')])
    _head(patches, 'unsorted/00176568.c', '/* UNIT 0x00176568 0x00176930 */')
    _sub(patches, 'unsorted/00176568.c', [
        ('obj->x135F = D_004225A0.x1219C;', 'obj->x135F = D_004225A0.x1219C || port_unlock_all();'),
        ('obj->x1360 = D_004225A0.x1219E;', 'obj->x1360 = D_004225A0.x1219E || port_unlock_all();')])
    _head(patches, 'unsorted/00193B20.c', '/* UNIT 0x00193B20 0x00193BB8 */')
    _sub(patches, 'unsorted/00193B20.c', [   # loading a save: options not owned are switched off
        ('if(!o[0x1219a])', 'if(!o[0x1219a]&&!port_unlock_all())'),
        ('if(!o[0x121a0])', 'if(!o[0x121a0]&&!port_unlock_all())'),
        ('if(!o[0x121a6])', 'if(!o[0x121a6]&&!port_unlock_all())'),
        ('if(!o[0x12198])', 'if(!o[0x12198]&&!port_unlock_all())')])
    _head(patches, 'unsorted/0019B780.c', '/* UNIT 0x0019B780 0x0019B8E8 */')
    _sub(patches, 'unsorted/0019B780.c', [
        ('func_0019BDA0(D_0043DCC0, %d, D_004225A0.x%X);' % (n, f),
         'func_0019BDA0(D_0043DCC0, %d, D_004225A0.x%X || port_unlock_all());' % (n, f))
        for n, f in ((25, 0x12195), (30, 0x12196), (35, 0x12197))])
    _head(patches, 'unsorted/001DDE70.c', '/* UNIT 0x001DDE70 0x001DE3E0 .rodata=0x2935C0:0x293660 */')
    _sub(patches, 'unsorted/001DDE70.c', [
        ('func_0019BDA0(D_0043DCC0, 0x%X, D_004225A0[0x%X]);' % (n, f),
         'func_0019BDA0(D_0043DCC0, 0x%X, D_004225A0[0x%X] || port_unlock_all());' % (n, f))
        for n, f in ((0x19, 0x12195), (0x1E, 0x12196), (0x23, 0x12197))])
    _head(patches, 'unsorted/001F9118.c', '/* UNIT 0x001F9118 0x001F94D0 .sbss=0x29E138:0x29E148 */')
    _sub(patches, 'unsorted/001F9118.c', [
        ('D_004225A0[0x%X] != 0);' % f, '(D_004225A0[0x%X] != 0 || port_unlock_all()));' % f)
        for f in (0x12192, 0x12190, 0x12191, 0x12193, 0x12194)])
    # The front end's menus (001EB1D8.cpp): rows a flag doesn't own are disabled.
    _head(patches, 'unsorted/001EB1D8.cpp',
          '/* UNIT 0x001EB1D8 0x001F8200 .rodata=0x2969E0:0x296E10 .sbss=0x29DFFC:0x29E000 '
          '.sdata=0x29D7E0:0x29D938 */', DECL_CPP)
    _sub(patches, 'unsorted/001EB1D8.cpp', [(a, a.replace(' == 0)', ' == 0 && !port_unlock_all())'))
        for a in ('if (D_004225A0__c5.x1219b == 0)', 'if (D_004225A0__c5.x1219e == 0)',
                  'if (D_004225A0__c6.x1219C == 0)', 'if (D_004225A0__c6.x1219B == 0)',
                  'if (D_004225A0__c6.x1219E == 0)', 'if (D_004225A0__c6.x1219D == 0)',
                  'if (D_004225A0__cEE2A0[0x1219D] == 0)', 'if (D_004225A0__c7.x1219D == 0)',
                  'if (D_004225A0__c9.x1219d == 0)', 'if (D_004225A0_M.x12198 == 0)',
                  'if (D_004225A0_M.x1219F == 0)')])
    _sub(patches, 'unsorted/001EB1D8.cpp', [
        ('        if (world__n27.x1219D) {', '        if (world__n27.x1219D || port_unlock_all()) {'),
        ('        if(world__n27.x1219D) {', '        if(world__n27.x1219D || port_unlock_all()) {')])

    # Turned on or off while the game runs (the settings page, pc_settings.c commit; finding 200).
    # The lobby's rider statues (bonus and pro riders) are the one thing the front end keeps:
    # func_001BB670 copies their unlock bits (func_00193FE8) into D_004B25A0 +0x3C, which the rider
    # select reads, and only at the front end's start, a memory card load and the retail cheat. So
    # the change is followed as those do it, as OPTIONS' LOAD GAME DATA does right after a load
    # (func_001F0160 case 6): the bits again, the statues' "new" flags (none new: the bits were
    # just copied), the statues' animations. None of the cheat's writes (func_001F6618 and the
    # profile's fields); nothing runs unless the setting changes.
    _sub(patches, 'unsorted/001EB1D8.cpp', [(
        'extern void func_001F60C8_M(int) asm("func_001F60C8");\n',
        'extern void func_001F60C8_M(int) asm("func_001F60C8");\n'
        'extern "C" void port_unlock_refresh(void) {\n'
        '    func_001BB670_M(&D_004B25A0_M);\n'
        '    func_001F66B0_M();\n'
        '    func_001F7DE0_M();\n'
        '}\n')])
