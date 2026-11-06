"""Source patches for the PC build, applied to a unit before port/tools/transform.py. Only for
asm the runtime (port/runtime/ps2asm.c) can't run: hand-written libvu0 routines that take their
arguments straight from $a0..$a2, and asm that reads a local through $sp (which only works with
the PS2 stack layout); data a macro defines, which the PS2 data image holds instead; fixed PS2
RAM addresses written as numbers (PS2_RAM, port/include/port.h). Each entry is checked: the
patched text must be found exactly once.

  ('text', old, new): replace old with new
  ('body', name, body): replace the body { ... } of function name's definition
  ('re', (pattern, count), replacement): re.sub, which must replace exactly count times
"""

# Rows are 16-byte vectors; out[i][j] = sum over k of in[i][k] * rot[j][k] (rot transposed), in
# the VU's order: ((c0*x + c1*y) + c2*z) + c3*w. Inputs are read before any output is written,
# as the asm does, so out may alias in.
MUL_TRANSPOSED_4 = r'''{
    const float (*a)[4] = (const float (*)[4])in, (*m)[4] = (const float (*)[4])rot;
    float r[4][4];
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            r[i][j] = ((m[j][0] * a[i][0] + m[j][1] * a[i][1]) + m[j][2] * a[i][2]) + m[j][3] * a[i][3];
    __builtin_memcpy(out, r, sizeof r);
}'''

# Rows 0-2: xyz = rot's 3x3 transposed times the row's xyz, w kept; row 3 copied.
MUL_TRANSPOSED_3 = r'''{
    const float (*a)[4] = (const float (*)[4])in, (*m)[4] = (const float (*)[4])rot;
    float r[4][4];
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++)
            r[i][j] = (m[j][0] * a[i][0] + m[j][1] * a[i][1]) + m[j][2] * a[i][2];
        r[i][3] = a[i][3];
    }
    for (j = 0; j < 4; j++)
        r[3][j] = a[3][j];
    __builtin_memcpy(out, r, sizeof r);
}'''

TRANSPOSE_4 = r'''{
    const float (*a)[4] = (const float (*)[4])in;
    float r[4][4];
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            r[i][j] = a[j][i];
    __builtin_memcpy(out, r, sizeof r);
}'''

COPY_4 = r'''{
    __builtin_memmove(out, in, 64);
}'''

VA_START_EE = '#define va_start(ap,last) (ap = (char *)__builtin_next_arg(last) - (8 - __builtin_args_info(2)) * 8)'
VA_START = '#define va_start(ap, last) __builtin_va_start(ap, last)'

STR8_NAMES = ['str_ALP2', 'str_ALPX2', 'str_ALPT2', 'str_AUBTDH2', 'str_BC2', 'str_BCTDX2', 'str_CITY2',
              'str_FUJI2', 'str_GLAC2P', 'str_GLACX2P', 'str_GLACT2P', 'str_JUNGLE2', 'str_MOAB2', 'str_MOABMX2',
              'str_PERU2', 'str_PERUMX2', 'str_MOSH2', 'str_fmt_s']
STR8_DECLS = ''.join('extern char %s[] __asm__("D_%08X"); ' % (n, 0x29D4A0 + 8 * i) for i, n in enumerate(STR8_NAMES)) \
    + 'extern int D_0029D530, D_0029D538;'

# The game's debug prints, empty in the retail build: on the PC they go to the log with DHD_LOG
# (port/hle/kernel.c hle_debug_print).
DEBUG_PRINT = r'''{
    extern void hle_debug_print(const char *format, __builtin_va_list args);
    __builtin_va_list args;
    __builtin_va_start(args, %s);
    hle_debug_print((const char *)%s, args);
    __builtin_va_end(args);
}'''

