#!/usr/bin/env python3
"""Tests of port/tools/stack_escapes.py and port/tools/redzone.py (finding 213) on small made-up
inputs: no matching toolchain, no game data. The red-zone rewrite test needs clang on PATH (the
core build's) and is skipped without it."""
import json, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import redzone  # noqa: E402
import stack_escapes as se  # noqa: E402

FAILS = []


def expect(name, got, want):
    if got != want:
        FAILS.append(name)
        print('FAIL %s: %r, expected %r' % (name, got, want))


# A callee that writes 32 bytes from its argument (a 16-byte header, then a 16-byte object: the
# shape of finding 207), one that walks its argument in a loop, and a caller that passes them a
# 16-byte local followed in its frame by another 16-byte local.
CC1 = r'''
	.ent	ctor
ctor:
	.frame	$sp,0,$31
	sw	$0,0($4)
	.set	noreorder
	.set	nomacro
	j	ctor2
	addu	$4,$4,16
	.set	macro
	.set	reorder
	.end	ctor
	.ent	ctor2
ctor2:
	.frame	$sp,0,$31
	sw	$0,12($4)
	j	$31
	.end	ctor2
	.ent	walk
walk:
	.frame	$sp,0,$31
	move	$2,$4
$L1:
	sw	$0,0($2)
	addu	$2,$2,4
	bne	$2,$5,$L1
	j	$31
	.end	walk
	.ent	caller
caller:
	.frame	$sp,48,$31
	subu	$sp,$sp,48
	sd	$31,32($sp)
	.set	noreorder
	.set	nomacro
	jal	ctor
	move	$4,$sp
	.set	macro
	.set	reorder
	addu	$4,$sp,16
	addu	$5,$sp,32
	jal	walk
	lw	$4,%lo(D_00400000)
	jal	ctor
	ld	$31,32($sp)
	addu	$sp,$sp,48
	j	$31
	.end	caller
 .stabs "int:t1=r1;-2147483648;2147483647;",128,0,0,0
.stabs "caller:F1",36,0,0,caller
 .stabs "head:2=ar1;0;3;1",128,0,0,-48
 .stabs "next:3=ar1;0;3;1",128,0,0,-32
'''


def test_dataflow():
    d = tempfile.mkdtemp()
    try:
        path = os.path.join(d, 'unit.s')
        with open(path, 'w', newline='\n') as f:
            f.write(CC1)
        fs, _ = se.read_cc1(path, 'unit.c')
        funcs = {f.name: f for f in fs}
        expect('locals', funcs['caller'].locals, [('head', 0, 16, 0), ('next', 16, 16, 0)])
        summ = {name: {} for name in funcs}
        raw, edges = [], []

        import collections
        summ = collections.defaultdict(lambda: collections.defaultdict(se.Summary))
        for f in funcs.values():
            def record(kind, *a, f=f):
                if kind == 'access':
                    k, lo, size, write, var = a
                    s = summ[f.name][k]
                    if write:
                        s.w = se.widen(s.w, lo, lo + size)
                    if var:
                        s.flags.add('walk-w' if write else 'walk-r')
                elif kind == 'call':
                    target, j, v, consts, indirect = a
                    if v[0] == 'arg':
                        edges.append((target, j, f.name, v[1], v[2]))
                    elif v[0] == 'sp':
                        raw.append((f.name, v[1], target, j, v[2], consts, indirect))
            se.analyse(f, summ, record)
        for callee, j, caller, k, c in edges:  # one level is enough here (ctor -> ctor2)
            src = summ[callee][j]
            if src.w:
                summ[caller][k].w = se.widen(summ[caller][k].w, src.w[0] + c, src.w[1] + c)
        expect('ctor writes header and object', summ['ctor'][0].w, (0, 32))
        expect('walk is a walk', 'walk-w' in summ['walk'][0].flags, True)
        # the escapes: &head to ctor (in the delay slot), &next and sp+32 to walk; the third call's $a0 came
        # from a load of a symbol, so it is no escape
        expect('escapes', sorted((r[1], r[2]) for r in raw), [(0, 'ctor'), (16, 'walk'), (32, 'walk')])
        rows = se.classify(funcs, summ, raw)
        by = {(r['local'], r['callee']): r for r in rows}
        expect('ctor overruns head', by[('head', 'ctor')]['verdict'], 'overrun-write')
        expect('what follows head', by[('head', 'ctor')]['after'], ['next@0x10(16)'])
        expect('walk is unbounded', by[('next', 'walk')]['verdict'], 'unbounded')
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_formats():
    expect('094 shortest', se.format_lengths('%d MORE CREDITS REQUIRED TO ADVANCE'), (34, 44))
    expect('%s open-ended', se.format_lengths('%s.%s;1')[1], None)
    expect('%02d width', se.format_lengths('%02d:%02d'), (5, 23))
    expect('%%', se.format_lengths('100%%'), (4, 4))
    expect('precision', se.format_lengths('%.2f')[0], 4)


