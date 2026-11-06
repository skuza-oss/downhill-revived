"""The Rust renderer's C ABI: the header and src/abi.rs must agree on every target.

    python port/tools/test_dhdr_abi.py                              x86_64 and wasm32
    python port/tools/test_dhdr_abi.py --arch wasm32                one target
    python port/tools/test_dhdr_abi.py --selftest                   planted faults in the header or abi.rs must fail

For each target:
1. clang compiles port/gs/test_dhdr_abi.c (which includes port/render/include/dhd_render.h) to
   LLVM IR, freestanding, -Wall -Werror (native targets also with DHD_GS_TEST_BUILD), and the
   header alone as C++. The IR's two constant arrays give every struct's size and alignment, every
   member's offset and size, and every constant's value.
2. The C rows must show no implicit padding: each struct's members, by offset, tile it exactly.
3. rustc compiles src/abi.rs on its own (it depends on core only) for the same target, with one
   compile-time assertion per C row, so any row Rust lays out or values differently, or lacks,
   fails the build.
Finally every function prototype in the header must equal its `<name>_fn` type in src/abi.rs
(same parameters, names and types, same return type), and the two lists must be the same.

Scratch goes to build/port/test-dhdr-abi. Exit 0 when everything agrees; 1 otherwise."""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / 'port/render/include/dhd_render.h'
ABI_RS = ROOT / 'port/render/src/abi.rs'
PROBE = ROOT / 'port/gs/test_dhdr_abi.c'
CRATE = ROOT / 'port/render'          # rust-toolchain.toml picks the pinned rustc here
OUT = ROOT / 'build/port/test-dhdr-abi'
TRIPLES = {'x86_64': 'x86_64-pc-windows-msvc', 'wasm32': 'wasm32-unknown-unknown'}


def clang():
    return shutil.which('clang') or str(Path.home() / 'scoop/apps/llvm/current/bin/clang.exe')


def run(cmd, cwd=None):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr).strip()


def decode_llvm_string(text):
    out = bytearray()
    i = 0
    while i < len(text):
        if text[i] == '\\':
            out.append(int(text[i + 1:i + 3], 16))
            i += 3
        else:
            out.append(ord(text[i]))
            i += 1
    return bytes(out)


def c_rows(arch, include, out, problems):
    """[(name, value)] from the C compiler for arch, or None when it did not compile."""
    triple = TRIPLES[arch]
    ll = out / ('abi-%s.ll' % arch)
    flags = ['--target=' + triple, '-ffreestanding', '-Wall', '-Wextra', '-Werror', '-I', str(include)]
    if arch != 'wasm32':
        flags.append('-DDHD_GS_TEST_BUILD=1')
    code, text = run([clang(), *flags, '-std=c11', '-O0', '-S', '-emit-llvm', '-o', str(ll), str(PROBE)])
    if code:
        problems.append('%s: test_dhdr_abi.c does not compile:\n%s' % (arch, text))
        return None
    cpp = out / 'header_cpp.cpp'
    cpp.write_text('#include "dhd_render.h"\n', encoding='utf-8')
    code, text = run([clang(), *flags, '-x', 'c++', '-std=c++17', '-fsyntax-only', str(cpp)])
    if code:
        problems.append('%s: dhd_render.h does not compile as C++:\n%s' % (arch, text))
    ir = ll.read_text(encoding='utf-8')
    names = re.search(r'^@dhdr_abi_names = [^=]*constant \[(\d+) x i8\] c"(.*)", align', ir, re.M)
    values = re.search(r'^@dhdr_abi_values = [^=]*constant \[(\d+) x i64\] \[(.*)\], align', ir, re.M)
    if not names or not values:
        problems.append('%s: the IR has no dhdr_abi_names / dhdr_abi_values arrays' % arch)
        return None
    name_list = decode_llvm_string(names.group(2)).decode('ascii').split('\0')
    while name_list and name_list[-1] == '':   # each row's NUL, then the literal's own
        name_list.pop()
    value_list = [int(v.strip().split()[1]) for v in values.group(2).split(',')]
    if len(name_list) != len(value_list) or len(value_list) != int(values.group(1)):
        problems.append('%s: %d names but %d values' % (arch, len(name_list), len(value_list)))
        return None
    return list(zip(name_list, value_list))