PATCHES = {
    # Retail consumes retained stack bytes on inactive/early-return trail paths.
    # Define the native producer's neutral input; this is not retail stack parity.
    # The gravity force is copied as a full quadword, although only xyz were
    # assigned. Define its unused w lane instead of exporting host stack data.
    'unsorted/00136038.c': [
        ('text', ' force[0]=0.0f; force[1]=0.0f; force[2]=-strength;',
         ' force[0]=0.0f; force[1]=0.0f; force[2]=-strength; force[3]=0.0f;')],
    'unsorted/001306C0.c': [
        ('text', '    active = (func_00138560(part) ^ 1) & 0xFF;',
         '    __builtin_memset(&out, 0, sizeof out);\n    active = (func_00138560(part) ^ 1) & 0xFF;')],
    # Named-play RPC sends all 48 bytes, including the unused x24, union high
    # bytes and trailing padding. Clear actual object storage before fields.
    'unsorted/00201B68.c': [
        ('text', '    p.flags = 0;',
         '    __builtin_memset(&p, 0, sizeof p);\n    p.flags = 0;')],
    'unsorted/00201BD0.c': [
        ('text', ' request.flags = 0;',
         ' __builtin_memset(&request, 0, sizeof request);\n request.flags = 0;')],
    # NPC target-distance sentinels also pass through the speed adjustment.
    'unsorted/00125178.cpp': [
        ('text', 'distance += distance*(1.0f-Min(1.0f,func_001385A8((char *)obj->x144+0x40C0)));',
         'distance = ps2_fadd(distance, ps2_fmul(distance, ps2_fsub(1.0f, Min(1.0f, func_001385A8((char *)obj->x144+0x40C0)))));')],
    # The proximity query writes max finite when there is no nearby rider.
    'unsorted/0011EDA8.c': [
        ('text', 'obj->x250 = obj->x250 * obj->x250;',
         'obj->x250 = ps2_fmul(obj->x250, obj->x250);')],
    # Bounding sentinels must remain finite while clipping a segment to slabs.
    'unsorted/002145F8.c': [
        ('text', 'float delta=*end-origin;', 'float delta=ps2_fsub(*end, origin);'),
        ('text', 'float inv=1.0f/delta;', 'float inv=ps2_fdiv(1.0f, delta);'),
        ('text', 'float a=(*lo-origin-radius)*inv;',
         'float a=ps2_fmul(ps2_fsub(ps2_fsub(*lo, origin), radius), inv);'),
        ('text', 'float b=(*hi-origin+radius)*inv;',
         'float b=ps2_fmul(ps2_fadd(ps2_fsub(*hi, origin), radius), inv);'),
        ('text', 'origin<*lo-radius || *hi+radius<origin',
         'origin<ps2_fsub(*lo, radius) || ps2_fadd(*hi, radius)<origin')],
    # The AI's next racing-line record when it has one: the matching C falls off the end, where the
    # EE returns $v0, the x74 it just loaded (asm func_0010B8E8); clang returned the controller.
    'unsorted/0010B8E8.c': [
        ('text', '    if (a->x74 == 0) return func_00117828(D_0029EA20, a->x0->x792c + 0x10);\n}',
         '    if (a->x74 == 0) return func_00117828(D_0029EA20, a->x0->x792c + 0x10);\n    return (void *)a->x74;\n}')],
    # Flat terrain can have a zero-height, zero-length interpolation segment.
    'unsorted/00111F98.c': [
        ('text', 'height=obj->x70->z-obj->x0->x792C->x18;',
         'height=ps2_fsub(obj->x70->z, obj->x0->x792C->x18);'),
        ('re', (r'\(int\)\(\(float\)difference\*obj->x2B8\)', 2),
         'ps2_cvt_w_s(ps2_fmul((float)difference, obj->x2B8))'),
        ('text', 'denom=maxf(height,obj->x70->z-obj->x74->z);',
         'denom=maxf(height,ps2_fsub(obj->x70->z, obj->x74->z));'),
        ('text', '(int)((float)step*height/denom)',
         'ps2_cvt_w_s(ps2_fdiv(ps2_fmul((float)step, height), denom))')],
    # Instant float tweens divide by a zero duration on the EE too.
    'unsorted/00208708.cpp': [
        ('text', '(float)(func_00202DA8() - obj->startTick) / (float)obj->duration',
         'ps2_fdiv((float)(func_00202DA8() - obj->startTick), (float)obj->duration)'),
        ('text', 'value * obj->delta + obj->start',
         'ps2_fadd(ps2_fmul(value, obj->delta), obj->start)')],
    # A stopped rider has zero angular speed during countdown.
    'unsorted/00131998.cpp': [
        ('text', '(float)func_00238F30() * 1.5707963705062866f / speed',
         'ps2_fdiv((float)func_00238F30() * 1.5707963705062866f, speed)')],
    # Loading/completion progress can be queried before its total is set.
    'unsorted/001F8920.c': [
        ('text', '(float)race->x500 / (float)race->x504',
         'ps2_fdiv((float)race->x500, (float)race->x504)')],
    # Empty animation tracks have zero duration. Keep the EE's finite divide,
    # multiply/add and integer-conversion semantics throughout the blend.
    'unsorted/001E75D8.c': [('body', 'Update', r'''{
    struct Handle h;
    float t;
    h = *GetHandle(state, e->x4);
    func_00203E10(&h, one);
    h = *GetHandle(state, e->xC);
    func_00203E10(&h, one);
    h = *GetHandle(state, e->x4);
    t = ps2_fmul(func_00204280(&h), ps2_fdiv(one, e->x14));
    h = *GetHandle(state, e->xC);
    t = ps2_fadd(t, ps2_fmul(ps2_fmul(func_00204280(&h),
        ps2_fdiv(one, e->x1C)), e->x48));
    t = ps2_fmul(t, ps2_fdiv(one, func_001E5198(control)));
    e->x4C = ps2_fmul((float)ps2_cvt_w_s(t), func_001E5190());
    h = *GetHandle(state, e->xC);
    e->x50 = ps2_fmul(ps2_fmul(ps2_fmul(ps2_fmul(func_00204280(&h),
        ps2_fdiv(one, e->x1C)), e->x48),
        ps2_fdiv(one, func_001E5198(control))), func_001E5190());
}''')],
    # The live bounding-grid sentinel can exceed int32. EE CVT.W.S saturates
    # before the grid clamps to 0..1023; SSE's indefinite integer would pick 0.
    'unsorted/00117A10.c': [
        ('text', '(int)((value - values->x4 + values->x8 + 0.1f) / values->x8)',
         'ps2_cvt_w_s((value - values->x4 + values->x8 + 0.1f) / values->x8)')],
    # %2.0f needs a double. The source's false int return for the SN
    # float-to-double packer and EE varargs wrapper lose its upper word.
    'unsorted/00177DA0.c': [
        ('text', ' func_00254D68(buf,D_0029C9D0,func_001FD300(value));',
         ' { extern int host_sprintf(char *, const char *, ...);\n'
         '   host_sprintf(buf,D_0029C9D0,(double)value); }')],
    # Retail consumes a0/f12/f13 and ignores a1, which the callers still pass.
    # Keep that unused stack slot on x86 so target/duration do not shift.
    'unsorted/001AD580.c': [
        ('text', 'void func_001AD580(Obj *obj,float target,float duration)',
         'void func_001AD580(Obj *obj,int unused_mode,float target,float duration)')],
    # The vtable getter returns a pointer-sized value in v0. Only its zero
    # store is quadword-sized; a native 128-bit return adds a hidden pointer.
    'unsorted/0018EB50.c': [
        ('text', 'extern u128 func_0018EAC8', 'extern int func_0018EAC8'),
        ('text', 'u128 func_0018EB50', 'int func_0018EB50'),
        ('text', 'u128 result =', 'int result =')],
    # The lighting query writes the color through the unchanged incoming a1.
    'unsorted/002211F0.c': [
        ('text', 'extern int func_00220DA0(void *);',
         'extern int func_00220DA0(void *, Color *);'),
        ('text', 'func_00220DA0(obj + 0x10)', 'func_00220DA0(obj + 0x10, c)')],
    # The particle spawners copy the position into temp with a quadword store and pass it to
    # func_0022C350, whose vector loads need the EE's 16-byte alignment (movaps on the PC).
    'unsorted/00228BE8.c': [
        # Zero-duration particle records are valid EE inputs. Preserve finite
        # reciprocals/slopes with the same divider as recompiled instructions.
        ('text', '1.0f / (float)(obj->entry.x6 + ((short)obj->scale.x1A >> 1))',
         'ps2_fdiv(1.0f, (float)(obj->entry.x6 + ((short)obj->scale.x1A >> 1)))'),
        ('text', '(obj->scale.x28 - obj->scale.x2C) / (float)(short)obj->scale.x1A',
         'ps2_fdiv(obj->scale.x28 - obj->scale.x2C, (float)(short)obj->scale.x1A)'),
        ('text', '(obj->big.x28 - obj->big.x24) / obj->big.x2C',
         'ps2_fdiv(obj->big.x28 - obj->big.x24, obj->big.x2C)'),
        ('text', '-obj->big.x28 / obj->big.x30',
         'ps2_fdiv(-obj->big.x28, obj->big.x30)'),
        ('text', '1.0f / (o->x2726 + (o->x27DA >> 1))',
         'ps2_fdiv(1.0f, (float)(o->x2726 + (o->x27DA >> 1)))'),
        ('text', ' float temp[4];\n u128__c10 copy;',
         ' float temp[4] __attribute__((aligned(16)));\n u128__c10 copy;'),
        ('text', ' float temp[4]; int index; Particle__cfb8 *p;',
         ' float temp[4] __attribute__((aligned(16))); int index; Particle__cfb8 *p;')],
    # func_00242D08 returns the two plane distances with one quadword store (sqc2): give the
    # local the whole aligned quadword the EE frame provided.
    # Retail writes high through 16(&low); on PC the locals are not adjacent.
    'unsorted/00214BA0.c': [
        ('text', 'vmax.xyz $vf11,$vf11,$vf12\\n\\tsqc2 $vf11,16(%0)',
         'vmax.xyz $vf11,$vf11,$vf12\\n\\tsqc2 $vf11,0(%1)')],
    # Retail reaches ends.after[i] as bp[2], past the end of ends.before. clang splits the struct
    # into two stack slots, so bp[2] landed on groups[0] and overwrote its record pointers with the
    # probe end point: the query read through float bits and faulted once x went negative.
    'unsorted/00131630.cpp': [
        ('text', '        ADD(bp[2], bp[0], scaled);', '        ADD(ends.after[i], bp[0], scaled);')],
    # Retail builds the load task at sp+0x12F30 through archive, which is w.pair.load in its frame.
    # The PC frame is laid out differently, so the task landed outside archive on other locals.
    # Retail copies all 64 bytes of buffer, past the string's end, into the rider's name field.
    # On the PC those trailing bytes are host stack leftovers, which change with any host code
    # change and broke dump comparisons; zeroed, they are the same in every build.
    'unsorted/00230DF8.c': [
        ('text', ' signed char buffer[64]; int i, count;', ' signed char buffer[64] = {0}; int i, count;')],
    'unsorted/00187438.c': [
        ('text', '            load = (LoadTask *)&archive[0x12F30];', '            load = &w.pair.load;')],
    'unsorted/00242998.cpp': [
        ('text', '    float distance[2]; Vector point;',
         '    float distance[4] __attribute__((aligned(16))); Vector point;')],
    # Retail passes sp to a quadword-copy helper; the EE stack is 16-byte aligned.
    'unsorted/00167A00.c': [
        ('text', '    float work[12];',
         '    float work[12] __attribute__((aligned(16)));')],
    # The PS2 ABI passes these aggregate values indirectly. Preserve the local
    # copy while accepting the pointer used by the rest of the engine.
    'unsorted/001304F8.c': [
        ('text', 'Vec4 a, Vec4 b, Vec4 c, Vec4 *dir)\n{',
         'const Vec4 *input_a, const Vec4 *input_b, const Vec4 *input_c, Vec4 *dir)\n{\n'
         '    Vec4 a = *input_a, b = *input_b, c = *input_c;')],
    'unsorted/00202E00.cpp': [
        ('re', (r'(\bfunc_\w+\()((?:struct )?(?:Handle|Arg)\w*)(\s+\w+)?(?=\s*[,\)])', 7),
               r'\1const \2 &\3')],
    # Loading-screen geometry uses a scratch buffer below the mapped low view.
    'unsorted/001829C0.c': [('text', 'func_0020D250(D_004D0380,0x100000);',
                            'func_0020D250(D_004D0380,PS2_RAM(0x100000));')],
    # Explicit test bypass until MPEG/IPU playback is implemented on the PC.
    # Audited separately from register-bank permutations: the resource ID is
    # 32 bits in the definition. A 64-bit x86 stack slot shifted both indices.
    'unsorted/001EB1D8.cpp': [('text', 'int func_001EBFD8(int arg0)\n{',
                             'int func_001EBFD8(int arg0)\n{\n    if (host_skip_movies()) return 1;')],
    'unsorted/0020D028.c': [('body', 'func_0020D028', DEBUG_PRINT % ('arg0', 'arg0'))],
    'unsorted/002189F8.c': [('body', 'func_002189F8', DEBUG_PRINT % ('format', 'format'))],
    # the unzip buffer at the top of PS2 RAM (0x1FDF800-0x1FFF7FF), written as numbers
    # the heap above the main thread's stack (0x93CAF0-0xA00000), given as a number
    'unsorted/0020AC80.c': [
                            # Slot-zero loading-screen NGPs link at 1 MB; relocate their
                            # root and PTR pointer lists when using the PC scratch mirror.
                            ('text', '        list = (int *)buffer;',
                             '        list = (int *)buffer;\n'
                             '        if (base == (int)PS2_RAM(0x100000)) {\n'
                             '            unsigned int *root = (unsigned int *)base;\n'
                             '            unsigned int i;\n'
                             '            for (i = 1; i <= root[0]; i++) root[i] += PS2_RAM(0);\n'
                             '            for (i = 1; i <= (unsigned int)list[0]; i++)\n'
                             '                *(unsigned int *)(base + list[i]) += PS2_RAM(0);\n'
                             '        }'),('text', 'func_00223A98(D_004D0350,0x93caf0,0xc3500);',
                             'func_00223A98(D_004D0350,PS2_RAM(0x93caf0),0xc3500);'),
                            # the data files are linked for PS2 address 0xA00000 (D_0029D9D4, a
                            # host address here, port/runtime/memory.c): their pointers move by
                            # (where loaded - that PS2 address), which also adds the RAM's base
                            ('text', 'delta = (int)func_0020D170(D_004D0380, slot) - D_0029D9D4;',
                             'delta = (int)func_0020D170(D_004D0380, slot) - (D_0029D9D4 - (int)PS2_RAM(0));'),
                            ('text', 'delta = (int)base - D_0029D9D4;',
                             'delta = (int)base - (D_0029D9D4 - (int)PS2_RAM(0));')],
    'unsorted/00240270.c': [('re', (r'\b(0x0?1F(?:DF8[0-9A-F]{2}|FF7FF))\b', 7), r'PS2_RAM(\1)'),
                          # huft_build copies all eight bytes of r into guest RAM.
                          # Name its padding and clear the leaf union so every byte
                          # is defined, independent of host stack/optimizer choices.
                          # The decoder still reads the same low 16-bit leaf value.
                          ('text', '    unsigned char b;\n    union {\n        unsigned short n;',
                           '    unsigned char b;\n    unsigned short padding;\n    union {\n        unsigned int bits;\n        unsigned short n;'),
                          ('text', '    Huft r;', '    Huft r = {0};'),
                          ('text', '            r.b = k - w;\n',
                           '            r.v.bits = 0; /* invalid leaves also have defined bytes */\n            r.b = k - w;\n'),
                          # The callers read the inflate block status ($v0 on the PS2):
                          # the block decoder's, or 2 for the invalid type 3 (the delay
                          # slot at 00241F50). wasm's in-TU bitcast shim of the void body
                          # would return its argument instead (test_inflate_status.py).
                          ('text', 'void func_00241E70(int *e)', 'int func_00241E70(int *e)'),
                          ('text', '    if (t == 0) {\n        func_002413E8();\n'
                                   '    } else if (t == 1) {\n        func_00241598();\n'
                                   '    } else if (t == 2) {\n        func_002417E0();\n    }\n}',
                           '    if (t == 0) {\n        return func_002413E8();\n'
                           '    } else if (t == 1) {\n        return func_00241598();\n'
                           '    } else if (t == 2) {\n        return func_002417E0();\n'
                           '    }\n    return 2; /* the retail $v0: the last compare\'s 2 */\n}'),
                          ('re', (r'\(\(int \(\*\)\(int \*\)\)func_00241E70\)\(&done\)', 2),
                           'func_00241E70(&done)')],
    # STR8 defines an 8-byte name string as two words; the PS2 data image has them, so the PC
    # build declares each name at its address instead (.sdata from 0x29D4A0, in order)
    'unsorted/001BA3B8.c': [('text', '#include "include_asm.h"\n', '#include "include_asm.h"\n' + STR8_DECLS),
                            ('text', '    int name = WORD4(a, b, c, d); int name##_hi = WORD4(e, f, g, h)',
                             '    /* in the PS2 data image */')],
    # func_00221888 loads and stores all four lanes of offset, whose w the C never sets: host
    # stack bytes reached the rider (obj->xDB0.w, rider +0x535C) and differed between PCs. The
    # game never reads that lane back (DHD_WATCH, finding 041); zero it so every byte is defined.
    'unsorted/001A4F70.c': [('text', '    Vec__c10 offset;\n', '    Vec__c10 offset = {0};\n')],
    # The race step's rider passes: a puppet rider (port/hle/puppet.c, finding 044) takes its
    # virtual pad and rigid body from snapshots instead of its controller and physics.
    'unsorted/0011A100.c': [
        ('text', 'extern void func_0011A938(Race *);',
         'extern void func_0011A938(Race *);\n'
         'extern int hle_puppet_input(Rider *), hle_puppet_physics(Rider *);\nextern void hle_puppet_field(void);'),
        ('text', '        func_0012C8B0(&race->riders[i]);',
         '        if (!hle_puppet_input(&race->riders[i])) func_0012C8B0(&race->riders[i]);'),
        ('text', '        func_0012D360(&race->riders[i]);',
         '        if (!hle_puppet_physics(&race->riders[i])) func_0012D360(&race->riders[i]);'),
        ('text', '    func_0011A938(race);\n}', '    func_0011A938(race);\n    hle_puppet_field();\n}')],
    # A puppet's crashes and hits come from its own PC (snapshots, events), never from here.
    'unsorted/0012E940.c': [
        ('text', 'void func_0012E940(char *self,int kind,Vec *direction,char *other) {',
         'extern int hle_puppet_hit(char *, int, void *, char *);\n'
         'void func_0012E940(char *self,int kind,Vec *direction,char *other) {'),
        ('text', ' if((unsigned char)(AT(unsigned long,self,0x4098)>>2)&1) return;',
         ' if(hle_puppet_hit(self,kind,direction,other)) return;\n'
         ' if((unsigned char)(AT(unsigned long,self,0x4098)>>2)&1) return;')],
    'unsorted/0012FE38.cpp': [
        ('text', 'void Machine::func_0012FE38(State *next)\n{\n',
         'extern "C" int hle_puppet_switch(char *, void *);\nvoid Machine::func_0012FE38(State *next)\n{\n'
         '    if (hle_puppet_switch((char *)this, next)) return;\n')],
    'unsorted/00155E78.cpp': [
        ('text', 'void func_00155E78(Obj *self,int kind) {',
         'extern "C" int hle_puppet_crash(char *);\nvoid func_00155E78(Obj *self,int kind) {'),
        ('text', ' char *rider=self->x0;\n', ' char *rider=self->x0;\n if(hle_puppet_crash(rider)) return;\n')],
    # The bottle in hand (attack move at rider +0x5F40): its start (func_00153C70) takes a bottle
    # only when the move's test, asked again there, picks the throw; the update then moves it to
    # the hand. A puppet enters the attack state when its snapshot says so, after the press its
    # pad showed is gone: no bottle, and the update wrote through NULL (guest 0x10, finding 121).
    # A rider of this PC always has one there, so the check changes nothing for it.
    'unsorted/00153A30.c': [
        ('text', ' if(D_0029C658 && obj->x2c==-1) {', ' if(D_0029C658 && obj->x2c==-1 && obj->x28) {')],
    # Online (port/hle/online.c, finding 047): each PC's own rider starts where the host has that
    # player; the puppets wear their players' riders and bikes (set after the AI riders are
    # chosen); the race ends on the results of every PC, not 8 s after this PC's rider finished.
    'unsorted/0012C450.cpp': [
        ('text', 'void func_00138B40(void *, void *);\n}',
         'void func_00138B40(void *, void *);\nint hle_puppet_grid(char *, int);\nvoid hle_puppet_start_point(void *, char *);\n}'),
        ('text', '        if (func_001A0498(D_0043EEC0, &start.pos, start.matrix)) {',
         '        hle_puppet_start_point(D_0043EEC0, p);\n        if (func_001A0498(D_0043EEC0, &start.pos, start.matrix)) {'),
        ('text', 'u128 *v = func_001177F8(D_0029EA20, I(0x7978));',
         'u128 *v = func_001177F8(D_0029EA20, hle_puppet_grid(p, I(0x7978)));')],
    # Shared online track pickups; retail effect and respawn code stay intact offline.
    'unsorted/00196590.c': [
        ('text', 'void func_00196590(Obj *obj, Rider *rider) {',
         'extern int hle_online_pickup_allowed(void *);\nextern void hle_online_pickup_taken(void *, void *);\n'
         'void func_00196590(Obj *obj, Rider *rider) {\n    if (!hle_online_pickup_allowed(rider)) return;'),
        ('text', '    obj->x8->x4 = 0;',
         '    obj->x8->x4 = 0;\n    hle_online_pickup_taken(obj, rider);')],
    # Online: no random start swap of the human (port/hle/puppet.c hle_puppet_keep_grid), so every
    # PC has the same grid and no two riders share a gate.
    'unsorted/0011AEC0.c': [
        ('text', 'extern void func_0011B030(World *world, Racer *a, Racer *b);',
         'extern void func_0011B030(World *world, Racer *a, Racer *b);\nextern int hle_puppet_keep_grid(void);'),
        ('text', '        if (i != r) {', '        if (i != r && !hle_puppet_keep_grid()) {')],
    'unsorted/001B7198.cpp': [
        ('text', 'extern void func_001B87F8(void *);',
         'extern void func_001B87F8(void *);\nextern void hle_online_riders(void *);\nextern int hle_online_ai_count(int);'),
        ('text', '    func_001B74E8(game);\n', '    func_001B74E8(game);\n    hle_online_riders(game);\n'),
        # Online: the AI count for the room (computer riders on or off, port/hle/online.c)
        ('text', '    return D_004B25A0.xB174 == 0 ? result : 0;\n', '    return hle_online_ai_count(D_004B25A0.xB174 == 0 ? result : 0);\n')],
    # Online: the other players' names over their riders (port/hle/online.c hle_online_names); a
    # cheat code does nothing in a room with cheats off, and each code goes in the log
    # (hle_cheat_code, finding 117)
    'unsorted/0012C9E0.c': [
        ('text', 'extern void func_0012CF68(void *, int);',
         'extern void func_0012CF68(void *, int);\nextern void hle_online_names(void *, int);\n'
         'extern void hle_cheat_code(void *);\nextern void hle_cheat_code_done(void *);'),
        ('text', '    int saved = obj->x7988;\n', '    int saved = obj->x7988;\n    hle_cheat_code(obj);\n'),
        ('text', '    func_0012CF68(obj, saved);\n',
         '    hle_cheat_code_done(obj);\n    func_0012CF68(obj, saved);\n    hle_online_names(obj, saved);\n')],
    'unsorted/0019F9B0.c': [
        ('text', 'extern void func_001614D8(void *, int);', 'extern void func_001614D8(void *, int);\nextern int hle_online_race_end(int);'),
        ('text', '        } else {\n            func_001B45F8(D_004B1F40, 1, time * 60);\n            if (!D_0043FA34[0]',
         '        } else {\n            if (!hle_online_race_end(time * 60)) func_001B45F8(D_004B1F40, 1, time * 60);\n'
         '            if (!D_0043FA34[0]')],
    'unsorted/002219A8.c': [('body', 'func_002219A8', MUL_TRANSPOSED_4)],
    'unsorted/00221AB8.c': [('body', 'func_00221AB8', MUL_TRANSPOSED_3)],
    'unsorted/00221D58.c': [('body', 'func_00221D58', TRANSPOSE_4)],
    # The sin/cos microprogram's first result goes to the local at sp+0 (first, s); pass it as an
    # operand instead (retail: sq $v0, 0x0($sp), with the second local at sp+0x10).
    'unsorted/00221F50.c': [('text', '"sq $v0, 0x0($sp)"\n        : : "r"(src), "r"(second) : "$2", "memory");',
                             '"sq $v0, 0x0(%2)"\n        : : "r"(src), "r"(second), "r"(first) : "$2", "memory");')],
    'unsorted/00222040.c': [('text', '"sq $v0, 0x0($sp)"\n        : : "r"(rot), "r"(c) : "$2", "memory");',
                             '"sq $v0, 0x0(%2)"\n        : : "r"(rot), "r"(c), "r"(s) : "$2", "memory");')],
    # vf12 is step, the local at sp+0 (retail passes $sp as &step to func_00149EC0).
    'unsorted/00148F60.cpp': [('text', '"lqc2 $vf12, 0x0($sp)\\n\\t"', '"lqc2 $vf12, 0x0(%3)\\n\\t"'),
                              ('text', ': "=m"(obj->x80) : "r"(&obj->x80), "f"(GetScale(obj)) : "$2");',
                               ': "=m"(obj->x80) : "r"(&obj->x80), "f"(GetScale(obj)), "r"(&step) : "$2");')],
    # The EE's va_start (gcc 2.95 MIPS builtins); on i386 va_list is a char * too.
    'unsorted/00234FB0.c': [('text', VA_START_EE, VA_START)],
    'unsorted/002350E8.c': [('text', VA_START_EE, VA_START)],
}

