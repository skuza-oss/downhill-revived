"""prepare_wasm2c.stable_units on a synthetic module (finding 151): the chunks compile, each
chunk's header declares only what the chunk uses, and a change to one function changes only the
chunk that holds it. Needs WABT (wat2wasm, wasm2c) and clang, as build_host64.py finds them; all
products stay under build/wasm2c-chunks-test/.

    python port/tools/test_wasm2c_chunks.py
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_host64 import LLVM, ROOT, SOURCE, TARGETS  # noqa: E402
from prepare_wasm2c import FUNC_DEF, prepare_units, stable_units  # noqa: E402

OUT = ROOT / 'build' / 'wasm2c-chunks-test'
COUNT = 80
SIZES = (1500, 40000, 5)  # small chunks for small functions


def module(changed=None):
    """COUNT functions, each calling the one before; f7 calls through the table."""
    funcs = []
    for i in range(COUNT):
        body = '(i32.add (local.get 0) (i32.const %d))' % (1000 + i + (1 if i == changed else 0))
        if i:
            body = '(call $f%d %s)' % (i - 1, body)
        if i == 7:
            body = '(call_indirect (type $t) (local.get 0) (i32.const 0))'
        funcs.append('(func $f%d (type $t) (param i32) (result i32) %s)' % (i, body))
    return ('(module (type $t (func (param i32) (result i32))) (memory (export "memory") 1)\n'
            '(table 2 funcref) (elem (i32.const 0) $f1 $f2)\n' + '\n'.join(funcs) + ')\n')


def build(work, wabt, changed=None):
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    exe = '.exe' if os.name == 'nt' else ''
    (work / 'm.wat').write_text(module(changed))
    subprocess.run([str(wabt / 'bin' / ('wat2wasm' + exe)), 'm.wat', '-o', 'm.wasm'], cwd=work, check=True)
    subprocess.run([str(wabt / 'bin' / ('wasm2c' + exe)), 'm.wasm', '-n', 'core', '--num-outputs=4',
                    '-o', 'core.c'], cwd=work, check=True)
    units = [work / ('core_%d.c' % i) for i in range(4)]
    prepare_units(units, work, exact_f32=True)
    return stable_units(units, work, SIZES)


def digests(units):
    return {u.name: hashlib.sha256(u.read_bytes() + (u.with_suffix('.h').read_bytes() if u.with_suffix('.h').exists()
                                                     else b'')).hexdigest() for u in units}


def main():
    t = TARGETS['win-x64' if os.name == 'nt' else 'linux-x64']
    wabt = t.wabt
    a = build(OUT / 'a', wabt)
    b = build(OUT / 'b', wabt, changed=40)
    chunks = [u for u in a if u.name != 'core_module.c']
    assert len(chunks) >= 4, 'expected several chunks, got %d' % len(chunks)
    moved = sum(len(FUNC_DEF.findall(u.read_text())) for u in chunks)
    assert moved >= COUNT - 2, 'only %d of %d functions in chunks' % (moved, COUNT)
    da, db = digests(a), digests(b)
    assert da.keys() == db.keys(), 'the change moved a chunk boundary'
    differ = [n for n in da if da[n] != db[n] and n != 'core_module.c']
    assert len(differ) == 1, 'one changed function changed %d chunks: %s' % (len(differ), differ)
    assert 'w2c_core_f40(' in (OUT / 'b' / 'chunks' / differ[0]).read_text()
    for u in chunks:  # own header: no declaration of a function the chunk does not name
        text, header = u.read_text(), u.with_suffix('.h').read_text()
        for name in ('w2c_core_f%d' % i for i in range(COUNT)):
            if name + '(' in header:
                assert re.search(r'\b%s\b' % name, text), '%s declares %s, unused' % (u.with_suffix('.h').name, name)
    rt = wabt / 'share/wabt/wasm2c'
    cc = [str(LLVM / 'clang.exe') if os.name == 'nt' else 'clang', '-fsyntax-only', '-w',
          '-DWASM_RT_MEMCHECK_GUARD_PAGES=1', '-DWASM_RT_NONCONFORMING_ALLOW_OOB_READ_ELIMINATION=1',  # syntax only: no GNU asm
          '-DWASM_RT_STACK_DEPTH_COUNT=1', '-DWASM_RT_THREAD_LOCAL=', '-I' + str(OUT / 'a'), '-I' + str(wabt / 'include'), '-I' + str(rt),
          '-I' + str(SOURCE), '-I' + str(ROOT / 'port/runtime'), '-I' + str(ROOT / 'port/include'),
          '-I' + str(ROOT / 'port/host')]
    for u in a:
        r = subprocess.run(cc + [str(u)], capture_output=True, text=True)
        assert r.returncode == 0, '%s does not compile:\n%s' % (u.name, r.stderr[-2000:])
    print('PASS: %d chunks; one changed function changed one chunk (%s); all compile' % (len(chunks), differ[0]))


if __name__ == '__main__':
    main()