def check_padding(arch, rows, problems):
    """Every struct's members tile it: no hole between members or at the end."""
    table = dict(rows)
    structs = [n[:-5] for n, _ in rows if n.endswith('#size') and '.' not in n]
    for s in structs:
        members = sorted((v, table[n + '#size'], n) for n, v in rows
                         if n.startswith(s + '.') and not n.endswith('#size'))
        at = 0
        for offset, size, name in members:
            if offset != at:
                problems.append('%s: %s: a hole of %d bytes before %s' % (arch, s, offset - at, name))
            at = offset + size
        if at != table[s + '#size']:
            problems.append('%s: %s: members end at %d, the struct is %d bytes' % (arch, s, at, table[s + '#size']))


def rust_check(arch, rows, abi_rs, out, problems):
    """Compiles abi.rs for arch with one assertion per C row."""
    lines = ['#![no_std]', '#![allow(dead_code, unused)]',
             '#[path = %s]' % ('r"%s"' % abi_rs), 'mod abi;',
             'const fn pointee<F>(_: *const F) -> usize { core::mem::size_of::<F>() }',
             'macro_rules! member_size { ($t:ty, $f:ident) => {{',
             '    let u = core::mem::MaybeUninit::<$t>::uninit();',
             '    let p = u.as_ptr();',
             '    pointee(unsafe { &raw const (*p).$f })',
             '}} }']
    for name, value in rows:
        if name.startswith('='):
            expr = 'abi::%s as i64' % name[1:]
        elif name.endswith('#align'):
            expr = 'core::mem::align_of::<abi::%s>() as i64' % name[:-6]
        elif name.endswith('#size') and '.' not in name:
            expr = 'core::mem::size_of::<abi::%s>() as i64' % name[:-5]
        elif name.endswith('#size'):
            t, f = name[:-5].split('.')
            expr = 'member_size!(abi::%s, %s) as i64' % (t, f)
        else:
            t, f = name.split('.')
            expr = 'core::mem::offset_of!(abi::%s, %s) as i64' % (t, f)
        lines.append('const _: () = assert!(%s == %d, "%s: the C header gives %d on %s");'
                     % (expr, value, name, value, arch))
    src = out / ('abi_check_%s.rs' % arch)
    src.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    code, text = run(['rustc', '--edition', '2021', '--crate-type', 'lib', '--crate-name', 'dhdr_abi_check',
                      '--target', TRIPLES[arch], '--emit=metadata', '-o', str(out / ('abi_check_%s.rmeta' % arch)),
                      str(src)], cwd=CRATE)
    if code:
        errors = [line for line in text.splitlines() if 'the C header gives' in line or line.startswith('error')]
        problems.append('%s: src/abi.rs disagrees with the header (%d rows checked):\n  %s'
                        % (arch, len(rows), '\n  '.join(errors[:40]) or text[:4000]))
        return False
    return True


# ---- function prototypes ----

C_BASE = {'int': 'i32', 'uint32_t': 'u32', 'uint8_t': 'u8', 'size_t': 'usize', 'char': 'c_char', 'void': 'c_void',
          'uint64_t': 'u64', 'int32_t': 'i32', 'float': 'f32', 'double': 'f64'}
RUST_SCALARS = {'i32', 'u32', 'u8', 'usize', 'u64', 'c_int', 'c_char', 'c_void', 'f32', 'f64'}


def strip_c_comments(text):
    return re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)