# Retail scheduling exceptions: use the shared native scalar/impulse contracts.
PATCHES['unsorted/00231CC0.c'] = [('text', '/* UNIT 0x00231CC0 0x00231D48 */', '/* UNIT 0x00231CC0 0x00231D48 */\n#include "impulse.h"'), ('text', 'extern void func_00231C88(void *, void *);', ''), ('text', 'extern int func_00221888(void *, void *, void *);', ''), ('text', 'int func_00231CC0(char *obj, void *a, void *b) {', 'void func_00231CC0(ImpulseBody *body, Vector4 *a, Vector4 *b) {\n char *obj=(char *)body;'), ('text', 'func_00231C88(obj,a);', 'func_00231C88(body,a);'), ('text', 'return func_00221888(obj+0x80,obj+0x70,obj+0xd0);', 'func_00221888((Matrix4 *)(obj+0x80),(Vector4 *)(obj+0x70),(Vector4 *)(obj+0xd0));')]
PATCHES['unsorted/00231D88.c'] = [('text', 'extern int func_00231CC0(struct Object *, float *, float *);', '#include "impulse.h"'), ('text', 'int func_00231D88(struct Object *obj, float *a, float *b)', 'void func_00231D88(struct Object *obj, float *a, float *b)'), ('text', 'return func_00231CC0(obj, rotatedA, b);', 'func_00231CC0((ImpulseBody *)obj,(Vector4 *)rotatedA,(Vector4 *)b);')]
PATCHES['unsorted/001AC620.c'] = [('text', 'extern void func_00203380(Vector *, Vector *, float);', '#include "scalar_api.h"'), ('text', 'func_00203380(', 'ScalarBridge00203380_25(')]
PATCHES['unsorted/001AC7B0.c'] = [('text', 'extern void func_00203380(Vector *, Vector *, float);', '#include "scalar_api.h"'), ('text', 'func_00203380(', 'ScalarBridge00203380_25(')]
PATCHES['unsorted/001ACA08.c'] = [('text', 'extern void func_00203380(struct Handle *, void *, float);', '#include "scalar_api.h"'), ('text', 'func_00203380(', 'ScalarBridge00203380_25(')]
# A race's seed (start grid, AI) is the EE time since a timer reset, so it follows load timing;
# PCSX2 comparisons pin it to the retail run's (port/hle/autopad.c hle_race_seed, DHD_RACE_SEED).
PATCHES['unsorted/0016B8C8.cpp'] = [('text', 'int func_00238F10(void);', 'int func_00238F10(void);\nextern "C" int hle_race_seed(int);'),
                                   ('text', 'temp_v0 = func_00238F10();', 'temp_v0 = hle_race_seed(func_00238F10());'),
                                   ('text', 'func_0023D8D8(0, 277.0f, sp4);', 'func_0023D8D8(0, 277.0f * port_fov_x, sp4);')]
