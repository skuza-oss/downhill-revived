"""The dead-patch guard (port_source.py, finding 165): a port patch whose text lies in source the
port does not compile (an #ifdef NONMATCHING block, whose function the core runs from recompiled
asm, or a #if defined(__mips__) branch) is refused, and no patch of port/patches*.py is one.
usage: python port/tools/test_patch_guard.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_source  # noqa: E402  (puts port/ on the path)
import patches  # noqa: E402
from port_source import DeadPatch, apply_patches, dead_patches, dead_ranges  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

UNIT = '''/* UNIT 0x00100000 0x00100100 */
#include "include_asm.h"

/* #ifdef NONMATCHING in a comment is not a directive */
int func_00100000(int a)
{
    return a + 1;
}

#ifdef NONMATCHING
typedef struct Obj { int x0; } Obj;
int func_00100010(Obj *obj)
{
    return obj->x0 * 16.66667f;
}
#else
INCLUDE_ASM("asm/nonmatchings/text", func_00100010);
#endif

#if defined(__mips__)
#define CALL func_00100000
#  if SOMETHING
int mips_only;
#  endif
#elif FEATURE
#define CALL other
#else
#define CALL port_call
#endif

#if !defined(__mips__)
int func_00100080(int a) { return a * 16.66667f; }
#else
int func_00100080(int a) { return 0; }
#endif
'''


def guarded(rel, text, entries):
    saved = patches.PATCHES.get(rel)
    patches.PATCHES[rel] = entries
    try:
        return apply_patches(rel, text)
    finally:
        if saved is None:
            del patches.PATCHES[rel]
        else:
            patches.PATCHES[rel] = saved


def refused(rel, text, entries, *words):
    try:
        guarded(rel, text, entries)
    except DeadPatch as e:
        for w in words:
            assert w in str(e), (w, str(e))
        return
    raise AssertionError('not refused: %r' % (entries,))


def main():
    # the dropped spans: the NONMATCHING C, the __mips__ branch (its nested #if inside), the
    # #else after !defined(__mips__); not the comment, not the unknown #elif FEATURE
    spans = [(UNIT[a:b], why, name) for a, b, why, name in dead_ranges(UNIT)]
    assert len(spans) == 3, spans
    assert spans[0][0].startswith('typedef struct Obj') and spans[0][1] == '#ifdef NONMATCHING'
    assert spans[0][2] == 'func_00100010'
    assert spans[1][0].startswith('#define CALL func_00100000') and 'mips_only' in spans[1][0]
    assert spans[1][1] == '#if defined(__mips__)' and spans[1][2] is None
    assert spans[2][0] == 'int func_00100080(int a) { return 0; }\n', spans[2]
    assert spans[2][1] == '#else (after #if !defined(__mips__))'

    rel = 'unsorted/00100000.c'
    # live code: the matching function, the #else of NONMATCHING, an unknown branch, !__mips__
    out = guarded(rel, UNIT, [('text', 'return a + 1;', 'return a + 2;'),
                              ('text', '#define CALL other', '#define CALL other2'),
                              ('text', '#define CALL port_call', '#define CALL port_call2'),
                              ('body', 'func_00100000', '{ return 0; }')])
    assert 'port_call2' in out and 'other2' in out and '{ return 0; }' in out
    # a text patch in the NONMATCHING C names the patch, the unit, the function and the reason
    refused(rel, UNIT, [('text', 'return a + 1;', 'return a;'), ('text', 'int func_00100010(Obj *obj)\n{',
                                                                 'int func_00100010(Obj *obj)\n{ hook();')],
            'dead port patch 1 of unsorted/00100000.c', 'func_00100010', '#ifdef NONMATCHING', 'recompiled asm')
    # a struct the NONMATCHING C declares (before the function head): still that block's function
    refused(rel, UNIT, [('text', 'typedef struct Obj { int x0; }', 'typedef struct Obj { int x1; }')],
            'func_00100010')
    # a regex whose count includes a dead match (the 16.66667f of patches_fps.py before finding 165)
    refused(rel, UNIT, [('re', (r'\* 16\.66667f', 2), '* scale')], "re '\\\\* 16", 'func_00100010')
    # a body patch on a function only the dropped branch defines
    unit_mips = UNIT.replace('#else\nint func_00100080', '#else\nint func_00100090')
    refused(rel, unit_mips, [('body', 'func_00100090', '{ return 1; }')], 'func_00100090',
            '#else (after #if !defined(__mips__))', 'does not compile')
    # nested inside a dropped branch
    refused(rel, UNIT, [('text', 'int mips_only;', 'int x;')], '#if defined(__mips__)')
    # the survey lists instead of raising
    found = []
    apply_patches_list = port_source.apply_patches
    saved = patches.PATCHES.get(rel)
    patches.PATCHES[rel] = [('text', 'int mips_only;', 'int x;'), ('text', 'return a + 1;', 'return a;')]
    try:
        out = apply_patches_list(rel, UNIT, found)
    finally:
        if saved is None:
            del patches.PATCHES[rel]
    assert len(found) == 1 and 'return a;' in out, found

    # every real patch: none in source the port does not compile
    dead = dead_patches(os.path.join(ROOT, 'src'))
    for message in dead:
        print(message)
    assert not dead, '%d dead port patch(es)' % len(dead)
    print('test_patch_guard: all passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