def test_types():
    t = se.Types({'u128'})
    for stab in ('int:t1=r1;-2147483648;2147483647;', 'long long unsigned int:t7=r7;0;-1;', 'u128:t61=7',
                 'Small:T26=s1552x0:1,0,32;pad4:27=ar1;0;1547;2,32,12384;;', 'char:t2=r2;0;127;',
                 'a:32=ar1;0;79;2'):
        t.add(stab)
    expect('struct', t.size('26'), 1552)
    expect('array', t.size('32'), 80)
    expect('mode(TI) typedef', t.size('61'), 16)
    expect('array of int', t.size('33=ar1;0;3;1'), 16)


C_SRC = r'''
extern void fill(void *, int);
#define USE(x) fill(&(x), sizeof(x))
int f(int n)
{
    int small[4];
    char name[8];
    int plain;
    fill(small, 32);
    USE(name);
    plain = n;
    if (n)
        return small[1];
    return plain + name[0];
}
'''


def test_redzone():
    clang = shutil.which('clang')
    if not clang:
        print('SKIP redzone rewrite: no clang on PATH')
        return
    d = tempfile.mkdtemp()
    try:
        src = os.path.join(d, 'u.c')
        with open(src, 'w', newline='\n') as f:
            f.write('#include "x.h"\n#line 1 "u.c"\n' + C_SRC)
        with open(os.path.join(d, 'x.h'), 'w') as f:
            f.write('/* the port.h stand-in */\n')
        p = subprocess.run([clang, '-fsyntax-only', '-w', '-Xclang', '-ast-dump=json', '-I', d, src],
                           capture_output=True, text=True)
        expect('ast', p.returncode, 0)
        text = open(src).read()
        out, rep = redzone.instrument(text, redzone.decode_all(p.stdout), False)
        expect('instrumented', (rep['functions'], rep['locals']), (1, 2))  # small, name; not plain
        expect('names', 'rz_names = "small\\0name"' in out, True)
        expect('macro argument rewritten', 'USE((dhd_rz_frame.v1))' in out, True)
        rz = os.path.join(d, 'u.rz.c')
        with open(rz, 'w', newline='\n') as f:
            f.write(out)
        p = subprocess.run([clang, '-fsyntax-only', '-Werror=implicit-function-declaration', '-I', d, rz],
                           capture_output=True, text=True)
        expect('rewrite compiles', (p.returncode, p.stderr.strip()[:300]), (0, ''))
        rt = os.path.join(d, 'rt.c')
        with open(rt, 'w', newline='\n') as f:
            f.write(redzone.RUNTIME)
        p = subprocess.run([clang, '-fsyntax-only', rt], capture_output=True, text=True)
        expect('runtime compiles', p.returncode, 0)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main():
    test_types()
    test_formats()
    test_dataflow()
    test_redzone()
    print('test_stack_escapes: %s' % ('FAIL (%d)' % len(FAILS) if FAILS else 'OK'))
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