# Widescreen (DHD_WIDESCREEN, port/hle/host.c host_widescreen_init): the X field of view of the
# 1P race, replay, bike shop and menu calls, scaled by port_fov_x (1 normally, 3/4 for 16:9) as
# the PCSX2 widescreen patch for this disc changes them (finding 043). The view table and tree
# widths are data, changed at boot.
PATCHES['unsorted/001A8A48.c'] = [('text', 'func_0023D8D8(0, 277.0f, y);', 'func_0023D8D8(0, 277.0f * port_fov_x, y);')]
PATCHES['unsorted/001F84A8.c'] = [('text', 'func_0023D8D8(0, f, b);', 'func_0023D8D8(0, f * port_fov_x, b);')]
# 688 is the title (func_001B6708, recompiled from asm) and shell menus' X field of view.
PATCHES['unsorted/0023D8D8.c'] = [('text', '    view->x0 = dist;', '    if (dist == 688.0f) dist *= port_fov_x;\n    view->x0 = dist;')]
# func_00202760 passes its fourth argument (a3, the callers' result buffer such as &D_0029E168)
# through to func_00247D28, which reads it; the source names only three, which matches retail only
# because a3 is never touched. Natively the pointer was dropped and the memory card code read stack
# garbage (a crash at boot unless DHD_LOG changed the garbage).
PATCHES['unsorted/00202760.c'] = [('text', 'extern int func_00247D28(int, int, int);', 'extern int func_00247D28(int, int, int, void *);'),
                                  ('text', 'int func_00202760(int arg0, int arg1, int arg2)', 'int func_00202760(int arg0, int arg1, int arg2, void *arg3)'),
                                  ('text', 'return func_00247D28(arg0, arg1, arg2);', 'return func_00247D28(arg0, arg1, arg2, arg3);')]