def c_type(decl, problems, where):
    """'const DhdrDeviceDesc *d' -> ('d', '*const DhdrDeviceDesc'); 'uint32_t want[16]' -> ('want', '*mut u32')."""
    decl = ' '.join(decl.split())
    m = re.fullmatch(r'(const )?([A-Za-z_]\w*)\s*(\*?)\s*([A-Za-z_]\w*)?\s*(\[\d+\])?', decl)
    if not m:
        problems.append('%s: cannot read the C declaration "%s"' % (where, decl))
        return None, '?'
    const, base, star, name, array = m.groups()
    base = C_BASE.get(base, base)
    if star or array:
        return name, '*%s %s' % ('const' if const else 'mut', base)
    if base == 'c_void':
        return name, 'void'
    return name, base


def header_functions(text, problems):
    text = strip_c_comments(text)
    out = {}
    for m in re.finditer(r'(?:DHDR_FN\((\w+)\)\s*)?([A-Za-z_][\w ]*?\*?)\s*\b(dhdr_\w+|dhd_host_log)\s*\(([^)]*)\)\s*;', text):
        tag, ret, name, params = m.groups()
        if tag is not None and tag != name:
            problems.append('header: DHDR_FN(%s) on %s' % (tag, name))
        if tag is None and name != 'dhd_host_log':
            problems.append('header: %s has no DHDR_FN import name' % name)
        _, rtype = c_type(ret.strip() + ' r', problems, name)
        plist = []
        if params.strip() != 'void':
            for p in params.split(','):
                plist.append(c_type(p, problems, name))
        out[name] = (rtype, plist)
    return out


def rust_type(t, problems, where):
    t = ' '.join(t.split())
    m = re.fullmatch(r'\*(const|mut) (\w+)', t)
    if m:
        base = 'i32' if m.group(2) == 'c_int' else m.group(2)
        return '*%s %s' % (m.group(1), base)
    if t in RUST_SCALARS:
        return 'i32' if t == 'c_int' else t
    problems.append('%s: cannot read the Rust type "%s"' % (where, t))
    return '?'


def rust_functions(text, problems):
    out = {}
    for m in re.finditer(r'pub type (\w+)_fn\s*=\s*unsafe extern "C" fn\(([^)]*)\)\s*(?:->\s*([^;]+))?;', text):
        name, params, ret = m.groups()
        plist = []
        for p in [p for p in params.split(',') if p.strip()]:
            pname, ptype = p.split(':', 1)
            plist.append((pname.strip(), rust_type(ptype, problems, name)))
        out[name] = (rust_type(ret, problems, name) if ret else 'void', plist)
    return out


def check_functions(header_text, abi_text, problems):
    c = header_functions(header_text, problems)
    r = rust_functions(abi_text, problems)
    for name in sorted(set(c) | set(r)):
        if name not in r:
            problems.append('functions: %s is in the header but src/abi.rs has no %s_fn' % (name, name))
        elif name not in c:
            problems.append('functions: src/abi.rs has %s_fn but the header has no %s' % (name, name))
        elif c[name] != r[name]:
            problems.append('functions: %s differs:\n  header   %s\n  abi.rs   %s' % (name, c[name], r[name]))
    return len(c)


def check(arches, header=HEADER, abi_rs=ABI_RS, out=OUT, quiet=False):
    """The problems found (empty: all agree)."""
    out.mkdir(parents=True, exist_ok=True)
    include = out / 'include'
    shutil.rmtree(include, ignore_errors=True)
    include.mkdir()
    shutil.copyfile(header, include / 'dhd_render.h')
    problems = []
    say = (lambda *a: None) if quiet else print
    rows = {}
    for arch in arches:
        rows[arch] = c_rows(arch, include, out, problems)
        if rows[arch] is not None:
            check_padding(arch, rows[arch], problems)
    for arch in arches:
        if rows.get(arch) is None:
            continue
        n = len(rows[arch])
        if rust_check(arch, rows[arch], abi_rs, out, problems):
            say('%s: %d rows agree (C header and src/abi.rs), no implicit padding' % (arch, n))
    count = check_functions(header.read_text(encoding='utf-8'), abi_rs.read_text(encoding='utf-8'), problems)
    if not any(p.startswith('functions') or p.startswith('header') for p in problems):
        say('functions: %d prototypes equal their src/abi.rs types' % count)
    return problems


