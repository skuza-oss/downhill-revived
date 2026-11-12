"""Stack locals that the EE frame makes one object (finding 213; the class of findings 094 and 207).

The retail code sometimes writes or reads past a stack local into the bytes the EE frame puts
after it: the padding up to the next 16-byte boundary, or the next local. In the port clang lays
locals out as it likes (address-taken scalars 4-aligned, aggregates 16-aligned), so the access
lands on something else. port/tools/stack_escapes.py finds them from the matching compiler's
frames; each patch here declares the object the EE frame really holds. The EE frames are those of
the matching build (stabs offsets from $sp); src/ keeps its retail form.
"""


def add(PATCHES):
    def patch(unit, *edits):
        PATCHES[unit] = PATCHES.get(unit, []) + [('text', old, new) for old, new in edits]

    # func_00221888 (a matrix times a vector) stores its result with sqc2: 16 bytes at the
    # quadword the address names (the low 4 bits ignored, as on the EE). EE frame of func_00155BA8:
    # normalized@0x0 (12), matrix@0x10 (64), angle@0x50 (4) and 12 bytes of padding up to 0x60, so
    # the quadword is angle and its padding, and angle gets the x lane. In the port `angle` is a
    # 4-aligned float: the store fills the 16-byte block around it, and angle may get another lane
    # while three neighbouring words are overwritten. The quadword it is:
    patch('unsorted/00155BA8.c',
          (' float normalized[3]; Transform matrix; Transform *transform; float angle;',
           ' float normalized[3]; Transform matrix; Transform *transform; float angle[4] __attribute__((aligned(16)));'),
          ('func_00221888((Vec *)matrix.rows,normalized,&angle);',
           'func_00221888((Vec *)matrix.rows,normalized,(Vec *)angle);'),
          ('if(limit<angle) func_00155E78', 'if(limit<angle[0]) func_00155E78'),
          ('else if(angle < -limit) func_00155E78', 'else if(angle[0] < -limit) func_00155E78'))

    # The same store through func_0017E670 (it passes its float * on to func_00221888). EE frame of
    # func_0017E440: value@0x0 (4), padding to 0x10.
    patch('unsorted/0017E440.c',
          (' float value; struct Flags *flags;',
           ' float value[4] __attribute__((aligned(16))); struct Flags *flags;'),
          ('if(func_0017E670(obj,&value)) {', 'if(func_0017E670(obj,value)) {'),
          ('if(value>0.0f) obj->x1C4=1;', 'if(value[0]>0.0f) obj->x1C4=1;'))

    # func_00203590 fills an AnimationHandle (include/animation.h: 16 bytes, xC/xE the track index)
    # and func_002038E0 / func_00203C28 read it back. Three callers give it 12 bytes; on the EE the
    # next 4 are frame padding (func_001F8CA8, func_0013C6A8: tmp@0x0 in a 0x40/0x50 frame;
    # func_00150C58: rot@0x0, handle@0x10, padding to 0x20). The handle they are:
    patch('unsorted/001F8CA8.c', ('    int tmp[3];', '    int tmp[4];'))
    patch('unsorted/0013C6A8.cpp', (' int tmp[3];', ' int tmp[4];'))
    patch('unsorted/001508D8.cpp',
          ('typedef struct Handle12 {\n    int x0;\n    int x4;\n    int x8;\n} Handle12;',
           'typedef struct Handle12 {\n    int x0;\n    int x4;\n    int x8;\n    int xC;\n} Handle12;'))

    # func_00221AB8 multiplies a whole 4x4 matrix (four quadwords from its second argument) by the
    # transpose of the third. func_001AE358 passes &c: on the EE c@0x20, d@0x30, e@0x40 and
    # unused@0x50 are that matrix's rows (unused is the fourth, never written: stack contents on the
    # EE too). In the port the 48 bytes after c were other locals or nothing, so the three angles
    # it eases into out->xA0..xA8 came from a garbage matrix. The rows, in order:
    patch('unsorted/001AE358.cpp',
          (' Vec a,b,c,d,e; char unused[16]; Vec matrix[4];',
           ' Vec a,b; Vec rows[4]; Vec &c=rows[0],&d=rows[1],&e=rows[2]; Vec matrix[4];'))