# fabsf without a prototype: gcc 2.95 still emits abs.s (retail), but clang calls the CRT as an
# implicit int function, reads EAX and leaves the x87 result on the stack. The leaked registers
# then fill the x87 stack and later float returns overflow it (finding 038).
PATCHES['unsorted/00174590.c'] = [('text', '} else if (fabsf(d) < bestDist) {', '} else if (__builtin_fabsf(d) < bestDist) {')]
PATCHES['unsorted/00235F38.c'] = [('text', 'if (fabsf(step) < minStep) {', 'if (__builtin_fabsf(step) < minStep) {')]
# The weather effect's packet builder writes the word before p as p[0x3fffffff], which wraps to p - 4
# in 32-bit address arithmetic (EE, i686). wasm32 folds the index into the store's unsigned offset,
# which never wraps: the 64-bit build wrote 4 GiB past p and crashed late in races; write p[-1].
PATCHES['unsorted/00224CC8.c'] = [('text', '((float *)p)[0x3fffffff]=', '((float *)p)[-1]=')]
# Weather objects reuse the particle pool's inline packet storage. Their first
# four position records carry per-view integer batch counts, not particle ages.
# The retail constructor initializes only the following history records, and
# 001E0480 leaves the count untouched at zero density. Old particle floats can
# consequently become thousands of VU1 iterations before the first batch is
# published (finding 124). Start every view empty; a nonzero update writes the
# complete position/count record before it can be drawn.
PATCHES['unsorted/00226D08.c'] = [('text', '    obj->x2830=cursor+0x40;',
    '    obj->x2830=cursor+0x40;\n    __builtin_memset(obj->x2830, 0, 4 * 16);')]
# The career finish text (func_0017A678) sprintfs "%d MORE CREDITS REQUIRED TO ADVANCE" into a 32-byte
# stack buffer when the race did not advance the career (issue #8, finding 094): with two digits that
# is 36 bytes. On the EE the overrun hits dead bytes of the frame above. In the wasm core buf is in the
# topmost shadow frame of the main EE fiber's 256 KB slot (fiber_x64.c), so the overrun wrote into the
# next slot's guard page at 0x06040000 and the 64-bit game crashed on every non-advancing finish.
PATCHES['unsorted/0017A678.cpp'] = PATCHES.get('unsorted/0017A678.cpp', []) + [
    ('text', '    char buf[0x20];', '    char buf[0x40];')]
# Tracks at 3-4 players (public issue #1): the PS2 offered only a fixed five (mask 0x020001C2:
# MOAB, ALPINEMX, MOABMX, JUNGLEMX, BCTDX), whatever the save had unlocked, most likely for its
# frame rate with four views; every track loads the same "2" level files at 2-4 players (CITYMX
# and CITYTDH their 1-player ones, as at 2 players). Here 3-4 players get the save's tracks, as
# 1-2 players do. PC SETTINGS > Unlock All (port_unlock_all, pc_settings.c) adds every track the
# game's unlock cheat gives (all but 21 DUAL and 23 MOSH) without writing the save.
# At 3 players the game draws four views (D_0029E450), but the level registers its objects for
# views 0-2 only (func_001589A8 -> func_0023D0B0, which takes each view's x4 slot from the pool
# func_00208FB8 on first use): the fourth view has no x4. The freeride tracks' race build
# (func_0011C6F0) then stores D_0029E448 through every view's x4 and their field callback writes
# x4 + 0xB8 (func_00195F90): on the EE into low RAM, in the 64-bit build into its null guard
# (issue #12: every freeride track crashed at 3 players). A view without x4 takes its slot here as
# func_0023D0B0 would; views that had one, every case the PS2 ran without the fault, are unchanged.
PATCHES['unsorted/0023D1A0.c'] = [
    ('text', 'extern int D_0029E448;', 'extern int D_0029E448;\nextern int *func_00208FB8(void);'),
    ('text', '    *entry->x4 = D_0029E448;', '    if (!entry->x4) entry->x4 = func_00208FB8();\n    *entry->x4 = D_0029E448;')]
# The same field callbacks (one per track family) pass each view's rider's x793C to func_0021D758;
# the fourth view at 3 players has no rider, so the EE read a word of low RAM and handed it on. The
# call is skipped for a view without a rider.
for unit, rider in (('0011C918.c', 'body->x793C'), ('00128DD8.c', 'body->x793C'), ('00181B88.c', 'rider->x793c'),
                    ('00195F90.c', 'rider->x793c'), ('0016C720.c', 'rider->x793C')):
    owner = rider.split('->')[0]
    PATCHES['unsorted/' + unit] = PATCHES.get('unsorted/' + unit, []) + [
        ('text', f'func_0021D758({rider}, ', f'if ({owner}) func_0021D758({rider}, ')]
# Glacier's callbacks can sample an empty fourth view before its x4 is allocated.
# No rider needs its motion; skip that view before sampling it.
PATCHES['unsorted/0016B400.c'] = [
    ('text', '        pos.q=func_0023CF78(i)->q;', '        if (!rider) continue;\n        pos.q=func_0023CF78(i)->q;')]
PATCHES['unsorted/0016C720.c'] += [
    ('text', '        player++;\n        pos = *func_0023CF78(i);',
     '        player++;\n        if (!rider) continue;\n        pos = *func_0023CF78(i);')]
# BC2's paired trackside controller stays in state 0 unless both actors bind
# (0016CE68). At 3+ players 00192F68 disables the actor pool, but the manager
# still schedules these inactive controllers. Their unbound parts have x0=1
# from 001224F0, so updating them requests a reset through a null owner.
# Leave inactive controllers dormant, as we already do for completed state 14.
PATCHES['unsorted/0016D0E0.c'] = [
    ('text', '    if (o->x0 == 14)', '    if (o->x0 == 0 || o->x0 == 14)')]
PATCHES['unsorted/00193F38.c'] = [
    ('text', 'extern int D_004B2828[];', 'extern int port_unlock_all(void);'),
    ('body', 'func_00193F38', '{\n    return *(int *)(obj + 0x12114) | (port_unlock_all() ? 0x35FFFFF : 0);\n}')]
PATCHES['unsorted/00193F68.c'] = [
    ('text', 'extern int D_004B2828[];\n', ''),
    ('body', 'func_00193F68', '{\n    return ((1 << bit) & func_00193F38(obj)) != 0;\n}')]