# ---- selftest: planted faults in a copy of the header or of abi.rs must each fail ----

MUTATIONS = [
    ('header', 'swapped members', 'uint32_t blend_key, blend_fix;', 'uint32_t blend_fix, blend_key;'),
    ('header', 'constant value', 'DHDR_DEV_FALLBACK = 16', 'DHDR_DEV_FALLBACK = 64'),
    ('header', 'removed member', 'uint8_t kind, depth_key, color_mask, pad;', 'uint8_t kind, depth_key, color_mask;'),
    ('header', 'implicit padding', 'uint32_t movie_width, movie_height, movie_on;',
     'uint16_t movie_width, movie_height; uint32_t movie_on;'),
    ('header', 'parameter type', 'const void *data, uint32_t bytes);', 'const void *data, size_t bytes);'),
    ('header', 'parameter order', 'uint32_t groups_x, uint32_t groups_y);', 'uint32_t groups_y, uint32_t groups_x);'),
    ('header', 'missing function', 'DHDR_FN(dhdr_surface_present) void dhdr_surface_present(DhdrSurface *s);', ''),
    ('abi.rs', 'Rust constant', 'pub const DHDR_LOG_WARN: u32 = 2;', 'pub const DHDR_LOG_WARN: u32 = 5;'),
    ('abi.rs', 'Rust member order', '    pub atst: u32,\n    pub aref: u32,', '    pub aref: u32,\n    pub atst: u32,'),
    ('abi.rs', 'Rust return type', 'fn(x: *mut DhdrExact, pixels: *mut u32) -> i32;', 'fn(x: *mut DhdrExact, pixels: *mut u32) -> u32;'),
]


def selftest():
    base = check(['x86_64'], quiet=True)
    if base:
        print('selftest: the unchanged header fails:\n' + '\n'.join(base))
        return 1
    print('selftest: unchanged header and abi.rs agree (x86_64)')
    wrong = 0
    for where, label, old, new in MUTATIONS:
        source = HEADER if where == 'header' else ABI_RS
        text = source.read_text(encoding='utf-8')
        if text.count(old) != 1:
            print('selftest: %s: the text to change is not in %s exactly once' % (label, where))
            wrong += 1
            continue
        out = OUT / 'selftest' / re.sub(r'\W+', '-', label)
        out.mkdir(parents=True, exist_ok=True)
        changed = out / source.name
        changed.write_text(text.replace(old, new), encoding='utf-8')
        files = {'header': changed} if where == 'header' else {'abi_rs': changed}
        problems = check(['x86_64'], out=out, quiet=True, **files)
        ok = bool(problems)
        print('selftest: %s (%s): %s' % (label, where, 'fails as it must (%s)' % problems[0].splitlines()[0] if ok
                                         else 'PASSED, which is wrong'))
        wrong += not ok
    print('selftest: %d mutations, %d wrong' % (len(MUTATIONS), wrong))
    return 1 if wrong else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--arch', default='x86_64,wasm32', help='comma list of x86_64, wasm32')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    arches = [a.strip() for a in args.arch.split(',') if a.strip()]
    bad = [a for a in arches if a not in TRIPLES]
    if bad or not arches:
        print('unknown arch: %s' % ', '.join(bad or ['(none)']))
        return 1
    problems = check(arches)
    for p in problems:
        print('FAIL ' + p)
    print('test_dhdr_abi: %s' % ('OK (%s)' % ', '.join(arches) if not problems else '%d problems' % len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