# ONLINE (port/hle/online_menu.c): a fourth main menu row (func_001ED5B8) under OPTIONS that opens
# the online pages, run by the game's popup menus as PC SETTINGS is. Its label is the host's (the
# game's table has three). Back from the pages the cursor stays on ONLINE; when a room's race
# starts it goes to SINGLE PLAYER, where online.c's menu script goes on to the race.
PATCHES['unsorted/001EB1D8.cpp'] = PATCHES.get('unsorted/001EB1D8.cpp', []) + [
    ('text', 'void func_001ED5B8__cED5B8(void) {\n    u8__cED5B8 disabled[3];',
     'extern int port_online_menu(void);\n'
     'extern int port_main_label(int *table, int index);\n'
     'static int port_main_row;\n'
     'void func_001ED5B8__cED5B8(void) {\n    u8__cED5B8 disabled[4];'),
    ('text', 'func_001F7420__cED5B8(&D_004C6CA8__cED5B8, 10, 0, 0, 2, 0, disabled, 3);',
     'func_001F7420__cED5B8(&D_004C6CA8__cED5B8, 10, 0, 0, 3, port_main_row, disabled, 4);'),
    ('text', 'func_001F5EE8__cED5B8(0, 0, 0, 0x212, index * 15 + 95, D_0029E054__cED5B8[index], 192);',
     'func_001F5EE8__cED5B8(0, 0, 0, 0x212, index * 15 + 95, port_main_label(D_0029E054__cED5B8, index), 192);'),
    ('text', 'func_001F5EE8__cED5B8(0, 1, 2, 0x212, index * 15 + 95, D_0029E054__cED5B8[index], 255);',
     'func_001F5EE8__cED5B8(0, 1, 2, 0x212, index * 15 + 95, port_main_label(D_0029E054__cED5B8, index), 255);'),
    ('text', 'func_001F5EE8__cED5B8(0, 0, 1, 0x212, index * 15 + 95, D_0029E054__cED5B8[index], 255);',
     'func_001F5EE8__cED5B8(0, 0, 1, 0x212, index * 15 + 95, port_main_label(D_0029E054__cED5B8, index), 255);'),
    ('text', '        choice = D_004C6CA8__cED5B8.x10;\n        if (choice == 0) {',
     '        choice = D_004C6CA8__cED5B8.x10;\n        port_main_row = 0;\n        if (choice == 0) {'),
    ('text', '            D_0029DF50__cED5B8 = D_004C6CA8__cED5B8.x10;\n        }\n',
     '            D_0029DF50__cED5B8 = D_004C6CA8__cED5B8.x10;\n'
     '        } else if (choice == 3) {\n'
     '            func_001F57D0__cED5B8(0x2329, 0);\n'
     '            port_main_row = port_online_menu() ? 0 : 3;\n'
     '            func_001F57D0__cED5B8(0x2329, 1);\n'
     '        }\n'),
    # Leaving the main menu, its last row is the game's own again (EE state as retail's: the
    # gates' route passes here); back on it, func_001F7420 sets 3 again.
    ('text', '        D_0029DEEC__cED5B8 = 15;\n        func_001F76B8__cED5B8(&D_004C6CA8__cED5B8);',
     '        D_0029DEEC__cED5B8 = 15;\n        D_004C6CA8__cED5B8.xC = 2;\n'
     '        func_001F76B8__cED5B8(&D_004C6CA8__cED5B8);'),
    ('text', '        func_001F5578__cED5B8();\n        func_001F76B8__cED5B8(&D_004C6CA8__cED5B8);\n    }\n}',
     '        func_001F5578__cED5B8();\n        D_004C6CA8__cED5B8.xC = 2;\n'
     '        func_001F76B8__cED5B8(&D_004C6CA8__cED5B8);\n    }\n}'),
]
# PC SETTINGS (port/hle/pc_settings.c): a tenth OPTIONS row (func_001F0160) that opens the PC's
# own page, run by the game's popup menus as its AUDIO row (case 2) runs the AUDIO / OTHER page.
# Row 9's table text (an unused "UNLOCK RIDERS/BIKES(test)") is replaced when drawn, not in data.
PATCHES['unsorted/001EB1D8.cpp'] += [
    ('text', 'void func_001F0160(void) {\n',
     'extern "C" void port_pc_settings(void);\n'
     'extern "C" char *port_options_label(char *label, int index);\n'
     '/* the row to come back to from a sub-page (the game itself puts the cursor on CONTROLS) */\n'
     'static int port_options_row;\n'
     'void func_001F0160(void) {\n'),
    ('text', '    unsigned char disabled[9];\n    int index, selected = 1, style;',
     '    unsigned char disabled[10];\n    int index, selected = 1, style;'),
    ('text', 'func_00254204_M(disabled,0,9);', 'func_00254204_M(disabled,0,10);'),
    ('text', 'func_001F7420_M(&D_004C6D98_M,13,15,0,8,0,disabled,9);',
     'func_001F7420_M(&D_004C6D98_M,13,15,0,9,port_options_row,disabled,10);'),
    ('text', '        PlayMenuSelectLegacy(&D_004C6D98_M);\n        D_0029DEEC_M = D_0029DED8_M;',
     '        PlayMenuSelectLegacy(&D_004C6D98_M);\n        port_options_row = D_004C6D98_M.x10;\n'
     '        D_0029DEEC_M = D_0029DED8_M;'),
    ('text', '        PlayMenuBackLegacy(&D_004C6D98_M);\n',
     '        PlayMenuBackLegacy(&D_004C6D98_M);\n        port_options_row = 0;\n'),
    ('text', 'func_001F5EE8_M(1,0,0,530,90 + index * 10,D_0029E070_M[index+1],192);',
     'func_001F5EE8_M(1,0,0,530,90 + index * 10,port_options_label(D_0029E070_M[index ? index : 10],index),192);'),
    ('text', 'func_001F5EE8_M(1,selected,style,530,90 + index * 10,D_0029E070_M[index+1],255);',
     'func_001F5EE8_M(1,selected,style,530,90 + index * 10,port_options_label(D_0029E070_M[index ? index : 10],index),255);'),
    ('text', '(u128)0x5F5245544E455F53UL);\n            }\n            break;\n        }\n',
     '(u128)0x5F5245544E455F53UL);\n            }\n            break;\n'
     '        case 9:\n'
     '            func_001F57D0_M(9001,0);\n'
     '            port_pc_settings();\n'
     '            D_0029DEEC_M = D_0029DED8_M;\n'
     '            D_0029DED8_M = 2;\n'
     '            func_001F57D0_M(9001,1);\n'
     '            break;\n'
     '        }\n'),
    ('text', 'if (D_004225A0_M.x12198 == 0) disabled[5] = 1;', 'if (D_004225A0_M.x12198 == 0) disabled[6] = 1;'),
    ('text', 'if (D_004225A0_M.x1219F == 0) disabled[4] = 1;', 'if (D_004225A0_M.x1219F == 0) disabled[5] = 1;'),
    ('text', 'switch (D_004C6D98_M.x10)', 'switch (D_004C6D98_M.x10 ? D_004C6D98_M.x10 - 1 : 9)'),
    # the page's left/right sounds, as the front end's other value rows play them
    ('text', '        func_001F76B8_M(&D_004C6D98_M);\n    }\n}\n\nextern "C" {\ntypedef struct { char bytes[48]; } LocalItems__w1;',
     '        func_001F76B8_M(&D_004C6D98_M);\n    }\n}\n'
     'extern "C" void port_menu_sound(int next) {\n'
     '    func_001C57A8_M(D_004B25A0_M.x4F0, next ? (u128)0x5458454E5F53UL\n'
     '                                            : ((u128)0x5355 << 64) | (u128)0x4F49564552505F53UL);\n'
     '}\n\nextern "C" {\ntypedef struct { char bytes[48]; } LocalItems__w1;'),
]
# The VIDEO CLIPS popup's update (vtable D_00288540 slot 5): on the EE it calls the base update
# func_0018E830 with the menu still in $a0, though the source names no argument. Natively the
# menu was dropped and OPTIONS > VIDEO CLIPS crashed (as func_00202760 below).
PATCHES['unsorted/001F94D0.c'] = [('text', 'extern void func_0018E830(void);', 'extern void func_0018E830(void *);'),
                                  ('text', 'void func_001F94D0(void) {\n    func_0018E830();',
                                   'void func_001F94D0(void *menu) {\n    func_0018E830(menu);')]
# The movie player's callback registration: func_0023C1D0 passes its five arguments on to
# sceMpegAddStrCallback (func_0024C630: mpeg, stream type, channel, callback, data) in the
# registers they came in; the source names none, so natively the call read through garbage and
# every movie crashed (round 38).
# The sound bank path's literals as 16-byte, 16-aligned, zero-padded arrays (finding 090 SP17).
# strcpy (func_00254544) and strcat (func_002542C4) read their source a word (ld) or a quadword
# at a time once it is aligned, past the NUL, and those bytes stay in the registers (t0-t2, a0's
# upper lanes) and in a later sprintf's vararg spill (0x0093C620-0x0093C637). A plain literal sits
# wherever the module's .rodata puts it (";1" was tail-merged into another path's literal), so
# those leftovers changed with any data layout change (OP2, SP4's one global, SP1's templates).
# In the arrays the bytes read past the NUL are always zero. The strings, so the path and the
# game, are the same.
PATCHES['unsorted/00208900.c'] = [
    ('text', 'void func_00208900(SoundBankList *list, int *bank, char *name) {',
     'static const char port_snd_dir[16] __attribute__((aligned(16))) = "\\\\SND\\\\";\n'
     'static const char port_iso_version[16] __attribute__((aligned(16))) = ";1";\n'
     'void func_00208900(SoundBankList *list, int *bank, char *name) {'),
    ('text', 'strcpy(path, "\\\\SND\\\\");', 'strcpy(path, port_snd_dir);'),
    ('text', 'func_002542C4(path, ";1");', 'func_002542C4(path, port_iso_version);'),
]

PATCHES['unsorted/0023C1D0.c'] = [
    ('text', 'extern void func_0024C630(void);', 'extern int func_0024C630(void *, int, int, void (*)(void), int);'),
    ('text', 'int func_0023C1D0(void)\n{\n    func_0024C630();',
     'int func_0023C1D0(void *mpeg, int type, int channel, void (*callback)(void), int data)\n{\n'
     '    func_0024C630(mpeg, type, channel, callback, data);')]
# Four destructor wrappers (vtables at D_00279300.., slot 1) chain to func_001FFD30(this, flags),
# which reads both from $a0/$a1; the source names neither, so natively both were garbage.
for _unit in ('00201010', '00201080', '002010F0', '00201160'):
    PATCHES['unsorted/%s.c' % _unit] = [
        ('text', 'extern void func_001FFD30(void);', 'extern void func_001FFD30(void *, int);'),
        ('text', 'void func_%s(void)\n{\n    func_001FFD30();' % _unit,
         'void func_%s(void *self, int flags)\n{\n    func_001FFD30(self, flags);' % _unit)]
# The movie player (finding 055): Sony's stream player sample on libmpeg/the IPU/PATH3/989snd's
# external stream, none of which the PC has, is replaced at its entry points by port/hle/movie.c
# (decoded with libmpeg2, shown by the presenter, its sound in the mixer). func_00223BB0 starts a
# movie and func_00223C60, called once a game frame, keeps it going (arg0 0: the player skipped);
# the flags the startup sequence and OPTIONS > VIDEO CLIPS wait on are set as the PS2's player sets
# them (D_0029DB00 0 while one plays, D_0029DAFC 1; func_00224308 at the end), and the sound mask
# of the start (func_00201D50) is still sent. The PS2 path below stays compiled, never reached.
PATCHES['unsorted/00223BB0.c'] = [
    ('text', 'int func_00223BB0(int arg0, int arg1)\n{\n',
     'int func_00223BB0(int arg0, int arg1)\n{\n'
     '    host_movie_start((const char *)arg0, arg1);\n'
     '    func_00201D50(0xFFFFFDFF);\n'
     '    D_0029DB20 = arg1;\n'
     '    D_0029DC1C = 0;\n'
     '    D_0029DB00 = 0;\n'
     '    D_0029DAFC = 1;\n'
     '    return 0;\n'),
    ('text', 'void func_00223C60(int arg0)\n{\n',
     'void func_00223C60(int arg0)\n{\n'
     '    extern int D_0029D9F8;\n'
     '    if (host_movie_update(arg0)) {\n'
     '        D_0029D9F8 = 0;\n'
     '        D_0029DB00 = 1;\n'
     '        D_0029DAFC = 0;\n'
     '    }\n'
     '    return;\n')]

# Widescreen 2D and HUD, and the main menu's EXIT GAME (finding 056): port/patches_widescreen.py,
# port/patches_exit.py (after the ONLINE row above, whose text it patches).
import patches_exit, patches_widescreen  # noqa: E402
patches_widescreen.add(PATCHES)
patches_exit.add(PATCHES)
import patches_options
patches_options.add(PATCHES)
import patches_fps
patches_fps.add(PATCHES)
import patches_unlock  # PC SETTINGS Unlock All: the unlock readers (finding 095)
patches_unlock.add(PATCHES)
import patches_online_flow  # the race's pause menu in an online room (finding 118)
patches_online_flow.add(PATCHES)

# Online's retail preview flow (finding 065). These hooks return immediately offline.
_ONLINE_SELECT_BEGIN = r"""
extern void func_001F67F0_online(int) asm("func_001F67F0");
extern void func_00203B68_online(void *, int, float) asm("func_00203B68");
void port_online_choose_end(void) {
    port_main_row = 3;
    D_004C6CA8__cED5B8.x4 = 0;
}
extern int D_004C6910_online[] asm("D_004C6910");  /* each player's place on the rider carousel */
extern int D_0029DF68_online asm("D_0029DF68");    /* the player choosing */
extern void func_001F60C8_online(int) asm("func_001F60C8");
extern "C" int port_online_rider_spot(void);
extern void func_00203FE0_online(void *, int) asm("func_00203FE0");
extern void func_00203E10_online(void *, float) asm("func_00203E10");
extern void func_001F5100_online(void) asm("func_001F5100");
extern void func_001F65A8_online(int) asm("func_001F65A8");
extern void func_001F65E0_online(int) asm("func_001F65E0");
extern void *D_00275BB0_online[] asm("D_00275BB0");  /* each place's stand animation */
extern int D_00275C30_online[] asm("D_00275C30");
extern int D_0029E01C_online asm("D_0029E01C");
extern void func_0021E428_online(int) asm("func_0021E428");  /* the rider screen's camera on (func_001F24B8) */
/* flight: the camera's flight from the title to the rider screen, as SINGLE PLAYER (the first
   choice). 0: back from the shop to the room: the rider screen on the player's own rider, as the
   new/continue screen's Back puts it there (func_001F3B98): the carousel's camera take that ends at
   that place (place p: take p + 1, the last place 14: take 16) at its end, the statues around it,
   the rider's stand animation, the rider alive. */
void port_online_choose_begin(int flight) {
    int spot = flight ? 1 : port_online_rider_spot();
    int take = spot == 14 ? 16 : spot + 1;
    func_001F67F0_online(0);
    D_004B25A0__cED5B8.x288 = 1;
    D_0029DF50__cED5B8 = 2;
    if (flight) {
        D_0029DED8__cED5B8 = 32;
        D_0029DEEC__cED5B8 = 15;
        func_00203590((struct AnimationHandle *)(void *)(D_004C6370__cED5B8), 0x2330, 0, 2);
        /* The flight from its start: once played it stays at its end, and playing it again would
           leave the camera where the last screen put it (the sky of the GET READY screen). */
        func_00203B68_online(D_004C6370__cED5B8, 1, 0.0f);
        func_002038E0__cED5B8(D_004C6370__cED5B8, 1);
        func_001F50F0__cED5B8(0x2330, 2);
    } else {
        D_0029DF68_online = 0;
        D_004C6910_online[0] = spot;
        D_0029DED8__cED5B8 = 3;
        D_0029DEEC__cED5B8 = 32;
        func_00203590((struct AnimationHandle *)(void *)(D_004C6370__cED5B8), 0x2330, 0, take);
        func_00203FE0_online(D_004C6370__cED5B8, 1);
        func_002038E0__cED5B8(D_004C6370__cED5B8, 0);
        func_001F50F0__cED5B8(0x2330, take);
        func_001F5100_online();
        func_001F65A8_online(1);
        func_001F65E0_online(0);
        func_001F60C8_online(spot);
        if (spot != 14) {
            struct { int w[4]; } stand;  /* a copy of the handle, as retail passes it */
            func_00203590((struct AnimationHandle *)D_00275BB0_online[spot], D_00275C30_online[spot], 0, 0);
            __builtin_memcpy(&stand, D_00275BB0_online[spot], sizeof stand);
            func_00203E10_online(&stand, 1.0f);
            __builtin_memcpy(&stand, D_00275BB0_online[spot], sizeof stand);
            func_002038E0__cED5B8(&stand, 1);
        }
        D_0029E01C_online = 0;
        /* the rider screen's camera on, as the rider screen does once the flight's take is at its end */
        func_0021E428_online(1);
    }
    func_001F57D0__cED5B8(0x2329, 0);
    D_0029DF44__cED5B8 = 0;
    D_0029DF40__cED5B8 = 210;
    func_001F5578__cED5B8();
    func_001C58A8__cED5B8(D_004B25A0__cED5B8.x4F0);
    func_001C5960__cED5B8(D_004B25A0__cED5B8.x4F0);
    func_001C5940__cED5B8(D_004B25A0__cED5B8.x4F0);
    func_001F6A08__cED5B8(spot);
}
"""
PATCHES['unsorted/001EB1D8.cpp'] += [
    ('text', 'int func_001EB4D0(void)\n{',
     'extern "C" int port_online_shell(void);\nint func_001EB4D0(void)\n{'),
    ('text', '        switch (D_0029DED8__target) {',
     '        if (!port_online_shell()) switch (D_0029DED8__target) {'),
    ('text', 'void func_001ED5B8__cED5B8(void) {\n    u8__cED5B8 disabled[5];',
     _ONLINE_SELECT_BEGIN + '\nvoid func_001ED5B8__cED5B8(void) {\n    u8__cED5B8 disabled[5];'),
    ('text', '            port_main_row = port_online_menu() ? 0 : 3;\n'
             '            func_001F57D0__cED5B8(0x2329, 1);',
     '            int online_action = port_online_menu();\n'
     '            port_main_row = online_action ? 0 : 3;\n'
     '            if (online_action == 2) port_online_choose_begin(1);\n'
     '            else func_001F57D0__cED5B8(0x2329, 1);'),
]

# Leaving an online room takes the rider screen's Back to the title; there the main menu chooses
# ONLINE again by itself (port_online_resume), so the player is back on the ONLINE page.
PATCHES['unsorted/001EB1D8.cpp'] += [
    ('text', 'extern int port_online_menu(void);\n',
     'extern int port_online_menu(void);\nextern int port_online_resume(void);\n'),
    ('text', '    func_001F4A98__cED5B8();\n',
     '    func_001F4A98__cED5B8();\n'
     '    if (port_online_resume()) {\n'
     '        D_004C6CA8__cED5B8.x10 = 3;\n'
     '        D_0029DF87__cED5B8 = D_0029DF86__cED5B8 = 0;\n'
     '        D_0029DF8B__cED5B8 = 1;\n'
     '    }\n'),
]

PATCHES['unsorted/001EB1D8.cpp'] += [
    ('text', 'void func_001EF0E0__c8(void) {',
     'extern int port_online_single_race(void);\nvoid func_001EF0E0__c8(void) {'),
    ('text', '    func_001F4A98__c8();\n    if (D_0029DF87__c8)',
     '    func_001F4A98__c8();\n'
     '    if (port_online_single_race()) { D_004C6DF8__c8.x10 = 0; D_0029DF8B__c8 = 1; D_0029DF87__c8 = D_0029DF86__c8 = 0; }\n'
     '    if (D_0029DF87__c8)'),
]

# Online the shop's own pages run (finding 128): its START commits the bike as retail does
# (func_0013A170), then online it leaves as the shop's Back does from its header (func_0013C970:
# the exit page D_00362490, which fades out and closes the popup as cancelled, so func_00139BF8 and
# func_001F1500 take the shell and its camera back to the rider screen) instead of opening the
# level chooser; port_online_shell opens the room there. Scripted test runs press
# START on the shop's first frame (port_online_shop_auto). Both return 0 offline.
PATCHES['unsorted/0013BA60.cpp'] = PATCHES.get('unsorted/0013BA60.cpp', []) + [
    ('text', 'extern void func_0013A170();\n',
     'extern void func_0013A170();\n'
     'extern "C" int port_online_shop_auto(void);\n'
     'extern "C" int port_online_shop_commit(void);\n'
     'extern char D_00362490[];\n'),
    ('text', '  if(!p->x524 && func_00190D60(input,8,p->x45C)) {\n'
             '   int other=0, game;\n'
             '   func_0013A170(); func_00140C38(D_0036B730);\n',
     '  if(!p->x524 && (func_00190D60(input,8,p->x45C) || port_online_shop_auto())) {\n'
     '   int other=0, game;\n'
     '   func_0013A170(); func_00140C38(D_0036B730);\n'
     '   if(port_online_shop_commit()) { func_001C57A8(D_004B2A90,(u128)0x5443454C45535F53UL); func_00190660(&D_003F52D0,D_00362490); return; }\n'),
]

# Online the shop's BIKES page starts on the setup's bike (the player's last choice, else the
# rider's default: online_menu.c shop_prepare), as the career's shop does (func_0013A270), not on
# the list's first: a pro who changes nothing keeps the pro's own bike (finding 128).
PATCHES['unsorted/00139BF8.c'] = PATCHES.get('unsorted/00139BF8.c', []) + [
    ('text', '        func_0013CDB0(&D_00365800);\n    }\n',
     '        func_0013CDB0(&D_00365800);\n        if (port_online_single_race()) func_0013A270();\n    }\n'),
    ('text', 'extern void func_0013A270(void);\n',
     'extern void func_0013A270(void);\nextern int port_online_single_race(void);\n'),
]

# The stock level chooser accepts the host's track only; guests retain the same shop exit.
PATCHES['unsorted/001F8920.c'] = PATCHES.get('unsorted/001F8920.c', []) + [
    ('text', 'void func_001F8920(struct Race *race)\n{',
     'extern int port_online_guest_track(void);\n'
     'extern void func_001F8A08(void *);\nvoid func_001F8920(struct Race *race)\n{'),
    ('text', '        func_0018E1C0(race);',
     '        func_0018E1C0(race);\n'
     '        if (port_online_guest_track()) func_001F8A08(race);'),
]

# The mouse on the front end's own menus (port/hle/menu_page.c, finding 092 UI2): each shell frame
# is marked and the shell's three text helpers report what they draw (to host memory only, no EE
# state), so the mouse finds the rows the game drew; it acts through pad presses only.
PATCHES['unsorted/001EB1D8.cpp'] += [
    ('text', 'extern "C" int port_online_shell(void);\n',
     'extern "C" int port_online_shell(void);\nextern "C" void port_shell_frame(void);\n'),
    ('text', '        if (!port_online_shell()) switch (D_0029DED8__target) {',
     '        port_shell_frame();\n        if (!port_online_shell()) switch (D_0029DED8__target) {'),
] + [
    ('text', head + ' {\n',
     'extern void port_shell_text(int align, int style, int x, int y, const void *text);\n' + head +
     ' {\n port_shell_text(%d, %s, x, y, (const void *)(%s));\n' % (align, style, text))
    for head, align, style, text in (
        ('void func_001F5398__c27(int style,int group,int color,int x,int y,void *text,int target)', 0, 'style', 'text'),
        ('void func_001F5D08__c33(int style,int group,int color,int x,int y,void *text,int target)', 1, 'style', 'text'),
        ('void func_001F5EE8__c34(int index,int row,int col,int x,int y,int z,int target)', 2, 'index', '(unsigned long)(unsigned)z'))
]

# Online, the countdown's timer holds until every PC may start (port/hle/online.c, start_step), the
# game drawing meanwhile, and the HUD says who it waits for.
PATCHES['unsorted/0019EE98.c'] = PATCHES.get('unsorted/0019EE98.c', []) + [
    ('text', 'void func_0019EE98(RaceStart *race) {',
     'extern int hle_online_countdown_hold(int state);\nvoid func_0019EE98(RaceStart *race) {'),
    ('text', '    race->timer = race->timer + func_00238F60();',
     '    race->timer = race->timer + (hle_online_countdown_hold(race->state) ? 0 : func_00238F60());'),
]
PATCHES['unsorted/00176ED8.c'] = PATCHES.get('unsorted/00176ED8.c', []) + [
    ('text', 'void func_00176ED8(Obj *obj)\n{',
     'extern void hle_online_hud(int view);\nextern int hle_online_watch_hud(void);\nextern void func_00176E40(Obj *, int);\nvoid func_00176ED8(Obj *obj)\n{\n    hle_online_hud(obj->player);\n    if (hle_online_watch_hud()) { func_00176E40(obj, 0); return; }'),
]

# WaitForRender's spin enters the kernel and sends a queued render chain the VIF1 interrupt
# would have sent (port/hle/dma.c, hle_render_wait; issue 22).
PATCHES['unsorted/001B4618.c'] = PATCHES.get('unsorted/001B4618.c', []) + [
    ('text', 'do {} while(D_0029E1C8 | D_0029E1CC | D_0029E1E4);',
     'do { extern void hle_render_wait(void); hle_render_wait(); } while(D_0029E1C8 | D_0029E1CC | D_0029E1E4);'),
]

# wasm32: size_t is unsigned long there and unsigned int on i686 (the same type as the game's).
PATCHES['unsorted/00235488.cpp'] = [('text', 'operator new[](unsigned int size)', 'operator new[](__SIZE_TYPE__ size)')]

# Room spectating: one camera override, shared with DHD_PUPPET_CAMERA.
PATCHES['unsorted/00148BE8.c'] = PATCHES.get('unsorted/00148BE8.c', []) + [
    ('text', 'void func_00148BE8(Object *s,int arg1) {',
     'extern void hle_online_watch_camera(void *);\nvoid func_00148BE8(Object *s,int arg1) {\n hle_online_watch_camera(s);'),
]

# A race runs (fps.h port_loop_frame, finding 159): the race world's update, every unpaused frame of
# the race's loop and in its load (func_0019C868); the shell's frame clears it. (func_001B40A0 itself
# is NONMATCHING C: the core runs its recompiled asm, so a patch there never runs; finding 165.)
# Online rooms (online.c hle_online_loop_frame, finding 165): START ends the race loop and the pause
# menu runs in the front end; the loop's first frame after RESUME lets the watching camera go
# (finding 122) and repeats a leave that came while the menu was up.
PATCHES['unsorted/0016C0C8.c'] = PATCHES.get('unsorted/0016C0C8.c', []) + [
    ('text', 'void func_0016C0C8(Obj *obj)\n{',
     'extern void port_loop_frame(int);\nextern void hle_online_loop_frame(void);\n'
     'void func_0016C0C8(Obj *obj)\n{\n    port_loop_frame(1);\n    hle_online_loop_frame();'),
]
